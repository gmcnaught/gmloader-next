# MFGPU Persistent Atlas Staging — Design

**Date:** 2026-07-14
**Branch:** `mister-sdl-buffer-output` (base `c749413`)
**Repo:** `gmloader-next`
**Status:** approved (design), pending implementation plan

## Context

The `RasterBackend` seam is behavior-transparent on hardware and the MFGPU
`BLT_OP_TRILIST` fabric pipeline runs end-to-end on the device
(draw → `blt_execute` → colorkey → `g_fb565` → `NativeVideoWriter` scanout).
The on-hardware test (2026-07-14, device `192.168.20.81`, `armv7l`) produced a
precise, honest map of the two things standing between "the fabric pipeline
runs" and "a real Maldita visual":

- **(a) Pre-existing crash.** `SIGSEGV PC=nil` (a call through a null GL function
  pointer) ~24 draws in, immediately after a `1024×2048` texture upload.
  Reproduces on the pre-seam backup, so it is **not** introduced by the seam.
  Caps every run at ~1–2s.
- **(b) Atlas overflow.** Maldita's `2048×2048` sprite pages (4M texels = 8MB as
  RGB565) exceed `backend_mfgpu`'s ~1MB source heap (512K texels), so those
  draws are **dropped**. Only the small `512×256` textures reach the fabric.

This design addresses **(b)** — persistent, identity-keyed texture staging. It
is a **correctness** prerequisite (not merely a perf optimization): without it,
the draws that carry Maldita's actual sprites never render on the fabric path.
Blocker **(a)** is a separate, sequenced follow-up (see "Out of scope").

### Key facts that shape the design

- **The TRILIST texture source is a single DDR heap base.** `blt_tri.c:44`
  reads `heap->base + h->src_off`. The 64MB-SDRAM whole-quest residency
  machinery (`blt_stage_surface_perm`, `sdram_src`, `blt_sdram_regions_init`)
  was built for the **2D blit/tile path**, not the triangle path. Routing
  TRILIST through SDRAM would require extending the bit-exact `blt_tri` engine
  **and** the RTL, re-proving bit-exactness — out of scope here.
- **`blt_execute` runs in-process on the A9** on device (the "fabric" is
  currently the A9 software rasterizer producing `g_fb565`; RTL offload is a
  later step). `g_srcdram` is a plain static array on both host and device —
  there is **no FPGA-DDR mmap** to size around. Growing the heap is a `.bss`
  bump, safe as process RAM on both the dev host and the MiSTer's ~1GB ARM RAM.
- **Texture identity is available at the seam.** `get_rtexture()` samples
  `g_boundTex2D`; `g_textures` is keyed by GL texture id. That id is a stable,
  unique key per GM texture page.

### Chosen approach (from brainstorming)

**Option A — persistent DDR-heap texture cache**, keyed by GL texture id,
funneled through a single `stage_texture()` helper so the residency backing
could later be repointed to SDRAM whole-quest residency (Option B) without
touching call sites. Option B (SDRAM residency for TRILIST) is recorded as a
separate future scaling epic.

## Architecture

`blitter.cpp` keeps ownership of GL state shadowing and draw decode. Beneath the
`RasterBackend` seam, `backend_mfgpu` gains a persistent texture cache resident
across frames. The single behavioral change to the seam contract is a new
`tex_key` argument on `draw`, which the decode layer already has and `backend_sw`
ignores.

```
blitter.cpp (decode; unchanged logic)
  └─ RasterBackend_Select()->draw(dst, verts, tris, tex, blend, alphaRef, tex_key)
       ├─ backend_sw.draw(...)   // tex_key ignored → byte-identical to today
       └─ backend_mfgpu.draw(...)
            └─ stage_texture(tex_key, tex) -> blt_surface_ref_t   // cache funnel
                 ├─ hit  → reuse ref (no re-upload)
                 └─ miss → convert RGBA→RGB565, blt_upload once, store ref
```

## Components

### 1. Seam signature extension (`tex_key`)

- `raster_backend.h`: `draw` gains `uint32_t tex_key` as the last parameter.
- `blitter.cpp` draw dispatch passes `g_boundTex2D` (0 when untextured).
- `backend_sw` ignores `tex_key` — behavior byte-identical (the SW-equivalence
  regression test must still pass unchanged).
- Existing `raster_backend_test.cpp` call sites updated to pass a key.

*Rationale for a signature extension over a side-channel setter:* the key is
true, explicit identity the decode layer already holds; it is testable (tests
pass distinct keys); it does not touch state-shadowing or decode logic. This is
a seam extension, consistent with the "decode layer off-limits" constraint
(which protects `Blitter_On*` / vertex-MVP / texture-blend resolution, not the
seam's own parameter list).

### 2. Persistent texture cache + `stage_texture()` funnel

- A fixed-size table in `raster_backend_mfgpu.cpp`:
  `struct MfTexEntry { uint32_t key; bool used; blt_surface_ref_t ref; uint64_t lru; };`
  (small N, e.g. 64 entries — Maldita's resident page count is a handful).
- `stage_texture(uint32_t key, const RTexture *t) -> blt_surface_ref_t`:
  - **hit** (matching `key`, `used`): bump `lru`, return the stored `ref`.
    **No re-conversion, no re-upload.**
  - **miss**: convert the page RGBA→RGB565 into the scratch buffer (reusing the
    existing `mf_texel565` colorkey-aware conversion), `blt_upload` once into the
    persistent heap, store `{key, ref, lru}`, return the ref. On a full table,
    evict the LRU entry first (see §5).
  - `key == 0` (untextured) resolves to a shared, one-time-staged 1×1 white page
    (never evicted) so vertex-color-only draws don't churn the cache.
- `mf_draw` calls `stage_texture()` instead of its current inline
  convert+`blt_upload` block. Colorkey blend selection (`has_key` etc.) is
  computed at stage time and remembered on the entry so the per-draw path stays
  cheap on cache hits.

*This helper is the single seam where Option B could later swap DDR-heap
residency for SDRAM whole-quest residency without changing `mf_draw` or call
sites.*

### 3. Invalidation (cache ↔ oracle coherence)

- New `extern "C" void RasterBackend_MFGPU_InvalidateTex(uint32_t id)`: if `id`
  is cached, `blt_emitter_free(&g_e, ref.off, ref.size)` and mark the entry
  unused.
- Called from `blitter.cpp`:
  - `Blitter_OnTexImage2D(tex, …)` — a (re)upload to an existing id changes its
    pixels → invalidate so the next draw re-stages.
  - `Blitter_OnDeleteTexture(tex)` — the id is gone → free its heap block.
- `Blitter_OnTexSubImage2D` remains a no-op **in both paths** (it already is for
  SW: `store_texture` is not called on sub-image, so the SW CPU-side `rgba` is
  not updated either). The cache therefore never diverges from the SW oracle.
  Documented known gap: streamed sub-texture atlas updates are not reflected —
  acceptable for Maldita's load-once palette atlases; revisit if a target GM
  game streams into a texture page.

### 4. Per-frame lifecycle change

- Remove `blt_heap_reset(&g_e)` + the per-frame `blt_alloc_init(&g_e.alloc, …)`
  from `mf_frame_begin`. Texture uploads now persist across frames.
- Per frame, only `blt_begin_frame(&g_e, …)` runs (it already resets the command
  list and the vtx cursor `vtx_used=0`). The vtx region (`[0, MF_VTX_REGION)`)
  is per-frame; the texture heap (`[MF_VTX_REGION, cap)`) persists.
- One-time wiring in `mf_init_once` (`blt_emitter_init`, `blt_alloc_init` for the
  texture region, `blt_vtx_buf_init`) is unchanged.

*This directly resolves the "re-init every frame wipes persistently-staged
surfaces" watch-item already noted in the backend's frame-model comment.*

### 5. Sizing + graceful eviction

- Conversion scratch grows to hold the largest single page: **8MB**
  (`2048×2048` texels × 2 bytes = 4M texels).
- Texture heap grows to hold the resident working set: **32MB** (≈ 4 full
  `2048²` pages, or many mixed sizes). The vtx region stays small (≤128KB).
  `g_srcdram` is resized to `MF_VTX_REGION + 32MB`.
- **LRU eviction**: when `blt_upload` fails for lack of contiguous space (or the
  entry table is full), free the least-recently-used cached entry
  (`blt_emitter_free`) and retry the upload. A working set larger than the heap
  therefore degrades to occasional re-upload on miss, rather than dropping the
  current draw. (Today's behavior — dropping the draw with a log — is retained
  only as the final fallback if even a single page cannot fit after eviction.)
- Static/file-scope arrays; safe as process RAM on both host and device.

## Data flow (per draw, mfgpu path)

1. Decode (unchanged) yields `BVtx[]`, `RTexture`, `RBlend`, and `tex_key`.
2. FBO / non-default target or `RB_PREMULT` → SW fallback (unchanged).
3. `stage_texture(tex_key, tex)` → resident `ref` (hit: reuse; miss: upload once,
   maybe evict LRU).
4. Convert + `blt_push_tris` the vertices (per-frame vtx buffer, unchanged).
5. Colorkey-aware blend selection (unchanged), `blt_trilist(ref, …)`.
6. `present` → `blt_execute` composites the accumulated ring into `g_fb565`
   (unchanged); main.cpp scans it out (unchanged).

The **vertex/command** path stays fully per-frame; only **texture staging**
becomes persistent.

## Error handling

- `stage_texture` miss with no room even after evicting all evictable entries:
  log `backend_mfgpu: texture NxM cannot fit heap after eviction - draw dropped`
  and drop the draw (final fallback, matches today's guard semantics).
- `blt_upload` `.valid == 0` / emitter overflow: existing per-draw logging is
  retained; a dropped texture upload drops the draw (never renders garbage).
- Invalidation of an unknown id: no-op.
- `tex_key == 0` untextured page failing to stage: fall back to the current
  inline 1×1 white upload for that draw.

## Testing (host TDD against the SW oracle)

Extend `raster_backend_test.cpp` (existing 8/8 TRILIST battery + colorkey cases
must stay green). New cases, each rendered two ways (`backend_sw` → RGB565 vs
`backend_mfgpu` → `blt_execute` → `RasterBackend_MFGPU_TestCopyFB565`) and
asserted within ±1 LSB in 5/6/5, into the 288×216 / fabric target:

1. **Large page not dropped** — a large texture (representative `1024×1024` to
   keep the test fast; a `2048×2048` smoke case if runtime permits) renders and
   matches the oracle. (Today: dropped.)
2. **Cache hit = single upload** — same `tex_key` drawn across two "frames";
   assert the page is uploaded exactly once (upload counter or `heap_used`
   delta) and both frames match the oracle.
3. **Invalidation re-stages** — draw key K, `RasterBackend_MFGPU_InvalidateTex(K)`,
   draw K again with changed pixels; assert re-upload and correct pixels.
4. **Two distinct keys coexist** — two textures resident simultaneously; both
   draws correct, heap holds both.
5. **Eviction correctness** — with a deliberately small heap (test hook to shrink
   capacity), a working set exceeding it evicts LRU and every draw still matches
   the oracle (via re-upload on miss).

A small test-only hook may expose the upload counter and (for case 5) a reduced
heap capacity; guarded so no device path uses it (mirrors the existing
`RasterBackend_MFGPU_TestCopyFB565` / `SetDefaultSurface` test hooks).

## Build / device verification

- Host: `make -f Makefile.gmloader raster-backend-test` → all cases pass.
- armhf: Docker cross-build (CLAUDE.md recipe) → clean link.
- Deploy to `192.168.20.81`; run Maldita with
  `GMLOADER_BLITTER=2 GMLOADER_RASTER=mfgpu`. Success criteria:
  - the `2048²` draws **no longer** log "exceeds scratch / upload overflow";
  - fabric-rendered sprites appear via the screenshot API (`:8182`), even within
    the current ~1–2s pre-crash window;
  - `backend_sw` default path remains byte-identical (regression check).
- Record findings in `.superpowers/sdd/progress.md`.

## Out of scope (sequenced follow-ups)

- **Blocker (a) — crash stub.** Diagnose which GL fn resolves to null after the
  `1024×2048` upload (the `resolve_thunked` / `eglGetProcAddress` NULL-logging
  hooks documented in CLAUDE.md), stub it to a no-op (Task-1
  `GetDefaultFrameBuffer` pattern) to extend runs past ~24 draws, then re-verify
  the fabric visual on the longer run. **This is the immediate next session
  after staging lands.**
- **Option B — SDRAM whole-quest residency for TRILIST.** Extend `blt_tri` +
  RTL to read triangle sources from the 64MB-SDRAM perm region; scales beyond a
  single-scene working set to whole-quest atlases. Larger, cross-repo
  (touches the vendored `mfgpu` engine + bit-exactness re-proof). The
  `stage_texture()` funnel is the intended integration point.
- **Per-texel (soft) alpha** for general GM games — future RTL item, unchanged
  by this work.

## Success criteria

1. Host battery (existing + 5 new cases) green; SW-equivalence regression
   unchanged.
2. armhf clean link.
3. On device: Maldita's large sprite atlases render on the fabric path (draws no
   longer dropped), verified via screenshot; SW path unregressed.
