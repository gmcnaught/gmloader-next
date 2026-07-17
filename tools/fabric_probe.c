/*
 *  fabric_probe.c — standalone armhf contract probe (go/no-go for the Maldita
 *  fabric-offload plan: docs/superpowers/plans/2026-07-14-maldita-fabric-offload.md,
 *  maldita.castilla-mister repo, Task 1).
 *
 *  Submits ONE minimal frame straight to the real FPGA fabric's DDR command
 *  ring: a blue clear, one 8x8 opaque magenta texture staged DDR3->SDRAM, one
 *  centered textured triangle sampling it, then bumps the doorbell and polls
 *  C_DONE. Proves an external (non-Solarus) producer can drive the fabric
 *  before any production backend work is built on top of it.
 *
 *  This is a standalone tool — NOT wired into gmloader-next's main build. It
 *  links the vendored 3rdparty/mfgpu host emitter (blt_emitter.c/blt_alloc.c)
 *  directly against the real DDR3 physical region via mmap, mirroring
 *  gmloader/mister/native_video_writer.c's open()/O_SYNC/mmap() pattern
 *  exactly (see tools/Makefile.fabric_probe for the cross-build).
 *
 *  DDR map + control-block field layout: 3rdparty/mfgpu/docs/blitter-protocol.md
 *  §2-3 (mirrors 3rdparty/mfgpu/rtl/blitter_defs.vh's *_QW simulation constants
 *  at their real physical addresses).
 *
 *  Deploy + run on device / verify on screen: the plan's Task 1 Steps 3-4
 *  (controller-run — no MiSTer in this dev environment; the build gate here
 *  is a clean armhf link).
 *
 *  GPL-3.0 (matches 3rdparty/mfgpu).
 */
#include "blt_emitter.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- Real DDR physical map (blitter-protocol.md §2, "16 MiB region at
 * 0x3B000000"). BLTCTRL is 8 qwords (0x40 bytes) of control-block fields;
 * RING starts immediately after (the "offset-7 aliasing lesson" in the
 * protocol doc: the ring truly begins at control qword 8). SRC and the
 * later TL_BUF/FRT/CFT regions (not used by this probe) bound the SRC heap's
 * usable capacity. */
#define MF_PHYS_BASE   0x3B000000UL
#define MF_MAP_SIZE    0x01000000UL   /* 16 MiB — the whole dedicated blitter region */
#define MF_RING_OFF    0x00000040UL   /* BLTCTRL (8 qwords) precedes the ring         */
#define MF_SRC_OFF     0x00080000UL
#define MF_TLBUF_OFF   0x00F40000UL   /* TL_BUF; bounds SRC_CAP so we never step on it */
#define MF_RING_CAP    (MF_SRC_OFF   - MF_RING_OFF)   /* ~512 KiB (0x7FFC0)  */
#define MF_SRC_CAP     (MF_TLBUF_OFF - MF_SRC_OFF)    /* ~14.8 MiB           */

/* [hardware contract, confirmed in 3rdparty/mfgpu/rtl/blitter_top.sv:329 —
 * "entry_qw_base <= SRC_QW + ({c_dst_y, c_dst_x} >> 3)"] BLT_OP_TRILIST vertex
 * entries are fetched from the SAME SRC heap base as texture pages — there is
 * NO separate vertex-buffer DDR region. So the vertex buffer must be carved
 * out of the front of SRC and the texture allocator re-based after it, or a
 * texture upload and the vertex push would alias the same bytes. This mirrors
 * gmloader/mister/raster_backend_mfgpu.cpp's MF_VTX_REGION split exactly
 * (that file's "ONE SOURCE-DDR BUFFER" comment). */
#define MF_VTX_REGION  (128UL * 1024UL)

/* Control-block field offsets (qwords from BLTCTRL; ARM writes the low 32
 * bits of each 8-byte qword slot — the upper 32 bits are reserved/zero).
 * Matches 3rdparty/mfgpu/rtl/blitter_defs.vh's C_* defines and the task
 * brief's Global Constraints verbatim. */
enum {
    C_SUBMIT   = 0,
    C_CMDCOUNT = 1,
    C_TARGET   = 2,
    C_CLEAR    = 3,
    C_FLAGS    = 4,
    C_DONE     = 5,
    C_STATUS   = 6,
    C_SRCSEL   = 7,
};

/* SAME-OFFSET STAGING (user-confirmed hardware contract): stage each source to
 * the SAME SDRAM byte offset as its DDR3 heap offset (SDRAM[off] = DDR[off]) via
 * blt_stage(off,size). The core's OP_TRILIST texel fetch samples SDRAM
 * unconditionally at src_off == tex.off (blitter_top.sv:919-923), and blt_trilist
 * writes src_off = tex.off — so a decoupled sdram_off (blt_stage_surface +
 * blt_sdram_init) would be sampled at the WRONG address and render black. With
 * same-offset staging no SDRAM allocator (blt_sdram_init) and no C_SRCSEL master
 * enable are needed. */
#define MF_DONE_TIMEOUT_MS 500

static inline uint16_t mf_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Control-block field access. Each field is a full 8-byte qword slot; only
 * the low 32 bits are meaningful (blitter-protocol.md §3, "one u32 per qword
 * slot"), so address as byte offset (qword_index * 8). */
static inline void ctrl_write32(volatile uint8_t *ctrl, int qword, uint32_t val) {
    volatile uint32_t *p = (volatile uint32_t *)(ctrl + (size_t)qword * 8u);
    *p = val;
}
static inline uint32_t ctrl_read32(volatile uint8_t *ctrl, int qword) {
    volatile uint32_t *p = (volatile uint32_t *)(ctrl + (size_t)qword * 8u);
    return *p;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
    /* ---- mmap the real DDR blitter region — mirrors native_video_writer.c's
     * open()/O_SYNC/mmap() pattern exactly (gmloader/mister/native_video_writer.c). */
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("fabric_probe: open /dev/mem"); return 1; }

    volatile uint8_t *base = (volatile uint8_t *)mmap(
        NULL, (size_t)MF_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
        (off_t)MF_PHYS_BASE);
    if (base == MAP_FAILED) {
        perror("fabric_probe: mmap");
        close(fd);
        return 1;
    }

    volatile uint8_t *ctrl   = base;                     /* BLTCTRL, 0x3B000000 */
    void             *ring_ptr = (void *)(base + MF_RING_OFF); /* RING, 0x3B000040 */
    void             *src_ptr  = (void *)(base + MF_SRC_OFF);  /* SRC,  0x3B080000 */

    /* Zero the control block up front (reserved qword halves + the
     * fabric-owned C_DONE/C_STATUS start clean). */
    memset((void *)ctrl, 0, (size_t)MF_RING_OFF);

    /* ---- Bind the emitter to the mmap'd ring + source heap ---------------- */
    blt_emitter_t e;
    blt_emitter_init(&e, ring_ptr, (size_t)MF_RING_CAP, src_ptr, (size_t)MF_SRC_CAP);
    /* Re-base the texture allocator after the reserved vertex region (see the
     * MF_VTX_REGION comment above) and bind the TRILIST vertex buffer to the
     * front of the SAME SRC heap. */
    blt_alloc_init(&e.alloc, (uint32_t)MF_VTX_REGION,
                   (uint32_t)(MF_SRC_CAP - MF_VTX_REGION));
    blt_vtx_buf_init(&e, src_ptr, (size_t)MF_VTX_REGION);
    /* No blt_sdram_init: same-offset staging needs no SDRAM allocator (see the
     * MF_DONE_TIMEOUT_MS comment block above). e.sdram_src stays 0. */

    /* ---- Build the one-frame scene ---------------------------------------- */
    blt_begin_frame(&e, /*target_buf=*/0, /*clear=*/1, mf_rgb565(0, 0, 160)); /* blue */

    uint16_t tex_px[8 * 8];
    uint16_t magenta = mf_rgb565(255, 0, 255);
    for (int i = 0; i < 8 * 8; i++) tex_px[i] = magenta;

    blt_surface_ref_t tex = blt_upload(&e, tex_px, 8, 8, 8 * 2);
    if (!tex.valid) {
        fprintf(stderr, "fabric_probe: blt_upload overflow\n");
        munmap((void *)base, MF_MAP_SIZE);
        close(fd);
        return 1;
    }
    if (blt_stage(&e, tex.off, (uint32_t)tex.stride * tex.h) != 0) {   /* same-offset */
        fprintf(stderr, "fabric_probe: blt_stage failed (overflow=%d)\n", e.overflow);
        munmap((void *)base, MF_MAP_SIZE);
        close(fd);
        return 1;
    }

    /* One centered triangle. blt_vtx_t = {x,y (12.4 signed px), u,v (12.4
     * unsigned texels), rgba, _rsvd}. White vertex color -> texture unmodified. */
    blt_vtx_t tris[3] = {
        { (int16_t)(160 << 4), (int16_t)(48  << 4), (uint16_t)(4 << 4), (uint16_t)(0 << 4), BLT_RGBA(255, 255, 255, 255), 0 },
        { (int16_t)(224 << 4), (int16_t)(176 << 4), (uint16_t)(7 << 4), (uint16_t)(7 << 4), BLT_RGBA(255, 255, 255, 255), 0 },
        { (int16_t)(96  << 4), (int16_t)(176 << 4), (uint16_t)(0 << 4), (uint16_t)(7 << 4), BLT_RGBA(255, 255, 255, 255), 0 },
    };
    uint32_t entry_off = blt_push_tris(&e, tris, 1);
    if (entry_off == 0xFFFFFFFFu) {
        fprintf(stderr, "fabric_probe: blt_push_tris overflow\n");
        munmap((void *)base, MF_MAP_SIZE);
        close(fd);
        return 1;
    }
    if (blt_trilist(&e, tex, BLT_BLEND_COPY, /*colorkey=*/0, /*alpha=*/255, entry_off, 1, /*flags=*/0) != 0) {
        fprintf(stderr, "fabric_probe: blt_trilist emit failed\n");
        munmap((void *)base, MF_MAP_SIZE);
        close(fd);
        return 1;
    }
    blt_end_frame(&e);
    if (e.overflow) {
        fprintf(stderr, "fabric_probe: emitter overflow — not submitting\n");
        munmap((void *)base, MF_MAP_SIZE);
        close(fd);
        return 1;
    }

    /* ---- Publish -----------------------------------------------------------
     * Ring + source-heap bytes already landed in DDR: the emitter wrote
     * directly through the mmap'd ring_ptr/src_ptr above (blt_upload,
     * blt_stage_surface, blt_push_tris, blt_trilist all copy/pack straight
     * into those buffers — no separate publish step for them). Now publish
     * the control-block mirror, THEN the doorbell (submit_seq) LAST, after a
     * memory barrier, so the doorbell can never be observed before the data
     * it describes. */
    ctrl_write32(ctrl, C_CMDCOUNT, (uint32_t)e.cmd_count);
    ctrl_write32(ctrl, C_TARGET,   (uint32_t)e.target_buf);
    ctrl_write32(ctrl, C_CLEAR,    (uint32_t)e.clear_color);
    ctrl_write32(ctrl, C_FLAGS,    (uint32_t)e.flags);
    ctrl_write32(ctrl, C_SRCSEL,   (uint32_t)e.sdram_src); /* 0: TRILIST texel fetch is
                                                            * unconditional-SDRAM; C_SRCSEL unused */

    __sync_synchronize();                        /* data before doorbell */
    ctrl_write32(ctrl, C_SUBMIT, e.submit_seq);   /* doorbell LAST        */

    /* ---- Poll C_DONE (~500 ms timeout) -------------------------------------- */
    long t0 = now_ms();
    uint32_t done = 0;
    bool timed_out = true;
    while (now_ms() - t0 < MF_DONE_TIMEOUT_MS) {
        done = ctrl_read32(ctrl, C_DONE);
        if (done == e.submit_seq) { timed_out = false; break; }
    }
    uint32_t status = ctrl_read32(ctrl, C_STATUS);

    printf("fabric_probe: submit_seq=%u C_DONE=%u C_STATUS=%u %s\n",
           e.submit_seq, done, status, timed_out ? "TIMEOUT" : "OK");

    /* ---- Hold the frame on screen (defeat the stale-frame watchdog) ---------
     * openbor_video_reader blanks VGA to black if VCTRL's frame counter does not
     * advance for ~29 vblanks (~0.5s) — openbor_video_reader.sv:710-715. A ONE-SHOT
     * submit therefore flashes the frame for ~0.5s and then goes black. Re-walk the
     * SAME ring (still resident in DDR) every 200ms so the counter keeps moving and
     * the frame stays visible — exactly what the real gmloader path does at 60fps.
     * Duration from argv[1] seconds (default 30; 0 = until Ctrl-C). */
    if (!timed_out) {
        long hold_sec = (argc > 1) ? atol(argv[1]) : 30;
        if (hold_sec) printf("fabric_probe: holding frame ~%lds (re-submit every 200ms; Ctrl-C to stop)\n", hold_sec);
        else          printf("fabric_probe: holding frame until Ctrl-C (re-submit every 200ms)\n");
        uint32_t seq = e.submit_seq;
        long hstart = now_ms();
        while (hold_sec == 0 || now_ms() - hstart < hold_sec * 1000L) {
            __sync_synchronize();
            ctrl_write32(ctrl, C_SUBMIT, ++seq);                    /* re-walk the same ring */
            long tp = now_ms();
            while (now_ms() - tp < 50 && ctrl_read32(ctrl, C_DONE) != seq) { }
            usleep(200000);                                         /* 200ms < ~0.5s watchdog */
        }
    }

    munmap((void *)base, MF_MAP_SIZE);
    close(fd);
    return timed_out ? 2 : (status != 0 ? 3 : 0);
}
