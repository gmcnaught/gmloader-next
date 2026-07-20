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
    // resident-page table size. Sub-region residency (per-sprite-quad staging)
    // caches by (tex_id, cropped rect) instead of one entry per whole page, so a
    // batched spritesheet draw can hold many small sub-rects at once -- 256 slots
    // covers a frame's distinct sprite-quad regions with headroom (spec tuning
    // default). Every entry is a real heap page, but each sub-rect is a few KB.
    MF_TEX_CACHE_N = 256,                         // resident-page table size
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
// Frames opened since init. Only consumed by the GMLOADER_MFGPU_UVLOG capture
// (to bound it to the first few frames and to tag its lines).
static int           g_frame_no = 0;

// Persistent resident-texture cache. Keyed by GL texture id (tex_key). Survives
// across frames (mf_frame_begin no longer resets the heap); entries are freed on
// GL invalidate/delete (Task 3) or LRU eviction under heap pressure (Task 4).
// A resident source page. Sub-region residency keys each entry by
// (key, rx,ry,rw,rh): `key` is the GL texture id and (rx,ry,rw,rh) is the cropped
// texel rect of `key` that this page holds (a whole-page entry is rx=ry=0,
// rw=full-w, rh=full-h). Two draws of the same texture that sample different
// sub-rects get distinct entries; invalidate frees every entry with a matching
// key regardless of rect.
struct MfTexEntry { uint32_t key; bool used; bool has_key; blt_surface_ref_t ref; uint64_t lru;
                    uint16_t rx, ry, rw, rh; };
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

static inline int   mf_clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float mf_clamp01(float x)              { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

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
    g_frame_no++;
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

// Shared tail for stage_texture / stage_texture_region: g_texscratch already
// holds `w*h` RGB565 texels for the page keyed (key, rx,ry,w,h). Upload it
// (evicting prior-frame pages under heap pressure), STAGE it DDR3->SDRAM, and
// insert a cache entry. Returns the ref (.valid==0 if it couldn't fit even after
// eviction -- the last failed blt_upload leaves g_e.overflow set so the frame
// drops, which is correct). `what` tags the HEAPLOG lines.
static blt_surface_ref_t mf_upload_and_cache(uint32_t key, int w, int h,
                                             int rx, int ry, bool has_key,
                                             const char *what) {
    bool ov_before = g_e.overflow;                    // preserve any overflow already set this frame
    blt_surface_ref_t ref = blt_upload(&g_e, g_texscratch, w, h, w * 2);
    int evicted = 0;
    while (!ref.valid && evict_one_lru()) { evicted++; ref = blt_upload(&g_e, g_texscratch, w, h, w * 2); }
    if (!ref.valid) {
        if (mf_heaplog_on()) {
            fprintf(stderr, "HEAPLOG STAGE FAIL %s key=%u rect=%d,%d %dx%d(%zu bytes) evicted=%d "
                    "heap_used=%u/%u\n", what, key, rx, ry, w, h, (size_t)w*h*2, evicted,
                    blt_alloc_used(&g_e.alloc), g_e.alloc.size);
            mf_heaplog_frame_set("at-fail", /*verbose=*/1);
        }
        return ref;   // overflow left set by the last blt_upload -> frame drops, correct
    }
    g_e.overflow = ov_before;                         // our transient failed-then-succeeded uploads did NOT overflow the frame
    g_upload_count++;
    if (mf_heaplog_on())
        fprintf(stderr, "HEAPLOG upload %s key=%u rect=%d,%d %dx%d bytes=%u off=%u evicted=%d heap_used=%u/%u\n",
                what, key, rx, ry, w, h, ref.size, ref.off, evicted, blt_alloc_used(&g_e.alloc), g_e.alloc.size);
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
    // insert into a free slot, else the LRU slot -- but NEVER a frame-pinned one.
    int slot = -1; uint64_t best = ~0ull;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (!g_texcache[i].used) { slot = i; break; }
        if (g_texcache[i].lru < best) { best = g_texcache[i].lru; slot = i; }
    }
    // Pin-aware insert: when the table is full and even the global-LRU victim was
    // touched THIS frame (lru > floor), its heap offset is referenced by a
    // trilist already emitted this frame and not yet executed -- freeing+reusing
    // it would alias that draw onto the new texture (the exact hazard evict_one_lru
    // guards on the heap-pressure path). There is no safe slot, so drop the frame
    // (mirror the heap-overflow graceful path): undo this upload, set overflow, and
    // fail the stage so the caller drops the draw. A busy frame degrades to a
    // DROPPED frame, never a wrong-pixel frame.
    if (g_texcache[slot].used && g_texcache[slot].lru > g_lru_frame_floor) {
        if (mf_heaplog_on())
            fprintf(stderr, "HEAPLOG CACHE-FULL %s key=%u: all %d slots frame-pinned "
                    "- dropping frame\n", what, key, MF_TEX_CACHE_N);
        blt_emitter_free(&g_e, ref.off, ref.size);   // the STAGE cmd dies with the dropped ring
        g_e.overflow = true;
        blt_surface_ref_t bad; bad.valid = 0; return bad;
    }
    if (g_texcache[slot].used)
        blt_emitter_free(&g_e, g_texcache[slot].ref.off, g_texcache[slot].ref.size);
    g_texcache[slot] = MfTexEntry{ key, true, has_key, ref, ++g_lru_clock,
                                   (uint16_t)rx, (uint16_t)ry, (uint16_t)w, (uint16_t)h };
    return ref;
}

static blt_surface_ref_t stage_texture(uint32_t key, const RTexture *t, bool *out_has_key) {
    // Whole-page entry: rect (0,0,tw,th). Untextured => 1x1 opaque-white page.
    int tw, th; bool textured = (t && t->valid && t->rgba);
    if (textured) { tw = t->w; th = t->h; } else { tw = 1; th = 1; }
    if (tw <= 0 || th <= 0 || (size_t)tw * th > MF_TEX_TEXELS) {
        blt_surface_ref_t bad; bad.valid = 0; *out_has_key = false; return bad;
    }
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == key &&
            g_texcache[i].rx == 0 && g_texcache[i].ry == 0 &&
            g_texcache[i].rw == tw && g_texcache[i].rh == th) {
            g_texcache[i].lru = ++g_lru_clock;
            *out_has_key = g_texcache[i].has_key;
            return g_texcache[i].ref;
        }
    bool has_key = false;
    if (textured) {
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                g_texscratch[(size_t)y * tw + x] = mf_texel565(t, x, y, &has_key);
    } else {
        g_texscratch[0] = 0xFFFF;   // 1x1 opaque white
    }
    blt_surface_ref_t ref = mf_upload_and_cache(key, tw, th, 0, 0, has_key, "whole");
    *out_has_key = ref.valid ? has_key : false;
    return ref;
}

// Sub-region residency: stage ONLY the tight UV sub-rect [u0,u1]x[v0,v1]
// (normalized) of texture `t` as its own small RGB565 page, keyed (key, rect).
// The rect is the texel bbox of the UVs expanded by a 1-texel margin and clamped
// to the page, so the fabric's +HALF-bias NEAREST sample can never address a
// texel just outside the cropped edge (see blt_tri.c). A cache hit reuses the
// resident sub-page (no re-upload). Returns the ref (ref.w/ref.h = cropped rw/rh)
// and, via out_rx/out_ry, the crop origin the caller needs to REBASE the quad's
// UVs into the cropped page. .valid==0 => could not fit even after eviction.
//
// UV-rebase bit-exactness: for any vertex UV `u`, the whole-page fabric texel
// coord is o.u = lround(clamp01(u)*t->w*16); rebasing to the cropped page gives
// o.u' = lround((clamp01(u)*t->w - rx)*16) = o.u - rx*16 (rx integer). So the
// cropped-page sample resolves the SAME physical texel (rx + o.u'/16 == o.u/16)
// -- byte-identical to the whole-page render, no ±1 slack introduced by cropping.
// The clamped, +1-texel-margin crop rect for UV bbox [u0,u1]x[v0,v1] of `t`. The
// margin guards the fabric's +HALF-bias NEAREST edge; clamping rx1/ry1 to the
// page (NOT page-1) keeps the max-edge texel w-1 inside the rect. Single-sourced
// so stage_texture_region and mf_draw's near-full-page decision agree exactly.
static void mf_crop_rect(const RTexture *t, float u0, float v0, float u1, float v1,
                         int *rx, int *ry, int *rw, int *rh) {
    int x0 = mf_clampi((int)floorf(u0 * t->w) - 1, 0, t->w - 1);
    int y0 = mf_clampi((int)floorf(v0 * t->h) - 1, 0, t->h - 1);
    int x1 = mf_clampi((int)ceilf (u1 * t->w) + 1, 1, t->w);
    int y1 = mf_clampi((int)ceilf (v1 * t->h) + 1, 1, t->h);
    *rx = x0; *ry = y0;
    *rw = (x1 - x0 < 1) ? 1 : x1 - x0;
    *rh = (y1 - y0 < 1) ? 1 : y1 - y0;
}

static blt_surface_ref_t stage_texture_region(uint32_t key, const RTexture *t,
                                              float u0, float v0, float u1, float v1,
                                              bool *out_has_key, int *out_rx, int *out_ry) {
    int rx, ry, rw, rh;
    mf_crop_rect(t, u0, v0, u1, v1, &rx, &ry, &rw, &rh);
    *out_rx = rx; *out_ry = ry;
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == key &&
            g_texcache[i].rx == rx && g_texcache[i].ry == ry &&
            g_texcache[i].rw == rw && g_texcache[i].rh == rh) {
            g_texcache[i].lru = ++g_lru_clock;
            *out_has_key = g_texcache[i].has_key;
            return g_texcache[i].ref;
        }
    if ((size_t)rw * rh > MF_TEX_TEXELS) {
        blt_surface_ref_t bad; bad.valid = 0; *out_has_key = false; return bad;
    }
    bool has_key = false;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++)
            g_texscratch[(size_t)y * rw + x] = mf_texel565(t, rx + x, ry + y, &has_key);
    blt_surface_ref_t ref = mf_upload_and_cache(key, rw, rh, rx, ry, has_key, "region");
    *out_has_key = ref.valid ? has_key : false;
    return ref;
}

// Emit one BLT_OP_TRILIST for `nt` triangles (`verts` = nt*3 vertices, whose UVs
// address the `tw`x`th` page `tex`). Converts + pushes the vertices, selects the
// colorkey-vs-blend mode (has_key + fully-opaque => BLT_BLEND_COLORKEY, exactly
// as the whole-page path did -- see the long note at mf_draw's blend site), and
// emits. Shared by the sub-region quad loop, the untextured single-page path, and
// the app-surface path (extra_flags carries BLT_F_SRC_SURFACE there). A hard emit
// failure is logged and drops just this group's triangles.
static void mf_emit_group(const blt_surface_ref_t &tex, int tw, int th,
                          const BVtx *verts, int nt, RBlend bl,
                          bool has_key, uint8_t extra_flags) {
    int nverts = nt * 3;
    if (nverts > MF_MAX_VERTS) {
        fprintf(stderr, "backend_mfgpu: %d tris exceed vertex scratch (%d verts) - draw dropped\n",
                nt, MF_MAX_VERTS);
        return;
    }
    float min_vtx_a = 1.0f;
    for (int i = 0; i < nverts; i++) {
        g_vtxscratch[i] = bvtx_to_blt(&verts[i], tw, th);
        if (verts[i].a < min_vtx_a) min_vtx_a = verts[i].a;
    }
    uint32_t eoff = blt_push_tris(&g_e, g_vtxscratch, nt);
    if (eoff == 0xFFFFFFFFu) {
        fprintf(stderr, "backend_mfgpu: vertex push overflow (%d tris) - draw dropped\n", nt);
        return;
    }
    uint8_t blend_mode;
    uint16_t colorkey;
    if (has_key && min_vtx_a * 255.0f >= 254.0f) {
        blend_mode = BLT_BLEND_COLORKEY;
        colorkey = MF_COLORKEY;
    } else {
        blend_mode = rblend_to_blt(bl);
        colorkey = 0;
    }
    if (blt_trilist(&g_e, tex, blend_mode, colorkey, /*alpha=*/255,
                    eoff, nt, extra_flags) != 0)
        fprintf(stderr, "backend_mfgpu: blt_trilist emit failed (%d tris) - draw dropped\n", nt);
}

// ── [Y-orientation bring-up capture] GMLOADER_MFGPU_UVLOG ────────────────────
// The app-surface composite branch below carries a self-flagged assumption:
// "UNVERIFIED against a real device UV capture (Task 1's frame-graph dump
// didn't log UV)". That unverified assumption is exactly what produced a wrong
// V-flip fix (ac41e1e, reverted in d0d5d27), so pin it with real data before
// touching orientation again.
//
// What decides where the flip belongs is the correspondence between SCREEN Y
// and texcoord V on the composite quad:
//   v at min-y (screen TOP) == 1  -> GM emits GL's bottom-origin FBO
//                                    convention; the fabric surface is
//                                    top-origin, so the composite is the
//                                    inversion site and must flip.
//   v at min-y (screen TOP) == 0  -> composite is already top-origin; the
//                                    inversion is NOT here (look at the RTL
//                                    scanout or the scene's own y).
// Also dumps page dims vs the 320x240 used region: if th > 240 the page is
// padded, and a normalized 1-v would be wrong even on the composite -- the
// flip has to be taken in ABSOLUTE PIXELS about the used height.
//
// APPSURF-scene draws are logged too (their screen-y range), so a flipped
// scene-into-FBO y is distinguishable from a flipped composite.
// Capped at the first few frames: one composite per frame, so a short capture
// is enough and the log stays readable over ssh.
static int mf_uvlog_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFGPU_UVLOG"); v = (e && *e) ? atoi(e) : 0;
                 if (e && *e && v <= 0) v = 3; }   // bare "=1"/non-numeric -> 3 frames
    return v;
}
static void mf_uvlog(const char *tag, const BVtx *v, int triCount, int tw, int th) {
    int nv = triCount * 3;
    if (nv <= 0) return;
    float ymin = v[0].y, ymax = v[0].y, v_at_ymin = v[0].v, v_at_ymax = v[0].v;
    float umin = v[0].u, umax = v[0].u, vmin = v[0].v, vmax = v[0].v;
    float xmin = v[0].x, xmax = v[0].x;
    for (int i = 1; i < nv; i++) {
        if (v[i].y < ymin) { ymin = v[i].y; v_at_ymin = v[i].v; }
        if (v[i].y > ymax) { ymax = v[i].y; v_at_ymax = v[i].v; }
        if (v[i].x < xmin) xmin = v[i].x;
        if (v[i].x > xmax) xmax = v[i].x;
        if (v[i].u < umin) umin = v[i].u;   if (v[i].u > umax) umax = v[i].u;
        if (v[i].v < vmin) vmin = v[i].v;   if (v[i].v > vmax) vmax = v[i].v;
    }
    // The verdict, stated outright so the capture needs no interpretation.
    const char *verdict = (v_at_ymin > v_at_ymax) ? "V-INVERTED(top-v>bot-v)"
                        : (v_at_ymin < v_at_ymax) ? "V-UPRIGHT(top-v<bot-v)"
                                                  : "V-DEGENERATE(equal)";
    fprintf(stderr, "UVLOG f=%d %s tris=%d page=%dx%d used=%dx%d padded=%s "
            "screen=[%.1f,%.1f..%.1f,%.1f] uv=[%.4f,%.4f..%.4f,%.4f] "
            "v@top=%.4f v@bot=%.4f %s\n",
            g_frame_no, tag, triCount, tw, th, BLT_FB_WIDTH, BLT_FB_HEIGHT,
            (tw > BLT_FB_WIDTH || th > BLT_FB_HEIGHT) ? "YES" : "no",
            xmin, ymin, xmax, ymax, umin, vmin, umax, vmax,
            v_at_ymin, v_at_ymax, verdict);
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
    (void)ar;   // the fabric tri path has no alpha-test; GM's threshold is ~0

    // ── source: the app surface itself (no staging) ───────────────────────────
    // One TRILIST over ALL the draw's tris: the surface is already fabric-
    // resident (rendered there by an earlier dst_is_appsurf draw), so there is
    // nothing to crop/stage. BLT_F_SRC_SURFACE ignores src_off/stride/x/y
    // (Task 2/3 contract); tex stays zeroed/invalid, which the relaxed
    // blt_trilist guard allows when the flag is set.
    if (src_is_appsurf) {
        blt_surface_ref_t tex; memset(&tex, 0, sizeof tex);
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
        int tw = t ? t->w : BLT_FB_WIDTH;
        int th = t ? t->h : BLT_FB_HEIGHT;
        if (tw <= 0) tw = BLT_FB_WIDTH;
        if (th <= 0) th = BLT_FB_HEIGHT;
        if (g_frame_no <= mf_uvlog_on()) mf_uvlog("COMPOSITE(appsurf->screen)", v, triCount, tw, th);
        // ── V-flip, composite ONLY (GL bottom-origin FBO -> top-origin surface)
        // GM composites the application surface with GL's FBO convention: v=0 is
        // the BOTTOM of the image. Device capture (GMLOADER_MFGPU_UVLOG, Maldita
        // title screen, 801/801 composite draws identical over 400 frames):
        //   page=512x256 content=288x216 screen=[0,0..320,240]
        //   uv=[0,0 .. 0.5625,0.8438]  v@top=0.8438  v@bot=0.0000
        // The fabric app surface is top-origin, so sampling that raw renders the
        // whole scene upside-down. This draw is the ONLY place the bottom-origin
        // convention reaches the fabric: per-sprite textures are top-origin in
        // memory (mf_texel565 reads row 0 first) and are bit-exact against the
        // SW rasterizer with raw UVs -- flipping THOSE is the reverted ac41e1e
        // bug, which relocated atlas samples instead of mirroring sprites. The
        // same capture confirms it: scene->appsurf draws are 533/533 V-UPRIGHT.
        //
        // Flip about the quad's OWN V extent, NOT a normalized 1-v. 0.8438 is
        // 216/256 -- the page is a PADDED POT (512x256) holding a 288x216
        // surface, so 1-v would map 0.8438 -> 0.1562 and 0 -> 1.0, straight into
        // the dead padding below the content. (vmin+vmax)-v maps vmin<->vmax
        // exactly, inverting the sampled band in place, and is self-calibrating:
        // it needs neither the page dims nor the used-region dims. Valid because
        // this quad by construction spans the whole surface -- blitter.cpp only
        // detects the app surface from a draw covering the full viewport.
        // Degenerate (vmin==vmax) collapses to identity, which is harmless.
        int nverts = triCount * 3;
        if (nverts > MF_MAX_VERTS) nverts = MF_MAX_VERTS;   // same cap mf_emit_group enforces
        float vmin = v[0].v, vmax = v[0].v;
        for (int i = 1; i < nverts; i++) {
            if (v[i].v < vmin) vmin = v[i].v;
            if (v[i].v > vmax) vmax = v[i].v;
        }
        const float vsum = vmin + vmax;
        static BVtx compscratch[MF_MAX_VERTS];
        for (int i = 0; i < nverts; i++) { compscratch[i] = v[i]; compscratch[i].v = vsum - v[i].v; }
        mf_emit_group(tex, tw, th, compscratch, triCount, bl, /*has_key=*/false, BLT_F_SRC_SURFACE);
        return;
    }

    // Scene draws INTO the app surface, for the same capture: if GM's
    // scene-into-FBO y were the flipped one (rather than the composite's v),
    // it would show up here as sprites whose screen-y is mirrored about the
    // surface height. A handful per frame is plenty to read the convention.
    if (g_frame_no <= mf_uvlog_on() && dst_is_appsurf && t) {
        static int log_frame = -1, log_count = 0;
        if (log_frame != g_frame_no) { log_frame = g_frame_no; log_count = 0; }
        if (log_count++ < 4)
            mf_uvlog("SCENE(->appsurf)", v, triCount, t->w > 0 ? t->w : 1, t->h > 0 ? t->h : 1);
    }

    // [Task 9 bring-up diagnostic] Log the UV bbox this draw ACTUALLY touches
    // within the full page, vs the full page's own dims -- to measure how much
    // of a large sprite-sheet a typical draw uses (sub-region staging feasibility
    // evidence). Cheap: only when heaplog is on.
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

    // ── untextured: one 1x1 opaque-white page, single TRILIST over all tris ────
    // Key 0 is reserved for that page; a real GL texture name is never 0, so no
    // collision. Cropping a 1x1 page is meaningless, so untextured fills stay on
    // the whole-page path.
    bool textured = (t && t->valid && t->rgba);
    if (!textured) {
        bool has_key = false;
        blt_surface_ref_t tex = stage_texture(0u, t, &has_key);
        if (!tex.valid) {
            fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
            return;
        }
        mf_emit_group(tex, tex.w, tex.h, v, triCount, bl, has_key, /*extra_flags=*/0);
        return;
    }

    // ── fallback: odd triangle count is not clean sprite-quads ────────────────
    // The sub-region path below assumes 2-tri (6-vertex) sprite-quads. A draw
    // whose tri count is odd isn't that shape (stray/fan geometry, rare and not
    // the batched-atlas hot path), so route the WHOLE draw through the intact
    // whole-page path -- bit-exact, and it shares one cache entry instead of
    // fragmenting into odd sub-rects.
    if (triCount % 2 != 0) {
        bool has_key = false;
        blt_surface_ref_t tex = stage_texture(tex_key, t, &has_key);
        if (!tex.valid) {
            fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
            return;
        }
        mf_emit_group(tex, tex.w, tex.h, v, triCount, bl, has_key, /*extra_flags=*/0);
        return;
    }

    // ── textured: per-sprite-quad sub-region staging ──────────────────────────
    // Split the draw into sprite-quads (2 tris / 6 verts), crop-stage only the
    // UV sub-rect each quad samples, rebase its UVs into that cropped page, and
    // emit one TRILIST per quad. This is the change that drops the per-frame
    // device working set from whole 2048^2 atlas pages to the ~1.5-3%-of-page the
    // sprites actually touch, bit-exact vs the old whole-page render (see
    // stage_texture_region's rebase-exactness note).
    //
    // Blend selection + the keyed-cutout / faded-sprite limitation live in
    // mf_emit_group (unchanged from the whole-page path): has_key is computed
    // per cropped region -- a region that includes a keyed texel reports
    // has_key, and a keyed texel a quad samples is always inside that quad's own
    // cropped rect, so the COLORKEY-vs-COPY choice is identical to whole-page.
    for (int q = 0; q < triCount / 2; q++) {
        const BVtx *gv = v + (size_t)q * 6;    // 6 verts = one sprite-quad
        // quad UV bbox (clamp01 to match the rebase + the fabric's clamped sampling)
        float u0 = 1.0f, u1 = 0.0f, v0 = 1.0f, v1 = 0.0f;
        for (int i = 0; i < 6; i++) {
            float uu = mf_clamp01(gv[i].u), vv = mf_clamp01(gv[i].v);
            if (uu < u0) u0 = uu; if (uu > u1) u1 = uu;
            if (vv < v0) v0 = vv; if (vv > v1) v1 = vv;
        }

        // Near-full-page fallback: if the cropped rect (+margin) already covers
        // >=90% of the page, cropping buys almost nothing and just fragments the
        // cache / re-stages ~the whole page every frame -- route this quad through
        // the shared whole-page entry instead (no rebase; UVs address the page).
        int rx, ry, rw, rh;
        mf_crop_rect(t, u0, v0, u1, v1, &rx, &ry, &rw, &rh);
        if ((double)rw * rh >= 0.9 * (double)t->w * t->h) {
            bool has_key = false;
            blt_surface_ref_t tex = stage_texture(tex_key, t, &has_key);
            if (!tex.valid) {
                fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
                return;
            }
            mf_emit_group(tex, tex.w, tex.h, gv, 2, bl, has_key, /*extra_flags=*/0);
            continue;
        }

        bool has_key = false; int srx = 0, sry = 0;
        blt_surface_ref_t tex = stage_texture_region(tex_key, t, u0, v0, u1, v1, &has_key, &srx, &sry);
        if (!tex.valid) {
            // The failed upload left g_e.overflow set -> the whole frame drops at
            // frame_end (correct: a texture that can't fit -- heap or cache-table
            // -- must not render half a command list). No point emitting the rest.
            fprintf(stderr, "backend_mfgpu: sub-region cannot fit heap after eviction - draw dropped\n");
            return;
        }
        // rebase this quad's UVs into the cropped page: cropped-page uv' =
        // (clamp01(uv)*page - rect_origin) / crop_dim. (srx/sry == rx/ry.)
        BVtx reb[6];
        for (int i = 0; i < 6; i++) {
            float u_abs = mf_clamp01(gv[i].u) * t->w, v_abs = mf_clamp01(gv[i].v) * t->h;
            reb[i] = gv[i];
            reb[i].u = (u_abs - srx) / (float)tex.w;
            reb[i].v = (v_abs - sry) / (float)tex.h;
        }
        mf_emit_group(tex, tex.w, tex.h, reb, 2, bl, has_key, /*extra_flags=*/0);
    }
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
