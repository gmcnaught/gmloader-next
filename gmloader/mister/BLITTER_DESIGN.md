# MiSTer 2D Software Blitter — Design (Phase 1)

## Goal
Replace the general softpipe/llvmpipe pipeline for the common GameMaker draw
(stock shader, textured quads, alpha blend) with a specialized NEON software
blitter writing straight to our 320×240 framebuffer. Target: locked 30–60fps for
2D games on the dual Cortex-A9.

Profiling (`fps-profiling-shaders` memory) shows software **texture sampling**
dominates the frame (~57%), with a ~43% non-shader floor (logic + readback). A
general 3D rasterizer carries triangle-setup/state overhead for what is really
sprite blitting; a purpose-built 2D blitter avoids it.

## Non-goals
- Custom game shaders, 3D, exotic blend equations → **fall back to GL** (llvmpipe).
- Pixel-perfect parity with Mesa. Visual correctness for the 2D subset only.

## Key insight: a closed software system
The shader dump found **only GameMaker's stock shaders** (no custom shaders) and
the runner uses FBOs (`application_surface`). So the blitter can be **self-contained**:
- Every GL texture → a CPU-side pixel copy we own.
- Every FBO color attachment → that same CPU texture, used as a render target.
- The default framebuffer (id 0) → our 320×240 output buffer.
- Every stock-shader draw → blit into the currently-bound render target.

Mesa is then only the **fallback** path for draws we can't handle. If a game uses
zero custom shaders, the blitter never touches Mesa for rendering.

## GL state we must shadow
To rasterize ourselves we mirror the subset of GL state a stock draw consumes:

| GL state | Intercepts | Why |
|---|---|---|
| Textures (pixels) | `glTexImage2D`, `glTexSubImage2D`, `glCompressedTexImage2D`, `glBindTexture`, `glDeleteTextures`, `glGenTextures` | Sample in software; also serve as FBO render targets |
| Texture params | `glTexParameteri` | Filter (nearest/linear), wrap mode |
| Vertex buffers | `glGenBuffers`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glDeleteBuffers` | Read vertex data at draw time |
| Vertex format | `glVertexAttribPointer`, `glEnableVertexAttribArray`, VAO binds | Decode position/uv/colour per vertex (offset/stride/type) |
| Render target | `glGenFramebuffers`, `glBindFramebuffer`, `glFramebufferTexture2D` | Direct blits to backbuffer vs an off-screen surface |
| Blend | `glEnable/Disable(GL_BLEND)`, `glBlendFunc(Separate)`, `glBlendEquation` | Compositing mode |
| Program | `glUseProgram` (+ link/attach tracking) | Decide stock-shader fast path vs fallback |
| MVP uniform | `glUniformMatrix4fv` (gm_Matrices[4] = WORLD_VIEW_PROJECTION) | Transform verts to clip/screen space |
| Alpha test | `glUniform*` for gm_AlphaTestEnabled / gm_AlphaRefValue | Honor `discard` threshold |
| Viewport/scissor | `glViewport`, `glScissor` | NDC→pixel mapping + clip rect |
| Clear | `glClear`, `glClearColor` | Clear the bound render target |

The draw hooks already exist (`GLDrawArrays_trace`/`GLDrawElements_trace` in
gles2.cpp) — they become the blitter dispatch point.

## Render-target model
- A `SoftSurface { uint8_t* rgba; int w, h; }` per render target.
- Default framebuffer (0) → the 320×240 output (our existing capture buffer; on
  flip it goes through RGBA→565 → DDR, same as today).
- Each FBO → the `SoftSurface` aliasing its attached texture's CPU pixels.
- `glBindFramebuffer` selects the current `SoftSurface`; all blits/clears target it.
- When a later draw samples an FBO's texture, it samples that same CPU buffer —
  render-to-texture "just works" with no GL readback.

## Per-draw fast-path classification
A draw is blitter-eligible iff ALL hold (else call `glad_glDraw*` = GL fallback):
1. Current program is a known **stock** shader program.
2. Primitive mode is `GL_TRIANGLES` (quads = 2 tris; trace will confirm strips/fans rare).
3. Vertex format matches the stock layout (pos vec3, uv vec2, colour ubyte4/float4).
4. Blend is disabled or a supported factor pair (e.g. SRC_ALPHA/ONE_MINUS_SRC_ALPHA,
   ONE/ONE additive, premultiplied).
5. Bound texture has a CPU copy (it will, if we shadowed all uploads).

## Rasterization pipeline (per eligible draw)
1. Fetch indices (DrawElements) / sequential range (DrawArrays) from shadowed buffers.
2. For each vertex: read pos/uv/colour, transform pos by the shadowed MVP, do the
   perspective divide (trivial/ortho for 2D), map NDC→pixel via viewport.
3. Assemble triangles; cull degenerate; clip to scissor/surface bounds.
4. Scanline-rasterize each triangle: interpolate uv + colour; **nearest** texture
   sample (pixel-art correct, 1 tap); multiply by vertex colour; alpha-test discard;
   blend into the target `SoftSurface`.
5. Inner loop is the NEON target: process pixels in quads; precompute fixed-point uv
   steps; specialize the common axis-aligned-quad case (no rotation → pure copy/blend
   with a constant uv step, skipping per-pixel edge math).

## Compositing & draw order
Because the blitter and GL fallback may both target the *same* surface, order
matters. Strategy, easiest→safest:
- **A. Blit-everything (expected):** if the trace shows ~0 custom-shader draws, the
  blitter handles all draws; Mesa is unused for rendering. No interleaving problem.
- **B. Per-surface ownership:** if fallback is needed, a surface touched by GL in a
  frame must be fully GL for that frame (and vice-versa) — chosen per surface, not
  per draw. Avoids mid-surface interleave.
- **C. Last resort:** GL-render the whole frame (status quo) when any
  unsupported draw appears.
The trace's custom-shader rate picks A vs B.

## Phasing
- **P0 (done, pending build):** draw-stream tracer — draws/verts, render/logic/capture
  split, non-triangle count, custom-shader rate. Gates the whole effort.
- **P1:** this design.
- **P2 PoC:** shadow textures+buffers+MVP+blend; software-rasterize the
  axis-aligned-quad case to the default framebuffer only; verify visuals on a simple
  scene; measure fps vs llvmpipe. Fall back to GL for everything else.
- **P3:** FBO/render-target support, rotated/scaled quads, full blend set, scissor,
  NEON inner loop, robust fallback. Per-game opt-in via config.

## Open questions the P0 trace must answer
1. Render-ms vs logic-ms split → is the ceiling high enough to bother? (gate)
2. Draws/frame and verts/frame → batching; sets blitter call overhead budget.
3. Custom-shader draw rate → compositing strategy A vs B.
4. Non-`GL_TRIANGLES` primitive rate → how much beyond quads to support.
5. Distinct blend modes in use → size of the blend matrix.

## Why this can reach 30fps where llvmpipe might not
At 320×240 the entire frame is 76,800 pixels. A NEON nearest-sample + alpha-blend
inner loop runs at hundreds of Mpix/s per core; even with heavy overdraw, blitting
a few hundred K textured pixels/frame across two cores is a sub-10ms job — the same
ballpark as 2D-heavy emulators that run full-speed on this SoC. The general GL
pipeline can't get there because its per-fragment path is generic; a specialized
2D blit is the standard way to win this on hardware without a GPU.
