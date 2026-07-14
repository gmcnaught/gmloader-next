// backend_mfgpu: FPGA-fabric back-end SKELETON (Task 4). Owns the emitter +
// frame lifecycle over host-side ring/heap/vtx buffers (device wiring to the
// real DDR ring is Task 6) and is validated on the host by executing the
// emitted ring through the SAME blt_execute software model the mfgpu
// refmodel unit tests use (3rdparty/mfgpu/host/test_emitter.c).
//
//   clear   : emits a REAL fabric BLT_OP_FILL via blt_fill() (not a direct
//             pixel write) so the clear-parity test in raster_backend_test.cpp
//             exercises the same emit -> blt_execute path Task 5's triangle
//             emit reuses. BLT_OP_FILL exists in the refmodel (blitter_ref.h),
//             so this follows the brief's "real fill op" path, not the
//             direct-RGB565-write fallback.
//   draw    : delegates verbatim to backend_sw (fabric TRILIST emit is Task 5).
//   present : no-op placeholder (fabric scanout is hardware; Task 6).
//
// The fabric framebuffer geometry is FIXED at BLT_FB_WIDTH x BLT_FB_HEIGHT
// (320x240, refmodel/blitter_ref.h) — that is the wire contract, not a
// per-draw parameter. clear() targets are clamped to that size; the engine's
// render surface is comfortably inside it.
#include "raster_backend.h"
extern "C" {
#include "blt_emitter.h"
#include "blt_wire.h"
#include "blitter_ref.h"
}
#include <string.h>

extern "C" const RasterBackend backend_sw;   /* draw fallback (Task 5 replaces) */

// One frame's worth of host-side emitter buffers, sized like
// test_emitter.c's RING_CAP/HEAP_CAP. Static/file-scope is fine for this
// single-threaded skeleton; Task 6 points these at the real DDR ring/heap.
enum {
    MF_RING_CAP = 64 * 1024,
    MF_HEAP_CAP = 1 * 1024 * 1024,
    MF_VTX_CAP  = 256 * 1024,
    MF_MAX_CMDS = MF_RING_CAP / BLT_CMD_BYTES,
};
static uint8_t       g_ring[MF_RING_CAP];
static uint8_t       g_heap[MF_HEAP_CAP];
static uint8_t       g_vtxbuf[MF_VTX_CAP];
static blt_emitter_t g_e;
static blt_cmd_t     g_cmds[MF_MAX_CMDS];
// Host execute target: the fixed-size fabric framebuffer that blt_execute
// composites into. On real hardware this is scanned out directly; on the
// host it is the oracle the clear-parity test reads back via
// RasterBackend_MFGPU_TestCopyFB565() below.
static uint16_t      g_fb565[BLT_FB_PIXELS];

static inline uint16_t mf_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void mf_frame_begin(void) {
    blt_emitter_init(&g_e, g_ring, sizeof g_ring, g_heap, sizeof g_heap);
    blt_vtx_buf_init(&g_e, g_vtxbuf, sizeof g_vtxbuf);
    blt_begin_frame(&g_e, /*target_buf=*/0, /*clear=*/0, /*clear_color=*/0);
}

static void mf_clear(RSurface *d, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    (void)a;   // fabric FILL writes opaque RGB565; no alpha channel on the wire
    int w = d->w < BLT_FB_WIDTH  ? d->w : BLT_FB_WIDTH;
    int h = d->h < BLT_FB_HEIGHT ? d->h : BLT_FB_HEIGHT;
    blt_fill(&g_e, 0, 0, w, h, mf_rgb565(r, g, b));
}

static void mf_draw(RSurface *d, const BVtx *v, int n, const RTexture *t, RBlend bl, float ar) {
    backend_sw.draw(d, v, n, t, bl, ar);   // Task 5 replaces with fabric TRILIST emit
}

static void mf_present(const RSurface *) { /* Task 6: device scanout */ }

static void mf_frame_end(void) {
    blt_end_frame(&g_e);
    memset(g_fb565, 0, sizeof g_fb565);
    if (g_e.overflow) return;   // nothing safe to execute this frame
    int n = g_e.cmd_count;
    if (n > MF_MAX_CMDS) n = MF_MAX_CMDS;
    for (int i = 0; i < n; i++)
        blt_unpack_cmd(g_ring + (size_t)i * BLT_CMD_BYTES, &g_cmds[i]);
    blt_surface_heap_t heap = { g_heap, g_e.heap_used, nullptr, nullptr };
    blt_execute(g_fb565, &heap, g_cmds, n);
}

// ---- Host validation only — NOT part of the RasterBackend vtable ----------
// Copies a tightly-packed w x h (<= BLT_FB_WIDTH x BLT_FB_HEIGHT) region of
// the last blt_execute'd RGB565 target out for the host clear-parity test to
// diff against backend_sw's output. No device code path calls this.
extern "C" void RasterBackend_MFGPU_TestCopyFB565(int w, int h, uint16_t *out) {
    if (w > BLT_FB_WIDTH)  w = BLT_FB_WIDTH;
    if (h > BLT_FB_HEIGHT) h = BLT_FB_HEIGHT;
    for (int y = 0; y < h; y++)
        memcpy(out + (size_t)y * w, g_fb565 + (size_t)y * BLT_FB_WIDTH,
               (size_t)w * sizeof(uint16_t));
}

extern "C" const RasterBackend backend_mfgpu = {
    "mfgpu", mf_frame_begin, mf_clear, mf_draw, mf_present, mf_frame_end,
};
