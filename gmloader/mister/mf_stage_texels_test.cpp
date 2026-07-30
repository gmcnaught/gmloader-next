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

int main(void) {
    test_alpha_threshold();
    test_colorkey_collision();
    test_dark_luma_boundary();
    test_mask_only_is_all_or_nothing();
    test_subrect_addressing();
    test_rgba4444_format();
    test_dark_classification_exhaustive();
    test_differential_sweep();

    if (g_fail) {
        fprintf(stderr, "\n%d FAILURE(S)\n", g_fail);
        return 1;
    }
    fprintf(stderr, "\nall mf_stage_texels tests passed\n");
    return 0;
}
