# MFGPU Persistent Atlas Staging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `backend_mfgpu` stage each unique GM texture page once into a
persistent DDR-heap cache keyed by GL texture id, so Maldita's large sprite
atlases (`2048²`, 8MB RGB565) render on the fabric path instead of being dropped
for exceeding the ~1MB per-frame heap.

**Architecture:** Extend the `RasterBackend.draw` seam with a `tex_key` (the
bound GL texture id the decode layer already holds). Beneath the seam,
`backend_mfgpu` gains a fixed-size texture cache resident across frames, funneled
through one `stage_texture()` helper: cache hit reuses the staged surface, miss
uploads once. Textures are invalidated on GL re-upload/delete, and evicted LRU
under heap pressure. `backend_sw` ignores `tex_key` (byte-identical). All logic
is validated on the host against the SW oracle before device deploy.

**Tech Stack:** C11 + C++17, `gmloader/mister/*`, vendored `3rdparty/mfgpu`
(`refmodel/`, `host/`), armhf cross-build via `gmloader-armhf-build:bullseye`
Docker image, MiSTer DE10-Nano at `192.168.20.81`.

## Global Constraints

- **Branch:** all work on `mister-sdl-buffer-output` (base `c749413`). Never
  `master` — it lacks MiSTer integration and will not build.
- **Decode layer is off-limits:** do NOT change GL state shadowing
  (`Blitter_On*` bodies beyond the two documented one-line invalidation calls),
  vertex/MVP decode, texture/blend resolution, or `gles2.cpp` wiring. Adding the
  `tex_key` parameter to the seam and the two `RasterBackend_MFGPU_InvalidateTex`
  calls is the only permitted touch to `blitter.cpp`.
- **`backend_sw` stays byte-identical.** The sw-equivalence regression case in
  `raster_backend_test.cpp` must keep passing unchanged (it ignores `tex_key`).
- **Cross-backend validation is present-space RGB565 with ±1 LSB tolerance**
  (`rgb565_within1`), not bit-exact — the fabric composites in 565, the SW
  surface is RGBA8888.
- **`blt_execute` runs in-process** on both host and device; `g_srcdram` is a
  static array, not an mmap'd FPGA region. Growing it is a `.bss` bump.
- **C ABI surface:** the seam header and all mfgpu host hooks are `extern "C"`.
- **Host test after every task:** `make -f Makefile.gmloader raster-backend-test`
  must print all-pass. armhf link is validated in Task 5 via the CLAUDE.md
  Docker recipe. Artifact:
  `build/arm-linux-gnueabihf/gmloader/gmloadernext.armhf`.

## Fixed interfaces (already in the tree — do not redefine)

From `3rdparty/mfgpu/host/blt_emitter.h`:
```c
typedef struct { uint32_t off; uint16_t stride, w, h; uint8_t format; int valid; uint32_t size; uint32_t sdram_off; } blt_surface_ref_t;
blt_surface_ref_t blt_upload(blt_emitter_t*, const uint16_t *pixels, int w, int h, int pitch); /* .valid==0 + e->overflow on OOM */
void blt_emitter_free(blt_emitter_t*, uint32_t off, uint32_t size);   /* free one upload's heap block */
void blt_begin_frame(blt_emitter_t*, int target_buf, int clear, uint16_t clear_color); /* resets cmd list + vtx cursor */
void blt_alloc_init(blt_alloc_t*, uint32_t base_off, uint32_t size);
```
From `raster_backend_convert.h`: `blt_vtx_t bvtx_to_blt(const BVtx*, int tex_w, int tex_h);` and `uint8_t rblend_to_blt(RBlend);`.
`blt_surface_ref_t` carries `.w`/`.h` (the staged page dims) — use them for `bvtx_to_blt`.

## File structure

- Modify `gmloader/mister/raster_backend.h` — add `tex_key` to the `draw`
  vtable; declare `RasterBackend_MFGPU_InvalidateTex`.
- Modify `gmloader/mister/raster_backend_sw.cpp` — `sw_draw` gains + ignores
  `tex_key`.
- Modify `gmloader/mister/raster_backend_mfgpu.cpp` — the persistent cache,
  `stage_texture()`, invalidation, eviction, buffer resize, per-frame lifecycle,
  and test hooks. This is the bulk of the work.
- Modify `gmloader/mister/blitter.cpp` — pass `g_boundTex2D` at the draw site;
  add two invalidation calls.
- Modify `gmloader/mister/raster_backend_test.cpp` — pass keys at existing draw
  sites; add the persistence/invalidation/eviction battery + test-hook decls.
- Modify `.superpowers/sdd/progress.md` — Task 5 findings.

---

## Task 1: Extend the `draw` seam with `tex_key` (pure plumbing, no behavior change)

**Files:**
- Modify: `gmloader/mister/raster_backend.h` (vtable `draw` signature)
- Modify: `gmloader/mister/raster_backend_sw.cpp` (`sw_draw` param, ignored)
- Modify: `gmloader/mister/raster_backend_mfgpu.cpp` (`mf_draw` param, ignored this task)
- Modify: `gmloader/mister/blitter.cpp:520` (pass `g_boundTex2D`)
- Modify: `gmloader/mister/raster_backend_test.cpp` (4 draw call sites: `:101`, `:141`, `:146`, `:339`)

**Interfaces:**
- Produces: `draw(RSurface*, const BVtx*, int triCount, const RTexture*, RBlend, float alphaRef, uint32_t tex_key)` — the extended vtable signature every later task consumes.

- [ ] **Step 1: Extend the vtable signature** — in `raster_backend.h`, change the
  `draw` field to:

```c
    void (*draw)(RSurface *dst, const BVtx *verts, int triCount,
                 const RTexture *tex, RBlend blend, float alphaRef,
                 uint32_t tex_key);
```

- [ ] **Step 2: Thread it through `backend_sw`** — in `raster_backend_sw.cpp`,
  change `sw_draw` to take and ignore the key (behavior byte-identical):

```c
static void sw_draw(RSurface *d, const BVtx *v, int n, const RTexture *t, RBlend bl, float ar, uint32_t tex_key) {
    (void)tex_key;   // SW rasterizer keys nothing on texture identity
    Blitter_RasterDraw(d, v, n, t, bl, ar, sw_threads);
}
```

- [ ] **Step 3: Thread it through `backend_mfgpu` (still ignored this task)** — in
  `raster_backend_mfgpu.cpp`, change `mf_draw`'s signature to append
  `, uint32_t tex_key` and add `(void)tex_key;` as its first line. Do NOT change
  its body yet. Also update the internal SW-fallback calls inside `mf_draw`
  (`backend_sw.draw(d, v, triCount, t, bl, ar);`) to pass the key through:
  `backend_sw.draw(d, v, triCount, t, bl, ar, tex_key);` (two sites: the FBO
  fallback and the `RB_PREMULT` fallback).

- [ ] **Step 4: Pass the real key at the decode site** — in `blitter.cpp:520`,
  change:

```c
                    RasterBackend_Select()->draw(&rt, &s_verts[0], count / 3, &tex, blend, 0.0f, g_boundTex2D);
```

- [ ] **Step 5: Update the test call sites** — in `raster_backend_test.cpp`, add a
  trailing key argument to every `draw` call. Add a tiny monotonic key helper
  near the top of the file (after the includes):

```c
static uint32_t next_key(void){ static uint32_t k = 1; return k++; }
```

  The main battery helper is `battery_case(...)` at `:132` (renders a case both
  ways and returns `compare565(...)`). Factor it so cases can pin the cache key —
  extract the body into `battery_case_key(..., uint32_t key)` that threads `key`
  into **both** `backend_sw.draw` and `backend_mfgpu.draw`, and keep
  `battery_case(...)` as a wrapper that passes `next_key()`:

```c
static int battery_case_key(const char *name, uint8_t br, uint8_t bg, uint8_t bb,
                            const RTexture *tex, const BVtx *v, int triCount,
                            RBlend blend, uint32_t key) {
    static uint8_t  rgba_sw[BW*BH*4];
    static uint8_t  rgba_mf[BW*BH*4];
    static uint16_t mf565[BW*BH];
    RSurface s_sw = { rgba_sw, BW, BH };
    RSurface s_mf = { rgba_mf, BW, BH };
    backend_sw.clear(&s_sw, br, bg, bb, 255);
    backend_sw.draw(&s_sw, v, triCount, tex, blend, 0.f, key);
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    backend_mfgpu.frame_begin();
    backend_mfgpu.clear(&s_mf, br, bg, bb, 255);
    backend_mfgpu.draw(&s_mf, v, triCount, tex, blend, 0.f, key);
    backend_mfgpu.frame_end();
    RasterBackend_MFGPU_TestCopyFB565(BW, BH, mf565);
    return compare565(name, &s_sw, mf565);
}
static int battery_case(const char *name, uint8_t br, uint8_t bg, uint8_t bb,
                        const RTexture *tex, const BVtx *v, int triCount, RBlend blend) {
    return battery_case_key(name, br, bg, bb, tex, v, triCount, blend, next_key());
}
```

  Then add the trailing key to the two remaining standalone `draw` sites:
  - `:101` `RasterBackend_Select()->draw(&sb, v, 1, &t, RB_NONE, 0.f, next_key());`
  - `:339` `backend_mfgpu.draw(&s_fbo, v, 1, &untex, RB_NONE, 0.f, next_key());`

  (The `:141`/`:146` pair are now inside `battery_case_key` above.) Every existing
  battery case still calls `battery_case(...)` → a fresh key each → no cross-case
  cache collision once Task 2 adds caching.

- [ ] **Step 6: Build the host test — verify no behavior change** — Run:
  `make -f Makefile.gmloader raster-backend-test`
  Expected: the full existing battery still prints all-pass (sw-equivalence,
  clear-parity, 8/8 TRILIST, colorkey cases, FBO fallback). `tex_key` is inert.

- [ ] **Step 7: Commit**

```bash
git add gmloader/mister/raster_backend.h gmloader/mister/raster_backend_sw.cpp \
        gmloader/mister/raster_backend_mfgpu.cpp gmloader/mister/blitter.cpp \
        gmloader/mister/raster_backend_test.cpp
git commit -m "refactor(blitter): thread tex_key through RasterBackend.draw (no behavior change)"
```

---

## Task 2: Persistent identity-keyed texture cache + `stage_texture()` + resize + persist across frames

**Files:**
- Modify: `gmloader/mister/raster_backend_mfgpu.cpp` (cache, `stage_texture`, buffer sizes, `mf_frame_begin`, test hooks)
- Modify: `gmloader/mister/raster_backend_test.cpp` (test-hook decls + two new cases)

**Interfaces:**
- Consumes: `blt_upload`, `blt_begin_frame`, `blt_alloc_init`, `blt_surface_ref_t`, `bvtx_to_blt`, `rblend_to_blt`, extended `draw` (Task 1).
- Produces (host test hooks, `extern "C"`):
  - `uint32_t RasterBackend_MFGPU_TestUploadCount(void)` — count of real `blt_upload`s since last reinit.
  - `void RasterBackend_MFGPU_TestReinit(uint32_t tex_heap_bytes)` — reset backend + cache + counter; `tex_heap_bytes==0` uses the full heap, else caps the texture allocator to `tex_heap_bytes` (for the Task 4 eviction test).
- Produces (internal, consumed by Tasks 3–4):
  - `static blt_surface_ref_t stage_texture(uint32_t key, const RTexture *t, bool *out_has_key)`.
  - `static bool evict_one_lru(void)` (stubbed here to `return false;`; real body in Task 4).

- [ ] **Step 1: Write the failing tests** — in `raster_backend_test.cpp`, add the
  test-hook declarations next to the existing mfgpu externs (after `:35`):

```c
extern "C" uint32_t RasterBackend_MFGPU_TestUploadCount(void);
extern "C" void RasterBackend_MFGPU_TestReinit(uint32_t tex_heap_bytes);
```

  Then add two cases. **(A) large page renders (not dropped) and matches the
  oracle** — via `battery_case_key` (Task 1). Build a `256×256` opaque RGBA8888
  texture with a per-texel gradient and a full-target quad (2 tris) over it:

```c
static int case_large_page(void) {
    enum { TW = 256, TH = 256 };
    static uint8_t tex[TW*TH*4];
    for (int y=0;y<TH;y++) for (int x=0;x<TW;x++){ uint8_t*p=tex+((y*TW+x)*4); p[0]=(uint8_t)x; p[1]=(uint8_t)y; p[2]=128; p[3]=255; }
    RTexture t = { tex, TW, TH, 1, 1, /*RGBA8888*/0, 1 };
    BVtx v[6] = {
        {  0.f,  0.f, 0,0, 1,1,1,1 }, { 240.f,  0.f, 1,0, 1,1,1,1 }, {  0.f,180.f, 0,1, 1,1,1,1 },
        { 240.f,  0.f, 1,0, 1,1,1,1 }, { 240.f,180.f, 1,1, 1,1,1,1 }, {  0.f,180.f, 0,1, 1,1,1,1 },
    };
    RasterBackend_MFGPU_TestReinit(0);
    return battery_case_key("large_page", 0,0,0, &t, v, 2, RB_NONE, next_key());  // ±1 LSB
}
```

  **(B) cache hit = single upload** — same key drawn across two frames uploads
  exactly once. This case needs direct frame control (two frames, one key), so it
  sets up its own default surface exactly as `battery_case_key` does:

```c
static int case_cache_hit(void) {
    static const uint8_t px[4] = { 200,100,50,255 };
    RTexture t = { px, 1, 1, 1, 1, 0, 1 };
    BVtx v[3] = { {2,2,0,0,1,1,1,1}, {28,4,1,0,1,1,1,1}, {4,28,0,1,1,1,1,1} };
    static uint8_t rgba_mf[BW*BH*4];
    RSurface s_mf = { rgba_mf, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    RasterBackend_MFGPU_TestReinit(0);
    const uint32_t K = 4242;
    for (int f=0; f<2; f++) {                 // two frames, same key
        backend_mfgpu.frame_begin();
        backend_mfgpu.clear(&s_mf, 0,0,0,255);
        backend_mfgpu.draw(&s_mf, v, 1, &t, RB_NONE, 0.f, K);
        backend_mfgpu.frame_end();
    }
    return RasterBackend_MFGPU_TestUploadCount() == 1;   // uploaded once, reused once
}
```

  Note: `RasterBackend_MFGPU_TestReinit` clears the cache/counter but not the
  `SetDefaultSurface` pointer, so set the default surface once before the loop
  (as above). Wire both new cases into `main()` (`:348`) with a printed label.

- [ ] **Step 2: Run to verify they fail** — Run:
  `make -f Makefile.gmloader raster-backend-test`
  Expected: **compile/link error** — `RasterBackend_MFGPU_TestUploadCount` /
  `RasterBackend_MFGPU_TestReinit` are undefined. (After Step 3 they link; case
  (B) would still fail because today every draw re-uploads.)

- [ ] **Step 3: Resize the backing buffers** — in `raster_backend_mfgpu.cpp`,
  replace the `enum { MF_RING_CAP ... }` block and the `g_srcdram` / `g_texscratch`
  declarations with:

```c
enum {
    MF_RING_CAP    = 64 * 1024,
    MF_VTX_REGION  = 128 * 1024,                 // per-frame TRILIST vertex buffer
    MF_TEX_HEAP    = 32u * 1024 * 1024,          // persistent texture pages (32MB)
    MF_SRCDRAM_CAP = MF_VTX_REGION + MF_TEX_HEAP,
    MF_MAX_CMDS    = MF_RING_CAP / BLT_CMD_BYTES,
    MF_MAX_VERTS   = 8192,
    MF_TEX_TEXELS  = 2048 * 2048,                // scratch = largest single page (8MB)
    MF_TEX_CACHE_N = 64,                         // resident-page table size
};
static uint8_t       g_ring[MF_RING_CAP];
static uint8_t       g_srcdram[MF_SRCDRAM_CAP];
static blt_emitter_t g_e;
static blt_cmd_t     g_cmds[MF_MAX_CMDS];
static blt_vtx_t     g_vtxscratch[MF_MAX_VERTS];
static uint16_t      g_texscratch[MF_TEX_TEXELS];
static bool          g_inited = false;
```

- [ ] **Step 4: Add the cache state + helpers** — after the `g_inited`/
  `g_frame_active` declarations, add:

```c
// Persistent resident-texture cache. Keyed by GL texture id (tex_key). Survives
// across frames (mf_frame_begin no longer resets the heap); entries are freed on
// GL invalidate/delete (Task 3) or LRU eviction under heap pressure (Task 4).
struct MfTexEntry { uint32_t key; bool used; bool has_key; blt_surface_ref_t ref; uint64_t lru; };
static MfTexEntry g_texcache[MF_TEX_CACHE_N];
static uint64_t   g_lru_clock   = 0;
static uint32_t   g_upload_count = 0;   // real blt_upload calls since reinit (test hook)
static uint32_t   g_tex_heap_cap = 0;   // 0 => full MF_TEX_HEAP; else test override

// Free the least-recently-used cached entry. Returns false if none to evict.
// Real body lands in Task 4; a stub here keeps stage_texture compiling.
static bool evict_one_lru(void);
```

- [ ] **Step 5: Rewrite `mf_init_once` to honor the heap cap and clear the cache** —
  replace `mf_init_once` with:

```c
static void mf_init_once(void) {
    if (g_inited) return;
    blt_emitter_init(&g_e, g_ring, sizeof g_ring, g_srcdram, sizeof g_srcdram);
    uint32_t cap = g_tex_heap_cap ? g_tex_heap_cap : MF_TEX_HEAP;
    blt_alloc_init(&g_e.alloc, MF_VTX_REGION, cap);       // texture allocator (persistent)
    blt_vtx_buf_init(&g_e, g_srcdram, MF_VTX_REGION);     // per-frame vertex buffer
    for (int i = 0; i < MF_TEX_CACHE_N; i++) g_texcache[i].used = false;
    g_lru_clock = 0; g_upload_count = 0;
    g_inited = true;
}
```

- [ ] **Step 6: Make `mf_frame_begin` persistent** — remove the per-frame heap
  reset so staged textures survive. Replace `mf_frame_begin` with:

```c
static void mf_frame_begin(void) {
    mf_init_once();
    // Textures persist across frames now (cache in g_texcache). Only the vtx
    // cursor + command list reset here (blt_begin_frame). No blt_heap_reset.
    blt_begin_frame(&g_e, /*target_buf=*/0, /*clear=*/0, /*clear_color=*/0);
    g_frame_active = true;
}
```

- [ ] **Step 7: Add `stage_texture()` and the eviction stub** — add before
  `mf_draw`:

```c
static bool evict_one_lru(void) { return false; }   // Task 4 replaces this

// Stage a texture page keyed by identity. Cache hit reuses the resident surface
// (no re-upload); miss converts RGBA->RGB565 into g_texscratch and blt_uploads
// once. Returns a ref with .valid==0 if the page cannot fit even after eviction
// (caller drops the draw). *out_has_key := did any texel fold to the colorkey.
static blt_surface_ref_t stage_texture(uint32_t key, const RTexture *t, bool *out_has_key) {
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].key == key) {
            g_texcache[i].lru = ++g_lru_clock;
            *out_has_key = g_texcache[i].has_key;
            return g_texcache[i].ref;
        }
    // miss: convert into scratch
    int tw, th; bool textured = (t && t->valid && t->rgba);
    if (textured) { tw = t->w; th = t->h; } else { tw = 1; th = 1; }
    if (tw <= 0 || th <= 0 || (size_t)tw * th > MF_TEX_TEXELS) {
        blt_surface_ref_t bad; bad.valid = 0; *out_has_key = false; return bad;
    }
    bool has_key = false;
    if (textured) {
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                g_texscratch[(size_t)y * tw + x] = mf_texel565(t, x, y, &has_key);
    } else {
        g_texscratch[0] = 0xFFFF;   // 1x1 opaque white
    }
    blt_surface_ref_t ref = blt_upload(&g_e, g_texscratch, tw, th, tw * 2);
    while (!ref.valid && evict_one_lru()) ref = blt_upload(&g_e, g_texscratch, tw, th, tw * 2);
    if (!ref.valid) { *out_has_key = false; return ref; }
    g_upload_count++;
    // insert into a free slot, evicting the LRU slot if the table is full
    int slot = -1; uint64_t best = ~0ull;
    for (int i = 0; i < MF_TEX_CACHE_N; i++) {
        if (!g_texcache[i].used) { slot = i; break; }
        if (g_texcache[i].lru < best) { best = g_texcache[i].lru; slot = i; }
    }
    if (g_texcache[slot].used)
        blt_emitter_free(&g_e, g_texcache[slot].ref.off, g_texcache[slot].ref.size);
    g_texcache[slot] = MfTexEntry{ key, true, has_key, ref, ++g_lru_clock };
    *out_has_key = has_key;
    return ref;
}
```

- [ ] **Step 8: Route `mf_draw` through `stage_texture`** — replace the inline
  "stage the texture page as RGB565" block in `mf_draw` (from `int tw, th;` down
  to the `blt_upload`/`if (!tex.valid)` guard) with:

```c
    uint32_t stage_key = (t && t->valid && t->rgba) ? tex_key : 0u;   // all untextured share key 0
    bool has_key = false;
    blt_surface_ref_t tex = stage_texture(stage_key, t, &has_key);
    if (!tex.valid) {
        fprintf(stderr, "backend_mfgpu: texture cannot fit heap after eviction - draw dropped\n");
        return;
    }
    int tw = tex.w, th = tex.h;   // staged page dims (1x1 for untextured)
```

  Remove the now-dead local `has_key` re-declaration further down (the block that
  looped `mf_texel565` to compute `has_key` is gone — `stage_texture` returns it).
  Keep the vertex conversion, `blt_push_tris`, and the colorkey blend-selection
  logic exactly as-is (they already read `has_key`, `tw`, `th`, `min_vtx_a`).

- [ ] **Step 9: Add the test hooks** — near the other `extern "C"` host hooks at
  the bottom of the file:

```c
extern "C" uint32_t RasterBackend_MFGPU_TestUploadCount(void) { return g_upload_count; }
extern "C" void RasterBackend_MFGPU_TestReinit(uint32_t tex_heap_bytes) {
    g_inited = false; g_frame_active = false;
    g_tex_heap_cap = tex_heap_bytes;   // 0 => full heap
    mf_init_once();                    // re-wires emitter, clears cache + counter
}
```

- [ ] **Step 10: Build + verify** — Run:
  `make -f Makefile.gmloader raster-backend-test`
  Expected: all cases print pass, including `case_large_page` (±1 LSB) and
  `case_cache_hit` (upload count == 1). Existing 8/8 TRILIST + colorkey +
  sw-equivalence + FBO-fallback remain pass (each existing case now passes a
  `next_key()` so no cross-case cache collision).

- [ ] **Step 11: Commit**

```bash
git add gmloader/mister/raster_backend_mfgpu.cpp gmloader/mister/raster_backend_test.cpp
git commit -m "feat(blitter): persistent identity-keyed texture cache (stage once, reuse across frames); 32MB heap"
```

---

## Task 3: Invalidate cached textures on GL re-upload / delete

**Files:**
- Modify: `gmloader/mister/raster_backend.h` (declare the hook)
- Modify: `gmloader/mister/raster_backend_mfgpu.cpp` (implement `RasterBackend_MFGPU_InvalidateTex`)
- Modify: `gmloader/mister/blitter.cpp` (`Blitter_OnTexImage2D`, `Blitter_OnDeleteTexture`)
- Modify: `gmloader/mister/raster_backend_test.cpp` (invalidation case)

**Interfaces:**
- Consumes: `blt_emitter_free`, the cache (Task 2).
- Produces: `void RasterBackend_MFGPU_InvalidateTex(uint32_t id)` — frees the cached entry for `id` (no-op if absent / backend not inited), forcing a re-stage on next draw.

- [ ] **Step 1: Write the failing test** — in `raster_backend_test.cpp`, add:

```c
static int case_invalidate(void) {
    static uint8_t px[4] = { 10,20,30,255 };
    RTexture t = { px, 1, 1, 1, 1, 0, 1 };
    BVtx v[3] = { {2,2,0,0,1,1,1,1}, {28,4,1,0,1,1,1,1}, {4,28,0,1,1,1,1,1} };
    static uint8_t rgba_mf[BW*BH*4];
    RSurface s_mf = { rgba_mf, BW, BH };
    RasterBackend_MFGPU_SetDefaultSurface(rgba_mf);
    RasterBackend_MFGPU_TestReinit(0);
    const uint32_t K = 77;
    // frame 1: stage (upload #1)
    backend_mfgpu.frame_begin(); backend_mfgpu.draw(&s_mf, v, 1, &t, RB_NONE, 0.f, K); backend_mfgpu.frame_end();
    // invalidate + change pixels, frame 2: must re-upload (upload #2)
    RasterBackend_MFGPU_InvalidateTex(K);
    px[0] = 250;
    backend_mfgpu.frame_begin(); backend_mfgpu.draw(&s_mf, v, 1, &t, RB_NONE, 0.f, K); backend_mfgpu.frame_end();
    return RasterBackend_MFGPU_TestUploadCount() == 2;
}
```

  Add its extern next to the others:
  `extern "C" void RasterBackend_MFGPU_InvalidateTex(uint32_t id);`
  and wire `case_invalidate` into `main()`.

- [ ] **Step 2: Run to verify it fails** — Run:
  `make -f Makefile.gmloader raster-backend-test`
  Expected: link error (`RasterBackend_MFGPU_InvalidateTex` undefined).

- [ ] **Step 3: Implement the hook** — in `raster_backend_mfgpu.cpp`, add near the
  other host hooks:

```c
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
```

- [ ] **Step 4: Declare it in the seam header** — in `raster_backend.h`, inside the
  `extern "C"` block, add:

```c
/* mfgpu back-end only: drop the cached staging of GL texture `id` (on GL
 * re-upload/delete). No-op when backend_sw is selected or nothing is cached. */
void RasterBackend_MFGPU_InvalidateTex(uint32_t id);
```

- [ ] **Step 5: Call it from the decode hooks** — in `blitter.cpp`:
  - In `Blitter_OnTexImage2D`, after the `store_texture(...)` line:

```c
    RasterBackend_MFGPU_InvalidateTex(tex ? tex : g_boundTex2D);
```

  - In `Blitter_OnDeleteTexture`, after it removes the id from `g_textures` (keep
    the existing erase logic; add this call with the same `tex` argument):

```c
    RasterBackend_MFGPU_InvalidateTex(tex);
```

- [ ] **Step 6: Build + verify** — Run:
  `make -f Makefile.gmloader raster-backend-test`
  Expected: all pass, including `case_invalidate` (upload count == 2). Existing
  cases unaffected.

- [ ] **Step 7: Commit**

```bash
git add gmloader/mister/raster_backend.h gmloader/mister/raster_backend_mfgpu.cpp \
        gmloader/mister/blitter.cpp gmloader/mister/raster_backend_test.cpp
git commit -m "feat(blitter): invalidate cached texture staging on GL re-upload/delete"
```

---

## Task 4: LRU eviction under heap pressure

**Files:**
- Modify: `gmloader/mister/raster_backend_mfgpu.cpp` (`evict_one_lru` real body)
- Modify: `gmloader/mister/raster_backend_test.cpp` (eviction + two-keys-coexist cases)

**Interfaces:**
- Consumes: the cache + `blt_emitter_free` (Task 2), `RasterBackend_MFGPU_TestReinit(cap)`.
- Produces: `stage_texture` that, on `blt_upload` OOM, frees the LRU resident entry and retries (already wired to `evict_one_lru` in Task 2; this task gives it a body).

- [ ] **Step 1: Write the failing tests** — in `raster_backend_test.cpp` add:

```c
// Build a WxH opaque texture whose colour encodes `tag` so re-uploads are
// distinguishable, draw it full-quad, and compare fabric vs SW oracle.
static int draw_tagged(uint32_t key, int W, int H, uint8_t tag) {
    static uint8_t buf[512*512*4];
    for (int i=0;i<W*H;i++){ buf[i*4]=tag; buf[i*4+1]=tag; buf[i*4+2]=tag; buf[i*4+3]=255; }
    RTexture t = { buf, W, H, 1, 1, 0, 1 };
    BVtx v[6] = {
        {  0.f,  0.f, 0,0, 1,1,1,1 }, { 200.f,  0.f, 1,0, 1,1,1,1 }, {  0.f,150.f, 0,1, 1,1,1,1 },
        { 200.f,  0.f, 1,0, 1,1,1,1 }, { 200.f,150.f, 1,1, 1,1,1,1 }, {  0.f,150.f, 0,1, 1,1,1,1 },
    };
    return battery_case_key("tagged", 0,0,0, &t, v, 2, RB_NONE, key);   // Task 1 helper, key-aware
}

static int case_two_keys(void) {                 // both resident, both correct
    RasterBackend_MFGPU_TestReinit(0);
    if (!draw_tagged(1, 64, 64, 40)) return 0;
    if (!draw_tagged(2, 64, 64, 200)) return 0;
    // redraw key 1 — must be a cache hit (no third upload)
    if (!draw_tagged(1, 64, 64, 40)) return 0;
    return RasterBackend_MFGPU_TestUploadCount() == 2;
}

static int case_eviction(void) {                       // cap holds 2x 512KB pages (256KB slack)
    RasterBackend_MFGPU_TestReinit(1280u*1024);        // 1.25MB texture heap
    if (!draw_tagged(1, 512, 512, 10)) return 0;       // upload1 (free 768KB)
    if (!draw_tagged(2, 512, 512, 20)) return 0;       // upload2 (free 256KB)
    if (!draw_tagged(3, 512, 512, 30)) return 0;       // evicts key1, upload3 -> {2,3}
    if (!draw_tagged(4, 512, 512, 40)) return 0;       // evicts key2, upload4 -> {3,4}
    if (!draw_tagged(1, 512, 512, 10)) return 0;       // key1 gone -> evicts key3, upload5
    return RasterBackend_MFGPU_TestUploadCount() == 5; // 4 initial + 1 re-stage; each pixel-correct
}
```

  These reuse `battery_case_key` (added in Task 1) and the `TestReinit(cap)` /
  `TestUploadCount()` hooks (Task 2). `draw_tagged`'s `buf` is a single shared
  `512*512*4` static scratch, sized for the largest case here. Wire
  `case_two_keys` and `case_eviction` into `main()`.

- [ ] **Step 2: Run to verify it fails** — Run:
  `make -f Makefile.gmloader raster-backend-test`
  Expected: `case_eviction` fails — with `evict_one_lru` still a `return false`
  stub, the 4th `512×512` upload returns `.valid==0` and the draw is dropped
  (mismatch / wrong upload count).

- [ ] **Step 3: Implement `evict_one_lru`** — replace the stub:

```c
static bool evict_one_lru(void) {
    int victim = -1; uint64_t best = ~0ull;
    for (int i = 0; i < MF_TEX_CACHE_N; i++)
        if (g_texcache[i].used && g_texcache[i].lru < best) { best = g_texcache[i].lru; victim = i; }
    if (victim < 0) return false;
    blt_emitter_free(&g_e, g_texcache[victim].ref.off, g_texcache[victim].ref.size);
    g_texcache[victim].used = false;
    return true;
}
```

- [ ] **Step 4: Build + verify** — Run:
  `make -f Makefile.gmloader raster-backend-test`
  Expected: all pass, including `case_two_keys` (2 uploads) and `case_eviction`
  (5 uploads, all pixels correct).

- [ ] **Step 5: Commit**

```bash
git add gmloader/mister/raster_backend_mfgpu.cpp gmloader/mister/raster_backend_test.cpp
git commit -m "feat(blitter): LRU eviction of resident textures under heap pressure"
```

---

## Task 5: armhf build, device deploy, on-hardware verification

**Files:**
- Modify: `.superpowers/sdd/progress.md` (findings)

**Interfaces:**
- Consumes: everything above. No new symbols.

- [ ] **Step 1: armhf cross-build** — Run the CLAUDE.md Docker recipe:

```bash
/opt/homebrew/bin/docker run --rm -v "$(pwd):/src" -w /src gmloader-armhf-build:bullseye bash -c '
  touch thunks/thunk_gen_dyn.h
  make -f Makefile.gmloader ARCH=arm-linux-gnueabihf MISTER_BUILD=1 MISTER_NATIVE_VIDEO=1 \
    "LLVM_INC=/usr/arm-linux-gnueabihf/include /usr/arm-linux-gnueabihf/include/c++/10/arm-linux-gnueabihf" \
    -j$(nproc)'
```
  Expected: clean link → `build/arm-linux-gnueabihf/gmloader/gmloadernext.armhf`
  (the `.bss` grew ~40MB — expected).

- [ ] **Step 2: Back up + deploy** — the device binary may be running; kill it
  first (no `pgrep` on MiSTer):

```bash
ssh root@192.168.20.81 'ps w | grep -q "[g]mloader -c" && pkill -9 -f "gmloader -c"; \
  cp -n /media/fat/games/gmloader/gmloader /media/fat/games/gmloader/gmloader.pre-atlas.bak'
scp build/arm-linux-gnueabihf/gmloader/gmloadernext.armhf root@192.168.20.81:/media/fat/games/gmloader/gmloader
ssh root@192.168.20.81 'chmod +x /media/fat/games/gmloader/gmloader'
```

- [ ] **Step 3: Regression — SW path unchanged** — run the default (sw) backend
  and confirm it still renders as before (same draws, same pre-existing crash
  point):

```bash
ssh root@192.168.20.81 'cd /media/fat/games/gmloader && \
  LD_LIBRARY_PATH=/media/fat/games/gmloader/mesa:/media/fat/games/gmloader \
  ./gmloader -c gmloader.json 2>&1 | head -60'
```
  Expected: identical boot/draw log to the pre-atlas backup; no new errors from
  the seam/cache; `busybox devmem 0x3A000000` increments during the run.

- [ ] **Step 4: Fabric path — large atlases no longer dropped** — run the mfgpu
  backend and capture the log:

```bash
ssh root@192.168.20.81 'cd /media/fat/games/gmloader && \
  GMLOADER_BLITTER=2 GMLOADER_RASTER=mfgpu \
  LD_LIBRARY_PATH=/media/fat/games/gmloader/mesa:/media/fat/games/gmloader \
  ./gmloader -c gmloader.json 2>&1 | head -120'
```
  Expected: **no** `exceeds scratch` / `texture upload overflow` /
  `cannot fit heap` lines for the `2048×2048` and `1024×2048` pages (they were
  dropped before). The run still ends at the ~24-draw pre-existing crash
  (blocker (a), out of scope) — that is expected and unchanged.

- [ ] **Step 5: Visual confirmation via screenshot API** — while the mfgpu run is
  live (or scripted to hold before the crash), capture the fabric framebuffer:

```bash
# POST to request a capture, then GET the newest screenshot (mrext on :8182)
curl -s -X POST http://192.168.20.81:8182/api/screenshots >/dev/null
curl -s http://192.168.20.81:8182/api/screenshots | tail -c 400
```
  Expected: the captured frame shows fabric-rendered Maldita sprites (from the
  now-resident large atlases), not just the small-texture subset. Save/note the
  screenshot path for the ledger. (User eyeballs the image to confirm.)

- [ ] **Step 6: Record findings + commit** — append a dated section to
  `.superpowers/sdd/progress.md` summarizing: armhf link clean, SW regression
  pass, fabric large-atlas draws no longer dropped, screenshot result, and that
  blocker (a) crash remains the next task. Then:

```bash
git add .superpowers/sdd/progress.md
git commit -m "docs(sdd): persistent atlas staging on-hardware results"
```
  Then in `mister-gmloader`: bump the `external/gmloader-next` submodule pointer
  to this tip and commit.

---

## Self-review

- **Spec coverage:** §1 seam key → Task 1. §2 cache + `stage_texture` funnel →
  Task 2. §3 invalidation → Task 3. §4 per-frame lifecycle → Task 2 Step 6. §5
  sizing → Task 2 Step 3; eviction → Task 4. §6 tests → Tasks 2–4 (large-page,
  cache-hit, invalidation, two-keys, eviction). §7 device verification → Task 5.
  Out-of-scope crash stub is explicitly deferred.
- **Type consistency:** `draw(..., uint32_t tex_key)` is defined in Task 1 and
  used identically in Tasks 2–5; `stage_texture(uint32_t, const RTexture*, bool*)`,
  `evict_one_lru(void)->bool`, `RasterBackend_MFGPU_InvalidateTex(uint32_t)`,
  `RasterBackend_MFGPU_TestUploadCount(void)->uint32_t`,
  `RasterBackend_MFGPU_TestReinit(uint32_t)`, and `MfTexEntry` fields
  (`key/used/has_key/ref/lru`) match across every task. `blt_surface_ref_t.w/h`
  used for `bvtx_to_blt` matches the header.
- **No placeholders:** every code step shows real code; `default_surface()` /
  `draw_and_compare*` are explicitly defined as the existing `:140` helper
  pattern, not invented APIs.
- **Ordering:** `evict_one_lru` is introduced as a stub in Task 2 (so
  `stage_texture` compiles and the cache/hit tests pass without eviction) and
  given its real body in Task 4 under a dedicated failing test — each task
  compiles and its tests pass at its own boundary.
