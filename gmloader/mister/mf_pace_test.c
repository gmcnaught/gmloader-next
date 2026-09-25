/* Host unit test for mf_pace.h — the scanout-locked doorbell pacer's decision.
 * Build+run: make -f Makefile.gmloader mf-pace-test (from repo root).
 */
#include "mf_pace.h"
#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

int main(void)
{
    mf_pace_t p = {0, 0};
    CHECK(mf_pace_ready(&p, 1234), "first doorbell never waits");
    mf_pace_rang(&p, 1234);
    CHECK(!mf_pace_ready(&p, 1234), "second doorbell in the same scanout frame waits");
    CHECK(mf_pace_ready(&p, 1235), "next boundary releases it");
    CHECK(mf_pace_ready(&p, 1240), "a late host (several boundaries passed) never waits");
    mf_pace_rang(&p, 1240);
    CHECK(!mf_pace_ready(&p, 1240), "one doorbell per scanout frame after a late one too");
    /* 2^32 wrap of scan_frame_cnt */
    mf_pace_rang(&p, 0xFFFFFFFFu);
    CHECK(!mf_pace_ready(&p, 0xFFFFFFFFu), "wrap: same value waits");
    CHECK(mf_pace_ready(&p, 0u), "wrap: 0xFFFFFFFF -> 0 is a boundary");
    /* core reload: the counter restarts from 0 */
    mf_pace_rang(&p, 5000);
    CHECK(mf_pace_ready(&p, 3), "a counter that went backwards (core reload) re-bases");
    if (failures) { printf("mf_pace_test: %d failure(s)\n", failures); return 1; }
    printf("mf_pace_test: all passed\n");
    return 0;
}
