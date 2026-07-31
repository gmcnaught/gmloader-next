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
// (288x216, refmodel/blitter_ref.h) — that is the wire contract, not a
// per-draw parameter. clear() targets are clamped to that size; the engine's
// render surface is comfortably inside it.
//
// ── FRAME MODEL (Task 2 update: persistent texture cache) ────────────────────
// blt_emitter_init / blt_vtx_buf_init / blt_alloc_init are ONE-TIME startup
// (they wire the ring, texture heap and vertex buffer). Only blt_begin_frame
// resets per-frame state (command list + vertex cursor). mf_init_once() does
// the one-time wiring; mf_frame_begin no longer resets the texture heap
// (no blt_heap_reset) — staged texture pages are cached by identity in
// g_texcache and persist across frames (see stage_texture below), so a draw
// that reuses a texture id reuses its already-uploaded page instead of
// re-uploading every frame.
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
#include "fps_overlay.h"   // [OSD-fps] clamp + 7-seg digit table (Solarus port)
#include "mf_seam_stat.h"   // [Phase 4 Stage A] submit-seam decomposition
#include "mf_pending_clear.h"   // [Phase 4 Stage B] deferred full-screen clear
extern "C" {
#include "blt_emitter.h"
#include "blt_wire.h"
#include "blitter_ref.h"
}
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // getenv (GMLOADER_MFGPU_HEAPLOG diagnostic, Task 9 bring-up)
#if defined(__ARM_NEON)
#include <arm_neon.h>   // mf_stage_texels' vector body (see its definition)
#endif
#include <time.h>     // [Phase 1 B2] clock_gettime for the publish timestamp, which is
                      // recorded in BOTH builds (mf_device_publish is not device-only)

// The mfgpu backend scans the fabric FB out as the display verbatim — the two
// geometry roots must be identical (native-288x216 single-source rule).
#ifdef MISTER_WIDTH
static_assert(MISTER_WIDTH == BLT_FB_WIDTH && MISTER_HEIGHT == BLT_FB_HEIGHT,
              "MISTER_WIDTH/HEIGHT must equal BLT_FB_WIDTH/HEIGHT");
#endif

extern "C" const RasterBackend backend_sw;   /* FBO / RB_PREMULT fallback */

// One frame's worth of host-side emitter buffers. Static/file-scope is fine for
// this single-threaded backend; Task 6 points these at the real DDR ring/heap.
enum {
    MF_RING_CAP    = 64 * 1024,                  // capacity of ONE host-oracle ring arena
    // [Phase 1 B1] DOUBLE-BUFFERED vertex region: two 128 KiB halves, alternated per
    // frame, so the host can build frame N+1's vertices while the fabric reads N's.
    // Grown rather than split because 128 KiB is ~2730 triangles of headroom and halving
    // it is a real ceiling; the cost is 128 KiB out of a ~14.4 MiB texture heap.
    // Needs NO RTL change: TRILIST commands carry the vertex offset, so the fabric
    // follows wherever blt_vtx_buf_init points.
    MF_VTX_HALF    = 128 * 1024,                 // one frame's TRILIST vertex buffer
    MF_VTX_REGION  = 2 * MF_VTX_HALF,            // both halves; heap starts above this
    MF_TEX_HEAP    = 32u * 1024 * 1024,          // persistent texture pages (32MB)
    MF_SRCDRAM_CAP = MF_VTX_REGION + MF_TEX_HEAP,
    MF_MAX_CMDS    = MF_RING_CAP / BLT_CMD_BYTES,
    MF_MAX_VERTS   = 8192,
    MF_TEX_TEXELS  = 2048 * 2048,                // scratch = largest single page (8MB)
    // resident-page table size. Sub-region residency (per-sprite-quad staging)
    // caches by (tex_id, cropped rect) instead of one entry per whole page, so a
    // batched spritesheet draw can hold many small sub-rects at once -- 256 slots
    // covers a frame's distinct sprite-quad regions with headroom (spec tuning
    // default). Every entry is a real heap page, but each sub-rect is a few KB.
    MF_TEX_CACHE_N = 256,                         // resident-page table size
};
// [Phase 1 B1] TWO host-oracle ring arenas. The device has ring A/ring B at fixed DDR
// offsets; the oracle needs the same disjointness or the alternation is untestable and
// case_arena_alternates would be asserting against one buffer relabelled.
static uint8_t       g_ring[2][MF_RING_CAP];
// Deliberate ~40MB static footprint (32MB texture heap here + 8MB conversion
// scratch below): safe and unremarkable on the MiSTer ARM's DDR3, not a
// mistake to "optimize away".
static uint8_t       g_srcdram[MF_SRCDRAM_CAP];
static blt_emitter_t g_e;
static blt_cmd_t     g_cmds[MF_MAX_CMDS];
static blt_vtx_t     g_vtxscratch[MF_MAX_VERTS];
// See g_srcdram above: this is the other half of the ~40MB deliberate footprint.
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
// Frames opened since init. Only consumed by the GMLOADER_MFGPU_UVLOG capture
// (to bound it to the first few frames and to tag its lines).
static int           g_frame_no = 0;

// Persistent resident-texture cache. Keyed by GL texture id (tex_key). Survives
// across frames (mf_frame_begin no longer resets the heap); entries are freed on
// GL invalidate/delete (Task 3) or LRU eviction under heap pressure (Task 4).
// A resident source page. Sub-region residency keys each entry by
// (key, rx,ry,rw,rh): `key` is the GL texture id and (rx,ry,rw,rh) is the cropped
// texel rect of `key` that this page holds (a whole-page entry is rx=ry=0,
// rw=full-w, rh=full-h). Two draws of the same texture that sample different
// sub-rects get distinct entries; invalidate frees every entry with a matching
// key regardless of rect.
struct MfTexEntry { uint32_t key; bool used; bool has_key; bool mask_only; blt_surface_ref_t ref; uint64_t lru;
                    uint16_t rx, ry, rw, rh; };
static MfTexEntry g_texcache[MF_TEX_CACHE_N];
static uint64_t   g_lru_clock   = 0;
static uint32_t   g_upload_count = 0;   // real blt_upload calls since reinit (test hook)
static uint32_t   g_stage_count  = 0;   // BLT_OP_STAGE emits since reinit (FO Task 3 test hook)
static uint32_t g_arena            = 0;      // [Phase 1 B1] which arena this frame owns (0/1)
static bool     g_fabric_pending   = false;  // a submitted batch has not been acked yet
static uint32_t g_pending_seq      = 0;      // the submit_seq we are waiting on
// [Phase 2 host lever] Seam witnesses: the seq the last publish rang the doorbell with, and
// the seq the last await polled C_DONE for. They must be equal — an await chasing any other
// sequence can never be satisfied. Set on both device and host paths so the oracle can
// assert it; C_DONE itself does not exist off-device.
static uint32_t g_last_published_seq = 0;
static uint32_t g_last_awaited_seq   = 0;
static bool     g_frame_dropped    = false;  // this frame is being dropped (emit nothing)
static uint32_t g_drop_count       = 0;      // frames dropped since reinit (test hook)
static int      g_test_fabric_busy = -1;     // host tests force the predicate (-1 = ask for real)
static uint32_t g_drop_run         = 0;      // consecutive drops (bounded by MF_DROP_LIMIT)
static int      g_last_trilist_blend = -1;   // blend mode of the last TRILIST emitted (test hook)

// [duplicate-draw elimination] The measured device frame graph emits the app-surface
// composite TWICE per frame, byte-identical: same target, same source texture, same
// full-screen rect, same blend (see the frame-graph capture in the ledger). Each of those
// passes is 2 triangles that each walk the FULL-SCREEN bounding box, and a harness A/B
// prices the pair at ~16ms of a frame -- so the redundant one is ~8ms, about a quarter of
// the whole frame.
//
// Dropping it needs no visual judgement, because a repeat is provably a no-op when the
// blend is idempotent. An opaque draw goes out as BLT_BLEND_COPY (or BLT_BLEND_COLORKEY
// when the region is keyed); COPY writes src over dst and COLORKEY writes src where it is
// not the key, so running either a second time over its own output lands on exactly the
// same pixels. A translucent draw does NOT qualify -- CONST_ALPHA composites, so a repeat
// darkens -- and is deliberately left alone.
//
// Self-verifying: if the two draws are ever not truly identical (different UVs, a moved
// vertex), the comparison simply fails and both are emitted, costing one memcmp.
enum { MF_DUP_MAX_TRIS = 16 };   // covers full-screen quads; keeps the compare trivial
static struct {
    bool     valid;
    uint32_t fbo, tex_key;
    int      bl;
    float    ar;
    int      triCount;
    BVtx     v[MF_DUP_MAX_TRIS * 3];
} g_last_draw;
static uint32_t g_dup_skipped = 0;   // draws elided this run (test hook + device log)

// [strip in-game CRT simulation] The game ships its own old-TV effect (obj_old_tv, with
// Draw_0 + Draw_64 events -- confirmed in game.droid; there are no shd_* shader assets and
// every draw runs under one program, so it is done with plain geometry and alpha). We do not
// want it: the output is always driven either to a real CRT or through the MiSTer framework,
// which applies its own CRT treatment, so the in-game simulation is both wasted fabric time
// and double-applied. There is no in-game option to disable it (options.ini holds only
// DisplayName), so it has to be dropped here.
//
// Signature, from the measured frame graph: after the app surface is presented opaquely to
// the default target, the SAME surface is drawn over itself again at ~70% alpha. That second
// pass is the ghost. It is a full-screen pass, and a screen-aligned quad is 2 triangles that
// EACH walk the full-screen bounding box, so dropping it is worth ~8ms of a ~31ms frame.
//
// THE PRECEDING-OPAQUE-PRESENT CONDITION IS LOAD-BEARING, not a nicety: obj_fade_in /
// obj_fade_out are legitimate transitions, and a fade-from-black can legitimately draw the
// app surface TRANSLUCENTLY over a cleared target. Dropping that would blank the screen for
// the whole transition. Requiring that an opaque present already landed THIS FRAME means the
// image is provably on screen before we drop anything, so a fade can never be swallowed.
static bool     g_appsurf_presented = false;   // an opaque app-surface present landed this frame
static uint32_t g_crt_stripped      = 0;       // ghost passes dropped (test hook + device log)
static int mf_strip_crt(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GMLOADER_MFGPU_STRIP_CRT");
        v = (e && *e) ? atoi(e) : 1;   // on by default: we never want the in-game CRT sim
    }
    return v;
}

// Would this draw go out with an idempotent blend? Mirrors mf_emit_group's decision:
// fully opaque => COPY or COLORKEY (both idempotent), anything else => CONST_ALPHA.
//
// NOTE `ar` is alphaRef -- the ALPHA-TEST THRESHOLD (gm_AlphaRefValue), not an opacity
// multiplier, and blitter.cpp:599 passes a constant 0.0f. An earlier version of this
// function required ar >= 0.999 as if it were opacity, which made the elision dead on
// arrival: it could never fire on a real draw. The threshold is irrelevant to idempotency
// anyway -- alpha test discards the same pixels every time -- and the signature comparison
// at the call site still requires the two draws to share the same ar, so a differing
// threshold blocks elision there.
static bool mf_draw_is_idempotent(const BVtx *v, int nverts, RBlend bl) {
    if (bl != RB_NONE && bl != RB_ALPHA) return false;   // ADD/MULTIPLY accumulate
    for (int i = 0; i < nverts; i++) if (v[i].a < 0.999f) return false;   // per-vertex opacity
    return true;
}
// Default ~1s at 60Hz: long enough that a short stall is covered by dropping rather than
// stomping, short enough that a fabric which never acks costs a second of black instead of a
// permanent freeze. Overridable with GMLOADER_MFGPU_DROP_LIMIT because the right value is an
// open experimental question: measured stalls run to ~18s, so 60 frames still crosses a long
// one ~6 times, and each crossing rebuilds the ring under a live batch. Setting it above a
// full stall (>=400 frames at the game's ~19fps) makes an episode completely stomp-free, which
// is how we test whether recurring stalls are partly a self-sustaining tail of earlier stomps
// (staged texture bytes and the ring persist across frames) rather than independent events.
enum { MF_DROP_LIMIT_DEFAULT = 60 };
static uint32_t mf_drop_limit(void) {
    static uint32_t v = 0;
    if (!v) {
        const char *e = getenv("GMLOADER_MFGPU_DROP_LIMIT");
        long n = (e && *e) ? atol(e) : 0;
        v = (n > 0) ? (uint32_t)n : (uint32_t)MF_DROP_LIMIT_DEFAULT;
    }
    return v;
}

static uint32_t   g_tex_heap_cap = 0;   // 0 => full MF_TEX_HEAP; else test override

// Snapshot of g_lru_clock taken at the start of the current frame (see
// mf_frame_begin). Any cache entry whose .lru is ABOVE this floor was
// touched (hit or staged) during THIS frame, and is therefore "pinned":
// blt_trilist already emitted a command referencing its heap offset, and
// that command isn't consumed until blt_execute runs in mf_frame_end, so
// freeing/reusing the offset before then would alias an already-emitted
// draw onto different pixels. evict_one_lru() only considers entries with
// .lru <= floor (untouched this frame) as eviction candidates.
static uint64_t   g_lru_frame_floor = 0;
// [Phase 1 B3] How many times eviction was ATTEMPTED (not how many succeeded). A retention
// test that cannot show the heap was under pressure cannot distinguish "the texture was
// protected" from "nothing was ever at risk"; this is what makes that distinction assertable.
static uint32_t   g_evict_attempts = 0;
// [Phase 1 B3] Times the pin-aware INSERT guard refused a full-table insert. The direct
// witness that that guard fired: the upload counter cannot serve, because g_upload_count is
// bumped before the guard runs and the refusal then undoes the upload -- so a refused insert
// still counts as an upload.
static uint32_t   g_cachefull_drops = 0;
// [Phase 1 B3] Why did THIS frame overflow? THREE paths reach mf_frame_end's overflow
// branch, and they have three DIFFERENT remedies -- so a message naming the wrong one sends
// the reader to the wrong lever, which is worse than saying nothing:
//
//   CACHE_FULL - the cache TABLE was full and every entry was pinned by the last two
//                frames. Remedy: more slots (MF_TEX_CACHE_N), or fewer DISTINCT textures
//                per frame. NOT heap bytes -- the heap can be nearly empty when this fires.
//   HEAP_FULL  - texture BYTES exhausted even after eviction. Remedy: size the heap.
//                This is the path the two-frame retention floor made more reachable, since
//                protecting two frames strictly shrinks the eviction-eligible set.
//   UNKNOWN    - neither refusal was recorded, so ring/vertex capacity is what remains.
//                Phrased as what is left rather than asserted, because nothing observed it.
//
// FIRST cause wins, and that is a CORRECTNESS MECHANISM rather than a tie-break preference --
// do not "simplify" it to last-wins.
//
// mf_draw's four staging-failure sites CANNOT distinguish the two causes: stage_texture
// returns an invalid ref for a heap-byte exhaustion and for a pin-aware table refusal alike
// (the cache-full guard returns `bad.valid = 0`, exactly as the !ref.valid path does). So all
// four blanket-assert MF_OVF_HEAP_FULL on any staging failure. That is correct ONLY because
// the cache-full path already called mf_note_ovf_cause(MF_OVF_CACHE_FULL) deeper in the
// stack, inside mf_upload_and_cache, before returning -- and first-wins then discards the
// outer, coarser claim.
//
// Flip this to last-wins and EVERY cache-full frame reports as heap-full: the precise defect
// this enum exists to fix, restored, and pointing readers at the wrong lever again. The
// outer sites depend on the inner note having landed first.
//
// (Secondary, and the reason first-wins is also the right reading: the refusal that started
// the frame's trouble is the informative one; a later different failure is usually its
// consequence.)
enum MfOvfCause { MF_OVF_UNKNOWN = 0, MF_OVF_CACHE_FULL = 1, MF_OVF_HEAP_FULL = 2 };
static MfOvfCause g_frame_ovf_cause = MF_OVF_UNKNOWN;
static inline void mf_note_ovf_cause(MfOvfCause c) {
    if (g_frame_ovf_cause == MF_OVF_UNKNOWN) g_frame_ovf_cause = c;
}
// One definition, used by both the device and oracle overflow sites, so the two cannot drift.
static const char *mf_ovf_cause_str(void) {
    switch (g_frame_ovf_cause) {
    case MF_OVF_CACHE_FULL:
        return "(CACHE-FULL: every texture-cache slot was pinned by the last two frames -- "
               "raise MF_TEX_CACHE_N or draw fewer distinct textures per frame; this is NOT "
               "a heap-size problem)";
    case MF_OVF_HEAP_FULL:
        return "(HEAP-FULL: texture bytes exhausted even after eviction -- size the texture "
               "heap, not the ring)";
    default:
        return "(no heap or cache-full refusal was recorded this frame; ring/vertex capacity "
               "is the remaining cause)";
    }
}
// [Phase 1 B3] The floor EVICTION compares against, which lags g_lru_frame_floor by one
// frame. With the ring double-buffered the host builds frame N+1 while the fabric is still
// reading N, so a texture touched in N is live even though N is "over". Evicting or
// re-staging it writes straight over memory the fabric is mid-read — the same class of
// failure as the in-flight-batch cascade at :707, and just as silent.
static uint64_t   g_lru_evict_floor = 0;

// Free the least-recently-used cached entry that is NOT pinned by this
// frame (see g_lru_frame_floor above). Returns false if no unpinned entry
// is available to evict (the caller's upload then legitimately fails and
// the draw is dropped — correct degradation when a single frame's texture
// working set exceeds the heap).
static bool evict_one_lru(void);

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

// [app-surface render target, step 1] The application-surface FBO id / its
// color-attachment texture id, pushed down by blitter.cpp's
// RasterBackend_MFGPU_SetAppSurface() whenever Blitter_AppSurfaceFBO()/Tex()
// (Task 4) update -- mirrors the g_defRGBA push-down pattern above so this
// backend stays GL-independent. 0/0 = not yet detected (every draw/clear
// routes as "not the app surface", matching today's behavior exactly).
static uint32_t g_appSurfFbo = 0, g_appSurfTex = 0;

// [app-surface render target, step 1] Which fabric composite target the
// emitter's LAST BLT_OP_SET_TARGET pointed at, mirroring blt_wire's
// BLT_TARGET_*; used only to skip a redundant re-emit when consecutive draws
// target the same buffer. Reset to MF_TARGET_WORK at the top of every frame
// (mf_frame_begin) -- blt_execute (and the RTL) always start a fresh command
// list targeting WORK regardless of where the PREVIOUS frame's ring left off,
// so this host-side cache must track that same per-frame reset or an early
// appsurf-targeted draw could silently land on WORK instead.
enum { MF_TARGET_WORK = BLT_TARGET_WORK, MF_TARGET_APPSURF = BLT_TARGET_APPSURF };
static int g_cur_target = MF_TARGET_WORK;

extern "C" void RasterBackend_MFGPU_SetAppSurface(uint32_t fbo, uint32_t tex) {
    g_appSurfFbo = fbo; g_appSurfTex = tex;
}

// [Phase 4 Stage B] Deferred full-screen clear. See mf_pending_clear.h for why
// the decision cannot be made in mf_clear().
static mf_pc_t g_pc;
// Run totals: g_pc itself is reset every frame (mf_frame_begin), so a device
// stat line or a host test that wants "since reinit" totals needs these.
static uint32_t g_pc_dropped_total = 0, g_pc_emitted_total = 0;
// Cached GMLOADER_MFGPU_DEFER_CLEAR read. A file-scope static (not a
// function-local static like the brief's first draft) so a host test can force
// it to re-resolve -- same idiom as mf_trace_v/RasterBackend_MFGPU_TestTraceReset
// below, for the same reason: a function-local static cannot be reset from
// outside the function, and RasterBackend_MFGPU_TestEnvReset needs to.
static int g_defer_clear_v = -1;
static int mf_defer_clear_on(void) {
    if (g_defer_clear_v < 0) {
        const char *e = getenv("GMLOADER_MFGPU_DEFER_CLEAR");
        g_defer_clear_v = (e && *e) ? atoi(e) : 1;   // on by default: this is the lever
    }
    return g_defer_clear_v;
}
// mf_pending_clear's target is a COMPACT slot index (0/1, bounds-checked against
// MF_PC_TARGETS==2 by mf_pc_valid_target). g_cur_target instead holds the raw
// BLT_TARGET_* wire id (WORK=0, APPSURF=2) -- passing 2 straight through would
// fail that bounds check and silently no-op every mf_pc_* call on the app
// surface, which for mf_pc_record means the clear is simply never emitted at
// all. Map explicitly rather than assume the two numberings coincide.
static inline int mf_pc_slot_of(int blt_target) {
    return (blt_target == MF_TARGET_APPSURF) ? 1 : 0;
}
static inline int mf_pc_target_of(int slot) {
    return (slot == 1) ? MF_TARGET_APPSURF : MF_TARGET_WORK;
}

static inline uint16_t mf_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline int   mf_clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float mf_clamp01(float x)              { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

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

// ── Coverage estimator: rect-clip a triangle, return its clipped area ───────
// Pure function: six vertex floats in, clipped area out via Sutherland-Hodgman
// against [0,BLT_FB_WIDTH] x [0,BLT_FB_HEIGHT] — no global/device state. Lives
// HERE, ahead of the MISTER_NATIVE_VIDEO block below, so it compiles and links
// in the plain host build too (raster-backend-test does not define
// MISTER_NATIVE_VIDEO); exposed to the host unit test as
// RasterBackend_MFGPU_TestClipTriArea() near the bottom of this file (grep
// "Host validation only") and exercised by gmloader/mister/mf_cov_clip_test.cpp.
//
// A triangle clipped against a convex rectangle is a convex polygon of at most
// 3+4=7 vertices (each of the 4 half-plane clips below can add at most one
// vertex to the running polygon). Its area is the standard shoelace sum.
//
// Winding-independent: every clip test compares a single coordinate against a
// constant plane, so a CW vs CCW input triangle still lands on the same side
// of each plane and produces the same clipped polygon (up to point order);
// the shoelace sum's absolute value is itself winding-independent.
static double mf_clip_tri_area(float x0, float y0, float x1, float y1,
                                float x2, float y2) {
    struct Pt { double x, y; };
    Pt poly[7] = { { (double)x0, (double)y0 },
                    { (double)x1, (double)y1 },
                    { (double)x2, (double)y2 } };
    int n = 3;

    // Clip the running polygon against one half-plane (`inside`), inserting
    // the exact edge/plane intersection (`isect`) wherever consecutive
    // vertices straddle it. Standard Sutherland-Hodgman inner loop; `plane`
    // carries the clip constant (0.0 or BLT_FB_WIDTH/HEIGHT) into both
    // callbacks so one lambda pair serves both the low and high edge of an
    // axis.
    auto clip = [&](bool (*inside)(const Pt &, double), double plane,
                    Pt (*isect)(const Pt &, const Pt &, double)) {
        Pt out[7];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const Pt &cur  = poly[i];
            const Pt &prev = poly[(i + n - 1) % n];
            bool cur_in  = inside(cur, plane);
            bool prev_in = inside(prev, plane);
            if (cur_in) {
                if (!prev_in) out[m++] = isect(prev, cur, plane);
                out[m++] = cur;
            } else if (prev_in) {
                out[m++] = isect(prev, cur, plane);
            }
        }
        n = m;
        for (int i = 0; i < n; i++) poly[i] = out[i];
    };

    clip([](const Pt &p, double lo) { return p.x >= lo; }, 0.0,
         [](const Pt &a, const Pt &b, double lo) {
             double t = (lo - a.x) / (b.x - a.x);
             return Pt{ lo, a.y + t * (b.y - a.y) };
         });
    if (n == 0) return 0.0;
    clip([](const Pt &p, double hi) { return p.x <= hi; }, (double)BLT_FB_WIDTH,
         [](const Pt &a, const Pt &b, double hi) {
             double t = (hi - a.x) / (b.x - a.x);
             return Pt{ hi, a.y + t * (b.y - a.y) };
         });
    if (n == 0) return 0.0;
    clip([](const Pt &p, double lo) { return p.y >= lo; }, 0.0,
         [](const Pt &a, const Pt &b, double lo) {
             double t = (lo - a.y) / (b.y - a.y);
             return Pt{ a.x + t * (b.x - a.x), lo };
         });
    if (n == 0) return 0.0;
    clip([](const Pt &p, double hi) { return p.y <= hi; }, (double)BLT_FB_HEIGHT,
         [](const Pt &a, const Pt &b, double hi) {
             double t = (hi - a.y) / (b.y - a.y);
             return Pt{ a.x + t * (b.x - a.x), hi };
         });
    if (n < 3) return 0.0;   // clipped away entirely, or collapsed to a point/edge

    double area2 = 0.0;
    for (int i = 0; i < n; i++) {
        const Pt &p = poly[i];
        const Pt &q = poly[(i + 1) % n];
        area2 += p.x * q.y - q.x * p.y;
    }
    return (area2 < 0.0 ? -area2 : area2) * 0.5;
}

// Host-side covered-pixel estimate. No fabric counter publishes covered_px, and
// the ~3.1x overdraw figure on record was back-calculated by dividing device
// dpath by the sim's 13 cyc/px — which assumes the very throughput the number is
// then used to characterize. This breaks that circularity.
//
// Sum of each triangle's RECT-CLIPPED area (mf_clip_tri_area above) into a
// running accumulator = covered area INCLUDING cross-triangle overdraw. A
// triangle mostly or fully off the render target now contributes only its
// true on-screen slice, not its full geometric area — the previous version
// summed unclipped |cross|/2 and merely clamped a SINGLE triangle's
// contribution to one screen's worth of pixels, which does not bound a
// scene's total (many partly-offscreen triangles each still counted their
// full area) and was measured to inflate cov_px_est by a SCENE-DEPENDENT
// factor: 2.9x between a static title screen and a scrolling gameplay scene
// whose parallax layers extend past both edges (8.67 vs 3.01 cyc/px on
// device, when the true ratio should be ~constant — see the ledger). It
// remains an ESTIMATE even after clipping: it still does not model per-pixel
// depth/blend rejection, overdraw REJECTED by blend state within a draw
// (every clipped triangle still counts fully, whether or not its pixels
// survive blending), or sub-pixel/edge rasterization rules. Degenerate
// (zero-area or collapsed) triangles count as zero, never NaN.
//
// Called from mf_emit_group ONLY, after a triangle group has actually been
// pushed into the fabric command ring (blt_push_tris + a successful
// blt_trilist) — review found the original call site (blitter.cpp, the
// generic backend-dispatch point) counted triangles the active backend went
// on to silently discard (in-flight-batch drop, self-referential appsurf
// guard, the CRT-ghost strip — on by default and ~124,000px/frame in
// gameplay, duplicate-draw elision), inflating cov_px_est by ~6.6x on
// device. Reported as cov_px_est, never as exact. Gated by mf_stat_on() at
// the call site so this costs nothing when GMLOADER_MFSUBMIT_STAT is unset;
// backend_sw never calls this at all, so the SW backend reports zero
// coverage (expected for this phase — we're measuring the fabric).
//
// Declared out here (not inside the MISTER_NATIVE_VIDEO block) purely so the
// call site in mf_emit_group — which is unconditional — type-checks in a
// host build too.
//
// mf_stat_on() itself lives here for the same reason (mf_emit_group's guard
// on it is unconditional too); the perf-counter instrumentation this gates on
// device (C_DONE busy-wait characterization, min≈max over a 30-submit window)
// is documented at its device-only call site in mf_submit_stat below, which itself
// checks mf_stat_on() first and is genuinely zero cost when GMLOADER_MFSUBMIT_STAT is
// unset. [Phase 4 Stage A review] That does NOT extend to the submit-seam stamps
// (g_seam_be/g_seam_ar in mf_publish_barrier, g_publish_t0 and the mf_seam_add call in
// mf_device_publish): the two clock_gettime stamps and the mf_seam_add call run every
// frame, in BOTH builds (device and host-oracle) -- not gated on mf_stat_on() at all.
// With the stat knob OFF, mf_submit_stat's own early return (`if (!mf_stat_on()) return;`
// below) means g_seam_frame_ms and g_seam_blocked are never written for the life of the
// process: g_seam_frame_ms stays 0.0 and g_seam_blocked stays permanently false, so the
// accumulator's `blocked`/`notice` split in mf_seam_add accrues as if nothing ever
// blocked. That is harmless TODAY only because the sole reader of the accumulator is the
// print in mf_submit_stat, gated on this same knob -- a future consumer that read g_seam
// without also checking mf_stat_on() would silently see zeros. The unconditional
// stamping itself is cheap regardless of the knob: even a clock_gettime call that misses
// the vDSO and pays a full ~2 us syscall is 0.012% of the 16.6882 ms scanout period.
static int mf_stat_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFSUBMIT_STAT"); v = (e && *e) ? 1 : 0; }
    return v;
}

static double g_cov_px_accum = 0.0;
// [Phase 2 host lever] The accumulator's value snapshotted at publish, i.e. the estimate
// belonging to the batch whose fabric counters mf_submit_stat will read. Needed because the
// await moved after this frame's draws; see mf_publish_barrier.
static double g_cov_px_published = 0.0;

// [Phase 1 A4] The derived covered-pixel figures, as a PURE function of its inputs.
//
// Deliberately outside #ifdef MISTER_NATIVE_VIDEO and free of globals. Reading C_FLAGS.hi
// needs a fabric and cannot be tested off-device -- but the arithmetic OVER that value is a
// total function with definite correct behaviour, and is testable anywhere. Keeping the two
// fused is what let a provenance bug (the "estimated" marker covering cyc_px but not
// overdraw, though both derive from the same denominator) survive into a change whose entire
// purpose was making provenance explicit.
struct MfCovDerived {
    double cov_den;     // the denominator actually used
    double cyc_px;      // dpath_ms * clk_MHz * 1000 / cov_den
    double overdraw;    // cov_den / screen_px
    double est_ratio;   // cov_est / cov_exact; < 0 means "not computable"
    int    estimated;   // 1 = fell back to the estimate; cov_den is NOT the fabric's count
};
static MfCovDerived mf_derive_cov(double dpath_ms, double cov_exact, double cov_est,
                                  double screen_px, double clk_mhz) {
    MfCovDerived d;
    // Fall back when the fabric reports nothing -- an older bitstream without A4, where
    // C_FLAGS.hi reads 0. Without this, cyc_px divides by zero and prints 0.0, which reads
    // as "pixels are free": the most misleading possible output for a perf task.
    d.estimated = (cov_exact > 1.0) ? 0 : 1;
    d.cov_den   = d.estimated ? cov_est : cov_exact;
    d.cyc_px    = (d.cov_den > 1.0) ? (dpath_ms * clk_mhz * 1000.0) / d.cov_den : 0.0;
    d.overdraw  = (screen_px > 0.0) ? d.cov_den / screen_px : 0.0;
    // Only meaningful against a real exact count. Reporting 0.00 would put an out-of-domain
    // value in a field whose invariant is >= 1.0 (the estimate is blind to per-pixel
    // rejection, so it must over-count, never under-count).
    d.est_ratio = d.estimated ? -1.0 : cov_est / cov_exact;
    return d;
}


static void mf_cov_add_triangle(float x0, float y0, float x1, float y1, float x2, float y2) {
    g_cov_px_accum += mf_clip_tri_area(x0, y0, x1, y1, x2, y2);
}

// ── CONTROL-BLOCK REGISTER INDICES + WRITE TRACE (Phase 1 B2) ────────────────
// Hoisted out of the device block so mf_device_publish's body can compile in BOTH builds.
// These are qword indices into BLTCTRL, the wire contract in
// 3rdparty/mfgpu/docs/blitter-protocol.md §2 — not addresses, so they are build-independent.
enum {
    MF_C_SUBMIT = 0, MF_C_CMDCOUNT = 1, MF_C_TARGET = 2, MF_C_CLEAR = 3,
    MF_C_FLAGS = 4, MF_C_DONE = 5, MF_C_STATUS = 6, MF_C_SRCSEL = 7,
};

// [Phase 1 B2] Publish writes a SINGLE shared control block and the fabric latches every
// word of it in its prologue the moment it sees a new C_SUBMIT. So the ORDER of these
// writes is the contract, not an implementation detail: the doorbell must be last and the
// barrier must precede it, or the fabric latches values the host had not finished writing.
//
// None of that was observable on the host oracle. mf_ctrl_wr's store compiles out there, so
// publish's entire body was invisible: counters see how many times the seam ran, the
// pairing witness sees in what order its two halves ran, and neither reaches INSIDE publish.
// Measured escapes before this shim, each passing all 69 cases: barrier moved after the
// doorbell, C_SUBMIT written first, and any single control word simply deleted.
//
// So mf_ctrl_wr records every write to a trace buffer in both builds and stores only on
// device — the same lift that made the seam itself testable, applied one level down.
//
// The trace is HOST-ONLY. Nothing on device ever reads it, and compiling it in would perturb
// the exact path the shim exists to protect — so on a device build mf_ctrl_wr below is
// byte-for-byte the volatile store it always was. Nothing is lost: the write ORDER lives in
// publish's source, which is identical in both builds, so checking it on the oracle checks
// the order the device executes.
enum { MF_TRACE_MAX = 32, MF_TRACE_BARRIER = -1 };   // -1 is not a valid qword index
struct MfCtrlTraceEnt { int reg; uint32_t val; };
static MfCtrlTraceEnt g_ctrl_trace[MF_TRACE_MAX];
static uint32_t       g_ctrl_trace_n        = 0;
static uint32_t       g_ctrl_trace_overflow = 0;   // sticky; asserted 0 by the host test

static inline void mf_trace_add(int reg, uint32_t v) {
#ifndef MISTER_NATIVE_VIDEO
    if (g_ctrl_trace_n >= MF_TRACE_MAX) {
        // FAIL LOUDLY. A trace that silently truncates makes the ordering assertion vacuous:
        // "the doorbell is last" would pass because the later writes were dropped, not
        // because they never happened. Sticky, so it cannot be missed by a later reset.
        if (!g_ctrl_trace_overflow)
            fprintf(stderr, "backend_mfgpu: FATAL control-write trace overflow (>%d entries "
                            "in one publish) — ordering assertions are no longer valid\n",
                    MF_TRACE_MAX);
        g_ctrl_trace_overflow++;
        return;
    }
    g_ctrl_trace[g_ctrl_trace_n].reg = reg;
    g_ctrl_trace[g_ctrl_trace_n].val = v;
    g_ctrl_trace_n++;
#else
    (void)reg; (void)v;   // device: no trace at all (see above)
#endif
}

static inline void mf_trace_reset(void) {
#ifndef MISTER_NATIVE_VIDEO
    g_ctrl_trace_n = 0;   // overflow is deliberately NOT cleared: it is sticky
#endif
}

// [Phase 4 Stage A] Submit-seam stamps. g_publish_t0 (declared further below, with the
// rest of the publish/await bookkeeping) IS the doorbell stamp — it is taken immediately
// after the doorbell write in mf_device_publish — so the seam adds only two: barrier entry
// and barrier exit. A sample spans doorbell N to doorbell N+1, so it can only be closed by
// the NEXT publish; g_seam_prev_db carries the opening stamp across the frame boundary.
//
// Declared here, OUTSIDE #ifdef MISTER_NATIVE_VIDEO and ahead of both use sites, because the
// two sites straddle the device-only transport block below: mf_submit_stat (inside it) is
// the first writer of g_seam_frame_ms/g_seam_blocked, while mf_device_publish and the test
// hooks (outside it, compiled in both builds) read g_seam itself. A declaration placed
// between the two would compile on whichever build reaches it first and silently fail to
// compile on the other.
static mf_seam_acc_t   g_seam;
static struct timespec g_seam_be, g_seam_ar, g_seam_prev_db;
static bool            g_seam_have_prev = false;
// The fabric's own compute counter for the batch the current `block` waited on,
// stashed by mf_submit_stat so the pairing survives the deferred await.
static double          g_seam_frame_ms = 0.0;
// Did the host ACTUALLY wait this frame? mf_device_await returns on its first poll
// when C_DONE already matches, so a frame that never waited still spends the few
// microseconds of one uncached read inside it — block_ms is never exactly zero and
// cannot answer this. The await's own `iters` can: it is 0 if and only if the first
// read already matched. Cleared at barrier entry so a frame that skips the await
// (drop run) cannot inherit the previous frame's answer.
static bool            g_seam_blocked = false;
// [Phase 4 Stage A review] Samples the barrier could not legitimately close: the two
// non-success `return true` paths in mf_publish_barrier (the !g_fabric_pending early-out
// and a reclaiming mf_drop_or_reclaim()) reach mf_device_publish without ever re-stamping
// g_seam_ar, so closing a sample there would pair a fresh g_seam_be against a g_seam_ar
// left over from the last successful barrier -- an entire drop run earlier. Both paths set
// g_seam_have_prev = false instead, so that frame contributes no sample; this counts how
// many were skipped. Windowed like g_seam itself: reset alongside mf_seam_reset() below.
static uint32_t         g_seam_incomplete = 0;

static inline double mf_seam_ms(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1e3 +
           (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

#ifdef MISTER_NATIVE_VIDEO
// ── Device fabric transport (FO Task 4; in-place DDR model, mirrors fabric_probe.c)
// On device the emitter binds DIRECTLY to the mmap'd DDR command region: blt_upload/
// blt_stage/blt_push_tris/blt_trilist write straight into DDR, so mf_frame_end only
// publishes the control block + rings the doorbell + polls C_DONE — no blt_execute,
// no per-frame copy. The Maldita core composites into on-chip BRAM and scans itself
// out. Layout + handshake: 3rdparty/mfgpu/docs/blitter-protocol.md §2-3, verified vs
// the milestone-a RTL. (MISTER_NATIVE_VIDEO is the device-build macro — the host
// raster-backend-test target does NOT define it, so it keeps the blt_execute oracle.)
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>   // getenv (GMLOADER_MFSUBMIT_STAT instrumentation)
// <time.h> was here; hoisted to the top includes when mf_device_publish stopped being
// device-only and needed clock_gettime in both builds.
enum {
    MF_DEV_PHYS_BASE = 0x3B000000u,
    MF_DEV_MAP_SIZE  = 0x01000000u,   // 16 MiB dedicated blitter region
    // [Phase 1 B1] DOUBLE-BUFFERED command ring. Must match the RTL exactly:
    // blitter_defs.vh RING_QW = 0x3B000040 (ring A), RING_B_QW = 0x3B040000 (ring B),
    // selected by C_SRCSEL bit 2 (C_RINGSEL_BIT). The 512 KiB region is SPLIT rather
    // than grown so SRC_QW / OFF_HEAP stays put -- blitter_defs.vh flags that constant
    // as a must-match-across-repos hazard.
    // Canonical source for these values is
    // wt-maldita-60fps/fpga/rtl/blitter_defs.vh -- NOT 3rdparty/mfgpu/rtl/blitter_defs.vh,
    // whose RING_QW/SRC_QW are small windowed sim addresses that disagree.
    MF_DEV_RING_OFF  = 0x00000040u,   // ring A: BLTCTRL (8 qwords) precedes it
    MF_DEV_RING_B_OFF= 0x00040000u,   // ring B
    MF_DEV_SRC_OFF   = 0x00080000u,   // SRC_QW = 0x3B080000 (DDR3 source heap) -- UNCHANGED
    MF_DEV_TLBUF_OFF = 0x00F40000u,   // bounds the usable SRC heap (~14.8 MiB)
    // [Phase 3 Stage B] openbor_video_reader.sv SCANFRM_ADDR = byte 0x3BFB0018,
    // i.e. offset 0x00FB0018 in this same 16 MiB mapping (above the SRC heap, in
    // the reader's sentinel-clean tail). Read-only here — see
    // RasterBackend_MFGPU_ScanoutRead below.
    MF_DEV_SCANFRM_OFF = 0x00FB0018u,
    MF_DEV_RING_CAP  = MF_DEV_RING_B_OFF - MF_DEV_RING_OFF,   // 256 KiB, ~8190 commands
    MF_DEV_SRC_CAP   = MF_DEV_TLBUF_OFF - MF_DEV_SRC_OFF,
    MF_DEV_DONE_TIMEOUT_MS = 200,
    // MF_C_* register indices moved above the guard: publish's body compiles in both builds.
};
static volatile uint8_t *g_dev_base = nullptr;   // mmap of 0x3B000000
static volatile uint8_t *g_dev_ctrl = nullptr;   // = base (control block)
static uint8_t          *g_dev_ring = nullptr;   // = base + RING_OFF
static uint8_t          *g_dev_ring_b = nullptr; // [Phase 1 B1] = base + RING_B_OFF
static uint8_t          *g_dev_src  = nullptr;   // = base + SRC_OFF
static bool              g_dev_ok   = false;     // mmap ok -> submits reach the fabric

static inline uint32_t mf_ctrl_rd(int qw) {
    return *(volatile uint32_t *)(g_dev_ctrl + (size_t)qw * 8u);
}
static inline uint32_t mf_ctrl_rd_hi(int qw) {
    return *(volatile uint32_t *)(g_dev_ctrl + (size_t)qw * 8u + 4u);   // high u32 of the qword
}
// Open /dev/mem + mmap the DDR blitter region once (mirrors native_video_writer.c).
static bool mf_ddr_map(void) {
    if (g_dev_base) return g_dev_ok;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { fprintf(stderr, "backend_mfgpu: open /dev/mem failed\n"); return false; }
    void *m = mmap(nullptr, MF_DEV_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)MF_DEV_PHYS_BASE);
    close(fd);   // the mapping outlives the fd
    if (m == MAP_FAILED) { fprintf(stderr, "backend_mfgpu: mmap 0x3B000000 failed\n"); return false; }
    g_dev_base = (volatile uint8_t *)m;
    g_dev_ctrl = g_dev_base;
    g_dev_ring = (uint8_t *)(g_dev_base + MF_DEV_RING_OFF);
    g_dev_ring_b = (uint8_t *)(g_dev_base + MF_DEV_RING_B_OFF);   // [Phase 1 B1]
    g_dev_src  = (uint8_t *)(g_dev_base + MF_DEV_SRC_OFF);
    memset((void *)g_dev_ctrl, 0, MF_DEV_RING_OFF);   // zero the control block
    g_dev_ok = true;
    return true;
}
// Fire-and-forget probe (GMLOADER_MFSUBMIT_NOWAIT=1): ring the doorbell and
// return WITHOUT polling C_DONE, to measure the throughput ceiling when the host
// stops serially waiting out the fabric's ~3-vsync completion latency. Unsafe for
// production (the in-place DDR ring can be re-emitted while the fabric still reads
// it → tearing) — a measurement knob only. Zero cost when unset.
static int mf_nowait_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFSUBMIT_NOWAIT"); v = (e && *e) ? 1 : 0; }
    return v;
}
// How long to sleep between C_DONE polls once the initial spin is exhausted, in
// microseconds (GMLOADER_MFGPU_POLL_US; 0 = pure spin, the default).
//
// MEASURED, DEFAULT DELIBERATELY OFF: the pure spin issues ~25000 UNCACHED C_DONE reads
// per frame for the whole ~30ms the fabric works, so this looked like free DDR bandwidth
// to hand back. A/B on device (120s per arm, same RBF/scene) says it is not:
//   poll_us=0   ~25000 polls/frame  fabric frame ~30.6ms  timeouts 44 reclaims 38
//   poll_us=250  ~2095 polls/frame  fabric frame ~32.6ms  timeouts 58 reclaims 50
// A 12x cut in poll traffic bought no fabric speedup and no fewer stalls (both arms sit
// inside scene-to-scene variation), so host poll pressure is NOT what the fabric is
// waiting on. Kept as a knob because it does idle a core that otherwise spins at 100%,
// which is worth re-testing against the frame-1 wedge rate if that turns out to be
// thermal — the HPS shares a die with the fabric.
enum { MF_POLL_SPIN_ITERS = 2000, MF_POLL_US_DEFAULT = 0 };
static long mf_poll_us(void) {
    static long v = -1;
    if (v < 0) {
        const char *e = getenv("GMLOADER_MFGPU_POLL_US");
        v = (e && *e) ? atol(e) : (long)MF_POLL_US_DEFAULT;
        if (v < 0) v = 0;
    }
    return v;
}
// Doorbell->C_DONE wait budget, overridable with GMLOADER_MFGPU_TIMEOUT_MS (default
// MF_DEV_DONE_TIMEOUT_MS). Giving up on the wait has the SAME hazard NOWAIT documents
// above: the next frame re-emits the in-place DDR ring / vertex buffer / texture heap
// while the fabric is still reading them. This knob exists to measure how much of an
// observed stall is a genuine fabric stall versus that re-emit corrupting the batch
// in flight (raise it far above a stall to remove the host from the equation).
static long mf_timeout_ms(void) {
    static long v = -1;
    if (v < 0) {
        const char *e = getenv("GMLOADER_MFGPU_TIMEOUT_MS");
        long n = (e && *e) ? atol(e) : 0;
        v = (n > 0) ? n : (long)MF_DEV_DONE_TIMEOUT_MS;
    }
    return v;
}
// clk_sys = 98.4375 MHz (PLL outclk_0 / DDRAM_CLK). Fabric perf counters are in
// clk_sys cycles: C_DONE high word = perf_frame_cyc (total fabric-busy: new-submit
// detect → done write), C_STATUS high word = perf_pipe_cyc (composite pipeline busy).
// Reading them isolates fabric COMPUTE from the host-observed doorbell→done wait: if
// compute << wait, the 3-vsync latency is notice/DDR-visibility, not the fabric.
#define MF_CLK_SYS_MHZ 98.4375

static void mf_submit_stat(const struct timespec *t0, long iters, int timeout) {
    if (!mf_stat_on()) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    double us = (now.tv_sec - t0->tv_sec) * 1e6 + (now.tv_nsec - t0->tv_nsec) / 1e3;
    // Fabric per-state cycle counters (clk_sys), from the profiling RTL:
    //   frame   = C_DONE.hi   perf_frame_cyc   (total fabric-busy: detect->done)
    //   texwait = C_STATUS.hi perf_texwait_cyc (S_TRI_GOTTEX p0_ok stall — texel fetch)
    //   tri     = C_SRCSEL.hi perf_tri_cyc     (all S_TRI_* states)
    // Derived: dpath = tri-texwait (rasterizer datapath), ovhd = frame-tri (ring/clear/setup).
    double frame_ms = (double)mf_ctrl_rd_hi(MF_C_DONE)   / (MF_CLK_SYS_MHZ * 1000.0);
    g_seam_frame_ms = frame_ms;        // [Phase 4 Stage A] pair with this frame's block
    // Did the host really wait? `iters > 0` means the first C_DONE read did not match.
    // A TIMED-OUT await is excluded: it waited on the full abandoned budget, so its
    // interval measures the timeout, not the fabric's doorbell->done latency, and
    // folding it into `notice` would report one as the other.
    g_seam_blocked  = (iters > 0) && !timeout;
    double texw_ms  = (double)mf_ctrl_rd_hi(MF_C_STATUS) / (MF_CLK_SYS_MHZ * 1000.0);
    double tri_ms   = (double)mf_ctrl_rd_hi(MF_C_SRCSEL) / (MF_CLK_SYS_MHZ * 1000.0);
    // [Phase 1 A4] EXACT covered-pixel count from the fabric, replacing cov_px_est as the
    // cyc/px denominator. cov_px_est is a Sutherland-Hodgman clip estimate and is blind to
    // per-pixel depth and blend rejection, so it over-counts whatever the fabric discards.
    // Keep reporting the estimate alongside for one bring-up period: the two disagreeing is
    // the ANSWER to Phase 0's open question #1 (measured ~8.0 vs sim ~6-7 cyc/px), not a
    // discrepancy to suppress.
    double cov_exact = (double)mf_ctrl_rd_hi(MF_C_FLAGS);
    static unsigned n = 0, to = 0; static double sum = 0; static long it_sum = 0;
    static double fsum = 0, tsum = 0, xsum = 0, csum = 0, esum = 0;
    n++; to += timeout ? 1u : 0u; sum += us; it_sum += iters; fsum += frame_ms; tsum += tri_ms; xsum += texw_ms;
    esum += cov_exact;
    // [Phase 1 B2] Still correctly paired with the batch being measured after the deferral.
    // mf_device_await -- and therefore this function -- now runs at the TOP of the next
    // mf_frame_begin, but strictly before blt_begin_frame and before any draw of that frame
    // calls mf_cov_add_triangle. So the accumulator still holds the estimate for the batch
    // whose counters were just read. Moving the await below blt_begin_frame would silently
    // fold the wrong frame's estimate in.
    // [Phase 2 host lever] The await moved to the publish barrier, i.e. AFTER this frame's
    // draws have already accumulated into g_cov_px_accum. Folding the live accumulator here
    // would pair the counters of the batch we just waited on with the NEXT frame's estimate.
    // g_cov_px_published is the snapshot taken at publish, so the pairing survives the move.
    csum += g_cov_px_published;
    if (n % 30 == 0) {
        double f=fsum/30.0, t=tsum/30.0, x=xsum/30.0, c=csum/30.0, e=esum/30.0;
        double dpath_ms = t - x;
        // Screen area comes from the fabric geometry macros, never a literal:
        // MISTER_WIDTH/HEIGHT arrive as -D flags, so a hardcoded 288x216 here
        // would silently survive a geometry change and report a wrong overdraw.
        const double screen_px = (double)BLT_FB_WIDTH * (double)BLT_FB_HEIGHT;
        MfCovDerived d = mf_derive_cov(dpath_ms, e, c, screen_px, MF_CLK_SYS_MHZ);
        // cov_src labels the provenance of the WHOLE derived group -- overdraw and cyc_px
        // both come from d.cov_den, so a marker attached to only one of them would leave the
        // other silently reinterpreted. est_ratio prints n/a rather than 0.00 so the field
        // never carries a value outside its own invariant's domain (>= 1.0).
        char ratio_buf[16];
        if (d.est_ratio < 0.0) snprintf(ratio_buf, sizeof ratio_buf, "n/a");
        else                   snprintf(ratio_buf, sizeof ratio_buf, "%.2f", d.est_ratio);
        fprintf(stderr, "MFSUBMIT n=%u wait_ms[avg=%.2f] fabric_ms[frame=%.2f tri=%.2f "
                "texwait=%.2f dpath=%.2f ovhd=%.2f] cov_px=%.0f cov_px_est=%.0f "
                "est_ratio=%s overdraw=%.2f cyc_px=%.1f cov_src=%s spin_avg=%ld to=%u\n",
                n, (sum/30.0)/1e3, f, t, x, dpath_ms, f-t,
                e, c, ratio_buf, d.overdraw, d.cyc_px,
                d.estimated ? "est" : "exact", it_sum/30, to);
        sum = 0; it_sum = 0; to = 0;
        fsum = 0; tsum = 0; xsum = 0; csum = 0; esum = 0;
    }
}
#endif // MISTER_NATIVE_VIDEO — device transport internals end here

#ifndef MISTER_NATIVE_VIDEO
// [Phase 1 B1] Host-oracle shadow of the control block. mf_ctrl_rd is device-only, so
// without this the read-modify-write C_SRCSEL needs (preserve bits 15:8, change bit 2)
// could not run off-device at all -- and the throttle-preservation property would be
// untestable. Writes land here instead of in MMIO; reads come back from it.
static uint32_t g_ctrl_shadow[8] = {0};
static inline uint32_t mf_ctrl_rd(int qw) { return g_ctrl_shadow[qw & 7]; }
#endif

// [Phase 1 B2] Control-word write: traced in BOTH builds, stored only on device. Device
// behaviour is unchanged — same store, same order; the trace is a few array writes per
// frame alongside five uncached MMIO round trips.
static inline void mf_ctrl_wr(int qw, uint32_t v) {
#ifdef MISTER_NATIVE_VIDEO
    *(volatile uint32_t *)(g_dev_ctrl + (size_t)qw * 8u) = v;   // low u32 of each 8B qword
#else
    mf_trace_add(qw, v);   // oracle: record instead of store, so publish's order is checkable
    g_ctrl_shadow[qw & 7] = v;   // ...and remain readable, for read-modify-write registers
#endif
}

// [Phase 1 B2] The publish barrier AND its trace marker, deliberately one call. If these
// were two statements a mutation could move __sync_synchronize() while leaving the marker
// behind, and the trace would still claim a barrier that is no longer there. Moving this
// moves both, so the test sees the real ordering.
static inline void mf_ctrl_barrier(void) {
    mf_trace_add(MF_TRACE_BARRIER, 0);   // no-op on device; one call, so it cannot drift
    __sync_synchronize();
}

// ── SUBMIT SEAM (Phase 1 B2) ─────────────────────────────────────────────────
// The frame's handoff to the fabric, in three parts: mf_device_publish (control-block
// mirror, barrier, doorbell), mf_device_await (poll C_DONE), and mf_device_submit, which
// is just the two in order. Split so Task 4 can run the engine's Process() for frame N+1
// in between; see each function for its own contract.
//
// Common to both halves: the ring + heap are already resident in DDR (the emitter was
// bound to g_dev_ring/g_dev_src), so there is nothing to copy. This core's C_STATUS is
// OSD-mirror, not an error latch — completion == C_DONE match, failure == timeout.
//
// Both halves sit BEHIND mf_frame_end's g_frame_dropped guard: a dropped frame rebuilds
// no ring and must ring no doorbell (pinned by case_submit_publish_await_split).
//
// These three deliberately sit OUTSIDE #ifdef MISTER_NATIVE_VIDEO, with only the
// MMIO and the poll loop guarded, because the host oracle has to be able to observe
// the seam: the raster-backend-test target does not define MISTER_NATIVE_VIDEO, so
// device-only publish/await counters would be permanently zero there and the
// pipelining this split exists to enable could not be tested without hardware.
// On the oracle the two functions reduce to the counter bump and the timestamp.

// [Phase 1 B2] Counters for the host-oracle test that publish and await are separable.
static uint32_t g_publish_count = 0;
static uint32_t g_await_count   = 0;
// Timestamp of the publish we are waiting on, so mf_submit_stat still measures the
// full doorbell->C_DONE interval even when the await happens a frame later.
static struct timespec g_publish_t0;

// [Phase 1 B2] ORDERING WITNESS. Counting publishes and awaits pins how many times and on
// which frames the seam runs, but NOT the order: on the host oracle both halves are
// near-no-ops (the MMIO and the poll compile out), so inverting them is behaviourally
// invisible there and every counter still lands on its expected value. Measured: with
// mf_device_submit's two calls swapped, all 69 host cases passed. This flag makes the
// ordering itself observable, which is the only way to catch it without hardware.
//
// On device the inversion is not cosmetic: await would poll C_DONE for a sequence that was
// never submitted, and mf_submit_stat would measure from a stale g_publish_t0.
//
// Lifetime: publish INCREMENTS it; it is cleared where mf_frame_begin clears
// g_fabric_pending, which is the single point at which the batch stops being in flight —
// acked by the await, found done by the cheap re-check during a drop run, or abandoned by a
// reclaim. A timed-out batch is deliberately still counted as outstanding (that is the whole
// premise of the in-flight guard below), so the drop run that follows does not look
// unpaired. Resolving it anywhere else would let the three exit paths disagree.
//
// A DEPTH rather than a bool, because a bool is idempotent: publish(); publish(); await()
// leaves a bool looking perfectly paired even though the doorbell was rung twice for one
// frame. Depth must never exceed 1, and must return to 0.
static int32_t  g_publish_depth        = 0;
static uint32_t g_unpaired_awaits      = 0;   // awaits with nothing in flight: must stay 0
static uint32_t g_seam_depth_violations = 0;  // publish while one was already in flight

// [Phase 1 B2] Publish the emitter's control-block mirror and ring the doorbell.
// Does NOT poll. Split out of the old mf_device_submit so the caller can run
// Process() for the next frame between the doorbell and the wait.
// [Phase 2 host lever] Which arena the in-flight batch is reading, so mf_frame_begin can
// tell "fabric is busy on the other arena" (normal overlap) from "fabric is busy on the
// arena I am about to rewind" (the wedge).
static uint32_t g_pending_arena = 0;

// [Phase 2 host lever] THE barrier, moved here from mf_frame_begin. Defined below, after
// mf_device_await and mf_fabric_still_busy, which it needs.
static bool mf_publish_barrier(void);

static void mf_device_publish(void) {
    // Body is NOT #ifdef'd: mf_ctrl_wr traces in both builds and stores only on device, so
    // the write order below is checkable on the host oracle. See the trace shim above.
    mf_trace_reset();                           // each publish's trace stands alone
    // [Phase 2 host lever] Snapshot the covered-pixel ESTIMATE for the batch being
    // published, and reset the accumulator for the frame that starts building next. The
    // await now runs before this line, so mf_submit_stat can no longer read the live
    // accumulator and get the right frame. Exact cov_px still comes from the fabric.
    g_cov_px_published = g_cov_px_accum;
    g_cov_px_accum     = 0.0;
    mf_ctrl_wr(MF_C_CMDCOUNT, (uint32_t)g_e.cmd_count);
    mf_ctrl_wr(MF_C_TARGET,   (uint32_t)g_e.target_buf);
    mf_ctrl_wr(MF_C_CLEAR,    (uint32_t)g_e.clear_color);
    mf_ctrl_wr(MF_C_FLAGS,    (uint32_t)g_e.flags);
    // [Phase 1 B1] Tell the fabric which ring this frame is in. C_SRCSEL bit 2
    // (C_RINGSEL_BIT); bits 0 and 1 are dead (source is hardwired to SDRAM,
    // comp_pipeline is the sole renderer) and bits 15:8 are the f2h write throttle,
    // which must be preserved -- hence read-modify-write rather than a plain store.
    {
        uint32_t srcsel = mf_ctrl_rd(MF_C_SRCSEL);
        srcsel = (srcsel & ~(1u << 2)) | ((g_arena & 1u) << 2);
        mf_ctrl_wr(MF_C_SRCSEL, srcsel);
    }
    mf_ctrl_barrier();                          // data before doorbell (traced)
    mf_ctrl_wr(MF_C_SUBMIT, g_e.submit_seq);    // doorbell LAST
    g_last_published_seq = g_e.submit_seq;      // [Phase 2 host lever] seam witness
    clock_gettime(CLOCK_MONOTONIC, &g_publish_t0);
    // [Phase 4 Stage A] Close the previous frame's seam sample. g_publish_t0 is
    // this frame's doorbell, which is the previous sample's closing edge.
    if (g_seam_have_prev) {
        mf_seam_add(&g_seam,
                    mf_seam_ms(&g_seam_prev_db, &g_seam_be),   // host
                    mf_seam_ms(&g_seam_be,      &g_seam_ar),   // block
                    mf_seam_ms(&g_seam_ar,      &g_publish_t0),// pub
                    mf_seam_ms(&g_seam_prev_db, &g_publish_t0),// period
                    g_seam_frame_ms,
                    g_seam_blocked ? 1 : 0);
#ifdef MISTER_NATIVE_VIDEO
        if (mf_stat_on() && mf_seam_ready(&g_seam)) {
            mf_seam_out_t o; mf_seam_derive(&g_seam, &o);
            fprintf(stderr,
                    "MFSEAM n=%u period=%.2f host=%.2f block=%.2f pub=%.2f "
                    "notice=%.2f blocked=%.0f%% suspect=%u incomplete=%u "
                    "host_hist=%u/%u/%u/%u/%u/%u/%u/%u "
                    "pub_hist=%u/%u/%u/%u/%u/%u/%u/%u\n",
                    g_seam.n, o.period_ms, o.host_ms, o.block_ms, o.pub_ms,
                    o.notice_ms, 100.0 * o.blocked_frac, o.suspect, g_seam_incomplete,
                    g_seam.host_hist[0], g_seam.host_hist[1], g_seam.host_hist[2],
                    g_seam.host_hist[3], g_seam.host_hist[4], g_seam.host_hist[5],
                    g_seam.host_hist[6], g_seam.host_hist[7],
                    g_seam.pub_hist[0], g_seam.pub_hist[1], g_seam.pub_hist[2],
                    g_seam.pub_hist[3], g_seam.pub_hist[4], g_seam.pub_hist[5],
                    g_seam.pub_hist[6], g_seam.pub_hist[7]);
            mf_seam_reset(&g_seam);
            g_seam_incomplete = 0;   // [Finding 1] windowed like g_seam itself
        }
#endif
    }
    g_seam_prev_db   = g_publish_t0;
    g_seam_have_prev = true;
    g_publish_count++;
    // Depth, not a bool: a bool is idempotent, so publish(); publish(); await() would look
    // perfectly paired while the doorbell had been rung twice for one frame — on device a
    // re-submit of a sequence that is still in flight. Depth must never exceed 1.
    if (++g_publish_depth > 1) g_seam_depth_violations++;
}

// [Phase 1 B2] Block until the fabric acks the published sequence. This is the old
// mf_device_submit's tail, moved verbatim: the poll loop, the backoff, the timeout
// budget, and the g_fabric_pending / g_pending_seq bookkeeping the in-flight-batch
// guard reads are all unchanged.
static void mf_device_await(void) {
    g_await_count++;
    // [Phase 1 B2] Ordering witness: awaiting with nothing in flight means publish and
    // await ran out of order, or a publish was lost. See g_publish_depth.
    if (g_publish_depth <= 0) g_unpaired_awaits++;
    // [Phase 2 host lever] Witness: WHICH sequence this await polls for. It must be the
    // batch that was published (g_pending_seq), never the emitter's live g_e.submit_seq.
    // Those were the same value while the await sat in mf_frame_begin -- blt_end_frame had
    // not yet bumped submit_seq for the new frame. At the publish barrier they differ by
    // one, so polling submit_seq waits for a batch that only exists AFTER the publish this
    // await is gating: every await then burns the full 200 ms timeout, and the timeout path
    // used to write that unpublished seq into g_pending_seq, wedging mf_fabric_still_busy()
    // true forever. Device-measured .62 2026-07-29: to=30 of 30, wait_ms avg 205, period
    // 535 ms (1.9 fps) against a fabric reporting 19.29 ms.
    // `want` is used BOTH by the witness and by the poll below, so the witness cannot
    // drift away from what is actually being waited for.
    const uint32_t want = g_pending_seq;
    g_last_awaited_seq  = want;
#ifdef MISTER_NATIVE_VIDEO
    struct timespec t0 = g_publish_t0;
    if (mf_nowait_on()) { mf_submit_stat(&t0, 0, /*timeout=*/0); return; }  // probe: no poll
    long iters = 0;
    const long poll_us = mf_poll_us();
    for (;;) {
        if (mf_ctrl_rd(MF_C_DONE) == want) {   // fabric consumed the PUBLISHED batch
            mf_submit_stat(&t0, iters, /*timeout=*/0);
            return;
        }
        iters++;
        // Back off instead of spinning. A pure spin issued 24k-43k UNCACHED C_DONE reads
        // per frame (measured on device) for the whole ~30ms the fabric was working —
        // roughly a million a second, all of them full round trips through the same DDR
        // controller the fabric fetches commands, vertices and textures through. The CPU
        // freed is not the point (this path is blocking either way): the point is handing
        // that memory bandwidth back to the fabric. Sleeping costs at most poll_us of
        // extra latency on a ~30ms frame. poll_us=0 restores the old pure spin for A/B.
        if (poll_us > 0 && iters > MF_POLL_SPIN_ITERS) {
            struct timespec ts = { 0, poll_us * 1000L };
            nanosleep(&ts, NULL);
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
        if (ms >= mf_timeout_ms()) {
            fprintf(stderr, "backend_mfgpu: fabric submit timeout (pending=%u emitter=%u done=%u "
                    "status=%u waited=%ldms)\n", want, g_e.submit_seq,
                    mf_ctrl_rd(MF_C_DONE), mf_ctrl_rd(MF_C_STATUS), ms);
            mf_submit_stat(&t0, iters, /*timeout=*/1);
            // [in-flight-batch guard] We are abandoning the wait, NOT the batch: the fabric
            // is still reading this ring, so the next frames must drop whole rather than
            // rebuilding it underneath. g_pending_seq already names that batch and is left
            // ALONE -- overwriting it with the emitter's live seq (which at the publish
            // barrier is the frame this await is gating, one ahead) is what made
            // mf_fabric_still_busy() true forever.
            g_fabric_pending = true;
            // g_publish_depth is NOT decremented on purpose: the batch is still in flight,
            // so a later re-await of it (Task 4 awaits from mf_frame_begin) stays paired.
            return;
        }
    }
#endif // MISTER_NATIVE_VIDEO
    // Host oracle: nothing to poll. Resolution (and the depth clear) happens in
    // mf_frame_begin where g_fabric_pending is cleared, identically to the device path.
}

static void mf_init_once(void) {
    if (g_inited) return;
#ifdef MISTER_NATIVE_VIDEO
    if (mf_ddr_map()) {
        // In-place: the emitter builds ring + heap directly in the mmap'd DDR.
        blt_emitter_init(&g_e, g_dev_ring, MF_DEV_RING_CAP, g_dev_src, MF_DEV_SRC_CAP);
        blt_alloc_init(&g_e.alloc, MF_VTX_REGION, MF_DEV_SRC_CAP - MF_VTX_REGION);
        blt_vtx_buf_init(&g_e, g_dev_src, MF_VTX_HALF);
    } else {
        // No /dev/mem: bind host RAM so the emitter never faults; g_dev_ok is false
        // so mf_frame_end drops (never submits to a null map).
        blt_emitter_init(&g_e, g_ring[0], MF_RING_CAP, g_srcdram, sizeof g_srcdram);
        blt_alloc_init(&g_e.alloc, MF_VTX_REGION, MF_TEX_HEAP);
        blt_vtx_buf_init(&g_e, g_srcdram, MF_VTX_HALF);
    }
#else
    blt_emitter_init(&g_e, g_ring[0], MF_RING_CAP, g_srcdram, sizeof g_srcdram);
    uint32_t cap = g_tex_heap_cap ? g_tex_heap_cap : MF_TEX_HEAP;
    blt_alloc_init(&g_e.alloc, MF_VTX_REGION, cap);       // texture allocator (persistent)
    blt_vtx_buf_init(&g_e, g_srcdram, MF_VTX_HALF);       // per-frame vertex buffer
#endif
    for (int i = 0; i < MF_TEX_CACHE_N; i++) g_texcache[i].used = false;
    g_lru_clock = 0; g_upload_count = 0; g_stage_count = 0; g_lru_frame_floor = 0;
    g_evict_attempts = 0; g_cachefull_drops = 0;   // [Phase 1 B3] pressure + refusal witnesses
    g_frame_ovf_cause = MF_OVF_UNKNOWN;            // [Phase 1 B3] per-frame overflow cause
    g_lru_evict_floor = 0;                         // [Phase 1 B3] lagging floor
    g_publish_count = 0; g_await_count = 0;   // [Phase 1 B2] submit-seam counters
    g_arena = 0;                                                              // [Phase 1 B1] arena parity
    g_publish_depth = 0; g_unpaired_awaits = 0; g_seam_depth_violations = 0;  // [Phase 1 B2] ordering witness
    g_last_published_seq = 0; g_last_awaited_seq = 0;   // [Phase 2 host lever] seam witnesses
    g_ctrl_trace_n = 0; g_ctrl_trace_overflow = 0;                             // [Phase 1 B2] control-write trace
    g_fabric_pending = false; g_frame_dropped = false; g_drop_count = 0; g_drop_run = 0;
    g_last_draw.valid = false; g_dup_skipped = 0;
    g_appsurf_presented = false; g_crt_stripped = 0;
    // [Phase 4 Stage B] host-test determinism: RasterBackend_MFGPU_TestReinit is
    // the file's one "start clean" primitive, and every case uses it as such.
    mf_pc_reset(&g_pc);
    g_pc_dropped_total = 0; g_pc_emitted_total = 0;
    g_inited = true;
}

// [in-flight-batch guard] The emitter builds the command ring, the vertex buffer and the
// texture heap IN PLACE in the DDR the fabric reads (mf_init_once binds g_dev_ring/g_dev_src).
// The ONLY thing keeping the host from overwriting a batch the fabric is still executing is
// the C_DONE await -- and its timeout abandons exactly that guarantee. [Phase 1 B2] That
// await is no longer at the end of the frame that published: it now runs at the top of the
// NEXT mf_frame_begin, so the host builds frame N+1's arena while the fabric still reads N's.
// What keeps that safe is not the poll's position but the double-buffering underneath it --
// ring and vertex arenas alternate (B1) and textures stay pinned for two frames (B3), so the
// batch the fabric is reading is never the one being rebuilt. Device-measured 2026-07-24: with the stock 200ms budget a single stalled frame
// produced ~78 timeouts, i.e. 78 successive frames that called blt_begin_frame (cmd_count=0,
// vtx_used=0) and re-staged textures straight over the batch the fabric was mid-way through
// reading. That is the mechanism behind the garbage geometry, "the game's stored tilemap
// rendered directly", and the escalation of isolated hiccups into 15-second cascades.
// (It is an AMPLIFIER, not the initiator: raising the budget to 5s left the underlying
// ~18s fabric stalls intact, so the fabric-side bug is tracked separately.)
// So: once a submit times out, drop WHOLE frames -- emit nothing, stage nothing, reset no
// cursors -- until the fabric acks the sequence it is still working on.
// Is the fabric still chewing on g_pending_seq? Only meaningful while g_fabric_pending.
static bool mf_fabric_still_busy(void) {
    if (g_test_fabric_busy >= 0) return g_test_fabric_busy != 0;
#ifdef MISTER_NATIVE_VIDEO
    if (g_dev_ok) return mf_ctrl_rd(MF_C_DONE) != g_pending_seq;
#endif
    return false;   // host oracle: blt_execute is synchronous, nothing is ever in flight
}

// [Phase 2 host lever] Everything in mf_device_publish rewrites the SINGLE shared control
// block (C_CMDCOUNT/C_TARGET/C_CLEAR/C_FLAGS/C_SRCSEL), and the fabric latches those in its
// prologue right after detecting a new C_SUBMIT (blitter_top.sv S_GOT_SRCSEL). So the
// previous batch must be acked before the first of those stores -- and NOT one line earlier
// than that, which is the whole point of this lever: draw emission, present and capture now
// run while the fabric is still working on the previous batch.
// Returns false if the batch could not be resolved, in which case the caller must NOT
// publish; stomping a live control block is the corruption cascade documented at the
// in-flight-batch guard in mf_frame_begin.
// THE resolution point for a published batch, and the ONLY one (the reclaim in
// mf_frame_begin aside). Keeping it single is what stops the exit paths from disagreeing
// about g_fabric_pending / g_publish_depth -- the property the old frame-start await had.
// [Phase 2 host lever] One drop, counted once, wherever it is decided. The wedge is now
// observable from TWO places -- mf_frame_begin (about to rewind the arena the fabric is
// reading) and mf_publish_barrier (about to rewrite the control block) -- and a wedge
// settles into dropping at the FIRST of them, which is why the drop-run counter and the
// reclaim cannot live in the barrier alone: during a real wedge the barrier is never
// reached again, the run never grows, the limit never trips, and the guard deadlocks
// forever. case_inflight_drop_limit pins exactly that.
// Returns true if the batch was RECLAIMED (abandoned; caller proceeds and rebuilds),
// false if the caller must drop this frame whole.
static bool mf_drop_or_reclaim(void) {
    // Give up rather than deadlock, exactly as the old frame-start guard did: a fabric
    // that NEVER acks (device-observed 2026-07-24: permanent frame-1 wedge, sub=1
    // done=0) would otherwise drop every frame forever and the game would never render.
    // After MF_DROP_LIMIT consecutive failures, abandon the batch: one deliberate stomp
    // as a last resort instead of one per frame.
    if (++g_drop_run >= mf_drop_limit()) {
        fprintf(stderr, "backend_mfgpu: fabric never acked seq=%u after %u frames - "
                "reclaiming the ring\n", g_pending_seq, mf_drop_limit());
        g_fabric_pending = false;
        g_publish_depth  = 0;
        // Reset here, not only on the barrier's success path: after a reclaim the next
        // publish finds g_fabric_pending false and returns from the barrier's first line,
        // so a run left at the limit would make the NEXT wedge reclaim on its first frame.
        g_drop_run       = 0;
        return true;
    }
    if ((g_drop_count++ % 60u) == 0u)
        fprintf(stderr, "backend_mfgpu: fabric still on seq=%u - frame dropped "
                "(ring left intact; %u dropped)\n", g_pending_seq, g_drop_count);
    return false;
}

static bool mf_publish_barrier(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_seam_be);
    g_seam_blocked = false;   // a frame that skips the await never blocked
    if (!g_fabric_pending) {
        // [Finding 1] Reachable right after mf_frame_begin's own mf_drop_or_reclaim()
        // already resolved the batch (see the in-flight-batch guard there): this barrier
        // does no work and g_seam_ar is NOT re-stamped. If mf_device_publish (called next,
        // unconditionally, by the caller) were to close a sample here, it would pair a
        // fresh g_seam_be (just stamped above) against whatever g_seam_ar the LAST
        // successful barrier left behind -- stale by however long the wedge ran. Skip
        // that sample instead of manufacturing a corrupt one; only counts as skipped if a
        // sample was actually pending (have_prev already false means nothing to lose, e.g.
        // the very first frame after reinit).
        if (g_seam_have_prev) g_seam_incomplete++;
        g_seam_have_prev = false;
        return true;
    }
    // Once per PUBLISHED BATCH, not once per publish attempt. drop_run==0 means this is the
    // first attempt against this batch. During a drop run the cheap non-blocking read below
    // is what discovers the fabric finishing; re-entering the blocking poll every attempt
    // would add a full timeout budget (200 ms) per dropped frame -- ~12 s across a 60-frame
    // drop run, turning a wedge into a far longer freeze than before pipelining.
    //
    // [Finding 3] Skipping the await also leaves g_seam_frame_ms/g_seam_blocked exactly as
    // mf_submit_stat last set them -- for the batch whose read TIMED OUT, not this one. That
    // staleness is harmless ONLY because g_seam_blocked was just reset to false above and
    // mf_submit_stat does not run again on this path to re-set it true, so a sample closed
    // from this frame (see mf_device_publish) can never fold the stale frame_ms into
    // notice_sum (mf_seam_add guards notice_sum on `blocked`). Load-bearing and easy to
    // break: any future change that calls mf_submit_stat or sets g_seam_blocked=true on a
    // skipped-await path would silently attribute an old batch's fabric time to this one.
    if (g_drop_run == 0) mf_device_await();
    if (mf_fabric_still_busy()) {
        bool reclaimed = mf_drop_or_reclaim();
        if (reclaimed) {
            // [Finding 1] The ring was abandoned after MF_DROP_LIMIT consecutive drops.
            // Same corruption as the !g_fabric_pending path above: g_seam_ar was never
            // re-stamped across the whole drop run, so the sample mf_device_publish would
            // otherwise close pairs a fresh g_seam_be against a g_seam_ar left over from
            // the last successful barrier -- an entire drop run earlier (device-observed:
            // ~60 frames x ~16.7 ms folded into one sample). Skip it.
            //
            // Coverage note: this branch is NOT exercised by
            // case_seam_reclaim_no_corrupt_sample. That test's forced-busy walk always
            // reclaims one level up, inside mf_frame_begin's own drop_or_reclaim call --
            // a wedge settles at the FIRST place that observes it (see the comment above
            // mf_drop_or_reclaim), which clears g_fabric_pending before the barrier is
            // reached again, so the barrier takes its !g_fabric_pending early-out instead
            // of ever calling mf_fabric_still_busy() here. Mutation-verified: forcing this
            // `if (reclaimed)` to never trigger leaves the whole suite green. The guard is
            // still correct -- the corruption it prevents is real -- it is just harder to
            // reach than the early-out above under the harness's default drop limit.
            if (g_seam_have_prev) g_seam_incomplete++;
            g_seam_have_prev = false;
        }
        // A false return (still dropping, not yet reclaimed) leaves g_seam_have_prev alone:
        // the caller does not call mf_device_publish this frame at all (no doorbell rung),
        // so no sample closes and there is nothing to skip.
        return reclaimed;
    }
    g_drop_run       = 0;
    g_fabric_pending = false;
    g_publish_depth  = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_seam_ar);
    return true;
}

static void mf_frame_begin(void) {
    mf_init_once();
    // [in-flight-batch guard] Decide BEFORE blt_begin_frame: it is the call that would
    // rewind the cursors over a live batch.
    if (g_fabric_pending) {
        // [Phase 2 host lever] The blocking await MOVED OUT of here, to mf_publish_barrier.
        // The barrier exists to protect the SINGLE shared control block
        // (C_CMDCOUNT/C_TARGET/C_CLEAR/C_FLAGS/C_SRCSEL), and every write to that block
        // happens in mf_device_publish -- nothing between here and the publish touches it.
        // Waiting here instead cost the whole frame: measured on .62 2026-07-29, period =
        // fabric 19.30ms + ~8ms of post-await host work (draw emission, present, capture)
        // that had no reason to be serialized. See
        // mister-gmloader/docs/superpowers/findings/2026-07-29-phase2-baseline.md.
        //
        // At frame start the fabric may legitimately still be reading the PREVIOUS batch --
        // that is exactly the overlap this lever buys -- and rebuilding is safe because the
        // arena flip below hands this frame the OTHER ring/vertex window (B1), textures are
        // pinned two frames (B3), and the batch that used THIS arena was already awaited by
        // the barrier one publish ago. So a busy fabric here is not a reason to drop.
        //
        // The ONE case that must still stop here: we are about to flip into (and rewind)
        // the very arena the fabric is still reading. With two arenas that means the fabric
        // is a whole frame behind, because in steady state the barrier resolved this arena's
        // batch one publish ago. Checked with the CHEAP non-blocking read; the BLOCKING wait
        // stays in mf_publish_barrier, which is the only place that needs it.
        //
        // A sustained wedge settles here, not at the barrier: frame N+1 builds (its arena is
        // free) and dies at the barrier, then N+2 onward all want N's arena and die right
        // here. So this path must do the drop-run accounting and the reclaim too, otherwise
        // the run freezes at 1 and the limit never trips. Reclaiming falls through and
        // rebuilds -- one deliberate stomp, the same last resort the barrier takes.
        if ((g_arena ^ 1u) == (g_pending_arena & 1u) && mf_fabric_still_busy()
            && !mf_drop_or_reclaim()) {
            g_frame_dropped = true;
            g_frame_active  = true;   // so present() still closes the frame
            return;
        }
        // Otherwise the batch stays pending ON PURPOSE: mf_publish_barrier is the single
        // resolution point, and clearing the flag here would make it skip its await and
        // stomp a live control block.
    }
    g_frame_dropped = false;
    g_frame_ovf_cause = MF_OVF_UNKNOWN;   // [Phase 1 B3] per-frame: reset before staging
    g_last_draw.valid = false;   // [duplicate-draw elimination] never span frames
    g_appsurf_presented = false; // [strip CRT] the present must re-land every frame
    // Snapshot the pin floor for this frame: any g_texcache entry touched
    // (hit or staged) from here through mf_frame_end gets .lru > this value
    // and is thus protected from eviction until the NEXT frame begins (see
    // g_lru_frame_floor and evict_one_lru).
    // [Phase 1 B3] Retire this frame's floor to the eviction floor BEFORE taking the new
    // snapshot, so entries touched in the previous frame stay pinned through this one.
    g_lru_evict_floor = g_lru_frame_floor;
    g_lru_frame_floor = g_lru_clock;
    // [Phase 1 B1] Flip to the other arena before rebuilding. The fabric is (or will
    // shortly be) reading the previous frame's arena, so this frame must not touch it.
    // Rebinding here — not at init — is what makes the alternation take effect, since
    // blt_begin_frame rewinds cursors into whatever buffers the emitter currently holds.
    //
    // The ring pointer is rebound DIRECTLY rather than via blt_emitter_init, which the plan
    // text suggested. blt_emitter_init memsets the whole emitter and re-runs
    // blt_alloc_init(&e->alloc, 0, heap_cap). Per frame that would zero submit_seq (so the
    // doorbell never advances — a permanent device wedge), wipe sdram_alloc / sdram_perm /
    // sdram_src (the resident atlas), and reset the texture allocator to base 0, overlapping
    // the vertex region, while g_texcache still believes those pages are live — handing out
    // heap the cache thinks it owns. That is the same silent aliasing this plan exists to
    // prevent. Only ring / ring_cap / the vertex window may move per frame.
    g_arena ^= 1u;
#ifdef MISTER_NATIVE_VIDEO
    if (g_dev_ok) {
        g_e.ring     = g_arena ? g_dev_ring_b : g_dev_ring;
        g_e.ring_cap = MF_DEV_RING_CAP;
        blt_vtx_buf_init(&g_e, g_dev_src + (g_arena ? MF_VTX_HALF : 0), MF_VTX_HALF);
    } else
#endif
    {
        g_e.ring     = g_ring[g_arena & 1u];
        g_e.ring_cap = MF_RING_CAP;
        blt_vtx_buf_init(&g_e, g_srcdram + (g_arena ? MF_VTX_HALF : 0), MF_VTX_HALF);
    }
    // Textures persist across frames now (cache in g_texcache). Only the vtx
    // cursor + command list reset here (blt_begin_frame). No blt_heap_reset.
    blt_begin_frame(&g_e, /*target_buf=*/0, /*clear=*/0, /*clear_color=*/0);
    // [app-surface render target, step 1] Every fresh ring starts targeting
    // WORK (blt_execute's own default, matched by the RTL) -- see g_cur_target's
    // comment for why this reset (not just the variable's initial value) matters.
    g_cur_target = MF_TARGET_WORK;
    // [Phase 4 Stage B] Fold last frame's counts into the run totals BEFORE
    // resetting g_pc: g_pc itself only ever holds the current frame's tally.
    g_pc_dropped_total += g_pc.dropped;
    g_pc_emitted_total += g_pc.emitted;
    // A deferred fill must never span frames -- the ring it would have gone
    // into has just been rewound.
    mf_pc_reset(&g_pc);
    g_frame_active = true;
    g_frame_no++;
}

// Task 7: production entry point for clear()/draw() — the frame loop never
// calls frame_begin() directly, so the first clear/draw of a frame opens it.
static void mf_ensure_frame(void) {
    if (!g_frame_active) mf_frame_begin();
}

// [app-surface render target, step 1] Emit a BLT_OP_SET_TARGET iff `d`'s
// target differs from the emitter's current one (g_cur_target), so back-to-
// back ops on the same buffer don't each redundantly re-select it. Shared by
// mf_clear and mf_draw so BOTH ops are target-aware -- the real per-frame
// order (Task 1) clears/draws WORK *after* the scene has already rendered
// into the app surface (SET_TARGET APPSURF), so a clear()-only target switch
// is just as load-bearing as draw()'s.
static void mf_select_target(uint32_t fbo) {
    bool is_appsurf = (g_appSurfFbo != 0) && (fbo == g_appSurfFbo);
    int want = is_appsurf ? MF_TARGET_APPSURF : MF_TARGET_WORK;
    if (want != g_cur_target) { blt_set_target(&g_e, want); g_cur_target = want; }
}

static void mf_clear(RSurface *d, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mf_ensure_frame();
    if (g_frame_dropped) return;   // [in-flight-batch guard] emit nothing this frame
    g_last_draw.valid = false;     // [duplicate-draw elimination] a clear breaks the run
    mf_select_target(d->fbo);
    (void)a;   // fabric FILL writes opaque RGB565; no alpha channel on the wire
    int w = d->w < BLT_FB_WIDTH  ? d->w : BLT_FB_WIDTH;
    int h = d->h < BLT_FB_HEIGHT ? d->h : BLT_FB_HEIGHT;
    uint16_t col = mf_rgb565(r, g, b);
    // [Phase 4 Stage B] Only a FULL-EXTENT clear is deferrable. A partial clear
    // is not the redundant full-screen fill this lever targets, and deferring it
    // would introduce a superseding rule (partial-then-full vs full-then-partial)
    // that buys nothing.
    if (mf_defer_clear_on() && w == BLT_FB_WIDTH && h == BLT_FB_HEIGHT) {
        mf_pc_record(&g_pc, mf_pc_slot_of(g_cur_target), w, h, col);
        return;
    }
    blt_fill(&g_e, 0, 0, w, h, col);
}

static bool evict_one_lru(void) {
    g_evict_attempts++;   // [Phase 1 B3] pressure witness: called, not necessarily succeeded
    // Pin-for-TWO-frames invariant: an entry with .lru > g_lru_evict_floor was hit or
    // staged during the current frame OR the previous one.
    //   - current frame: a blt_trilist command already emitted this frame may reference its
    //     heap offset, and that command isn't consumed until blt_execute runs at frame end.
    //     Freeing it now would let a later stage_texture's retry reuse the same offset with
    //     different pixels, so the earlier (already-emitted) draw samples the wrong texture.
    //   - previous frame [Phase 1 B3]: with the ring double-buffered the fabric is still
    //     reading that frame's batch while the host builds this one, so its textures are
    //     live in hardware even though the frame is "over".
    // Only entries older than BOTH (.lru <= g_lru_evict_floor) are eviction-eligible.
    int victim = -1; uint64_t best = ~0ull;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (g_texcache[i].used && g_texcache[i].lru <= g_lru_evict_floor && g_texcache[i].lru < best) {
            best = g_texcache[i].lru; victim = i;
        }
    }
    if (victim < 0) return false;   // nothing unpinned to evict
    blt_emitter_free(&g_e, g_texcache[victim].ref.off, g_texcache[victim].ref.size);
    g_texcache[victim].used = false;
    return true;
}

// Stage a texture page keyed by identity. Cache hit reuses the resident surface
// (no re-upload); miss converts RGBA->RGB565 into g_texscratch and blt_uploads
// once. Returns a ref with .valid==0 if the page cannot fit even after eviction
// (caller drops the draw). *out_has_key := did any texel fold to the colorkey.
// [Task 9 bring-up diagnostic] GMLOADER_MFGPU_HEAPLOG=1: log every real texture
// upload (miss) with its size and the resulting heap occupancy, every stage
// failure with the full pinned-working-set breakdown, and a per-frame summary
// of exactly which textures were touched (pinned) this frame -- to measure the
// STEADY-STATE per-frame texture working set against the device's ~14.75MB
// SRC heap (MF_DEV_SRC_CAP), since the host's 32MB MF_TEX_HEAP masked this
// capacity gap in every host-side parity test. Zero cost when unset.
static int mf_heaplog_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFGPU_HEAPLOG"); v = (e && *e) ? 1 : 0; }
    return v;
}
// Sum bytes + count of g_texcache entries "pinned" this frame (lru > floor --
// see g_lru_frame_floor's comment): the actual working set blt_execute must
// hold resident SIMULTANEOUSLY to render the frame, not just cumulative
// uploads. Also prints each entry when `verbose`.
static void mf_heaplog_frame_set(const char *tag, int verbose) {
    if (!mf_heaplog_on()) return;
    uint32_t total = 0; int n = 0;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (!g_texcache[i].used || g_texcache[i].lru <= g_lru_frame_floor) continue;
        total += g_texcache[i].ref.size; n++;
        if (verbose)
            fprintf(stderr, "HEAPLOG   pinned key=%u %ux%u bytes=%u off=%u\n",
                    g_texcache[i].key, g_texcache[i].ref.w, g_texcache[i].ref.h,
                    g_texcache[i].ref.size, g_texcache[i].ref.off);
    }
    fprintf(stderr, "HEAPLOG %s: %d distinct textures pinned this frame, %u bytes "
            "(%.2fMB) | heap_used=%u/%u (%.2fMB/%.2fMB)\n",
            tag, n, total, total / (1024.0*1024.0),
            blt_alloc_used(&g_e.alloc), g_e.alloc.size,
            blt_alloc_used(&g_e.alloc) / (1024.0*1024.0), g_e.alloc.size / (1024.0*1024.0));
}

// Shared tail for stage_texture / stage_texture_region: g_texscratch already
// holds `w*h` RGB565 texels for the page keyed (key, rx,ry,w,h). Upload it
// (evicting prior-frame pages under heap pressure), STAGE it DDR3->SDRAM, and
// insert a cache entry. Returns the ref (.valid==0 if it couldn't fit even after
// eviction -- the last failed blt_upload leaves g_e.overflow set so the frame
// drops, which is correct). `what` tags the HEAPLOG lines.
// ── [Y-orientation bring-up capture] GMLOADER_MFGPU_TEXDUMP ──────────────────
// Writes the EXACT RGB565 texels about to be uploaded+staged, in the exact row
// order they are written to the heap, as a P6 PPM under /tmp/mfgpu-tex/.
//
// This is the experiment that separates the two remaining candidate layers for
// the residual per-sprite V inversion (glyphs mirrored in place on device while
// the whole-frame orientation is correct):
//   dump reads UPRIGHT  -> the page IS staged top-origin, exactly as the SW
//                          oracle and the RTL texel formula both assume, so the
//                          mirror is introduced DOWNSTREAM (staging/scanout) and
//                          an emitter-side V-flip would only mask it.
//   dump reads MIRRORED -> the source rows really do arrive bottom-origin, the
//                          emitter is the right layer, and the fix is a
//                          band-local (per-triangle) flip -- NOT ac41e1e's
//                          page-normalized 1-v, which relocates atlas samples.
// Row 0 of the PPM is row 0 of the staged page by construction (no reordering
// here), so the file's own top IS the page's top -- that is what makes the
// comparison against the screen meaningful.
static int mf_texdump_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFGPU_TEXDUMP"); v = (e && *e) ? atoi(e) : 0;
                 if (e && *e && v <= 0) v = 40; }
    return v;
}
static void mf_texdump(uint32_t key, int w, int h, int rx, int ry, const char *what) {
    static int dumped = 0;
    if (dumped >= mf_texdump_on()) return;
    if (w <= 0 || h <= 0) return;
    static bool mkdir_done = false;
    if (!mkdir_done) { system("mkdir -p /tmp/mfgpu-tex"); mkdir_done = true; }
    char path[256];
    snprintf(path, sizeof path, "/tmp/mfgpu-tex/%03d_%s_key%u_r%d,%d_%dx%d.ppm",
             dumped, what, key, rx, ry, w, h);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)                    // row 0 first == page row 0
        for (int x = 0; x < w; x++) {
            uint16_t p = g_texscratch[(size_t)y * w + x];
            uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 0x1F) << 3),
                               (uint8_t)(((p >>  5) & 0x3F) << 2),
                               (uint8_t)(( p        & 0x1F) << 3) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "TEXDUMP %s\n", path);
    dumped++;
}

static blt_surface_ref_t mf_upload_and_cache(uint32_t key, int w, int h,
                                             int rx, int ry, bool has_key, bool mask_only,
                                             const char *what) {
    if (mf_texdump_on()) mf_texdump(key, w, h, rx, ry, what);
    bool ov_before = g_e.overflow;                    // preserve any overflow already set this frame
    blt_surface_ref_t ref = blt_upload(&g_e, g_texscratch, w, h, w * 2);
    int evicted = 0;
    while (!ref.valid && evict_one_lru()) { evicted++; ref = blt_upload(&g_e, g_texscratch, w, h, w * 2); }
    if (!ref.valid) {
        if (mf_heaplog_on()) {
            fprintf(stderr, "HEAPLOG STAGE FAIL %s key=%u rect=%d,%d %dx%d(%zu bytes) evicted=%d "
                    "heap_used=%u/%u\n", what, key, rx, ry, w, h, (size_t)w*h*2, evicted,
                    blt_alloc_used(&g_e.alloc), g_e.alloc.size);
            mf_heaplog_frame_set("at-fail", /*verbose=*/1);
        }
        return ref;   // overflow left set by the last blt_upload -> frame drops, correct
    }
    g_e.overflow = ov_before;                         // our transient failed-then-succeeded uploads did NOT overflow the frame
    g_upload_count++;
    if (mf_heaplog_on())
        fprintf(stderr, "HEAPLOG upload %s key=%u rect=%d,%d %dx%d bytes=%u off=%u evicted=%d heap_used=%u/%u\n",
                what, key, rx, ry, w, h, ref.size, ref.off, evicted, blt_alloc_used(&g_e.alloc), g_e.alloc.size);
    // Fabric-offload Task 3: stage this freshly-uploaded page DDR3->SDRAM at the
    // SAME offset (SDRAM[off] = DDR[off]) so the fabric's TRILIST texel fetch —
    // which samples SDRAM unconditionally at src_off == ref.off — resolves. Emitted
    // unconditionally (host + device): the refmodel skips OP_STAGE as a no-op (see
    // case_stage_noop), so the host oracle stays ±1 LSB. Same-offset by design, NOT
    // blt_stage_surface (its decoupled sdram_off != src_off would render black on HW).
    // Tied to the upload (miss only) => each resident page is staged exactly once;
    // cache hits reuse the SDRAM-resident copy with no re-STAGE.
    if (blt_stage(&g_e, ref.off, (uint32_t)ref.stride * ref.h) != 0)
        fprintf(stderr, "backend_mfgpu: blt_stage overflow (off=%u) - draw may drop\n", ref.off);
    else
        g_stage_count++;
    // insert into a free slot, else the LRU slot -- but NEVER a frame-pinned one.
    int slot = -1; uint64_t best = ~0ull;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (!g_texcache[i].used) { slot = i; break; }
        if (g_texcache[i].lru < best) { best = g_texcache[i].lru; slot = i; }
    }
    // Pin-aware insert: when the table is full and even the global-LRU victim was
    // touched THIS frame (lru > floor), its heap offset is referenced by a
    // trilist already emitted this frame and not yet executed -- freeing+reusing
    // it would alias that draw onto the new texture (the exact hazard evict_one_lru
    // guards on the heap-pressure path). There is no safe slot, so drop the frame
    // (mirror the heap-overflow graceful path): undo this upload, set overflow, and
    // fail the stage so the caller drops the draw. A busy frame degrades to a
    // DROPPED frame, never a wrong-pixel frame.
    if (g_texcache[slot].used && g_texcache[slot].lru > g_lru_evict_floor) {
        g_cachefull_drops++;   // [Phase 1 B3] refusal witness
        mf_note_ovf_cause(MF_OVF_CACHE_FULL);   // ...and name the cause in the log
        if (mf_heaplog_on())
            fprintf(stderr, "HEAPLOG CACHE-FULL %s key=%u: all %d slots frame-pinned "
                    "- dropping frame\n", what, key, MF_TEX_CACHE_N);
        blt_emitter_free(&g_e, ref.off, ref.size);   // the STAGE cmd dies with the dropped ring
        g_e.overflow = true;
        blt_surface_ref_t bad; bad.valid = 0; return bad;
    }
    if (g_texcache[slot].used)
        blt_emitter_free(&g_e, g_texcache[slot].ref.off, g_texcache[slot].ref.size);
    g_texcache[slot] = MfTexEntry{ key, true, has_key, mask_only, ref, ++g_lru_clock,
                                   (uint16_t)rx, (uint16_t)ry, (uint16_t)w, (uint16_t)h };
    return ref;
}

// [strip in-game CRT simulation] Is this staged texel incapable of doing anything but darken?
// The old-TV overlay is the CRT tube: rounded bezel, transparent centre, and SCANLINE shading
// in the surround. Device dump of the region it actually samples (578x434 of the atlas):
// 76.8% transparent, 19.2% pure black, 3.9% very dark browns -- 44 distinct colours whose
// BRIGHTEST is luminance 46 of 255. So "black only" was too strict (it rejected the scanlines);
// the real property is that nothing in it is bright. 64 sits far above the measured 46 and far
// below any real art.
enum { MF_DARK_MAX = 64 };
static inline bool mf_texel_is_dark(uint16_t px) {
    if (px == MF_COLORKEY) return true;                       // transparent: draws nothing
    int r = ((px >> 11) & 0x1F) << 3, g = ((px >> 5) & 0x3F) << 2, b = (px & 0x1F) << 3;
    return ((r * 77 + g * 151 + b * 28) >> 8) <= MF_DARK_MAX;  // Rec.601 luma
}

#if defined(__ARM_NEON)
// Horizontal reductions over a 0x0000/0xFFFF lane mask. Written with the
// ARMv7-A subset (pairwise fold + 32-bit lane reads) rather than vmaxvq/vminvq,
// which are AArch64-only -- this file must compile for the armhf device AND for
// the arm64 dev host that runs the unit test.
static inline bool mf_neon_any_set_u16(uint16x8_t v) {
    uint32x2_t t = vreinterpret_u32_u16(vorr_u16(vget_low_u16(v), vget_high_u16(v)));
    return (vget_lane_u32(t, 0) | vget_lane_u32(t, 1)) != 0u;
}
static inline bool mf_neon_all_set_u16(uint16x8_t v) {
    uint32x2_t t = vreinterpret_u32_u16(vand_u16(vget_low_u16(v), vget_high_u16(v)));
    return (vget_lane_u32(t, 0) & vget_lane_u32(t, 1)) == 0xFFFFFFFFu;
}

// Eight RGBA8888 texels -> eight RGB565, reproducing mf_texel565 + MF_COLORKEY +
// mf_texel_is_dark lane-for-lane. `acc_key` accumulates the transparent-lane
// masks (OR) and `acc_dark` the dark-lane masks (AND); the caller folds them.
//
// Two details this must not get wrong, both pinned by the unit test:
//  * the colorkey collision nudge applies to the PACKED value before the hole
//    select, so a genuinely transparent lane still emits the key untouched;
//  * mf_texel_is_dark re-derives r/g/b from the PACKED 565, not from the source
//    bytes, so the luma below uses the re-expanded channels. The worst-case sum
//    248*77 + 252*151 + 248*28 = 64,092 stays inside 16 bits, so no widening.
static inline void mf_stage8_rgba8888(const uint8_t *src, uint16_t *dst,
                                      uint16x8_t *acc_key, uint16x8_t *acc_dark) {
    uint8x8x4_t p = vld4_u8(src);                         // r, g, b, a
    uint16x8_t keym = vcltq_u16(vmovl_u8(p.val[3]), vdupq_n_u16(128));

    // (r & 0xF8) << 8  |  (g & 0xFC) << 3  |  b >> 3
    uint16x8_t r16 = vandq_u16(vshll_n_u8(p.val[0], 8), vdupq_n_u16(0xF800));
    uint16x8_t g16 = vandq_u16(vshll_n_u8(p.val[1], 3), vdupq_n_u16(0x07E0));
    uint16x8_t b16 = vmovl_u8(vshr_n_u8(p.val[2], 3));
    uint16x8_t px  = vorrq_u16(vorrq_u16(r16, g16), b16);

    uint16x8_t key = vdupq_n_u16(MF_COLORKEY);
    px = veorq_u16(px, vandq_u16(vceqq_u16(px, key), vdupq_n_u16(0x0020)));
    px = vbslq_u16(keym, key, px);
    vst1q_u16(dst, px);

    uint16x8_t rr = vandq_u16(vshrq_n_u16(px, 8), vdupq_n_u16(0x00F8));
    uint16x8_t gg = vandq_u16(vshrq_n_u16(px, 3), vdupq_n_u16(0x00FC));
    uint16x8_t bb = vandq_u16(vshlq_n_u16(px, 3), vdupq_n_u16(0x00F8));
    uint16x8_t luma = vmulq_u16(rr, vdupq_n_u16(77));
    luma = vmlaq_u16(luma, gg, vdupq_n_u16(151));
    luma = vmlaq_u16(luma, bb, vdupq_n_u16(28));
    luma = vshrq_n_u16(luma, 8);
    uint16x8_t dark = vorrq_u16(vcleq_u16(luma, vdupq_n_u16((uint16_t)MF_DARK_MAX)),
                                vceqq_u16(px, key));

    *acc_key  = vorrq_u16(*acc_key,  keym);
    *acc_dark = vandq_u16(*acc_dark, dark);
}
#endif // __ARM_NEON

// Convert the rw x rh sub-rect of `t` rooted at (rx,ry) into a tightly-packed
// rw*rh RGB565 page at `out` (source strided by t->w, destination by rw), and
// report the two page-level properties the caller caches alongside it:
//   *out_has_key   : some texel was transparent, so a colorkey draw has holes
//   *out_mask_only : EVERY texel is dark, so this page can only ever darken
// Whole-page staging is just the rect (0,0,t->w,t->h); `t` must be textured
// (valid pixels), which both callers establish before getting here.
//
// This is the per-texel pass that runs on every texture-cache miss, so it is
// vectorized below. Host-tested against an independent oracle including the
// vector tail and both texel formats (mf_stage_texels_test.cpp, reachable via
// RasterBackend_MFGPU_TestStageTexels near the bottom of this file).
static void mf_stage_texels(const RTexture *t, int rx, int ry, int rw, int rh,
                            uint16_t *out, bool *out_has_key, bool *out_mask_only) {
    bool has_key = false;
    bool mask_only = true;
#if defined(__ARM_NEON)
    // RGBA8888 only. RGBA4444 (the g_tex16 memory toggle, off by default) keeps
    // the scalar path below -- vld4_u8 assumes 4 bytes/texel, and the same
    // carve-out already exists in blitter_raster.cpp's blit_span.
    if (t->format != RTEX_RGBA4444) {
        uint16x8_t acc_key  = vdupq_n_u16(0);        // OR of the per-lane hole masks
        uint16x8_t acc_dark = vdupq_n_u16(0xFFFF);   // AND of the per-lane dark masks
        for (int y = 0; y < rh; y++) {
            const uint8_t *src = t->rgba + ((size_t)(ry + y) * t->w + rx) * 4;
            uint16_t *dst = out + (size_t)y * rw;
            int x = 0;
            for (; x + 8 <= rw; x += 8, src += 8 * 4, dst += 8)
                mf_stage8_rgba8888(src, dst, &acc_key, &acc_dark);
            for (; x < rw; x++, dst++) {             // tail: the scalar path verbatim
                uint16_t px = mf_texel565(t, rx + x, ry + y, &has_key);
                *dst = px;
                if (!mf_texel_is_dark(px)) mask_only = false;
            }
        }
        // Fold the vector accumulators in ONCE, after every row: has_key is a
        // pure OR and mask_only a pure AND, so order does not matter. Rows
        // narrower than 8 lanes never enter the body and leave both identities
        // (0 / all-ones) untouched.
        if (mf_neon_any_set_u16(acc_key))   has_key = true;
        if (!mf_neon_all_set_u16(acc_dark)) mask_only = false;
        *out_has_key   = has_key;
        *out_mask_only = mask_only;
        return;
    }
#endif
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            uint16_t px = mf_texel565(t, rx + x, ry + y, &has_key);
            out[(size_t)y * rw + x] = px;
            if (!mf_texel_is_dark(px)) mask_only = false;
        }
    *out_has_key = has_key;
    *out_mask_only = mask_only;
}

static blt_surface_ref_t stage_texture(uint32_t key, const RTexture *t, bool *out_has_key) {
    // Whole-page entry: rect (0,0,tw,th). Untextured => 1x1 opaque-white page.
    int tw, th; bool textured = (t && t->valid && t->rgba);
    if (textured) { tw = t->w; th = t->h; } else { tw = 1; th = 1; }
    if (tw <= 0 || th <= 0 || (size_t)tw * th > MF_TEX_TEXELS) {
        blt_surface_ref_t bad; bad.valid = 0; *out_has_key = false; return bad;
    }
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == key &&
            g_texcache[i].rx == 0 && g_texcache[i].ry == 0 &&
            g_texcache[i].rw == tw && g_texcache[i].rh == th) {
            g_texcache[i].lru = ++g_lru_clock;
            *out_has_key = g_texcache[i].has_key;
            return g_texcache[i].ref;
        }
    bool has_key = false;
    // [strip in-game CRT simulation] classify while already visiting every texel -- see the
    // sub-region path for why a black+transparent-only page is interesting.
    bool mask_only = textured;
    if (textured) {
        mf_stage_texels(t, 0, 0, tw, th, g_texscratch, &has_key, &mask_only);
    } else {
        g_texscratch[0] = 0xFFFF;   // 1x1 opaque white
    }
    blt_surface_ref_t ref = mf_upload_and_cache(key, tw, th, 0, 0, has_key, mask_only, "whole");
    *out_has_key = ref.valid ? has_key : false;
    return ref;
}

// Sub-region residency: stage ONLY the tight UV sub-rect [u0,u1]x[v0,v1]
// (normalized) of texture `t` as its own small RGB565 page, keyed (key, rect).
// The rect is the texel bbox of the UVs expanded by a 1-texel margin and clamped
// to the page, so the fabric's +HALF-bias NEAREST sample can never address a
// texel just outside the cropped edge (see blt_tri.c). A cache hit reuses the
// resident sub-page (no re-upload). Returns the ref (ref.w/ref.h = cropped rw/rh)
// and, via out_rx/out_ry, the crop origin the caller needs to REBASE the quad's
// UVs into the cropped page. .valid==0 => could not fit even after eviction.
//
// UV-rebase bit-exactness: for any vertex UV `u`, the whole-page fabric texel
// coord is o.u = lround(clamp01(u)*t->w*16); rebasing to the cropped page gives
// o.u' = lround((clamp01(u)*t->w - rx)*16) = o.u - rx*16 (rx integer). So the
// cropped-page sample resolves the SAME physical texel (rx + o.u'/16 == o.u/16)
// -- byte-identical to the whole-page render, no ±1 slack introduced by cropping.
// The clamped, +1-texel-margin crop rect for UV bbox [u0,u1]x[v0,v1] of `t`. The
// margin guards the fabric's +HALF-bias NEAREST edge; clamping rx1/ry1 to the
// page (NOT page-1) keeps the max-edge texel w-1 inside the rect. Single-sourced
// so stage_texture_region and mf_draw's near-full-page decision agree exactly.
static void mf_crop_rect(const RTexture *t, float u0, float v0, float u1, float v1,
                         int *rx, int *ry, int *rw, int *rh) {
    int x0 = mf_clampi((int)floorf(u0 * t->w) - 1, 0, t->w - 1);
    int y0 = mf_clampi((int)floorf(v0 * t->h) - 1, 0, t->h - 1);
    int x1 = mf_clampi((int)ceilf (u1 * t->w) + 1, 1, t->w);
    int y1 = mf_clampi((int)ceilf (v1 * t->h) + 1, 1, t->h);
    *rx = x0; *ry = y0;
    *rw = (x1 - x0 < 1) ? 1 : x1 - x0;
    *rh = (y1 - y0 < 1) ? 1 : y1 - y0;
}

static blt_surface_ref_t stage_texture_region(uint32_t key, const RTexture *t,
                                              float u0, float v0, float u1, float v1,
                                              bool *out_has_key, int *out_rx, int *out_ry,
                                              bool *out_mask_only) {
    int rx, ry, rw, rh;
    mf_crop_rect(t, u0, v0, u1, v1, &rx, &ry, &rw, &rh);
    *out_rx = rx; *out_ry = ry;
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == key &&
            g_texcache[i].rx == rx && g_texcache[i].ry == ry &&
            g_texcache[i].rw == rw && g_texcache[i].rh == rh) {
            g_texcache[i].lru = ++g_lru_clock;
            *out_has_key = g_texcache[i].has_key;
            if (out_mask_only) *out_mask_only = g_texcache[i].mask_only;
            return g_texcache[i].ref;
        }
    if ((size_t)rw * rh > MF_TEX_TEXELS) {
        blt_surface_ref_t bad; bad.valid = 0; *out_has_key = false;
        if (out_mask_only) *out_mask_only = false;
        return bad;
    }
    bool has_key = false;
    // [strip in-game CRT simulation] A region containing NOTHING but transparent and black can
    // only ever darken what is under it. obj_old_tv's tube/iris mask is exactly that: device
    // dumps of its frames measure 100% black+colorkey with the black fraction ANIMATING
    // (4.7% -> 27.1% -> 38.2% -> 47.8%), i.e. a tube iris opening and closing over the image.
    bool mask_only = true;
    mf_stage_texels(t, rx, ry, rw, rh, g_texscratch, &has_key, &mask_only);
    blt_surface_ref_t ref = mf_upload_and_cache(key, rw, rh, rx, ry, has_key, mask_only, "region");
    *out_has_key = ref.valid ? has_key : false;
    if (out_mask_only) *out_mask_only = ref.valid ? mask_only : false;
    return ref;
}

// [Phase 3 Stage A] GMLOADER_MFGPU_TRACE forward decls: defined next to
// mf_uvlog_on below (same bring-up-capture neighbourhood), called from
// mf_emit_group's success path above that definition.
static int mf_trace_on(void);
static int mf_trace_in_window(void);
static void mf_trace_group(const blt_surface_ref_t &tex, int tw, int th,
                           uint8_t blend_mode, uint16_t colorkey,
                           uint8_t extra_flags, int nt);

// Emit one BLT_OP_TRILIST for `nt` triangles (`verts` = nt*3 vertices, whose UVs
// address the `tw`x`th` page `tex`). Converts + pushes the vertices, selects the
// colorkey-vs-blend mode (has_key + fully-opaque => BLT_BLEND_COLORKEY, exactly
// as the whole-page path did -- see the long note at mf_draw's blend site), and
// emits. Shared by the sub-region quad loop, the untextured single-page path, and
// the app-surface path (extra_flags carries BLT_F_SRC_SURFACE there). A hard emit
// failure is logged and drops just this group's triangles.
static void mf_emit_group(const blt_surface_ref_t &tex, int tw, int th,
                          const BVtx *verts, int nt, RBlend bl,
                          bool has_key, uint8_t extra_flags) {
    int nverts = nt * 3;
    if (nverts > MF_MAX_VERTS) {
        fprintf(stderr, "backend_mfgpu: %d tris exceed vertex scratch (%d verts) - draw dropped\n",
                nt, MF_MAX_VERTS);
        return;
    }
    float min_vtx_a = 1.0f;
    for (int i = 0; i < nverts; i++) {
        g_vtxscratch[i] = bvtx_to_blt(&verts[i], tw, th);
        if (verts[i].a < min_vtx_a) min_vtx_a = verts[i].a;
    }
    uint32_t eoff = blt_push_tris(&g_e, g_vtxscratch, nt);
    if (eoff == 0xFFFFFFFFu) {
        fprintf(stderr, "backend_mfgpu: vertex push overflow (%d tris) - draw dropped\n", nt);
        return;
    }
    // [Phase 1 B1] blt_push_tris returns an offset relative to vtx_buf, but the consumer —
    // blt_execute and the fabric alike — resolves it as heap_base + entry_off (see
    // blt_vtx_buf_init's comment in 3rdparty/mfgpu). Those coincided only while vtx_buf WAS
    // the heap base. Arena 1's vertex window starts MF_VTX_HALF up, so the offset must be
    // rebased or every TRILIST reads its vertices from the wrong address — garbage geometry
    // with no error, on device and in the oracle alike.
    eoff += g_arena ? (uint32_t)MF_VTX_HALF : 0u;
    uint8_t blend_mode;
    uint16_t colorkey;
    if (has_key && min_vtx_a * 255.0f >= 254.0f) {
        blend_mode = BLT_BLEND_COLORKEY;
        colorkey = MF_COLORKEY;
    } else {
        blend_mode = rblend_to_blt(bl);
        // A fully-opaque ALPHA draw over a source with no per-texel alpha IS a copy, and
        // the difference is not free on the fabric. BLT_BLEND_CONST_ALPHA sets the RTL's
        // tri_need_dst, which inserts the B_DSTW/B_DSTC comp_fbram read into EVERY pixel's
        // walk (8 states per pixel instead of 6); BLT_BLEND_COPY skips it. With cmd.alpha
        // 255 and every vertex alpha at 1.0 the blend reduces to out = src exactly, so the
        // output is bit-identical either way -- this only stops paying for the read.
        //
        // Safe by the same reasoning as the has_key branch above: !has_key means the staged
        // region contained no colorkey sentinel, i.e. no transparent texels (mf_texel565
        // maps transparency onto MF_COLORKEY and sets has_key), and a triangle list has no
        // per-texel alpha channel at all (see the note at the top of this file). So an
        // opaque, un-keyed ALPHA draw has nothing to blend against.
        //
        // Worth real time here: the measured frame graph is THREE full-screen 288x216 ALPHA
        // draws every frame (two app-surface composites plus the border) = 230400 pixels,
        // each of which was paying for a destination read it could not use.
        if (blend_mode == BLT_BLEND_CONST_ALPHA && min_vtx_a * 255.0f >= 254.0f)
            blend_mode = BLT_BLEND_COPY;
        colorkey = 0;
    }
    g_last_trilist_blend = blend_mode;   // host-test hook (opaque-ALPHA -> COPY promotion)

    // [Phase 4 Stage B follow-up] A pending APPSURF clear must be discharged not
    // only by a draw that TARGETS the app surface (the destination-side logic
    // just below, keyed on g_cur_target) but also by a draw that SAMPLES it --
    // extra_flags carrying BLT_F_SRC_SURFACE, i.e. the app-surface composite at
    // this file's src_is_appsurf call site. Without this, clear(APPSURF) -> (no
    // draw targeting APPSURF) -> composite reading APPSURF leaves the fill
    // pending past the sampling draw: it still reaches the ring eventually (
    // mf_pc_flush_all at frame end), but by then the composite already sampled
    // the PRE-CLEAR surface -- a stale-pixel read mf_pending_clear.h's contract
    // forbids. Discharge it here, correctly targeted, before this draw's own
    // blt_trilist below, so the fill lands in the ring ahead of the read.
    if (extra_flags & BLT_F_SRC_SURFACE) {
        const int as_slot = mf_pc_slot_of(MF_TARGET_APPSURF);
        if (mf_pc_pending(&g_pc, as_slot)) {
            const int saved_target = g_cur_target;
            if (saved_target != MF_TARGET_APPSURF) {
                blt_set_target(&g_e, MF_TARGET_APPSURF);
                g_cur_target = MF_TARGET_APPSURF;
            }
            int fw, fh; uint16_t fc;
            if (mf_pc_take(&g_pc, as_slot, &fw, &fh, &fc))
                blt_fill(&g_e, 0, 0, fw, fh, fc);
            if (g_cur_target != saved_target) {
                blt_set_target(&g_e, saved_target);
                g_cur_target = saved_target;
            }
        }
    }

    // [Phase 4 Stage B] Discharge or drop the deferred full-screen clear. This
    // is the only point where BOTH facts are known: that the draw survived every
    // silent-discard path in mf_draw, and what fabric blend it actually resolved
    // to. A COPY over the whole target writes every pixel, so the clear beneath
    // it cannot be observed.
    const int pc_target  = mf_pc_slot_of(g_cur_target);
    const int pc_pending = mf_pc_pending(&g_pc, pc_target);
    int pc_covers = 0;
    if (pc_pending) {
        if (nt == 2) {
            float cxs[6], cys[6];
            for (int i = 0; i < 6; i++) { cxs[i] = verts[i].x; cys[i] = verts[i].y; }
            pc_covers = mf_pc_is_cover(blend_mode == BLT_BLEND_COPY, nt,
                                       cxs, cys, BLT_FB_WIDTH, BLT_FB_HEIGHT);
        }
        if (!pc_covers) {
            // Not provably covered: emit the fill FIRST, in the ring position it
            // would have occupied had it never been deferred.
            int fw, fh; uint16_t fc;
            if (mf_pc_take(&g_pc, pc_target, &fw, &fh, &fc))
                blt_fill(&g_e, 0, 0, fw, fh, fc);
        }
    }

    if (blt_trilist(&g_e, tex, blend_mode, colorkey, /*alpha=*/255,
                    eoff, nt, extra_flags) != 0) {
        fprintf(stderr, "backend_mfgpu: blt_trilist emit failed (%d tris) - draw dropped\n", nt);
        return;
    }
    // Drop only once the covering draw is actually in the ring: a failed
    // blt_trilist must leave the clear pending for a later draw or for frame end.
    if (pc_pending && pc_covers) mf_pc_drop(&g_pc, pc_target);
    if (mf_trace_on() && mf_trace_in_window())
        mf_trace_group(tex, tw, th, blend_mode, colorkey, extra_flags, nt);
    // Coverage estimate (Task 4, moved here per review — see the long comment at
    // g_cov_px_accum's declaration): this is the actual point triangles are pushed
    // into the fabric ring, downstream of every silent-discard path in mf_draw
    // (in-flight-batch guard, self-referential appsurf guard, CRT-ghost strip,
    // duplicate-draw elision) and of both overflow checks above. `verts` are still
    // the pre-bvtx_to_blt screen-space coordinates, same space blitter.cpp decoded.
    if (mf_stat_on()) {
        for (int i = 0; i < nt; i++) {
            const BVtx &a = verts[i * 3 + 0];
            const BVtx &b = verts[i * 3 + 1];
            const BVtx &c = verts[i * 3 + 2];
            mf_cov_add_triangle(a.x, a.y, b.x, b.y, c.x, c.y);
        }
    }
}

// ── [Phase 3 Stage A] GMLOADER_MFGPU_TRACE ───────────────────────────────────
// Draw-stream capture for offline sizing + sim replay. Dumps, at the single
// point every surviving draw passes (mf_emit_group, after blt_trilist accepts),
// the exact wire-level data the fabric will read: the resolved blend/colorkey
// and the converted blt_vtx_t integers — NOT the float BVtx, so the offline
// consumers replay what the device executed, conversion included.
// Cached getenv() reads: file-scope (not function-local) statics so a
// host-test-only hook (RasterBackend_MFGPU_TestTraceReset, near the other
// Test* hooks below) can force them to re-resolve. Production still gets the
// zero-cost idiom -- nothing here reads getenv() more than once per process
// unless that hook is called, which only the host test suite does.
static FILE *mf_trace_f = NULL;
static int  mf_trace_v = -1;                        // cached mf_trace_on() result
static long mf_trace_start = -1, mf_trace_frames = -1;  // cached window bounds
static int mf_trace_on(void) {
    if (mf_trace_v < 0) {
        const char *e = getenv("GMLOADER_MFGPU_TRACE");
        if (e && *e) { mf_trace_f = fopen(e, "w"); mf_trace_v = (mf_trace_f != NULL); }
        else mf_trace_v = 0;
    }
    return mf_trace_v;
}
static long mf_trace_env_long(const char *name, long dflt) {
    const char *e = getenv(name);
    return (e && *e) ? atol(e) : dflt;
}
static int mf_trace_in_window(void) {
    if (mf_trace_start < 0) {
        mf_trace_start  = mf_trace_env_long("GMLOADER_MFGPU_TRACE_START", 0);
        mf_trace_frames = mf_trace_env_long("GMLOADER_MFGPU_TRACE_FRAMES", 8);
    }
    return g_frame_no >= mf_trace_start && g_frame_no < mf_trace_start + mf_trace_frames;
}
static void mf_trace_group(const blt_surface_ref_t &tex, int tw, int th,
                           uint8_t blend_mode, uint16_t colorkey,
                           uint8_t extra_flags, int nt) {
    fprintf(mf_trace_f,
            "MFTRACE G f=%d off=%u stride=%u texw=%d texh=%d fmt=%u "
            "blend=%u key=%u alpha=255 flags=%u nt=%d\n",
            g_frame_no, tex.off, (unsigned)tex.stride, tw, th,
            (unsigned)tex.format, (unsigned)blend_mode, (unsigned)colorkey,
            (unsigned)extra_flags, nt);
    for (int i = 0; i < nt * 3; i++)
        fprintf(mf_trace_f, "MFTRACE V %d %d %d %d %08x\n",
                (int)g_vtxscratch[i].x, (int)g_vtxscratch[i].y,
                (int)g_vtxscratch[i].u, (int)g_vtxscratch[i].v,
                g_vtxscratch[i].rgba);
    fflush(mf_trace_f);   // engine may be SIGKILLed by the bench teardown
}

// ── [Y-orientation bring-up capture] GMLOADER_MFGPU_UVLOG ────────────────────
// The app-surface composite branch below carries a self-flagged assumption:
// "UNVERIFIED against a real device UV capture (Task 1's frame-graph dump
// didn't log UV)". That unverified assumption is exactly what produced a wrong
// V-flip fix (ac41e1e, reverted in d0d5d27), so pin it with real data before
// touching orientation again.
//
// What decides where the flip belongs is the correspondence between SCREEN Y
// and texcoord V on the composite quad:
//   v at min-y (screen TOP) == 1  -> GM emits GL's bottom-origin FBO
//                                    convention; the fabric surface is
//                                    top-origin, so the composite is the
//                                    inversion site and must flip.
//   v at min-y (screen TOP) == 0  -> composite is already top-origin; the
//                                    inversion is NOT here (look at the RTL
//                                    scanout or the scene's own y).
// Also dumps page dims vs the 288x216 used region: if th > BLT_FB_HEIGHT the page is
// padded, and a normalized 1-v would be wrong even on the composite -- the
// flip has to be taken in ABSOLUTE PIXELS about the used height.
//
// APPSURF-scene draws are logged too (their screen-y range), so a flipped
// scene-into-FBO y is distinguishable from a flipped composite.
// Capped at the first few frames: one composite per frame, so a short capture
// is enough and the log stays readable over ssh.
static int mf_uvlog_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_MFGPU_UVLOG"); v = (e && *e) ? atoi(e) : 0;
                 if (e && *e && v <= 0) v = 3; }   // bare "=1"/non-numeric -> 3 frames
    return v;
}
static void mf_uvlog(const char *tag, const BVtx *v, int triCount, int tw, int th) {
    int nv = triCount * 3;
    if (nv <= 0) return;
    float ymin = v[0].y, ymax = v[0].y, v_at_ymin = v[0].v, v_at_ymax = v[0].v;
    float umin = v[0].u, umax = v[0].u, vmin = v[0].v, vmax = v[0].v;
    float xmin = v[0].x, xmax = v[0].x;
    for (int i = 1; i < nv; i++) {
        if (v[i].y < ymin) { ymin = v[i].y; v_at_ymin = v[i].v; }
        if (v[i].y > ymax) { ymax = v[i].y; v_at_ymax = v[i].v; }
        if (v[i].x < xmin) xmin = v[i].x;
        if (v[i].x > xmax) xmax = v[i].x;
        if (v[i].u < umin) umin = v[i].u;   if (v[i].u > umax) umax = v[i].u;
        if (v[i].v < vmin) vmin = v[i].v;   if (v[i].v > vmax) vmax = v[i].v;
    }
    // The verdict, stated outright so the capture needs no interpretation.
    const char *verdict = (v_at_ymin > v_at_ymax) ? "V-INVERTED(top-v>bot-v)"
                        : (v_at_ymin < v_at_ymax) ? "V-UPRIGHT(top-v<bot-v)"
                                                  : "V-DEGENERATE(equal)";
    fprintf(stderr, "UVLOG f=%d %s tris=%d page=%dx%d used=%dx%d padded=%s "
            "screen=[%.1f,%.1f..%.1f,%.1f] uv=[%.4f,%.4f..%.4f,%.4f] "
            "v@top=%.4f v@bot=%.4f %s\n",
            g_frame_no, tag, triCount, tw, th, BLT_FB_WIDTH, BLT_FB_HEIGHT,
            (tw > BLT_FB_WIDTH || th > BLT_FB_HEIGHT) ? "YES" : "no",
            xmin, ymin, xmax, ymax, umin, vmin, umax, vmax,
            v_at_ymin, v_at_ymax, verdict);
}

static void mf_draw(RSurface *d, const BVtx *v, int triCount,
                    const RTexture *t, RBlend bl, float ar, uint32_t tex_key) {
    mf_ensure_frame();
    if (g_frame_dropped) return;   // [in-flight-batch guard] no emit, and no texture staging
    // [app-surface render target, step 1] d->fbo identifies the target FBO
    // (blitter.cpp's get_render_target); tex_key identifies the sampled GL
    // texture. Either can alias the detected application surface.
    bool dst_is_appsurf = (g_appSurfFbo != 0) && (d->fbo == g_appSurfFbo);
    bool src_is_appsurf = (g_appSurfTex != 0) && (tex_key == g_appSurfTex);

    // Self-referential draw (render INTO the app surface while ALSO sampling
    // it) should be structurally impossible for any well-formed GameMaker
    // draw stream: Task 4's detection only fires for a g_curFBO==0 draw, so
    // g_appSurfFbo is always a real nonzero FBO id and that same detecting/
    // sampling draw always has d->fbo==0 -- the two conditions can't both be
    // true without a genuine GL feedback loop (spec-level UB no engine relies
    // on). Guarded anyway (reviewer's Task 5 follow-up): the RTL's shared
    // surf_rd port starves silently if comp_target==APPSURF and SRC_SURFACE
    // are both active, so if this invariant is ever violated, fail loud here
    // rather than let it surface as unexplained visual corruption on device.
    if (dst_is_appsurf && src_is_appsurf) {
        fprintf(stderr, "backend_mfgpu: self-referential APPSURF draw (dst==src) - unsupported, dropped\n");
        return;
    }

    // [strip in-game CRT simulation] Classify this draw against the app-surface present.
    if (src_is_appsurf && !dst_is_appsurf) {
        float mina = 1.0f;
        for (int i = 0; i < triCount * 3; i++) if (v[i].a < mina) mina = v[i].a;
        if (mina >= 0.999f) {
            g_appsurf_presented = true;        // the real present: always emitted
        } else if (g_appsurf_presented && mf_strip_crt()) {
            g_crt_stripped++;                  // obj_old_tv's ghost over an already-present image
            return;
        }
    }

    // [duplicate-draw elimination] Identical to the draw immediately before it, with an
    // idempotent blend => re-running it cannot change a pixel. Skip the whole thing:
    // no staging, no vertex push, no command.
#ifdef MISTER_NATIVE_VIDEO
    // [composite characterisation] Is the second full-screen pass a BLUR/BLOOM (offset or
    // scaled UVs, genuinely new pixels) or the SAME image re-blended over itself? The latter
    // is a mathematical no-op whatever its alpha, because the first pass already wrote
    // dst = src, so a*src + (1-a)*dst == src. Dump both draws' vertices once to tell them
    // apart -- position, UV, tint and alpha, since only alpha may differ for the no-op case.
    if (mf_stat_on()) {
        static int shots = 0;
        static bool have_prev = false;
        static uint32_t p_fbo, p_key; static int p_tri;
        static BVtx p_v[MF_DUP_MAX_TRIS * 3];
        if (have_prev && shots < 2 && p_fbo == d->fbo && p_key == tex_key &&
            p_tri == triCount && triCount == 2) {
            for (int i = 0; i < 6; i++)
                fprintf(stderr, "MFVTX prev[%d] xy=%.2f,%.2f uv=%.4f,%.4f rgba=%.3f,%.3f,%.3f,%.3f | "
                        "cur[%d] xy=%.2f,%.2f uv=%.4f,%.4f rgba=%.3f,%.3f,%.3f,%.3f\n",
                        i, (double)p_v[i].x, (double)p_v[i].y, (double)p_v[i].u, (double)p_v[i].v,
                        (double)p_v[i].r, (double)p_v[i].g, (double)p_v[i].b, (double)p_v[i].a,
                        i, (double)v[i].x, (double)v[i].y, (double)v[i].u, (double)v[i].v,
                        (double)v[i].r, (double)v[i].g, (double)v[i].b, (double)v[i].a);
            shots++;
        }
        if (triCount <= MF_DUP_MAX_TRIS) {
            p_fbo = d->fbo; p_key = tex_key; p_tri = triCount;
            memcpy(p_v, v, sizeof(BVtx) * (size_t)triCount * 3);
            have_prev = true;
        } else have_prev = false;
    }
#endif

    if (triCount > 0 && triCount <= MF_DUP_MAX_TRIS &&
        mf_draw_is_idempotent(v, triCount * 3, bl)) {
        if (g_last_draw.valid && g_last_draw.fbo == d->fbo && g_last_draw.tex_key == tex_key &&
            g_last_draw.bl == (int)bl && g_last_draw.ar == ar &&
            g_last_draw.triCount == triCount &&
            memcmp(g_last_draw.v, v, sizeof(BVtx) * (size_t)triCount * 3) == 0) {
            g_dup_skipped++;
            return;
        }
        g_last_draw.valid    = true;
        g_last_draw.fbo      = d->fbo;
        g_last_draw.tex_key  = tex_key;
        g_last_draw.bl       = (int)bl;
        g_last_draw.ar       = ar;
        g_last_draw.triCount = triCount;
        memcpy(g_last_draw.v, v, sizeof(BVtx) * (size_t)triCount * 3);
    } else {
        g_last_draw.valid = false;   // anything non-qualifying breaks the run
    }

    // Any OTHER (non-default, non-appsurf) render-to-texture target: the
    // fabric has one scanout FB plus the one app-surface BRAM, so effect
    // surfaces beyond the application surface stay on the SW rasterizer
    // (step-1 scope; step 2 adds N surfaces).
    if (g_defRGBA && d->rgba != g_defRGBA && !dst_is_appsurf) {
        backend_sw.draw(d, v, triCount, t, bl, ar, tex_key); return;
    }
    // Premultiplied-alpha source-over (dst = src + dst*(1-a)) has no exact fabric
    // blend; keep it on SW for bring-up (see raster_backend_convert.h).
    if (bl == RB_PREMULT)        { backend_sw.draw(d, v, triCount, t, bl, ar, tex_key); return; }
    if (triCount <= 0) return;

    mf_select_target(d->fbo);
    (void)ar;   // the fabric tri path has no alpha-test; GM's threshold is ~0

    // ── source: the app surface itself (no staging) ───────────────────────────
    // One TRILIST over ALL the draw's tris: the surface is already fabric-
    // resident (rendered there by an earlier dst_is_appsurf draw), so there is
    // nothing to crop/stage. BLT_F_SRC_SURFACE ignores src_off/stride/x/y
    // (Task 2/3 contract); tex stays zeroed/invalid, which the relaxed
    // blt_trilist guard allows when the flag is set.
    if (src_is_appsurf) {
        blt_surface_ref_t tex; memset(&tex, 0, sizeof tex);
        // UV scale: t->w/t->h are the ORIGINAL GL texture's real dimensions
        // (get_rtexture() in blitter.cpp), which may be a padded page (e.g.
        // a 2048x2048 POT atlas -- Task 1's boot trace) larger than the
        // <=288x216 used region. bvtx_to_blt(v, tw, th) = clamp(uv,0,1)*tw*16
        // is an ABSOLUTE PIXEL coordinate within that tw x th space; by the
        // (assumed) top-left-aligned used-region convention, that absolute
        // pixel coordinate is identical to the app-surface's own absolute
        // pixel addressing (both start at (0,0)), so reusing t->w/t->h here
        // -- NOT BLT_FB_WIDTH/HEIGHT -- reproduces the same coordinate the SW
        // rasterizer would sample from the real bound texture. UNVERIFIED
        // against a real device UV capture (Task 1's frame-graph dump didn't
        // log UV) -- flag for Task 9 bring-up if the surface samples offset.
        int tw = t ? t->w : BLT_FB_WIDTH;
        int th = t ? t->h : BLT_FB_HEIGHT;
        if (tw <= 0) tw = BLT_FB_WIDTH;
        if (th <= 0) th = BLT_FB_HEIGHT;
        if (g_frame_no <= mf_uvlog_on()) mf_uvlog("COMPOSITE(appsurf->screen)", v, triCount, tw, th);
        // ── V-flip, composite ONLY (GL bottom-origin FBO -> top-origin surface)
        // GM composites the application surface with GL's FBO convention: v=0 is
        // the BOTTOM of the image. Device capture (GMLOADER_MFGPU_UVLOG, Maldita
        // title screen, 801/801 composite draws identical over 400 frames):
        //   page=512x256 content=288x216 screen=[0,0..320,240]
        //   uv=[0,0 .. 0.5625,0.8438]  v@top=0.8438  v@bot=0.0000
        // The fabric app surface is top-origin, so sampling that raw renders the
        // whole scene upside-down. This draw is the ONLY place the bottom-origin
        // convention reaches the fabric: per-sprite textures are top-origin in
        // memory (mf_texel565 reads row 0 first) and are bit-exact against the
        // SW rasterizer with raw UVs -- flipping THOSE is the reverted ac41e1e
        // bug, which relocated atlas samples instead of mirroring sprites. The
        // same capture confirms it: scene->appsurf draws are 533/533 V-UPRIGHT.
        //
        // Flip about the quad's OWN V extent, NOT a normalized 1-v. 0.8438 is
        // 216/256 -- the page is a PADDED POT (512x256) holding a 288x216
        // surface, so 1-v would map 0.8438 -> 0.1562 and 0 -> 1.0, straight into
        // the dead padding below the content. (vmin+vmax)-v maps vmin<->vmax
        // exactly, inverting the sampled band in place, and is self-calibrating:
        // it needs neither the page dims nor the used-region dims. Valid because
        // this quad by construction spans the whole surface -- blitter.cpp only
        // detects the app surface from a draw covering the full viewport.
        // Degenerate (vmin==vmax) collapses to identity, which is harmless.
        int nverts = triCount * 3;
        if (nverts > MF_MAX_VERTS) nverts = MF_MAX_VERTS;   // same cap mf_emit_group enforces
        float vmin = v[0].v, vmax = v[0].v;
        for (int i = 1; i < nverts; i++) {
            if (v[i].v < vmin) vmin = v[i].v;
            if (v[i].v > vmax) vmax = v[i].v;
        }
        const float vsum = vmin + vmax;
        static BVtx compscratch[MF_MAX_VERTS];
        for (int i = 0; i < nverts; i++) { compscratch[i] = v[i]; compscratch[i].v = vsum - v[i].v; }
        mf_emit_group(tex, tw, th, compscratch, triCount, bl, /*has_key=*/false, BLT_F_SRC_SURFACE);
        return;
    }

    // Scene draws INTO the app surface, for the same capture: if GM's
    // scene-into-FBO y were the flipped one (rather than the composite's v),
    // it would show up here as sprites whose screen-y is mirrored about the
    // surface height. A handful per frame is plenty to read the convention.
    if (g_frame_no <= mf_uvlog_on() && dst_is_appsurf && t) {
        static int log_frame = -1, log_count = 0;
        if (log_frame != g_frame_no) { log_frame = g_frame_no; log_count = 0; }
        if (log_count++ < 4)
            mf_uvlog("SCENE(->appsurf)", v, triCount, t->w > 0 ? t->w : 1, t->h > 0 ? t->h : 1);
    }

    // [Task 9 bring-up diagnostic] Log the UV bbox this draw ACTUALLY touches
    // within the full page, vs the full page's own dims -- to measure how much
    // of a large sprite-sheet a typical draw uses (sub-region staging feasibility
    // evidence). Cheap: only when heaplog is on.
    if (mf_heaplog_on() && t && t->valid && t->w > 0 && t->h > 0) {
        float umin=1e9f,umax=-1e9f,vmin=1e9f,vmax=-1e9f;
        double sum_tri_px_area = 0;   // sum of PER-TRIANGLE bboxes (texels)
        for (int i = 0; i < triCount*3; i++) {
            if (v[i].u<umin) umin=v[i].u; if (v[i].u>umax) umax=v[i].u;
            if (v[i].v<vmin) vmin=v[i].v; if (v[i].v>vmax) vmax=v[i].v;
        }
        for (int q = 0; q < triCount; q++) {
            float tu0=1e9f,tu1=-1e9f,tv0=1e9f,tv1=-1e9f;
            for (int k = 0; k < 3; k++) {
                const BVtx &vv = v[q*3+k];
                if (vv.u<tu0) tu0=vv.u; if (vv.u>tu1) tu1=vv.u;
                if (vv.v<tv0) tv0=vv.v; if (vv.v>tv1) tv1=vv.v;
            }
            sum_tri_px_area += (double)(tu1-tu0)*t->w * (double)(tv1-tv0)*t->h;
        }
        int pxw = (int)((umax-umin) * t->w + 0.5f), pxh = (int)((vmax-vmin) * t->h + 0.5f);
        fprintf(stderr, "HEAPLOG uvbbox key=%u page=%dx%d tris=%d touched=%dx%d(%d bytes) "
                "of %u bytes (%.1f%%) sum_tri_px_area=%.0f (%.1f%% of page, %.1f%% of bbox)\n",
                tex_key, t->w, t->h, triCount, pxw, pxh, pxw*pxh*2, (unsigned)t->w*t->h*2,
                100.0*(pxw*pxh*2)/((double)t->w*t->h*2), sum_tri_px_area,
                100.0*sum_tri_px_area/((double)t->w*t->h),
                pxw*pxh>0 ? 100.0*sum_tri_px_area/((double)pxw*pxh) : 0.0);
    }

    // ── untextured: one 1x1 opaque-white page, single TRILIST over all tris ────
    // Key 0 is reserved for that page; a real GL texture name is never 0, so no
    // collision. Cropping a 1x1 page is meaningless, so untextured fills stay on
    // the whole-page path.
    bool textured = (t && t->valid && t->rgba);
    if (!textured) {
        bool has_key = false;
        blt_surface_ref_t tex = stage_texture(0u, t, &has_key);
        if (!tex.valid) {
            mf_note_ovf_cause(MF_OVF_HEAP_FULL);
            fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
            return;
        }
        mf_emit_group(tex, tex.w, tex.h, v, triCount, bl, has_key, /*extra_flags=*/0);
        return;
    }

    // ── fallback: odd triangle count is not clean sprite-quads ────────────────
    // The sub-region path below assumes 2-tri (6-vertex) sprite-quads. A draw
    // whose tri count is odd isn't that shape (stray/fan geometry, rare and not
    // the batched-atlas hot path), so route the WHOLE draw through the intact
    // whole-page path -- bit-exact, and it shares one cache entry instead of
    // fragmenting into odd sub-rects.
    if (triCount % 2 != 0) {
        bool has_key = false;
        blt_surface_ref_t tex = stage_texture(tex_key, t, &has_key);
        if (!tex.valid) {
            mf_note_ovf_cause(MF_OVF_HEAP_FULL);
            fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
            return;
        }
        mf_emit_group(tex, tex.w, tex.h, v, triCount, bl, has_key, /*extra_flags=*/0);
        return;
    }

    // ── textured: per-sprite-quad sub-region staging ──────────────────────────
    // Split the draw into sprite-quads (2 tris / 6 verts), crop-stage only the
    // UV sub-rect each quad samples, rebase its UVs into that cropped page, and
    // emit one TRILIST per quad. This is the change that drops the per-frame
    // device working set from whole 2048^2 atlas pages to the ~1.5-3%-of-page the
    // sprites actually touch, bit-exact vs the old whole-page render (see
    // stage_texture_region's rebase-exactness note).
    //
    // Blend selection + the keyed-cutout / faded-sprite limitation live in
    // mf_emit_group (unchanged from the whole-page path): has_key is computed
    // per cropped region -- a region that includes a keyed texel reports
    // has_key, and a keyed texel a quad samples is always inside that quad's own
    // cropped rect, so the COLORKEY-vs-COPY choice is identical to whole-page.
    for (int q = 0; q < triCount / 2; q++) {
        const BVtx *gv = v + (size_t)q * 6;    // 6 verts = one sprite-quad
        // quad UV bbox (clamp01 to match the rebase + the fabric's clamped sampling)
        float u0 = 1.0f, u1 = 0.0f, v0 = 1.0f, v1 = 0.0f;
        for (int i = 0; i < 6; i++) {
            float uu = mf_clamp01(gv[i].u), vv = mf_clamp01(gv[i].v);
            if (uu < u0) u0 = uu; if (uu > u1) u1 = uu;
            if (vv < v0) v0 = vv; if (vv > v1) v1 = vv;
        }

        // Near-full-page fallback: if the cropped rect (+margin) already covers
        // >=90% of the page, cropping buys almost nothing and just fragments the
        // cache / re-stages ~the whole page every frame -- route this quad through
        // the shared whole-page entry instead (no rebase; UVs address the page).
        int rx, ry, rw, rh;
        mf_crop_rect(t, u0, v0, u1, v1, &rx, &ry, &rw, &rh);
        if ((double)rw * rh >= 0.9 * (double)t->w * t->h) {
            bool has_key = false;
            blt_surface_ref_t tex = stage_texture(tex_key, t, &has_key);
            if (!tex.valid) {
                mf_note_ovf_cause(MF_OVF_HEAP_FULL);
                fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
                return;
            }
            mf_emit_group(tex, tex.w, tex.h, gv, 2, bl, has_key, /*extra_flags=*/0);
            continue;
        }

        bool has_key = false; int srx = 0, sry = 0; bool mask_only = false;
        blt_surface_ref_t tex = stage_texture_region(tex_key, t, u0, v0, u1, v1, &has_key, &srx, &sry,
                                                    &mask_only);
        // [strip in-game CRT simulation] obj_old_tv's CRT overlay: a FULL-SCREEN pass whose
        // staged region holds nothing but transparent and very dark texels -- the tube bezel
        // plus its SCANLINE shading -- drawn over the image
        // after it has already been presented. Its entire contribution is darkening the edges,
        // which is the framework's job on this hardware, and it costs a whole full-screen pass
        // (a screen-aligned quad is 2 triangles that EACH walk the full-screen bbox).
        //
        // The full-screen test is what keeps ordinary art safe: plenty of sprites are black +
        // transparent (shadows, silhouettes, letterboxing bars), and those must still draw.
        if (mask_only && mf_strip_crt() && g_appsurf_presented && !dst_is_appsurf) {
            float qx0 = gv[0].x, qx1 = gv[0].x, qy0 = gv[0].y, qy1 = gv[0].y;
            for (int i = 1; i < 6; i++) {
                if (gv[i].x < qx0) qx0 = gv[i].x;   if (gv[i].x > qx1) qx1 = gv[i].x;
                if (gv[i].y < qy0) qy0 = gv[i].y;   if (gv[i].y > qy1) qy1 = gv[i].y;
            }
            if ((qx1 - qx0) >= 0.8f * (float)d->w && (qy1 - qy0) >= 0.8f * (float)d->h) {
                g_crt_stripped++;
                continue;
            }
        }
        if (!tex.valid) {
            // The failed upload left g_e.overflow set -> the whole frame drops at
            // frame_end (correct: a texture that can't fit -- heap or cache-table
            // -- must not render half a command list). No point emitting the rest.
            mf_note_ovf_cause(MF_OVF_HEAP_FULL);
            fprintf(stderr, "backend_mfgpu: sub-region cannot fit heap after eviction - draw dropped\n");
            return;
        }
        // rebase this quad's UVs into the cropped page: cropped-page uv' =
        // (clamp01(uv)*page - rect_origin) / crop_dim. (srx/sry == rx/ry.)
        BVtx reb[6];
        for (int i = 0; i < 6; i++) {
            float u_abs = mf_clamp01(gv[i].u) * t->w, v_abs = mf_clamp01(gv[i].v) * t->h;
            reb[i] = gv[i];
            reb[i].u = (u_abs - srx) / (float)tex.w;
            reb[i].v = (v_abs - sry) / (float)tex.h;
        }
        mf_emit_group(tex, tex.w, tex.h, reb, 2, bl, has_key, /*extra_flags=*/0);
    }
}

// ---- [OSD-fps] FPS overlay (ported from the Solarus core) -------------------
// blitter_top.sv mirrors the OSD "FPS Overlay" toggle (status[20]) into
// C_STATUS low32 bit1 every frame (S_WR_STATUS). When set, present() emits a
// 2-digit 7-segment readout as plain BLT_OP_FILL rects after all of the
// frame's draws, so it paints over the scene. Geometry mirrors the Solarus
// implementation, re-anchored to the 288x216 fabric framebuffer.
static const int      FPSOV_DIGIT_W = 8;
static const int      FPSOV_DIGIT_H = 14;
static const int      FPSOV_SEG_T   = 2;    // segment thickness
static const int      FPSOV_GAP     = 2;    // gap between the two digits
static const int      FPSOV_MARGIN  = 12;   // margin from the FB's right/bottom edges
static const int      FPSOV_BG_PAD  = 2;    // background panel padding around the digits
static const uint16_t FPSOV_BG      = 0x0000;   // black background panel
static const uint16_t FPSOV_FG      = 0x07E0;   // green digits (RGB565)

// Rolling FPS at present() cadence, the same 30-frame accumulator Solarus ran
// in its MainLoop. mf_present is called once per engine frame, so the readout
// is engine FPS (what the fabric actually displays), not fabric raster rate.
static double g_fps_value = 0.0;
static void mf_fps_tick(void) {
    static struct timespec last = {0, 0};
    static double acc_ms = 0.0;
    static int    acc_n  = 0;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec | last.tv_nsec) {
        acc_ms += (double)(now.tv_sec - last.tv_sec) * 1000.0
                + (double)(now.tv_nsec - last.tv_nsec) / 1e6;
        if (++acc_n >= 30) {
            g_fps_value = acc_ms > 0.0 ? 1000.0 * acc_n / acc_ms : 0.0;
            acc_ms = 0.0; acc_n = 0;
        }
    }
    last = now;
}

// [OSD-fps] Whether the OSD "FPS Overlay" toggle is currently on (C_STATUS
// low32 bit1, a raw status[20] level — see blitter_top's S_WR_STATUS). Oracle
// build reads the ctrl shadow, which stays 0 unless a test sets it, so the
// parity goldens are untouched.
static inline bool mf_fps_overlay_enabled(void) {
#ifdef MISTER_NATIVE_VIDEO
    if (!g_dev_ok) return false;
#endif
    return (mf_ctrl_rd(MF_C_STATUS) & 0x2u) != 0;
}

// [OSD-fps] Draw one 7-segment digit (0-9) via blt_fill segment rects.
// (x,y) = top-left of the digit cell.
static void mf_emit_fps_digit(int x, int y, int digit) {
    if (digit < 0 || digit > 9) return;
    uint8_t segs = FPSOV_SEGMENTS[digit];
    const int W = FPSOV_DIGIT_W, H = FPSOV_DIGIT_H, T = FPSOV_SEG_T;
    if (segs & 0x01) blt_fill(&g_e, x + 1,     y,             W - 2, T,       FPSOV_FG); // a
    if (segs & 0x02) blt_fill(&g_e, x + W - T, y + 1,         T,     H/2 - 1, FPSOV_FG); // b
    if (segs & 0x04) blt_fill(&g_e, x + W - T, y + H/2,       T,     H/2 - 1, FPSOV_FG); // c
    if (segs & 0x08) blt_fill(&g_e, x + 1,     y + H - T,     W - 2, T,       FPSOV_FG); // d
    if (segs & 0x10) blt_fill(&g_e, x,         y + H/2,       T,     H/2 - 1, FPSOV_FG); // e
    if (segs & 0x20) blt_fill(&g_e, x,         y + 1,         T,     H/2 - 1, FPSOV_FG); // f
    if (segs & 0x40) blt_fill(&g_e, x + 1,     y + (H-T)/2,   W - 2, T,       FPSOV_FG); // g
}

// [OSD-fps] Draw the 2-digit FPS readout (00-99) with a background panel,
// bottom-right corner of the WORK buffer. Called from present() right before
// mf_frame_end, so it overlays the game's own draws for this frame.
static void mf_emit_fps_overlay_fills(void) {
    // The overlay lands on the scanned-out WORK buffer, never the app surface:
    // restore the target if the frame's last op left it on APPSURF.
    if (g_cur_target != MF_TARGET_WORK) {
        blt_set_target(&g_e, MF_TARGET_WORK);
        g_cur_target = MF_TARGET_WORK;
    }
    int fps = fps_overlay_clamp(g_fps_value);
    int tens = fps / 10, ones = fps % 10;
    const int total_w = FPSOV_DIGIT_W * 2 + FPSOV_GAP;
    const int x0 = BLT_FB_WIDTH  - total_w - FPSOV_MARGIN;
    const int y0 = BLT_FB_HEIGHT - FPSOV_DIGIT_H - FPSOV_MARGIN;
    blt_fill(&g_e, x0 - FPSOV_BG_PAD, y0 - FPSOV_BG_PAD,
             total_w + 2 * FPSOV_BG_PAD, FPSOV_DIGIT_H + 2 * FPSOV_BG_PAD, FPSOV_BG);
    mf_emit_fps_digit(x0, y0, tens);
    mf_emit_fps_digit(x0 + FPSOV_DIGIT_W + FPSOV_GAP, y0, ones);
}

static void mf_frame_end(void);   // forward decl: defined below, called from mf_present

// [Phase 4 Stage B] Discharge every still-pending deferred clear, re-selecting
// each fill's own target via mf_pc_target_of() before emitting (g_cur_target
// may have moved since the clear was recorded). Idempotent: mf_pc_take
// returns 0 for an already-taken/empty slot, so a second call in the same
// frame is a no-op. Extracted so mf_present() can run it BEFORE the FPS
// overlay -- see the ordering note at mf_present() below -- while
// mf_frame_end() keeps its own call as a backstop for callers that reach
// frame-end without going through mf_present().
static void mf_pc_flush_all(void) {
    for (int slot = 0; slot < MF_PC_TARGETS; slot++) {
        int fw, fh; uint16_t fc;
        if (mf_pc_take(&g_pc, slot, &fw, &fh, &fc)) {
            int t = mf_pc_target_of(slot);
            if (t != g_cur_target) { blt_set_target(&g_e, t); g_cur_target = t; }
            blt_fill(&g_e, 0, 0, fw, fh, fc);
        }
    }
}

// Task 7: production entry point for present() — the frame loop calls
// clear()/draw() then present() (never frame_end() directly). Execute the
// accumulated ring into g_fb565 (mf_frame_end already does the blt_execute)
// and close the frame so the next clear/draw starts fresh. If no clear/draw
// happened this "frame" (g_frame_active false), there is nothing to execute.
static void mf_present(const RSurface *) {
    mf_fps_tick();   // engine present cadence == the FPS the display shows
    if (g_frame_active) {
        // [Phase 4 Stage B] Discharge any pending deferred clear BEFORE the FPS
        // overlay draws. A frame that clears and issues no draw never reaches
        // mf_emit_group (the only other place a pending clear is resolved), so
        // without this the overlay's fills would land in the ring first and the
        // clear -- flushed later by mf_frame_end -- would paint over the readout.
        // Guarded by g_frame_dropped only, matching mf_frame_end's own guard:
        // a dropped frame's ring must not grow, but g_e.overflow does not block
        // this (mf_frame_end already runs the equivalent flush regardless of
        // overflow, since overflow is only checked after blt_end_frame).
        if (!g_frame_dropped)
            mf_pc_flush_all();
        // [OSD-fps] Emit last, over everything this frame drew. Skipped when the
        // ring was never rebuilt (g_frame_dropped) or already overflowed — those
        // frames are dropped by mf_frame_end and must not grow the batch.
        if (!g_frame_dropped && !g_e.overflow && mf_fps_overlay_enabled())
            mf_emit_fps_overlay_fills();
        mf_frame_end();
        g_frame_active = false;
    }
}

static void mf_frame_end(void) {
    // [in-flight-batch guard] Nothing was emitted and the cursors were never rewound: the
    // ring still holds the batch the fabric is reading. blt_end_frame would bump submit_seq
    // and the doorbell would then ack a batch that was never rebuilt, so return before both.
    if (g_frame_dropped) return;
    // [Phase 4 Stage B] A frame that cleared and never drew still has to clear.
    // Backstop for callers that reach frame-end without going through
    // mf_present() (which already ran this above): idempotent, so if
    // mf_present() already discharged the pending fill this is a no-op.
    mf_pc_flush_all();
    // [Task 9 bring-up diagnostic] Log BEFORE blt_end_frame touches anything --
    // g_lru_frame_floor (set in mf_frame_begin) still delimits exactly this
    // frame's pinned working set at this point.
    mf_heaplog_frame_set(g_e.overflow ? "frame-end(OVERFLOWED)" : "frame-end(ok)", /*verbose=*/1);
    // [duplicate-draw elimination] report periodically so a device run shows whether the
    // repeat the frame graph captured is actually being elided, and how often.
#ifdef MISTER_NATIVE_VIDEO
    if (mf_stat_on()) {
        static uint32_t nf = 0, last = 0;
        if (++nf % 300 == 0) {
            fprintf(stderr, "MFDUP frames=%u dup_draws_elided=%u (+%u since last) crt_ghost_stripped=%u\n",
                    nf, g_dup_skipped, g_dup_skipped - last, g_crt_stripped);
            last = g_dup_skipped;
            fprintf(stderr, "MFCLEAR frames=%u clears_dropped=%u clears_emitted=%u defer=%d\n",
                    nf, g_pc_dropped_total, g_pc_emitted_total, mf_defer_clear_on());
        }
    }
#endif
    // [Phase 1 B1] Ring capacity halved to ~8190 commands when the ring was
    // double-buffered. Log a new high-water mark so approaching the cap is visible
    // instead of surfacing as an overflow-driven black frame.
    {
        static uint32_t hw = 0;
        if ((uint32_t)g_e.cmd_count > hw) {
            hw = (uint32_t)g_e.cmd_count;
            if (hw > 4096u)
                fprintf(stderr, "backend_mfgpu: cmd_count high-water %u (ring cap ~8190)\n", hw);
        }
    }
    blt_end_frame(&g_e);
#ifdef MISTER_NATIVE_VIDEO
    // Device: the ring + heap are already resident in the mmap'd DDR (the emitter
    // was bound to g_dev_ring/g_dev_src). Publish the control block, ring the
    // doorbell, and poll C_DONE — the fabric composites into on-chip BRAM and scans
    // itself out. No blt_execute, no g_fb565 on the hot path.
    if (g_e.overflow) {
        fprintf(stderr, "backend_mfgpu: emitter overflow this frame - frame dropped %s\n",
                mf_ovf_cause_str());
        return;
    }
    // [Phase 1 B2] Publish and return. The await moved to mf_frame_begin, so the engine's
    // Process() for the next frame now overlaps the fabric's raster window instead of running
    // strictly before the submit. Safe only because the ring and vertex buffer are
    // double-buffered (B1) and textures are pinned for two frames (B3) — without both, this
    // is the corruption cascade documented at the in-flight-batch guard below.
    if (g_dev_ok) {
        // [Phase 2 host lever] Barrier immediately before the control-block writes, not at
        // frame start. A false return means the previous batch never acked; publishing would
        // stomp it, so drop this frame's batch and leave the ring intact.
        if (!mf_publish_barrier()) {
            g_frame_dropped = true;
            fprintf(stderr, "backend_mfgpu: publish barrier timed out on seq=%u - batch dropped\n",
                    g_pending_seq);
        } else {
            mf_device_publish();
            g_fabric_pending = true; g_pending_seq = g_e.submit_seq;
            g_pending_arena  = g_arena & 1u;
        }
    }
    else          fprintf(stderr, "backend_mfgpu: device DDR unmapped - frame dropped\n");
#else
    // Host oracle: software-execute the ring into g_fb565 (parity tests read it back).
    if (g_e.overflow) {
        fprintf(stderr, "backend_mfgpu: emitter overflow this frame - frame dropped %s\n",
                mf_ovf_cause_str());
        memset(g_fb565, 0, sizeof g_fb565);   // nothing safe to execute this frame
        return;
    }
    // [Phase 1 B2] Drive the same submit seam the device path uses, so the publish/await
    // bookkeeping is observable off-device. blt_execute below stands in for the fabric and
    // is synchronous, so the await is trivially satisfied and costs nothing here.
    // [Phase 1 B2] Oracle: publish, then let blt_execute stand in for the fabric. The await
    // moves to the next mf_frame_begin exactly as on device, so the deferral is observable
    // off-device — without setting g_fabric_pending here the oracle never reaches the await
    // and case_await_deferred_one_frame is unsatisfiable.
    // [Phase 2 host lever] Same barrier-then-publish order as the device path, so the
    // oracle exercises the moved barrier rather than the old frame-start await. The return
    // value is LOAD-BEARING: a failed barrier means the fabric is still reading the ring, so
    // ringing the doorbell would stomp it.
    // [Phase 2 host lever] And it leaves g_fb565 ALONE. The device has no g_fb565: a frame
    // that never publishes leaves the fabric scanning out the last batch that did, so the
    // picture holds. Blanking here would make the oracle report a black frame where the
    // device reports an unchanged one -- and "did the dropped frame execute anyway?" is
    // exactly what case_inflight_drop asks by comparing framebuffers.
    if (!mf_publish_barrier()) {
        g_frame_dropped = true;
        return;
    }
    mf_device_publish();
    g_fabric_pending = true; g_pending_seq = g_e.submit_seq;
    g_pending_arena  = g_arena & 1u;
    int n = g_e.cmd_count;
    if (n > MF_MAX_CMDS) n = MF_MAX_CMDS;
    for (int i = 0; i < n; i++)
        blt_unpack_cmd(g_e.ring + (size_t)i * BLT_CMD_BYTES, &g_cmds[i]);
    // The whole g_srcdram is the source region: vertices at low offsets, texture
    // pages above MF_VTX_REGION. blt_execute only reads (and clamps OOB), so
    // passing the full size keeps both offset ranges in-bounds.
    blt_surface_heap_t heap = { g_srcdram, sizeof g_srcdram, nullptr, nullptr };
    memset(g_fb565, 0, sizeof g_fb565);   // only a frame that actually executes repaints
    blt_execute(g_fb565, &heap, g_cmds, n);
#endif
}

// Task 7 device wiring: the fabric framebuffer, for main.cpp to hand straight
// to NativeVideoWriter_WriteFrame after present() (g_fb565 is already RGB565 —
// no Blitter_ToRGB565 conversion needed). w/h are BLT_FB_WIDTH x BLT_FB_HEIGHT
// (288x216, refmodel/blitter_ref.h), the fixed fabric scanout geometry — same
// as MISTER_WIDTH/MISTER_HEIGHT today, so callers can use either pair, but the
// row stride must always be computed from *this* w (BLT_FB_WIDTH), not assumed.
extern "C" const uint16_t *RasterBackend_MFGPU_GetFB565(int *w, int *h) {
    if (w) *w = BLT_FB_WIDTH;
    if (h) *h = BLT_FB_HEIGHT;
    return g_fb565;
}

// [Phase 3 Stage B] Scanout instrument readout — the DISPLAY's frame boundary,
// which C_DONE (a fabric-completion count) is not. openbor_video_reader.sv
// publishes both words in ONE 64-bit DDR beat at SCANFRM_ADDR = byte 0x3BFB0018:
//   +0x00  scan_frame_cnt   monotonic, +1 per scanout frame boundary, wraps 2^32
//   +0x04  scan_period_cyc  clk_sys (98.4375 MHz) cycles between the last two
//                           boundaries; measured 1,642,740 = 16.6882 ms = 59.9228 Hz
// That address is offset 0x00FB0018 inside the 16 MiB region this back-end already
// mmaps, so the per-frame read the main loop's frame cap needs is a plain uncached
// load — no `devmem` process spawn. Read-only: the reader owns these words.
// Returns 1 when the mapping is live (device + /dev/mem + fabric back-end), else 0
// with the outputs untouched — callers MUST treat 0 as "no instrument" and fall back.
extern "C" int RasterBackend_MFGPU_ScanoutRead(uint32_t *frame_cnt, uint32_t *period_cyc) {
#ifdef MISTER_NATIVE_VIDEO
    if (!g_dev_ok || !g_dev_base) return 0;
    const volatile uint32_t *p =
        (const volatile uint32_t *)(g_dev_base + (size_t)MF_DEV_SCANFRM_OFF);
    // Count first: the two 32-bit loads are not mutually atomic, and a stale
    // period merely describes the previous interval (harmless), whereas a stale
    // count would mis-order the cap's advance test.
    uint32_t c = p[0];
    uint32_t t = p[1];
    if (frame_cnt)  *frame_cnt  = c;
    if (period_cyc) *period_cyc = t;
    return 1;
#else
    (void)frame_cnt; (void)period_cyc;
    return 0;   // host oracle build: no fabric, no scanout
#endif
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

// Host test hook (TDD; see gmloader/mister/mf_cov_clip_test.cpp): expose the
// pure rect-clip area function directly, independent of mf_stat_on()/the
// accumulator/any device state, so the coverage estimator's actual clipping
// geometry can be unit-tested. See mf_clip_tri_area's definition (above
// mf_texel565's #ifdef MISTER_NATIVE_VIDEO neighbor) for the algorithm.
extern "C" double RasterBackend_MFGPU_TestClipTriArea(float x0, float y0, float x1, float y1,
                                                       float x2, float y2) {
    return mf_clip_tri_area(x0, y0, x1, y1, x2, y2);
}

// Host test hook (TDD; see gmloader/mister/mf_stage_texels_test.cpp): stage a
// rect straight into a caller-supplied buffer, bypassing g_texscratch, the
// texture cache and the emitter, so the per-texel conversion can be diffed
// against an independent oracle. Writes exactly rw*rh uint16s. The bool outputs
// are int* so the test can print them without a bool/int mismatch.
extern "C" void RasterBackend_MFGPU_TestStageTexels(const RTexture *t, int rx, int ry,
                                                    int rw, int rh, uint16_t *out,
                                                    int *out_has_key, int *out_mask_only) {
    bool has_key = false, mask_only = true;
    mf_stage_texels(t, rx, ry, rw, rh, out, &has_key, &mask_only);
    if (out_has_key)   *out_has_key   = has_key   ? 1 : 0;
    if (out_mask_only) *out_mask_only = mask_only ? 1 : 0;
}

// Host/Task-6 hook: tell the backend which surface pixels are the default fb, so
// draw() can route FBO / render-to-texture targets to the SW fallback. Pass the
// default RSurface's rgba pointer (NULL restores "everything is default fb").
extern "C" void RasterBackend_MFGPU_SetDefaultSurface(const uint8_t *rgba) {
    g_defRGBA = rgba;
}

// [Phase 3 Stage A] host hook: read g_frame_no as-is rather than mirroring or
// assuming it — it is a whole-process monotonic counter TestReinit never
// resets (only mf_frame_begin increments it), so a test that needs to reason
// about the GMLOADER_MFGPU_TRACE window must read the real value instead of
// hardcoding 0. Same "read from the backend" rationale as the OVF_* hooks below.
extern "C" int RasterBackend_MFGPU_TestFrameNo(void) { return g_frame_no; }
// [Phase 3 Stage A] host-test-only hook: force GMLOADER_MFGPU_TRACE's cached
// getenv() reads to re-resolve. mf_trace_on()/mf_trace_in_window() cache in
// process-lifetime statics on purpose (zero-cost on device, same idiom as
// mf_stat_on()), which makes them resolve ONCE per process -- fine on device,
// but it means a host case that sets the env vars mid-run only works if
// nothing upstream in the same test binary already resolved the cache.
// Mirrors RasterBackend_MFGPU_TestReinit's "force a clean state" contract:
// closes any open trace file and clears every cached value, so the next
// mf_trace_on()/mf_trace_in_window() call reads the environment fresh,
// regardless of what already ran earlier in the process.
extern "C" void RasterBackend_MFGPU_TestTraceReset(void) {
    if (mf_trace_f) { fclose(mf_trace_f); mf_trace_f = NULL; }
    mf_trace_v = -1;
    mf_trace_start = -1;
    mf_trace_frames = -1;
}
// Task 2 host hooks: count real blt_uploads (cache-hit-vs-miss proof) and force
// a clean re-init with an optional capped texture-heap size (0 = full MF_TEX_HEAP;
// nonzero lets the Task 4 eviction test shrink the allocator on purpose).
extern "C" uint32_t RasterBackend_MFGPU_TestUploadCount(void) { return g_upload_count; }
// FO Task 3 host hook: BLT_OP_STAGE emits since reinit (proves stage-once-per-page).
extern "C" uint32_t RasterBackend_MFGPU_TestStageCount(void) { return g_stage_count; }
// [in-flight-batch guard] host-test hooks: force the "fabric still busy" predicate
// (-1 = ask for real) and read the dropped-frame tally.
extern "C" void RasterBackend_MFGPU_TestSetFabricBusy(int busy) {
    g_test_fabric_busy = busy;
    // busy>0 simulates the whole device condition, not just the predicate: a submit that
    // timed out and left a batch unacked. busy==0 deliberately LEAVES g_fabric_pending set,
    // so the next frame exercises the "acked -> clear the flag and resume" branch.
    // [Phase 1 B2] Also model the PUBLISH the simulated in-flight batch implies. Setting
    // g_fabric_pending alone would leave the pairing depth at 0, so the deferred await in
    // the next mf_frame_begin would count an unpaired await -- a false positive created by
    // the hook, not by the code. Assignment rather than increment: exactly one batch is ever
    // outstanding, so this is idempotent whether or not a real publish already happened.
    if (busy > 0) { g_fabric_pending = true; g_pending_seq = g_e.submit_seq; g_publish_depth = 1; }
}
extern "C" uint32_t RasterBackend_MFGPU_TestDropCount(void) { return g_drop_count; }
// [Phase 1 B2] host-test hooks: prove publish and await are separable and each ran once.
extern "C" uint32_t RasterBackend_MFGPU_TestPublishCount(void) { return g_publish_count; }
extern "C" uint32_t RasterBackend_MFGPU_TestAwaitCount(void)   { return g_await_count; }
// [Phase 1 B2] Ordering witness: awaits that ran with no batch in flight, and publishes
// issued while one was already in flight. Both must always be 0 — counters alone cannot see
// publish/await order on the oracle (see g_publish_depth).
extern "C" uint32_t RasterBackend_MFGPU_TestUnpairedAwaits(void) { return g_unpaired_awaits; }
// [Phase 2 host lever] The seq the last doorbell carried, and the seq the last await polled
// C_DONE for. Unequal means the await can never be satisfied.
extern "C" uint32_t RasterBackend_MFGPU_TestLastPublishedSeq(void) { return g_last_published_seq; }
// [Phase 4 Stage A] Complete seam samples since reinit. Off-device nothing prints,
// so the accumulator is never reset and this counts frames directly.
extern "C" uint32_t RasterBackend_MFGPU_TestSeamSampleCount(void) { return g_seam.n; }
// [Finding 1 regression] Samples skipped because the barrier returned true without
// re-stamping g_seam_ar (the !g_fabric_pending early-out or a reclaim) -- see
// mf_publish_barrier. Windowed like g_seam.n, but read here before either resets.
extern "C" uint32_t RasterBackend_MFGPU_TestSeamIncomplete(void) { return g_seam_incomplete; }
extern "C" uint32_t RasterBackend_MFGPU_TestLastAwaitedSeq(void)   { return g_last_awaited_seq; }
extern "C" uint32_t RasterBackend_MFGPU_TestEmitterSeq(void)       { return g_e.submit_seq; }
extern "C" uint32_t RasterBackend_MFGPU_TestSeamDepthViolations(void) { return g_seam_depth_violations; }
// [Phase 1 B2] Control-word write trace for the LAST publish: reg index per entry
// (MF_TRACE_BARRIER == -1 marks the memory barrier), so a test can pin that the doorbell is
// written last and the barrier precedes it. Invisible to counters -- mf_ctrl_wr's store
// compiles out on the oracle, so publish's body was otherwise untestable off-device.
extern "C" uint32_t RasterBackend_MFGPU_TestTraceLen(void) { return g_ctrl_trace_n; }
// Nonzero => the trace truncated, so every ordering assertion over it is vacuous. Asserted 0.
extern "C" uint32_t RasterBackend_MFGPU_TestTraceOverflow(void) { return g_ctrl_trace_overflow; }
extern "C" int RasterBackend_MFGPU_TestTraceReg(uint32_t i) {
    return (i < g_ctrl_trace_n) ? g_ctrl_trace[i].reg : -99;
}
// [Phase 1 B1] The VALUE written, not just which register. Task 1's trace proved where a
// write happened; Task 2's contract (C_SRCSEL bit 2 == g_arena) is a value property, and a
// select bit stuck at 0 passes every ordering assertion while the fabric reads ring A
// forever. Position alone cannot see it.
extern "C" uint32_t RasterBackend_MFGPU_TestTraceVal(uint32_t i) {
    return (i < g_ctrl_trace_n) ? g_ctrl_trace[i].val : 0u;
}
// [Phase 1 B1] host-test hooks: prove the arenas alternate and are disjoint. Field names
// are the emitter's real ones (blt_emitter_t: `ring`, `vtx_buf`), not assumed.
extern "C" uint32_t  RasterBackend_MFGPU_TestArena(void)    { return g_arena; }
// [Phase 1 B3] eviction ATTEMPTS since reinit — the vacuity guard for the retention case.
extern "C" uint32_t  RasterBackend_MFGPU_TestEvictAttempts(void) { return g_evict_attempts; }
// [Phase 1 B3] full-table inserts refused by the pin-aware guard, and the table's slot count.
// The slot count is exposed rather than mirrored in the test so the two cannot drift: a test
// that fills a hardcoded 256 slots stops filling the table the moment MF_TEX_CACHE_N grows,
// and goes quietly vacuous with nothing failing.
extern "C" uint32_t  RasterBackend_MFGPU_TestCacheFullDrops(void) { return g_cachefull_drops; }
// [Phase 1 B3] Which cause was recorded for the frame just closed. Exposed so the value that
// SELECTS the overflow message is itself asserted -- a cause that is never recorded would
// silently restore the ambiguity it exists to remove, and nothing would fail.
extern "C" int       RasterBackend_MFGPU_TestFrameOvfCause(void) { return (int)g_frame_ovf_cause; }
// Exposed so the test does not hand-mirror MfOvfCause's values. Same reasoning as
// TestTexCacheSlots: a mirrored constant is a second source of truth that drifts silently.
extern "C" int       RasterBackend_MFGPU_OvfCauseUnknown(void)   { return (int)MF_OVF_UNKNOWN; }
extern "C" int       RasterBackend_MFGPU_OvfCauseCacheFull(void) { return (int)MF_OVF_CACHE_FULL; }
extern "C" int       RasterBackend_MFGPU_OvfCauseHeapFull(void)  { return (int)MF_OVF_HEAP_FULL; }
// [Phase 1 A4] The covered-pixel derivation, exposed for host testing. Reading C_FLAGS.hi
// needs a fabric; the arithmetic over it does not, and is where the provenance bug lived.
extern "C" void RasterBackend_MFGPU_TestDeriveCov(double dpath_ms, double cov_exact,
                                                  double cov_est, double screen_px,
                                                  double clk_mhz, double *cov_den,
                                                  double *cyc_px, double *overdraw,
                                                  double *est_ratio, int *estimated) {
    MfCovDerived d = mf_derive_cov(dpath_ms, cov_exact, cov_est, screen_px, clk_mhz);
    if (cov_den) *cov_den = d.cov_den;   if (cyc_px)   *cyc_px   = d.cyc_px;
    if (overdraw) *overdraw = d.overdraw; if (est_ratio) *est_ratio = d.est_ratio;
    if (estimated) *estimated = d.estimated;
}
extern "C" uint32_t  RasterBackend_MFGPU_TestTexCacheSlots(void)  { return MF_TEX_CACHE_N; }
extern "C" uintptr_t RasterBackend_MFGPU_TestRingBase(void) { return (uintptr_t)g_e.ring; }
extern "C" uintptr_t RasterBackend_MFGPU_TestVtxBase(void)  { return (uintptr_t)g_e.vtx_buf; }
// [Phase 1 B1] Seed C_SRCSEL's shadow so the read-modify-write has a non-zero throttle
// field (bits 15:8) to preserve — otherwise "the throttle survives" is a vacuous 0 == 0.
extern "C" void RasterBackend_MFGPU_TestSetCtrlSrcsel(uint32_t v) {
#ifndef MISTER_NATIVE_VIDEO
    g_ctrl_shadow[MF_C_SRCSEL] = v;
#else
    (void)v;
#endif
}
// [OSD-fps] Force mf_fps_overlay_enabled()'s C_STATUS bit1 so a host test can
// exercise the FPS overlay path without a device: MF_C_STATUS is never
// written elsewhere in this file (it is an OSD-mirror register the firmware
// sets, only ever read here), so this cannot be clobbered by frame logic.
extern "C" void RasterBackend_MFGPU_TestSetFpsOverlay(int on) {
#ifndef MISTER_NATIVE_VIDEO
    g_ctrl_shadow[MF_C_STATUS] = on ? 0x2u : 0u;
#else
    (void)on;
#endif
}
// Blend mode the last TRILIST went out with, so a test can pin the opaque-ALPHA -> COPY
// promotion (and, just as importantly, that a NON-opaque draw is left alone).
extern "C" int RasterBackend_MFGPU_TestLastTrilistBlend(void) { return g_last_trilist_blend; }
// [duplicate-draw elimination] draws elided because they repeated the previous one verbatim.
extern "C" uint32_t RasterBackend_MFGPU_TestDupSkipped(void) { return g_dup_skipped; }
// [strip in-game CRT simulation] old-TV ghost passes dropped.
extern "C" uint32_t RasterBackend_MFGPU_TestCrtStripped(void) { return g_crt_stripped; }
// [Phase 4 Stage B] deferred full-screen clear: witness counters (run totals +
// whatever the in-flight frame has not yet folded in) and a ring-content probe.
extern "C" uint32_t RasterBackend_MFGPU_TestClearsDropped(void) { return g_pc_dropped_total + g_pc.dropped; }
extern "C" uint32_t RasterBackend_MFGPU_TestClearsEmitted(void) { return g_pc_emitted_total + g_pc.emitted; }
// Count BLT_OP_FILL entries currently in g_e's command list (the ring, not a
// decoded copy) -- valid any time after present()/frame_end closes the frame.
extern "C" uint32_t RasterBackend_MFGPU_TestFillCommandCount(void) {
    uint32_t n = 0;
    for (int i = 0; i < g_e.cmd_count; i++) {
        blt_cmd_t c;
        blt_unpack_cmd(g_e.ring + (size_t)i * BLT_CMD_BYTES, &c);
        if (c.opcode == BLT_OP_FILL) n++;
    }
    return n;
}
extern "C" int RasterBackend_MFGPU_TestFirstCommandIsFill(void) {
    if (g_e.cmd_count <= 0) return 0;
    blt_cmd_t c;
    blt_unpack_cmd(g_e.ring, &c);
    return c.opcode == BLT_OP_FILL;
}
// [Ring-order regression] The RGB565 color of the FIRST BLT_OP_FILL anywhere
// in the ring (not necessarily the first command overall -- a SET_TARGET may
// precede it), or -1 if the ring has no fill at all. Distinguishes WHICH fill
// went out first (the deferred clear vs. the FPS overlay's background panel)
// where TestFirstCommandIsFill only pins that a fill happened to be first.
extern "C" int RasterBackend_MFGPU_TestFirstFillColor(void) {
    for (int i = 0; i < g_e.cmd_count; i++) {
        blt_cmd_t c;
        blt_unpack_cmd(g_e.ring + (size_t)i * BLT_CMD_BYTES, &c);
        if (c.opcode == BLT_OP_FILL) return (int)c.color;
    }
    return -1;
}
// [Ring-order regression, source-sample discharge] True (1) iff the ring's
// first BLT_OP_FILL appears BEFORE its first BLT_OP_TRILIST; 0 if either is
// absent or the fill comes after. TestFirstCommandIsFill only answers "is
// index 0 a fill", which a SET_TARGET (or nothing at all, if the fill never
// got emitted ahead of the draw) already defeats -- this instead pins the fill
// against the specific draw it must precede, regardless of what else sits
// between them in the ring.
extern "C" int RasterBackend_MFGPU_TestFillPrecedesTrilist(void) {
    int fill_idx = -1, tri_idx = -1;
    for (int i = 0; i < g_e.cmd_count; i++) {
        blt_cmd_t c;
        blt_unpack_cmd(g_e.ring + (size_t)i * BLT_CMD_BYTES, &c);
        if (fill_idx < 0 && c.opcode == BLT_OP_FILL)    fill_idx = i;
        if (tri_idx  < 0 && c.opcode == BLT_OP_TRILIST) tri_idx  = i;
    }
    if (fill_idx < 0 || tri_idx < 0) return 0;
    return fill_idx < tri_idx;
}
// [Phase 4 Stage B] host-test-only hook: force GMLOADER_MFGPU_DEFER_CLEAR's
// cached getenv() read to re-resolve. Same rationale + idiom as
// RasterBackend_MFGPU_TestTraceReset above (GMLOADER_MFGPU_TRACE): a host case
// that sets the env var mid-run needs the cache cleared, or it only works as
// the first thing in the binary to call mf_defer_clear_on().
extern "C" void RasterBackend_MFGPU_TestEnvReset(void) { g_defer_clear_v = -1; }
extern "C" void RasterBackend_MFGPU_TestReinit(uint32_t tex_heap_bytes) {
    g_inited = false; g_frame_active = false;
    g_tex_heap_cap = tex_heap_bytes;   // 0 => full heap
    mf_init_once();                    // re-wires emitter, clears cache + counter
    g_lru_frame_floor = 0;             // start clean: nothing pinned before frame 1
    g_lru_evict_floor = 0;             // [Phase 1 B3] ...and nothing pinned from before that
    mf_seam_reset(&g_seam);            // [Phase 4 Stage A] reinit means "no history"
    g_seam_have_prev = false;          // ...so the next publish opens a sample, not closes one
    g_seam_blocked   = false;
    g_seam_frame_ms  = 0.0;            // [Finding 2] stale fabric-ms from a prior run
    g_seam_incomplete = 0;             // [Finding 2] stale skipped-sample count from a prior run
}
// [Phase 4 Stage B] Alias over the established "start clean" primitive. No
// bespoke whole-state reset hook existed before this task -- every other case
// in this file already uses RasterBackend_MFGPU_TestReinit(0) for that job: it
// re-runs mf_init_once (which now also clears g_pc/g_pc_dropped_total/
// g_pc_emitted_total, see mf_init_once's [Phase 4 Stage B] block).
extern "C" void RasterBackend_MFGPU_TestReset(void) { RasterBackend_MFGPU_TestReinit(0); }

// Free the cached entry for GL texture `id` so the next draw re-stages it. Called
// by blitter.cpp on TexImage2D re-upload and DeleteTexture — exactly when the SW
// oracle's CPU pixels change, so the cache never diverges from SW.
extern "C" void RasterBackend_MFGPU_InvalidateTex(uint32_t id) {
    if (!g_inited) return;
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == id) {
            blt_emitter_free(&g_e, g_texcache[i].ref.off, g_texcache[i].ref.size);
            g_texcache[i].used = false;
        }
}

extern "C" const RasterBackend backend_mfgpu = {
    "mfgpu", mf_frame_begin, mf_clear, mf_draw, mf_present, mf_frame_end,
};
