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
// Task 2: cache introspection/reset hooks (host-test-only, not part of the vtable).
extern "C" uint32_t RasterBackend_MFGPU_TestUploadCount(void);
extern "C" void RasterBackend_MFGPU_TestReinit(uint32_t tex_heap_bytes);

extern "C" void RasterBackend_MFGPU_InvalidateTex(uint32_t id);

// Task 1: tiny monotonic key so each battery case gets a distinct tex_key —
// no cross-case cache collision once Task 2 adds caching (tex_key is inert
// this task, but every draw call site now threads one through).
static uint32_t next_key(void){ static uint32_t k = 1; return k++; }

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
    RasterBackend_Select()->draw(&sb, v, 1, &t, RB_NONE, 0.f, next_key());   /* through seam */
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
static int battery_case_key(const char *name, uint8_t br, uint8_t bg, uint8_t bb,
                            const RTexture *tex, const BVtx *v, int triCount,
                            RBlend blend, uint32_t key) {
    static uint8_t  rgba_sw[BW*BH*4];
    static uint8_t  rgba_mf[BW*BH*4];   // identity handle for the default-fb check
    static uint16_t mf565[BW*BH];
    RSurface s_sw = { rgba_sw, BW, BH };
    RSurface s_mf = { rgba_mf, BW, BH };

    backend_sw.clear(&s_sw, br, bg, bb, 255);
    backend_sw.draw(&s_sw, v, triCount, tex, blend, 0.f, key);

    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);   // this surface IS the default fb
    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s_mf, br, bg, bb, 255);
    backend_mfgpu.draw(&s_mf, v, triCount, tex, blend, 0.f, key);
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);

    return compare565(name, &s_sw, mf565);
}
static int battery_case(const char *name, uint8_t br, uint8_t bg, uint8_t bb,
                        const RTexture *tex, const BVtx *v, int triCount, RBlend blend) {
    return battery_case_key(name, br, bg, bb, tex, v, triCount, blend, next_key());
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

    // Task 6: hard-edged (alpha in {0,255}) textures for the colorkey battery.
    // 8x8 with a 1-texel transparent border (interior opaque).
    static uint8_t pxBorder[8*8*4];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            uint8_t *p = pxBorder + (y*8 + x) * 4;
            bool border = (x == 0 || x == 7 || y == 0 || y == 7);
            if (border) { p[0]=0;   p[1]=0;   p[2]=0;   p[3]=0;   }
            else        { p[0]=180; p[1]=90;  p[2]=40;  p[3]=255; }
        }
    RTexture texBorder = { pxBorder, 8, 8, 1, 1, /*RTEX_RGBA8888*/0, /*opaque*/0 };

    // 4x4 checkerboard alpha (opaque texels on (x+y) even). Cell (0,0) is pure
    // magenta (255,0,255) -> converts exactly to MF_COLORKEY (0xF81F), so this
    // case also exercises the opaque/colorkey-collision nudge in the staging.
    static uint8_t pxCheck[4*4*4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t *p = pxCheck + (y*4 + x) * 4;
            bool opaque = ((x + y) % 2) == 0;
            if (!opaque)          { p[0]=0;   p[1]=0;   p[2]=0;   p[3]=0;   }
            else if (x==0 && y==0){ p[0]=255; p[1]=0;   p[2]=255; p[3]=255; }
            else                  { p[0]=90;  p[1]=140; p[2]=200; p[3]=255; }
        }
    RTexture texCheck = { pxCheck, 4, 4, 1, 1, /*RTEX_RGBA8888*/0, /*opaque*/0 };

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

    // 8/9) keyed textures — hard-edged cutout (alpha in {0,255}); vertex alpha
    //    is fully opaque so these must take the BLT_BLEND_COLORKEY path.
    //
    //    Each texel is drawn as its own flat-shaded screen cell with a UV
    //    CONSTANT across all 6 vertices of its 2 triangles (not interpolated
    //    corner-to-corner). This sidesteps a pre-existing, already-documented
    //    divergence between the two rasterizers' nearest-sample rounding (the
    //    golden fabric rounds-to-nearest-texel via a +HALF bias in
    //    tex_nearest(), the SW oracle floors — see the tex-NxM case's "diagonal
    //    neighbour" note above): for a texture interpolated smoothly across a
    //    quad that's a sub-LSB rounding difference, but for a *binary* alpha
    //    edge it would flip a whole texel's classification (opaque vs keyed)
    //    over roughly half of that texel's screen footprint. u=(tx+0.25)/tw
    //    sits inside both conventions' agreement window [tx,tx+0.5) for any
    //    texel index tx, so every quad deterministically samples the texel
    //    it's meant to, on both rasterizers.
    auto keyed_case = [&](const char *name, const RTexture &tex, int tw, int th, int cell) {
        static BVtx v[64*6];
        int n = 0;
        for (int ty = 0; ty < th; ty++) {
            for (int tx = 0; tx < tw; tx++) {
                float u = (tx + 0.25f) / (float)tw, uv = (ty + 0.25f) / (float)th;
                float x0 = 40.f + tx*cell, y0 = 40.f + ty*cell;
                float x1 = x0 + cell,       y1 = y0 + cell;
                BVtx q[6] = {
                    { x0,y0, u,uv, 1,1,1,1 }, { x1,y0, u,uv, 1,1,1,1 }, { x1,y1, u,uv, 1,1,1,1 },
                    { x0,y0, u,uv, 1,1,1,1 }, { x1,y1, u,uv, 1,1,1,1 }, { x0,y1, u,uv, 1,1,1,1 },
                };
                for (int i = 0; i < 6; i++) v[n++] = q[i];
            }
        }
        ok &= battery_case(name, 5,5,5, &tex, v, n/3, RB_NONE);
    };
    keyed_case("keyed-border",  texBorder, 8, 8, 4);   // 1-texel transparent border
    keyed_case("keyed-checker", texCheck,  4, 4, 8);   // checkerboard alpha + nudge

    // RGBA4444 coverage (Task 7): same 1-texel transparent border shape as
    // texBorder, but staged in the packed RGBA4444 layout the production GL
    // decode path uses (uint16 texel R4<<12|G4<<8|B4<<4|A4) -- alpha strictly
    // {0,15} (fully transparent vs fully opaque in 4-bit). Exercises
    // mf_texel565's RTEX_RGBA4444 branch (raster_backend_mfgpu.cpp) and the
    // SW rasterizer's fmt16 path (blitter_raster.cpp), both untested until now.
    static uint16_t pxKeyed4444[8*8];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            bool border = (x == 0 || x == 7 || y == 0 || y == 7);
            unsigned r4 = 0xB, g4 = 0x5, b4 = 0x3, a4 = border ? 0x0 : 0xF;
            pxKeyed4444[y*8 + x] = (uint16_t)((r4 << 12) | (g4 << 8) | (b4 << 4) | a4);
        }
    RTexture texKeyed4444 = { (const uint8_t *)pxKeyed4444, 8, 8, 1, 1, /*RTEX_RGBA4444*/1, /*opaque*/0 };
    keyed_case("keyed-4444-border", texKeyed4444, 8, 8, 4);   // RGBA4444 1-bit-alpha border

    // 10) FBO fallback — a non-default target must delegate to backend_sw (writes
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
        backend_mfgpu.draw(&s_fbo, v, 1, &untex, RB_NONE, 0.f, next_key());
        backend_mfgpu.frame_end();
        if (memcmp(ref, fbo, sizeof ref) == 0) printf("  OK   %-16s (SW-identical)\n", "fbo-fallback");
        else { printf("  FAIL %-16s FBO target not routed to SW\n", "fbo-fallback"); ok = 0; }
    }

    return ok;
}

// ---- Task 2: persistent identity-keyed texture cache -------------------------

// (A) A 256x256 opaque page is well within the new 32MB persistent texture heap
// (the old ~500KB/frame scratch heap would have dropped it) and must still match
// the SW oracle within +/-1 LSB 565.
static int case_large_page(void) {
    enum { TW = 256, TH = 256 };
    static uint8_t tex[TW*TH*4];
    for (int y=0;y<TH;y++) for (int x=0;x<TW;x++){ uint8_t*p=tex+((y*TW+x)*4); p[0]=(uint8_t)x; p[1]=(uint8_t)y; p[2]=128; p[3]=255; }
    RTexture t = { tex, TW, TH, 1, 1, /*RGBA8888*/0, 1 };
    BVtx v[6] = {
        {  0.f,  0.f, 0,0, 1,1,1,1 }, { 240.f,  0.f, 1,0, 1,1,1,1 }, {  0.f,180.f, 0,1, 1,1,1,1 },
        { 240.f,  0.f, 1,0, 1,1,1,1 }, { 240.f,180.f, 1,1, 1,1,1,1 }, {  0.f,180.f, 0,1, 1,1,1,1 },
    };
    RasterBackend_MFGPU_TestReinit(0);
    return battery_case_key("large_page", 0,0,0, &t, v, 2, RB_NONE, next_key());  // ±1 LSB
}

// (B) Cache hit = single upload: the same key drawn across two frames must upload
// exactly once. Needs direct frame control (two frames, one key), so it sets up
// its own default surface exactly as battery_case_key does.
static int case_cache_hit(void) {
    static const uint8_t px[4] = { 200,100,50,255 };
    RTexture t = { px, 1, 1, 1, 1, 0, 1 };
    BVtx v[3] = { {2,2,0,0,1,1,1,1}, {28,4,1,0,1,1,1,1}, {4,28,0,1,1,1,1,1} };
    static uint8_t rgba_mf[BW*BH*4];
    RSurface s_mf = { rgba_mf, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    RasterBackend_MFGPU_TestReinit(0);
    const uint32_t K = 4242;
    for (int f=0; f<2; f++) {                 // two frames, same key
        backend_mfgpu.frame_begin();
        backend_mfgpu.clear(&s_mf, 0,0,0,255);
        backend_mfgpu.draw(&s_mf, v, 1, &t, RB_NONE, 0.f, K);
        backend_mfgpu.frame_end();
    }
    return RasterBackend_MFGPU_TestUploadCount() == 1;   // uploaded once, reused once
}


static int case_invalidate(void) {
    static uint8_t px[4] = { 10,20,30,255 };
    RTexture t = { px, 1, 1, 1, 1, 0, 1 };
    BVtx v[3] = { {2,2,0,0,1,1,1,1}, {28,4,1,0,1,1,1,1}, {4,28,0,1,1,1,1,1} };
    static uint8_t rgba_mf[BW*BH*4];
    RSurface s_mf = { rgba_mf, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    RasterBackend_MFGPU_TestReinit(0);
    const uint32_t K = 77;
    // frame 1: stage (upload #1)
    backend_mfgpu.frame_begin(); backend_mfgpu.draw(&s_mf, v, 1, &t, RB_NONE, 0.f, K); backend_mfgpu.frame_end();
    // invalidate + change pixels, frame 2: must re-upload (upload #2)
    RasterBackend_MFGPU_InvalidateTex(K);
    px[0] = 250;
    backend_mfgpu.frame_begin(); backend_mfgpu.draw(&s_mf, v, 1, &t, RB_NONE, 0.f, K); backend_mfgpu.frame_end();
    return RasterBackend_MFGPU_TestUploadCount() == 2;
}

// ---- Task 4: LRU eviction under heap pressure --------------------------------
// Build a WxH opaque texture whose colour encodes `tag` so re-uploads are
// distinguishable, draw it full-quad, and compare fabric vs SW oracle.
static int draw_tagged(uint32_t key, int W, int H, uint8_t tag) {
    static uint8_t buf[512*512*4];
    for (int i=0;i<W*H;i++){ buf[i*4]=tag; buf[i*4+1]=tag; buf[i*4+2]=tag; buf[i*4+3]=255; }
    RTexture t = { buf, W, H, 1, 1, 0, 1 };
    BVtx v[6] = {
        {  0.f,  0.f, 0,0, 1,1,1,1 }, { 200.f,  0.f, 1,0, 1,1,1,1 }, {  0.f,150.f, 0,1, 1,1,1,1 },
        { 200.f,  0.f, 1,0, 1,1,1,1 }, { 200.f,150.f, 1,1, 1,1,1,1 }, {  0.f,150.f, 0,1, 1,1,1,1 },
    };
    return battery_case_key("tagged", 0,0,0, &t, v, 2, RB_NONE, key);   // Task 1 helper, key-aware
}

static int case_two_keys(void) {                 // both resident, both correct
    RasterBackend_MFGPU_TestReinit(0);
    if (!draw_tagged(1, 64, 64, 40)) return 0;
    if (!draw_tagged(2, 64, 64, 200)) return 0;
    // redraw key 1 — must be a cache hit (no third upload)
    if (!draw_tagged(1, 64, 64, 40)) return 0;
    return RasterBackend_MFGPU_TestUploadCount() == 2;
}

static int case_eviction(void) {                       // cap holds 2x 512KB pages (256KB slack)
    RasterBackend_MFGPU_TestReinit(1280u*1024);        // 1.25MB texture heap
    if (!draw_tagged(1, 512, 512, 10)) return 0;       // upload1 (free 768KB)
    if (!draw_tagged(2, 512, 512, 20)) return 0;       // upload2 (free 256KB)
    if (!draw_tagged(3, 512, 512, 30)) return 0;       // evicts key1, upload3 -> {2,3}
    if (!draw_tagged(4, 512, 512, 40)) return 0;       // evicts key2, upload4 -> {3,4}
    if (!draw_tagged(1, 512, 512, 10)) return 0;       // key1 gone -> evicts key3, upload5
    return RasterBackend_MFGPU_TestUploadCount() == 5; // 4 initial + 1 re-stage; each pixel-correct
}

int main(void){
    int ok = 1;
    if (!one_case()) { printf("FAIL sw-equivalence\n"); ok = 0; }
    else printf("raster_backend sw-equivalence OK\n");
    if (!case_clear_parity()) { printf("FAIL mfgpu-clear-parity\n"); ok = 0; }
    else printf("raster_backend mfgpu-clear-parity OK\n");
    if (!battery()) { printf("FAIL mfgpu-trilist-battery\n"); ok = 0; }
    else printf("raster_backend mfgpu-trilist-battery OK\n");
    if (!case_large_page()) { printf("FAIL mfgpu-cache-large-page\n"); ok = 0; }
    else printf("raster_backend mfgpu-cache-large-page OK\n");
    if (!case_cache_hit()) { printf("FAIL mfgpu-cache-hit\n"); ok = 0; }
    else printf("raster_backend mfgpu-cache-hit OK\n");
    if (!case_invalidate()) { printf("FAIL mfgpu-invalidate\n"); ok = 0; }
    else printf("raster_backend mfgpu-invalidate OK\n");
    if (!case_two_keys()) { printf("FAIL mfgpu-two-keys\n"); ok = 0; }
    else printf("raster_backend mfgpu-two-keys OK\n");
    if (!case_eviction()) { printf("FAIL mfgpu-eviction\n"); ok = 0; }
    else printf("raster_backend mfgpu-eviction OK\n");
    return ok ? 0 : 1;
}
