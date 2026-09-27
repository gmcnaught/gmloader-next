// Host unit test (TDD) for the fabric texture-staging texel loop
// (mf_stage_texels in raster_backend_mfgpu.cpp), the per-texel conversion that
// stage_texture / stage_texture_region run on every texture-cache miss:
//
//   RGBA8888 or RGBA4444 source texel
//     -> alpha < 128            : emit MF_COLORKEY (0xF81F) and raise has_key
//     -> alpha >= 128           : pack RGB565; if that lands exactly on the key,
//                                 nudge it by one green LSB so an opaque texel
//                                 can never be mistaken for a hole
//     -> mask_only              : stays set only while EVERY staged texel is
//                                 "dark" (the key itself, or Rec.601 luma <= 64)
//
// This exists so that loop can be vectorized without trusting inspection. The
// oracle below is written from the contract above rather than lifted from the
// production body, and the boundary constants (alpha 127/128, the 0xF81F
// collision, luma 64/65) are ALSO pinned by hand-computed goldens, so a wrong
// oracle cannot quietly bless a wrong implementation.
//
// Coverage that matters for a SIMD rewrite: rect widths that are not multiples
// of the vector width (the tail), sub-rect origins (the source is strided by
// t->w while the destination is tightly packed by rw), and both texel formats.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <vector>

#include "blitter_raster.h"   // RTexture, RTEX_*

extern "C" void RasterBackend_MFGPU_TestStageTexels(const RTexture *t, int rx, int ry,
                                                    int rw, int rh, uint16_t *out,
                                                    int *out_has_key, int *out_mask_only);
// [TRILIST PALPHA] ARGB4444 staging + region classification.
extern "C" void RasterBackend_MFGPU_TestStageTexels4444(const RTexture *t, int rx, int ry,
                                                        int rw, int rh, uint16_t *out,
                                                        int *out_mask_only);
extern "C" int RasterBackend_MFGPU_TestPaClassify(const RTexture *t, int rx, int ry, int rw, int rh);
extern "C" int RasterBackend_MFGPU_TestPaPick(int cls, int faded);
enum { PA_KNOWN = 1, PA_PARTIAL = 2, PA_HOLE = 4, PA_LOSSLESS = 8 };

static int g_fail = 0;

static void report(const char *name, bool ok) {
    if (ok) {
        fprintf(stderr, "ok   %s\n", name);
    } else {
        fprintf(stderr, "FAIL %s\n", name);
        g_fail++;
    }
}

static void check_u16(const char *name, uint16_t got, uint16_t expect) {
    if (got != expect) {
        fprintf(stderr, "FAIL %-34s got=0x%04X expect=0x%04X\n", name, got, expect);
        g_fail++;
    } else {
        fprintf(stderr, "ok   %-34s 0x%04X\n", name, got);
    }
}

static void check_int(const char *name, int got, int expect) {
    if (got != expect) {
        fprintf(stderr, "FAIL %-34s got=%d expect=%d\n", name, got, expect);
        g_fail++;
    } else {
        fprintf(stderr, "ok   %-34s %d\n", name, got);
    }
}

// ---- independent oracle, from the contract in this file's header ------------

static const uint16_t KEY = 0xF81F;

static uint16_t oracle_texel(const RTexture *t, int x, int y, int *has_key) {
    int r, g, b, a;
    if (t->format == RTEX_RGBA4444) {
        uint16_t p = ((const uint16_t *)t->rgba)[(size_t)y * t->w + x];
        int r4 = (p >> 12) & 0xF, g4 = (p >> 8) & 0xF, b4 = (p >> 4) & 0xF, a4 = p & 0xF;
        r = (r4 << 4) | r4; g = (g4 << 4) | g4; b = (b4 << 4) | b4; a = (a4 << 4) | a4;
    } else {
        const uint8_t *p = t->rgba + ((size_t)y * t->w + x) * 4;
        r = p[0]; g = p[1]; b = p[2]; a = p[3];
    }
    if (a < 128) { *has_key = 1; return KEY; }
    uint16_t px = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    if (px == KEY) px ^= 0x0020;
    return px;
}

static bool oracle_dark(uint16_t px) {
    if (px == KEY) return true;
    int r = ((px >> 11) & 0x1F) << 3, g = ((px >> 5) & 0x3F) << 2, b = (px & 0x1F) << 3;
    return ((r * 77 + g * 151 + b * 28) >> 8) <= 64;
}

static void oracle_stage(const RTexture *t, int rx, int ry, int rw, int rh,
                         std::vector<uint16_t> &out, int *has_key, int *mask_only) {
    out.assign((size_t)rw * rh, 0);
    *has_key = 0;
    *mask_only = 1;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            uint16_t px = oracle_texel(t, rx + x, ry + y, has_key);
            out[(size_t)y * rw + x] = px;
            if (!oracle_dark(px)) *mask_only = 0;
        }
}

// ---- helpers ----------------------------------------------------------------

// A 1x1 RGBA8888 page: the cheapest way to pin one texel's conversion exactly.
static void stage_one_8888(int r, int g, int b, int a,
                           uint16_t *px, int *has_key, int *mask_only) {
    uint8_t pix[4] = { (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
    RTexture t{}; t.rgba = pix; t.w = 1; t.h = 1; t.valid = 1; t.format = RTEX_RGBA8888;
    RasterBackend_MFGPU_TestStageTexels(&t, 0, 0, 1, 1, px, has_key, mask_only);
}

static uint32_t g_rng = 0x13572468u;
static uint32_t rnd(void) {                     // xorshift32: deterministic corpus
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

// ---- tests ------------------------------------------------------------------

// The alpha threshold is a hard cut at 128, not a rounding of "mostly opaque".
static void test_alpha_threshold() {
    uint16_t px; int hk, mo;

    stage_one_8888(0xFF, 0xFF, 0xFF, 127, &px, &hk, &mo);
    check_u16("alpha 127 -> colorkey", px, KEY);
    check_int("alpha 127 -> has_key", hk, 1);
    check_int("alpha 127 -> mask_only", mo, 1);      // the key is "dark" by definition

    stage_one_8888(0xFF, 0xFF, 0xFF, 128, &px, &hk, &mo);
    check_u16("alpha 128 -> opaque white", px, 0xFFFF);
    check_int("alpha 128 -> no has_key", hk, 0);
    check_int("alpha 128 -> not mask_only", mo, 0);
}

// An opaque texel whose RGB565 lands exactly on the sentinel must be moved off
// it, or a colorkey draw would punch a hole through solid art.
static void test_colorkey_collision() {
    uint16_t px; int hk, mo;
    // r=0xFF -> 0x1F, g=0x00 -> 0x00, b=0xFF -> 0x1F  ==  0xF81F  ==  the key.
    stage_one_8888(0xFF, 0x00, 0xFF, 0xFF, &px, &hk, &mo);
    check_u16("opaque magenta nudged off key", px, (uint16_t)(KEY ^ 0x0020));
    check_int("opaque magenta not has_key", hk, 0);

    // ...and the nudged value must not read back as a hole.
    check_int("nudged value != key", px == KEY ? 1 : 0, 0);
}

// mask_only is what lets the CRT-overlay strip fire; its luma cut is <= 64.
static void test_dark_luma_boundary() {
    uint16_t px; int hk, mo;
    // 565 quantizes 64 -> r5=8,g6=16,b5=8 -> 64,64,64; luma = 64*(77+151+28)>>8 = 64.
    stage_one_8888(64, 64, 64, 255, &px, &hk, &mo);
    check_int("luma 64 is dark", mo, 1);
    // 72 -> r5=9,g6=18,b5=9 -> 72,72,72; luma = 72. Just over the cut.
    stage_one_8888(72, 72, 72, 255, &px, &hk, &mo);
    check_int("luma 72 is not dark", mo, 0);
}

// One bright texel anywhere in the page clears mask_only for the whole page.
static void test_mask_only_is_all_or_nothing() {
    const int W = 19, H = 3;                        // 19: not a multiple of 8 or 16
    std::vector<uint8_t> pix((size_t)W * H * 4, 0);
    for (size_t i = 0; i < (size_t)W * H; i++) pix[i * 4 + 3] = 255;   // opaque black
    RTexture t{}; t.rgba = pix.data(); t.w = W; t.h = H; t.valid = 1;
    t.format = RTEX_RGBA8888;

    std::vector<uint16_t> out((size_t)W * H);
    int hk, mo;
    RasterBackend_MFGPU_TestStageTexels(&t, 0, 0, W, H, out.data(), &hk, &mo);
    check_int("all-black page is mask_only", mo, 1);

    // Brighten the LAST texel of the LAST row — the one a vector body would
    // reach only through its scalar tail.
    pix[((size_t)W * H - 1) * 4 + 1] = 255;         // green
    RasterBackend_MFGPU_TestStageTexels(&t, 0, 0, W, H, out.data(), &hk, &mo);
    check_int("one bright tail texel clears it", mo, 0);
}

// The source is strided by t->w; the destination is packed by rw. Getting that
// wrong is the classic sub-rect bug and it survives every uniform-colour test.
static void test_subrect_addressing() {
    const int W = 12, H = 6;
    std::vector<uint8_t> pix((size_t)W * H * 4, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = &pix[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)(x * 8); p[1] = (uint8_t)(y * 8); p[2] = 0; p[3] = 255;
        }
    RTexture t{}; t.rgba = pix.data(); t.w = W; t.h = H; t.valid = 1;
    t.format = RTEX_RGBA8888;

    const int RX = 3, RY = 2, RW = 5, RH = 3;
    std::vector<uint16_t> got((size_t)RW * RH);
    int hk, mo;
    RasterBackend_MFGPU_TestStageTexels(&t, RX, RY, RW, RH, got.data(), &hk, &mo);

    std::vector<uint16_t> want;
    int whk, wmo;
    oracle_stage(&t, RX, RY, RW, RH, want, &whk, &wmo);
    report("sub-rect packs tightly", got == want);
    check_int("sub-rect has_key", hk, whk);
    check_int("sub-rect mask_only", mo, wmo);

    // Spot-check one interior texel against the source by hand, so a matching
    // oracle bug cannot hide a matching implementation bug.
    const uint8_t *src = &pix[((size_t)(RY + 1) * W + (RX + 2)) * 4];
    uint16_t expect = (uint16_t)(((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[2] >> 3));
    check_u16("sub-rect texel (2,1) by hand", got[(size_t)1 * RW + 2], expect);
}

// RGBA4444 nibble-replicate expansion, the g_tex16 memory-saving path.
static void test_rgba4444_format() {
    const int W = 9, H = 2;
    std::vector<uint16_t> pix((size_t)W * H);
    for (size_t i = 0; i < pix.size(); i++) pix[i] = (uint16_t)(rnd() & 0xFFFF);
    RTexture t{}; t.rgba = (const uint8_t *)pix.data(); t.w = W; t.h = H; t.valid = 1;
    t.format = RTEX_RGBA4444;

    std::vector<uint16_t> got((size_t)W * H);
    int hk, mo;
    RasterBackend_MFGPU_TestStageTexels(&t, 0, 0, W, H, got.data(), &hk, &mo);

    std::vector<uint16_t> want; int whk, wmo;
    oracle_stage(&t, 0, 0, W, H, want, &whk, &wmo);
    report("rgba4444 page matches oracle", got == want);
    check_int("rgba4444 has_key", hk, whk);
    check_int("rgba4444 mask_only", mo, wmo);

    // A4 = 7 expands to 0x77 = 119 < 128 -> hole; A4 = 8 expands to 0x88 = 136 -> kept.
    uint16_t two[2] = { (uint16_t)0xFFF7, (uint16_t)0xFFF8 };
    RTexture t2{}; t2.rgba = (const uint8_t *)two; t2.w = 2; t2.h = 1; t2.valid = 1;
    t2.format = RTEX_RGBA4444;
    uint16_t o2[2]; int hk2, mo2;
    RasterBackend_MFGPU_TestStageTexels(&t2, 0, 0, 2, 1, o2, &hk2, &mo2);
    check_u16("4444 a4=7 -> colorkey", o2[0], KEY);
    check_u16("4444 a4=8 -> opaque white", o2[1], 0xFFFF);
}

// Exhaustive dark-classification sweep over the whole RGB565 space.
//
// Added after mutation testing: perturbing the green luma weight (151 -> 150) in
// the vector body left every other test in this file green, because the Rec.601
// weights only change a verdict for colours sitting exactly on the luma == 64
// cut. Random corpora almost never land there. This visits all 65,536 packed
// values, so any weight, shift or comparison drift is caught.
//
// Each value is staged as a width-8 page of identical texels: 8 lanes is wide
// enough to take the vector body (a 1x1 page would only ever exercise the scalar
// tail), and identical lanes make the page's mask_only equal to that one
// colour's verdict. Source bytes are the exact 565->888 expansion, so the value
// round-trips to itself.
static void test_dark_classification_exhaustive() {
    bool all_ok = true;
    uint16_t first_bad = 0;
    for (uint32_t v = 0; v < 65536u; v++) {
        const uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
        const uint8_t g = (uint8_t)(((v >> 5) & 0x3F) << 2);
        const uint8_t b = (uint8_t)((v & 0x1F) << 3);

        uint8_t pix[8 * 4];
        for (int i = 0; i < 8; i++) {
            pix[i * 4 + 0] = r; pix[i * 4 + 1] = g;
            pix[i * 4 + 2] = b; pix[i * 4 + 3] = 255;
        }
        RTexture t{}; t.rgba = pix; t.w = 8; t.h = 1; t.valid = 1;
        t.format = RTEX_RGBA8888;

        uint16_t got[8]; int hk, mo;
        RasterBackend_MFGPU_TestStageTexels(&t, 0, 0, 8, 1, got, &hk, &mo);

        std::vector<uint16_t> want; int whk, wmo;
        oracle_stage(&t, 0, 0, 8, 1, want, &whk, &wmo);

        if (memcmp(got, want.data(), sizeof got) != 0 || hk != whk || mo != wmo) {
            if (all_ok) { first_bad = (uint16_t)v; }
            all_ok = false;
        }
    }
    if (!all_ok)
        fprintf(stderr, "  first bad 565 value: 0x%04X\n", first_bad);
    report("all 65536 RGB565 values classify correctly", all_ok);
}

// Differential sweep: every width from 1..40 crosses whatever vector width the
// implementation picks, plus its tail, in both formats and at offset origins.
static void test_differential_sweep() {
    bool all_ok = true;
    int cases = 0;
    for (int fmt = 0; fmt < 2; fmt++) {
        for (int rw = 1; rw <= 40; rw++) {
            const int rh = 1 + (rw % 4);
            const int rx = rw % 3, ry = rw % 2;
            const int W = rx + rw + 2, H = ry + rh + 1;

            std::vector<uint8_t> pix8;
            std::vector<uint16_t> pix16;
            RTexture t{};
            t.w = W; t.h = H; t.valid = 1;
            if (fmt == RTEX_RGBA4444) {
                pix16.resize((size_t)W * H);
                for (auto &p : pix16) p = (uint16_t)(rnd() & 0xFFFF);
                t.rgba = (const uint8_t *)pix16.data();
                t.format = RTEX_RGBA4444;
            } else {
                pix8.resize((size_t)W * H * 4);
                for (auto &p : pix8) p = (uint8_t)(rnd() & 0xFF);
                // Force alpha onto both sides of the 128 cut, and plant the
                // exact key colour, so every branch is hit somewhere.
                for (size_t i = 0; i < (size_t)W * H; i++) {
                    pix8[i * 4 + 3] = (uint8_t)((rnd() & 1) ? 255 : 0);
                    if ((rnd() & 7) == 0) {
                        pix8[i * 4 + 0] = 0xFF; pix8[i * 4 + 1] = 0x00;
                        pix8[i * 4 + 2] = 0xFF; pix8[i * 4 + 3] = 0xFF;
                    }
                }
                t.rgba = pix8.data();
                t.format = RTEX_RGBA8888;
            }

            std::vector<uint16_t> got((size_t)rw * rh, 0xDEAD);
            int hk = -1, mo = -1;
            RasterBackend_MFGPU_TestStageTexels(&t, rx, ry, rw, rh, got.data(), &hk, &mo);

            std::vector<uint16_t> want; int whk, wmo;
            oracle_stage(&t, rx, ry, rw, rh, want, &whk, &wmo);

            cases++;
            if (got != want || hk != whk || mo != wmo) {
                if (all_ok)
                    fprintf(stderr, "  first mismatch: fmt=%d rect=%d,%d %dx%d "
                            "has_key %d/%d mask_only %d/%d\n",
                            fmt, rx, ry, rw, rh, hk, whk, mo, wmo);
                all_ok = false;
            }
        }
    }
    fprintf(stderr, "     (%d rect cases)\n", cases);
    report("differential sweep matches oracle", all_ok);
}

// ---- [TRILIST PALPHA] ARGB4444 staging ---------------------------------------
// Oracle, from the contract (raster_backend_mfgpu.cpp "ARGB4444 staging"):
//   A4 = round(a*15/255) (no ties exist for integer a); A4 == 0 -> 0x0000.
//   R4/G4/B4 = the RGB565 channel truncated to 4 bits, which is the top nibble of
//   the 8-bit channel: (r>>3)>>1 == r>>4, (g>>2)>>2 == g>>4.
//   Decode (fabric / refmodel): R5 = {r4, r4[3]}, G6 = {g4, g4[3:2]}, B5 = {b4, b4[3]}.
//   Lossless iff the decode equals (r>>3, g>>2, b>>3) for every texel with A4 > 0.
static void src_rgba(const RTexture *t, int x, int y, int *r, int *g, int *b, int *a) {
    if (t->format == RTEX_RGBA4444) {
        uint16_t p = ((const uint16_t *)t->rgba)[(size_t)y * t->w + x];
        int r4 = (p >> 12) & 0xF, g4 = (p >> 8) & 0xF, b4 = (p >> 4) & 0xF, a4 = p & 0xF;
        *r = (r4 << 4) | r4; *g = (g4 << 4) | g4; *b = (b4 << 4) | b4; *a = (a4 << 4) | a4;
    } else {
        const uint8_t *p = t->rgba + ((size_t)y * t->w + x) * 4;
        *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
    }
}
static int oracle_a4(int a) { return (int)(a * 15.0 / 255.0 + 0.5); }
static uint16_t oracle_4444(int r, int g, int b, int a) {
    int a4 = oracle_a4(a);
    if (a4 == 0) return 0;
    return (uint16_t)((a4 << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
}
static bool oracle_exact(int r, int g, int b) {
    int r4 = r >> 4, g4 = g >> 4, b4 = b >> 4;
    return (((r4 << 1) | (r4 >> 3)) == (r >> 3)) && (((g4 << 2) | (g4 >> 2)) == (g >> 2)) &&
           (((b4 << 1) | (b4 >> 3)) == (b >> 3));
}
static int oracle_classify(const RTexture *t, int rx, int ry, int rw, int rh) {
    bool partial = false, hole = false, lossless = true;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            int r, g, b, a; src_rgba(t, rx + x, ry + y, &r, &g, &b, &a);
            int a4 = oracle_a4(a);
            if (a4 == 0) { hole = true; continue; }
            if (a4 < 15) partial = true;
            if (!oracle_exact(r, g, b)) lossless = false;
        }
    // A lossy rect need not report PARTIAL/HOLE (the scan may stop early): mask them.
    int c = PA_KNOWN | (lossless ? PA_LOSSLESS : 0);
    if (lossless) c |= (partial ? PA_PARTIAL : 0) | (hole ? PA_HOLE : 0);
    return c;
}
static int classify_masked(const RTexture *t, int rx, int ry, int rw, int rh) {
    int c = RasterBackend_MFGPU_TestPaClassify(t, rx, ry, rw, rh);
    return (c & PA_LOSSLESS) ? c : (c & (PA_KNOWN | PA_LOSSLESS));
}
static uint16_t stage_one_4444(int r, int g, int b, int a) {
    uint8_t pix[4] = { (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
    RTexture t{}; t.rgba = pix; t.w = 1; t.h = 1; t.valid = 1; t.format = RTEX_RGBA8888;
    uint16_t px = 0xDEAD; int mo = -1;
    RasterBackend_MFGPU_TestStageTexels4444(&t, 0, 0, 1, 1, &px, &mo);
    return px;
}

// Hand-computed goldens, independent of both oracle and implementation.
static void test_4444_goldens() {
    check_u16("4444 white a=128 -> A4 8",   stage_one_4444(255, 255, 255, 128), 0x8FFF);
    check_u16("4444 black opaque",           stage_one_4444(0, 0, 0, 255),       0xF000);
    check_u16("4444 red a=200 -> A4 12",     stage_one_4444(255, 0, 0, 200),     0xCF00);
    check_u16("4444 a=8 -> transparent 0",   stage_one_4444(255, 255, 255, 8),   0x0000);
    check_u16("4444 a=9 -> A4 1",            stage_one_4444(255, 255, 255, 9),   0x1FFF);
    check_u16("4444 a=246 -> A4 14",         stage_one_4444(255, 255, 255, 246), 0xEFFF);
    check_u16("4444 a=247 -> A4 15",         stage_one_4444(255, 255, 255, 247), 0xFFFF);
    check_u16("4444 0x12,0x34,0x56 a=255",   stage_one_4444(0x12, 0x34, 0x56, 255), 0xF135);
}

// Every alpha value, and every value of each colour channel, against the oracle.
static void test_4444_exhaustive_channels() {
    bool ok = true;
    for (int a = 0; a < 256 && ok; a++)
        if (stage_one_4444(255, 255, 255, a) != oracle_4444(255, 255, 255, a)) {
            fprintf(stderr, "  alpha %d: got 0x%04X want 0x%04X\n", a,
                    stage_one_4444(255, 255, 255, a), oracle_4444(255, 255, 255, a));
            ok = false;
        }
    for (int c = 0; c < 256 && ok; c++) {
        if (stage_one_4444(c, 0, 0, 255) != oracle_4444(c, 0, 0, 255) ||
            stage_one_4444(0, c, 0, 255) != oracle_4444(0, c, 0, 255) ||
            stage_one_4444(0, 0, c, 255) != oracle_4444(0, 0, c, 255)) {
            fprintf(stderr, "  channel value %d mismatches\n", c); ok = false;
        }
    }
    report("4444 encode: every alpha and channel value matches oracle", ok);
}

// The classification the staging policy turns on.
static void test_pa_classify_cases() {
    enum { W = 8, H = 4 };
    uint8_t px[W * H * 4];
    RTexture t{}; t.rgba = px; t.w = W; t.h = H; t.valid = 1; t.format = RTEX_RGBA8888;
    auto fill = [&](int r, int g, int b) {
        for (int i = 0; i < W * H; i++) { px[i*4] = (uint8_t)r; px[i*4+1] = (uint8_t)g; px[i*4+2] = (uint8_t)b;
                                          px[i*4+3] = (uint8_t)(i * 255 / (W * H - 1)); }   // 0..255 ramp
    };
    // Single-colour alpha masks (system_font, spr_torch_darkness): lossless.
    fill(255, 255, 255);
    check_int("classify white ramp mask", RasterBackend_MFGPU_TestPaClassify(&t, 0, 0, W, H),
              PA_KNOWN | PA_PARTIAL | PA_HOLE | PA_LOSSLESS);
    fill(0, 0, 0);
    check_int("classify black ramp mask", RasterBackend_MFGPU_TestPaClassify(&t, 0, 0, W, H),
              PA_KNOWN | PA_PARTIAL | PA_HOLE | PA_LOSSLESS);
    // A colour 4444 cannot hold (r = 0x08: R5 = 1 -> r4 = 0 -> decodes to 0): lossy.
    fill(0x08, 0x20, 0x30);
    check_int("classify lossy colour", classify_masked(&t, 0, 0, W, H), PA_KNOWN);
    // Hard-edged cutout: holes and opaque texels only, no partial.
    for (int i = 0; i < W * H; i++) { px[i*4] = px[i*4+1] = px[i*4+2] = 255; px[i*4+3] = (i & 1) ? 255 : 0; }
    check_int("classify hard-edged white", RasterBackend_MFGPU_TestPaClassify(&t, 0, 0, W, H),
              PA_KNOWN | PA_HOLE | PA_LOSSLESS);
    // A lossy colour under A4 == 0 is never drawn, so it does not make the rect lossy.
    px[0] = 0x08; px[1] = 0x20; px[2] = 0x30; px[3] = 5;
    check_int("classify lossy colour under hole ignored",
              RasterBackend_MFGPU_TestPaClassify(&t, 0, 0, W, H), PA_KNOWN | PA_HOLE | PA_LOSSLESS);
    // ...but under A4 = 1 it is drawn, and does.
    px[3] = 9;
    check_int("classify lossy colour at A4=1", classify_masked(&t, 0, 0, W, H), PA_KNOWN);
    // Sub-rect: the lossy texel at (0,0) is outside rect (1,0)..
    check_int("classify sub-rect excludes lossy texel",
              RasterBackend_MFGPU_TestPaClassify(&t, 1, 0, W - 1, H), PA_KNOWN | PA_HOLE | PA_LOSSLESS);
    // Fully opaque, lossless: nothing to do.
    for (int i = 0; i < W * H; i++) { px[i*4] = px[i*4+1] = px[i*4+2] = 255; px[i*4+3] = 255; }
    check_int("classify opaque white", RasterBackend_MFGPU_TestPaClassify(&t, 0, 0, W, H),
              PA_KNOWN | PA_LOSSLESS);
    // Policy: unfaded -> soft edges only, and only when lossless; faded -> any
    // transparency, lossy or not (the alternative paints the colorkey sentinel).
    const int L = PA_KNOWN | PA_LOSSLESS;
    check_int("pick partial",            RasterBackend_MFGPU_TestPaPick(L | PA_PARTIAL, 0), 1);
    check_int("pick hole unfaded",       RasterBackend_MFGPU_TestPaPick(L | PA_HOLE, 0), 0);
    check_int("pick hole faded",         RasterBackend_MFGPU_TestPaPick(L | PA_HOLE, 1), 1);
    check_int("pick opaque faded",       RasterBackend_MFGPU_TestPaPick(L, 1), 0);
    check_int("pick lossy partial",      RasterBackend_MFGPU_TestPaPick(PA_KNOWN | PA_PARTIAL, 0), 0);
    check_int("pick lossy faded",        RasterBackend_MFGPU_TestPaPick(PA_KNOWN | PA_PARTIAL | PA_HOLE, 1), 1);
    check_int("pick lossy hole faded",   RasterBackend_MFGPU_TestPaPick(PA_KNOWN | PA_HOLE, 1), 1);
    check_int("pick lossy opaque faded", RasterBackend_MFGPU_TestPaPick(PA_KNOWN, 1), 0);
}

// Random rects over random textures, both source formats, against the oracle --
// the staged texels, mask_only and the classification.
static void test_4444_differential_sweep() {
    bool all_ok = true; int cases = 0;
    for (int fmt = 0; fmt < 2; fmt++) {
        for (int iter = 0; iter < 200; iter++) {
            const int tw = 1 + (int)(rnd() % 40), th = 1 + (int)(rnd() % 12);
            std::vector<uint8_t> buf((size_t)tw * th * 4);
            // Mix exact colours (pure nibble-replicated) with arbitrary ones so both
            // lossless and lossy rects occur.
            const bool exact = (rnd() & 1) != 0;
            const int base = (int)(rnd() % 16) * 17;
            for (int i = 0; i < tw * th; i++) {
                if (fmt == 0) {
                    for (int c = 0; c < 3; c++) buf[i*4+c] = exact ? (uint8_t)base : (uint8_t)rnd();
                    uint32_t k = rnd() % 4;
                    buf[i*4+3] = k == 0 ? 0 : k == 1 ? 255 : (uint8_t)rnd();
                } else {
                    uint16_t p = (uint16_t)rnd();
                    if (exact) p = (uint16_t)((p & 0x000F) | ((base >> 4) * 0x1110));
                    ((uint16_t *)buf.data())[i] = p;
                }
            }
            RTexture t{}; t.rgba = buf.data(); t.w = tw; t.h = th; t.valid = 1;
            t.format = fmt ? RTEX_RGBA4444 : RTEX_RGBA8888;
            const int rx = (int)(rnd() % tw), ry = (int)(rnd() % th);
            const int rw = 1 + (int)(rnd() % (tw - rx)), rh = 1 + (int)(rnd() % (th - ry));
            std::vector<uint16_t> got((size_t)rw * rh, 0xDEAD);
            int mo = -1;
            RasterBackend_MFGPU_TestStageTexels4444(&t, rx, ry, rw, rh, got.data(), &mo);
            int wmo = 1; bool texels_ok = true;
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    int r, g, b, a; src_rgba(&t, rx + x, ry + y, &r, &g, &b, &a);
                    uint16_t w = oracle_4444(r, g, b, a);
                    if (got[(size_t)y * rw + x] != w) texels_ok = false;
                    if (w >> 12) {
                        int r4 = (w >> 8) & 0xF, g4 = (w >> 4) & 0xF, b4 = w & 0xF;
                        uint16_t d = (uint16_t)((((r4 << 1) | (r4 >> 3)) << 11) |
                                                (((g4 << 2) | (g4 >> 2)) << 5) | ((b4 << 1) | (b4 >> 3)));
                        if (!oracle_dark(d)) wmo = 0;
                    }
                }
            const int cls = classify_masked(&t, rx, ry, rw, rh);
            const int wcls = oracle_classify(&t, rx, ry, rw, rh);
            cases++;
            if (!texels_ok || mo != wmo || cls != wcls) {
                if (all_ok)
                    fprintf(stderr, "  first mismatch: fmt=%d rect=%d,%d %dx%d texels %s mask %d/%d cls %d/%d\n",
                            fmt, rx, ry, rw, rh, texels_ok ? "ok" : "DIFF", mo, wmo, cls, wcls);
                all_ok = false;
            }
        }
    }
    fprintf(stderr, "     (%d rect cases)\n", cases);
    report("4444 differential sweep matches oracle", all_ok);
}

int main(void) {
    test_alpha_threshold();
    test_colorkey_collision();
    test_dark_luma_boundary();
    test_mask_only_is_all_or_nothing();
    test_subrect_addressing();
    test_rgba4444_format();
    test_dark_classification_exhaustive();
    test_differential_sweep();
    test_4444_goldens();
    test_4444_exhaustive_channels();
    test_pa_classify_cases();
    test_4444_differential_sweep();

    if (g_fail) {
        fprintf(stderr, "\n%d FAILURE(S)\n", g_fail);
        return 1;
    }
    fprintf(stderr, "\nall mf_stage_texels tests passed\n");
    return 0;
}
