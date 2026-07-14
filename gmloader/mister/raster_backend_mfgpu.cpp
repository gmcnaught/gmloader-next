// backend_mfgpu: FPGA-fabric back-end (Task 5). Owns the emitter + frame
// lifecycle over host-side ring/heap/vtx buffers (device wiring to the real DDR
// ring is Task 6) and is validated on the host by executing the emitted ring
// through the SAME blt_execute software model the mfgpu refmodel unit tests use
// (3rdparty/mfgpu/host/test_emitter.c).
//
//   clear   : emits a REAL fabric BLT_OP_FILL via blt_fill() (not a direct
//             pixel write) so the parity tests in raster_backend_test.cpp
//             exercise the same emit -> blt_execute path draw() uses.
//   draw    : converts the decoded screen-space triangle list into a
//             BLT_OP_TRILIST fabric command (blt_push_tris + blt_trilist) over
//             an RGB565 texture page. FBO / non-default targets and RB_PREMULT
//             fall back to backend_sw (see notes below).
//   present : executes the accumulated ring (blt_execute -> g_fb565) and closes
//             the frame; main.cpp then scans g_fb565 out via NativeVideoWriter (Task 7).
//
// The fabric framebuffer geometry is FIXED at BLT_FB_WIDTH x BLT_FB_HEIGHT
// (320x240, refmodel/blitter_ref.h) — that is the wire contract, not a
// per-draw parameter. clear() targets are clamped to that size; the engine's
// render surface is comfortably inside it.
//
// ── FRAME MODEL (Task 4 review fix) ──────────────────────────────────────────
// blt_emitter_init / blt_vtx_buf_init are ONE-TIME startup (they wire the ring,
// heap and vertex buffer). Only blt_begin_frame resets per-frame state (command
// list + vertex cursor). mf_init_once() does the one-time wiring; mf_frame_begin
// reclaims the previous frame's transient texture uploads (blt_heap_reset) then
// calls blt_begin_frame. Re-running blt_emitter_init every frame (the Task-4
// skeleton) would wipe any persistently-staged surfaces mid-run — wrong once the
// per-draw upload is replaced by cached/perm-staged texture pages (a later perf
// task). For bring-up every draw re-uploads its texture, so the heap is reset
// each frame.
//
// ── ONE SOURCE-DDR BUFFER ────────────────────────────────────────────────────
// blt_execute resolves BOTH the TRILIST vertices (heap->base + entry_off) and
// the texture page (heap->base + src_off) from a SINGLE heap base. So the vertex
// buffer and the texture-upload heap must live in one backing store, at disjoint
// offset ranges. g_srcdram is that store: [0, MF_VTX_REGION) is the per-frame
// vertex buffer (blt_vtx_buf_init base), [MF_VTX_REGION, cap) is the texture
// allocator (blt_alloc_init base). Both offset spaces are then valid relative to
// g_srcdram, exactly as test_emitter.c's single srcdram buffer.
//
// ── TEXTURE FORMAT (conforms to the golden blt_raster_tri) ───────────────────
// The TRILIST rasterizer (refmodel/blt_tri.c) samples the texture page as
// RGB565 (tex_nearest reads a raw 16-bit texel; the format byte is ignored) and
// modulates it by the interpolated per-vertex color (blt_tint565). There is NO
// per-texel alpha and NO BLT_BLEND_PALPHA case in a triangle list. So textures
// are staged as RGB565 (blt_upload, not blt_upload_argb4444), an untextured draw
// uses a 1x1 opaque-white page, and alpha compositing rides BLT_BLEND_CONST_ALPHA
// with header alpha=255 (effective alpha = interpolated vtx.a). See
// raster_backend_convert.h for the full rationale.
//
// ── COLORKEY STAGING FOR HARD-EDGED (1-BIT) ALPHA (Task 6) ────────────────────
// The one hole in "no per-texel alpha": blt_tri.c DOES have BLT_BLEND_COLORKEY
// (skip a texel that == the header colorkey). mf_texel565() folds any texel
// with alpha<128 into the sentinel MF_COLORKEY (0xF81F, magenta), nudging any
// opaque texel that happens to convert to that exact value off by one green
// LSB so it can never collide. mf_draw() then emits BLT_BLEND_COLORKEY instead
// of the Task 5 blend when the staged texture used the key (has_key) AND the
// draw's vertex alpha is fully opaque (min vtx.a*255 >= 254) — that is exactly
// the case (256-color palette sprites, e.g. Maldita Castilla) where the SW
// oracle's per-texel-alpha result and the fabric's colorkey result are
// pixel-identical (mod 565 rounding). A faded cutout (vtx.a < 254 on a keyed
// texture) can't combine colorkey + const-alpha in one TRILIST pass, so it
// falls back to CONST_ALPHA (no cutout, see the comment at the call site);
// real per-texel alpha is a future RTL item.
#include "raster_backend.h"
#include "raster_backend_convert.h"
extern "C" {
#include "blt_emitter.h"
#include "blt_wire.h"
#include "blitter_ref.h"
}
#include <string.h>
#include <stdio.h>

extern "C" const RasterBackend backend_sw;   /* FBO / RB_PREMULT fallback */

// One frame's worth of host-side emitter buffers. Static/file-scope is fine for
// this single-threaded backend; Task 6 points these at the real DDR ring/heap.
enum {
    MF_RING_CAP   = 64 * 1024,
    MF_SRCDRAM_CAP= 1 * 1024 * 1024,   // shared vertex + texture source DDR
    MF_VTX_REGION = 96 * 1024,         // low slice of g_srcdram: TRILIST vertices
    MF_MAX_CMDS   = MF_RING_CAP / BLT_CMD_BYTES,
    MF_MAX_VERTS  = 8192,              // per-draw vertex-conversion scratch
    MF_TEX_TEXELS = MF_SRCDRAM_CAP / 2 // per-draw RGB565 texture scratch (texels)
};
static uint8_t       g_ring[MF_RING_CAP];
static uint8_t       g_srcdram[MF_SRCDRAM_CAP];
static blt_emitter_t g_e;
static blt_cmd_t     g_cmds[MF_MAX_CMDS];
static blt_vtx_t     g_vtxscratch[MF_MAX_VERTS];
static uint16_t      g_texscratch[MF_TEX_TEXELS];
static bool          g_inited = false;

// Task 7: the production frame loop (main.cpp) calls clear()/draw()/present()
// but never frame_begin()/frame_end() (those are a host-test-only convenience —
// see mf_frame_begin's comment). g_frame_active makes the backend self-contained
// in that world: the first clear/draw of a frame implicitly opens it
// (mf_ensure_frame), and present() closes it (runs blt_execute into g_fb565 and
// resets state so the next clear/draw starts a fresh frame). Host tests that
// call frame_begin()/clear()/draw()/frame_end() directly are unaffected: those
// calls just re-set/consume the same flag along the way.
static bool          g_frame_active = false;

// Host execute target: the fixed-size fabric framebuffer blt_execute composites
// into. On real hardware this is scanned out directly; on the host it is the
// oracle the parity tests read back via RasterBackend_MFGPU_TestCopyFB565().
static uint16_t      g_fb565[BLT_FB_PIXELS];

// The default-framebuffer surface pixels (blitter.cpp's g_defSurf). A draw whose
// dst->rgba differs is an FBO / render-to-texture target the single-scanout
// fabric cannot represent, so it falls back to the SW rasterizer. NULL (unset,
// e.g. before Task 6 wires it) => treat every target as the default fb. Set on
// the host by RasterBackend_MFGPU_SetDefaultSurface().
static const uint8_t *g_defRGBA = nullptr;

static inline uint16_t mf_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Sentinel RGB565 value (magenta) used to mark "this texel is transparent" for
// BLT_BLEND_COLORKEY staging (Task 6). The golden rasterizer (blt_tri.c) skips
// any texel that compares == the header colorkey, so a hard-edged (alpha in
// {0,255}) 1-bit-alpha texture round-trips through colorkey exactly.
static const uint16_t MF_COLORKEY = 0xF81F;

// Convert one RTexture texel to RGB565, folding hard-edged alpha into the
// BLT_BLEND_COLORKEY sentinel (Task 6). Soft/intermediate alpha still has no
// fabric representation (no per-texel alpha in a triangle list) and is treated
// as opaque, same as Task 5.
//   alpha < 128            -> emit MF_COLORKEY (this texel is "hole" for a
//                              colorkey draw; harmless dead colour otherwise).
//   alpha >= 128            -> convert RGB->565; if that exact value collides
//                              with MF_COLORKEY, nudge it off by one green LSB
//                              so an opaque texel can never be mistaken for the
//                              key (out_has_key still only set by real holes).
static inline uint16_t mf_texel565(const RTexture *t, int x, int y, bool *out_has_key) {
    uint8_t r, g, b, a;
    if (t->format == RTEX_RGBA4444) {
        const uint16_t *p16 = (const uint16_t *)t->rgba;
        uint16_t p = p16[(size_t)y * t->w + x];        // packed (R4<<12|G4<<8|B4<<4|A4)
        unsigned r4 = (p >> 12) & 0xF, g4 = (p >> 8) & 0xF, b4 = (p >> 4) & 0xF, a4 = p & 0xF;
        r = (uint8_t)((r4 << 4) | r4);
        g = (uint8_t)((g4 << 4) | g4);
        b = (uint8_t)((b4 << 4) | b4);                  // nibble replicate -> 8-bit
        a = (uint8_t)((a4 << 4) | a4);
    } else {
        const uint8_t *p = t->rgba + ((size_t)y * t->w + x) * 4;  // RTEX_RGBA8888
        r = p[0]; g = p[1]; b = p[2]; a = p[3];
    }
    if (a < 128) { *out_has_key = true; return MF_COLORKEY; }
    uint16_t result = mf_rgb565(r, g, b);
    if (result == MF_COLORKEY) result ^= 0x0020;   // opaque texel must never == the key
    return result;
}

static void mf_init_once(void) {
    if (g_inited) return;
    blt_emitter_init(&g_e, g_ring, sizeof g_ring, g_srcdram, sizeof g_srcdram);
    // Reserve the low MF_VTX_REGION bytes for the per-frame vertex buffer; the
    // texture allocator owns the rest. Re-init the allocator with a non-zero base
    // so uploaded texture offsets never collide with vertex-entry offsets when
    // both are resolved from the single g_srcdram base by blt_execute.
    blt_alloc_init(&g_e.alloc, MF_VTX_REGION, sizeof g_srcdram - MF_VTX_REGION);
    blt_vtx_buf_init(&g_e, g_srcdram, MF_VTX_REGION);
    g_inited = true;
}

static void mf_frame_begin(void) {
    mf_init_once();
    // Reclaim the previous frame's transient per-draw texture uploads. (No
    // persistent staging yet; when texture pages are cached/perm-staged this
    // becomes a targeted free instead of a whole-heap reset.)
    blt_heap_reset(&g_e);
    blt_alloc_init(&g_e.alloc, MF_VTX_REGION, sizeof g_srcdram - MF_VTX_REGION);
    blt_begin_frame(&g_e, /*target_buf=*/0, /*clear=*/0, /*clear_color=*/0);
    g_frame_active = true;
}

// Task 7: production entry point for clear()/draw() — the frame loop never
// calls frame_begin() directly, so the first clear/draw of a frame opens it.
static void mf_ensure_frame(void) {
    if (!g_frame_active) mf_frame_begin();
}

static void mf_clear(RSurface *d, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mf_ensure_frame();
    (void)a;   // fabric FILL writes opaque RGB565; no alpha channel on the wire
    int w = d->w < BLT_FB_WIDTH  ? d->w : BLT_FB_WIDTH;
    int h = d->h < BLT_FB_HEIGHT ? d->h : BLT_FB_HEIGHT;
    blt_fill(&g_e, 0, 0, w, h, mf_rgb565(r, g, b));
}

static void mf_draw(RSurface *d, const BVtx *v, int triCount,
                    const RTexture *t, RBlend bl, float ar, uint32_t tex_key) {
    (void)tex_key;
    mf_ensure_frame();
    // FBO / non-default render target: the fabric has one scanout FB, so
    // render-to-texture targets fall back to the SW rasterizer (writes d->rgba).
    if (g_defRGBA && d->rgba != g_defRGBA) { backend_sw.draw(d, v, triCount, t, bl, ar, tex_key); return; }
    // Premultiplied-alpha source-over (dst = src + dst*(1-a)) has no exact fabric
    // blend; keep it on SW for bring-up (see raster_backend_convert.h).
    if (bl == RB_PREMULT)        { backend_sw.draw(d, v, triCount, t, bl, ar, tex_key); return; }
    if (triCount <= 0) return;

    // ── stage the texture page as RGB565 ──────────────────────────────────────
    int tw, th;
    const bool textured = (t && t->valid && t->rgba);
    if (textured) { tw = t->w; th = t->h; } else { tw = 1; th = 1; }
    if (tw <= 0 || th <= 0 || (size_t)tw * th > MF_TEX_TEXELS) {
        fprintf(stderr, "backend_mfgpu: texture %dx%d exceeds scratch (%d texels) - draw dropped\n",
                tw, th, MF_TEX_TEXELS);
        return;
    }
    bool has_key = false;
    if (textured) {
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                g_texscratch[(size_t)y * tw + x] = mf_texel565(t, x, y, &has_key);
    } else {
        g_texscratch[0] = 0xFFFF;   // 1x1 opaque white -> vertex color passthrough
    }
    blt_surface_ref_t tex = blt_upload(&g_e, g_texscratch, tw, th, tw * 2);
    if (!tex.valid) {
        fprintf(stderr, "backend_mfgpu: texture upload overflow (%dx%d) - draw dropped\n", tw, th);
        return;
    }

    // ── convert + push the vertices ───────────────────────────────────────────
    int nverts = triCount * 3;
    if (nverts > MF_MAX_VERTS) {
        fprintf(stderr, "backend_mfgpu: %d tris exceed vertex scratch (%d verts) - draw dropped\n",
                triCount, MF_MAX_VERTS);
        return;
    }
    float min_vtx_a = 1.0f;
    for (int i = 0; i < nverts; i++) {
        g_vtxscratch[i] = bvtx_to_blt(&v[i], tw, th);
        if (v[i].a < min_vtx_a) min_vtx_a = v[i].a;
    }
    uint32_t eoff = blt_push_tris(&g_e, g_vtxscratch, triCount);
    if (eoff == 0xFFFFFFFFu) {
        fprintf(stderr, "backend_mfgpu: vertex push overflow (%d tris) - draw dropped\n", triCount);
        return;
    }

    // ── emit the TRILIST header (header alpha 255: CONST_ALPHA rides vtx.a) ────
    (void)ar;   // the fabric tri path has no alpha-test; GM's threshold is ~0
    //
    // Colorkey-aware blend selection (Task 6): a texture that staged any texel
    // as MF_COLORKEY (has_key) is a hard-edged (alpha in {0,255}) cutout — if
    // the draw is also fully opaque at the vertex level (min_vtx_a*255 >= 254),
    // BLT_BLEND_COLORKEY reproduces the SW oracle's per-texel-alpha cutout
    // exactly (mod 565 rounding). Otherwise fall back to the Task 5 path.
    //
    // KNOWN LIMITATION: a *faded* cutout sprite (keyed texture drawn with
    // vtx.a < 254, e.g. a fade-out) cannot combine colorkey + const-alpha in a
    // single fabric TRILIST pass (blt_tri.c's blend switch is one case, not a
    // pipeline) — it falls back to CONST_ALPHA below, which composites the key
    // colour's pixels too (no cutout) while fading. Soft/faded transparency
    // needs real per-texel alpha in the fabric raster path; that's a future RTL
    // item, out of scope here.
    uint8_t blend_mode;
    uint16_t colorkey;
    if (has_key && min_vtx_a * 255.0f >= 254.0f) {
        blend_mode = BLT_BLEND_COLORKEY;
        colorkey = MF_COLORKEY;
    } else {
        blend_mode = rblend_to_blt(bl);
        colorkey = 0;
    }
    if (blt_trilist(&g_e, tex, blend_mode, colorkey, /*alpha=*/255,
                    eoff, triCount) != 0)
        fprintf(stderr, "backend_mfgpu: blt_trilist emit failed (%d tris) - draw dropped\n", triCount);
}

static void mf_frame_end(void);   // forward decl: defined below, called from mf_present

// Task 7: production entry point for present() — the frame loop calls
// clear()/draw() then present() (never frame_end() directly). Execute the
// accumulated ring into g_fb565 (mf_frame_end already does the blt_execute)
// and close the frame so the next clear/draw starts fresh. If no clear/draw
// happened this "frame" (g_frame_active false), there is nothing to execute.
static void mf_present(const RSurface *) {
    if (g_frame_active) {
        mf_frame_end();
        g_frame_active = false;
    }
}

static void mf_frame_end(void) {
    blt_end_frame(&g_e);
    memset(g_fb565, 0, sizeof g_fb565);
    if (g_e.overflow) {
        fprintf(stderr, "backend_mfgpu: emitter overflow this frame - frame dropped\n");
        return;   // nothing safe to execute this frame
    }
    int n = g_e.cmd_count;
    if (n > MF_MAX_CMDS) n = MF_MAX_CMDS;
    for (int i = 0; i < n; i++)
        blt_unpack_cmd(g_ring + (size_t)i * BLT_CMD_BYTES, &g_cmds[i]);
    // The whole g_srcdram is the source region: vertices at low offsets, texture
    // pages above MF_VTX_REGION. blt_execute only reads (and clamps OOB), so
    // passing the full size keeps both offset ranges in-bounds.
    blt_surface_heap_t heap = { g_srcdram, sizeof g_srcdram, nullptr, nullptr };
    blt_execute(g_fb565, &heap, g_cmds, n);
}

// Task 7 device wiring: the fabric framebuffer, for main.cpp to hand straight
// to NativeVideoWriter_WriteFrame after present() (g_fb565 is already RGB565 —
// no Blitter_ToRGB565 conversion needed). w/h are BLT_FB_WIDTH x BLT_FB_HEIGHT
// (320x240, refmodel/blitter_ref.h), the fixed fabric scanout geometry — same
// as MISTER_WIDTH/MISTER_HEIGHT today, so callers can use either pair, but the
// row stride must always be computed from *this* w (BLT_FB_WIDTH), not assumed.
extern "C" const uint16_t *RasterBackend_MFGPU_GetFB565(int *w, int *h) {
    if (w) *w = BLT_FB_WIDTH;
    if (h) *h = BLT_FB_HEIGHT;
    return g_fb565;
}

// ---- Host validation only — NOT part of the RasterBackend vtable ----------
// Copies a tightly-packed w x h (<= BLT_FB_WIDTH x BLT_FB_HEIGHT) region of the
// last blt_execute'd RGB565 target out for the host parity tests to diff against
// backend_sw's output. No device code path calls this.
extern "C" void RasterBackend_MFGPU_TestCopyFB565(int w, int h, uint16_t *out) {
    if (w > BLT_FB_WIDTH)  w = BLT_FB_WIDTH;
    if (h > BLT_FB_HEIGHT) h = BLT_FB_HEIGHT;
    for (int y = 0; y < h; y++)
        memcpy(out + (size_t)y * w, g_fb565 + (size_t)y * BLT_FB_WIDTH,
               (size_t)w * sizeof(uint16_t));
}

// Host/Task-6 hook: tell the backend which surface pixels are the default fb, so
// draw() can route FBO / render-to-texture targets to the SW fallback. Pass the
// default RSurface's rgba pointer (NULL restores "everything is default fb").
extern "C" void RasterBackend_MFGPU_SetDefaultSurface(const uint8_t *rgba) {
    g_defRGBA = rgba;
}

extern "C" const RasterBackend backend_mfgpu = {
    "mfgpu", mf_frame_begin, mf_clear, mf_draw, mf_present, mf_frame_end,
};
