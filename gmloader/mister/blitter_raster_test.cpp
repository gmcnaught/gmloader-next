//
//  Host unit test for the GL-free software rasterizer (blitter_raster.cpp).
//  Build & run on the dev machine (no GL/SDL needed):
//    g++ -std=c++17 -O2 -fsanitize=address -g \
//        gmloader/mister/blitter_raster.cpp gmloader/mister/blitter_raster_test.cpp \
//        -o /tmp/rtest && /tmp/rtest
//
#include "blitter_raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// ---- tiny test harness ------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void report(const char *name, bool ok) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) ++g_pass; else ++g_fail;
}

// Fetch a dest pixel.
static inline const uint8_t *px(const RSurface &s, int x, int y) {
    return s.rgba + ((size_t)y * (size_t)s.w + (size_t)x) * 4;
}

static bool px_eq(const RSurface &s, int x, int y,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t *p = px(s, x, y);
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

// Allow +/- tol per channel for blended results (rounding tolerant).
static bool px_near(const RSurface &s, int x, int y,
                    int r, int g, int b, int a, int tol) {
    const uint8_t *p = px(s, x, y);
    return abs((int)p[0] - r) <= tol && abs((int)p[1] - g) <= tol &&
           abs((int)p[2] - b) <= tol && abs((int)p[3] - a) <= tol;
}

// ---- shared fixtures --------------------------------------------------------

// 2x2 texture: row0 = red, green ; row1 = blue, white  (R,G,B,A bytes).
static uint8_t g_tex2x2[2 * 2 * 4] = {
    255,   0,   0, 255,    0, 255,   0, 255,   // (0,0) red   (1,0) green
      0,   0, 255, 255,  255, 255, 255, 255,   // (0,1) blue  (1,1) white
};

static RTexture make_tex2x2() {
    RTexture t;
    t.rgba = g_tex2x2;
    t.w = 2; t.h = 2;
    t.nearest = 1;
    t.valid = 1;
    t.format = RTEX_RGBA8888;
    t.opaque = 1;   // all four texels have alpha 255
    return t;
}

// Build a fresh 16x16 surface cleared to black opaque.
static RSurface make_surface(uint8_t *buf, int w, int h) {
    RSurface s; s.rgba = buf; s.w = w; s.h = h;
    Blitter_ClearSurface(&s, 0, 0, 0, 255);
    return s;
}

// Emit a fullscreen quad (covering [0,w] x [0,h]) as two triangles with the
// given uv corners + a constant vertex colour, into the supplied surface.
static void draw_fullscreen_quad(RSurface &s, const RTexture *tex, RBlend blend,
                                 float alphaRef,
                                 float r, float g, float b, float a) {
    float W = (float)s.w, H = (float)s.h;
    // Corners: TL(0,0) uv(0,0), TR(W,0) uv(1,0), BR(W,H) uv(1,1), BL(0,H) uv(0,1)
    BVtx TL{ 0, 0, 0, 0, r, g, b, a };
    BVtx TR{ W, 0, 1, 0, r, g, b, a };
    BVtx BR{ W, H, 1, 1, r, g, b, a };
    BVtx BL{ 0, H, 0, 1, r, g, b, a };
    BVtx t0[3] = { TL, TR, BR };
    BVtx t1[3] = { TL, BR, BL };
    Blitter_RasterTri(&s, t0, tex, blend, alphaRef);
    Blitter_RasterTri(&s, t1, tex, blend, alphaRef);
}

// ---- Case 1: textured fullscreen quad, four quadrant centres == four texels --
static void test_textured_quad() {
    uint8_t buf[16 * 16 * 4];
    RSurface s = make_surface(buf, 16, 16);
    RTexture tex = make_tex2x2();

    // White vertex colour, opaque copy.
    draw_fullscreen_quad(s, &tex, RB_NONE, -1.0f, 1, 1, 1, 1);

    // Quadrant centres: top-left ~(4,4), top-right ~(12,4),
    // bottom-left ~(4,12), bottom-right ~(12,12).
    bool ok = true;
    ok &= px_eq(s,  4,  4, 255,   0,   0, 255);  // red
    ok &= px_eq(s, 12,  4,   0, 255,   0, 255);  // green
    ok &= px_eq(s,  4, 12,   0,   0, 255, 255);  // blue
    ok &= px_eq(s, 12, 12, 255, 255, 255, 255);  // white
    report("textured quad: quadrant centres match texels", ok);
}

// ---- Case 1b: same quad sampled from a packed RGBA4444 texture --------------
// Colours are nibble-aligned (0x00 / 0xFF) so the 8->4->8 round-trip is exact;
// this checks the packed-format sampler decode (GMLOADER_BLITTER_TEX16 path).
static void test_textured_quad_rgba4444() {
    // Pack the same 2x2 fixture as RGBA4444: (R<<12)|(G<<8)|(B<<4)|A, top nibble.
    uint16_t tex16[4];
    for (int i = 0; i < 4; ++i) {
        const uint8_t *s = g_tex2x2 + i * 4;
        tex16[i] = (uint16_t)(((s[0] >> 4) << 12) | ((s[1] >> 4) << 8) |
                              ((s[2] >> 4) << 4)  |  (s[3] >> 4));
    }
    RTexture tex;
    tex.rgba = (const uint8_t *)tex16;
    tex.w = 2; tex.h = 2; tex.nearest = 1; tex.valid = 1;
    tex.format = RTEX_RGBA4444;
    tex.opaque = 1;

    uint8_t buf[16 * 16 * 4];
    RSurface s = make_surface(buf, 16, 16);
    draw_fullscreen_quad(s, &tex, RB_NONE, -1.0f, 1, 1, 1, 1);

    bool ok = true;
    ok &= px_eq(s,  4,  4, 255,   0,   0, 255);  // red
    ok &= px_eq(s, 12,  4,   0, 255,   0, 255);  // green
    ok &= px_eq(s,  4, 12,   0,   0, 255, 255);  // blue
    ok &= px_eq(s, 12, 12, 255, 255, 255, 255);  // white
    report("RGBA4444 quad: packed-texel decode matches RGBA8888", ok);
}

// ---- Case 2: RB_ALPHA blend over a known background -------------------------
static void test_alpha_blend() {
    uint8_t buf[16 * 16 * 4];
    RSurface s; s.rgba = buf; s.w = 16; s.h = 16;
    // Background: solid blue (0,0,255,255).
    Blitter_ClearSurface(&s, 0, 0, 255, 255);

    // 50%-alpha solid red, no texture, alpha blend.
    //   out.rgb = src.rgb*0.5 + dst.rgb*0.5
    //   red over blue -> (127, 0, 127); out.a = 0.5 + 1*0.5 = 1.0 -> 255
    draw_fullscreen_quad(s, nullptr, RB_ALPHA, -1.0f, 1.0f, 0.0f, 0.0f, 0.5f);

    bool ok = px_near(s, 8, 8, 128, 0, 128, 255, 1);
    report("RB_ALPHA: 50% red over blue == ~(128,0,128,255)", ok);
}

// ---- Case 3: alpha-test discard leaves destination unchanged ----------------
static void test_alpha_test_discard() {
    uint8_t buf[16 * 16 * 4];
    RSurface s; s.rgba = buf; s.w = 16; s.h = 16;
    Blitter_ClearSurface(&s, 10, 20, 30, 40);

    // frag.a = 0.25; alphaRef = 0.5 -> frag.a <= ref -> every pixel discarded.
    draw_fullscreen_quad(s, nullptr, RB_ALPHA, 0.5f, 1.0f, 1.0f, 1.0f, 0.25f);

    bool ok = true;
    ok &= px_eq(s, 0, 0, 10, 20, 30, 40);
    ok &= px_eq(s, 8, 8, 10, 20, 30, 40);
    ok &= px_eq(s, 15, 15, 10, 20, 30, 40);
    report("alpha test: low-alpha frag discarded, dst unchanged", ok);
}

// ---- Case 4: clipping — triangle extending past bounds writes only in-bounds -
static void test_clipping() {
    uint8_t buf[16 * 16 * 4];
    RSurface s = make_surface(buf, 16, 16);   // black opaque

    // Big triangle whose verts sit well outside the surface on all sides; it
    // fully covers the 16x16 area. Solid green, no texture, opaque.
    // With ASan this also catches any out-of-bounds write.
    BVtx t[3] = {
        { -100.0f,  -50.0f, 0, 0, 0, 1, 0, 1 },
        {  200.0f,  -50.0f, 0, 0, 0, 1, 0, 1 },
        {   50.0f,  300.0f, 0, 0, 0, 1, 0, 1 },
    };
    Blitter_RasterTri(&s, t, nullptr, RB_NONE, -1.0f);

    // Every in-bounds pixel should be green (the tri covers the whole surface).
    bool ok = true;
    for (int y = 0; y < s.h && ok; ++y)
        for (int x = 0; x < s.w && ok; ++x)
            ok &= px_eq(s, x, y, 0, 255, 0, 255);
    report("clipping: oversized tri fills surface, no OOB write", ok);
}

// ---- Case 5: robustness — degenerate / non-finite triangles are no-ops ------
static void test_degenerate() {
    uint8_t buf[8 * 8 * 4];
    RSurface s; s.rgba = buf; s.w = 8; s.h = 8;
    Blitter_ClearSurface(&s, 7, 7, 7, 7);

    // Zero-area (collinear) triangle.
    BVtx zeroArea[3] = {
        { 0, 0, 0, 0, 1, 1, 1, 1 },
        { 4, 4, 0, 0, 1, 1, 1, 1 },
        { 8, 8, 0, 0, 1, 1, 1, 1 },
    };
    Blitter_RasterTri(&s, zeroArea, nullptr, RB_NONE, -1.0f);

    // NaN vertex.
    float nan = nanf("");
    BVtx bad[3] = {
        { nan, 0, 0, 0, 1, 1, 1, 1 },
        { 8,   0, 0, 0, 1, 1, 1, 1 },
        { 4,   8, 0, 0, 1, 1, 1, 1 },
    };
    Blitter_RasterTri(&s, bad, nullptr, RB_NONE, -1.0f);

    bool ok = true;
    for (int y = 0; y < s.h && ok; ++y)
        for (int x = 0; x < s.w && ok; ++x)
            ok &= px_eq(s, x, y, 7, 7, 7, 7);
    report("robustness: zero-area + NaN tris are no-ops", ok);
}

// ---- Case 7: opaque fast-path equivalence -----------------------------------
// The blitter's opaque fast-path (blitter.cpp) downgrades an RB_ALPHA draw to
// RB_NONE when the source is provably opaque (all texel alpha==255 AND all vertex
// alpha==1). This proves the downgrade is pixel-identical: an opaque source drawn
// under RB_ALPHA must match the same draw forced to RB_NONE.
static void test_opaque_fastpath_equiv() {
    uint8_t bufA[16 * 16 * 4], bufN[16 * 16 * 4];
    // Non-trivial background so a wrong blend (reading dst) would diverge.
    RSurface sA; sA.rgba = bufA; sA.w = 16; sA.h = 16;
    RSurface sN; sN.rgba = bufN; sN.w = 16; sN.h = 16;
    Blitter_ClearSurface(&sA, 30, 60, 90, 120);
    Blitter_ClearSurface(&sN, 30, 60, 90, 120);

    RTexture tex = make_tex2x2();   // opaque (all alpha 255)

    // Opaque vertex colour (a==1). RB_ALPHA over the bg, vs RB_NONE plain copy.
    draw_fullscreen_quad(sA, &tex, RB_ALPHA, -1.0f, 1, 1, 1, 1);
    draw_fullscreen_quad(sN, &tex, RB_NONE,  -1.0f, 1, 1, 1, 1);

    bool ok = true;
    for (int y = 0; y < 16 && ok; ++y)
        for (int x = 0; x < 16 && ok; ++x) {
            const uint8_t *a = px(sA, x, y), *n = px(sN, x, y);
            ok &= a[0]==n[0] && a[1]==n[1] && a[2]==n[2] && a[3]==n[3];
        }
    report("opaque fast-path: RB_ALPHA opaque source == RB_NONE", ok);
}

int main() {
    test_textured_quad();
    test_textured_quad_rgba4444();
    test_alpha_blend();
    test_alpha_test_discard();
    test_clipping();
    test_degenerate();
    test_opaque_fastpath_equiv();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
