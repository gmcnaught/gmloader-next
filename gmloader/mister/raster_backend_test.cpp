// Host unit test for the RasterBackend seam (Task 3) and the backend_mfgpu
// skeleton (Task 4). Task 3's case proves the refactor is pixel-neutral:
// driving a small triangle through backend_sw->draw() must produce a
// byte-identical RSurface to calling Blitter_RasterDraw directly. Task 4's
// case proves backend_mfgpu->clear, executed on the host through the SAME
// blt_execute software model the mfgpu refmodel unit tests use, matches
// backend_sw->clear in present-space RGB565.
#include "raster_backend.h"
#include "blitter_raster.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

// backend_mfgpu (Task 4) + its host-only readback hook. Declared here rather
// than via raster_backend.h because RasterBackend_MFGPU_TestCopyFB565 is not
// part of the vtable seam — it exists solely so this host test can read the
// fixed-size fabric framebuffer blt_execute composited into.
extern "C" const RasterBackend backend_sw;
extern "C" const RasterBackend backend_mfgpu;
extern "C" void RasterBackend_MFGPU_TestCopyFB565(int w, int h, uint16_t *out);

// Mirrors Blitter_ToRGB565's per-pixel packing formula (gmloader/mister/
// blitter.cpp) without linking that file: it lives inside #ifdef
// MISTER_NATIVE_VIDEO and depends on GL-decode globals (g_rw/g_rh) plus the
// fixed MISTER_WIDTH/MISTER_HEIGHT letterbox macros, none of which apply to
// this same-size host comparison. `fb565` must be a tightly-packed
// sw->w * sw->h buffer (as RasterBackend_MFGPU_TestCopyFB565 produces).
// Reusable comparator — Task 5's triangle-parity test needs the same ±1 LSB
// per-channel (5/6/5) tolerance check.
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

int main(void){
    int ok = 1;
    if (!one_case()) { printf("FAIL sw-equivalence\n"); ok = 0; }
    else printf("raster_backend sw-equivalence OK\n");
    if (!case_clear_parity()) { printf("FAIL mfgpu-clear-parity\n"); ok = 0; }
    else printf("raster_backend mfgpu-clear-parity OK\n");
    return ok ? 0 : 1;
}
