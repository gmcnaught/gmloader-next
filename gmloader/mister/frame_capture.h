#pragma once
#include <stdint.h>

#ifdef MISTER_NATIVE_VIDEO

void     FrameCapture_Init(int src_w, int src_h);
void     FrameCapture_ReadFrame(void);
const uint8_t* FrameCapture_GetRGBA(void);
void     FrameCapture_ConvertToRGB565(uint16_t* dst);

#else

static inline void     FrameCapture_Init(int src_w, int src_h) { (void)src_w; (void)src_h; }
static inline void     FrameCapture_ReadFrame(void) {}
static inline const uint8_t* FrameCapture_GetRGBA(void) { return 0; }
static inline void     FrameCapture_ConvertToRGB565(uint16_t* dst) { (void)dst; }

#endif
