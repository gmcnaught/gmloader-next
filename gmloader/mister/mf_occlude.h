#ifndef MF_OCCLUDE_H
#define MF_OCCLUDE_H
// Occlusion cull: drop triangles whose every pixel is overwritten later in the
// same frame, on the same target, by an opaque screen-aligned quad.
//
// Measured case (level_1_4, 2026-09-25): the room background bck_cellars is a
// full-screen COPY, and the 64x32 wall-tile layer drawn right after it is keyed
// but has no transparent texel, so it hides ~61k of the background's 62k pixels
// -- ~4.8 ms of a 19.2 ms fabric frame spent on pixels nobody sees.
//
// HOW. mf_emit_group records every triangle it pushes (pixel bbox, target, the
// vertex's place in the arena) and, for each 2-triangle quad that provably
// writes every pixel it covers, an occluder rect. At frame end, BEFORE the
// doorbell, mf_occ_resolve walks the frame backwards keeping a per-target
// bitmask of "pixels a later opaque quad overwrites"; a triangle whose whole
// candidate-pixel bbox is in that mask is culled by collapsing it to zero area
// (blt_tri.c `if(area==0) continue;`, blitter_top.sv ts_degenerate -> S_TRI_NEXT).
//
// EXACTNESS. The output must be bit-identical to the uncut frame, so:
//  - An occluder rect is a SUBSET of the pixels an axis-aligned rectangle drawn
//    as two triangles writes under blt_tri.c's rules: pixel centres
//    ((p<<4)|8) strictly inside (x0,x1)x(y0,y1). The two halves share the
//    diagonal and the top-left bias gives each diagonal pixel to exactly one.
//    Exact whenever no centre lies on the rect boundary (pixel-aligned quads).
//  - A culled triangle's test uses a SUPERSET of its pixels (centres inside its
//    vertex bbox), so "bbox in mask" implies "every pixel in mask".
//  - Opacity is decided by the caller (COPY blend, or COLORKEY whose sampled
//    texel rect has no key texel); nothing here guesses it.
//  - A draw that SAMPLES a target (the app-surface composite) is a read of that
//    target: walking backwards, its mask is cleared there, so an occluder after
//    the read never culls a draw the read depends on.
//  - FILLs are neither culled nor occluders (not triangles; conservative).
//
// Pure: no I/O, no device headers; host-testable (mf_occlude_test.cpp).
#include <stdint.h>
#include <string.h>

enum { MF_OCC_W = 288, MF_OCC_H = 216, MF_OCC_TARGETS = 2,
       MF_OCC_WORDS = (MF_OCC_W + 31) / 32 };

// floor(a / b) for b > 0, correct for negative a.
static inline int32_t mf_occ_floordiv(int32_t a, int32_t b) {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

typedef struct { int16_t x0, y0, x1, y1; } MfOccRect;   // inclusive pixel rect

// Candidate pixels of a triangle with 12.4 vertex coords: centres inside its
// vertex bbox, clamped to the target. Returns 0 if it can write no pixel.
static inline int mf_occ_tri_bbox(const int16_t xs[3], const int16_t ys[3], MfOccRect *r) {
    int32_t lx = xs[0], hx = xs[0], ly = ys[0], hy = ys[0];
    for (int i = 1; i < 3; i++) {
        if (xs[i] < lx) lx = xs[i]; if (xs[i] > hx) hx = xs[i];
        if (ys[i] < ly) ly = ys[i]; if (ys[i] > hy) hy = ys[i];
    }
    // centre c = 16p+8 in [lx,hx]  <=>  p in [ceil((lx-8)/16), floor((hx-8)/16)]
    int32_t px0 = -mf_occ_floordiv(-(lx - 8), 16), px1 = mf_occ_floordiv(hx - 8, 16);
    int32_t py0 = -mf_occ_floordiv(-(ly - 8), 16), py1 = mf_occ_floordiv(hy - 8, 16);
    if (px0 < 0) px0 = 0; if (py0 < 0) py0 = 0;
    if (px1 > MF_OCC_W - 1) px1 = MF_OCC_W - 1;
    if (py1 > MF_OCC_H - 1) py1 = MF_OCC_H - 1;
    if (px0 > px1 || py0 > py1) return 0;
    r->x0 = (int16_t)px0; r->y0 = (int16_t)py0; r->x1 = (int16_t)px1; r->y1 = (int16_t)py1;
    return 1;
}

// Exact pixel set of a quad given as two triangles (6 vertices, 12.4), if the
// two triangles are the two halves of one axis-aligned rectangle. Returns 0 if
// the geometry is anything else, or covers no pixel on the target.
static inline int mf_occ_quad_rect(const int16_t xs[6], const int16_t ys[6], MfOccRect *r) {
    int32_t x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 6; i++) {
        if (xs[i] < x0) x0 = xs[i]; if (xs[i] > x1) x1 = xs[i];
        if (ys[i] < y0) y0 = ys[i]; if (ys[i] > y1) y1 = ys[i];
    }
    if (x0 == x1 || y0 == y1) return 0;
    // Every vertex a corner; each triangle three DISTINCT corners; together all
    // four. (Two such triangles are complementary halves of the rect.)
    unsigned all = 0;
    for (int t = 0; t < 2; t++) {
        unsigned m = 0;
        for (int k = 0; k < 3; k++) {
            int32_t x = xs[t * 3 + k], y = ys[t * 3 + k];
            if ((x != x0 && x != x1) || (y != y0 && y != y1)) return 0;
            unsigned bit = 1u << (((x == x1) ? 1 : 0) | ((y == y1) ? 2 : 0));
            if (m & bit) return 0;
            m |= bit;
        }
        all |= m;
    }
    if (all != 0xFu) return 0;
    // Centres strictly inside: x0 < 16p+8 < x1  <=>  p in [floor((x0-8)/16)+1,
    // ceil((x1-8)/16)-1]. A centre exactly ON the boundary is left out: which
    // of those the top-left rule keeps depends on the edge, and a subset is
    // always safe. For pixel-aligned quads (every tile / sprite at an integer
    // position) no centre lies on the boundary, so this is the exact set.
    int32_t px0 = mf_occ_floordiv(x0 - 8, 16) + 1, px1 = -mf_occ_floordiv(-(x1 - 8), 16) - 1;
    int32_t py0 = mf_occ_floordiv(y0 - 8, 16) + 1, py1 = -mf_occ_floordiv(-(y1 - 8), 16) - 1;
    if (px0 < 0) px0 = 0; if (py0 < 0) py0 = 0;
    if (px1 > MF_OCC_W - 1) px1 = MF_OCC_W - 1;
    if (py1 > MF_OCC_H - 1) py1 = MF_OCC_H - 1;
    if (px0 > px1 || py0 > py1) return 0;
    r->x0 = (int16_t)px0; r->y0 = (int16_t)py0; r->x1 = (int16_t)px1; r->y1 = (int16_t)py1;
    return 1;
}

// One recorded triangle, in submission order.
typedef struct {
    MfOccRect bbox;       // candidate pixels (valid iff has_bbox)
    MfOccRect occ;        // occluder rect added AFTER this triangle is tested (iff has_occ)
    uint8_t   target;     // 0 = WORK, 1 = APPSURF
    uint8_t   has_bbox;
    uint8_t   has_occ;    // set on the FIRST triangle of an opaque quad
    uint8_t   reads_appsurf; // set on the FIRST triangle of a surface-sampling group
} MfOccTri;

typedef struct { uint32_t m[MF_OCC_TARGETS][MF_OCC_H][MF_OCC_WORDS]; } MfOccMask;

static inline void mf_occ_mask_add(MfOccMask *mk, int tg, const MfOccRect *r) {
    for (int y = r->y0; y <= r->y1; y++) {
        uint32_t *row = mk->m[tg][y];
        for (int x = r->x0; x <= r->x1; ) {
            const int w = x >> 5, b = x & 31;
            const int n = (r->x1 - x + 1 < 32 - b) ? (r->x1 - x + 1) : (32 - b);
            const uint32_t bits = (n == 32) ? 0xFFFFFFFFu : (((1u << n) - 1u) << b);
            row[w] |= bits;
            x += n;
        }
    }
}

static inline int mf_occ_mask_covers(const MfOccMask *mk, int tg, const MfOccRect *r) {
    for (int y = r->y0; y <= r->y1; y++) {
        const uint32_t *row = mk->m[tg][y];
        for (int x = r->x0; x <= r->x1; ) {
            const int w = x >> 5, b = x & 31;
            const int n = (r->x1 - x + 1 < 32 - b) ? (r->x1 - x + 1) : (32 - b);
            const uint32_t bits = (n == 32) ? 0xFFFFFFFFu : (((1u << n) - 1u) << b);
            if ((row[w] & bits) != bits) return 0;
            x += n;
        }
    }
    return 1;
}

// Backward pass. Sets cull[i] = 1 for every triangle that is safe to drop.
// Returns the number culled. `mk` is caller-owned scratch (~15 KB).
static inline int mf_occ_resolve(const MfOccTri *tris, int n, uint8_t *cull, MfOccMask *mk) {
    memset(mk, 0, sizeof *mk);
    int culled = 0;
    for (int i = n - 1; i >= 0; i--) {
        const MfOccTri *t = &tris[i];
        cull[i] = 0;
        if (t->target < MF_OCC_TARGETS && t->has_bbox &&
            mf_occ_mask_covers(mk, t->target, &t->bbox)) {
            cull[i] = 1;
            culled++;
        }
        if (t->reads_appsurf) memset(mk->m[1], 0, sizeof mk->m[1]);
        if (t->has_occ && t->target < MF_OCC_TARGETS) mf_occ_mask_add(mk, t->target, &t->occ);
    }
    return culled;
}

#endif /* MF_OCCLUDE_H */
