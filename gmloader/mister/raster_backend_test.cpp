// Host unit test for the RasterBackend seam (Task 3). Proves the refactor is
// pixel-neutral: driving a small triangle through backend_sw->draw() must
// produce a byte-identical RSurface to calling Blitter_RasterDraw directly.
#include "raster_backend.h"
#include "blitter_raster.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
int main(void){ if(!one_case()){ printf("FAIL sw-equivalence\n"); return 1; }
    printf("raster_backend sw-equivalence OK\n"); return 0; }
