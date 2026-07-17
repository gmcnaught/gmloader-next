// backend_mfgpu: FPGA-fabric back-end (Task 5). Owns the emitter + frame
// lifecycle over host-side ring/heap/vtx buffers (device wiring to the real DDR
// ring is Task 6) and is validated on the host by executing the emitted ring
// through the SAME blt_execute software model the mfgpu refmodel unit tests use
// (3rdparty/mfgpu/host/test_emitter.c).
//
//   clear   : emits a REAL fabric BLT_OP_FILL via blt_fill() (not a direct
//             pixel write) so the parity tests in raster_backend_test.cpp
//             exercise the same emit -> blt_execute path draw() uses.
//   draw    : converts the decoded screen-space triangle list into a
//             BLT_OP_TRILIST fabric command (blt_push_tris + blt_trilist) over
//             an RGB565 texture page. FBO / non-default targets and RB_PREMULT
//             fall back to backend_sw (see notes below).
//   present : executes the accumulated ring (blt_execute -> g_fb565) and closes
//             the frame; main.cpp then scans g_fb565 out via NativeVideoWriter (Task 7).
//
// The fabric framebuffer geometry is FIXED at BLT_FB_WIDTH x BLT_FB_HEIGHT
// (320x240, refmodel/blitter_ref.h) — that is the wire contract, not a
// per-draw parameter. clear() targets are clamped to that size; the engine's
// render surface is comfortably inside it.
//
// ── FRAME MODEL (Task 2 update: persistent texture cache) ────────────────────
// blt_emitter_init / blt_vtx_buf_init / blt_alloc_init are ONE-TIME startup
// (they wire the ring, texture heap and vertex buffer). Only blt_begin_frame
// resets per-frame state (command list + vertex cursor). mf_init_once() does
// the one-time wiring; mf_frame_begin no longer resets the texture heap
// (no blt_heap_reset) — staged texture pages are cached by identity in
// g_texcache and persist across frames (see stage_texture below), so a draw
// that reuses a texture id reuses its already-uploaded page instead of
// re-uploading every frame.
//
// ── ONE SOURCE-DDR BUFFER ────────────────────────────────────────────────────
// blt_execute resolves BOTH the TRILIST vertices (heap->base + entry_off) and
// the texture page (heap->base + src_off) from a SINGLE heap base. So the vertex
// buffer and the texture-upload heap must live in one backing store, at disjoint
// offset ranges. g_srcdram is that store: [0, MF_VTX_REGION) is the per-frame
// vertex buffer (blt_vtx_buf_init base), [MF_VTX_REGION, cap) is the texture
// allocator (blt_alloc_init base). Both offset spaces are then valid relative to
// g_srcdram, exactly as test_emitter.c's single srcdram buffer.
//
// ── TEXTURE FORMAT (conforms to the golden blt_raster_tri) ───────────────────
// The TRILIST rasterizer (refmodel/blt_tri.c) samples the texture page as
// RGB565 (tex_nearest reads a raw 16-bit texel; the format byte is ignored) and
// modulates it by the interpolated per-vertex color (blt_tint565). There is NO
// per-texel alpha and NO BLT_BLEND_PALPHA case in a triangle list. So textures
// are staged as RGB565 (blt_upload, not blt_upload_argb4444), an untextured draw
// uses a 1x1 opaque-white page, and alpha compositing rides BLT_BLEND_CONST_ALPHA
// with header alpha=255 (effective alpha = interpolated vtx.a). See
// raster_backend_convert.h for the full rationale.
//
// ── COLORKEY STAGING FOR HARD-EDGED (1-BIT) ALPHA (Task 6) ────────────────────
// The one hole in "no per-texel alpha": blt_tri.c DOES have BLT_BLEND_COLORKEY
// (skip a texel that == the header colorkey). mf_texel565() folds any texel
// with alpha<128 into the sentinel MF_COLORKEY (0xF81F, magenta), nudging any
// opaque texel that happens to convert to that exact value off by one green
// LSB so it can never collide. mf_draw() then emits BLT_BLEND_COLORKEY instead
// of the Task 5 blend when the staged texture used the key (has_key) AND the
// draw's vertex alpha is fully opaque (min vtx.a*255 >= 254) — that is exactly
// the case (256-color palette sprites, e.g. Maldita Castilla) where the SW
// oracle's per-texel-alpha result and the fabric's colorkey result are
// pixel-identical (mod 565 rounding). A faded cutout (vtx.a < 254 on a keyed
// texture) can't combine colorkey + const-alpha in one TRILIST pass, so it
// falls back to CONST_ALPHA (no cutout, see the comment at the call site);
// real per-texel alpha is a future RTL item.
#include "raster_backend.h"
#include "raster_backend_convert.h"
extern "C" {
#include "blt_emitter.h"
#include "blt_wire.h"
#include "blitter_ref.h"
}
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // getenv (GMLOADER_MFGPU_HEAPLOG diagnostic, Task 9 bring-up)

extern "C" const RasterBackend backend_sw;   /* FBO / RB_PREMULT fallback */

// One frame's worth of host-side emitter buffers. Static/file-scope is fine for
// this single-threaded backend; Task 6 points these at the real DDR ring/heap.
enum {
    MF_RING_CAP    = 64 * 1024,
    MF_VTX_REGION  = 128 * 1024,                 // per-frame TRILIST vertex buffer
    MF_TEX_HEAP    = 32u * 1024 * 1024,          // persistent texture pages (32MB)
    MF_SRCDRAM_CAP = MF_VTX_REGION + MF_TEX_HEAP,
    MF_MAX_CMDS    = MF_RING_CAP / BLT_CMD_BYTES,
    MF_MAX_VERTS   = 8192,
    MF_TEX_TEXELS  = 2048 * 2048,                // scratch = largest single page (8MB)
    MF_TEX_CACHE_N = 64,                         // resident-page table size
};
static uint8_t       g_ring[MF_RING_CAP];
// Deliberate ~40MB static footprint (32MB texture heap here + 8MB conversion
// scratch below): safe and unremarkable on the MiSTer ARM's DDR3, not a
// mistake to "optimize away".
static uint8_t       g_srcdram[MF_SRCDRAM_CAP];
static blt_emitter_t g_e;
static blt_cmd_t     g_cmds[MF_MAX_CMDS];
static blt_vtx_t     g_vtxscratch[MF_MAX_VERTS];
// See g_srcdram above: this is the other half of the ~40MB deliberate footprint.
static uint16_t      g_texscratch[MF_TEX_TEXELS];
static bool          g_inited = false;

// Task 7: the production frame loop (main.cpp) calls clear()/draw()/present()
// but never frame_begin()/frame_end() (those are a host-test-only convenience —
// see mf_frame_begin's comment). g_frame_active makes the backend self-contained
// in that world: the first clear/draw of a frame implicitly opens it
// (mf_ensure_frame), and present() closes it (runs blt_execute into g_fb565 and
// resets state so the next clear/draw starts a fresh frame). Host tests that
// call frame_begin()/clear()/draw()/frame_end() directly are unaffected: those
// calls just re-set/consume the same flag along the way.
static bool          g_frame_active = false;

// Persistent resident-texture cache. Keyed by GL texture id (tex_key). Survives
// across frames (mf_frame_begin no longer resets the heap); entries are freed on
// GL invalidate/delete (Task 3) or LRU eviction under heap pressure (Task 4).
struct MfTexEntry { uint32_t key; bool used; bool has_key; blt_surface_ref_t ref; uint64_t lru; };
static MfTexEntry g_texcache[MF_TEX_CACHE_N];
static uint64_t   g_lru_clock   = 0;
static uint32_t   g_upload_count = 0;   // real blt_upload calls since reinit (test hook)
static uint32_t   g_stage_count  = 0;   // BLT_OP_STAGE emits since reinit (FO Task 3 test hook)
static uint32_t   g_tex_heap_cap = 0;   // 0 => full MF_TEX_HEAP; else test override

// Snapshot of g_lru_clock taken at the start of the current frame (see
// mf_frame_begin). Any cache entry whose .lru is ABOVE this floor was
// touched (hit or staged) during THIS frame, and is therefore "pinned":
// blt_trilist already emitted a command referencing its heap offset, and
// that command isn't consumed until blt_execute runs in mf_frame_end, so
// freeing/reusing the offset before then would alias an already-emitted
// draw onto different pixels. evict_one_lru() only considers entries with
// .lru <= floor (untouched this frame) as eviction candidates.
static uint64_t   g_lru_frame_floor = 0;

// Free the least-recently-used cached entry that is NOT pinned by this
// frame (see g_lru_frame_floor above). Returns false if no unpinned entry
// is available to evict (the caller's upload then legitimately fails and
// the draw is dropped — correct degradation when a single frame's texture
// working set exceeds the heap).
static bool evict_one_lru(void);

// Host execute target: the fixed-size fabric framebuffer blt_execute composites
// into. On real hardware this is scanned out directly; on the host it is the
// oracle the parity tests read back via RasterBackend_MFGPU_TestCopyFB565().
static uint16_t      g_fb565[BLT_FB_PIXELS];

// The default-framebuffer surface pixels (blitter.cpp's g_defSurf). A draw whose
// dst->rgba differs is an FBO / render-to-texture target the single-scanout
// fabric cannot represent, so it falls back to the SW rasterizer. NULL (unset,
// e.g. before Task 6 wires it) => treat every target as the default fb. Set on
// the host by RasterBackend_MFGPU_SetDefaultSurface().
static const uint8_t *g_defRGBA = nullptr;

// [app-surface render target, step 1] The application-surface FBO id / its
// color-attachment texture id, pushed down by blitter.cpp's
// RasterBackend_MFGPU_SetAppSurface() whenever Blitter_AppSurfaceFBO()/Tex()
// (Task 4) update -- mirrors the g_defRGBA push-down pattern above so this
// backend stays GL-independent. 0/0 = not yet detected (every draw/clear
// routes as "not the app surface", matching today's behavior exactly).
static uint32_t g_appSurfFbo = 0, g_appSurfTex = 0;

// [app-surface render target, step 1] Which fabric composite target the
// emitter's LAST BLT_OP_SET_TARGET pointed at, mirroring blt_wire's
// BLT_TARGET_*; used only to skip a redundant re-emit when consecutive draws
// target the same buffer. Reset to MF_TARGET_WORK at the top of every frame
// (mf_frame_begin) -- blt_execute (and the RTL) always start a fresh command
// list targeting WORK regardless of where the PREVIOUS frame's ring left off,
// so this host-side cache must track that same per-frame reset or an early
// appsurf-targeted draw could silently land on WORK instead.
enum { MF_TARGET_WORK = BLT_TARGET_WORK, MF_TARGET_APPSURF = BLT_TARGET_APPSURF };
static int g_cur_target = MF_TARGET_WORK;

extern "C" void RasterBackend_MFGPU_SetAppSurface(uint32_t fbo, uint32_t tex) {
    g_appSurfFbo = fbo; g_appSurfTex = tex;
}

static inline uint16_t mf_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Sentinel RGB565 value (magenta) used to mark "this texel is transparent" for
// BLT_BLEND_COLORKEY staging (Task 6). The golden rasterizer (blt_tri.c) skips
// any texel that compares == the header colorkey, so a hard-edged (alpha in
// {0,255}) 1-bit-alpha texture round-trips through colorkey exactly.
static const uint16_t MF_COLORKEY = 0xF81F;

// Convert one RTexture texel to RGB565, folding hard-edged alpha into the
// BLT_BLEND_COLORKEY sentinel (Task 6). Soft/intermediate alpha still has no
// fabric representation (no per-texel alpha in a triangle list) and is treated
// as opaque, same as Task 5.
//   alpha < 128            -> emit MF_COLORKEY (this texel is "hole" for a
//                              colorkey draw; harmless dead colour otherwise).
//   alpha >= 128            -> convert RGB->565; if that exact value collides
//                              with MF_COLORKEY, nudge it off by one green LSB
//                              so an opaque texel can never be mistaken for the
//                              key (out_has_key still only set by real holes).
static inline uint16_t mf_texel565(const RTexture *t, int x, int y, bool *out_has_key) {
    uint8_t r, g, b, a;
    if (t->format == RTEX_RGBA4444) {
        const uint16_t *p16 = (const uint16_t *)t->rgba;
        uint16_t p = p16[(size_t)y * t->w + x];        // packed (R4<<12|G4<<8|B4<<4|A4)
        unsigned r4 = (p >> 12) & 0xF, g4 = (p >> 8) & 0xF, b4 = (p >> 4) & 0xF, a4 = p & 0xF;
        r = (uint8_t)((r4 << 4) | r4);
        g = (uint8_t)((g4 << 4) | g4);
        b = (uint8_t)((b4 << 4) | b4);                  // nibble replicate -> 8-bit
        a = (uint8_t)((a4 << 4) | a4);
    } else {
        const uint8_t *p = t->rgba + ((size_t)y * t->w + x) * 4;  // RTEX_RGBA8888
        r = p[0]; g = p[1]; b = p[2]; a = p[3];
    }
    if (a < 128) { *out_has_key = true; return MF_COLORKEY; }
    uint16_t result = mf_rgb565(r, g, b);
    if (result == MF_COLORKEY) result ^= 0x0020;   // opaque texel must never == the key
    return result;
}

#ifdef MISTER_NATIVE_VIDEO
// ── Device fabric transport (FO Task 4; in-place DDR model, mirrors fabric_probe.c)
// On device the emitter binds DIRECTLY to the mmap'd DDR command region: blt_upload/
// blt_stage/blt_push_tris/blt_trilist write straight into DDR, so mf_frame_end only
// publishes the control block + rings the doorbell + polls C_DONE — no blt_execute,
// no per-frame copy. The Maldita core composites into on-chip BRAM and scans itself
// out. Layout + handshake: 3rdparty/mfgpu/docs/blitter-protocol.md §2-3, verified vs
// the milestone-a RTL. (MISTER_NATIVE_VIDEO is the device-build macro — the host
// raster-backend-test target does NOT define it, so it keeps the blt_execute oracle.)
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>   // getenv (GMLOADER_MFSUBMIT_STAT instrumentation)
enum {
    MF_DEV_PHYS_BASE = 0x3B000000u,
    MF_DEV_MAP_SIZE  = 0x01000000u,   // 16 MiB dedicated blitter region
    MF_DEV_RING_OFF  = 0x00000040u,   // BLTCTRL (8 qwords) precedes the ring
    MF_DEV_SRC_OFF   = 0x00080000u,   // SRC_QW = 0x3B080000 (DDR3 source heap)
    MF_DEV_TLBUF_OFF = 0x00F40000u,   // bounds the usable SRC heap (~14.8 MiB)
    MF_DEV_RING_CAP  = MF_DEV_SRC_OFF   - MF_DEV_RING_OFF,
    MF_DEV_SRC_CAP   = MF_DEV_TLBUF_OFF - MF_DEV_SRC_OFF,
    MF_C_SUBMIT = 0, MF_C_CMDCOUNT = 1, MF_C_TARGET = 2, MF_C_CLEAR = 3,
    MF_C_FLAGS = 4, MF_C_DONE = 5, MF_C_STATUS = 6, MF_C_SRCSEL = 7,
    MF_DEV_DONE_TIMEOUT_MS = 200,
};
static volatile uint8_t *g_dev_base = nullptr;   // mmap of 0x3B000000
static volatile uint8_t *g_dev_ctrl = nullptr;   // = base (control block)
static uint8_t          *g_dev_ring = nullptr;   // = base + RING_OFF
static uint8_t          *g_dev_src  = nullptr;   // = base + SRC_OFF
static bool              g_dev_ok   = false;     // mmap ok -> submits reach the fabric

static inline void mf_ctrl_wr(int qw, uint32_t v) {
    *(volatile uint32_t *)(g_dev_ctrl + (size_t)qw * 8u) = v;   // low u32 of each 8B qword
}
static inline uint32_t mf_ctrl_rd(int qw) {
    return *(volatile uint32_t *)(g_dev_ctrl + (size_t)qw * 8u);
}
static inline uint32_t mf_ctrl_rd_hi(int qw) {
    return *(volatile uint32_t *)(g_dev_ctrl + (size_t)qw * 8u + 4u);   // high u32 of the qword
}
// Open /dev/mem + mmap the DDR blitter region once (mirrors native_video_writer.c).
static bool mf_ddr_map(void) {
    if (g_dev_base) return g_dev_ok;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { fprintf(stderr, "backend_mfgpu: open /dev/mem failed\n"); return false; }
    void *m = mmap(nullptr, MF_DEV_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)MF_DEV_PHYS_BASE);
    close(fd);   // the mapping outlives the fd
    if (m == MAP_FAILED) { fprintf(stderr, "backend_mfgpu: mmap 0x3B000000 failed\n"); return false; }
    g_dev_base = (volatile uint8_t *)m;
    g_dev_ctrl = g_dev_base;
    g_dev_ring = (uint8_t *)(g_dev_base + MF_DEV_RING_OFF);
    g_dev_src  = (uint8_t *)(g_dev_base + MF_DEV_SRC_OFF);
    memset((void *)g_dev_ctrl, 0, MF_DEV_RING_OFF);   // zero the control block
    g_dev_ok = true;
    return true;
}
// Instrumentation (perf profiling of the "capture" bucket): characterize the
// C_DONE busy-wait — how long we blocked, how many spin iterations that cost,
// and whether it exited on match vs timeout. min≈max over a 30-submit window ⇒
// the wait is quantized (vsync-locked fabric completion) rather than jittery.
// Enable with GMLOADER_MFSUBMIT_STAT=1. Zero cost when unset.
static int mf_stat_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFSUBMIT_STAT"); v = (e && *e) ? 1 : 0; }
    return v;
}
// Fire-and-forget probe (GMLOADER_MFSUBMIT_NOWAIT=1): ring the doorbell and
// return WITHOUT polling C_DONE, to measure the throughput ceiling when the host
// stops serially waiting out the fabric's ~3-vsync completion latency. Unsafe for
// production (the in-place DDR ring can be re-emitted while the fabric still reads
// it → tearing) — a measurement knob only. Zero cost when unset.
static int mf_nowait_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFSUBMIT_NOWAIT"); v = (e && *e) ? 1 : 0; }
    return v;
}
// clk_sys = 98.4375 MHz (PLL outclk_0 / DDRAM_CLK). Fabric perf counters are in
// clk_sys cycles: C_DONE high word = perf_frame_cyc (total fabric-busy: new-submit
// detect → done write), C_STATUS high word = perf_pipe_cyc (composite pipeline busy).
// Reading them isolates fabric COMPUTE from the host-observed doorbell→done wait: if
// compute << wait, the 3-vsync latency is notice/DDR-visibility, not the fabric.
#define MF_CLK_SYS_MHZ 98.4375
static void mf_submit_stat(const struct timespec *t0, long iters, int timeout) {
    if (!mf_stat_on()) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    double us = (now.tv_sec - t0->tv_sec) * 1e6 + (now.tv_nsec - t0->tv_nsec) / 1e3;
    // Fabric per-state cycle counters (clk_sys), from the profiling RTL:
    //   frame   = C_DONE.hi   perf_frame_cyc   (total fabric-busy: detect->done)
    //   texwait = C_STATUS.hi perf_texwait_cyc (S_TRI_GOTTEX p0_ok stall — texel fetch)
    //   tri     = C_SRCSEL.hi perf_tri_cyc     (all S_TRI_* states)
    // Derived: dpath = tri-texwait (rasterizer datapath), ovhd = frame-tri (ring/clear/setup).
    double frame_ms = (double)mf_ctrl_rd_hi(MF_C_DONE)   / (MF_CLK_SYS_MHZ * 1000.0);
    double texw_ms  = (double)mf_ctrl_rd_hi(MF_C_STATUS) / (MF_CLK_SYS_MHZ * 1000.0);
    double tri_ms   = (double)mf_ctrl_rd_hi(MF_C_SRCSEL) / (MF_CLK_SYS_MHZ * 1000.0);
    static unsigned n = 0, to = 0; static double lo = 1e12, hi = 0, sum = 0; static long it_sum = 0;
    static double fsum = 0, tsum = 0, xsum = 0;
    n++; to += timeout ? 1u : 0u; sum += us; it_sum += iters; fsum += frame_ms; tsum += tri_ms; xsum += texw_ms;
    if (us < lo) lo = us; if (us > hi) hi = us;
    if (n % 30 == 0) {
        double f=fsum/30.0, t=tsum/30.0, x=xsum/30.0;
        fprintf(stderr, "MFSUBMIT n=%u wait_ms[avg=%.2f] fabric_ms[frame=%.2f tri=%.2f "
                "texwait=%.2f dpath=%.2f ovhd=%.2f] spin_avg=%ld to=%u\n",
                n, (sum/30.0)/1e3, f, t, x, t-x, f-t, it_sum/30, to);
        lo = 1e12; hi = 0; sum = 0; it_sum = 0; to = 0; fsum = 0; tsum = 0; xsum = 0;
    }
}

// Publish the emitter's control-block mirror, ring the doorbell (submit_seq LAST,
// after a barrier), poll C_DONE. Ring + heap are already resident in DDR (the
// emitter was bound to g_dev_ring/g_dev_src), so there is nothing to copy. This
// core's C_STATUS is OSD-mirror, not an error latch — completion == C_DONE match,
// failure == timeout.
static void mf_device_submit(void) {
    mf_ctrl_wr(MF_C_CMDCOUNT, (uint32_t)g_e.cmd_count);
    mf_ctrl_wr(MF_C_TARGET,   (uint32_t)g_e.target_buf);
    mf_ctrl_wr(MF_C_CLEAR,    (uint32_t)g_e.clear_color);
    mf_ctrl_wr(MF_C_FLAGS,    (uint32_t)g_e.flags);
    __sync_synchronize();                       // data before doorbell
    mf_ctrl_wr(MF_C_SUBMIT, g_e.submit_seq);    // doorbell LAST
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (mf_nowait_on()) { mf_submit_stat(&t0, 0, /*timeout=*/0); return; }  // probe: no poll
    long iters = 0;
    for (;;) {
        if (mf_ctrl_rd(MF_C_DONE) == g_e.submit_seq) {   // fabric consumed it
            mf_submit_stat(&t0, iters, /*timeout=*/0);
            return;
        }
        iters++;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
        if (ms >= MF_DEV_DONE_TIMEOUT_MS) {
            fprintf(stderr, "backend_mfgpu: fabric submit timeout (submit=%u done=%u status=%u)\n",
                    g_e.submit_seq, mf_ctrl_rd(MF_C_DONE), mf_ctrl_rd(MF_C_STATUS));
            mf_submit_stat(&t0, iters, /*timeout=*/1);
            return;
        }
    }
}
#endif // MISTER_NATIVE_VIDEO

static void mf_init_once(void) {
    if (g_inited) return;
#ifdef MISTER_NATIVE_VIDEO
    if (mf_ddr_map()) {
        // In-place: the emitter builds ring + heap directly in the mmap'd DDR.
        blt_emitter_init(&g_e, g_dev_ring, MF_DEV_RING_CAP, g_dev_src, MF_DEV_SRC_CAP);
        blt_alloc_init(&g_e.alloc, MF_VTX_REGION, MF_DEV_SRC_CAP - MF_VTX_REGION);
        blt_vtx_buf_init(&g_e, g_dev_src, MF_VTX_REGION);
    } else {
        // No /dev/mem: bind host RAM so the emitter never faults; g_dev_ok is false
        // so mf_frame_end drops (never submits to a null map).
        blt_emitter_init(&g_e, g_ring, sizeof g_ring, g_srcdram, sizeof g_srcdram);
        blt_alloc_init(&g_e.alloc, MF_VTX_REGION, MF_TEX_HEAP);
        blt_vtx_buf_init(&g_e, g_srcdram, MF_VTX_REGION);
    }
#else
    blt_emitter_init(&g_e, g_ring, sizeof g_ring, g_srcdram, sizeof g_srcdram);
    uint32_t cap = g_tex_heap_cap ? g_tex_heap_cap : MF_TEX_HEAP;
    blt_alloc_init(&g_e.alloc, MF_VTX_REGION, cap);       // texture allocator (persistent)
    blt_vtx_buf_init(&g_e, g_srcdram, MF_VTX_REGION);     // per-frame vertex buffer
#endif
    for (int i = 0; i < MF_TEX_CACHE_N; i++) g_texcache[i].used = false;
    g_lru_clock = 0; g_upload_count = 0; g_stage_count = 0; g_lru_frame_floor = 0;
    g_inited = true;
}

static void mf_frame_begin(void) {
    mf_init_once();
    // Snapshot the pin floor for this frame: any g_texcache entry touched
    // (hit or staged) from here through mf_frame_end gets .lru > this value
    // and is thus protected from eviction until the NEXT frame begins (see
    // g_lru_frame_floor and evict_one_lru).
    g_lru_frame_floor = g_lru_clock;
    // Textures persist across frames now (cache in g_texcache). Only the vtx
    // cursor + command list reset here (blt_begin_frame). No blt_heap_reset.
    blt_begin_frame(&g_e, /*target_buf=*/0, /*clear=*/0, /*clear_color=*/0);
    // [app-surface render target, step 1] Every fresh ring starts targeting
    // WORK (blt_execute's own default, matched by the RTL) -- see g_cur_target's
    // comment for why this reset (not just the variable's initial value) matters.
    g_cur_target = MF_TARGET_WORK;
    g_frame_active = true;
}

// Task 7: production entry point for clear()/draw() — the frame loop never
// calls frame_begin() directly, so the first clear/draw of a frame opens it.
static void mf_ensure_frame(void) {
    if (!g_frame_active) mf_frame_begin();
}

// [app-surface render target, step 1] Emit a BLT_OP_SET_TARGET iff `d`'s
// target differs from the emitter's current one (g_cur_target), so back-to-
// back ops on the same buffer don't each redundantly re-select it. Shared by
// mf_clear and mf_draw so BOTH ops are target-aware -- the real per-frame
// order (Task 1) clears/draws WORK *after* the scene has already rendered
// into the app surface (SET_TARGET APPSURF), so a clear()-only target switch
// is just as load-bearing as draw()'s.
static void mf_select_target(uint32_t fbo) {
    bool is_appsurf = (g_appSurfFbo != 0) && (fbo == g_appSurfFbo);
    int want = is_appsurf ? MF_TARGET_APPSURF : MF_TARGET_WORK;
    if (want != g_cur_target) { blt_set_target(&g_e, want); g_cur_target = want; }
}

static void mf_clear(RSurface *d, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mf_ensure_frame();
    mf_select_target(d->fbo);
    (void)a;   // fabric FILL writes opaque RGB565; no alpha channel on the wire
    int w = d->w < BLT_FB_WIDTH  ? d->w : BLT_FB_WIDTH;
    int h = d->h < BLT_FB_HEIGHT ? d->h : BLT_FB_HEIGHT;
    blt_fill(&g_e, 0, 0, w, h, mf_rgb565(r, g, b));
}

static bool evict_one_lru(void) {
    // Pin-this-frame invariant: an entry with .lru > g_lru_frame_floor was
    // hit or staged during the CURRENT frame, so a blt_trilist command
    // already emitted this frame may reference its heap offset — that
    // command isn't consumed until blt_execute runs at frame end. Freeing
    // such an entry now would let a later stage_texture's retry reuse the
    // same offset with different pixels, so the earlier (already-emitted)
    // draw would sample the wrong texture when the frame finally executes.
    // Only entries from a PRIOR frame (.lru <= floor) are eviction-eligible.
    int victim = -1; uint64_t best = ~0ull;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (g_texcache[i].used && g_texcache[i].lru <= g_lru_frame_floor && g_texcache[i].lru < best) {
            best = g_texcache[i].lru; victim = i;
        }
    }
    if (victim < 0) return false;   // nothing unpinned to evict
    blt_emitter_free(&g_e, g_texcache[victim].ref.off, g_texcache[victim].ref.size);
    g_texcache[victim].used = false;
    return true;
}

// Stage a texture page keyed by identity. Cache hit reuses the resident surface
// (no re-upload); miss converts RGBA->RGB565 into g_texscratch and blt_uploads
// once. Returns a ref with .valid==0 if the page cannot fit even after eviction
// (caller drops the draw). *out_has_key := did any texel fold to the colorkey.
// [Task 9 bring-up diagnostic] GMLOADER_MFGPU_HEAPLOG=1: log every real texture
// upload (miss) with its size and the resulting heap occupancy, every stage
// failure with the full pinned-working-set breakdown, and a per-frame summary
// of exactly which textures were touched (pinned) this frame -- to measure the
// STEADY-STATE per-frame texture working set against the device's ~14.75MB
// SRC heap (MF_DEV_SRC_CAP), since the host's 32MB MF_TEX_HEAP masked this
// capacity gap in every host-side parity test. Zero cost when unset.
static int mf_heaplog_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFGPU_HEAPLOG"); v = (e && *e) ? 1 : 0; }
    return v;
}
// Sum bytes + count of g_texcache entries "pinned" this frame (lru > floor --
// see g_lru_frame_floor's comment): the actual working set blt_execute must
// hold resident SIMULTANEOUSLY to render the frame, not just cumulative
// uploads. Also prints each entry when `verbose`.
static void mf_heaplog_frame_set(const char *tag, int verbose) {
    if (!mf_heaplog_on()) return;
    uint32_t total = 0; int n = 0;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (!g_texcache[i].used || g_texcache[i].lru <= g_lru_frame_floor) continue;
        total += g_texcache[i].ref.size; n++;
        if (verbose)
            fprintf(stderr, "HEAPLOG   pinned key=%u %ux%u bytes=%u off=%u\n",
                    g_texcache[i].key, g_texcache[i].ref.w, g_texcache[i].ref.h,
                    g_texcache[i].ref.size, g_texcache[i].ref.off);
    }
    fprintf(stderr, "HEAPLOG %s: %d distinct textures pinned this frame, %u bytes "
            "(%.2fMB) | heap_used=%u/%u (%.2fMB/%.2fMB)\n",
            tag, n, total, total / (1024.0*1024.0),
            blt_alloc_used(&g_e.alloc), g_e.alloc.size,
            blt_alloc_used(&g_e.alloc) / (1024.0*1024.0), g_e.alloc.size / (1024.0*1024.0));
}

static blt_surface_ref_t stage_texture(uint32_t key, const RTexture *t, bool *out_has_key) {
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == key) {
            g_texcache[i].lru = ++g_lru_clock;
            *out_has_key = g_texcache[i].has_key;
            return g_texcache[i].ref;
        }
    // miss: convert into scratch
    int tw, th; bool textured = (t && t->valid && t->rgba);
    if (textured) { tw = t->w; th = t->h; } else { tw = 1; th = 1; }
    if (tw <= 0 || th <= 0 || (size_t)tw * th > MF_TEX_TEXELS) {
        blt_surface_ref_t bad; bad.valid = 0; *out_has_key = false; return bad;
    }
    bool has_key = false;
    if (textured) {
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                g_texscratch[(size_t)y * tw + x] = mf_texel565(t, x, y, &has_key);
    } else {
        g_texscratch[0] = 0xFFFF;   // 1x1 opaque white
    }
    bool ov_before = g_e.overflow;                    // preserve any overflow already set this frame
    blt_surface_ref_t ref = blt_upload(&g_e, g_texscratch, tw, th, tw * 2);
    int evicted = 0;
    while (!ref.valid && evict_one_lru()) { evicted++; ref = blt_upload(&g_e, g_texscratch, tw, th, tw * 2); }
    if (!ref.valid) {
        *out_has_key = false;
        if (mf_heaplog_on()) {
            fprintf(stderr, "HEAPLOG STAGE FAIL key=%u want=%dx%d(%zu bytes) evicted=%d "
                    "heap_used=%u/%u\n", key, tw, th, (size_t)tw*th*2, evicted,
                    blt_alloc_used(&g_e.alloc), g_e.alloc.size);
            mf_heaplog_frame_set("at-fail", /*verbose=*/1);
        }
        return ref;
    }  // last blt_upload left overflow set -> frame drops, correct
    g_e.overflow = ov_before;                         // our transient failed-then-succeeded uploads did NOT overflow the frame
    g_upload_count++;
    if (mf_heaplog_on())
        fprintf(stderr, "HEAPLOG upload key=%u %dx%d bytes=%u off=%u evicted=%d heap_used=%u/%u\n",
                key, tw, th, ref.size, ref.off, evicted, blt_alloc_used(&g_e.alloc), g_e.alloc.size);
    // Fabric-offload Task 3: stage this freshly-uploaded page DDR3->SDRAM at the
    // SAME offset (SDRAM[off] = DDR[off]) so the fabric's TRILIST texel fetch —
    // which samples SDRAM unconditionally at src_off == ref.off — resolves. Emitted
    // unconditionally (host + device): the refmodel skips OP_STAGE as a no-op (see
    // case_stage_noop), so the host oracle stays ±1 LSB. Same-offset by design, NOT
    // blt_stage_surface (its decoupled sdram_off != src_off would render black on HW).
    // Tied to the upload (miss only) => each resident page is staged exactly once;
    // cache hits reuse the SDRAM-resident copy with no re-STAGE.
    if (blt_stage(&g_e, ref.off, (uint32_t)ref.stride * ref.h) != 0)
        fprintf(stderr, "backend_mfgpu: blt_stage overflow (off=%u) - draw may drop\n", ref.off);
    else
        g_stage_count++;
    // insert into a free slot, evicting the LRU slot if the table is full
    int slot = -1; uint64_t best = ~0ull;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (!g_texcache[i].used) { slot = i; break; }
        if (g_texcache[i].lru < best) { best = g_texcache[i].lru; slot = i; }
    }
    if (g_texcache[slot].used)
        blt_emitter_free(&g_e, g_texcache[slot].ref.off, g_texcache[slot].ref.size);
    g_texcache[slot] = MfTexEntry{ key, true, has_key, ref, ++g_lru_clock };
    *out_has_key = has_key;
    return ref;
}

static void mf_draw(RSurface *d, const BVtx *v, int triCount,
                    const RTexture *t, RBlend bl, float ar, uint32_t tex_key) {
    mf_ensure_frame();
    // [app-surface render target, step 1] d->fbo identifies the target FBO
    // (blitter.cpp's get_render_target); tex_key identifies the sampled GL
    // texture. Either can alias the detected application surface.
    bool dst_is_appsurf = (g_appSurfFbo != 0) && (d->fbo == g_appSurfFbo);
    bool src_is_appsurf = (g_appSurfTex != 0) && (tex_key == g_appSurfTex);

    // Self-referential draw (render INTO the app surface while ALSO sampling
    // it) should be structurally impossible for any well-formed GameMaker
    // draw stream: Task 4's detection only fires for a g_curFBO==0 draw, so
    // g_appSurfFbo is always a real nonzero FBO id and that same detecting/
    // sampling draw always has d->fbo==0 -- the two conditions can't both be
    // true without a genuine GL feedback loop (spec-level UB no engine relies
    // on). Guarded anyway (reviewer's Task 5 follow-up): the RTL's shared
    // surf_rd port starves silently if comp_target==APPSURF and SRC_SURFACE
    // are both active, so if this invariant is ever violated, fail loud here
    // rather than let it surface as unexplained visual corruption on device.
    if (dst_is_appsurf && src_is_appsurf) {
        fprintf(stderr, "backend_mfgpu: self-referential APPSURF draw (dst==src) - unsupported, dropped\n");
        return;
    }

    // Any OTHER (non-default, non-appsurf) render-to-texture target: the
    // fabric has one scanout FB plus the one app-surface BRAM, so effect
    // surfaces beyond the application surface stay on the SW rasterizer
    // (step-1 scope; step 2 adds N surfaces).
    if (g_defRGBA && d->rgba != g_defRGBA && !dst_is_appsurf) {
        backend_sw.draw(d, v, triCount, t, bl, ar, tex_key); return;
    }
    // Premultiplied-alpha source-over (dst = src + dst*(1-a)) has no exact fabric
    // blend; keep it on SW for bring-up (see raster_backend_convert.h).
    if (bl == RB_PREMULT)        { backend_sw.draw(d, v, triCount, t, bl, ar, tex_key); return; }
    if (triCount <= 0) return;

    mf_select_target(d->fbo);

    // ── source: the app surface itself (no staging), or a normal SDRAM page ──
    blt_surface_ref_t tex;
    int tw, th;
    bool has_key = false;
    if (src_is_appsurf) {
        // BLT_F_SRC_SURFACE ignores src_off/src_stride/src_x/src_y (Task 2/3
        // contract) -- no blt_upload/blt_stage for the surface, it's already
        // fabric-resident (rendered there by an earlier dst_is_appsurf draw
        // this frame, or a prior one). tex stays zeroed/invalid; Task 2's
        // relaxed blt_trilist guard allows that when the flag is set.
        memset(&tex, 0, sizeof tex);
        // UV scale: t->w/t->h are the ORIGINAL GL texture's real dimensions
        // (get_rtexture() in blitter.cpp), which may be a padded page (e.g.
        // a 2048x2048 POT atlas -- Task 1's boot trace) larger than the
        // <=320x240 used region. bvtx_to_blt(v, tw, th) = clamp(uv,0,1)*tw*16
        // is an ABSOLUTE PIXEL coordinate within that tw x th space; by the
        // (assumed) top-left-aligned used-region convention, that absolute
        // pixel coordinate is identical to the app-surface's own absolute
        // pixel addressing (both start at (0,0)), so reusing t->w/t->h here
        // -- NOT BLT_FB_WIDTH/HEIGHT -- reproduces the same coordinate the SW
        // rasterizer would sample from the real bound texture. UNVERIFIED
        // against a real device UV capture (Task 1's frame-graph dump didn't
        // log UV) -- flag for Task 9 bring-up if the surface samples offset.
        tw = t ? t->w : BLT_FB_WIDTH;
        th = t ? t->h : BLT_FB_HEIGHT;
        if (tw <= 0) tw = BLT_FB_WIDTH;
        if (th <= 0) th = BLT_FB_HEIGHT;
    } else {
        // [Task 9 bring-up diagnostic] Log the UV bbox this draw ACTUALLY
        // touches within the full page, vs the full page's own dims -- to
        // measure how much of a large sprite-sheet a typical draw uses (sub-
        // region staging feasibility evidence). Cheap: only when heaplog is on.
        if (mf_heaplog_on() && t && t->valid && t->w > 0 && t->h > 0) {
            float umin=1e9f,umax=-1e9f,vmin=1e9f,vmax=-1e9f;
            double sum_tri_px_area = 0;   // sum of PER-TRIANGLE bboxes (texels)
            for (int i = 0; i < triCount*3; i++) {
                if (v[i].u<umin) umin=v[i].u; if (v[i].u>umax) umax=v[i].u;
                if (v[i].v<vmin) vmin=v[i].v; if (v[i].v>vmax) vmax=v[i].v;
            }
            for (int q = 0; q < triCount; q++) {
                float tu0=1e9f,tu1=-1e9f,tv0=1e9f,tv1=-1e9f;
                for (int k = 0; k < 3; k++) {
                    const BVtx &vv = v[q*3+k];
                    if (vv.u<tu0) tu0=vv.u; if (vv.u>tu1) tu1=vv.u;
                    if (vv.v<tv0) tv0=vv.v; if (vv.v>tv1) tv1=vv.v;
                }
                sum_tri_px_area += (double)(tu1-tu0)*t->w * (double)(tv1-tv0)*t->h;
            }
            int pxw = (int)((umax-umin) * t->w + 0.5f), pxh = (int)((vmax-vmin) * t->h + 0.5f);
            fprintf(stderr, "HEAPLOG uvbbox key=%u page=%dx%d tris=%d touched=%dx%d(%d bytes) "
                    "of %u bytes (%.1f%%) sum_tri_px_area=%.0f (%.1f%% of page, %.1f%% of bbox)\n",
                    tex_key, t->w, t->h, triCount, pxw, pxh, pxw*pxh*2, (unsigned)t->w*t->h*2,
                    100.0*(pxw*pxh*2)/((double)t->w*t->h*2), sum_tri_px_area,
                    100.0*sum_tri_px_area/((double)t->w*t->h),
                    pxw*pxh>0 ? 100.0*sum_tri_px_area/((double)pxw*pxh) : 0.0);
        }
        // ── stage the texture page as RGB565 ──────────────────────────────
        // Key 0 is reserved for the untextured 1x1 opaque-white page; a real GL
        // texture name is never 0 (0 means "no texture bound"), so no collision.
        uint32_t stage_key = (t && t->valid && t->rgba) ? tex_key : 0u;   // all untextured share key 0
        tex = stage_texture(stage_key, t, &has_key);
        if (!tex.valid) {
            fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
            return;
        }
        tw = tex.w; th = tex.h;   // staged page dims (1x1 for untextured)
    }

    // ── convert + push the vertices ───────────────────────────────────────────
    int nverts = triCount * 3;
    if (nverts > MF_MAX_VERTS) {
        fprintf(stderr, "backend_mfgpu: %d tris exceed vertex scratch (%d verts) - draw dropped\n",
                triCount, MF_MAX_VERTS);
        return;
    }
    float min_vtx_a = 1.0f;
    for (int i = 0; i < nverts; i++) {
        g_vtxscratch[i] = bvtx_to_blt(&v[i], tw, th);
        if (v[i].a < min_vtx_a) min_vtx_a = v[i].a;
    }
    uint32_t eoff = blt_push_tris(&g_e, g_vtxscratch, triCount);
    if (eoff == 0xFFFFFFFFu) {
        fprintf(stderr, "backend_mfgpu: vertex push overflow (%d tris) - draw dropped\n", triCount);
        return;
    }

    // ── emit the TRILIST header (header alpha 255: CONST_ALPHA rides vtx.a) ────
    (void)ar;   // the fabric tri path has no alpha-test; GM's threshold is ~0
    //
    // Colorkey-aware blend selection (Task 6): a texture that staged any texel
    // as MF_COLORKEY (has_key) is a hard-edged (alpha in {0,255}) cutout — if
    // the draw is also fully opaque at the vertex level (min_vtx_a*255 >= 254),
    // BLT_BLEND_COLORKEY reproduces the SW oracle's per-texel-alpha cutout
    // exactly (mod 565 rounding). Otherwise fall back to the Task 5 path.
    // has_key is always false for a surface-sampled draw (no texel staging
    // happens for it at all), so it never takes the colorkey path.
    //
    // KNOWN LIMITATION: a *faded* cutout sprite (keyed texture drawn with
    // vtx.a < 254, e.g. a fade-out) cannot combine colorkey + const-alpha in a
    // single fabric TRILIST pass (blt_tri.c's blend switch is one case, not a
    // pipeline) — it falls back to CONST_ALPHA below, which composites the key
    // colour's pixels too (no cutout) while fading. Soft/faded transparency
    // needs real per-texel alpha in the fabric raster path; that's a future RTL
    // item, out of scope here.
    uint8_t blend_mode;
    uint16_t colorkey;
    if (has_key && min_vtx_a * 255.0f >= 254.0f) {
        blend_mode = BLT_BLEND_COLORKEY;
        colorkey = MF_COLORKEY;
    } else {
        blend_mode = rblend_to_blt(bl);
        colorkey = 0;
    }
    uint8_t extra_flags = src_is_appsurf ? BLT_F_SRC_SURFACE : 0;
    if (blt_trilist(&g_e, tex, blend_mode, colorkey, /*alpha=*/255,
                    eoff, triCount, extra_flags) != 0)
        fprintf(stderr, "backend_mfgpu: blt_trilist emit failed (%d tris) - draw dropped\n", triCount);
}

static void mf_frame_end(void);   // forward decl: defined below, called from mf_present

// Task 7: production entry point for present() — the frame loop calls
// clear()/draw() then present() (never frame_end() directly). Execute the
// accumulated ring into g_fb565 (mf_frame_end already does the blt_execute)
// and close the frame so the next clear/draw starts fresh. If no clear/draw
// happened this "frame" (g_frame_active false), there is nothing to execute.
static void mf_present(const RSurface *) {
    if (g_frame_active) {
        mf_frame_end();
        g_frame_active = false;
    }
}

static void mf_frame_end(void) {
    // [Task 9 bring-up diagnostic] Log BEFORE blt_end_frame touches anything --
    // g_lru_frame_floor (set in mf_frame_begin) still delimits exactly this
    // frame's pinned working set at this point.
    mf_heaplog_frame_set(g_e.overflow ? "frame-end(OVERFLOWED)" : "frame-end(ok)", /*verbose=*/1);
    blt_end_frame(&g_e);
#ifdef MISTER_NATIVE_VIDEO
    // Device: the ring + heap are already resident in the mmap'd DDR (the emitter
    // was bound to g_dev_ring/g_dev_src). Publish the control block, ring the
    // doorbell, and poll C_DONE — the fabric composites into on-chip BRAM and scans
    // itself out. No blt_execute, no g_fb565 on the hot path.
    if (g_e.overflow) {
        fprintf(stderr, "backend_mfgpu: emitter overflow this frame - frame dropped\n");
        return;
    }
    if (g_dev_ok) mf_device_submit();
    else          fprintf(stderr, "backend_mfgpu: device DDR unmapped - frame dropped\n");
#else
    // Host oracle: software-execute the ring into g_fb565 (parity tests read it back).
    memset(g_fb565, 0, sizeof g_fb565);
    if (g_e.overflow) {
        fprintf(stderr, "backend_mfgpu: emitter overflow this frame - frame dropped\n");
        return;   // nothing safe to execute this frame
    }
    int n = g_e.cmd_count;
    if (n > MF_MAX_CMDS) n = MF_MAX_CMDS;
    for (int i = 0; i < n; i++)
        blt_unpack_cmd(g_ring + (size_t)i * BLT_CMD_BYTES, &g_cmds[i]);
    // The whole g_srcdram is the source region: vertices at low offsets, texture
    // pages above MF_VTX_REGION. blt_execute only reads (and clamps OOB), so
    // passing the full size keeps both offset ranges in-bounds.
    blt_surface_heap_t heap = { g_srcdram, sizeof g_srcdram, nullptr, nullptr };
    blt_execute(g_fb565, &heap, g_cmds, n);
#endif
}

// Task 7 device wiring: the fabric framebuffer, for main.cpp to hand straight
// to NativeVideoWriter_WriteFrame after present() (g_fb565 is already RGB565 —
// no Blitter_ToRGB565 conversion needed). w/h are BLT_FB_WIDTH x BLT_FB_HEIGHT
// (320x240, refmodel/blitter_ref.h), the fixed fabric scanout geometry — same
// as MISTER_WIDTH/MISTER_HEIGHT today, so callers can use either pair, but the
// row stride must always be computed from *this* w (BLT_FB_WIDTH), not assumed.
extern "C" const uint16_t *RasterBackend_MFGPU_GetFB565(int *w, int *h) {
    if (w) *w = BLT_FB_WIDTH;
    if (h) *h = BLT_FB_HEIGHT;
    return g_fb565;
}

// ---- Host validation only — NOT part of the RasterBackend vtable ----------
// Copies a tightly-packed w x h (<= BLT_FB_WIDTH x BLT_FB_HEIGHT) region of the
// last blt_execute'd RGB565 target out for the host parity tests to diff against
// backend_sw's output. No device code path calls this.
extern "C" void RasterBackend_MFGPU_TestCopyFB565(int w, int h, uint16_t *out) {
    if (w > BLT_FB_WIDTH)  w = BLT_FB_WIDTH;
    if (h > BLT_FB_HEIGHT) h = BLT_FB_HEIGHT;
    for (int y = 0; y < h; y++)
        memcpy(out + (size_t)y * w, g_fb565 + (size_t)y * BLT_FB_WIDTH,
               (size_t)w * sizeof(uint16_t));
}

// Host/Task-6 hook: tell the backend which surface pixels are the default fb, so
// draw() can route FBO / render-to-texture targets to the SW fallback. Pass the
// default RSurface's rgba pointer (NULL restores "everything is default fb").
extern "C" void RasterBackend_MFGPU_SetDefaultSurface(const uint8_t *rgba) {
    g_defRGBA = rgba;
}

// Task 2 host hooks: count real blt_uploads (cache-hit-vs-miss proof) and force
// a clean re-init with an optional capped texture-heap size (0 = full MF_TEX_HEAP;
// nonzero lets the Task 4 eviction test shrink the allocator on purpose).
extern "C" uint32_t RasterBackend_MFGPU_TestUploadCount(void) { return g_upload_count; }
// FO Task 3 host hook: BLT_OP_STAGE emits since reinit (proves stage-once-per-page).
extern "C" uint32_t RasterBackend_MFGPU_TestStageCount(void) { return g_stage_count; }
extern "C" void RasterBackend_MFGPU_TestReinit(uint32_t tex_heap_bytes) {
    g_inited = false; g_frame_active = false;
    g_tex_heap_cap = tex_heap_bytes;   // 0 => full heap
    mf_init_once();                    // re-wires emitter, clears cache + counter
    g_lru_frame_floor = 0;             // start clean: nothing pinned before frame 1
}

// Free the cached entry for GL texture `id` so the next draw re-stages it. Called
// by blitter.cpp on TexImage2D re-upload and DeleteTexture — exactly when the SW
// oracle's CPU pixels change, so the cache never diverges from SW.
extern "C" void RasterBackend_MFGPU_InvalidateTex(uint32_t id) {
    if (!g_inited) return;
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == id) {
            blt_emitter_free(&g_e, g_texcache[i].ref.off, g_texcache[i].ref.size);
            g_texcache[i].used = false;
        }
}

extern "C" const RasterBackend backend_mfgpu = {
    "mfgpu", mf_frame_begin, mf_clear, mf_draw, mf_present, mf_frame_end,
};
