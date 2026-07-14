// RasterBackend seam (Task 3). Behind this vtable sits the code that actually
// owns pixels: clear a surface, rasterize a decoded triangle list into it, and
// present the finished frame. blitter.cpp's GL-free decode (RSurface/BVtx/
// RTexture/RBlend, produced by state-shadow + draw decode) is unchanged by
// this seam — only the callee for clear/draw/present moves behind it.
//
// Task 3 ships exactly one implementation, `backend_sw`, which is a thin
// wrapper around today's software rasterizer (blitter_raster.cpp) with zero
// added logic and zero pixel change. A later task adds an FPGA-fabric
// back-end (`backend_mfgpu`) behind the same seam and makes the selector
// choose between them; RasterBackend_Select() always returns backend_sw here.
#ifndef RASTER_BACKEND_H
#define RASTER_BACKEND_H
#include "blitter_raster.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RasterBackend {
    const char *name;
    void (*frame_begin)(void);
    void (*clear)(RSurface *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void (*draw)(RSurface *dst, const BVtx *verts, int triCount,
                 const RTexture *tex, RBlend blend, float alphaRef,
                 uint32_t tex_key);
    void (*present)(const RSurface *defSurf);
    void (*frame_end)(void);
} RasterBackend;

/* Returns the active back-end. Task 3 always returns backend_sw;
 * Task 6 makes this env-selectable. */
const RasterBackend *RasterBackend_Select(void);

/* SW back-end only: mirror blitter.cpp's g_threads (GMLOADER_BLITTER_THREADS)
 * so backend_sw's rasterizer uses the same worker-thread count the direct
 * Blitter_RasterDraw call used before this refactor. Call once at init. */
void RasterBackend_SW_SetThreads(int n);


/* mfgpu back-end only: drop the cached staging of GL texture `id` (on GL
 * re-upload/delete). No-op when backend_sw is selected or nothing is cached. */
void RasterBackend_MFGPU_InvalidateTex(uint32_t id);
#ifdef __cplusplus
}
#endif
#endif
