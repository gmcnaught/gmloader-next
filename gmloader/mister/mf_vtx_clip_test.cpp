// Host unit test for the guard-band screen clip (mf_vtx_clip.h) and the
// saturating fixed-point pack (raster_backend_convert.h).
//
// What is being proved, and why each case is here:
//
//   1. The BUG, reproduced against the golden. A quad straddling the int16 12.4
//      wrap boundary is packed, then run through blt_tri.c's OWN bbox + top-left
//      coverage math (reimplemented here, integer-identical). Pre-fix that lights
//      every one of the 216 rows; post-fix it lights none. This is the test that
//      actually tracks the device symptom -- the red full-height bars on the
//      level-1 bridge -- rather than an internal detail of the clipper.
//   2. The fast path is byte-identical. Ordinary geometry, including geometry
//      well off-screen but inside the guard band, must not be touched at all:
//      no re-triangulation, no attribute drift, nothing for a regression to hide
//      in. mf_vtx_needs_clip() must answer false for it.
//   3. The range invariant. Whatever the clipper emits, every vertex must pack
//      without wrapping -- checked by round-tripping through bvtx_to_blt and
//      comparing against the float coordinate.
//   4. Attributes are interpolated, not copied. A clip that kept the original
//      u/v would stretch the texture instead of cutting it.
//   5. Degenerate input (NaN, infinity) is dropped rather than packed.
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "mf_vtx_clip.h"
#include "raster_backend_convert.h"

static int g_fail = 0;

static void ok(const char *name, int cond, const char *detail) {
    if (cond) { fprintf(stderr, "ok   %-42s %s\n", name, detail ? detail : ""); }
    else      { fprintf(stderr, "FAIL %-42s %s\n", name, detail ? detail : ""); g_fail++; }
}

// ── the golden's coverage math, integer-identical to blt_tri.c ───────────────
// SUB=4, pixel-centre sampling at ((px<<4)|8), CCW normalise, top-left bias.
// Reimplemented rather than linked so this test stays dependency-free; it is the
// same twelve lines the fabric and the reference model both execute.
#define SUB 4
#define ONE 16
#define HALF 8

static long long tedge(long long ax, long long ay, long long bx, long long by,
                       long long cx, long long cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}
static int ttop_left(long long ax, long long ay, long long bx, long long by) {
    return (ay == by && bx < ax) || (by > ay);
}

// The pack EXACTLY as it shipped before this fix: a bare narrowing cast, which
// on every compiler we build with truncates to the low 16 bits. Kept so the
// "reproduce the bug" checks below stay real -- bvtx_to_blt now saturates, so
// asking it to wrap would silently make those assertions vacuous.
static int16_t pack_wrapping(float px) { return (int16_t)lroundf(px * 16.0f); }

// Rows of the framebuffer that this triangle list would light, after packing.
// wrapping=1 models the pre-fix packer; wrapping=0 uses production bvtx_to_blt.
static int rows_lit(const BVtx *v, int nt, int wrapping) {
    int lit[BLT_FB_HEIGHT];
    memset(lit, 0, sizeof lit);
    for (int t = 0; t < nt; t++) {
        blt_vtx_t p[3];
        for (int k = 0; k < 3; k++) {
            const BVtx *s = &v[t * 3 + k];
            p[k] = bvtx_to_blt(s, 16, 16);
            if (wrapping) { p[k].x = pack_wrapping(s->x); p[k].y = pack_wrapping(s->y); }
        }
        long long x0 = p[0].x, y0 = p[0].y, x1 = p[1].x, y1 = p[1].y, x2 = p[2].x, y2 = p[2].y;
        long long area = tedge(x0, y0, x1, y1, x2, y2);
        if (area == 0) continue;
        if (area < 0) { long long tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; area = -area; }
        long long lx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        long long hx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        long long ly = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        long long hy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        int minx = (int)(lx >> SUB), maxx = (int)((hx + ONE - 1) >> SUB);
        int miny = (int)(ly >> SUB), maxy = (int)((hy + ONE - 1) >> SUB);
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx >= BLT_FB_WIDTH)  maxx = BLT_FB_WIDTH - 1;
        if (maxy >= BLT_FB_HEIGHT) maxy = BLT_FB_HEIGHT - 1;
        long long b0 = ttop_left(x1, y1, x2, y2) ? 0 : -1;
        long long b1 = ttop_left(x2, y2, x0, y0) ? 0 : -1;
        long long b2 = ttop_left(x0, y0, x1, y1) ? 0 : -1;
        for (int py = miny; py <= maxy; py++) {
            long long sy = ((long long)py << SUB) | HALF;
            for (int px = minx; px <= maxx; px++) {
                long long sx = ((long long)px << SUB) | HALF;
                if (tedge(x1, y1, x2, y2, sx, sy) + b0 < 0) continue;
                if (tedge(x2, y2, x0, y0, sx, sy) + b1 < 0) continue;
                if (tedge(x0, y0, x1, y1, sx, sy) + b2 < 0) continue;
                lit[py] = 1; break;
            }
        }
    }
    int n = 0;
    for (int y = 0; y < BLT_FB_HEIGHT; y++) n += lit[y];
    return n;
}

// A textured axis-aligned quad as two triangles, UVs corner-mapped.
static void quad(BVtx *out, float x, float y, float w, float h) {
    const float px[4] = { x, x + w, x + w, x };
    const float py[4] = { y, y,     y + h, y + h };
    const float qu[4] = { 0, 1, 1, 0 };
    const float qv[4] = { 0, 0, 1, 1 };
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        BVtx b;
        b.x = px[idx[i]]; b.y = py[idx[i]];
        b.u = qu[idx[i]]; b.v = qv[idx[i]];
        b.r = b.g = b.b = b.a = 1.0f;
        out[i] = b;
    }
}

static int packs_in_range(const BVtx *v, int nverts) {
    for (int i = 0; i < nverts; i++) {
        blt_vtx_t p = bvtx_to_blt(&v[i], 16, 16);
        // The pack must be the true coordinate, not a wrapped one.
        if (fabsf((float)p.x / 16.0f - v[i].x) > 0.5f) return 0;
        if (fabsf((float)p.y / 16.0f - v[i].y) > 0.5f) return 0;
    }
    return 1;
}

int main(void) {
    BVtx in[6], out[MF_VTX_CLIP_POLY_MAX * 3 * 2];
    char msg[256];

    // ── 1. the device symptom: a quad straddling the wrap boundary ───────────
    // 24x24 sprite at y=2040: y=2040 packs to +32640, y=2064 to 33024 which
    // wraps to -32512. Pre-fix this is the full-height smear.
    quad(in, 180.0f, 2040.0f, 24.0f, 24.0f);
    {
        // Prove the unfixed behaviour first, so the test fails loudly if the
        // wrap ever stops being reproducible and the rest becomes vacuous.
        BVtx raw[6];
        memcpy(raw, in, sizeof raw);
        for (int i = 0; i < 6; i++) raw[i].y = raw[i].y;  // no clip applied
        int lit_before = rows_lit(raw, 2, /*wrapping=*/1);
        snprintf(msg, sizeof msg, "%d/%d rows lit unclipped", lit_before, BLT_FB_HEIGHT);
        ok("straddle-y reproduces the smear", lit_before == BLT_FB_HEIGHT, msg);
    }
    ok("straddle-y is detected", mf_vtx_needs_clip(in, 6) == 1, NULL);
    {
        int nt = mf_vtx_clip_group(in, 2, out, (int)(sizeof out / sizeof out[0]));
        int lit_after = rows_lit(out, nt, /*wrapping=*/0);
        snprintf(msg, sizeof msg, "tris 2->%d, %d/%d rows lit", nt, lit_after, BLT_FB_HEIGHT);
        // y=2040 is outside the clip rect (216+1024=1240), so it draws nothing --
        // which is exactly what the SW rasterizer does with the same quad.
        ok("straddle-y draws nothing after clip", lit_after == 0, msg);
    }

    // ── straddle in x: full-width band pre-fix ───────────────────────────────
    quad(in, 2040.0f, 100.0f, 24.0f, 24.0f);
    ok("straddle-x is detected", mf_vtx_needs_clip(in, 6) == 1, NULL);
    {
        int nt = mf_vtx_clip_group(in, 2, out, (int)(sizeof out / sizeof out[0]));
        snprintf(msg, sizeof msg, "tris 2->%d", nt);
        ok("straddle-x draws nothing after clip", rows_lit(out, nt, /*wrapping=*/0) == 0, msg);
    }

    // ── the teleport case: wholly past the boundary, wraps coherently ────────
    // y=4200 packs to a clean on-screen y=104 pre-fix -- a phantom sprite.
    quad(in, 100.0f, 4200.0f, 24.0f, 24.0f);
    {
        int lit_before = rows_lit(in, 2, /*wrapping=*/1);
        snprintf(msg, sizeof msg, "%d rows lit unclipped (phantom)", lit_before);
        ok("teleport reproduces the phantom", lit_before > 0, msg);
    }
    {
        int nt = mf_vtx_clip_group(in, 2, out, (int)(sizeof out / sizeof out[0]));
        snprintf(msg, sizeof msg, "tris 2->%d", nt);
        ok("teleport draws nothing after clip", nt == 0 || rows_lit(out, nt, /*wrapping=*/0) == 0, msg);
    }

    // ── a long stretched primitive: straddles regardless of position ─────────
    // Crosses the viewport AND reaches past the boundary, so it must survive the
    // clip with its visible part intact -- the case a plain reject would break.
    quad(in, 140.0f, 100.0f, 24.0f, 3000.0f);
    ok("long quad is detected", mf_vtx_needs_clip(in, 6) == 1, NULL);
    {
        int nt = mf_vtx_clip_group(in, 2, out, (int)(sizeof out / sizeof out[0]));
        int lit_after = rows_lit(out, nt, /*wrapping=*/0);
        snprintf(msg, sizeof msg, "tris 2->%d, %d/%d rows lit, %s", nt, lit_after,
                 BLT_FB_HEIGHT, packs_in_range(out, nt * 3) ? "in range" : "OUT OF RANGE");
        // It genuinely covers the whole screen height from y=100 down, so rows
        // 100..215 must still be lit -- clipping must not blank a real draw.
        ok("long quad keeps its visible part",
           nt > 0 && lit_after == BLT_FB_HEIGHT - 100 && packs_in_range(out, nt * 3), msg);
    }

    // ── 2. fast path: normal and merely-offscreen geometry is untouched ──────
    quad(in, 100.0f, 100.0f, 24.0f, 24.0f);
    ok("on-screen quad takes the fast path", mf_vtx_needs_clip(in, 6) == 0, NULL);
    quad(in, -400.0f, -600.0f, 24.0f, 24.0f);
    ok("offscreen-but-in-band takes fast path", mf_vtx_needs_clip(in, 6) == 0,
       "x=-400,y=-600: representable, so no clip");
    quad(in, 1900.0f, 1900.0f, 24.0f, 24.0f);
    ok("just inside the guard takes fast path", mf_vtx_needs_clip(in, 6) == 0,
       "x,y=1900..1924");

    // ── 3. + 4. clipped output is in range and attributes were interpolated ──
    // A quad from y=-3000 to y=+50: the top is cut at y=-1024, and the u/v at the
    // cut must be interpolated, not the original corner's.
    quad(in, 100.0f, -3000.0f, 24.0f, 3050.0f);
    {
        int nt = mf_vtx_clip_group(in, 2, out, (int)(sizeof out / sizeof out[0]));
        int inrange = packs_in_range(out, nt * 3);
        snprintf(msg, sizeof msg, "tris 2->%d, %s", nt, inrange ? "in range" : "OUT OF RANGE");
        ok("clipped output packs without wrapping", nt > 0 && inrange, msg);

        float vmin = 2.0f, vmax = -1.0f;
        for (int i = 0; i < nt * 3; i++) {
            if (out[i].v < vmin) vmin = out[i].v;
            if (out[i].v > vmax) vmax = out[i].v;
        }
        // The original quad's v runs 0..1 over y=-3000..+50. The surviving band is
        // y=-1024..+50, so the cut edge's v is ((-1024)-(-3000))/3050 = 0.6479.
        const float expect_vmin = (-1024.0f - -3000.0f) / 3050.0f;
        snprintf(msg, sizeof msg, "v range [%.4f..%.4f], cut expected at %.4f",
                 vmin, vmax, expect_vmin);
        ok("clip interpolates texcoords", fabsf(vmin - expect_vmin) < 0.002f && vmax > 0.99f, msg);
    }

    // ── 5. degenerate input is dropped, never packed ─────────────────────────
    quad(in, 100.0f, 100.0f, 24.0f, 24.0f);
    in[1].y = NAN;
    ok("NaN routes to the clipper", mf_vtx_needs_clip(in, 6) == 1, NULL);
    {
        // tri 0 carries the NaN and must be dropped; tri 1 is finite and in-band,
        // so it survives -- a NaN must not take the whole draw with it.
        int nt = mf_vtx_clip_group(in, 2, out, (int)(sizeof out / sizeof out[0]));
        snprintf(msg, sizeof msg, "tris 2->%d", nt);
        ok("NaN triangle dropped, sibling kept", nt == 1, msg);
    }
    quad(in, 100.0f, 100.0f, 24.0f, 24.0f);
    in[0].x = INFINITY;
    {
        int nt = mf_vtx_clip_group(in, 2, out, (int)(sizeof out / sizeof out[0]));
        snprintf(msg, sizeof msg, "tris 2->%d", nt);
        ok("infinite triangle dropped", nt == 1, msg);
    }

    // ── the saturating pack backstop, independent of the clipper ─────────────
    {
        BVtx v; memset(&v, 0, sizeof v);
        v.x = 9000.0f; v.y = -9000.0f;
        blt_vtx_t p = bvtx_to_blt(&v, 16, 16);
        snprintf(msg, sizeof msg, "x=%d y=%d (limits +32767/-32768)", p.x, p.y);
        ok("out-of-range pack saturates, not wraps", p.x == 32767 && p.y == -32768, msg);
        v.x = NAN;
        p = bvtx_to_blt(&v, 16, 16);
        ok("NaN pack saturates", p.x == -32768, NULL);
    }

    fprintf(stderr, g_fail ? "\n%d FAILURE(S)\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
