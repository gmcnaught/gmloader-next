#ifdef MISTER_NATIVE_VIDEO

#include "frame_capture.h"
#include <string.h>   // memset, memcpy
#include <stdint.h>

// glad provides GL_RGBA, GL_UNSIGNED_BYTE, glReadPixels for GLES2
#include "thunks/khronos/glad.h"

#ifndef MISTER_WIDTH
#define MISTER_WIDTH  320
#endif
#ifndef MISTER_HEIGHT
#define MISTER_HEIGHT 240
#endif

// Static output buffer — 320x240 RGBA8888, no heap allocation
static uint8_t  s_frame_rgba[MISTER_WIDTH * MISTER_HEIGHT * 4];

// Worst-case source readback buffer: 1280x720 RGBA8888 (~3.5 MiB BSS, fine on
// MiSTer's 512 MiB).  Covers all common GameMaker internal resolutions:
// 640x480, 960x540, 1280x720. If a game exceeds this, ReadFrame falls back to
// direct readback without scaling (safe, not ideal).
static uint8_t  s_scaled_rgba[1280 * 720 * 4];

static int      s_src_w     = MISTER_WIDTH;
static int      s_src_h     = MISTER_HEIGHT;
static int      s_needs_scale = 0;

// Precomputed inverse-scale ratios (fixed-point <<8) and letterbox offsets.
static int s_x_ratio;   // (src_w << 8) / s_dst_w
static int s_y_ratio;   // (src_h << 8) / s_dst_h
static int s_dst_x;     // x offset in dest frame (letterbox)
static int s_dst_y;     // y offset in dest frame (letterbox)
static int s_dst_w;     // scaled output width  (<= MISTER_WIDTH)
static int s_dst_h;     // scaled output height (<= MISTER_HEIGHT)

void FrameCapture_Init(int src_w, int src_h) {
    s_src_w = src_w;
    s_src_h = src_h;
    s_needs_scale = (src_w != MISTER_WIDTH || src_h != MISTER_HEIGHT);

    if (s_needs_scale) {
        // Compute the scale factor that fits src inside MISTER_WIDTH x MISTER_HEIGHT
        // while preserving aspect ratio (letterbox / fit-inside).
        //
        // scale256 approach from OpenBOR_7533/patch_sdl_dummy.py:
        //   sx256 = ceil(src_w / dst_w) in fixed-point, then pick the larger
        //   of sx256 / sy256 so that neither axis overflows the destination.
        //
        // Here we work in the forward direction: how much can we scale UP each
        // axis before it hits the destination limit.  We take the SMALLER ratio
        // so neither axis exceeds its limit.
        int scale_x = (MISTER_WIDTH  << 8) / src_w;  // max scale in x
        int scale_y = (MISTER_HEIGHT << 8) / src_h;  // max scale in y
        int scale   = (scale_x < scale_y) ? scale_x : scale_y;

        s_dst_w = (src_w * scale) >> 8;
        s_dst_h = (src_h * scale) >> 8;

        // Clamp to destination bounds (rounding guard)
        if (s_dst_w > MISTER_WIDTH)  s_dst_w = MISTER_WIDTH;
        if (s_dst_h > MISTER_HEIGHT) s_dst_h = MISTER_HEIGHT;

        // Center the scaled frame (letterbox bars filled with black in ReadFrame)
        s_dst_x = (MISTER_WIDTH  - s_dst_w) / 2;
        s_dst_y = (MISTER_HEIGHT - s_dst_h) / 2;

        // Inverse ratios: for each dest pixel, which source pixel does it map to?
        s_x_ratio = (src_w << 8) / s_dst_w;
        s_y_ratio = (src_h << 8) / s_dst_h;
    }
}

void FrameCapture_ReadFrame(void) {
    if (!s_needs_scale) {
        // Source matches dest — direct readback into output buffer.
        glReadPixels(0, 0, MISTER_WIDTH, MISTER_HEIGHT,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_frame_rgba);
        return;
    }

    // Safety check: source must fit in the temp buffer.
    if (s_src_w * s_src_h * 4 > (int)sizeof(s_scaled_rgba)) {
        // Source resolution exceeds worst-case buffer — fall back to direct
        // readback at dest size rather than reading out of bounds.
        glReadPixels(0, 0, MISTER_WIDTH, MISTER_HEIGHT,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_frame_rgba);
        return;
    }

    // Readback at source resolution into temp buffer, then downscale.
    glReadPixels(0, 0, s_src_w, s_src_h,
                 GL_RGBA, GL_UNSIGNED_BYTE, s_scaled_rgba);

    // Clear output to black (fills letterbox bars).
    memset(s_frame_rgba, 0, sizeof(s_frame_rgba));

    // Bilinear downscale using integer fixed-point (scale256 style).
    //
    // For each destination pixel (dx, dy) we compute the fractional source
    // coordinate, sample the four nearest source pixels, and bilinearly blend.
    // All arithmetic stays in integers — no floats, matching the OpenBOR
    // patch_sdl_dummy.py approach exactly.
    for (int dy = 0; dy < s_dst_h; dy++) {
        int sy_fp   = dy * s_y_ratio;       // source Y, fixed-point (<<8)
        int sy0     = sy_fp >> 8;           // integer floor
        int fy      = sy_fp & 0xFF;         // fractional part [0, 255]
        int sy1     = sy0 + 1;
        if (sy1 >= s_src_h) sy1 = s_src_h - 1;

        for (int dx = 0; dx < s_dst_w; dx++) {
            int sx_fp   = dx * s_x_ratio;
            int sx0     = sx_fp >> 8;
            int fx      = sx_fp & 0xFF;
            int sx1     = sx0 + 1;
            if (sx1 >= s_src_w) sx1 = s_src_w - 1;

            // Four neighbouring source pixels for bilinear blend.
            const uint8_t* p00 = &s_scaled_rgba[(sy0 * s_src_w + sx0) * 4];
            const uint8_t* p10 = &s_scaled_rgba[(sy0 * s_src_w + sx1) * 4];
            const uint8_t* p01 = &s_scaled_rgba[(sy1 * s_src_w + sx0) * 4];
            const uint8_t* p11 = &s_scaled_rgba[(sy1 * s_src_w + sx1) * 4];

            uint8_t* out = &s_frame_rgba[((s_dst_y + dy) * MISTER_WIDTH + (s_dst_x + dx)) * 4];

            // Blend each channel independently.
            // Horizontal blend first, then vertical — all results stay in [0, 255].
            for (int c = 0; c < 4; c++) {
                int top    = (int)p00[c] + (((int)(p10[c] - p00[c]) * fx) >> 8);
                int bottom = (int)p01[c] + (((int)(p11[c] - p01[c]) * fx) >> 8);
                out[c]     = (uint8_t)(top + (((bottom - top) * fy) >> 8));
            }
        }
    }
}

const uint8_t* FrameCapture_GetRGBA(void) {
    return s_frame_rgba;
}

void FrameCapture_ConvertToRGB565(uint16_t* dst) {
    // Convert s_frame_rgba (RGBA8888, byte order R G B A) → RGB565.
    //
    // GL_RGBA / GL_UNSIGNED_BYTE on little-endian ARM delivers bytes in
    // memory order R, G, B, A — no R/B swap needed.
    //
    // RGB565 packing: bits [15:11] = R5, [10:5] = G6, [4:0] = B5.
    // This matches the OpenBOR DDR3 format exactly so NativeVideoWriter_WriteFrame
    // can consume the output of this function directly.
    //
    // gmloader renders via Android libyoyo.so using standard RGBA —
    // unlike OpenBOR which uses BGR internally, no channel swap is required.
    const uint8_t* src = s_frame_rgba;
    int n = MISTER_WIDTH * MISTER_HEIGHT;
    for (int i = 0; i < n; i++) {
        uint8_t r = src[i * 4 + 0];
        uint8_t g = src[i * 4 + 1];
        uint8_t b = src[i * 4 + 2];
        dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

#endif /* MISTER_NATIVE_VIDEO */
