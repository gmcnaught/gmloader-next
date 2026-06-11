//
//  MiSTer 2D software blitter — P2 step 2: software triangle rasterizer.
//  See blitter_raster.h for the API and pipeline contract. Scalar, correct,
//  GL-free. Standard barycentric edge-function rasterizer with a top-left fill
//  rule; nearest texture sampling with wrap-repeat; per-pixel alpha test + blend.
//
#include "blitter_raster.h"

#include <math.h>
#include <stdint.h>
#include <stddef.h>   // size_t

namespace {

// ---- small helpers ----------------------------------------------------------

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float clamp01(float x) {
    return clampf(x, 0.0f, 1.0f);
}

// 0..255 -> 0..1
static inline float u8_to_f(uint8_t b) {
    return (float)b * (1.0f / 255.0f);
}

// 0..1 -> 0..255 with rounding + clamp.
static inline uint8_t f_to_u8(float x) {
    float s = clamp01(x) * 255.0f + 0.5f;
    if (s > 255.0f) s = 255.0f;
    return (uint8_t)s;
}

// Floor-based modulo into [0, n) — implements GL wrap-repeat for a texel index.
static inline int wrap_repeat(int i, int n) {
    if (n <= 0) return 0;
    int m = i % n;
    if (m < 0) m += n;
    return m;
}

// One RGBA texel as floats (R,G,B,A byte order). Nearest sample, wrap-repeat.
struct FRGBA { float r, g, b, a; };

static inline FRGBA sample_tex(const RTexture *tex, float u, float v) {
    if (!tex || !tex->valid || !tex->rgba || tex->w <= 0 || tex->h <= 0) {
        // Untextured / unusable: opaque white so frag = vertex colour.
        return FRGBA{ 1.0f, 1.0f, 1.0f, 1.0f };
    }
    // Nearest: tx = floor(u * w), wrapped into range. Non-finite uv -> texel 0.
    int tx = 0, ty = 0;
    if (isfinite(u)) tx = wrap_repeat((int)floorf(u * (float)tex->w), tex->w);
    if (isfinite(v)) ty = wrap_repeat((int)floorf(v * (float)tex->h), tex->h);
    const uint8_t *p = tex->rgba + ((size_t)ty * (size_t)tex->w + (size_t)tx) * 4;
    return FRGBA{ u8_to_f(p[0]), u8_to_f(p[1]), u8_to_f(p[2]), u8_to_f(p[3]) };
}

// Signed area of the triangle (a,b,c) in screen space, *2. >0 / <0 by winding.
static inline float edge(float ax, float ay, float bx, float by,
                         float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

// Top-left fill rule: a sample exactly on an edge belongs to the triangle iff
// that edge is a "top" or "left" edge. Operates on the edge vector (a -> b) of
// the edge OPPOSITE the vertex whose weight this edge function produces.
static inline bool is_top_left(float ax, float ay, float bx, float by) {
    bool top  = (ay == by) && (bx < ax);   // horizontal edge on top (CW screen y-down)
    bool left = (by > ay);                  // edge goes downward
    return top || left;
}

} // namespace

// ---- public: clear ----------------------------------------------------------
void Blitter_ClearSurface(RSurface *s, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!s || !s->rgba || s->w <= 0 || s->h <= 0) return;
    size_t n = (size_t)s->w * (size_t)s->h;
    uint8_t *p = s->rgba;
    for (size_t i = 0; i < n; ++i) {
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
        p += 4;
    }
}

// ---- public: rasterize one triangle -----------------------------------------
void Blitter_RasterTri(RSurface *dst, const BVtx v[3], const RTexture *tex,
                       RBlend blend, float alphaRef) {
    if (!dst || !dst->rgba || dst->w <= 0 || dst->h <= 0) return;

    const BVtx &v0 = v[0], &v1 = v[1], &v2 = v[2];

    // Reject any non-finite vertex position — keeps the bbox / area math safe.
    if (!isfinite(v0.x) || !isfinite(v0.y) ||
        !isfinite(v1.x) || !isfinite(v1.y) ||
        !isfinite(v2.x) || !isfinite(v2.y)) {
        return;
    }

    // Signed area (*2). Zero => degenerate; sign => winding. We accept both
    // windings (2D sprites arrive either way) by normalizing the bary weights.
    float area = edge(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0.0f || !isfinite(area)) return;
    float invArea = 1.0f / area;

    // Bounding box, clamped to the surface. Pixel centres at (x+0.5, y+0.5).
    float minxf = fminf(v0.x, fminf(v1.x, v2.x));
    float minyf = fminf(v0.y, fminf(v1.y, v2.y));
    float maxxf = fmaxf(v0.x, fmaxf(v1.x, v2.x));
    float maxyf = fmaxf(v0.y, fmaxf(v1.y, v2.y));

    int minx = (int)floorf(minxf);
    int miny = (int)floorf(minyf);
    int maxx = (int)ceilf(maxxf);
    int maxy = (int)ceilf(maxyf);

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > dst->w) maxx = dst->w;
    if (maxy > dst->h) maxy = dst->h;
    if (minx >= maxx || miny >= maxy) return;   // fully off-surface / empty

    // Top-left fill bias per edge. Each edge Ei is opposite vertex Vi:
    //   e0 = (v1 -> v2)  feeds weight w0 (for v0)
    //   e1 = (v2 -> v0)  feeds weight w1 (for v1)
    //   e2 = (v0 -> v1)  feeds weight w2 (for v2)
    // Bias is applied in the (positive-area) convention; for negative area we
    // flip the comparison sense, so compute a per-edge inclusion threshold.
    bool tl0 = is_top_left(v1.x, v1.y, v2.x, v2.y);
    bool tl1 = is_top_left(v2.x, v2.y, v0.x, v0.y);
    bool tl2 = is_top_left(v0.x, v0.y, v1.x, v1.y);

    bool ccw = (area > 0.0f);   // positive signed area == counter-clockwise here

    for (int y = miny; y < maxy; ++y) {
        float py = (float)y + 0.5f;
        uint8_t *row = dst->rgba + (size_t)y * (size_t)dst->w * 4;

        for (int x = minx; x < maxx; ++x) {
            float px = (float)x + 0.5f;

            // Edge functions = sub-triangle areas (*2) for the bary weights.
            float w0 = edge(v1.x, v1.y, v2.x, v2.y, px, py);
            float w1 = edge(v2.x, v2.y, v0.x, v0.y, px, py);
            float w2 = edge(v0.x, v0.y, v1.x, v1.y, px, py);

            // Inside test honouring winding + top-left rule. For positive area
            // (ccw) a pixel is inside when all wi >= 0; on a shared edge (wi==0)
            // keep it only for top/left edges to avoid double-coverage. For
            // negative area the signs invert.
            bool inside;
            if (ccw) {
                inside =
                    (w0 > 0.0f || (w0 == 0.0f && tl0)) &&
                    (w1 > 0.0f || (w1 == 0.0f && tl1)) &&
                    (w2 > 0.0f || (w2 == 0.0f && tl2));
            } else {
                inside =
                    (w0 < 0.0f || (w0 == 0.0f && tl0)) &&
                    (w1 < 0.0f || (w1 == 0.0f && tl1)) &&
                    (w2 < 0.0f || (w2 == 0.0f && tl2));
            }
            if (!inside) continue;

            // Barycentric coordinates (normalized by signed area).
            float b0 = w0 * invArea;
            float b1 = w1 * invArea;
            float b2 = w2 * invArea;

            // Interpolate uv + vertex colour (2D / ortho: no perspective divide).
            float u = b0 * v0.u + b1 * v1.u + b2 * v2.u;
            float vv = b0 * v0.v + b1 * v1.v + b2 * v2.v;
            float cr = b0 * v0.r + b1 * v1.r + b2 * v2.r;
            float cg = b0 * v0.g + b1 * v1.g + b2 * v2.g;
            float cb = b0 * v0.b + b1 * v1.b + b2 * v2.b;
            float ca = b0 * v0.a + b1 * v1.a + b2 * v2.a;

            // Sample texture, modulate by vertex colour.
            FRGBA t = sample_tex(tex, u, vv);
            float sr = clamp01(cr) * t.r;
            float sg = clamp01(cg) * t.g;
            float sb = clamp01(cb) * t.b;
            float sa = clamp01(ca) * t.a;

            // Alpha test (GL discard semantics: frag.a <= ref is dropped).
            if (sa <= alphaRef) continue;

            uint8_t *d = row + (size_t)x * 4;

            // Composite. Fast path RB_NONE skips reading the destination.
            float or_, og, ob, oa;
            if (blend == RB_NONE) {
                or_ = sr; og = sg; ob = sb; oa = sa;
            } else {
                float dr = u8_to_f(d[0]);
                float dg = u8_to_f(d[1]);
                float db = u8_to_f(d[2]);
                float da = u8_to_f(d[3]);
                switch (blend) {
                    case RB_ALPHA: {
                        float ia = 1.0f - sa;
                        or_ = sr * sa + dr * ia;
                        og  = sg * sa + dg * ia;
                        ob  = sb * sa + db * ia;
                        oa  = sa + da * ia;
                    } break;
                    case RB_PREMULT: {
                        float ia = 1.0f - sa;
                        or_ = sr + dr * ia;
                        og  = sg + dg * ia;
                        ob  = sb + db * ia;
                        oa  = sa + da * ia;
                    } break;
                    case RB_ADD: {
                        or_ = clamp01(sr * sa + dr);
                        og  = clamp01(sg * sa + dg);
                        ob  = clamp01(sb * sa + db);
                        oa  = fmaxf(sa, da);
                    } break;
                    default: {  // unreachable; treat as opaque copy
                        or_ = sr; og = sg; ob = sb; oa = sa;
                    } break;
                }
            }

            d[0] = f_to_u8(or_);
            d[1] = f_to_u8(og);
            d[2] = f_to_u8(ob);
            d[3] = f_to_u8(oa);
        }
    }
}
