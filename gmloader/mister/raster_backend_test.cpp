// Host unit test for the RasterBackend seam (Task 3), the backend_mfgpu skeleton
// (Task 4), and the real fabric TRILIST draw (Task 5).
//
// Task 3's case proves the refactor is pixel-neutral: driving a small triangle
// through backend_sw->draw() must produce a byte-identical RSurface to calling
// Blitter_RasterDraw directly.
//
// Task 4's case proves backend_mfgpu->clear, executed on the host through the
// SAME blt_execute software model the mfgpu refmodel unit tests use, matches
// backend_sw->clear in present-space RGB565.
//
// Task 5's battery proves backend_mfgpu->draw emits a BLT_OP_TRILIST that, run
// through blt_execute into an RGB565 framebuffer, matches backend_sw->draw
// (RGBA8888 -> RGB565) within ±1 LSB per 5/6/5 channel. It is NOT bit-exact by
// design: the SW rasterizer blends in 8-bit then truncates to 565 while the
// fabric blends natively in 565, so a small rounding divergence is expected and
// tolerated. (Bit-exactness lives inside mfgpu: refmodel <-> RTL.) All battery
// triangles use INTEGER vertex coordinates so the two rasterizers' coverage is
// identical (lround(x*16) == the SW float edge functions), isolating the
// comparison to blend/interpolation rounding.
#include "raster_backend.h"
#include "blitter_raster.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

// backend_mfgpu (Task 4/5) + its host-only hooks. Declared here rather than via
// raster_backend.h because these are not part of the vtable seam — they exist
// solely so this host test can read the fixed-size fabric framebuffer
// blt_execute composited into, and steer the FBO-fallback decision.
extern "C" const RasterBackend backend_sw;
extern "C" const RasterBackend backend_mfgpu;
extern "C" void RasterBackend_MFGPU_TestCopyFB565(int w, int h, uint16_t *out);
extern "C" void RasterBackend_MFGPU_SetDefaultSurface(const uint8_t *rgba);

// Mirrors Blitter_ToRGB565's per-pixel packing formula (gmloader/mister/
// blitter.cpp) without linking that file: it lives inside #ifdef
// MISTER_NATIVE_VIDEO and depends on GL-decode globals (g_rw/g_rh) plus the
// fixed MISTER_WIDTH/MISTER_HEIGHT letterbox macros, none of which apply to
// this same-size host comparison. `fb565` must be a tightly-packed
// sw->w * sw->h buffer (as RasterBackend_MFGPU_TestCopyFB565 produces).
static int rgb565_within1(const RSurface *sw, const uint16_t *fb565) {
    for (int y = 0; y < sw->h; y++) {
        for (int x = 0; x < sw->w; x++) {
            const uint8_t *p = sw->rgba + ((size_t)y * sw->w + x) * 4;
            int r = p[0] >> 3, g = p[1] >> 2, b = p[2] >> 3;
            uint16_t px = fb565[(size_t)y * sw->w + x];
            int fr = (px >> 11) & 0x1F, fg = (px >> 5) & 0x3F, fb = px & 0x1F;
            if (abs(r - fr) > 1 || abs(g - fg) > 1 || abs(b - fb) > 1) return 0;
        }
    }
    return 1;
}

// Same tolerance as rgb565_within1, but reports the worst per-channel delta and
// dumps the first differing pixel (>1) to aid debugging. Returns 1 if all
// pixels are within ±1.
static int compare565(const char *name, const RSurface *sw, const uint16_t *fb565) {
    int maxd = 0, fx = -1, fy = -1;
    uint16_t f_sw = 0, f_fb = 0;
    for (int y = 0; y < sw->h; y++) {
        for (int x = 0; x < sw->w; x++) {
            const uint8_t *p = sw->rgba + ((size_t)y * sw->w + x) * 4;
            int r = p[0] >> 3, g = p[1] >> 2, b = p[2] >> 3;
            uint16_t px = fb565[(size_t)y * sw->w + x];
            int fr = (px >> 11) & 0x1F, fg = (px >> 5) & 0x3F, fb = px & 0x1F;
            int dr = abs(r - fr), dg = abs(g - fg), db = abs(b - fb);
            int d = dr > dg ? dr : dg; if (db > d) d = db;
            if (d > maxd) maxd = d;
            if (d > 1 && fx < 0) {
                fx = x; fy = y;
                f_sw = (uint16_t)((r << 11) | (g << 5) | b);
                f_fb = px;
            }
        }
    }
    if (fx >= 0) {
        printf("  FAIL %-16s first diff @ (%d,%d): sw565=0x%04X fb565=0x%04X (maxΔ=%d)\n",
               name, fx, fy, f_sw, f_fb, maxd);
        return 0;
    }
    printf("  OK   %-16s maxΔ=%d\n", name, maxd);
    return 1;
}

// ---- Task 3: backend_sw->draw is byte-identical to Blitter_RasterDraw --------
static int one_case(void) {
    enum { W = 32, H = 32 };
    uint8_t a[W*H*4], b[W*H*4];
    RSurface sa = { a, W, H }, sb = { b, W, H };
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    static const uint8_t tex[4] = { 200, 100, 50, 255 };
    RTexture t = { tex, 1, 1, /*nearest*/1, /*valid*/1, /*format RTEX_RGBA8888*/0, /*opaque*/1 };
    BVtx v[3] = {
        { 2.f, 2.f, 0.f, 0.f, 1,1,1,1 },
        { 28.f, 4.f, 1.f, 0.f, 1,1,1,1 },
        { 4.f, 28.f, 0.f, 1.f, 1,1,1,1 },
    };
    Blitter_RasterDraw(&sa, v, 1, &t, RB_NONE, 0.f, 1);          /* reference */
    RasterBackend_Select()->draw(&sb, v, 1, &t, RB_NONE, 0.f);   /* through seam */
    return memcmp(a, b, sizeof a) == 0;
}

// ---- Task 4: backend_mfgpu->clear parity in present-space RGB565 -------------
static int case_clear_parity(void) {
    enum { W = 288, H = 216 };
    static uint8_t rgba_sw[W*H*4], rgba_mf[W*H*4];
    static uint16_t mf565[W*H];
    // Pre-fill both surfaces with garbage so a no-op clear would fail the check.
    memset(rgba_sw, 0xAA, sizeof rgba_sw);
    memset(rgba_mf, 0x55, sizeof rgba_mf);
    RSurface s_sw = { rgba_sw, W, H };
    RSurface s_mf = { rgba_mf, W, H };

    backend_sw.clear(&s_sw, 30, 60, 90, 255);

    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s_mf, 30, 60, 90, 255);
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(W, H, mf565);

    return rgb565_within1(&s_sw, mf565);
}

// ---- Task 5: TRILIST draw battery vs the SW oracle (±1 LSB 565) --------------
enum { BW = 288, BH = 216 };

// Render one case both ways into a BW x BH target and compare in RGB565.
//   SW    : backend_sw.clear(bg) + backend_sw.draw -> RGBA8888
//   fabric: backend_mfgpu {frame_begin, clear(bg), draw, frame_end} -> RGB565
static int battery_case(const char *name, uint8_t br, uint8_t bg, uint8_t bb,
                        const RTexture *tex, const BVtx *v, int triCount, RBlend blend) {
    static uint8_t  rgba_sw[BW*BH*4];
    static uint8_t  rgba_mf[BW*BH*4];   // identity handle for the default-fb check
    static uint16_t mf565[BW*BH];
    RSurface s_sw = { rgba_sw, BW, BH };
    RSurface s_mf = { rgba_mf, BW, BH };

    backend_sw.clear(&s_sw, br, bg, bb, 255);
    backend_sw.draw(&s_sw, v, triCount, tex, blend, 0.f);

    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);   // this surface IS the default fb
    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s_mf, br, bg, bb, 255);
    backend_mfgpu.draw(&s_mf, v, triCount, tex, blend, 0.f);
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);

    return compare565(name, &s_sw, mf565);
}

static int battery(void) {
    printf("TRILIST battery (fabric vs SW oracle, ±1 LSB 565):\n");
    int ok = 1;

    // Untextured sampler returns opaque white; the 1x1-white fabric page matches.
    RTexture untex = { nullptr, 0, 0, 1, /*valid*/0, 0, 1 };
    // 1x1 opaque RGBA8888 texel.
    static const uint8_t px1[4] = { 200, 100, 50, 255 };
    RTexture tex1 = { px1, 1, 1, 1, 1, /*RTEX_RGBA8888*/0, 1 };
    // 4x4 opaque grayscale gradient. A nearest-texel boundary mispick between the
    // two rasterizers is at worst a diagonal neighbour (x+y differs by 2); the
    // step of 2/255 keeps that within one 6-bit green LSB (Δ4/255 -> Δ1), so the
    // case still exercises N×M addressing without a fixture-induced >±1 miss.
    static uint8_t px16[4*4*4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t g = (uint8_t)(120 + (x + y) * 2);
            uint8_t *p = px16 + (y*4 + x) * 4;
            p[0] = g; p[1] = g; p[2] = g; p[3] = 255;
        }
    RTexture tex4 = { px16, 4, 4, 1, 1, 0, 1 };

    // 1) opaque tri — flat-tinted white passthrough, coverage over a bg.
    {
        BVtx v[3] = {
            {  16.f, 16.f, 0,0, 0.80f,0.40f,0.20f,1 },
            { 240.f, 32.f, 0,0, 0.80f,0.40f,0.20f,1 },
            {  48.f,180.f, 0,0, 0.80f,0.40f,0.20f,1 },
        };
        ok &= battery_case("opaque-tri", 20,30,40, &untex, v, 1, RB_NONE);
    }
    // 2) alpha tri — 1x1 opaque texel, vertex alpha 0.5 -> CONST_ALPHA over bg.
    {
        BVtx v[3] = {
            {  40.f, 24.f, 0.5f,0.5f, 1,1,1,0.5f },
            { 220.f, 40.f, 0.5f,0.5f, 1,1,1,0.5f },
            {  60.f,170.f, 0.5f,0.5f, 1,1,1,0.5f },
        };
        ok &= battery_case("alpha-tri", 80,20,60, &tex1, v, 1, RB_ALPHA);
    }
    // 3) additive tri — full alpha (so SW's src*a == src) -> saturating ADD.
    {
        BVtx v[3] = {
            {  32.f, 32.f, 0,0, 0.50f,0.35f,0.25f,1 },
            { 200.f, 40.f, 0,0, 0.50f,0.35f,0.25f,1 },
            {  70.f,160.f, 0,0, 0.50f,0.35f,0.25f,1 },
        };
        ok &= battery_case("additive-tri", 40,40,40, &untex, v, 1, RB_ADD);
    }
    // 4) multi-tri quad — two triangles sharing a diagonal (top-left rule).
    {
        BVtx v[6] = {
            {  40.f, 40.f, 0,0, 0.30f,0.60f,0.90f,1 },
            { 200.f, 40.f, 0,0, 0.30f,0.60f,0.90f,1 },
            { 200.f,150.f, 0,0, 0.30f,0.60f,0.90f,1 },
            {  40.f, 40.f, 0,0, 0.30f,0.60f,0.90f,1 },
            { 200.f,150.f, 0,0, 0.30f,0.60f,0.90f,1 },
            {  40.f,150.f, 0,0, 0.30f,0.60f,0.90f,1 },
        };
        ok &= battery_case("multi-tri-quad", 10,10,10, &tex1, v, 2, RB_NONE);
    }
    // 5) non-axis-aligned tri — slanted integer-vertex triangle.
    {
        BVtx v[3] = {
            {  50.f, 20.f, 0,0, 0.90f,0.90f,0.20f,1 },
            { 240.f,110.f, 0,0, 0.90f,0.90f,0.20f,1 },
            {  90.f,190.f, 0,0, 0.90f,0.90f,0.20f,1 },
        };
        ok &= battery_case("nonaa-tri", 15,25,35, &untex, v, 1, RB_NONE);
    }
    // 6) 1x1 texture — explicit opaque single-texel modulate.
    {
        BVtx v[3] = {
            {  24.f, 24.f, 0,0, 1,1,1,1 },
            { 180.f, 36.f, 0,0, 1,1,1,1 },
            {  36.f,150.f, 0,0, 1,1,1,1 },
        };
        ok &= battery_case("tex-1x1", 0,0,0, &tex1, v, 1, RB_NONE);
    }
    // 7) N×M texture — 4x4 gradient mapped 0..1 across a quad (nearest sample).
    {
        BVtx v[6] = {
            {  48.f, 48.f, 0.f,0.f, 1,1,1,1 },
            { 176.f, 48.f, 1.f,0.f, 1,1,1,1 },
            { 176.f,160.f, 1.f,1.f, 1,1,1,1 },
            {  48.f, 48.f, 0.f,0.f, 1,1,1,1 },
            { 176.f,160.f, 1.f,1.f, 1,1,1,1 },
            {  48.f,160.f, 0.f,1.f, 1,1,1,1 },
        };
        ok &= battery_case("tex-NxM", 5,5,5, &tex4, v, 2, RB_NONE);
    }

    // 8) FBO fallback — a non-default target must delegate to backend_sw (writes
    //    the dst surface directly, byte-identical to the reference rasterizer).
    {
        enum { W = 64, H = 64 };
        static uint8_t ref[W*H*4], fbo[W*H*4];
        RSurface s_ref = { ref, W, H }, s_fbo = { fbo, W, H };
        memset(ref, 0, sizeof ref); memset(fbo, 0, sizeof fbo);
        BVtx v[3] = {
            {  8.f,  8.f, 0,0, 0.5f,0.7f,0.9f,1 },
            { 56.f, 12.f, 0,0, 0.5f,0.7f,0.9f,1 },
            { 12.f, 56.f, 0,0, 0.5f,0.7f,0.9f,1 },
        };
        Blitter_RasterDraw(&s_ref, v, 1, &untex, RB_NONE, 0.f, 1);
        // A default surface that is NOT this fbo -> mf_draw takes the SW fallback.
        static uint8_t other; RasterBackend_MFGPU_SetDefaultSurface(&other);
        backend_mfgpu.frame_begin();
        backend_mfgpu.draw(&s_fbo, v, 1, &untex, RB_NONE, 0.f);
        backend_mfgpu.frame_end();
        if (memcmp(ref, fbo, sizeof ref) == 0) printf("  OK   %-16s (SW-identical)\n", "fbo-fallback");
        else { printf("  FAIL %-16s FBO target not routed to SW\n", "fbo-fallback"); ok = 0; }
    }

    return ok;
}

int main(void){
    int ok = 1;
    if (!one_case()) { printf("FAIL sw-equivalence\n"); ok = 0; }
    else printf("raster_backend sw-equivalence OK\n");
    if (!case_clear_parity()) { printf("FAIL mfgpu-clear-parity\n"); ok = 0; }
    else printf("raster_backend mfgpu-clear-parity OK\n");
    if (!battery()) { printf("FAIL mfgpu-trilist-battery\n"); ok = 0; }
    else printf("raster_backend mfgpu-trilist-battery OK\n");
    return ok ? 0 : 1;
}
