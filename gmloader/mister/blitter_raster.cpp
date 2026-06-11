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
#include <pthread.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace {

// ---- small helpers ----------------------------------------------------------

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float clamp01(float x) {
    return clampf(x, 0.0f, 1.0f);
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

// Divide a 0..65025 product by 255, rounded — the standard (x + 0x80 ...) trick.
static inline int div255(int x) { x += 0x80; return (x + (x >> 8)) >> 8; }

static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

// ---- fixed-point scheme -----------------------------------------------------
// Attributes (u,v in *texel* units, colour in 0..255) are carried in 16.16
// fixed-point and stepped per pixel/row with integer adds.  The edge functions
// are carried in int64 (bbox-area*2 can reach ~150k and the per-pixel float
// product can be large; int64 is overflow-safe and on A9 a single 64-bit add is
// cheap relative to a VFP op).  We snap the *float* edge setup to integer once
// at setup, preserving the top-left rule via the sign of the snapped value.
static const int FXSH = 16;
static const int FXONE = 1 << FXSH;

// Composite one source RGBA (already modulated + alpha-tested) into dst pixel.
// blend is taken as a template-ish switch by the caller's mode.
static inline void blend_pixel(uint8_t *d, int sr, int sg, int sb, int sa,
                               RBlend blend) {
    if (blend == RB_NONE) {
        d[0]=(uint8_t)sr; d[1]=(uint8_t)sg; d[2]=(uint8_t)sb; d[3]=(uint8_t)sa;
        return;
    }
    int dr=d[0], dg=d[1], db=d[2], da=d[3], ia=255-sa, o;
    switch (blend) {
        case RB_ALPHA:
            d[0]=div255(sr*sa+dr*ia); d[1]=div255(sg*sa+dg*ia);
            d[2]=div255(sb*sa+db*ia); o=sa+div255(da*ia); d[3]=o>255?255:o;
            break;
        case RB_PREMULT:
            o=sr+div255(dr*ia); d[0]=o>255?255:o; o=sg+div255(dg*ia); d[1]=o>255?255:o;
            o=sb+div255(db*ia); d[2]=o>255?255:o; o=sa+div255(da*ia); d[3]=o>255?255:o;
            break;
        case RB_ADD:
            o=div255(sr*sa)+dr; d[0]=o>255?255:o; o=div255(sg*sa)+dg; d[1]=o>255?255:o;
            o=div255(sb*sa)+db; d[2]=o>255?255:o; d[3]=(uint8_t)(sa>da?sa:da);
            break;
        default:
            d[0]=(uint8_t)sr; d[1]=(uint8_t)sg; d[2]=(uint8_t)sb; d[3]=(uint8_t)sa; break;
    }
}

#if defined(__ARM_NEON)
// Vectorized RB_ALPHA / RB_NONE composite for up to-8 source pixels.  Operates
// on de-interleaved channel vectors (8x u8 each).  `cov` is a per-lane mask
// (0xFF keep / 0x00 skip) folding both triangle coverage and the alpha test.
// Source is assumed already modulated.  Returns nothing; writes dst in place.
static inline void blend8_alpha_neon(uint8_t *d /*32 bytes RGBA*/,
                                     uint8x8_t sr, uint8x8_t sg,
                                     uint8x8_t sb, uint8x8_t sa,
                                     uint8x8_t cov, int blend) {
    uint8x8x4_t dpix = vld4_u8(d);           // dr,dg,db,da
    uint8x8_t ia = vsub_u8(vdup_n_u8(255), sa);
    // (x*255 + 0x80); (t + (t>>8))>>8 == div255, done in 16-bit lanes.
    auto div255_16 = [](uint16x8_t t) -> uint8x8_t {
        t = vaddq_u16(t, vdupq_n_u16(0x80));
        t = vaddq_u16(t, vshrq_n_u16(t, 8));
        return vshrn_n_u16(t, 8);
    };
    uint8x8_t or_, og, ob, oa;
    if (blend == RB_NONE) {
        or_ = sr; og = sg; ob = sb; oa = sa;
    } else { // RB_ALPHA
        or_ = div255_16(vaddq_u16(vmull_u8(sr, sa), vmull_u8(dpix.val[0], ia)));
        og  = div255_16(vaddq_u16(vmull_u8(sg, sa), vmull_u8(dpix.val[1], ia)));
        ob  = div255_16(vaddq_u16(vmull_u8(sb, sa), vmull_u8(dpix.val[2], ia)));
        // out.a = sa + div255(da*ia), saturating.
        uint8x8_t t = div255_16(vmull_u8(dpix.val[3], ia));
        oa = vqadd_u8(sa, t);
    }
    // select out where cov, else keep dst.
    uint8x8x4_t res;
    res.val[0] = vbsl_u8(cov, or_, dpix.val[0]);
    res.val[1] = vbsl_u8(cov, og, dpix.val[1]);
    res.val[2] = vbsl_u8(cov, ob, dpix.val[2]);
    res.val[3] = vbsl_u8(cov, oa, dpix.val[3]);
    vst4_u8(d, res);
}
#endif // __ARM_NEON

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

// Static per-draw state consumed by produce_src.  Attribute *increments* live
// in the caller's locals; this carries only what the sampler/modulate needs.
struct QuadSetup {
    int hasTex, tw, th;
    const uint8_t *tpix;
    bool whiteCol;   // vertex colour is all 255 -> skip modulate
};

// Per-pixel source produce (nearest sample + modulate) given 16.16 accumulators.
static inline void produce_src(const QuadSetup &q, int32_t u, int32_t vv,
                               int32_t cr, int32_t cg, int32_t cb, int32_t ca,
                               int &sr, int &sg, int &sb, int &sa) {
    int ccr, ccg, ccb, cca;
    if (q.whiteCol) { ccr=ccg=ccb=cca=255; }
    else {
        ccr = clampi(cr >> FXSH, 0, 255); ccg = clampi(cg >> FXSH, 0, 255);
        ccb = clampi(cb >> FXSH, 0, 255); cca = clampi(ca >> FXSH, 0, 255);
    }
    if (q.hasTex) {
        int tx = (u >> FXSH) % q.tw; if (tx < 0) tx += q.tw;
        int ty = (vv >> FXSH) % q.th; if (ty < 0) ty += q.th;
        const uint8_t *tp = q.tpix + ((size_t)ty * q.tw + tx) * 4;
        if (q.whiteCol) { sr=tp[0]; sg=tp[1]; sb=tp[2]; sa=tp[3]; }
        else { sr=div255(tp[0]*ccr); sg=div255(tp[1]*ccg);
               sb=div255(tp[2]*ccb); sa=div255(tp[3]*cca); }
    } else { sr=ccr; sg=ccg; sb=ccb; sa=cca; }
}

// Blit a contiguous, already-coverage-tested span [x0,x1) on one row.  The
// per-pixel edge test is gone (caller proved coverage); we only sample/modulate,
// alpha-test, and blend.  NEON does 8 px at a time for RB_NONE/RB_ALPHA; scalar
// otherwise.  Attribute accumulators (u..ca) are advanced by reference so the
// caller stays in sync if it needs them afterwards (it doesn't, per row).
static inline void blit_span(const QuadSetup &q, uint8_t *row, int x0, int x1,
                             RBlend blend, int aref,
                             int32_t u, int32_t vv, int32_t cr, int32_t cg,
                             int32_t cb, int32_t ca,
                             int32_t uX, int32_t vX, int32_t rX, int32_t gX,
                             int32_t bX, int32_t aX) {
    int x = x0;
#if defined(__ARM_NEON)
    if (blend == RB_NONE || blend == RB_ALPHA) {
        for (; x + 8 <= x1; x += 8) {
            uint8_t srA[8], sgA[8], sbA[8], saA[8], covarr[8];
            int32_t lu=u, lvv=vv, lcr=cr, lcg=cg, lcb=cb, lca=ca;
            for (int k=0;k<8;++k) {
                int sr,sg,sb,sa;
                produce_src(q, lu, lvv, lcr, lcg, lcb, lca, sr,sg,sb,sa);
                covarr[k]=(sa>aref)?0xFF:0x00;
                srA[k]=(uint8_t)sr; sgA[k]=(uint8_t)sg; sbA[k]=(uint8_t)sb; saA[k]=(uint8_t)sa;
                lu+=uX; lvv+=vX; lcr+=rX; lcg+=gX; lcb+=bX; lca+=aX;
            }
            blend8_alpha_neon(row + (size_t)x*4,
                              vld1_u8(srA), vld1_u8(sgA), vld1_u8(sbA), vld1_u8(saA),
                              vld1_u8(covarr), (int)blend);
            u=lu; vv=lvv; cr=lcr; cg=lcg; cb=lcb; ca=lca;
        }
    }
#endif
    for (; x < x1; ++x, u+=uX, vv+=vX, cr+=rX, cg+=gX, cb+=bX, ca+=aX) {
        int sr,sg,sb,sa;
        produce_src(q, u, vv, cr, cg, cb, ca, sr,sg,sb,sa);
        if (sa <= aref) continue;
        blend_pixel(row + (size_t)x * 4, sr, sg, sb, sa, blend);
    }
}

// ---- public: rasterize one triangle -----------------------------------------
// Edge functions and per-vertex attributes are linear in screen (x,y), so we
// evaluate them once at the bbox origin and step by a constant increment per
// pixel/row (DDA). The edge functions are int64 fixed-point (snapped from the
// float setup); the per-pixel attributes (u,v in texel units, r,g,b,a in 0..255)
// are 16.16 integer. The whole inner loop is integer — no per-pixel float ops.
// Rasterize one triangle, but only for destination rows in the half-open band
// [yLo,yHi).  The band clamp is applied AFTER bbox clipping and uses the same
// half-open convention as the bbox/top-left fill, so a triangle spanning a band
// boundary is covered by exactly one band (the row y belongs to the band whose
// [yLo,yHi) contains it).  yLo/yHi are expected pre-clamped to [0,dst->h] by the
// caller, but we clamp defensively anyway.
static void raster_tri_rows(RSurface *dst, const BVtx v[3], const RTexture *tex,
                            RBlend blend, float alphaRef, int yLo, int yHi) {
    if (!dst || !dst->rgba || dst->w <= 0 || dst->h <= 0) return;
    if (yLo < 0) yLo = 0;
    if (yHi > dst->h) yHi = dst->h;
    if (yLo >= yHi) return;

    const BVtx &v0 = v[0], &v1 = v[1], &v2 = v[2];

    if (!isfinite(v0.x) || !isfinite(v0.y) ||
        !isfinite(v1.x) || !isfinite(v1.y) ||
        !isfinite(v2.x) || !isfinite(v2.y)) {
        return;
    }

    float area = edge(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0.0f || !isfinite(area)) return;
    float invArea = 1.0f / area;

    float minxf = fminf(v0.x, fminf(v1.x, v2.x));
    float minyf = fminf(v0.y, fminf(v1.y, v2.y));
    float maxxf = fmaxf(v0.x, fmaxf(v1.x, v2.x));
    float maxyf = fmaxf(v0.y, fmaxf(v1.y, v2.y));

    int minx = (int)floorf(minxf), miny = (int)floorf(minyf);
    int maxx = (int)ceilf(maxxf),  maxy = (int)ceilf(maxyf);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > dst->w) maxx = dst->w;
    if (maxy > dst->h) maxy = dst->h;
    // `miny`/`minx` anchor the float edge/attribute setup below and MUST be
    // band-independent so every band reproduces the single-threaded float
    // expressions (and thus the identical `lrintf` LSBs) for a given row y.
    // The band only restricts which rows we iterate, via [bandLo,bandHi).
    int bandLo = miny, bandHi = maxy;
    if (bandLo < yLo) bandLo = yLo;
    if (bandHi > yHi) bandHi = yHi;
    // Half-open band clamp: disjoint bands tile the triangle exactly — no
    // double-write, no gap at the boundary.
    if (minx >= maxx || bandLo >= bandHi) return;

    bool tl0 = is_top_left(v1.x, v1.y, v2.x, v2.y);
    bool tl1 = is_top_left(v2.x, v2.y, v0.x, v0.y);
    bool tl2 = is_top_left(v0.x, v0.y, v1.x, v1.y);
    bool ccw = (area > 0.0f);

    // Edge gradients: w = edge(a,b,p) -> dw/dx = -(by-ay), dw/dy = (bx-ax).
    float e0dxf = -(v2.y - v1.y), e0dyf = (v2.x - v1.x);
    float e1dxf = -(v0.y - v2.y), e1dyf = (v0.x - v2.x);
    float e2dxf = -(v1.y - v0.y), e2dyf = (v1.x - v0.x);
    float ox = (float)minx + 0.5f, oy = (float)miny + 0.5f;
    float e0of = edge(v1.x, v1.y, v2.x, v2.y, ox, oy);
    float e1of = edge(v2.x, v2.y, v0.x, v0.y, ox, oy);
    float e2of = edge(v0.x, v0.y, v1.x, v1.y, ox, oy);

    // Attribute gradients: attr = invArea*(a0*w0 + a1*w1 + a2*w2), linear in p.
    float texW = (tex && tex->valid && tex->w > 0) ? (float)tex->w : 1.0f;
    float texH = (tex && tex->valid && tex->h > 0) ? (float)tex->h : 1.0f;
    #define GRADF(a0, a1, a2, gx, gy, go)                                 \
        float gx = invArea * ((a0)*e0dxf + (a1)*e1dxf + (a2)*e2dxf);      \
        float gy = invArea * ((a0)*e0dyf + (a1)*e1dyf + (a2)*e2dyf);      \
        float go = invArea * ((a0)*e0of  + (a1)*e1of  + (a2)*e2of)
    GRADF(v0.u*texW, v1.u*texW, v2.u*texW, uXf, uYf, uOf);
    GRADF(v0.v*texH, v1.v*texH, v2.v*texH, vXf, vYf, vOf);
    GRADF(v0.r*255.f, v1.r*255.f, v2.r*255.f, rXf, rYf, rOf);
    GRADF(v0.g*255.f, v1.g*255.f, v2.g*255.f, gXf, gYf, gOf);
    GRADF(v0.b*255.f, v1.b*255.f, v2.b*255.f, bXf, bYf, bOf);
    GRADF(v0.a*255.f, v1.a*255.f, v2.a*255.f, aXf, aYf, aOf);
    #undef GRADF

    int hasTex = (tex && tex->valid && tex->rgba) ? 1 : 0;
    int tw = (int)texW, th = (int)texH;
    const uint8_t *tpix = hasTex ? tex->rgba : nullptr;
    int aref = (int)(clamp01(alphaRef) * 255.0f);

    // Snap the per-pixel (x-step) attribute increments to 16.16 fixed-point.
    // Texel-space u/v can be large (texW up to 2048) but u,v stay within a few
    // multiples of texW, so the 16.16 integer part has ample headroom (<=~32k).
    // The per-row origin (which folds the y-step) is computed in float once per
    // scanline and snapped there, so the y-step increments aren't needed here.
    const float F = (float)FXONE;
    int32_t uX=(int32_t)lrintf(uXf*F);
    int32_t vX=(int32_t)lrintf(vXf*F);
    int32_t rX=(int32_t)lrintf(rXf*F);
    int32_t gX=(int32_t)lrintf(gXf*F);
    int32_t bX=(int32_t)lrintf(bXf*F);
    int32_t aX=(int32_t)lrintf(aXf*F);

    bool whiteCol = (v0.r>=1.f && v0.g>=1.f && v0.b>=1.f && v0.a>=1.f &&
                     v1.r>=1.f && v1.g>=1.f && v1.b>=1.f && v1.a>=1.f &&
                     v2.r>=1.f && v2.g>=1.f && v2.b>=1.f && v2.a>=1.f);

    QuadSetup q;
    q.hasTex=hasTex; q.tw=tw; q.th=th; q.tpix=tpix; q.whiteCol = whiteCol;

    // Edge functions in fixed-point: scale floats by FXONE then round to int64.
    // Per-pixel int64 add; the inside test is a sign compare.  dw/dx,dw/dy are
    // integers in practice (vertex deltas), so the snapped accumulators are
    // exact when verts are integral and otherwise correct to sub-LSB.
    int64_t e0dx=(int64_t)lrintf(e0dxf*F), e0dy=(int64_t)lrintf(e0dyf*F);
    int64_t e1dx=(int64_t)lrintf(e1dxf*F), e1dy=(int64_t)lrintf(e1dyf*F);
    int64_t e2dx=(int64_t)lrintf(e2dxf*F), e2dy=(int64_t)lrintf(e2dyf*F);
    int64_t e0o=(int64_t)llrintf((double)e0of*F);
    int64_t e1o=(int64_t)llrintf((double)e1of*F);
    int64_t e2o=(int64_t)llrintf((double)e2of*F);

    // For each edge, narrow the covered [lo,hi) column interval (relative to
    // minx) on a scanline.  The edge value at column x is ev(x)=e_at+edx*x,
    // linear in x; "inside" is ev>0 (ccw) / ev<0 (cw), with the top-left rule
    // allowing ev==0 when tl.  Because the inside set is a half-line in x, we
    // solve its integer boundary directly (one division per edge per row) and
    // clip [lo,hi) to it — no per-pixel edge test.  Edges parallel to the
    // scanline (edx==0) gate the whole row in or out.
    auto narrow = [&](int64_t e_at, int64_t edx, bool tl, int &lo, int &hi) {
        if (edx == 0) {
            bool in = ccw ? (e_at > 0 || (e_at == 0 && tl))
                          : (e_at < 0 || (e_at == 0 && tl));
            if (!in) hi = lo;               // exclude the whole row
            return;
        }
        // ev(x)=e_at+edx*x is strictly monotonic (edx!=0), so the inside set is
        // a half-line.  inside(x) is a robust predicate; we locate the boundary
        // near the real crossing fl=floor(-e_at/edx) and snap with a tiny
        // bounded search (<=2 steps) that can't run away.
        int64_t num = -e_at;
        int64_t fl = num / edx;
        if ((num % edx) != 0 && ((num < 0) != (edx < 0))) fl -= 1;  // floor div
        auto inside = [&](int64_t x) {
            int64_t ev = e_at + edx*x;
            return ccw ? (ev > 0 || (ev == 0 && tl))
                       : (ev < 0 || (ev == 0 && tl));
        };
        // Inside is the upper half (large x) iff (edx>0)==ccw.
        bool upper = ((edx > 0) == ccw);
        if (upper) {
            // first included column = smallest x with inside(x).  Candidates
            // around fl: fl-1, fl, fl+1.  Clamp search to [-2,+2] of fl.
            int64_t b = fl - 2;
            while (b <= fl + 2 && !inside(b)) ++b;
            if (b > lo) lo = (int)b;
        } else {
            // last included column = largest x with inside(x).
            int64_t b = fl + 2;
            while (b >= fl - 2 && !inside(b)) --b;
            if (b + 1 < hi) hi = (int)(b + 1);
        }
        if (lo < 0) lo = 0;
        if (hi < lo) hi = lo;
    };

    for (int y = bandLo; y < bandHi; ++y) {
        int dy = y - miny;   // anchored at bbox miny (band-independent setup)
        int64_t e0r = e0o + e0dy*dy, e1r = e1o + e1dy*dy, e2r = e2o + e2dy*dy;
        uint8_t *row = dst->rgba + (size_t)y * (size_t)dst->w * 4;

        // Compute covered column interval [xlo,xhi) relative to minx.
        int lo = 0, hi = maxx - minx;
        narrow(e0r, e0dx, tl0, lo, hi);
        if (lo < hi) narrow(e1r, e1dx, tl1, lo, hi);
        if (lo < hi) narrow(e2r, e2dx, tl2, lo, hi);
        if (lo >= hi) continue;

        // Advance attributes from minx to (minx+lo).
        int32_t u  = (int32_t)lrintf((uOf + uYf*dy)*F) + uX*lo;
        int32_t vv = (int32_t)lrintf((vOf + vYf*dy)*F) + vX*lo;
        int32_t cr = (int32_t)lrintf((rOf + rYf*dy)*F) + rX*lo;
        int32_t cg = (int32_t)lrintf((gOf + gYf*dy)*F) + gX*lo;
        int32_t cb = (int32_t)lrintf((bOf + bYf*dy)*F) + bX*lo;
        int32_t ca = (int32_t)lrintf((aOf + aYf*dy)*F) + aX*lo;

        blit_span(q, row, minx+lo, minx+hi, blend, aref,
                  u, vv, cr, cg, cb, ca, uX, vX, rX, gX, bX, aX);
    }
}

// ---- public: rasterize one triangle (single-threaded, whole surface) ---------
void Blitter_RasterTri(RSurface *dst, const BVtx v[3], const RTexture *tex,
                       RBlend blend, float alphaRef) {
    if (!dst) return;
    raster_tri_rows(dst, v, tex, blend, alphaRef, 0, dst->h);
}

// ---- persistent worker thread pool ------------------------------------------
// Created lazily on the first multi-threaded draw and reused for every draw and
// frame thereafter (never spawned per-draw).  N workers are joined to the
// calling thread for one band each: the caller rasterizes band 0 itself and the
// pool covers bands 1..N-1, so a 2-thread draw uses 1 worker + the caller.
namespace {

struct DrawJob {
    RSurface       *dst;
    const BVtx     *verts;
    int             triCount;
    const RTexture *tex;
    RBlend          blend;
    float           alphaRef;
    int             yLo, yHi;   // this worker's band
};

struct RasterPool {
    static const int kMaxWorkers = 8;      // workers excluding the caller thread

    pthread_mutex_t mtx   = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  wake  = PTHREAD_COND_INITIALIZER;   // workers wait for work
    pthread_cond_t  done  = PTHREAD_COND_INITIALIZER;   // caller waits for finish

    pthread_t       tid[kMaxWorkers];
    DrawJob         job[kMaxWorkers];
    int             nWorkers   = 0;   // spawned worker threads
    uint64_t        generation = 0;   // bumped each dispatch; workers compare against seen
    uint64_t        seen[kMaxWorkers];
    int             pending    = 0;   // workers still processing the current generation
    bool            shutdown   = false;

    void rasterizeBand(const DrawJob &j) {
        for (int t = 0; t < j.triCount; ++t)
            raster_tri_rows(j.dst, j.verts + (size_t)t * 3, j.tex, j.blend,
                            j.alphaRef, j.yLo, j.yHi);
    }
};

static RasterPool g_pool;

struct WorkerArg { RasterPool *pool; int idx; };
static WorkerArg g_workerArgs[RasterPool::kMaxWorkers];

static void *worker_main(void *vp) {
    WorkerArg *wa = (WorkerArg *)vp;
    RasterPool *p = wa->pool;
    int idx = wa->idx;
    for (;;) {
        pthread_mutex_lock(&p->mtx);
        while (!p->shutdown && p->seen[idx] == p->generation)
            pthread_cond_wait(&p->wake, &p->mtx);
        if (p->shutdown) { pthread_mutex_unlock(&p->mtx); break; }
        DrawJob j = p->job[idx];          // copy the job out under the lock
        p->seen[idx] = p->generation;
        pthread_mutex_unlock(&p->mtx);

        p->rasterizeBand(j);              // rasterize OUTSIDE the lock (parallel)

        pthread_mutex_lock(&p->mtx);
        if (--p->pending == 0) pthread_cond_signal(&p->done);
        pthread_mutex_unlock(&p->mtx);
    }
    return nullptr;
}

// Ensure at least `want` worker threads exist (caller holds no lock).
static void pool_ensure_workers(int want) {
    if (want > RasterPool::kMaxWorkers) want = RasterPool::kMaxWorkers;
    pthread_mutex_lock(&g_pool.mtx);
    while (g_pool.nWorkers < want) {
        int i = g_pool.nWorkers;
        g_pool.seen[i] = g_pool.generation;   // start in sync (no pending work)
        g_workerArgs[i].pool = &g_pool;
        g_workerArgs[i].idx  = i;
        if (pthread_create(&g_pool.tid[i], nullptr, worker_main, &g_workerArgs[i]) != 0)
            break;                            // out of threads: run with fewer
        ++g_pool.nWorkers;
    }
    pthread_mutex_unlock(&g_pool.mtx);
}

} // namespace

// ---- public: rasterize a whole draw, optionally across cores -----------------
void Blitter_RasterDraw(RSurface *dst, const BVtx *verts, int triCount,
                        const RTexture *tex, RBlend blend, float alphaRef,
                        int threads) {
    if (!dst || !dst->rgba || dst->w <= 0 || dst->h <= 0) return;
    if (!verts || triCount <= 0) return;

    // Decide the parallel width.  Cap by surface height (need >=1 row per band)
    // and by a small-work threshold where pool overhead would dominate.
    int H = dst->h;
    int maxBands = RasterPool::kMaxWorkers + 1;   // caller + workers
    if (threads > maxBands) threads = maxBands;
    if (threads > H)        threads = H;
    bool tiny = (triCount < 2) || ((int64_t)dst->w * H < 64 * 64);
    if (threads <= 1 || tiny) {
        for (int t = 0; t < triCount; ++t)
            raster_tri_rows(dst, verts + (size_t)t * 3, tex, blend, alphaRef, 0, H);
        return;
    }

    int nBands = threads;
    int nWork  = nBands - 1;               // worker threads needed (caller does band 0)
    pool_ensure_workers(nWork);

    // If we couldn't get all workers, fall back to however many we actually have.
    if (g_pool.nWorkers < nWork) nWork = g_pool.nWorkers;
    nBands = nWork + 1;
    if (nBands <= 1) {                      // no workers available at all
        for (int t = 0; t < triCount; ++t)
            raster_tri_rows(dst, verts + (size_t)t * 3, tex, blend, alphaRef, 0, H);
        return;
    }

    // Contiguous, disjoint, gap-free row bands tiling [0,H).
    auto bandLo = [&](int b) { return (int)((int64_t)H * b / nBands); };

    pthread_mutex_lock(&g_pool.mtx);
    ++g_pool.generation;
    g_pool.pending = nWork;
    // Workers NOT dispatched this generation must stay parked: pre-mark their
    // `seen` to the new generation so the broadcast doesn't make them grab a
    // stale job or touch `pending`.
    for (int w = nWork; w < g_pool.nWorkers; ++w)
        g_pool.seen[w] = g_pool.generation;
    for (int b = 1; b < nBands; ++b) {     // bands 1..nBands-1 -> workers
        int w = b - 1;
        DrawJob &j = g_pool.job[w];
        j.dst = dst; j.verts = verts; j.triCount = triCount;
        j.tex = tex; j.blend = blend; j.alphaRef = alphaRef;
        j.yLo = bandLo(b); j.yHi = bandLo(b + 1);
        // Leave job[w].seen behind the new generation so worker w wakes for it.
        g_pool.seen[w] = g_pool.generation - 1;
    }
    pthread_cond_broadcast(&g_pool.wake);
    pthread_mutex_unlock(&g_pool.mtx);

    // The caller thread rasterizes band 0 concurrently with the workers.
    for (int t = 0; t < triCount; ++t)
        raster_tri_rows(dst, verts + (size_t)t * 3, tex, blend, alphaRef,
                        bandLo(0), bandLo(1));

    // Barrier: wait for all worker bands of this generation to finish.
    pthread_mutex_lock(&g_pool.mtx);
    while (g_pool.pending != 0)
        pthread_cond_wait(&g_pool.done, &g_pool.mtx);
    pthread_mutex_unlock(&g_pool.mtx);
}
