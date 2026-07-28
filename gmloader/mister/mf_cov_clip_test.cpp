// Host unit test (TDD) for the covered-pixel estimator's triangle-vs-render-
// target clipping. See the long comment at mf_clip_tri_area's definition in
// raster_backend_mfgpu.cpp for the algorithm (Sutherland-Hodgman against
// [0,BLT_FB_WIDTH] x [0,BLT_FB_HEIGHT], shoelace area).
//
// This proves the CLIPPING GEOMETRY in isolation, independent of mf_stat_on(),
// the accumulator, or any fabric/device state — RasterBackend_MFGPU_TestClipTriArea
// is a thin host-test-only wrapper around the pure function.
//
// Background: mf_cov_add_triangle() used to sum each triangle's FULL geometric
// area (no clipping) and merely clamp the per-triangle result to one screen's
// worth of pixels. Two screenshot-confirmed device scenes showed cyc/px varying
// 2.9x (title 8.67 vs gameplay 3.01) purely because gameplay has more offscreen
// geometry (scrolling parallax past both edges) — proof the old estimator counted
// off-screen area. Clipping fixes that at the source.
#include <stdio.h>
#include <math.h>

extern "C" double RasterBackend_MFGPU_TestClipTriArea(float x0, float y0, float x1, float y1,
                                                       float x2, float y2);
// Render-target geometry the production code clips against — pulled from the
// same macros raster_backend_mfgpu.cpp uses (refmodel/blitter_ref.h), never a
// literal, so this test tracks a geometry change automatically.
extern "C" {
#include "blitter_ref.h"
}

static int g_fail = 0;

static void check_close(const char *name, double got, double expect, double eps) {
    if (fabs(got - expect) > eps) {
        fprintf(stderr, "FAIL %-28s got=%.6f expect=%.6f (eps=%.6f)\n", name, got, expect, eps);
        g_fail++;
    } else {
        fprintf(stderr, "ok   %-28s got=%.6f expect=%.6f\n", name, got, expect);
    }
}

int main(void) {
    const double W = (double)BLT_FB_WIDTH;
    const double H = (double)BLT_FB_HEIGHT;
    const double EPS = 1e-6;

    // 1) Fully inside: area unchanged (untouched by clipping).
    {
        double got = RasterBackend_MFGPU_TestClipTriArea(10, 10, 50, 10, 10, 50);
        check_close("fully-inside", got, 800.0, EPS);   // 0.5*40*40
        // Reversed winding must give the identical area (winding independence, #6).
        double got_rev = RasterBackend_MFGPU_TestClipTriArea(10, 10, 10, 50, 50, 10);
        check_close("fully-inside (reversed)", got_rev, 800.0, EPS);
    }

    // 2) Fully outside: contributes zero, NOT a clamped full screen.
    {
        double got = RasterBackend_MFGPU_TestClipTriArea((float)(W + 10), 10,
                                                          (float)(W + 50), 10,
                                                          (float)(W + 10), 50);
        check_close("fully-outside (right)", got, 0.0, EPS);
        double got_top = RasterBackend_MFGPU_TestClipTriArea(10, (float)(-(H + 10)),
                                                              50, (float)(-(H + 10)),
                                                              10, (float)(-(H + 50)));
        check_close("fully-outside (above)", got_top, 0.0, EPS);
    }

    // 3) Straddles the right edge: hand-computed clipped area.
    //    A=(W-8,100) B=(W+12,100) C=(W-8,140) — full triangle area = 0.5*20*40=400.
    //    The clip plane x<=W cuts off a similar right triangle with legs
    //    (W+12-W)=12 and 124-100=24 (since the AB/BC intersections land at
    //    x=W, y=100 and x=W, y=124 respectively — see the derivation in the
    //    task ledger) => clipped-off area = 0.5*12*24 = 144, so the surviving
    //    clipped area is 400-144 = 256. This offset construction is
    //    translation-invariant in W (only the fixed offsets -8/+12/40 matter),
    //    so the expected value holds even if BLT_FB_WIDTH changes.
    {
        float ax = (float)(W - 8), ay = 100, bx = (float)(W + 12), by = 100,
              cx = (float)(W - 8), cy = 140;
        double got = RasterBackend_MFGPU_TestClipTriArea(ax, ay, bx, by, cx, cy);
        check_close("straddle-right-edge", got, 256.0, EPS);
        // Reversed winding (swap B and C) must give the same clipped area (#6).
        double got_rev = RasterBackend_MFGPU_TestClipTriArea(ax, ay, cx, cy, bx, by);
        check_close("straddle-right-edge (reversed)", got_rev, 256.0, EPS);
    }

    // 4) Much larger than the screen and fully covering it: contributes
    //    EXACTLY one screen area, not the triangle's own (enormous) area.
    {
        double K = 10.0 * (W + H);   // safely bigger than the far corner (W,H)
        float ax = (float)-K, ay = (float)-K;
        float bx = (float)(2.0 * K), by = (float)-K;
        float cx = (float)-K, cy = (float)(2.0 * K);
        double got = RasterBackend_MFGPU_TestClipTriArea(ax, ay, bx, by, cx, cy);
        check_close("covers-whole-screen", got, W * H, 1e-3);
    }

    // 5) Degenerate/zero-area triangles: contribute zero, no NaN.
    {
        double got_point = RasterBackend_MFGPU_TestClipTriArea(50, 50, 50, 50, 50, 50);
        check_close("degenerate-point", got_point, 0.0, EPS);
        if (isnan(got_point)) { fprintf(stderr, "FAIL degenerate-point produced NaN\n"); g_fail++; }

        double got_line = RasterBackend_MFGPU_TestClipTriArea(10, 10, 20, 20, 30, 30);
        check_close("degenerate-colinear", got_line, 0.0, EPS);
        if (isnan(got_line)) { fprintf(stderr, "FAIL degenerate-colinear produced NaN\n"); g_fail++; }

        // Degenerate AND straddling the clip boundary (forces a divide inside
        // an edge-intersection branch) — still must not be NaN.
        double got_edge = RasterBackend_MFGPU_TestClipTriArea((float)(W - 5), 10,
                                                               (float)(W + 5), 10,
                                                               (float)(W - 5), 10);
        check_close("degenerate-on-clip-line", got_edge, 0.0, EPS);
        if (isnan(got_edge)) { fprintf(stderr, "FAIL degenerate-on-clip-line produced NaN\n"); g_fail++; }
    }

    if (g_fail) {
        fprintf(stderr, "\n%d CHECK(S) FAILED\n", g_fail);
        return 1;
    }
    fprintf(stderr, "\nALL CHECKS PASSED\n");
    return 0;
}
