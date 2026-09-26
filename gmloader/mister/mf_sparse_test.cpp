// Host test for mf_sparse.h, against the golden rasterizer (blt_tri.c).
//   make -f Makefile.gmloader mf-sparse-test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mf_sparse.h"
extern "C" {
#include "blt_tri.h"
}

static const int W = MF_OCC_W, H = MF_OCC_H;
static const uint16_t KEY = 0xF81F;
enum { TW_MAX = 320, TH_MAX = 256, MAX_RECTS = 1024 };
static uint16_t g_page[TW_MAX * TH_MAX];
static uint8_t  g_bits[TW_MAX * ((TH_MAX + MF_SP_B - 1) / MF_SP_B)];
static blt_vtx_t g_out[MAX_RECTS * 6];
static blt_surface_heap_t g_heap;
static uint16_t fb_a[288 * 216], fb_b[288 * 216], fb_bg[288 * 216];

static int rnd(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

static blt_cmd_t cmd_key(int tw, int th) {
    blt_cmd_t h; memset(&h, 0, sizeof h);
    h.opcode = BLT_OP_TRILIST; h.blend_mode = BLT_BLEND_COLORKEY; h.colorkey = KEY; h.alpha = 255;
    h.src_off = 0; h.src_stride = (uint16_t)(tw * 2); h.src_x = (uint16_t)tw; h.src_y = (uint16_t)th;
    return h;
}
static blt_vtx_t V(int x, int y, int u, int v, uint32_t rgba) {
    blt_vtx_t o; memset(&o, 0, sizeof o);
    o.x = (int16_t)x; o.y = (int16_t)y; o.u = (uint16_t)u; o.v = (uint16_t)v; o.rgba = rgba;
    return o;
}
// One of the four ways a rect is cut into two triangles (vertex x/y/u/v all from its corners).
static void quad(blt_vtx_t *q, int diag, int x0, int y0, int x1, int y1,
                 int u0, int v0, int u1, int v1, uint32_t c) {
    const blt_vtx_t A = V(x0, y0, u0, v0, c), B = V(x1, y0, u1, v0, c),
                    C = V(x1, y1, u1, v1, c), D = V(x0, y1, u0, v1, c);
    const blt_vtx_t p[4][6] = { { A, B, C, A, C, D }, { A, B, D, B, C, D },
                                { C, A, B, D, C, A }, { D, A, B, C, D, B } };
    memcpy(q, p[diag], sizeof p[diag]);
}

// Page content: the key everywhere, plus rain-like diagonal streaks or random dots.
static void fill_page(int tw, int th, int density_ppm, int streaks) {
    for (int i = 0; i < tw * th; i++) g_page[i] = KEY;
    const long want = (long)tw * th * density_ppm / 1000000;
    for (long n = 0; n < want; ) {
        int x = rnd(0, tw - 1), y = rnd(0, th - 1);
        const int len = streaks ? rnd(2, 7) : 1;
        for (int k = 0; k < len && x < tw && y < th; k++, n++) {
            g_page[y * tw + x] = (uint16_t)rnd(0, 0xFFFF);
            if (g_page[y * tw + x] == KEY) g_page[y * tw + x] ^= 0x20;
            x += (k & 1); y++;
        }
    }
}

// 1. Split output is bit-identical to the uncut quad over random scenes.
static int case_random_exact(void) {
    long split = 0, refused = 0, px_before = 0, px_after = 0, empty = 0;
    for (int it = 0; it < 6000; it++) {
        const int tw = rnd(16, TW_MAX), th = rnd(16, TH_MAX);
        static const int dens[] = { 500, 2000, 6500, 20000, 100000, 400000 };
        fill_page(tw, th, dens[rnd(0, 5)], rnd(0, 1));
        mf_sp_build(g_page, tw, th, KEY, g_bits);
        g_heap.base = (const uint8_t *)g_page; g_heap.size = sizeof g_page;
        // texel sub-rect and on-screen placement (may hang off any edge)
        const int w = rnd(1, tw), h = rnd(1, th);
        const int tu = rnd(0, tw - w), tv = rnd(0, th - h);
        const int X = rnd(-w + 1, W - 1), Y = rnd(-h + 1, H - 1);
        uint32_t c = 0xFFFFFFFFu;
        if (it % 5 == 1) c = (uint32_t)rand() | 0xFF000000u;
        blt_vtx_t q[6];
        quad(q, rnd(0, 3), X * 16, Y * 16, (X + w) * 16, (Y + h) * 16,
             tu * 16, tv * 16, (tu + w) * 16, (tv + h) * 16, c);
        for (int i = 0; i < W * H; i++) fb_bg[i] = (uint16_t)rand();
        const blt_cmd_t cmd = cmd_key(tw, th);
        memcpy(fb_a, fb_bg, sizeof fb_a);
        blt_raster_tri(fb_a, &g_heap, &cmd, q, 2, NULL);
        const int k = mf_sp_split(q, tw, th, g_bits, g_out, MAX_RECTS);
        if (k < 0) { refused++; continue; }
        split++; if (k == 0) empty++;
        memcpy(fb_b, fb_bg, sizeof fb_b);
        blt_raster_tri(fb_b, &g_heap, &cmd, g_out, 2 * k, NULL);
        if (memcmp(fb_a, fb_b, sizeof fb_a) != 0) {
            int bx = -1, by = -1;
            for (int i = 0; i < W * H; i++) if (fb_a[i] != fb_b[i]) { bx = i % W; by = i / W; break; }
            printf("  FAIL random  it=%d tw=%d th=%d tex(%d,%d %dx%d) at(%d,%d) rgba=%08x k=%d "
                   "first diff px(%d,%d)\n", it, tw, th, tu, tv, w, h, X, Y, c, k, bx, by);
            return 0;
        }
        int vx0 = X < 0 ? 0 : X, vy0 = Y < 0 ? 0 : Y;
        int vx1 = X + w > W ? W : X + w, vy1 = Y + h > H ? H : Y + h;
        px_before += (long)(vx1 - vx0) * (vy1 - vy0);
        for (int r = 0; r < k; r++)
            px_after += (long)((g_out[r * 6 + 1].x - g_out[r * 6].x) / 16) *
                        ((g_out[r * 6 + 2].y - g_out[r * 6].y) / 16);
    }
    if (split < 500) { printf("  FAIL random  only %ld quads split - test proves little\n", split); return 0; }
    printf("  OK   random       %ld split (%ld empty), %ld left whole, bit-identical; "
           "split px %ld -> %ld\n", split, empty, refused, px_before, px_after);
    return 1;
}

// 2. Ineligible geometry is refused, never "approximately" split.
static int case_refuse(void) {
    const int tw = 256, th = 214;
    fill_page(tw, th, 6500, 1);
    mf_sp_build(g_page, tw, th, KEY, g_bits);
    blt_vtx_t q[6];
    struct { const char *what; int x0, y0, x1, y1, u0, v0, u1, v1; } bad[] = {
        { "off pixel grid",  8, 0, 256 * 16 + 8, 214 * 16, 0, 0, 256 * 16, 214 * 16 },
        { "scaled 2:1",      0, 0, 256 * 16, 214 * 16, 0, 0, 128 * 16, 107 * 16 },
        { "flipped",         0, 0, 256 * 16, 214 * 16, 256 * 16, 0, 0, 214 * 16 },
        { "sub-texel uv",    0, 0, 200 * 16, 200 * 16, 4, 0, 200 * 16 + 4, 200 * 16 },
        { "clamped texels",  0, 0, 288 * 16, 214 * 16, 0, 0, 288 * 16, 214 * 16 },
    };
    for (auto &b : bad) {
        quad(q, 0, b.x0, b.y0, b.x1, b.y1, b.u0, b.v0, b.u1, b.v1, 0xFFFFFFFFu);
        if (mf_sp_split(q, tw, th, g_bits, g_out, MAX_RECTS) >= 0) {
            printf("  FAIL refuse  %s accepted\n", b.what); return 0;
        }
    }
    quad(q, 0, 0, 0, 256 * 16, 214 * 16, 0, 0, 256 * 16, 214 * 16, 0xFFFFFFFFu);
    q[4].rgba = 0xFFFFFFFEu;
    if (mf_sp_split(q, tw, th, g_bits, g_out, MAX_RECTS) >= 0) {
        printf("  FAIL refuse  mixed vertex colours accepted\n"); return 0;
    }
    quad(q, 0, 0, 0, 256 * 16, 214 * 16, 0, 0, 256 * 16, 214 * 16, 0xFFFFFFFFu);
    if (mf_sp_split(q, tw, th, g_bits, g_out, MAX_RECTS) < 0) {
        printf("  FAIL refuse  control (rain-density full-screen quad) refused\n"); return 0;
    }
    printf("  OK   refuse       off-grid / scaled / flipped / sub-texel / clamped / mixed rgba refused\n");
    return 1;
}

int main(void) {
    srand(12345);
    int ok = 1;
    ok &= case_refuse();
    ok &= case_random_exact();
    printf(ok ? "mf-sparse-test: PASS\n" : "mf-sparse-test: FAIL\n");
    return ok ? 0 : 1;
}
