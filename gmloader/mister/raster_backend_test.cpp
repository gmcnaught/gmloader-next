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

// Fabric-offload Task 2: exercise the refmodel's BLT_OP_STAGE handling directly
// (not via the vtable). blt_emitter.h pulls in blitter_ref.h (blt_cmd_t/blt_vtx_t/
// BLT_*/blt_execute/blt_surface_heap_t) + blt_alloc.h; blt_wire.h gives
// blt_unpack_cmd + BLT_CMD_BYTES.
extern "C" {
#include "blt_emitter.h"
#include "blt_wire.h"
}

// backend_mfgpu (Task 4/5) + its host-only hooks. Declared here rather than via
// raster_backend.h because these are not part of the vtable seam — they exist
// solely so this host test can read the fixed-size fabric framebuffer
// blt_execute composited into, and steer the FBO-fallback decision.
extern "C" const RasterBackend backend_sw;
extern "C" const RasterBackend backend_mfgpu;
extern "C" void RasterBackend_MFGPU_TestCopyFB565(int w, int h, uint16_t *out);
extern "C" void RasterBackend_MFGPU_SetDefaultSurface(const uint8_t *rgba);
// Task 5: push the app-surface FBO/tex identity down (mirrors SetDefaultSurface;
// see blitter.cpp/raster_backend_mfgpu.cpp for why this is a push, not a pull).
extern "C" void RasterBackend_MFGPU_SetAppSurface(uint32_t fbo, uint32_t tex);
// Task 2: cache introspection/reset hooks (host-test-only, not part of the vtable).
extern "C" uint32_t RasterBackend_MFGPU_TestUploadCount(void);
extern "C" uint32_t RasterBackend_MFGPU_TestStageCount(void);   // FO Task 3
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

// (A) A 1024x1024 opaque page (1024*1024*2 = 2MB RGB565, once staged) is a
// genuinely large page: it exceeds the OLD ~1MB/frame scratch heap (which
// would have dropped it) but sits comfortably in the new 32MB persistent
// texture heap, and must still match the SW oracle within +/-1 LSB 565.
static int case_large_page(void) {
    enum { TW = 1024, TH = 1024 };
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
    if (RasterBackend_MFGPU_TestUploadCount() != 2) return 0;

    // Airtight: the count alone doesn't prove the NEW pixels actually made it
    // to the fabric framebuffer (a stale cached page would also re-upload if
    // the count logic were wrong elsewhere) — read back and check the
    // rendered pixel reflects the changed texel (250,20,30) not the original
    // (10,20,30). (6,6) is inside the triangle (2,2)-(28,4)-(4,28).
    static uint16_t mf565[BW*BH];
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);
    uint16_t px565 = mf565[6 * BW + 6];
    int r5 = (px565 >> 11) & 0x1F, g5 = (px565 >> 5) & 0x3F, b5 = px565 & 0x1F;
    int er5 = 250 >> 3, eg5 = 20 >> 2, eb5 = 30 >> 3;
    if (abs(r5 - er5) > 1 || abs(g5 - eg5) > 1 || abs(b5 - eb5) > 1) {
        printf("  FAIL invalidate-pixel got 565=(%d,%d,%d) expected ~(%d,%d,%d)\n",
               r5, g5, b5, er5, eg5, eb5);
        return 0;
    }
    return 1;
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

// ---- Fix wave: pin-on-emit eviction (no intra-frame use-after-free/alias) ----
// A single frame draws two DISTINCT textures into non-overlapping screen
// regions, with the texture heap capped to hold only ONE of the two pages.
// texA is staged+emitted first (pinning it for the rest of THIS frame);
// texB's stage_texture must then fail to evict texA (still pinned) to make
// room, so texB's upload fails. texA's region must never show texB's colour
// (what an unguarded evict_one_lru would alias it to, since both draws would
// share one heap offset and blt_execute only runs at frame end) — see the
// long comment inline below for what it shows instead (either texA's own
// colour, or a safe whole-frame drop via the pre-existing overflow net).
static int case_intra_frame_no_alias(void) {
    enum { TW = 512, TH = 512 };
    static uint8_t texA_px[TW*TH*4], texB_px[TW*TH*4];
    for (int i = 0; i < TW*TH; i++) {
        texA_px[i*4+0] = 60;  texA_px[i*4+1] = 60;  texA_px[i*4+2] = 60;  texA_px[i*4+3] = 255;
        texB_px[i*4+0] = 180; texB_px[i*4+1] = 180; texB_px[i*4+2] = 180; texB_px[i*4+3] = 255;
    }
    RTexture texA = { texA_px, TW, TH, 1, 1, /*RGBA8888*/0, 1 };
    RTexture texB = { texB_px, TW, TH, 1, 1, /*RGBA8888*/0, 1 };

    static uint8_t  rgba_mf[BW*BH*4];
    static uint16_t mf565[BW*BH];
    RSurface s_mf = { rgba_mf, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);

    // 768KB holds exactly ONE 512x512 RGB565 page (512*512*2 = 512KB) but not
    // two (1MB) — forces texB to need texA's slot within the same frame.
    RasterBackend_MFGPU_TestReinit(768u*1024);

    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s_mf, 0, 0, 0, 255);

    // texA: LEFT region, x in [0,100], full height.
    BVtx vA[6] = {
        {   0.f,      0.f, 0,0, 1,1,1,1 }, { 100.f,      0.f, 1,0, 1,1,1,1 }, {   0.f, (float)BH, 0,1, 1,1,1,1 },
        { 100.f,      0.f, 1,0, 1,1,1,1 }, { 100.f, (float)BH, 1,1, 1,1,1,1 }, {   0.f, (float)BH, 0,1, 1,1,1,1 },
    };
    backend_mfgpu.draw(&s_mf, vA, 2, &texA, RB_NONE, 0.f, 101);   // stages+pins texA this frame

    // texB: RIGHT region, x in [150,250], full height. Must NOT be able to
    // evict texA's still-pinned page to make room.
    BVtx vB[6] = {
        { 150.f,      0.f, 0,0, 1,1,1,1 }, { 250.f,      0.f, 1,0, 1,1,1,1 }, { 150.f, (float)BH, 0,1, 1,1,1,1 },
        { 250.f,      0.f, 1,0, 1,1,1,1 }, { 250.f, (float)BH, 1,1, 1,1,1,1 }, { 150.f, (float)BH, 0,1, 1,1,1,1 },
    };
    backend_mfgpu.draw(&s_mf, vB, 2, &texB, RB_NONE, 0.f, 102);   // expected to fail-to-stage, drop

    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);

    // Sample well inside the LEFT (texA) region.
    //
    // With the fix, texB's stage_texture can never free texA's pinned page,
    // so its retry-upload permanently fails; that trips the PRE-EXISTING
    // (Task 4) emitter-overflow safety net, which drops the WHOLE frame
    // (mf_frame_end sees g_e.overflow and leaves g_fb565 zeroed) rather than
    // risk executing a partially-corrupt command list — so the left region
    // reads black (r5==0), never texB's aliased colour. Without the fix,
    // evict_one_lru frees texA's still-referenced offset, texB's retry
    // succeeds (no overflow), the frame renders, and texA's ALREADY-EMITTED
    // trilist command samples texB's pixels at the reused offset -> the left
    // region reads ~tag180 (r5==22). Either way the assertion below is the
    // real invariant this test protects: the aliased colour must never reach
    // the framebuffer.
    uint16_t px = mf565[(BH / 2) * BW + 50];
    int r5 = (px >> 11) & 0x1F;
    // tag 60 -> r5==7; black (safe frame-drop) -> r5==0; tag 180 (aliased,
    // the bug) -> r5==22. <=12 passes for the first two, fails only for the
    // aliasing case.
    if (r5 > 12) {
        printf("  FAIL intra-frame-no-alias left region r5=%d (expected <=12, texA not aliased to texB)\n", r5);
        return 0;
    }
    return 1;
}

// ── Fabric-offload Task 3: SDRAM residency — stage once per page ──────────────
// A texture drawn across two frames must emit exactly ONE BLT_OP_STAGE (staged
// into SDRAM on the first-frame miss, reused on the second-frame cache hit) and
// one blt_upload, and render within ±1 LSB of the SW oracle both frames.
static int case_sdram_residency(void) {
    static const uint8_t px[4] = { 60, 180, 240, 255 };
    RTexture t = { px, 1, 1, 1, 1, 0, 1 };
    BVtx v[3] = { {2,2,0,0,1,1,1,1}, {28,4,1,0,1,1,1,1}, {4,28,0,1,1,1,1,1} };
    static uint8_t  rgba_sw[BW*BH*4];
    static uint8_t  rgba_mf[BW*BH*4];
    static uint16_t mf565[BW*BH];
    RSurface s_sw = { rgba_sw, BW, BH };
    RSurface s_mf = { rgba_mf, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    RasterBackend_MFGPU_TestReinit(0);
    const uint32_t K = 909;
    backend_sw.clear(&s_sw, 0, 0, 0, 255);
    backend_sw.draw(&s_sw, v, 1, &t, RB_NONE, 0.f, K);   // deterministic oracle
    for (int f = 0; f < 2; f++) {
        backend_mfgpu.frame_begin();
        backend_mfgpu.clear(&s_mf, 0, 0, 0, 255);
        backend_mfgpu.draw(&s_mf, v, 1, &t, RB_NONE, 0.f, K);
        backend_mfgpu.frame_end();
        RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);
        if (!rgb565_within1(&s_sw, mf565)) { printf("  sdram_residency: frame %d mismatch\n", f); return 0; }
    }
    return RasterBackend_MFGPU_TestUploadCount() == 1 && RasterBackend_MFGPU_TestStageCount() == 1;
}

// ── Fabric-offload Task 2: BLT_OP_STAGE is a refmodel no-op ───────────────────
// With same-offset staging (SDRAM[off] = DDR[off], user-confirmed hardware
// contract), the fabric's TRILIST texel fetch reads SDRAM[src_off] == a copy of
// DDR[src_off]. The host refmodel has no SDRAM: blt_execute skips OP_STAGE
// (blitter_ref.c) and reads texels straight from the DDR heap at src_off. So a
// ring that STAGEs a texture before its TRILIST must render byte-IDENTICALLY to
// the same ring without the STAGE. This locks that contract, so Task 3 can emit
// blt_stage unconditionally and keep the host oracle honest.
static int stage_noop_fb_nonempty(const uint16_t *fb) {
    for (int i = 0; i < BLT_FB_PIXELS; i++) if (fb[i]) return 1;
    return 0;
}
static void stage_noop_build_exec(int with_stage, uint16_t *fb) {
    enum { VTX_REGION = 128 * 1024, HEAP = 1 << 20 };
    static uint8_t  ring[8192];
    static uint8_t  srcdram[HEAP];
    static blt_cmd_t cmds[256];
    blt_emitter_t e;
    blt_emitter_init(&e, ring, sizeof ring, srcdram, sizeof srcdram);
    blt_alloc_init(&e.alloc, VTX_REGION, (uint32_t)(HEAP - VTX_REGION));
    blt_vtx_buf_init(&e, srcdram, VTX_REGION);
    blt_begin_frame(&e, 0, 0, 0);

    uint16_t tex[8 * 8];
    for (int i = 0; i < 8 * 8; i++) tex[i] = 0xF81F;   // opaque magenta
    blt_surface_ref_t ref = blt_upload(&e, tex, 8, 8, 8 * 2);

    if (with_stage) blt_stage(&e, ref.off, (uint32_t)ref.stride * ref.h);  // same-offset

    blt_vtx_t tris[3] = {   // integer coords -> identical coverage both ways
        { (int16_t)(160 << 4), (int16_t)(60 << 4),  (uint16_t)(4 << 4), (uint16_t)(0 << 4), BLT_RGBA(255,255,255,255), 0 },
        { (int16_t)(220 << 4), (int16_t)(170 << 4), (uint16_t)(7 << 4), (uint16_t)(7 << 4), BLT_RGBA(255,255,255,255), 0 },
        { (int16_t)(100 << 4), (int16_t)(170 << 4), (uint16_t)(0 << 4), (uint16_t)(7 << 4), BLT_RGBA(255,255,255,255), 0 },
    };
    uint32_t eoff = blt_push_tris(&e, tris, 1);
    blt_trilist(&e, ref, BLT_BLEND_COPY, /*colorkey=*/0, /*alpha=*/255, eoff, 1, /*flags=*/0);
    blt_end_frame(&e);

    int n = e.cmd_count;
    if (n > (int)(sizeof cmds / sizeof cmds[0])) n = (int)(sizeof cmds / sizeof cmds[0]);
    for (int i = 0; i < n; i++)
        blt_unpack_cmd(ring + (size_t)i * BLT_CMD_BYTES, &cmds[i]);
    memset(fb, 0, (size_t)BLT_FB_PIXELS * sizeof(uint16_t));
    blt_surface_heap_t heap = { srcdram, sizeof srcdram, NULL, NULL };
    blt_execute(fb, &heap, cmds, n);
}
static int case_stage_noop(void) {
    static uint16_t fb_plain[BLT_FB_PIXELS];
    static uint16_t fb_staged[BLT_FB_PIXELS];
    stage_noop_build_exec(/*with_stage=*/0, fb_plain);
    stage_noop_build_exec(/*with_stage=*/1, fb_staged);
    if (!stage_noop_fb_nonempty(fb_plain)) { printf("  stage_noop: triangle did not render\n"); return 0; }
    return memcmp(fb_plain, fb_staged, sizeof fb_plain) == 0;
}

// ── Task 2 (sub-region residency): per-quad crop-stage bit-exact + small ──────
// Fill helper: a smooth low-contrast RGB gradient over a 512x512 page. Smooth on
// purpose (<=1 565 LSB between adjacent texels), so the fabric's +HALF-bias
// nearest sample and the SW oracle's floor never disagree by more than the +-1
// tolerance at a texel boundary -- while a GROSS crop-offset bug (sampling a
// wrong sub-rect dozens of texels away) shifts the gradient far enough to blow
// past +-1. Exactly the fixture rationale the tex-NxM battery case documents.
static void fill_gradient_512(uint8_t *tex, int TW, int TH) {
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            uint8_t *p = tex + ((size_t)y * TW + x) * 4;
            p[0] = (uint8_t)(x / 2);   // 0..255 across the page width
            p[1] = (uint8_t)(y / 2);
            p[2] = 100; p[3] = 255;
        }
}

// A single sprite-quad sampling a small UV sub-rect of a 512x512 (512KB RGB565)
// page must (a) render +-1 LSB identical to the whole-page render (the SW oracle
// samples the full texture), and (b) stage ONLY that sub-rect.
//
// The texture heap is capped to 256KB -- SMALLER than the 512KB whole page but
// far larger than the ~51x51-texel sub-rect (~5KB). So the RED (pre-crop, whole-
// page staging) is unambiguous: the 512KB page cannot fit -> blt_upload fails ->
// the frame drops (g_fb565 zeroed) -> compare565 FAILS and upload_count stays 0.
// The GREEN (per-quad crop): only the sub-rect is staged -> it fits -> renders
// bit-exact and upload_count == 1. This is the on-device 20MB-vs-14.62MB heap
// overflow the whole feature exists to fix, reproduced in miniature.
static int case_subregion_matches_wholepage(void) {
    enum { TW = 512, TH = 512 };
    static uint8_t tex[TW*TH*4];
    fill_gradient_512(tex, TW, TH);
    RTexture t = { tex, TW, TH, 1, 1, /*RGBA8888*/0, 1 };
    // One quad (2 tris) sampling UV [0.1,0.2] x [0.3,0.4] -- a ~51x51-texel
    // sub-rect of the 512x512 page -- mapped onto a screen rect.
    BVtx v[6] = {
        {  40.f,  40.f, 0.1f,0.3f, 1,1,1,1 }, { 200.f,  40.f, 0.2f,0.3f, 1,1,1,1 }, { 200.f,150.f, 0.2f,0.4f, 1,1,1,1 },
        {  40.f,  40.f, 0.1f,0.3f, 1,1,1,1 }, { 200.f,150.f, 0.2f,0.4f, 1,1,1,1 }, {  40.f,150.f, 0.1f,0.4f, 1,1,1,1 },
    };
    RasterBackend_MFGPU_TestReinit(256u*1024);   // < 512KB whole page, >> the sub-rect
    int ok = battery_case_key("subregion", 8,8,8, &t, v, 2, RB_NONE, next_key());
    uint32_t up = RasterBackend_MFGPU_TestUploadCount();
    if (up != 1) {
        printf("  FAIL subregion upload_count=%u (expected 1 sub-rect, not the whole page)\n", up);
        ok = 0;
    }
    return ok;
}

// Batched multi-sprite (mirrors the real key5 sprite-batch): ONE draw with 4 tris
// = two quads sampling two DIFFERENT sub-rects of the same page. Both must render
// bit-exact vs whole-page, and TWO small regions must stage (not the full page).
// Same 256KB cap => whole-page staging (pre-crop) can't fit -> RED; two sub-rects
// fit -> GREEN with upload_count == 2.
static int case_subregion_batched_multi(void) {
    enum { TW = 512, TH = 512 };
    static uint8_t tex[TW*TH*4];
    fill_gradient_512(tex, TW, TH);
    RTexture t = { tex, TW, TH, 1, 1, /*RGBA8888*/0, 1 };
    BVtx v[12] = {
        // quad A: screen [20,20]-[110,110], UV sub-rect [0.10,0.20] x [0.10,0.20]
        {  20.f, 20.f, 0.10f,0.10f, 1,1,1,1 }, { 110.f, 20.f, 0.20f,0.10f, 1,1,1,1 }, { 110.f,110.f, 0.20f,0.20f, 1,1,1,1 },
        {  20.f, 20.f, 0.10f,0.10f, 1,1,1,1 }, { 110.f,110.f, 0.20f,0.20f, 1,1,1,1 }, {  20.f,110.f, 0.10f,0.20f, 1,1,1,1 },
        // quad B: screen [140,20]-[230,110], UV sub-rect [0.70,0.80] x [0.70,0.80]
        { 140.f, 20.f, 0.70f,0.70f, 1,1,1,1 }, { 230.f, 20.f, 0.80f,0.70f, 1,1,1,1 }, { 230.f,110.f, 0.80f,0.80f, 1,1,1,1 },
        { 140.f, 20.f, 0.70f,0.70f, 1,1,1,1 }, { 230.f,110.f, 0.80f,0.80f, 1,1,1,1 }, { 140.f,110.f, 0.70f,0.80f, 1,1,1,1 },
    };
    RasterBackend_MFGPU_TestReinit(256u*1024);
    int ok = battery_case_key("subregion-batch", 8,8,8, &t, v, 4, RB_NONE, next_key());
    uint32_t up = RasterBackend_MFGPU_TestUploadCount();
    if (up != 2) {
        printf("  FAIL subregion-batch upload_count=%u (expected 2 sub-rects, not the whole page)\n", up);
        ok = 0;
    }
    return ok;
}

// ── Task 3 (fallback + edge + pin-aware insert) ───────────────────────────────
// Build a grid of screen cells, each a 2-tri quad sampling ONE texel of the page
// at a constant mid-texel UV (tx+0.25)/tw -- exact on both rasterizers (no
// nearest-boundary ambiguity), so a high-contrast per-texel pattern compares
// bit-exact and a wrong/dropped texel is caught. Samples texels
// (tx0+i*stride, ty0+j*stride) for i in [0,nx), j in [0,ny); cells are `cell` px
// from origin (ox,oy). Writes nx*ny*6 verts, returns the triangle count.
static int build_texel_grid(BVtx *out, int tx0, int ty0, int nx, int ny, int stride,
                            int tw, int th, float cell, float ox, float oy) {
    int n = 0;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            int tx = tx0 + i*stride, ty = ty0 + j*stride;
            float u = (tx + 0.25f) / (float)tw, uv = (ty + 0.25f) / (float)th;
            float x0 = ox + i*cell, y0 = oy + j*cell, x1 = x0 + cell, y1 = y0 + cell;
            BVtx q[6] = {
                { x0,y0, u,uv, 1,1,1,1 }, { x1,y0, u,uv, 1,1,1,1 }, { x1,y1, u,uv, 1,1,1,1 },
                { x0,y0, u,uv, 1,1,1,1 }, { x1,y1, u,uv, 1,1,1,1 }, { x0,y1, u,uv, 1,1,1,1 },
            };
            for (int k = 0; k < 6; k++) out[n++] = q[k];
        }
    return nx*ny*2;
}

// After a draw of `key` that (per Task 3) routes through the WHOLE-PAGE path, a
// subsequent whole-page (UV 0..1) draw of the same key must be a cache HIT.
// Returns upload_count after the second draw: 1 iff both share the one whole-page
// entry; >1 iff the first draw cached a different (cropped) rect. Shares the live
// cache (no reinit).
static uint32_t wholepage_followup_uploads(const RTexture *t, uint32_t key) {
    static uint8_t rgba[BW*BH*4];
    RSurface s = { rgba, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba);
    BVtx wp[6] = {
        {   0.f,  0.f, 0,0, 1,1,1,1 }, { 200.f,  0.f, 1,0, 1,1,1,1 }, { 200.f,150.f, 1,1, 1,1,1,1 },
        {   0.f,  0.f, 0,0, 1,1,1,1 }, { 200.f,150.f, 1,1, 1,1,1,1 }, {   0.f,150.f, 0,1, 1,1,1,1 },
    };
    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s, 8,8,8,255);
    backend_mfgpu.draw(&s, wp, 2, t, RB_NONE, 0.f, key);
    backend_mfgpu.frame_end();
    return RasterBackend_MFGPU_TestUploadCount();
}

// Odd triCount (not clean sprite-quads) routes the WHOLE draw through the intact
// whole-page path: renders bit-exact, and a following whole-page draw of the same
// texture is a cache HIT (upload_count stays 1). Before Task 3, odd was split into
// a quad + a trailing tri cropped to two sub-rects != the whole page, so the
// whole-page follow-up would MISS and upload again (RED: uploads>1).
static int case_fallback_odd_tricount(void) {
    enum { TW = 64, TH = 64 };
    static uint8_t tex[TW*TH*4];
    for (int y=0;y<TH;y++) for (int x=0;x<TW;x++){ uint8_t*p=tex+((y*TW+x)*4);
        p[0]=(uint8_t)(x*4); p[1]=(uint8_t)(y*4); p[2]=90; p[3]=255; }
    RTexture t = { tex, TW, TH, 1, 1, 0, 1 };
    BVtx v[9] = {                              // 3 tris = 1 quad + 1 stray triangle
        {  20.f,20.f, 0.10f,0.10f, 1,1,1,1 }, { 120.f,20.f, 0.50f,0.10f, 1,1,1,1 }, { 120.f,120.f, 0.50f,0.50f, 1,1,1,1 },
        {  20.f,20.f, 0.10f,0.10f, 1,1,1,1 }, { 120.f,120.f,0.50f,0.50f, 1,1,1,1 }, {  20.f,120.f,0.10f,0.50f, 1,1,1,1 },
        { 140.f,20.f, 0.60f,0.60f, 1,1,1,1 }, { 240.f,20.f, 0.90f,0.60f, 1,1,1,1 }, { 140.f,120.f,0.60f,0.90f, 1,1,1,1 },
    };
    RasterBackend_MFGPU_TestReinit(0);
    uint32_t K = next_key();
    int ok = battery_case_key("odd-tricount", 8,8,8, &t, v, 3, RB_NONE, K);
    uint32_t up = wholepage_followup_uploads(&t, K);
    if (up != 1) { printf("  FAIL odd-tricount routed to crop, not whole-page (uploads=%u)\n", up); ok = 0; }
    return ok;
}

// A quad whose UV bbox covers ~the whole page (>=90% area) routes through the
// whole-page path (avoids re-cropping ~the entire page every frame). Renders
// bit-exact, and a whole-page follow-up is a cache HIT. Before Task 3 the
// near-full quad cached a cropped rect (e.g. 9,9,495,495) != the whole page, so
// the follow-up MISSED (RED: uploads>1).
static int case_nearfullpage(void) {
    enum { TW = 512, TH = 512 };
    static uint8_t tex[TW*TH*4];
    fill_gradient_512(tex, TW, TH);
    RTexture t = { tex, TW, TH, 1, 1, 0, 1 };
    BVtx v[6] = {                              // UV [0.02,0.98]^2 ~= 93% of the page
        {  40.f,40.f, 0.02f,0.02f, 1,1,1,1 }, { 200.f,40.f, 0.98f,0.02f, 1,1,1,1 }, { 200.f,150.f, 0.98f,0.98f, 1,1,1,1 },
        {  40.f,40.f, 0.02f,0.02f, 1,1,1,1 }, { 200.f,150.f,0.98f,0.98f, 1,1,1,1 }, {  40.f,150.f,0.02f,0.98f, 1,1,1,1 },
    };
    RasterBackend_MFGPU_TestReinit(0);
    uint32_t K = next_key();
    int ok = battery_case_key("nearfull", 8,8,8, &t, v, 2, RB_NONE, K);
    uint32_t up = wholepage_followup_uploads(&t, K);
    if (up != 1) { printf("  FAIL nearfull routed to crop, not whole-page (uploads=%u)\n", up); ok = 0; }
    return ok;
}

// A sprite whose UV bbox touches the texture's MAX row/col: verifies the +1-texel
// crop margin clamps to the page (rx1 = clampi(ceil(u1*w)+1, 1, w) includes texel
// w-1) so the edge texel is never dropped. The max row/col are a distinct bright
// colour; mid-texel UV keeps the sample exact, so a dropped/clamped-wrong edge
// texel diverges past +-1.
static int case_edge_sprite(void) {
    enum { TW = 16, TH = 16 };
    static uint8_t tex[TW*TH*4];
    for (int y=0;y<TH;y++) for (int x=0;x<TW;x++){ uint8_t*p=tex+((y*TW+x)*4);
        p[0]=(uint8_t)(x*17); p[1]=(uint8_t)(y*17);
        p[2]=(x==TW-1 || y==TH-1) ? 255 : 40;   // bright max-edge marker
        p[3]=255; }
    RTexture t = { tex, TW, TH, 1, 1, 0, 1 };
    static BVtx v[4*4*6];
    int tris = build_texel_grid(v, /*tx0*/12,/*ty0*/12, /*nx*/4,/*ny*/4, /*stride*/1,
                                TW, TH, /*cell*/20.f, /*ox*/40.f, /*oy*/40.f);
    RasterBackend_MFGPU_TestReinit(0);
    return battery_case_key("edge-sprite", 5,5,5, &t, v, tris, RB_NONE, next_key());
}

// Pin-aware cache insert (Task-2 reviewer-mandated). Force >MF_TEX_CACHE_N (256)
// DISTINCT sub-regions in ONE frame (a 20x15 grid = 300 cells, texels spaced 5
// apart so each crop rect is a distinct cache key). The full-table insert cannot
// evict a frame-pinned entry (its heap is referenced by an already-emitted, not-
// yet-executed trilist); it must DROP the frame (overflow) instead of freeing +
// reusing that slot. Invariant: for every drawn cell centre, the fabric pixel is
// EITHER black (the frame gracefully dropped) OR within +-1 of the SW oracle. A
// NON-black pixel that differs from the oracle == an intra-frame texture ALIAS
// (the bug). Before the fix the frame does NOT overflow (all uploads fit) and the
// aliasing inserts render wrong textures -> RED.
static int case_pin_aware_insert(void) {
    enum { TW = 512, TH = 512, NX = 20, NY = 15 };
    static uint8_t tex[TW*TH*4];
    for (int y=0;y<TH;y++) for (int x=0;x<TW;x++){ uint8_t*p=tex+((size_t)(y*TW+x)*4);
        p[0]=(uint8_t)(x*4); p[1]=(uint8_t)(y*4); p[2]=128; p[3]=255; }   // per-texel distinct, non-black
    RTexture t = { tex, TW, TH, 1, 1, 0, 1 };
    static BVtx v[NX*NY*6];
    const float CELL = 8.f, OX = 8.f, OY = 8.f;
    int tris = build_texel_grid(v, /*tx0*/10,/*ty0*/10, NX, NY, /*stride*/5,
                                TW, TH, CELL, OX, OY);   // 300 distinct sub-regions

    static uint8_t  rgba_sw[BW*BH*4], rgba_mf[BW*BH*4];
    static uint16_t mf565[BW*BH];
    RSurface s_sw = { rgba_sw, BW, BH }, s_mf = { rgba_mf, BW, BH };
    backend_sw.clear(&s_sw, 8,8,8,255);
    backend_sw.draw(&s_sw, v, tris, &t, RB_NONE, 0.f, 1);   // whole-texture oracle

    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    RasterBackend_MFGPU_TestReinit(0);
    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s_mf, 8,8,8,255);
    backend_mfgpu.draw(&s_mf, v, tris, &t, RB_NONE, 0.f, 55);
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);

    int checked = 0, dropped = 0;
    for (int j = 0; j < NY; j++)
        for (int i = 0; i < NX; i++) {
            int cx = (int)(OX + i*CELL + CELL/2), cy = (int)(OY + j*CELL + CELL/2);
            uint16_t fb = mf565[cy*BW + cx];
            const uint8_t *p = s_sw.rgba + ((size_t)cy*BW + cx)*4;
            int sr=p[0]>>3, sg=p[1]>>2, sb=p[2]>>3;
            int fr=(fb>>11)&0x1F, fg=(fb>>5)&0x3F, fbb=fb&0x1F;
            checked++;
            if (fb == 0) { dropped++; continue; }          // gracefully dropped
            if (abs(sr-fr)>1 || abs(sg-fg)>1 || abs(sb-fbb)>1) {
                printf("  FAIL pin-aware-insert ALIAS @cell(%d,%d) sw=(%d,%d,%d) fb=(%d,%d,%d)\n",
                       i,j,sr,sg,sb,fr,fg,fbb);
                return 0;
            }
        }
    printf("  OK   pin-aware-insert  %d cells checked, %d gracefully dropped (no alias)\n", checked, dropped);
    return 1;
}

// ── Task 5: route app-surface draws to the fabric surface, sample via
// BLT_F_SRC_SURFACE ──────────────────────────────────────────────────────────
// Two-pass scene through the REAL backend entry points (mirrors Task 3's
// test_surface_src, one layer up): a sprite drawn into the (simulated)
// application-surface FBO, then a fullscreen quad sampling that surface onto
// WORK over a cleared background. This is what actually exercises
// mf_draw's BLT_F_SRC_SURFACE path end-to-end -- until this test, nothing
// called blt_trilist with that flag set (Task 2's relaxed !tex.valid guard
// branch had no caller/test at all).
//
// Ordering deliberately matches the REAL steady-state graph (Task 1), not the
// old "backgrounds first" assumption: scene->APPSURF is emitted BEFORE the
// WORK clear, so this also exercises mf_clear's target-awareness (it must
// switch back to WORK, not leave the ring pointed at APPSURF from the sprite
// draw).
//
// Verification strategy: SPOT-CHECK interior/exterior points (like Task 3's
// test_surface_src), not a full-canvas compare565(). The fabric's blt_raster_tri
// and the SW rasterizer (blitter_raster.cpp) use DIFFERENT nearest-sample
// sub-pixel conventions -- already documented in Task 3 (blt_tri.c rounds via a
// +HALF bias, the SW oracle floors), a full LSB off at the sprite's edges, not
// a rounding-noise level difference a +-1 LSB colour tolerance can absorb. The
// existing keyed_case() battery avoids this by choosing UV offsets that land
// inside both conventions' agreement window; a sharp sprite boundary can't be
// dodged that way, so this test checks points comfortably inside and outside
// the sprite instead of literally every pixel.
static int case_surface_route(void) {
    enum { W = 64, H = 48 };   // app-surface's used region for this test (arbitrary, <=BW,BH)
    const uint32_t APPSURF_FBO = 50, APPSURF_TEX = 51;
    const uint8_t BG_R = 20, BG_G = 30, BG_B = 40;

    RasterBackend_MFGPU_SetAppSurface(0, 0);   // clean slate regardless of test order
    RasterBackend_MFGPU_TestReinit(0);

    // 1x1 opaque texel for the sprite drawn INTO the app surface.
    static const uint8_t spritepx[4] = { 220, 60, 160, 255 };
    RTexture spriteTex = { spritepx, 1, 1, 1, 1, /*RGBA8888*/0, 1 };
    BVtx spriteVerts[3] = {
        {  4.f,  4.f, 0,0, 1,1,1,1 },
        { 40.f,  8.f, 0,0, 1,1,1,1 },
        {  8.f, 36.f, 0,0, 1,1,1,1 },
    };
    // Fullscreen (over the WxH app-surface region) quad sampling the app surface.
    // TWO vertex sets, deliberately NOT the same UVs -- this is what makes the
    // comparison a real test of the composite V-flip rather than a tautology:
    //
    //   oracleVerts  UPRIGHT (v=0 at screen top). backend_sw has no notion of an
    //                app surface; it just samples a plain top-origin buffer, so
    //                these UVs produce THE IMAGE THAT SHOULD APPEAR ON SCREEN.
    //                This is independent ground truth and is NOT adjusted to
    //                match the implementation.
    //   deviceVerts  INVERTED (v=vmax at screen top, v=0 at bottom) -- GL's
    //                bottom-origin FBO convention, exactly what GM emits per the
    //                GMLOADER_MFGPU_UVLOG device capture (v@top=0.8438 >
    //                v@bot=0.0000, 801/801 composite draws over 400 frames).
    //                This is what the FABRIC path receives.
    //
    // So the two backends get different inputs reflecting a real convention
    // difference, and must still produce the same picture: the fabric's
    // composite flip has to undo GM's inversion. Drop the flip and the fabric
    // renders mirrored while the oracle does not -> RED. (Contrast the reverted
    // ac41e1e, which fed the ORACLE the flipped UVs so both sampled identically
    // -- that made the oracle blind to orientation, which is precisely how the
    // upside-down/random-tile bugs got through green tests.)
    BVtx oracleVerts[6] = {
        {   0.f,   0.f, 0.f,0.f, 1,1,1,1 }, { (float)W,   0.f, 1.f,0.f, 1,1,1,1 }, { (float)W,(float)H, 1.f,1.f, 1,1,1,1 },
        {   0.f,   0.f, 0.f,0.f, 1,1,1,1 }, { (float)W,(float)H, 1.f,1.f, 1,1,1,1 }, {   0.f,(float)H, 0.f,1.f, 1,1,1,1 },
    };
    BVtx deviceVerts[6] = {
        {   0.f,   0.f, 0.f,1.f, 1,1,1,1 }, { (float)W,   0.f, 1.f,1.f, 1,1,1,1 }, { (float)W,(float)H, 1.f,0.f, 1,1,1,1 },
        {   0.f,   0.f, 0.f,1.f, 1,1,1,1 }, { (float)W,(float)H, 1.f,0.f, 1,1,1,1 }, {   0.f,(float)H, 0.f,0.f, 1,1,1,1 },
    };

    // ---- SW oracle: an independent app-surface buffer, then sampled as a
    // normal texture -- backend_sw has no notion of "the app surface", so the
    // oracle is built by hand from the same two operations. ----------------
    static uint8_t appsurf_sw[W*H*4], work_sw[BW*BH*4];
    RSurface s_appsurf_sw = { appsurf_sw, W, H, 0 };
    RSurface s_work_sw    = { work_sw,   BW, BH, 0 };
    // Init the SW oracle's app-surface buffer to OPAQUE black (alpha=255), not a raw
    // memset-zero (alpha=0): the fabric's real appsurf buffer is RGB565 with NO alpha
    // channel at all, so "undrawn" there just means "reads as black", never "reads as
    // transparent". A memset-zero buffer has alpha=0 everywhere outside the sprite,
    // and Blitter_RasterDraw's alphaRef is clamp01()'d to [0,1] -- alphaRef=0 (or even
    // a negative value, clamped up to 0) still discards a frag.a==0 texel ("<=" test),
    // so a naive all-zero buffer would make the SW oracle's sample draw silently skip
    // its "background" pixels (leaving the prior WORK clear showing through) while the
    // fabric's COPY blend has no such discard and genuinely overwrites with black --
    // a divergence in the TEST FIXTURE, not in the code under test.
    Blitter_ClearSurface(&s_appsurf_sw, 0, 0, 0, 255);
    backend_sw.draw(&s_appsurf_sw, spriteVerts, 1, &spriteTex, RB_NONE, 0.f, next_key());
    backend_sw.clear(&s_work_sw, BG_R, BG_G, BG_B, 255);
    RTexture appsurfAsTex = { appsurf_sw, W, H, /*nearest*/1, /*valid*/1, /*RGBA8888*/0, /*opaque*/1 };
    backend_sw.draw(&s_work_sw, oracleVerts, 2, &appsurfAsTex, RB_NONE, 0.f, next_key());

    // ---- fabric path: through the REAL backend entry points ------------------
    static uint8_t  work_mf[BW*BH*4];
    static uint16_t mf565[BW*BH];
    RSurface s_appsurf_mf = { nullptr, W, H, APPSURF_FBO };   // rgba unused: fabric-only target
    RSurface s_work_mf    = { work_mf, BW, BH, 0 };
    RasterBackend_MFGPU_SetDefaultSurface(work_mf);
    RasterBackend_MFGPU_SetAppSurface(APPSURF_FBO, APPSURF_TEX);

    backend_mfgpu.frame_begin();
    backend_mfgpu.draw(&s_appsurf_mf, spriteVerts, 1, &spriteTex, RB_NONE, 0.f, next_key());  // scene -> APPSURF
    backend_mfgpu.clear(&s_work_mf, BG_R, BG_G, BG_B, 255);                                   // WORK background
    // The sampled "texture" here is the app surface itself: rgba/valid are
    // never read by mf_draw's src_is_appsurf path (no staging happens for
    // it) -- only w/h matter, for the UV-to-absolute-pixel scale (see
    // mf_draw's comment). tex_key == APPSURF_TEX is what selects this path.
    RTexture surfSrcTex = { nullptr, W, H, 1, 0, 0, 0 };
    backend_mfgpu.draw(&s_work_mf, deviceVerts, 2, &surfSrcTex, RB_NONE, 0.f, APPSURF_TEX);      // appsurf -> WORK
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);

    RasterBackend_MFGPU_SetAppSurface(0, 0);   // don't leak state to other tests

    // Spot-check: (20,15) is comfortably inside the sprite (4,4)-(40,8)-(8,36)
    // on both rasterizers' conventions; (55,40) is comfortably outside it (but
    // still inside the WxH sampled region, so it exercises the SET_TARGET/
    // BLT_F_SRC_SURFACE path reading real "background" surface content, not a
    // trivially-untouched pixel); (200,150) is well outside the whole quad,
    // proving the WORK clear survived undisturbed elsewhere.
    auto px565 = [&](const RSurface &s, int x, int y) -> uint16_t {
        const uint8_t *p = s.rgba + ((size_t)y * s.w + x) * 4;
        return (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
    };
    int ok = 1;
    struct { const char *name; int x, y; } pts[] = {
        { "inside-sprite",  20, 15 },
        { "appsurf-bg",     55, 40 },
        { "outside-quad",  200,150 },
    };
    for (auto &pt : pts) {
        uint16_t sw = px565(s_work_sw, pt.x, pt.y);
        uint16_t fb = mf565[pt.y * BW + pt.x];
        int sr=(sw>>11)&0x1F, sg=(sw>>5)&0x3F, sb=sw&0x1F;
        int fr=(fb>>11)&0x1F, fg=(fb>>5)&0x3F, fb5=fb&0x1F;
        int d = abs(sr-fr); if (abs(sg-fg)>d) d=abs(sg-fg); if (abs(sb-fb5)>d) d=abs(sb-fb5);
        if (d > 1) {
            printf("  FAIL surface-route:%-14s @(%d,%d) sw565=0x%04X fb565=0x%04X\n", pt.name, pt.x, pt.y, sw, fb);
            ok = 0;
        } else {
            printf("  OK   surface-route:%-14s sw565=0x%04X fb565=0x%04X\n", pt.name, sw, fb);
        }
    }
    return ok;
}

static int rgb565_is(uint16_t px, int r5, int g5, int b5) {
    int r=(px>>11)&0x1F, g=(px>>5)&0x3F, b=px&0x1F;
    return abs(r-r5)<=1 && abs(g-g5)<=1 && abs(b-b5)<=1;
}

// ── app-surface composite V-flip ─────────────────────────────────────────────
// Locks the ONE place GL's bottom-origin FBO convention actually reaches the
// fabric. Geometry is taken verbatim from a device capture
// (GMLOADER_MFGPU_UVLOG, Maldita title screen, 801/801 composite draws
// identical across 400 frames):
//
//   page=512x256  content=288x216  screen=[0,0..320,240]
//   uv=[0.0000,0.0000 .. 0.5625,0.8438]   v@top=0.8438  v@bot=0.0000
//
// v@top > v@bot: GM emits the composite with v=0 at the BOTTOM of the image
// (GL's FBO origin), while the fabric app surface is top-origin, so sampling it
// raw renders the whole scene upside-down. mf_draw must invert V on this draw.
//
// Note 0.8438 == 216/256 and 0.5625 == 288/512: the page is PADDED (a POT 512x256
// holding the 288x216 surface). That is why the flip CANNOT be a normalized
// 1-v -- that would map 0.8438 -> 0.1562 and 0.0 -> 1.0, i.e. straight into the
// dead padding rows below the content. The flip has to be taken about the
// sampled band itself. This case would pass under a naive 1-v only if the page
// were unpadded, so the padded dims here are load-bearing, not decoration.
//
// Hand-asserted (no SW oracle): the app surface is filled by POSITION (top half
// GREEN, bottom half MAGENTA via per-vertex tint on a 1x1 white page), which is
// flip-invariant, so the only thing under test is the composite's own V.
static int case_appsurf_composite_vflip(void) {
    enum { PAGE_W = 512, PAGE_H = 256 };          // padded POT page (device)
    const float UMAX = (float)BW / PAGE_W;        // 288/512 = 0.5625
    const float VMAX = (float)BH / PAGE_H;        // 216/256 = 0.8438
    const uint32_t APPSURF_FBO = 60, APPSURF_TEX = 61;
    int ok = 1;

    static const uint8_t whitepx[4] = { 255,255,255,255 };
    RTexture white = { whitepx, 1, 1, 1, 1, 0, 1 };
    RSurface s_appsurf = { nullptr, BW, BH, APPSURF_FBO };
    static uint8_t  work_mf[BW*BH*4];
    static uint16_t work565[BW*BH];
    RSurface s_work = { work_mf, BW, BH, 0 };

    auto colorquad = [&](float x0,float y0,float x1,float y1,float r,float g,float b, BVtx *q){
        BVtx a[6] = { {x0,y0,0,0,r,g,b,1},{x1,y0,0,0,r,g,b,1},{x1,y1,0,0,r,g,b,1},
                      {x0,y0,0,0,r,g,b,1},{x1,y1,0,0,r,g,b,1},{x0,y1,0,0,r,g,b,1} };
        for (int i=0;i<6;i++) q[i]=a[i];
    };
    BVtx greenq[6], magq[6];
    colorquad(0,0,      BW,BH/2, 0.f,0.784f,0.f,     greenq);   // TOP half  GREEN
    colorquad(0,BH/2,   BW,BH,   0.784f,0.f,0.784f,  magq);     // BOT half  MAGENTA

    // The composite, exactly as the device emits it: v=VMAX at screen TOP,
    // v=0 at screen BOTTOM (GL bottom-origin), u spanning only the used width.
    BVtx comp[6] = {
        {   0.f,    0.f, 0.f, VMAX, 1,1,1,1 }, { (float)BW,    0.f, UMAX, VMAX, 1,1,1,1 },
        { (float)BW,(float)BH, UMAX, 0.f,  1,1,1,1 },
        {   0.f,    0.f, 0.f, VMAX, 1,1,1,1 }, { (float)BW,(float)BH, UMAX, 0.f, 1,1,1,1 },
        {   0.f,(float)BH, 0.f, 0.f,  1,1,1,1 },
    };

    RasterBackend_MFGPU_SetDefaultSurface(work_mf);
    RasterBackend_MFGPU_SetAppSurface(APPSURF_FBO, APPSURF_TEX);
    RasterBackend_MFGPU_TestReinit(0);
    backend_mfgpu.frame_begin();
    backend_mfgpu.draw(&s_appsurf, greenq, 2, &white, RB_NONE, 0.f, next_key());
    backend_mfgpu.draw(&s_appsurf, magq,   2, &white, RB_NONE, 0.f, next_key());
    backend_mfgpu.clear(&s_work, 0,0,0,255);
    RTexture surfSrc = { nullptr, PAGE_W, PAGE_H, 1, 0, 0, 0 };   // padded page dims
    backend_mfgpu.draw(&s_work, comp, 2, &surfSrc, RB_NONE, 0.f, APPSURF_TEX);
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, work565);
    RasterBackend_MFGPU_SetAppSurface(0, 0);   // don't leak state into later cases

    uint16_t top = work565[(BH/8)*BW + BW/2];       // deep in the top of the composite
    uint16_t bot = work565[(BH*7/8)*BW + BW/2];     // deep in the bottom
    if (!rgb565_is(top, 0,49,0) || !rgb565_is(bot, 25,0,25)) {
        printf("  FAIL composite-vflip top=0x%04X (want GREEN, the surface's TOP) "
               "bot=0x%04X (want MAGENTA)\n", top, bot);
        ok = 0;
    } else printf("  OK   composite-vflip  top=GREEN bot=MAGENTA (surface upright on screen)\n");
    return ok;
}

// ── atlas sub-region: the guard that was missing ─────────────────────────────
// Regression lock for the reverted per-sprite V-flip (ac41e1e / d0d5d27). That
// commit flipped v'=1-v on EVERY sprite, normalized over the whole page, which
// silently RELOCATES the sample on an atlas instead of mirroring the sprite --
// on device it swapped the title screen for unrelated tiles of the same sheet.
//
// The old case_vflip could not see this: it drew a FULL-PAGE quad (v spanning
// 0..1), the single geometry where a page-normalized flip is invisible. Real GM
// draws are the opposite -- the device capture shows scene sprites sampling
// narrow bands of a 2048x2048 sheet (e.g. v=0.1084..0.8613).
//
// So: a 4x4 grid of distinctly-coloured 16x16 tiles, draw exactly ONE interior
// tile, and assert the rendered colour is THAT tile's. Tile (1,1) sits at
// v=[0.25,0.50]; a 1-v flip sends it to v=[0.50,0.75] == tile row 2, a different
// colour -> unambiguous FAIL. Tile colours are chosen so row 1 and row 2 differ
// well beyond 565 rounding.
static int case_atlas_subregion_tile(void) {
    enum { TW = 64, TH = 64, TILE = 16 };
    static uint8_t tex[TW*TH*4];
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            uint8_t *p = tex + ((size_t)y*TW + x)*4;
            int tx = x / TILE, ty = y / TILE;
            p[0] = (uint8_t)(40*tx + 20); p[1] = (uint8_t)(40*ty + 20); p[2] = 128; p[3] = 255;
        }
    RTexture t = { tex, TW, TH, 1, 1, 0, 1 };

    const int TX = 1, TY = 1;                       // the tile under test
    const float u0 = (float)(TX*TILE)/TW, u1 = (float)((TX+1)*TILE)/TW;   // 0.25..0.50
    const float v0 = (float)(TY*TILE)/TH, v1 = (float)((TY+1)*TILE)/TH;   // 0.25..0.50
    // Inset the UVs by a quarter-texel so neither rasterizer's nearest rounding
    // can drift onto the neighbouring tile at the shared boundary.
    const float eps = 0.25f / TW;
    const float su0 = u0+eps, su1 = u1-eps, sv0 = v0+eps, sv1 = v1-eps;
    const float X0 = 40.f, Y0 = 40.f, X1 = 200.f, Y1 = 160.f;
    BVtx q[6] = {
        { X0,Y0, su0,sv0, 1,1,1,1 }, { X1,Y0, su1,sv0, 1,1,1,1 }, { X1,Y1, su1,sv1, 1,1,1,1 },
        { X0,Y0, su0,sv0, 1,1,1,1 }, { X1,Y1, su1,sv1, 1,1,1,1 }, { X0,Y1, su0,sv1, 1,1,1,1 },
    };

    static uint8_t  rgba_mf[BW*BH*4];
    static uint16_t mf565[BW*BH];
    RSurface s_mf = { rgba_mf, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    RasterBackend_MFGPU_SetAppSurface(0, 0);
    RasterBackend_MFGPU_TestReinit(0);
    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s_mf, 0,0,0,255);
    backend_mfgpu.draw(&s_mf, q, 2, &t, RB_NONE, 0.f, next_key());
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);

    // Expected = tile (1,1) = rgb(60,60,128); the 1-v mis-sample would land on
    // tile (1,2) = rgb(60,100,128), which differs by 10 in the 6-bit green field.
    const int want_r5 = 60>>3, want_g6 = 60>>2, want_b5 = 128>>3;
    uint16_t got = mf565[((int)((Y0+Y1)/2))*BW + (int)((X0+X1)/2)];
    if (!rgb565_is(got, want_r5, want_g6, want_b5)) {
        printf("  FAIL atlas-subregion got=0x%04X want=0x%04X (tile %d,%d); "
               "a page-normalized V-flip would sample tile %d,%d = 0x%04X\n",
               got, (uint16_t)((want_r5<<11)|(want_g6<<5)|want_b5), TX, TY,
               TX, TH/TILE-1-TY, (uint16_t)(((60>>3)<<11)|((100>>2)<<5)|(128>>3)));
        return 0;
    }
    printf("  OK   atlas-subregion  tile(%d,%d) sampled correctly (0x%04X)\n", TX, TY, got);
    return 1;
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
    if (!case_intra_frame_no_alias()) { printf("FAIL mfgpu-intra-frame-no-alias\n"); ok = 0; }
    else printf("raster_backend mfgpu-intra-frame-no-alias OK\n");
    if (!case_stage_noop()) { printf("FAIL mfgpu-stage-noop\n"); ok = 0; }
    else printf("raster_backend mfgpu-stage-noop OK\n");
    if (!case_sdram_residency()) { printf("FAIL mfgpu-sdram-residency\n"); ok = 0; }
    else printf("raster_backend mfgpu-sdram-residency OK\n");
    if (!case_subregion_matches_wholepage()) { printf("FAIL mfgpu-subregion\n"); ok = 0; }
    else printf("raster_backend mfgpu-subregion OK\n");
    if (!case_subregion_batched_multi()) { printf("FAIL mfgpu-subregion-batch\n"); ok = 0; }
    else printf("raster_backend mfgpu-subregion-batch OK\n");
    if (!case_fallback_odd_tricount()) { printf("FAIL mfgpu-fallback-odd\n"); ok = 0; }
    else printf("raster_backend mfgpu-fallback-odd OK\n");
    if (!case_nearfullpage()) { printf("FAIL mfgpu-nearfull\n"); ok = 0; }
    else printf("raster_backend mfgpu-nearfull OK\n");
    if (!case_edge_sprite()) { printf("FAIL mfgpu-edge-sprite\n"); ok = 0; }
    else printf("raster_backend mfgpu-edge-sprite OK\n");
    if (!case_pin_aware_insert()) { printf("FAIL mfgpu-pin-aware-insert\n"); ok = 0; }
    else printf("raster_backend mfgpu-pin-aware-insert OK\n");
    // Placed last: sets g_appSurfFbo/Tex (backend_mfgpu) nonzero mid-run, which
    // would change dst_is_appsurf's outcome for any earlier-in-main() case that
    // happens to share an FBO/tex id (none do today, but keep this last so a
    // future case can't be silently affected without noticing).
    if (!case_surface_route()) { printf("FAIL mfgpu-surface-route\n"); ok = 0; }
    else printf("raster_backend mfgpu-surface-route OK\n");
    if (!case_appsurf_composite_vflip()) { printf("FAIL mfgpu-composite-vflip\n"); ok = 0; }
    else printf("raster_backend mfgpu-composite-vflip OK\n");
    if (!case_atlas_subregion_tile()) { printf("FAIL mfgpu-atlas-subregion\n"); ok = 0; }
    else printf("raster_backend mfgpu-atlas-subregion OK\n");
    return ok ? 0 : 1;
}
