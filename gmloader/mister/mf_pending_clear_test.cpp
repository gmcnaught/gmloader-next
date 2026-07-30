// Host unit test for the Phase 4 Stage B deferred full-screen clear
// (mf_pending_clear.h).
//
// The module is deliberately pure -- no I/O, no globals, no device headers --
// so the cover proof and the slot bookkeeping are testable without a MiSTer.
// raster_backend_mfgpu.cpp contributes only the vertices and the resolved
// blend mode.
#include "mf_pending_clear.h"

#include <stdio.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

enum { W = 288, H = 216 };

// The canonical GameMaker full-screen quad: two triangles over the four corners,
// sharing the (0,0)-(W,H) diagonal.
static void full_quad(float *xs, float *ys) {
    const float qx[6] = { 0, (float)W, 0,   (float)W, (float)W, 0        };
    const float qy[6] = { 0, 0,        (float)H, 0,   (float)H, (float)H };
    for (int i = 0; i < 6; i++) { xs[i] = qx[i]; ys[i] = qy[i]; }
}

static void case_full_copy_quad_is_a_cover(void) {
    float xs[6], ys[6]; full_quad(xs, ys);
    CHECK(mf_pc_is_cover(/*blend_copy=*/1, /*nt=*/2, xs, ys, W, H) == 1);
}

// A non-COPY blend does not write every covered pixel (CONST_ALPHA reads dst,
// COLORKEY skips keyed texels), so it can never discharge a clear.
static void case_non_copy_blend_is_not_a_cover(void) {
    float xs[6], ys[6]; full_quad(xs, ys);
    CHECK(mf_pc_is_cover(/*blend_copy=*/0, 2, xs, ys, W, H) == 0);
}

// THE defect a bounding-box test would have: two thin slivers at opposite
// corners have a full-screen bbox and cover almost nothing.
static void case_two_slivers_with_full_bbox_are_not_a_cover(void) {
    const float xs[6] = { 0, 1, 0,        (float)W, (float)W - 1, (float)W };
    const float ys[6] = { 0, 0, 1,        (float)H, (float)H,     (float)H - 1 };
    CHECK(mf_pc_is_cover(1, 2, xs, ys, W, H) == 0);
}

// Both triangles on the SAME three corners: only half the rect is painted twice.
static void case_duplicate_half_is_not_a_cover(void) {
    const float xs[6] = { 0, (float)W, 0,   0, (float)W, 0   };
    const float ys[6] = { 0, 0,        (float)H, 0, 0,   (float)H };
    CHECK(mf_pc_is_cover(1, 2, xs, ys, W, H) == 0);
}

// A quad that falls one pixel short of an edge leaves that edge unpainted.
static void case_quad_short_of_the_edge_is_not_a_cover(void) {
    const float qx[6] = { 0, (float)W - 1, 0,        (float)W - 1, (float)W - 1, 0        };
    const float qy[6] = { 0, 0,            (float)H, 0,            (float)H,     (float)H };
    CHECK(mf_pc_is_cover(1, 2, qx, qy, W, H) == 0);
}

// A quad that OVERHANGS still covers the rect -- conservative in the safe
// direction, so it must be accepted.
static void case_overhanging_quad_is_a_cover(void) {
    const float qx[6] = { -4, (float)W + 4, -4,           (float)W + 4, (float)W + 4, -4           };
    const float qy[6] = { -4, -4,           (float)H + 4, -4,           (float)H + 4, (float)H + 4 };
    CHECK(mf_pc_is_cover(1, 2, qx, qy, W, H) == 1);
}

// Anything that is not a 2-triangle quad is out of scope by construction.
static void case_non_two_triangle_draw_is_not_a_cover(void) {
    float xs[6], ys[6]; full_quad(xs, ys);
    CHECK(mf_pc_is_cover(1, /*nt=*/1, xs, ys, W, H) == 0);
    CHECK(mf_pc_is_cover(1, /*nt=*/4, xs, ys, W, H) == 0);
}

static void case_record_take_and_drop_bookkeeping(void) {
    mf_pc_t p; mf_pc_reset(&p);
    CHECK(mf_pc_pending(&p, 0) == 0);
    CHECK(p.dropped == 0 && p.emitted == 0);

    mf_pc_record(&p, 0, W, H, 0x1234);
    CHECK(mf_pc_pending(&p, 0) == 1);

    int w = 0, h = 0; uint16_t c = 0;
    CHECK(mf_pc_take(&p, 0, &w, &h, &c) == 1);
    CHECK(w == W && h == H && c == 0x1234);
    CHECK(p.emitted == 1 && p.dropped == 0);
    CHECK(mf_pc_pending(&p, 0) == 0);
    CHECK(mf_pc_take(&p, 0, &w, &h, &c) == 0);   // idempotent when empty

    mf_pc_record(&p, 0, W, H, 0x4321);
    mf_pc_drop(&p, 0);
    CHECK(p.dropped == 1 && p.emitted == 1);
    CHECK(mf_pc_pending(&p, 0) == 0);
}

// The two targets are independent: a draw into WORK must not discharge a clear
// that was recorded against APPSURF.
static void case_targets_are_independent(void) {
    mf_pc_t p; mf_pc_reset(&p);
    mf_pc_record(&p, 0, W, H, 0x0001);
    mf_pc_record(&p, 1, W, H, 0x0002);
    int w, h; uint16_t c;
    CHECK(mf_pc_take(&p, 0, &w, &h, &c) == 1);
    CHECK(c == 0x0001);
    CHECK(mf_pc_pending(&p, 1) == 1);
    CHECK(mf_pc_take(&p, 1, &w, &h, &c) == 1);
    CHECK(c == 0x0002);
}

// Two clears with no draw between them: the second provably overwrites the
// first, so the first is a real, countable saving -- not a lost fill.
static void case_second_record_supersedes_the_first(void) {
    mf_pc_t p; mf_pc_reset(&p);
    mf_pc_record(&p, 0, W, H, 0x1111);
    mf_pc_record(&p, 0, W, H, 0x2222);
    CHECK(p.dropped == 1);
    int w, h; uint16_t c;
    CHECK(mf_pc_take(&p, 0, &w, &h, &c) == 1);
    CHECK(c == 0x2222);
    CHECK(p.emitted == 1);
}

// Two triangles that share an EDGE pair rather than a diagonal pair hit all
// four corners between them but do NOT tile the rect: TL,TR,BL and TL,TR,BR
// both contain the top edge, and (0.5W, 0.9H) lies in neither. A corner-union
// test alone accepts this; it must be rejected.
static void case_two_halves_sharing_an_edge_are_not_a_cover(void) {
    const float xs[6] = { 0, (float)W, 0,        0, (float)W, (float)W };
    const float ys[6] = { 0, 0,        (float)H, 0, 0,        (float)H };
    CHECK(mf_pc_is_cover(1, 2, xs, ys, W, H) == 0);
}

// An out-of-range target index must be inert, never a memory error.
static void case_out_of_range_target_is_inert(void) {
    mf_pc_t p; mf_pc_reset(&p);
    mf_pc_record(&p, MF_PC_TARGETS, W, H, 0xFFFF);
    mf_pc_record(&p, -1, W, H, 0xFFFF);
    CHECK(mf_pc_pending(&p, MF_PC_TARGETS) == 0);
    CHECK(mf_pc_pending(&p, -1) == 0);
    CHECK(p.dropped == 0 && p.emitted == 0);
}

int main(void) {
    case_full_copy_quad_is_a_cover();
    case_non_copy_blend_is_not_a_cover();
    case_two_slivers_with_full_bbox_are_not_a_cover();
    case_duplicate_half_is_not_a_cover();
    case_quad_short_of_the_edge_is_not_a_cover();
    case_overhanging_quad_is_a_cover();
    case_non_two_triangle_draw_is_not_a_cover();
    case_record_take_and_drop_bookkeeping();
    case_targets_are_independent();
    case_second_record_supersedes_the_first();
    case_two_halves_sharing_an_edge_are_not_a_cover();
    case_out_of_range_target_is_inert();
    if (g_fail) { printf("mf_pending_clear: FAILURES\n"); return 1; }
    printf("mf_pending_clear: all tests passed\n");
    return 0;
}
