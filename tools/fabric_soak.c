/* fabric_soak.c — standalone RTL soak/measurement harness for the Maldita fabric.
 *
 * WHY THIS EXISTS
 * ---------------
 * The ~18-second fabric stalls were only ever observed through gmloader, which makes
 * every RTL experiment a two-variable experiment: the engine's workload varies with the
 * game (scene, texture churn, frame pacing), and — worse — when a submit times out the
 * engine used to rebuild the command ring straight over the batch the fabric was still
 * reading. You could never tell whether a change to the RTL had helped, or whether the
 * game had simply walked into a different scene.
 *
 * This binary removes the engine from the loop entirely. It talks straight to the DDR
 * ring (same mmap + emitter as tools/fabric_probe.c), submits a fixed, reproducible
 * per-frame workload, and — the important part — NEVER re-emits while a batch is
 * unacked. Whatever it measures is the fabric's behaviour and nothing else.
 *
 * Run it with the engine disabled so nothing else touches the fabric:
 *     touch /media/fat/games/gmloader/NOENGINE     # wrapper runs framework-only
 *     <reload the core>                            # wrapper starts, spawns no child
 *     /tmp/fabric_soak.armhf --secs=120
 *     rm /media/fat/games/gmloader/NOENGINE        # restore normal launches
 *
 * WHAT IT MEASURES
 * ----------------
 * Per submit: the doorbell -> C_DONE latency. Any latency over --stall-ms counts as a
 * stall episode. During a stall it samples the SCANOUT control word (the DDR double-
 * buffer publish counter comp_fb_dma writes) to record whether scanout froze WITH the
 * blitter or kept running — that is the single most diagnostic bit we have, because a
 * blitter parked in S_SNAP_DRAIN freezes both, while a blitter that is merely slow does
 * not. It also reports the fabric's own per-frame cycle counters and wd_fire_count.
 *
 * The workload knobs exist so one variable can be moved at a time. The stall is
 * suspected to live in DDR arbitration between the blitter, comp_fb_dma and the scanout
 * reader, so the interesting sweeps are --stage (DDR3->SDRAM copy traffic) and
 * --appsurf (second render target + SRC_SURFACE sample-back), with --tris as the
 * rasterizer-load control.
 *
 * Defaults approximate one real game frame as measured on device (BLITPROF draws=10
 * tris=322; HEAPLOG ~29 textures / 0.74MB staged per frame).
 *
 * Copyright (C) 2026 — GPL-3.0
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>

#include "blt_emitter.h"

/* ---- the fixed DDR blitter window (mirrors raster_backend_mfgpu.cpp / fabric_probe.c) */
#define MF_PHYS_BASE   0x3B000000UL
#define MF_MAP_SIZE    0x01000000UL   /* 16 MiB — covers ring, heap AND the scanout regs */
#define MF_RING_OFF    0x00000040UL
#define MF_SRC_OFF     0x00080000UL
#define MF_TLBUF_OFF   0x00F40000UL
#define MF_RING_CAP    (MF_SRC_OFF   - MF_RING_OFF)
#define MF_SRC_CAP     (MF_TLBUF_OFF - MF_SRC_OFF)
#define MF_VTX_REGION  (128UL * 1024UL)

/* The scanout double-buffer control word comp_fb_dma publishes, at 0x3BF40000 — inside
 * the same 16 MiB mapping, so no second mmap. [31:2]=frame_counter, [0]=active_buffer
 * (comp_fb_dma.sv / openbor_video_reader.sv). This is our scanout liveness signal. */
#define MF_SCANOUT_OFF 0x00F40000UL

enum { C_SUBMIT=0, C_CMDCOUNT=1, C_TARGET=2, C_CLEAR=3, C_FLAGS=4, C_DONE=5, C_STATUS=6, C_SRCSEL=7 };

/* clk_sys = 98.4375 MHz; the fabric's perf counters are in these cycles. */
#define MF_CLK_MHZ 98.4375

#define FB_W 320
#define FB_H 240

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static inline void ctrl_wr(volatile uint8_t *c, int qw, uint32_t v) {
    *(volatile uint32_t *)(c + (size_t)qw * 8u) = v;
}
static inline uint32_t ctrl_rd(volatile uint8_t *c, int qw) {
    return *(volatile uint32_t *)(c + (size_t)qw * 8u);
}
static inline uint32_t ctrl_rd_hi(volatile uint8_t *c, int qw) {
    return *(volatile uint32_t *)(c + (size_t)qw * 8u + 4u);
}
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ---- configuration ------------------------------------------------------------- */
struct cfg {
    int    secs;        /* soak duration                                              */
    int    tris;        /* triangles per frame, spread over `draws` TRILISTs           */
    int    draws;       /* TRILIST commands per frame                                  */
    int    stage;       /* STAGE (DDR3->SDRAM) commands per frame                       */
    int    upload;      /* texture pages re-written ARM->DDR per frame (host bus traffic) */
    int    tex_kb;      /* size of each staged texture page                            */
    int    appsurf;     /* 1 = render to APPSURF then sample it back (real frame graph) */
    int    cblend;      /* composite blend: 0 = COPY, 1 = CONST_ALPHA (costs a dst read) */
    double rate_hz;     /* submit pacing; 0 = as fast as the fabric will ack            */
    double stall_ms;    /* latency above this counts as a stall episode                 */
    double giveup_ms;   /* abandon a wait after this (report, never re-emit under it)   */
};

static void usage(const char *me) {
    printf(
      "usage: %s [options]\n"
      "  --secs=N       soak duration in seconds (default 60, 0 = until Ctrl-C)\n"
      "  --tris=N       triangles per frame (default 322 = measured game frame)\n"
      "  --draws=N      TRILIST commands per frame (default 10)\n"
      "  --stage=N      STAGE DDR3->SDRAM copies per frame (default 24; 0 = none)\n"
      "  --upload=N     texture pages re-written ARM->DDR per frame (default 0). The\n"
      "                 engine memcpys ~0.74MB/frame of sub-region texture data into the\n"
      "                 heap; that host-side write traffic hits the same DDR controller\n"
      "                 the fabric reads through, and is absent from a stage-only soak.\n"
      "  --tex-kb=N     KiB per staged texture page (default 32)\n"
      "  --appsurf=0|1  emit the real two-target frame graph (default 1)\n"
      "  --cblend=0|1   full-screen composite blend: 0=COPY (default), 1=CONST_ALPHA.\n"
      "                 ALPHA sets the RTL's tri_need_dst, inserting a comp_fbram read\n"
      "                 into every pixel's walk. Diffing the two arms measures what that\n"
      "                 read actually costs, on identical geometry with no game variance.\n"
      "  --rate=HZ      submit pacing (default 60; 0 = unpaced)\n"
      "  --stall-ms=N   latency above this counts as a stall (default 250)\n"
      "  --giveup-ms=N  abandon one wait after this many ms (default 30000)\n"
      "\n"
      "Sweep one knob at a time against a fixed RBF, or the same knobs across two RBFs.\n"
      "The final SOAK line is machine-comparable — diff it between builds.\n", me);
}

static bool arg_int(const char *a, const char *k, int *out) {
    size_t n = strlen(k);
    if (strncmp(a, k, n) != 0) return false;
    *out = atoi(a + n);
    return true;
}
static bool arg_dbl(const char *a, const char *k, double *out) {
    size_t n = strlen(k);
    if (strncmp(a, k, n) != 0) return false;
    *out = atof(a + n);
    return true;
}

/* ---- one frame's worth of commands ----------------------------------------------
 * Shape mirrors the device frame graph captured in the ledger:
 *   SET_TARGET(APPSURF); clear; scene TRILISTs        -> the app surface
 *   SET_TARGET(WORK);    clear; SRC_SURFACE composite -> the work buffer, twice
 *                               plus a normal-texture "border" draw
 * With --appsurf=0 everything renders straight to WORK (single target), which is the
 * control for "is the second render target / SRC_SURFACE path implicated?".
 */
static int build_frame(blt_emitter_t *e, const struct cfg *c,
                       blt_surface_ref_t *texs, int ntex, uint32_t frame_no,
                       int tex_w, int tex_h) {
    blt_begin_frame(e, /*target_buf=*/0, /*clear=*/0, 0);

    /* Per-frame DDR3->SDRAM staging traffic. This is the load the arbiter has to
     * interleave with comp_fb_dma's vblank writeback and the reader's line fetches. */
    for (int i = 0; i < c->stage; i++) {
        blt_surface_ref_t *t = &texs[(frame_no + (uint32_t)i) % (uint32_t)ntex];
        if (blt_stage(e, t->off, (uint32_t)t->stride * t->h) != 0) return -1;
    }

    if (c->appsurf && blt_set_target(e, BLT_TARGET_APPSURF) != 0) return -1;
    if (blt_fill(e, 0, 0, FB_W, FB_H, rgb565(0, 0, 40)) != 0) return -1;

    /* Scene: `tris` triangles spread over `draws` TRILIST commands. Each triangle is a
     * small textured sprite-sized quad half, walked across the screen so the rasterizer
     * does real coverage work rather than degenerate no-ops. */
    int per_draw = c->draws > 0 ? (c->tris + c->draws - 1) / c->draws : c->tris;
    int emitted = 0;
    for (int d = 0; d < c->draws && emitted < c->tris; d++) {
        int n = c->tris - emitted;
        if (n > per_draw) n = per_draw;
        blt_vtx_t *tv = (blt_vtx_t *)malloc(sizeof(blt_vtx_t) * (size_t)n * 3);
        if (!tv) return -1;
        for (int i = 0; i < n; i++) {
            int idx = emitted + i;
            int x = (idx * 13 + (int)frame_no) % (FB_W - 24);
            int y = (idx * 7  + (int)frame_no) % (FB_H - 24);
            /* 24x24 half-quad — about a real sprite triangle's coverage. UVs span the
             * WHOLE page (not a 16-texel corner) so consecutive pixels walk far apart in
             * SDRAM and the texel fetches genuinely miss, which is what puts read
             * pressure on the arbiter alongside STAGE and the scanout reader. */
            uint16_t umax = (uint16_t)((tex_w - 1) << 4), vmax = (uint16_t)((tex_h - 1) << 4);
            tv[i*3+0] = (blt_vtx_t){ (int16_t)(x << 4),        (int16_t)(y << 4),
                                     (uint16_t)0,              (uint16_t)0,
                                     BLT_RGBA(255,255,255,255), 0 };
            tv[i*3+1] = (blt_vtx_t){ (int16_t)((x + 24) << 4), (int16_t)(y << 4),
                                     umax,                     (uint16_t)0,
                                     BLT_RGBA(255,255,255,255), 0 };
            tv[i*3+2] = (blt_vtx_t){ (int16_t)(x << 4),        (int16_t)((y + 24) << 4),
                                     (uint16_t)0,              vmax,
                                     BLT_RGBA(255,255,255,255), 0 };
        }
        uint32_t off = blt_push_tris(e, tv, n);
        free(tv);
        if (off == 0xFFFFFFFFu) return -1;
        if (blt_trilist(e, texs[(frame_no + (uint32_t)d) % (uint32_t)ntex],
                        BLT_BLEND_COLORKEY, /*colorkey=*/0xF81F, /*alpha=*/255,
                        off, n, /*flags=*/0) != 0) return -1;
        emitted += n;
    }

    if (c->appsurf) {
        if (blt_set_target(e, BLT_TARGET_WORK) != 0) return -1;
        if (blt_fill(e, 0, 0, FB_W, FB_H, rgb565(0, 0, 0)) != 0) return -1;
        /* Composite the app surface back over WORK as a full-screen quad, twice —
         * exactly what the captured device frame graph does (draw#N+1 / draw#N+2). */
        for (int pass = 0; pass < 2; pass++) {
            blt_vtx_t q[6] = {
                { 0, 0, 0, 0, BLT_RGBA(255,255,255,255), 0 },
                { (int16_t)(FB_W << 4), 0, (uint16_t)(FB_W << 4), 0, BLT_RGBA(255,255,255,255), 0 },
                { 0, (int16_t)(FB_H << 4), 0, (uint16_t)(FB_H << 4), BLT_RGBA(255,255,255,255), 0 },
                { (int16_t)(FB_W << 4), 0, (uint16_t)(FB_W << 4), 0, BLT_RGBA(255,255,255,255), 0 },
                { (int16_t)(FB_W << 4), (int16_t)(FB_H << 4), (uint16_t)(FB_W << 4), (uint16_t)(FB_H << 4),
                  BLT_RGBA(255,255,255,255), 0 },
                { 0, (int16_t)(FB_H << 4), 0, (uint16_t)(FB_H << 4), BLT_RGBA(255,255,255,255), 0 },
            };
            uint32_t off = blt_push_tris(e, q, 2);
            if (off == 0xFFFFFFFFu) return -1;
            /* tex fields are ignored under BLT_F_SRC_SURFACE (blt_emitter.h) */
            if (blt_trilist(e, texs[0],
                            c->cblend ? BLT_BLEND_CONST_ALPHA : BLT_BLEND_COPY,
                            0, 255, off, 2, BLT_F_SRC_SURFACE) != 0) return -1;
        }
    }

    blt_end_frame(e);
    return e->overflow ? -1 : 0;
}

int main(int argc, char **argv) {
    struct cfg c = { .secs = 60, .tris = 322, .draws = 10, .stage = 24, .upload = 0, .tex_kb = 32,
                     .appsurf = 1, .cblend = 0, .rate_hz = 60.0, .stall_ms = 250.0, .giveup_ms = 30000.0 };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        if (arg_int(a, "--secs=", &c.secs)      ) continue;
        if (arg_int(a, "--tris=", &c.tris)      ) continue;
        if (arg_int(a, "--draws=", &c.draws)    ) continue;
        if (arg_int(a, "--stage=", &c.stage)    ) continue;
        if (arg_int(a, "--upload=", &c.upload)  ) continue;
        if (arg_int(a, "--tex-kb=", &c.tex_kb)  ) continue;
        if (arg_int(a, "--appsurf=", &c.appsurf)) continue;
        if (arg_int(a, "--cblend=", &c.cblend)  ) continue;
        if (arg_dbl(a, "--rate=", &c.rate_hz)   ) continue;
        if (arg_dbl(a, "--stall-ms=", &c.stall_ms)) continue;
        if (arg_dbl(a, "--giveup-ms=", &c.giveup_ms)) continue;
        fprintf(stderr, "fabric_soak: unknown option %s\n", a); usage(argv[0]); return 2;
    }
    signal(SIGINT, on_sigint);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("fabric_soak: open /dev/mem"); return 1; }
    volatile uint8_t *base = mmap(NULL, MF_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                                  fd, (off_t)MF_PHYS_BASE);
    if (base == MAP_FAILED) { perror("fabric_soak: mmap"); close(fd); return 1; }
    volatile uint8_t *ctrl = base;
    volatile uint32_t *scanout = (volatile uint32_t *)(base + MF_SCANOUT_OFF);

    memset((void *)ctrl, 0, MF_RING_OFF);

    blt_emitter_t e;
    blt_emitter_init(&e, (void *)(base + MF_RING_OFF), MF_RING_CAP,
                     (void *)(base + MF_SRC_OFF), MF_SRC_CAP);
    blt_alloc_init(&e.alloc, (uint32_t)MF_VTX_REGION, (uint32_t)(MF_SRC_CAP - MF_VTX_REGION));
    blt_vtx_buf_init(&e, (void *)(base + MF_SRC_OFF), MF_VTX_REGION);

    /* Upload the texture pages ONCE (same-offset staging: the per-frame STAGE commands
     * re-copy them DDR3->SDRAM, which is the traffic we want, without re-uploading). */
    /* Square-ish pages: a 16-wide strip made every sprite sample the same few qwords,
     * so texwait measured 0.04ms against the engine's ~5ms — i.e. the texel-fetch load
     * (SDRAM reads contending with STAGE writes and the scanout traffic) was absent,
     * which is exactly the pressure this harness needs to reproduce. */
    int tex_w = 1, tex_h;
    while ((tex_w * 2) * (tex_w * 2) * 2 <= c.tex_kb * 1024) tex_w *= 2;
    tex_h = tex_w;
    int ntex = 32;
    blt_surface_ref_t *texs = calloc((size_t)ntex, sizeof *texs);
    uint16_t *px = malloc((size_t)tex_w * (size_t)tex_h * 2);
    if (!texs || !px) { fprintf(stderr, "fabric_soak: oom\n"); return 1; }
    for (int t = 0; t < ntex; t++) {
        for (int i = 0; i < tex_w * tex_h; i++)
            px[i] = rgb565((uint8_t)(40 + t * 7), (uint8_t)(i * 3), (uint8_t)(255 - t * 5));
        texs[t] = blt_upload(&e, px, tex_w, tex_h, tex_w * 2);
        if (!texs[t].valid) { fprintf(stderr, "fabric_soak: heap overflow at tex %d\n", t); return 1; }
    }
    free(px);

    printf("fabric_soak: tris=%d draws=%d stage=%d upload=%d tex=%dx%d(%dKiB) appsurf=%d "
           "cblend=%s rate=%.0fHz secs=%d stall_ms=%.0f\n",
           c.tris, c.draws, c.stage, c.upload, tex_w, tex_h, c.tex_kb, c.appsurf,
           c.cblend ? "ALPHA" : "COPY", c.rate_hz, c.secs, c.stall_ms);
    printf("fabric_soak: NOTE this harness never re-emits under an unacked batch — every\n"
           "             number below is the fabric's own behaviour, not host corruption.\n");

    /* ---- soak ------------------------------------------------------------------- */
    size_t lat_cap = 65536, lat_n = 0;
    double *lat = malloc(lat_cap * sizeof *lat);
    if (!lat) { fprintf(stderr, "fabric_soak: oom\n"); return 1; }

    /* source buffer for --upload (host RAM -> DDR, like the engine's texture staging) */
    uint8_t *upload_src = malloc((size_t)tex_w * (size_t)tex_h * 2);
    if (!upload_src) { fprintf(stderr, "fabric_soak: oom\n"); return 1; }
    memset(upload_src, 0x5A, (size_t)tex_w * (size_t)tex_h * 2);

    double t_start = now_ms();
    double period_ms = (c.rate_hz > 0.0) ? 1000.0 / c.rate_hz : 0.0;
    uint32_t frames = 0, stalls = 0, giveups = 0, build_fails = 0;
    double stalled_ms_total = 0.0, worst_ms = 0.0;
    uint32_t worst_scan_delta = 0; bool worst_scan_frozen = false;
    uint32_t scan_first = *scanout >> 2, scan_last = scan_first;

    while (!g_stop) {
        if (c.secs > 0 && (now_ms() - t_start) >= c.secs * 1000.0) break;
        double t_frame = now_ms();

        /* Host-side DDR write traffic: rewrite N texture pages through the mmap before
         * building the frame, mirroring the engine's per-frame sub-region uploads. The
         * pages keep their heap offsets (no re-alloc), so only the memcpy load is added. */
        for (int i = 0; i < c.upload; i++) {
            blt_surface_ref_t *t = &texs[(frames + (uint32_t)i) % (uint32_t)ntex];
            volatile uint8_t *dst = base + MF_SRC_OFF + t->off;
            memcpy((void *)dst, upload_src, (size_t)t->stride * (size_t)t->h);
        }

        if (build_frame(&e, &c, texs, ntex, frames, tex_w, tex_h) != 0) {
            build_fails++;
            /* ring/heap overflow is a harness bug, not a fabric result — stop rather
             * than silently measuring a truncated workload */
            fprintf(stderr, "fabric_soak: frame build failed (overflow=%d) — aborting\n", e.overflow);
            break;
        }

        ctrl_wr(ctrl, C_CMDCOUNT, (uint32_t)e.cmd_count);
        ctrl_wr(ctrl, C_TARGET,   (uint32_t)e.target_buf);
        ctrl_wr(ctrl, C_CLEAR,    (uint32_t)e.clear_color);
        ctrl_wr(ctrl, C_FLAGS,    (uint32_t)e.flags);
        ctrl_wr(ctrl, C_SRCSEL,   (uint32_t)e.sdram_src);
        __sync_synchronize();
        ctrl_wr(ctrl, C_SUBMIT, e.submit_seq);

        /* Wait for the ack. No timeout-and-continue: re-emitting here is exactly the
         * host-side corruption this harness exists to exclude. */
        double t0 = now_ms(), t_now = t0;
        uint32_t scan_at_submit = *scanout >> 2;
        bool acked = false;
        while (!g_stop) {
            if (ctrl_rd(ctrl, C_DONE) == e.submit_seq) { acked = true; break; }
            t_now = now_ms();
            if ((t_now - t0) >= c.giveup_ms) break;
        }
        double wait_ms = now_ms() - t0;
        uint32_t scan_at_done = *scanout >> 2;
        scan_last = scan_at_done;

        if (!acked) {
            giveups++;
            fprintf(stderr, "fabric_soak: seq=%u UNACKED after %.0fms (scanout %s) — "
                            "fabric wedged, stopping\n", e.submit_seq, wait_ms,
                    scan_at_done == scan_at_submit ? "FROZEN too" : "still advancing");
            break;
        }

        if (lat_n == lat_cap) {
            lat_cap *= 2; double *nl = realloc(lat, lat_cap * sizeof *lat);
            if (!nl) { fprintf(stderr, "fabric_soak: oom\n"); break; }
            lat = nl;
        }
        lat[lat_n++] = wait_ms;
        frames++;

        if (wait_ms >= c.stall_ms) {
            stalls++;
            stalled_ms_total += wait_ms;
            uint32_t sd = scan_at_done - scan_at_submit;
            if (wait_ms > worst_ms) {
                worst_ms = wait_ms;
                worst_scan_delta = sd;
                /* The diagnostic bit: did the scanout publish counter advance while the
                 * blitter was not acking? Frozen => the blitter never reached S_SNAP
                 * (or comp_fb_dma could not finish), which is the S_SNAP_DRAIN picture.
                 * Advancing => the blitter is merely slow, scanout is unaffected. */
                worst_scan_frozen = (sd == 0);
            }
            printf("  STALL seq=%u %.0fms  scanout_frames_during=%u %s\n",
                   e.submit_seq, wait_ms, sd, sd == 0 ? "(FROZEN)" : "");
            fflush(stdout);
        }

        if (period_ms > 0.0) {
            double spent = now_ms() - t_frame;
            if (spent < period_ms) {
                struct timespec ts;
                double rem = period_ms - spent;
                ts.tv_sec = (time_t)(rem / 1000.0);
                ts.tv_nsec = (long)((rem - ts.tv_sec * 1000.0) * 1e6);
                nanosleep(&ts, NULL);
            }
        }
    }

    /* ---- report ------------------------------------------------------------------ */
    double elapsed = (now_ms() - t_start) / 1000.0;
    double mean = 0.0, p50 = 0.0, p99 = 0.0, maxl = 0.0;
    if (lat_n) {
        for (size_t i = 0; i < lat_n; i++) mean += lat[i];
        mean /= (double)lat_n;
        double *s = malloc(lat_n * sizeof *s);
        if (s) {
            memcpy(s, lat, lat_n * sizeof *s);
            qsort(s, lat_n, sizeof *s, cmp_dbl);
            p50 = s[lat_n / 2];
            p99 = s[(size_t)((double)lat_n * 0.99)];
            maxl = s[lat_n - 1];
            free(s);
        }
    }
    uint32_t status = ctrl_rd(ctrl, C_STATUS);
    uint32_t wd_fires = (status >> 8) & 0xFFFFFFu;
    double frame_ms = (double)ctrl_rd_hi(ctrl, C_DONE)   / (MF_CLK_MHZ * 1000.0);
    double texw_ms  = (double)ctrl_rd_hi(ctrl, C_STATUS) / (MF_CLK_MHZ * 1000.0);
    double tri_ms   = (double)ctrl_rd_hi(ctrl, C_SRCSEL) / (MF_CLK_MHZ * 1000.0);

    printf("\nfabric_soak: %.1fs, %u frames acked (%.1f/s), %u stalls >=%.0fms, %u giveups\n",
           elapsed, frames, elapsed > 0 ? frames / elapsed : 0.0, stalls, c.stall_ms, giveups);
    printf("  latency ms: mean=%.1f p50=%.1f p99=%.1f max=%.1f\n", mean, p50, p99, maxl);
    printf("  fabric self-report (last frame): frame=%.2fms tri=%.2fms texwait=%.2fms  "
           "wd_fires=%u\n", frame_ms, tri_ms, texw_ms, wd_fires);
    printf("  scanout publishes during soak: %u\n", scan_last - scan_first);
    if (stalls)
        printf("  worst stall %.0fms: scanout published %u frames during it -> %s\n",
               worst_ms, worst_scan_delta,
               worst_scan_frozen ? "SCANOUT FROZE WITH THE BLITTER (S_SNAP tail / DMA stuck)"
                                 : "scanout kept running (blitter slow, not parked)");

    /* one machine-comparable line: diff this between RBFs or knob settings */
    printf("SOAK tris=%d stage=%d upload=%d appsurf=%d cblend=%d rate=%.0f | frames=%u stalls=%u "
           "stalled_pct=%.1f max_ms=%.0f p99_ms=%.1f wd=%u giveups=%u scan_frozen=%d\n",
           c.tris, c.stage, c.upload, c.appsurf, c.cblend, c.rate_hz, frames, stalls,
           elapsed > 0 ? 100.0 * (stalled_ms_total / 1000.0) / elapsed : 0.0,
           maxl, p99, wd_fires, giveups, worst_scan_frozen ? 1 : 0);

    free(lat); free(texs); free(upload_src);
    munmap((void *)base, MF_MAP_SIZE);
    close(fd);
    return (giveups || build_fails) ? 1 : 0;
}
