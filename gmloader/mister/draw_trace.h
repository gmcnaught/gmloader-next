//
//  Draw-stream tracer — gmloader MiSTer (Phase-0 blitter recon)
//
//  Decomposes each frame's budget to decide whether a specialized 2D blitter
//  is worth building, and what its ceiling is:
//    - software-render ms : wall time spent INSIDE glDraw* (softpipe/llvmpipe
//                           execute synchronously, so this is rasterization time)
//    - game-logic ms      : RunnerJNILib::Process() time minus render time
//                           (the GameMaker VM floor — not movable by a blitter)
//    - capture ms         : glReadPixels + flip + RGBA->565 + DDR write
//  plus draw-call and vertex counts (batching quality) and a non-GL_TRIANGLES
//  draw count (primitives the blitter would need to handle beyond quads).
//
//  Enabled with env GMLOADER_DRAW_TRACE=1. Zero overhead when disabled.
//
#pragma once

#ifdef MISTER_NATIVE_VIDEO

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     DrawTrace_Init(void);                 // read env once at startup
int      DrawTrace_Enabled(void);              // hot-path gate (cached)
uint64_t DrawTrace_NowNs(void);                // CLOCK_MONOTONIC nanoseconds

// Called from the glDraw* hooks with the measured draw time.
void     DrawTrace_RecordDraw(uint64_t draw_ns, int vert_count, int is_triangles);

// glClear timing (movable GL overhead) and viewport size (render resolution):
// these split the "logic" bucket and reveal whether the game renders to an
// oversized surface.
void     DrawTrace_RecordClear(uint64_t clear_ns);
void     DrawTrace_RecordViewport(int w, int h);

// Called once per loop iteration with the Process() and capture wall times.
void     DrawTrace_FrameEnd(uint64_t process_ns, uint64_t capture_ns);

#ifdef __cplusplus
}
#endif

#endif // MISTER_NATIVE_VIDEO
