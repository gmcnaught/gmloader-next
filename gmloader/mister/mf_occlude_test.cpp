// Host test for mf_occlude.h, against the golden rasterizer (blt_tri.c).
//   make -f Makefile.gmloader mf-occlude-test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mf_occlude.h"
extern "C" {
#include "blitter_ref.h"
#include "blt_tri.h"
}

static const int W = MF_OCC_W, H = MF_OCC_H;
static uint16_t g_tex[64 * 64];   // opaque texel page, texel (u,v) = 1 + u + 64*v
static blt_surface_heap_t g_heap;

static blt_cmd_t cmd_copy(void) {
    blt_cmd_t h; memset(&h, 0, sizeof h);
    h.opcode = BLT_OP_TRILIST; h.blend_mode = BLT_BLEND_COPY; h.alpha = 255;
    h.src_off = 0; h.src_stride = 64 * 2; h.src_x = 64; h.src_y = 64;
    return h;
}
static blt_vtx_t V(int x, int y, int u, int v) {
    blt_vtx_t o; memset(&o, 0, sizeof o);
    o.x = (int16_t)x; o.y = (int16_t)y; o.u = (uint16_t)u; o.v = (uint16_t)v;
    o.rgba = 0xFFFFFFFFu;
    return o;
}
// Pixels written by `tris` (golden): fb starts at 0, texels are never 0.
static void golden_cover(const blt_vtx_t *tris, int nt, uint8_t *cov) {
    static uint16_t fb[288 * 216];
    memset(fb, 0, sizeof fb);
    blt_cmd_t h = cmd_copy();
    blt_raster_tri(fb, &g_heap, &h, tris, nt, NULL);
    for (int i = 0; i < W * H; i++) cov[i] = fb[i] != 0;
}
static int rnd(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

// 1. mf_occ_quad_rect is the EXACT pixel set of both triangle splits.
static int case_quad_rect_exact(void) {
    static uint8_t cov[288 * 216];
    for (int it = 0; it < 4000; it++) {
        int x0 = rnd(-200, 288 * 16 + 100), x1 = x0 + rnd(1, 900);
        int y0 = rnd(-200, 216 * 16 + 100), y1 = y0 + rnd(1, 900);
        if (it % 3 == 0) { x0 &= ~15; x1 &= ~15; y0 &= ~15; y1 &= ~15; if (x1 <= x0) x1 = x0 + 16; if (y1 <= y0) y1 = y0 + 16; }
        for (int diag = 0; diag < 4; diag++) {
            blt_vtx_t q[6];
            if (diag == 0) { q[0]=V(x0,y0,0,0); q[1]=V(x1,y0,0,0); q[2]=V(x1,y1,0,0); q[3]=V(x0,y0,0,0); q[4]=V(x1,y1,0,0); q[5]=V(x0,y1,0,0); }
            if (diag == 1) { q[0]=V(x0,y0,0,0); q[1]=V(x1,y0,0,0); q[2]=V(x0,y1,0,0); q[3]=V(x1,y0,0,0); q[4]=V(x1,y1,0,0); q[5]=V(x0,y1,0,0); }
            if (diag == 2) { q[0]=V(x1,y1,0,0); q[1]=V(x0,y0,0,0); q[2]=V(x1,y0,0,0); q[3]=V(x0,y1,0,0); q[4]=V(x1,y1,0,0); q[5]=V(x0,y0,0,0); }
            if (diag == 3) { q[0]=V(x0,y1,0,0); q[1]=V(x0,y0,0,0); q[2]=V(x1,y0,0,0); q[3]=V(x1,y1,0,0); q[4]=V(x0,y1,0,0); q[5]=V(x1,y0,0,0); }
            golden_cover(q, 2, cov);
            int16_t xs[6], ys[6];
            for (int i = 0; i < 6; i++) { xs[i] = q[i].x; ys[i] = q[i].y; }
            MfOccRect r; int ok = mf_occ_quad_rect(xs, ys, &r);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                int in = ok && x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
                // Always a subset; exact when no pixel centre lies on the rect edge.
                const int on_edge = ((x0 - 8) % 16 == 0) || ((x1 - 8) % 16 == 0) ||
                                    ((y0 - 8) % 16 == 0) || ((y1 - 8) % 16 == 0);
                if ((in && !cov[y * W + x]) || (!on_edge && in != cov[y * W + x])) {
                    printf("  FAIL quad-rect  it=%d diag=%d rect12.4=(%d,%d)-(%d,%d) px(%d,%d) model=%d golden=%d\n",
                           it, diag, x0, y0, x1, y1, x, y, in, cov[y * W + x]);
                    return 0;
                }
            }
        }
    }
    printf("  OK   quad-rect    subset of blt_raster_tri on 16000 quads, exact off-edge\n");
    return 1;
}

// 2. Non-rectangles are refused.
static int case_quad_rect_refuses(void) {
    int16_t xs[6] = {0, 160, 160, 0, 160, 0}, ys[6] = {0, 0, 160, 0, 160, 160};
    MfOccRect r;
    if (!mf_occ_quad_rect(xs, ys, &r)) { printf("  FAIL refuse  valid quad refused\n"); return 0; }
    int16_t sk[6] = {0, 160, 170, 0, 170, 0};           // skewed corner
    if (mf_occ_quad_rect(sk, ys, &r)) { printf("  FAIL refuse  skew accepted\n"); return 0; }
    int16_t dup[6] = {0, 160, 160, 0, 160, 160};        // same half twice
    int16_t dupy[6] = {0, 0, 160, 0, 0, 160};
    if (mf_occ_quad_rect(dup, dupy, &r)) { printf("  FAIL refuse  duplicate half accepted\n"); return 0; }
    printf("  OK   quad-refuse  skewed / duplicated halves refused\n");
    return 1;
}

// 3. mf_occ_tri_bbox is a superset of every triangle's golden pixels.
static int case_tri_bbox_superset(void) {
    static uint8_t cov[288 * 216];
    for (int it = 0; it < 4000; it++) {
        blt_vtx_t t[3];
        for (int k = 0; k < 3; k++) t[k] = V(rnd(-300, 288 * 16 + 300), rnd(-300, 216 * 16 + 300), 0, 0);
        golden_cover(t, 1, cov);
        int16_t xs[3] = {t[0].x, t[1].x, t[2].x}, ys[3] = {t[0].y, t[1].y, t[2].y};
        MfOccRect r; int ok = mf_occ_tri_bbox(xs, ys, &r);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            if (!cov[y * W + x]) continue;
            if (!ok || x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) {
                printf("  FAIL tri-bbox  it=%d golden pixel (%d,%d) outside bbox\n", it, x, y);
                return 0;
            }
        }
    }
    printf("  OK   tri-bbox     superset of golden coverage on 4000 triangles\n");
    return 1;
}

// 4. End to end: random scenes of quads (some opaque occluders, some not)
// rendered with and without the culled triangles are pixel-identical, and
// the reads_appsurf barrier is honoured.
static int case_resolve_bit_identical(void) {
    static MfOccTri rec[256];
    static uint8_t cull[256];
    static MfOccMask mk;
    static uint16_t fb_a[288 * 216], fb_b[288 * 216];
    long total_culled = 0;
    for (int it = 0; it < 300; it++) {
        const int nq = rnd(2, 60);
        blt_vtx_t tris[256 * 3];
        uint8_t blend[128];
        for (int q = 0; q < nq; q++) {
            int x0 = rnd(-40, 280) * 16 + (it % 2 ? rnd(0, 15) : 0), y0 = rnd(-40, 210) * 16;
            int x1 = x0 + rnd(1, 160) * 16, y1 = y0 + rnd(1, 120) * 16;
            int u = rnd(0, 32) * 16, v = rnd(0, 32) * 16;
            blt_vtx_t *p = &tris[q * 6];
            p[0] = V(x0, y0, u, v);            p[1] = V(x1, y0, u + 256, v);
            p[2] = V(x1, y1, u + 256, v + 256); p[3] = V(x0, y0, u, v);
            p[4] = V(x1, y1, u + 256, v + 256); p[5] = V(x0, y1, u, v + 256);
            if (rnd(0, 5) == 0) { p[2].x += 40; p[4].x += 40; }   // not a rect: never an occluder
            blend[q] = rnd(0, 2) ? BLT_BLEND_COPY : BLT_BLEND_ADD;
            for (int k = 0; k < 2; k++) {
                MfOccTri *t = &rec[q * 2 + k];
                memset(t, 0, sizeof *t);
                int16_t xs[3], ys[3];
                for (int j = 0; j < 3; j++) { xs[j] = p[k * 3 + j].x; ys[j] = p[k * 3 + j].y; }
                t->has_bbox = (uint8_t)mf_occ_tri_bbox(xs, ys, &t->bbox);
            }
            int16_t qx[6], qy[6];
            for (int j = 0; j < 6; j++) { qx[j] = p[j].x; qy[j] = p[j].y; }
            if (blend[q] == BLT_BLEND_COPY)
                rec[q * 2].has_occ = (uint8_t)mf_occ_quad_rect(qx, qy, &rec[q * 2].occ);
        }
        const int n = nq * 2;
        total_culled += mf_occ_resolve(rec, n, cull, &mk);
        memset(fb_a, 0, sizeof fb_a); memset(fb_b, 0, sizeof fb_b);
        for (int q = 0; q < nq; q++) {
            blt_cmd_t h = cmd_copy(); h.blend_mode = blend[q];
            blt_raster_tri(fb_a, &g_heap, &h, &tris[q * 6], 2, NULL);
            blt_vtx_t kept[6];
            memcpy(kept, &tris[q * 6], sizeof kept);
            for (int k = 0; k < 2; k++)
                if (cull[q * 2 + k]) { kept[k*3+1].x = kept[k*3+2].x = kept[k*3].x;
                                       kept[k*3+1].y = kept[k*3+2].y = kept[k*3].y; }
            blt_raster_tri(fb_b, &g_heap, &h, kept, 2, NULL);
        }
        if (memcmp(fb_a, fb_b, sizeof fb_a) != 0) {
            printf("  FAIL resolve  scene %d differs after cull\n", it);
            return 0;
        }
    }
    if (total_culled == 0) { printf("  FAIL resolve  nothing culled - test proves nothing\n"); return 0; }
    printf("  OK   resolve      300 random scenes bit-identical, %ld triangles culled\n", total_culled);
    return 1;
}

// 5. A surface read between occluder and occludee blocks the cull.
static int case_read_barrier(void) {
    static MfOccMask mk;
    MfOccTri rec[3]; uint8_t cull[3];
    memset(rec, 0, sizeof rec);
    MfOccRect full = {0, 0, W - 1, H - 1};
    rec[0].target = 1; rec[0].has_bbox = 1; rec[0].bbox = full;          // draw on APPSURF
    rec[1].target = 0; rec[1].has_bbox = 1; rec[1].bbox = full;          // composite reads APPSURF
    rec[1].reads_appsurf = 1;
    rec[2].target = 1; rec[2].has_bbox = 1; rec[2].bbox = full;          // later opaque on APPSURF
    rec[2].has_occ = 1; rec[2].occ = full;
    mf_occ_resolve(rec, 3, cull, &mk);
    if (cull[0]) { printf("  FAIL barrier  draw read by the composite was culled\n"); return 0; }
    rec[1].reads_appsurf = 0;
    mf_occ_resolve(rec, 3, cull, &mk);
    if (!cull[0]) { printf("  FAIL barrier  control case not culled\n"); return 0; }
    printf("  OK   barrier      a surface read between occluder and draw blocks the cull\n");
    return 1;
}

int main(void) {
    srand(12345);
    for (int v = 0; v < 64; v++) for (int u = 0; u < 64; u++) g_tex[v * 64 + u] = (uint16_t)(1 + u + 64 * v);
    g_heap.base = (const uint8_t *)g_tex; g_heap.size = sizeof g_tex;
    int ok = 1;
    ok &= case_quad_rect_exact();
    ok &= case_quad_rect_refuses();
    ok &= case_tri_bbox_superset();
    ok &= case_resolve_bit_identical();
    ok &= case_read_barrier();
    printf(ok ? "mf-occlude-test: PASS\n" : "mf-occlude-test: FAIL\n");
    return ok ? 0 : 1;
}
