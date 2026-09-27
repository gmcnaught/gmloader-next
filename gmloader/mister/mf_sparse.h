#ifndef MF_SPARSE_H
#define MF_SPARSE_H
// Sparse keyed quads: re-emit a mostly-transparent COLORKEY quad as only the
// sub-rects that hold non-key texels.
//
// Measured case (level_2_3, the windmill room, 2026-09-26): the storm's rain
// layer bck_rain_light is a full-screen COLORKEY draw of a 252x214 texture in
// which 348 of 53,928 texels are not the key. The fabric still walks and
// texture-fetches every one of its ~61k pixels: 5.05 ms of a 17.6 ms frame
// (RTL replay of a device capture, real texel cache). Split, the same layer is
// ~110 small rects and ~2k pixels.
//
// HOW. When a page is staged, mf_sp_build records, per band of MF_SP_B texel
// rows and per column, which rows of the band hold a non-key texel. At emit
// time mf_sp_split turns an eligible quad into the rects of runs of such
// columns, one pair of triangles per rect.
//
// EXACTNESS. The output must be bit-identical to the uncut quad (blt_tri.c is
// the contract), so:
//  - Eligible quad: two triangles that are the halves of one axis-aligned rect
//    (mf_occ_quad_rect), every vertex on the pixel grid (x, y multiples of 16),
//    a 1:1 unflipped UV map on the texel grid (u = x - x0 + u0 with u0 a
//    multiple of 16; same for v), one rgba on all six vertices, and a sampled
//    texel rect inside the page (no clamping). Pixel (p, q) then samples texel
//    (tu0 + p - px0, tv0 + q - py0).
//  - That mapping is exact, not approximate: blt_raster_tri interpolates with
//    the top-left-BIASED edge values, so for an attribute that is an integer
//    at every pixel centre the numerator is area*a - S, where S is the sum of
//    that attribute over the vertices whose opposite edge carries the -1 bias,
//    and divr returns a exactly iff S <= area/2. mf_sp_tri_exact evaluates S
//    for every triangle -- the original's two and each one emitted -- with the
//    same CCW normalization and top_left() as blt_tri.c; rects too small to pass
//    are padded, and a quad that cannot be made to pass is left alone.
//  - COLORKEY writes tint(texel) iff texel != key and never reads the
//    destination: a pixel whose texel is the key is a no-op (dropping it is
//    exact), and a pixel written twice by two rects of the same group is
//    written the same value twice (so padding may overlap other rects).
//  - Every emitted rect lies inside the original's pixel rect, and together
//    they cover every pixel whose texel is not the key.
//
// COST (what to split, how to pad): on the fabric a triangle pays per ROW it
// touches, not just per pixel -- each row start is a texel-cache fill. Replay of
// a device frame (2026-09-26) put it at ~85 cycles per triangle-row against ~8
// per pixel, so rects are padded WIDER before taller, runs are merged across
// short gaps, and a split must beat the whole quad under that model.
//
// Pure: no I/O, no device headers; host-testable (mf_sparse_test.cpp).
#include <stdint.h>
#include <string.h>
#include "mf_occlude.h"    // mf_occ_quad_rect, MfOccRect
#include "blitter_ref.h"   // blt_vtx_t

enum {
    MF_SP_B        = 8,      // texel rows per band (one uint8_t of row bits per column)
    MF_SP_MIN_PX   = 4096,   // quads with fewer visible pixels are not worth splitting
    // Fabric cost model, in pixel-equivalents (~8 cycles per pixel):
    MF_SP_ROW_PX   = 11,     // one triangle-row (~85 cycles)
    MF_SP_RECT_PX  = 15,     // one rect's two triangle setups (~120 cycles)
    MF_SP_GAP      = 16,     // merge two runs across at most this many empty columns
};

static inline int mf_sp_map_bytes(int tw, int th) { return tw * ((th + MF_SP_B - 1) / MF_SP_B); }

// bits[band * tw + x], bit r: texel (x, band*MF_SP_B + r) of the tw x th page
// (row stride tw) is not `key`. Returns the number of non-key texels.
static inline int mf_sp_build(const uint16_t *page, int tw, int th, uint16_t key, uint8_t *bits) {
    memset(bits, 0, (size_t)mf_sp_map_bytes(tw, th));
    int n = 0;
    for (int y = 0; y < th; y++) {
        const uint16_t *row = page + (size_t)y * tw;
        uint8_t *b = bits + (size_t)(y / MF_SP_B) * tw;
        const uint8_t bit = (uint8_t)(1u << (y % MF_SP_B));
        for (int x = 0; x < tw; x++)
            if (row[x] != key) { b[x] |= bit; n++; }
    }
    return n;
}

// Exact-interpolation test for one triangle, mirroring blt_raster_tri: CCW
// normalization, top_left() biases, and the per-attribute sum S over the
// vertices whose weight carries the -1 bias. See EXACTNESS.
static inline int mf_sp_top_left(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    return (ay == by && bx < ax) || (by > ay);
}
static inline int mf_sp_tri_exact(const blt_vtx_t *a, const blt_vtx_t *b, const blt_vtx_t *c) {
    const int64_t x0 = a->x, y0 = a->y;
    int64_t x1 = b->x, y1 = b->y, x2 = c->x, y2 = c->y;
    int64_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0) return 1;                         // degenerate: draws nothing
    if (area < 0) {
        const blt_vtx_t *t = b; b = c; c = t;
        int64_t tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; area = -area;
    }
    const int m0 = !mf_sp_top_left((int32_t)x1, (int32_t)y1, (int32_t)x2, (int32_t)y2);   // weight of a
    const int m1 = !mf_sp_top_left((int32_t)x2, (int32_t)y2, (int32_t)x0, (int32_t)y0);   // weight of b
    const int m2 = !mf_sp_top_left((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);   // weight of c
    const blt_vtx_t *vv[3] = { a, b, c };
    const int mm[3] = { m0, m1, m2 };
    int64_t su = 0, sv = 0, sc[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        if (!mm[i]) continue;
        su += vv[i]->u; sv += vv[i]->v;
        for (int k = 0; k < 4; k++) sc[k] += (vv[i]->rgba >> (8 * k)) & 0xFFu;
    }
    const int64_t half = area / 2;
    if (su > half || sv > half) return 0;
    for (int k = 0; k < 4; k++) if (sc[k] > half) return 0;
    return 1;
}

typedef struct { int16_t x0, y0, x1, y1; } MfSpRect;   // inclusive texel rect

// The two triangles of texel rect r (texel = pixel + (dx, dy)), in the corner
// order mf_occ_quad_rect accepts; `base` supplies rgba and the reserved word.
static inline void mf_sp_rect_tris(const MfSpRect *r, int32_t dx, int32_t dy,
                                   const blt_vtx_t *base, blt_vtx_t o[6]) {
    const int16_t X0 = (int16_t)((r->x0 - dx) * 16), X1 = (int16_t)((r->x1 + 1 - dx) * 16);
    const int16_t Y0 = (int16_t)((r->y0 - dy) * 16), Y1 = (int16_t)((r->y1 + 1 - dy) * 16);
    const uint16_t U0 = (uint16_t)(r->x0 * 16), U1 = (uint16_t)((r->x1 + 1) * 16);
    const uint16_t V0 = (uint16_t)(r->y0 * 16), V1 = (uint16_t)((r->y1 + 1) * 16);
    for (int i = 0; i < 6; i++) o[i] = *base;
    o[0].x = X0; o[0].y = Y0; o[0].u = U0; o[0].v = V0;
    o[1].x = X1; o[1].y = Y0; o[1].u = U1; o[1].v = V0;
    o[2].x = X1; o[2].y = Y1; o[2].u = U1; o[2].v = V1;
    o[3].x = X1; o[3].y = Y1; o[3].u = U1; o[3].v = V1;
    o[4].x = X0; o[4].y = Y1; o[4].u = U0; o[4].v = V1;
    o[5].x = X0; o[5].y = Y0; o[5].u = U0; o[5].v = V0;
}

// Grow r inside `lim` -- wider first, rows cost more than pixels -- until both
// of its triangles interpolate exactly; writes them to o. 0 if even `lim` fails.
static inline int mf_sp_pad(MfSpRect *r, const MfSpRect *lim, int32_t dx, int32_t dy,
                            const blt_vtx_t *base, blt_vtx_t o[6]) {
    for (int it = 0; it < 64; it++) {
        mf_sp_rect_tris(r, dx, dy, base, o);
        if (mf_sp_tri_exact(&o[0], &o[1], &o[2]) && mf_sp_tri_exact(&o[3], &o[4], &o[5])) return 1;
        const int32_t w = r->x1 - r->x0 + 1, h = r->y1 - r->y0 + 1;
        if (w < lim->x1 - lim->x0 + 1) {             // widen by ~25%, right then left
            int32_t grow = w / 4 > 0 ? w / 4 : 1;
            int32_t x1 = r->x1 + grow;
            if (x1 > lim->x1) { r->x0 = (int16_t)(r->x0 - (x1 - lim->x1)); x1 = lim->x1; }
            if (r->x0 < lim->x0) r->x0 = lim->x0;
            r->x1 = (int16_t)x1;
        } else if (h < lim->y1 - lim->y0 + 1) {      // then taller
            int32_t grow = h / 4 > 0 ? h / 4 : 1;
            int32_t y1 = r->y1 + grow;
            if (y1 > lim->y1) { r->y0 = (int16_t)(r->y0 - (y1 - lim->y1)); y1 = lim->y1; }
            if (r->y0 < lim->y0) r->y0 = lim->y0;
            r->y1 = (int16_t)y1;
        } else {
            return 0;
        }
    }
    return 0;
}

// Split the quad q[6] (12.4 wire vertices; UVs address the tw x th page whose
// map is `bits`) drawn on an fbw x fbh target. On success writes 6*k vertices
// to `out` and returns k (0 = the visible part holds no non-key texel: draw
// nothing). Returns -1 when the quad is ineligible, not worth splitting, or
// needs more than max_rects rects -- the caller then emits it unchanged.
static inline int mf_sp_split(const blt_vtx_t q[6], int tw, int th, const uint8_t *bits,
                              blt_vtx_t *out, int max_rects) {
    int16_t xs[6], ys[6];
    for (int i = 0; i < 6; i++) { xs[i] = q[i].x; ys[i] = q[i].y; }
    MfOccRect vis;                                   // visible pixels, clamped to the target
    if (!mf_occ_quad_rect(xs, ys, &vis)) return -1;
    const uint32_t rgba = q[0].rgba;
    int32_t qx0 = xs[0], qx1 = xs[0], qy0 = ys[0], qy1 = ys[0];
    for (int i = 0; i < 6; i++) {
        if (q[i].rgba != rgba) return -1;
        if (xs[i] < qx0) qx0 = xs[i]; if (xs[i] > qx1) qx1 = xs[i];
        if (ys[i] < qy0) qy0 = ys[i]; if (ys[i] > qy1) qy1 = ys[i];
    }
    if ((qx0 | qx1 | qy0 | qy1) & 15) return -1;     // off the pixel grid
    int32_t uA = -1, uB = -1, vA = -1, vB = -1;      // u at x0 / x1, v at y0 / y1
    for (int i = 0; i < 6; i++) {
        int32_t &uu = (xs[i] == qx0) ? uA : uB;
        int32_t &vv = (ys[i] == qy0) ? vA : vB;
        if (uu < 0) uu = q[i].u; else if (uu != q[i].u) return -1;
        if (vv < 0) vv = q[i].v; else if (vv != q[i].v) return -1;
    }
    if (uB - uA != qx1 - qx0 || vB - vA != qy1 - qy0) return -1;   // 1:1, unflipped
    if ((uA | vA) & 15) return -1;                   // texel grid
    if (!mf_sp_tri_exact(&q[0], &q[1], &q[2]) || !mf_sp_tri_exact(&q[3], &q[4], &q[5])) return -1;
    const int32_t vis_px = (vis.x1 - vis.x0 + 1) * (vis.y1 - vis.y0 + 1);
    if (vis_px < MF_SP_MIN_PX) return -1;
    const int32_t whole = vis_px + 2 * (vis.y1 - vis.y0 + 1) * MF_SP_ROW_PX + MF_SP_RECT_PX;
    // texel = pixel + (dx, dy)
    const int32_t dx = uA / 16 - qx0 / 16, dy = vA / 16 - qy0 / 16;
    const MfSpRect lim = { (int16_t)(vis.x0 + dx), (int16_t)(vis.y0 + dy),
                           (int16_t)(vis.x1 + dx), (int16_t)(vis.y1 + dy) };
    if (lim.x0 < 0 || lim.y0 < 0 || lim.x1 > tw - 1 || lim.y1 > th - 1) return -1;   // clamped

    int k = 0;
    int32_t cost = 0;
    for (int band = lim.y0 / MF_SP_B; band <= lim.y1 / MF_SP_B; band++) {
        const int by0 = band * MF_SP_B;
        int r0 = lim.y0 - by0, r1 = lim.y1 - by0;    // band-relative rows in view
        if (r0 < 0) r0 = 0; if (r1 > MF_SP_B - 1) r1 = MF_SP_B - 1;
        const uint8_t rmask = (uint8_t)(((1u << (r1 + 1)) - 1u) & ~((1u << r0) - 1u));
        const uint8_t *b = bits + (size_t)band * tw;
        int x = lim.x0;
        while (x <= lim.x1) {
            if (!(b[x] & rmask)) { x++; continue; }
            int s = x; uint8_t rows = 0;
            for (;;) {                               // extend the run, bridging short gaps
                while (x <= lim.x1 && (b[x] & rmask)) rows |= b[x++] & rmask;
                int g = x;
                while (g <= lim.x1 && !(b[g] & rmask)) g++;
                if (g > lim.x1 || g - x > MF_SP_GAP) break;
                x = g;
            }
            int ry0 = 0, ry1 = MF_SP_B - 1;
            while (!(rows & (1u << ry0))) ry0++;
            while (!(rows & (1u << ry1))) ry1--;
            MfSpRect r = { (int16_t)s, (int16_t)(by0 + ry0), (int16_t)(x - 1), (int16_t)(by0 + ry1) };
            if (k >= max_rects) return -1;
            blt_vtx_t *o = out + (size_t)k * 6;
            if (!mf_sp_pad(&r, &lim, dx, dy, &q[0], o)) return -1;
            const int32_t h = r.y1 - r.y0 + 1;
            cost += (r.x1 - r.x0 + 1) * h + 2 * h * MF_SP_ROW_PX + MF_SP_RECT_PX;
            if (cost * 4 >= whole * 3) return -1;    // not sparse enough to pay
            k++;
        }
    }
    return k;
}

#endif /* MF_SPARSE_H */
