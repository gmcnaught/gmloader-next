// backend_sw: thin RasterBackend wrapper around today's software rasterizer
// (blitter_raster.cpp). Zero added logic, zero pixel change — every call is a
// direct pass-through to the same functions blitter.cpp called before this
// seam existed.
#include "raster_backend.h"

// Mirrors blitter.cpp's g_threads (GMLOADER_BLITTER_THREADS). Set once from
// Blitter_Init() via RasterBackend_SW_SetThreads() so backend_sw's rasterizer
// uses the exact same worker-thread count the direct Blitter_RasterDraw call
// used before this refactor.
static int sw_threads = 1;
void RasterBackend_SW_SetThreads(int n) { sw_threads = n; }

static void sw_frame_begin(void) {}

static void sw_clear(RSurface *d, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Blitter_ClearSurface(d, r, g, b, a);
}

static void sw_draw(RSurface *d, const BVtx *v, int n, const RTexture *t, RBlend bl, float ar) {
    Blitter_RasterDraw(d, v, n, t, bl, ar, sw_threads);
}

// Present is intentionally a no-op here. Today's actual present/convert step
// (Blitter_ToRGB565, called from main.cpp's frame loop) writes into a
// destination buffer owned by main.cpp and is gated on
// NativeVideoWriter_IsActive() — state this vtable's `present(const RSurface*)`
// signature does not carry. Moving that native-video-entangled code into this
// file risks a behavior change, which Task 3 must not do (see task-3-brief.md
// ambiguity resolution #2). blitter.cpp's Blitter_PresentDefault() still calls
// this hook (so present is routed through the seam), but the real conversion/
// write path is left calling Blitter_ToRGB565 directly, unchanged, from
// main.cpp. A later task can extend the vtable if the fabric back-end needs a
// real present step.
static void sw_present(const RSurface *) {}

static void sw_frame_end(void) {}

extern "C" const RasterBackend backend_sw = {
    "sw", sw_frame_begin, sw_clear, sw_draw, sw_present, sw_frame_end,
};
extern "C" const RasterBackend *RasterBackend_Select(void) { return &backend_sw; }
