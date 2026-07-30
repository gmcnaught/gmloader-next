// Host unit test for the Phase 4 Stage A submit-seam accumulator (mf_seam_stat.h).
//
// The accumulator is deliberately pure — no I/O, no globals, no device headers —
// so the whole decomposition's arithmetic is testable without a MiSTer attached.
// The device wiring in raster_backend_mfgpu.cpp contributes only timestamps.
#include "mf_seam_stat.h"

#include <stdio.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

// The three intervals are consecutive slices of one period, so on a well-formed
// frame they sum to it exactly.
static void case_means_and_identity_closes(void) {
    mf_seam_acc_t a; mf_seam_reset(&a);
    mf_seam_add(&a, 5.0, 10.0, 0.5, 15.5, 16.20);
    mf_seam_add(&a, 7.0,  8.0, 0.5, 15.5, 16.20);
    mf_seam_out_t o; mf_seam_derive(&a, &o);
    CHECK(a.n == 2);
    CHECK(o.suspect == 0);
    CHECK(NEAR(o.host_ms,   6.0));
    CHECK(NEAR(o.block_ms,  9.0));
    CHECK(NEAR(o.pub_ms,    0.5));
    CHECK(NEAR(o.period_ms, 15.5));
}

// A frame whose parts do not sum to its period means a stamp was missed, a
// publish was lost, or the frame was dropped. That must be counted, never
// averaged away.
static void case_identity_fires_on_gap(void) {
    mf_seam_acc_t a; mf_seam_reset(&a);
    mf_seam_add(&a, 5.0, 10.0, 0.5, 15.5, 16.20);   // closes
    mf_seam_add(&a, 5.0, 10.0, 0.5, 20.0, 16.20);   // 4.5 ms unaccounted
    mf_seam_out_t o; mf_seam_derive(&a, &o);
    CHECK(o.suspect == 1);
}

static void case_identity_tolerance_edge(void) {
    mf_seam_acc_t a; mf_seam_reset(&a);
    mf_seam_add(&a, 5.0, 10.0, 0.5, 15.5 + 0.04, 16.20);   // inside tolerance
    CHECK(a.suspect == 0);
    mf_seam_add(&a, 5.0, 10.0, 0.5, 15.5 + 0.06, 16.20);   // outside
    CHECK(a.suspect == 1);
}

static void case_ready_at_window(void) {
    mf_seam_acc_t a; mf_seam_reset(&a);
    for (int i = 0; i < MF_SEAM_WINDOW - 1; i++)
        mf_seam_add(&a, 5.0, 10.0, 0.5, 15.5, 16.20);
    CHECK(!mf_seam_ready(&a));
    mf_seam_add(&a, 5.0, 10.0, 0.5, 15.5, 16.20);
    CHECK(mf_seam_ready(&a));
}

static void case_reset_clears(void) {
    mf_seam_acc_t a; mf_seam_reset(&a);
    mf_seam_add(&a, 5.0, 10.0, 0.5, 20.0, 16.20);
    CHECK(a.n == 1 && a.suspect == 1);
    mf_seam_reset(&a);
    CHECK(a.n == 0 && a.suspect == 0);
    mf_seam_out_t o; mf_seam_derive(&a, &o);
    CHECK(NEAR(o.host_ms, 0.0) && NEAR(o.period_ms, 0.0));   // no divide by zero
}

int main(void) {
    case_means_and_identity_closes();
    case_identity_fires_on_gap();
    case_identity_tolerance_edge();
    case_ready_at_window();
    case_reset_clears();
    printf(g_fail ? "mf-seam-stat FAIL\n" : "mf-seam-stat PASS\n");
    return g_fail;
}
