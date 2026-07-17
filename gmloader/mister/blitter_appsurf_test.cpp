//
//  Host unit test for Task 4 (app-surface BRAM render-target plan, step 1):
//  identify the GameMaker application-surface FBO from blitter.cpp's GL-shadow
//  layer, using the Task 1 empirical rule -- the FBO whose colour-attachment
//  texture is later drawn as a fullscreen quad to the default framebuffer
//  (steady-state device capture: FBO 1 / tex 4).
//
//  Build & run on the dev machine (arm64 host-native clang++, no ARM cross /
//  Docker needed):
//    make -f Makefile.gmloader blitter-appsurf-test
//
//  Unlike blitter_raster_test.cpp (which drives the GL-free rasterizer
//  directly and never touches blitter.cpp), this test links blitter.cpp
//  itself and drives its real GL-shadow entry points (Blitter_On*,
//  Blitter_TryDrawArrays) to simulate one frame's worth of GL calls -- the
//  only way to exercise handle_draw()'s detection logic, which is `static`
//  and has no lower-level seam.
//
#include "blitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static void report(const char *name, bool ok) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) ++g_pass; else ++g_fail;
}

// An identity WORLD_VIEW_PROJECTION, uploaded the way GM actually does it: one
// glUniformMatrix4fv(count=5) call carrying gm_Matrices[0..4], where element
// [4] is memcpy'd into g_wvp unconditionally once count>=5 (see
// Blitter_OnUniformMatrix4fv) -- so `loc` doesn't need to resolve to anything
// for this path; identity means clip-space == input position.
static void upload_identity_wvp() {
    static float ident5[5 * 16] = {0};
    for (int m = 0; m < 5; m++)
        for (int i = 0; i < 4; i++)
            ident5[m * 16 + i * 4 + i] = 1.0f;
    Blitter_OnUniformMatrix4fv(/*loc=*/-1, /*count=*/5, ident5);
}

// Drive one GL_TRIANGLES draw (2 tris, 6 verts, client-side position array,
// no index buffer) through the real hook sequence + Blitter_TryDrawArrays.
// `xy` is 6*2 NDC-space {x,y} pairs (z is implicitly 0).
static void draw_quad(GLuint program, GLuint posAttrIdx, const float *xy) {
    float pos[6 * 3];
    for (int v = 0; v < 6; v++) { pos[v*3+0] = xy[v*2+0]; pos[v*3+1] = xy[v*2+1]; pos[v*3+2] = 0.0f; }
    Blitter_OnUseProgram(program);
    Blitter_OnBindAttribLocation(program, posAttrIdx, "in_Position");
    upload_identity_wvp();
    Blitter_OnEnableVertexAttrib(posAttrIdx, 1);
    Blitter_OnVertexAttribPointer(posAttrIdx, 3, GL_FLOAT, GL_FALSE, 0, pos);
    Blitter_TryDrawArrays(GL_TRIANGLES, 0, 6);
}

// NDC corners covering the full clip volume [-1,-1]..[1,1] -- decodes to a
// screen bbox spanning the whole viewport (default 320x240), the exact
// pattern Blitter_AppSurfaceFBO() looks for (maxx-minx >= g_rw-1, same y).
static const float FULLSCREEN[6*2] = {
    -1,-1,  1,-1,  1, 1,
    -1,-1,  1, 1, -1, 1,
};
// A small quad nowhere near full coverage -- must not trip detection.
static const float PARTIAL[6*2] = {
    -0.1f,-0.1f,  0.1f,-0.1f,  0.1f, 0.1f,
    -0.1f,-0.1f,  0.1f, 0.1f, -0.1f, 0.1f,
};

// ---- Case 1: a partial-coverage draw to the default fb, sampling an FBO's
// colour texture, must NOT (mis)trigger detection. Runs FIRST, while
// Blitter_AppSurfaceFBO()/Tex() are still at their untouched initial state --
// detection has no reset (matches production: "updates once the pattern is
// seen", never un-detects), so this ordering is the only way to observe the
// not-yet-detected state at all. */
static void test_appsurf_detect_partial_draw_ignored() {
    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 3);
    Blitter_OnFramebufferTexture2D(GL_COLOR_ATTACHMENT0, 9);

    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 0);
    Blitter_OnBindTexture(GL_TEXTURE_2D, 9);
    draw_quad(/*program=*/2, /*posAttrIdx=*/0, PARTIAL);

    report("partial-coverage draw does not (mis)trigger detection",
           Blitter_AppSurfaceFBO() == 0u && Blitter_AppSurfaceTex() == 0u);
}

// ---- Case 2: the real pattern -- render into FBO 7 (colour tex 6), then
// draw tex 6 fullscreen to the default framebuffer -- identifies FBO 7 / tex
// 6 as the application surface. Mirrors the brief's TEST(appsurf_detect),
// using FBO/tex ids 7/6 as given there (Task 1's actual device capture found
// FBO 1 / tex 4 -- arbitrary GL names either way; the RULE is what's tested).
static void test_appsurf_detect() {
    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 7);
    Blitter_OnFramebufferTexture2D(GL_COLOR_ATTACHMENT0, 6);
    // (A real scene draws sprites into FBO 7 here; detection only cares about
    // the later fbo=0 fullscreen sample, so no draw-into-FBO-7 call is needed
    // to exercise it -- see blitter.cpp's handle_draw: the check fires only
    // when g_curFBO==0.)

    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 0);
    Blitter_OnBindTexture(GL_TEXTURE_2D, 6);
    draw_quad(/*program=*/1, /*posAttrIdx=*/0, FULLSCREEN);

    report("fullscreen-blit-of-FBO-texture identifies FBO 7 (tex 6) as the app surface",
           Blitter_AppSurfaceFBO() == 7u && Blitter_AppSurfaceTex() == 6u);
}

// ---- Case 3: a second, later FBO/texture pair drawn fullscreen updates the
// detection -- "updates once the pattern is seen" (per the brief's Interfaces
// note), not latched to the first match forever.
static void test_appsurf_detect_updates() {
    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 11);
    Blitter_OnFramebufferTexture2D(GL_COLOR_ATTACHMENT0, 12);

    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 0);
    Blitter_OnBindTexture(GL_TEXTURE_2D, 12);
    draw_quad(/*program=*/1, /*posAttrIdx=*/0, FULLSCREEN);

    report("a later fullscreen-blit-of-FBO-texture updates the detection",
           Blitter_AppSurfaceFBO() == 11u && Blitter_AppSurfaceTex() == 12u);
}

// ---- Case 4 (reviewer-found regression): a deleted FBO's stale
// g_fboColorTex attachment record must not survive to false-match a later
// GL id reuse. FBO 20's color attachment is tex 21; tex 21 is deleted (GL
// apps commonly recreate surfaces -- Task 1's Appendix A found this exact
// game recycles names across room/surface transitions); id 21 is then
// reused as FBO 22's attachment. A fullscreen draw sampling tex 21 must
// detect FBO 22 (the live attachment), not the deleted FBO 20 -- without
// the prune in Blitter_OnDeleteTexture, std::map's key-ordered iteration
// (20 < 22) would hit the stale {20:21} entry first and misdetect FBO 20.
static void test_appsurf_detect_ignores_deleted_fbo_attachment() {
    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 20);
    Blitter_OnFramebufferTexture2D(GL_COLOR_ATTACHMENT0, 21);
    Blitter_OnDeleteTexture(21);   // FBO 20 is defunct; should prune {20:21}

    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 22);
    Blitter_OnFramebufferTexture2D(GL_COLOR_ATTACHMENT0, 21);   // id 21 reused

    Blitter_OnBindFramebuffer(GL_FRAMEBUFFER, 0);
    Blitter_OnBindTexture(GL_TEXTURE_2D, 21);
    draw_quad(/*program=*/1, /*posAttrIdx=*/0, FULLSCREEN);

    report("a deleted FBO's stale attachment does not shadow a reused id's live FBO",
           Blitter_AppSurfaceFBO() == 22u && Blitter_AppSurfaceTex() == 21u);
}

int main() {
    setenv("GMLOADER_BLITTER", "1", 1);   // level 1: shadow+decode; no GL fallback needed
    Blitter_Init();

    test_appsurf_detect_partial_draw_ignored();
    test_appsurf_detect();
    test_appsurf_detect_updates();
    test_appsurf_detect_ignores_deleted_fbo_attachment();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
