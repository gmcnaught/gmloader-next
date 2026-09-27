//
//  Host unit test for blitter.cpp's blend-state shadow (Blitter_OnBlendFunc /
//  Blitter_OnBlendEnable).
//
//  Build & run:  make -f Makefile.gmloader blitter-blend-state-test
//
//  Regression cases for two defects measured on device (.62, 2026-09-27):
//   1. Cursed Castilla EX sets glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
//      once during init, BEFORE Blitter_Init() (g_enabled=0), then only toggles
//      GL_BLEND. The old hook returned early while disabled, so the factors
//      stayed at GL_ONE/GL_ZERO for the whole run.
//   2. The old hook used 0 as "factor not given", so GL_ZERO (== 0) could never
//      be recorded as a src or dst factor.
//
#include "blitter.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, bool ok) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) ++g_pass; else ++g_fail;
}
static bool state_is(int en, GLenum src, GLenum dst) {
    int e = -1; GLenum s = 0xFFFF, d = 0xFFFF;
    Blitter_GetBlendState(&e, &s, &d);
    return e == en && s == src && d == dst;
}

int main() {
    // GL defaults before anything is called.
    check("defaults are disabled ONE/ZERO", state_is(0, GL_ONE, GL_ZERO));

    // Case 1: factors set while the blitter is still disabled (pre-Init).
    Blitter_OnBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    setenv("GMLOADER_BLITTER", "1", 1);
    Blitter_Init();
    check("blitter enabled after Init", Blitter_Enabled() == 1);
    check("pre-Init glBlendFunc survives Init",
          state_is(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    Blitter_OnBlendEnable(1);
    check("glEnable(GL_BLEND) keeps the recorded factors",
          state_is(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    Blitter_OnBlendEnable(0);
    check("glDisable(GL_BLEND) keeps the recorded factors",
          state_is(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    Blitter_OnBlendEnable(1);

    // Case 2: GL_ZERO as dst, then as src.
    Blitter_OnBlendFunc(GL_ONE, GL_ZERO);
    check("GL_ZERO recordable as dst", state_is(1, GL_ONE, GL_ZERO));
    Blitter_OnBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Blitter_OnBlendFunc(GL_ZERO, GL_SRC_COLOR);
    check("GL_ZERO recordable as src", state_is(1, GL_ZERO, GL_SRC_COLOR));

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
