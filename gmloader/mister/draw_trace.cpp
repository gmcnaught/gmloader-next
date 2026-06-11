//
//  Draw-stream tracer — gmloader MiSTer (Phase-0 blitter recon). See draw_trace.h.
//
#ifdef MISTER_NATIVE_VIDEO

#include "draw_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int      g_enabled = 0;
static uint64_t g_frame   = 0;

// Per-frame accumulators (reset in DrawTrace_FrameEnd).
static uint32_t f_draws    = 0;   // glDraw* calls this frame
static uint64_t f_verts    = 0;   // vertices/indices submitted this frame
static uint64_t f_draw_ns  = 0;   // wall time spent inside glDraw* this frame
static uint32_t f_nontri   = 0;   // draws with a primitive mode != GL_TRIANGLES
static uint64_t f_clear_ns = 0;   // wall time spent inside glClear this frame
static uint32_t f_clears   = 0;   // glClear calls this frame
static int      f_vp_w     = 0;   // largest viewport seen this frame (render res)
static int      f_vp_h     = 0;

uint64_t DrawTrace_NowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void DrawTrace_Init(void)
{
    const char *e = getenv("GMLOADER_DRAW_TRACE");
    g_enabled = (e && *e != '\0') ? 1 : 0;
    if (g_enabled)
        fprintf(stderr, "DRAWTRACE enabled\n");
}

int DrawTrace_Enabled(void) { return g_enabled; }

void DrawTrace_RecordDraw(uint64_t draw_ns, int vert_count, int is_triangles)
{
    f_draws++;
    if (vert_count > 0) f_verts += (uint64_t)vert_count;
    f_draw_ns += draw_ns;
    if (!is_triangles) f_nontri++;
}

void DrawTrace_RecordClear(uint64_t clear_ns)
{
    f_clears++;
    f_clear_ns += clear_ns;
}

void DrawTrace_RecordViewport(int w, int h)
{
    if ((long)w * h > (long)f_vp_w * f_vp_h) { f_vp_w = w; f_vp_h = h; }
}

void DrawTrace_FrameEnd(uint64_t process_ns, uint64_t capture_ns)
{
    if (!g_enabled) return;
    g_frame++;

    // process_ns covers game logic + draw submission + synchronous raster.
    // The draw time IS the software-render time; the remainder is the GM VM.
    uint64_t logic_ns = (process_ns > f_draw_ns) ? (process_ns - f_draw_ns) : 0;

    // Sample every 30 frames to keep the log readable.
    if (g_frame % 30 == 0) {
        fprintf(stderr,
            "DRAWTRACE f=%llu draws=%u verts=%llu vp=%dx%d | render=%.1fms "
            "logic=%.1fms (clear=%.1fms x%u) capture=%.1fms frame=%.1fms nonTRI=%u\n",
            (unsigned long long)g_frame, f_draws, (unsigned long long)f_verts,
            f_vp_w, f_vp_h,
            f_draw_ns / 1e6, logic_ns / 1e6, f_clear_ns / 1e6, f_clears,
            capture_ns / 1e6, (process_ns + capture_ns) / 1e6, f_nontri);
    }

    f_draws = 0;
    f_verts = 0;
    f_draw_ns = 0;
    f_nontri = 0;
    f_clear_ns = 0;
    f_clears = 0;
    f_vp_w = 0;
    f_vp_h = 0;
}

#endif // MISTER_NATIVE_VIDEO
