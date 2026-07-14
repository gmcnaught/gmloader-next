// Pure, GL-free, host-testable conversion helpers that translate the decoded
// RasterBackend primitives (BVtx / RBlend, blitter_raster.h) into the MFGPU
// fabric wire types (blt_vtx_t / BLT_BLEND_*, refmodel/blitter_ref.h).
//
// Kept header-only and dependency-light so raster_backend_test.cpp can unit-test
// the fixed-point coordinate + color packing in isolation. This conversion is
// correctness-critical: the fabric interprets these bits directly.
//
// IMPORTANT — the mapping conforms to the GOLDEN TRILIST rasterizer
// (3rdparty/mfgpu/refmodel/blt_tri.c), which drove some deviations from the
// original Task 5 brief (see rblend_to_blt + raster_backend_mfgpu.cpp notes):
//   * The TRILIST raster samples the texture page as RGB565 and modulates it by
//     the interpolated per-vertex color (blt_tint565). There is NO per-texel
//     alpha in a triangle list.
//   * Its blend switch has cases COPY / CONST_ALPHA / ADD / MULTIPLY / COLORKEY
//     and NO BLT_BLEND_PALPHA case (PALPHA falls through to an opaque copy).
//     Alpha compositing is therefore done with BLT_BLEND_CONST_ALPHA, whose
//     effective alpha is (interpolated vtx.a * header.alpha)/255. So RB_ALPHA
//     maps to CONST_ALPHA, not PALPHA.
#pragma once

#include "blitter_raster.h"   // BVtx, RBlend, RB_*
#include <math.h>
#include <stdint.h>

extern "C" {
#include "blitter_ref.h"      // blt_vtx_t, BLT_RGBA, BLT_BLEND_*
}

static inline float rbc_clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// One decoded vertex -> one fabric vertex.
//   screen x,y : signed 12.4 fixed-point (pixels << 4)
//   texel u,v  : unsigned 12.4 fixed-point (clamp(uv,0,1) * tex_dim << 4)
//   rgba       : per-vertex color r | g<<8 | b<<16 | a<<24 (matches BLT_RGBA)
// lroundf() gives round-half-away-from-zero, the exact rounding the brief
// specifies ("lround(v->x*16)"); the BLT_RGBA channel order is confirmed against
// the packing macro in blitter_ref.h (r | g<<8 | b<<16 | a<<24).
static inline blt_vtx_t bvtx_to_blt(const BVtx *v, int tex_w, int tex_h) {
    blt_vtx_t o;
    o.x = (int16_t)lroundf(v->x * 16.0f);
    o.y = (int16_t)lroundf(v->y * 16.0f);
    o.u = (uint16_t)lroundf(rbc_clampf(v->u, 0.0f, 1.0f) * (float)tex_w * 16.0f);
    o.v = (uint16_t)lroundf(rbc_clampf(v->v, 0.0f, 1.0f) * (float)tex_h * 16.0f);
    o.rgba = BLT_RGBA(lroundf(v->r * 255.0f), lroundf(v->g * 255.0f),
                      lroundf(v->b * 255.0f), lroundf(v->a * 255.0f));
    o._rsvd = 0;
    return o;
}

// RBlend -> fabric TRILIST blend mode.
//   RB_NONE    -> BLT_BLEND_COPY
//   RB_ALPHA   -> BLT_BLEND_CONST_ALPHA (vertex-alpha source-over; see header note)
//   RB_PREMULT -> BLT_BLEND_CONST_ALPHA (nominal; the backend actually routes
//                 RB_PREMULT to the SW rasterizer because the fabric has no exact
//                 premultiplied source-over `dst = src + dst*(1-a)` blend)
//   RB_ADD     -> BLT_BLEND_ADD
static inline uint8_t rblend_to_blt(RBlend b) {
    switch (b) {
        case RB_NONE:    return BLT_BLEND_COPY;
        case RB_ALPHA:   return BLT_BLEND_CONST_ALPHA;
        case RB_PREMULT: return BLT_BLEND_CONST_ALPHA;
        case RB_ADD:     return BLT_BLEND_ADD;
    }
    return BLT_BLEND_COPY;
}
