// prim_assemble.h — GL primitive assembly: fans and strips -> a triangle LIST.
//
// The blitter's rasterize path (blitter.cpp handle_draw) and both raster
// backends consume a flat triangle list: 3N vertices, N triangles, no implicit
// sharing. GL's GL_TRIANGLE_FAN / GL_TRIANGLE_STRIP share vertices between
// adjacent triangles, so they have to be expanded before they get there.
//
// This is not a nicety. handle_draw's gate used to be `mode == GL_TRIANGLES`,
// and GameMaker draws quads as fans -- so on Cursed Castilla EX every
// app-surface (rt=FBO) draw was a 5-vertex fan and every one of them was
// dropped, silently, while the fbo=0 GL_TRIANGLES draws rendered fine. That is
// what an unchanging app surface looked like from the outside.
//
// Header-only and dependency-free (like mf_seam_stat.h) so it can be unit
// tested on the host without dragging in GL or the backend link set.
#pragma once
#include <vector>

// GL enum values, spelled out so this header needs no GL headers.
#ifndef PRIM_GL_TRIANGLES
#define PRIM_GL_TRIANGLES      0x0004u
#define PRIM_GL_TRIANGLE_STRIP 0x0005u
#define PRIM_GL_TRIANGLE_FAN   0x0006u
#endif

// True if `mode` is a primitive this header expands (i.e. NOT already a list).
inline bool prim_needs_assembly(unsigned mode) {
    return mode == PRIM_GL_TRIANGLE_STRIP || mode == PRIM_GL_TRIANGLE_FAN;
}

// Expand `count` vertices of `mode` from `in` into a triangle list in `out`.
// Returns the number of vertices written (3 per triangle), or 0 if `mode` is
// not a fan/strip or there are too few vertices to form one triangle.
//
// Winding: GL's rule is reproduced -- a fan is (v0, vi+1, vi+2), a strip
// alternates (vi, vi+1, vi+2) / (vi+1, vi, vi+2) so every triangle faces the
// same way. Neither rasterizer is winding-sensitive today (blitter_raster.cpp
// picks its inside test from the sign of the signed area; blt_tri.c
// CCW-normalizes by swapping b/c), so this only matters if a backface test is
// ever added -- but getting it wrong now would be invisible until then.
template <class V>
inline int prim_assemble(unsigned mode, const V *in, int count, std::vector<V> &out) {
    out.clear();
    if (!prim_needs_assembly(mode) || count < 3 || !in) return 0;
    out.reserve((size_t)(count - 2) * 3);
    for (int i = 0; i + 2 < count; i++) {
        if (mode == PRIM_GL_TRIANGLE_FAN) {
            out.push_back(in[0]);
            out.push_back(in[i + 1]);
            out.push_back(in[i + 2]);
        } else if (i & 1) {                 // odd strip triangle: GL flips winding
            out.push_back(in[i + 1]);
            out.push_back(in[i]);
            out.push_back(in[i + 2]);
        } else {
            out.push_back(in[i]);
            out.push_back(in[i + 1]);
            out.push_back(in[i + 2]);
        }
    }
    return (int)out.size();
}
