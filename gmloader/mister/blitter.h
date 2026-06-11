//
//  MiSTer 2D software blitter — interface scaffolding (Phase 2).
//  See BLITTER_DESIGN.md for the rationale and the full state-shadowing model.
//
//  Status: SCAFFOLDING ONLY — not yet wired or built. Defines the types and the
//  single dispatch entry point the gles2.cpp draw hooks will call. The blitter
//  is gated behind config/env and falls back to GL for anything unsupported, so
//  it is inert until explicitly enabled.
//
#pragma once

#ifdef MISTER_NATIVE_VIDEO

#include <stdint.h>
#include "thunks/khronos/glad.h"   // GL types

#ifdef __cplusplus
extern "C" {
#endif

// ---- Render targets ---------------------------------------------------------
// One per FBO color attachment; the default framebuffer (id 0) maps to the
// 320x240 output buffer. An FBO's surface aliases its attached texture's pixels,
// so render-to-texture needs no GL readback.
typedef struct {
    uint8_t *rgba;     // tightly packed RGBA8888, h rows of w*4 bytes
    int      w;
    int      h;
    int      owns;     // 1 if blitter allocated rgba (default fb borrows capture buf)
} SoftSurface;

// ---- Shadowed GL objects ----------------------------------------------------
typedef struct {
    uint8_t *rgba;     // CPU copy of the texture's level-0 pixels (RGBA8888)
    int      w;
    int      h;
    int      nearest;  // GL_NEAREST (1) vs GL_LINEAR (0) min/mag filter
    int      valid;    // pixels present and decodable
} ShadowTexture;

typedef struct {
    uint8_t *data;     // CPU copy of buffer contents
    uint32_t size;
} ShadowBuffer;

// Supported blend modes; anything else => GL fallback for that draw.
typedef enum {
    BLEND_NONE = 0,        // GL_BLEND disabled: opaque copy
    BLEND_ALPHA,           // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    BLEND_PREMULT,         // ONE, ONE_MINUS_SRC_ALPHA
    BLEND_ADD,             // ONE, ONE  (or SRC_ALPHA, ONE)
    BLEND_UNSUPPORTED
} BlendMode;

// ---- Lifecycle / config -----------------------------------------------------
void Blitter_Init(void);          // read env/config; allocate default surface
int  Blitter_Enabled(void);       // master gate (env GMLOADER_BLITTER=1)

// ---- State-shadow hooks (called from the corresponding gles2.cpp thunks) ----
// These keep our mirror of GL state up to date. They are cheap and run whenever
// the blitter is enabled, regardless of whether a given draw ends up fast-pathed.
void Blitter_OnTexImage2D(GLuint tex, int w, int h, GLenum fmt, GLenum type, const void *px);
void Blitter_OnTexSubImage2D(GLuint tex, int x, int y, int w, int h, GLenum fmt, GLenum type, const void *px);
void Blitter_OnTexParameter(GLuint tex, GLenum pname, GLint param);
void Blitter_OnBindTexture(GLenum target, GLuint tex);
void Blitter_OnDeleteTexture(GLuint tex);

void Blitter_OnBufferData(GLenum target, uint32_t size, const void *data);
void Blitter_OnBufferSubData(GLenum target, uint32_t off, uint32_t size, const void *data);
void Blitter_OnBindBuffer(GLenum target, GLuint buf);
void Blitter_OnVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr);
void Blitter_OnEnableVertexAttrib(GLuint index, int enabled);
void Blitter_OnBindAttribLocation(GLuint program, GLuint index, const char *name);

void Blitter_OnBindFramebuffer(GLenum target, GLuint fbo);
void Blitter_OnFramebufferTexture2D(GLenum attach, GLuint tex);

void Blitter_OnUseProgram(GLuint program);
void Blitter_OnGetUniformLocation(GLuint program, const char *name, GLint loc);
void Blitter_OnUniformMatrix4fv(GLint location, GLsizei count, const GLfloat *value);  // capture matrices
void Blitter_OnBlendState(int enabled, GLenum src, GLenum dst);
void Blitter_OnViewport(int x, int y, int w, int h);
void Blitter_OnScissor(int enabled, int x, int y, int w, int h);

// ---- Draw dispatch ----------------------------------------------------------
// Returns 1 if the blitter fully handled the draw (caller must NOT call GL),
// 0 if the draw is not fast-path eligible (caller falls back to glad_glDraw*).
int Blitter_TryDrawArrays(GLenum mode, GLint first, GLsizei count);
int Blitter_TryDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);

// ---- Frame boundary ---------------------------------------------------------
// Called when the default framebuffer is ready to present (canFlip). Returns the
// RGBA8888 320x240 result for the existing RGBA->565->DDR path, or NULL if the
// blitter did not produce this frame (GL fallback owns it).
const uint8_t *Blitter_PresentDefault(void);
void Blitter_OnClear(GLbitfield mask);

#ifdef __cplusplus
}
#endif

#endif // MISTER_NATIVE_VIDEO
