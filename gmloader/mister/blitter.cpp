//
//  MiSTer 2D software blitter — P2 step 1: state shadowing + draw DECODE + log.
//  See BLITTER_DESIGN.md. This step proves we can extract correct blittable
//  primitives (screen-space textured triangles, render target, texture, blend)
//  from the GL draw stream. It does NOT rasterize yet — Blitter_TryDraw* always
//  returns 0 (fall back to GL), so there is no visual change. The rasterizer is
//  step 2, written once this decode is verified against a real scene.
//
//  Enable the decode log with env GMLOADER_BLITTER=1.
//
#ifdef MISTER_NATIVE_VIDEO

#include "blitter.h"
#include "blitter_raster.h"
#include "raster_backend.h"
#include "configuration.h"   // gmloader_config.blitter (default level)

// Task 7: lets backend_mfgpu (raster_backend_mfgpu.cpp) tell the default fb's
// pixels apart from an FBO / render-to-texture target's, so its draw() can
// route the latter to the SW fallback. No-op when backend_sw is selected.
extern "C" void RasterBackend_MFGPU_SetDefaultSurface(const uint8_t *rgba);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <map>
#include <string>
#include <array>
#include <vector>

static inline uint64_t bl_now_ns() {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
// Per-frame profiler buckets (env GMLOADER_BLITTER_PROF=1). GMLOADER_BLITTER_NOTEX=1
// forces the rasterizer to skip the texture fetch (samples white) to isolate the
// texture-sampling cost from raster overhead.
static int      g_prof = 0, g_notex = 0;
static int      g_tex16 = 0;     // GMLOADER_BLITTER_TEX16: store uploaded textures as
                                 // packed RGBA4444 (2 bpp) instead of RGBA8888 (4 bpp)
static int      g_opaque = 1;    // GMLOADER_BLITTER_OPAQUE (default on): downgrade
                                 // RB_ALPHA/RB_PREMULT to RB_NONE when src is provably
                                 // opaque, skipping the per-pixel div255 blend
static int      g_cull = 1;      // GMLOADER_BLITTER_CULL (default on): skip draws that
                                 // provably can't change a visible pixel (overdraw)
static int      g_threads = 1;   // GMLOADER_BLITTER_THREADS (rasterizer worker cores)
static uint64_t g_pf_raster = 0, g_pf_clear = 0, g_pf_tex = 0, g_pf_present = 0;
static uint32_t g_pf_draws = 0, g_pf_tris = 0, g_pf_frame = 0, g_pf_culled = 0;

#ifndef MISTER_WIDTH
#define MISTER_WIDTH  320
#endif
#ifndef MISTER_HEIGHT
#define MISTER_HEIGHT 240
#endif

// ---- master gate ------------------------------------------------------------
// GMLOADER_BLITTER level: 0 off; 1 = shadow + decode-log + GL fallback (safe,
// validated); 2 = blitter OWNS rendering (rasterize into our surfaces, skip GL
// scene draws/clears, present our framebuffer).
static int      g_level   = 0;
static int      g_own     = 0;   // level >= 2
static int      g_enabled = 0;   // level >= 1
static int      g_rw = MISTER_WIDTH, g_rh = MISTER_HEIGHT;  // render size (<= DDR 320x240)
static uint8_t *g_defSurf = nullptr;   // default framebuffer, RGBA8888, GL bottom-up

void Blitter_Init(void) {
    // Level comes from gmloader.json ("blitter"); env GMLOADER_BLITTER overrides
    // it when set (for dev A/B). Default 0 (off) if neither is given.
    const char *e = getenv("GMLOADER_BLITTER");
    g_level   = e ? atoi(e) : gmloader_config.blitter;
    g_enabled = g_level >= 1;
    g_own     = g_level >= 2;
    g_prof    = getenv("GMLOADER_BLITTER_PROF") ? 1 : 0;
    g_notex   = getenv("GMLOADER_BLITTER_NOTEX") ? 1 : 0;
    g_tex16   = getenv("GMLOADER_BLITTER_TEX16") ? 1 : 0;
    { const char *op = getenv("GMLOADER_BLITTER_OPAQUE"); g_opaque = op ? atoi(op) : 1; }
    { const char *c  = getenv("GMLOADER_BLITTER_CULL");   g_cull   = c  ? atoi(c)  : 1; }
    { const char *th = getenv("GMLOADER_BLITTER_THREADS");
      g_threads = th ? atoi(th) : 1;
      if (g_threads < 1) g_threads = 1; else if (g_threads > 4) g_threads = 4; }
    RasterBackend_SW_SetThreads(g_threads);   // keep backend_sw in sync with g_threads
    g_rw = MISTER_WIDTH; g_rh = MISTER_HEIGHT;
    if (g_own) {   // render-size override only applies when the blitter presents
        const char *rw = getenv("GMLOADER_RENDER_W"), *rh = getenv("GMLOADER_RENDER_H");
        if (rw) g_rw = atoi(rw);
        if (rh) g_rh = atoi(rh);
        if (g_rw <= 0 || g_rw > MISTER_WIDTH)  g_rw = MISTER_WIDTH;
        if (g_rh <= 0 || g_rh > MISTER_HEIGHT) g_rh = MISTER_HEIGHT;
    }
    if (g_enabled) {
        g_defSurf = (uint8_t *)calloc((size_t)g_rw * g_rh, 4);
        RasterBackend_MFGPU_SetDefaultSurface(g_defSurf);
        fprintf(stderr, "BLITTER enabled (level %d, own=%d, render %dx%d -> DDR %dx%d, "
                "tex=%s, opaque=%d, cull=%d)\n",
                g_level, g_own, g_rw, g_rh, MISTER_WIDTH, MISTER_HEIGHT,
                g_tex16 ? "RGBA4444" : "RGBA8888", g_opaque, g_cull);
    }
}
int Blitter_Enabled(void) { return g_enabled; }
int Blitter_RenderW(void) { return g_rw; }
int Blitter_RenderH(void) { return g_rh; }

// ---- shadowed GL state ------------------------------------------------------
namespace {

struct Tex {
    int w = 0, h = 0;
    GLenum fmt = 0, type = 0;
    uint8_t *rgba = nullptr;   // CPU copy, null if format unsupported. RGBA8888
                               // (4 bpp) normally; packed RGBA4444 (2 bpp) when
                               // `packed` (uploaded textures only, GMLOADER_BLITTER_TEX16).
    int packed = 0;            // 1 => rgba is RGBA4444 (see RTEX_RGBA4444)
    int opaque = 0;            // 1 if every source alpha byte == 255 (scanned on upload)
    int valid = 0;
};

struct Buf {
    uint8_t *data = nullptr;
    uint32_t size = 0;
};

struct Attrib {
    int      enabled = 0;
    GLint    size = 0;          // components (1..4)
    GLenum   type = 0;
    GLboolean norm = 0;
    GLsizei  stride = 0;
    uintptr_t offset = 0;       // byte offset into the bound array buffer
    GLuint   buffer = 0;        // array buffer bound when the pointer was set
};

std::map<GLuint, Tex>  g_textures;
std::map<GLuint, Buf>  g_buffers;
// name->location attribute map per program (from glBindAttribLocation)
std::map<GLuint, std::map<std::string, GLuint>> g_attrLoc;

GLuint  g_curProgram   = 0;
GLuint  g_arrayBuffer  = 0;     // GL_ARRAY_BUFFER
GLuint  g_elemBuffer   = 0;     // GL_ELEMENT_ARRAY_BUFFER
GLuint  g_boundTex2D   = 0;     // texture on the active unit (we assume unit 0)
Attrib  g_attribs[16];

GLuint  g_curFBO       = 0;     // 0 = default framebuffer
std::map<GLuint, GLuint> g_fboColorTex;   // fbo -> color attachment texture id

int     g_blendEnabled = 0;
GLenum  g_blendSrc = GL_ONE, g_blendDst = GL_ZERO;

int     g_vpX = 0, g_vpY = 0, g_vpW = 320, g_vpH = 240;

// Uniform matrices tracked by GL location; the WVP is gm_Matrices[4]. We learn
// gm_Matrices' base location from glGetUniformLocation (array elements have
// consecutive locations per spec), so we capture the WVP however GM uploads it
// (whole array, or just element [4]).
std::map<GLuint, std::map<std::string, GLint>> g_uniformLoc;  // program -> name -> loc
std::map<GLint, std::array<float,16>>          g_matByLoc;    // loc -> mat4
float g_wvp[16];      // gm_Matrices[4] = WORLD_VIEW_PROJECTION (count>=5 upload)
int   g_wvpValid = 0;

uint64_t g_drawNo = 0;
const uint64_t LOG_FIRST = 24;  // log the first N draws in detail

// Hook-fire counters — confirm which state-setting calls actually reach our
// thunk vs. go straight to Mesa via eglGetProcAddress (RTLD_GLOBAL bypass).
uint32_t g_nVap=0, g_nBindBuf=0, g_nBufData=0, g_nGetULoc=0, g_nUniMat=0, g_nEnVap=0, g_nUseProg=0;

// Convert an uploaded texture to an RGBA8888 CPU copy (common GM formats only).
void store_texture(GLuint id, int w, int h, GLenum fmt, GLenum type, const void *px) {
    Tex &t = g_textures[id];
    free(t.rgba); t.rgba = nullptr; t.valid = 0; t.packed = 0; t.opaque = 0;
    t.w = w; t.h = h; t.fmt = fmt; t.type = type;
    if (!px || w <= 0 || h <= 0) return;            // allocated-but-unfilled (FBO target)
    if (type == GL_UNSIGNED_BYTE && (fmt == GL_RGBA)) {
        size_t n = (size_t)w * h;
        uint64_t _t0 = g_prof ? bl_now_ns() : 0;
        // Scan the source alpha once: a fully-opaque texture lets RB_ALPHA collapse
        // to a plain copy (RB_NONE) at draw time (see handle_draw). Scanned on the
        // RGBA8888 source so it holds regardless of the packed/unpacked store below.
        { const uint8_t *s = (const uint8_t *)px; int op = 1;
          for (size_t i = 0; i < n; i++) if (s[i*4+3] != 255) { op = 0; break; }
          t.opaque = op; }
        if (g_tex16) {
            // Pack RGBA8888 -> RGBA4444 (2 bpp): keep the top nibble of each
            // channel. Halves the sampler's per-texel gather; nearest-only.
            const uint8_t *s = (const uint8_t *)px;
            uint16_t *d = (uint16_t *)malloc(n * 2);
            if (d) {
                for (size_t i = 0; i < n; i++)
                    d[i] = (uint16_t)(((s[i*4+0] >> 4) << 12) | ((s[i*4+1] >> 4) << 8) |
                                      ((s[i*4+2] >> 4) << 4)  |  (s[i*4+3] >> 4));
                t.rgba = (uint8_t *)d; t.valid = 1; t.packed = 1;
            }
        } else {
            t.rgba = (uint8_t *)malloc(n * 4);
            if (t.rgba) { memcpy(t.rgba, px, n * 4); t.valid = 1; }
        }
        if (g_prof) g_pf_tex += bl_now_ns() - _t0;
    }
    // other formats (RGB, LUMINANCE, compressed) -> leave invalid; such draws
    // will fall back to GL. The log reports the format so we know what to add.
}

// column-major mat4 (GL default) times vec4
void mat4_mul_vec4(const float *m, const float *v, float *out) {
    for (int r = 0; r < 4; r++)
        out[r] = m[0*4+r]*v[0] + m[1*4+r]*v[1] + m[2*4+r]*v[2] + m[3*4+r]*v[3];
}

const char* blend_name() {
    if (!g_blendEnabled) return "NONE";
    if (g_blendSrc == GL_SRC_ALPHA && g_blendDst == GL_ONE_MINUS_SRC_ALPHA) return "ALPHA";
    if (g_blendSrc == GL_ONE && g_blendDst == GL_ONE_MINUS_SRC_ALPHA) return "PREMULT";
    if ((g_blendSrc == GL_ONE && g_blendDst == GL_ONE) ||
        (g_blendSrc == GL_SRC_ALPHA && g_blendDst == GL_ONE)) return "ADD";
    return "OTHER";
}

// Find the attribute location bound to a given name for the current program.
// Returns -1 if unknown.
int attr_index(const char *name) {
    auto pit = g_attrLoc.find(g_curProgram);
    if (pit == g_attrLoc.end()) return -1;
    auto it = pit->second.find(name);
    return (it == pit->second.end()) ? -1 : (int)it->second;
}

// The WVP matrix (gm_Matrices[4]) for the current program, or null if unknown.
const float* get_mvp() {
    if (g_wvpValid) return g_wvp;          // count>=5 array upload (the GM case)
    auto pit = g_uniformLoc.find(g_curProgram);
    if (pit == g_uniformLoc.end()) return nullptr;
    auto &m = pit->second;
    GLint loc = -1;
    auto d = m.find("gm_Matrices[4]");
    if (d != m.end()) loc = d->second;
    else {
        auto b = m.find("gm_Matrices");
        if (b == m.end()) b = m.find("gm_Matrices[0]");
        if (b != m.end()) loc = b->second + 4;   // array element locations are consecutive
    }
    if (loc < 0) return nullptr;
    auto it = g_matByLoc.find(loc);
    return (it == g_matByLoc.end()) ? nullptr : it->second.data();
}

// Read one attribute's components for vertex i into out[4]. Resolves the data
// base per-attrib: a VBO (a.buffer != 0) or a client-side array (a.buffer == 0,
// where a.offset is the real CPU pointer).
void read_attrib(const Attrib &a, int i, float *out) {
    out[0]=0; out[1]=0; out[2]=0; out[3]=1;
    if (!a.enabled) return;
    const uint8_t *base;
    if (a.buffer) {
        auto it = g_buffers.find(a.buffer);
        if (it == g_buffers.end() || !it->second.data) return;
        base = it->second.data + a.offset;
    } else {
        if (!a.offset) return;
        base = (const uint8_t *)a.offset;          // client-side array
    }
    int comp   = (a.type == GL_FLOAT) ? 4 : 1;
    int stride = a.stride ? a.stride : a.size * comp;   // 0 => tightly packed
    const uint8_t *p = base + (size_t)i * stride;
    for (int c = 0; c < a.size && c < 4; c++) {
        if (a.type == GL_FLOAT)              out[c] = ((const float *)p)[c];
        else if (a.type == GL_UNSIGNED_BYTE) out[c] = p[c] / (a.norm ? 255.0f : 1.0f);
        else                                 out[c] = 0;
    }
}

// Map current GL blend state to an RBlend; false if unsupported.
bool get_rblend(RBlend *out) {
    if (!g_blendEnabled) { *out = RB_NONE; return true; }
    if (g_blendSrc == GL_SRC_ALPHA && g_blendDst == GL_ONE_MINUS_SRC_ALPHA) { *out = RB_ALPHA; return true; }
    if (g_blendSrc == GL_ONE && g_blendDst == GL_ONE_MINUS_SRC_ALPHA)       { *out = RB_PREMULT; return true; }
    if ((g_blendSrc == GL_ONE && g_blendDst == GL_ONE) ||
        (g_blendSrc == GL_SRC_ALPHA && g_blendDst == GL_ONE))               { *out = RB_ADD; return true; }
    return false;
}

// Current render target as an RSurface. The default fb maps to g_defSurf; an FBO
// maps to its color attachment texture's CPU buffer (allocated on first use, so
// it works as both a render target and a sampleable texture). False if unknown.
bool get_render_target(RSurface *out) {
    if (g_curFBO == 0) {
        out->rgba = g_defSurf; out->w = g_rw; out->h = g_rh;
        return out->rgba != nullptr;
    }
    auto fit = g_fboColorTex.find(g_curFBO);
    if (fit == g_fboColorTex.end()) return false;
    Tex &t = g_textures[fit->second];
    if (t.w <= 0 || t.h <= 0) return false;
    // A render target must be a full RGBA8888 surface (the rasterizer writes 4
    // bpp). If this texture was uploaded as packed RGBA4444, drop it and back the
    // target with a fresh 8888 buffer — never alias a 2 bpp buffer as 4 bpp.
    if (t.packed) { free(t.rgba); t.rgba = nullptr; t.packed = 0; }
    if (!t.rgba) { t.rgba = (uint8_t *)calloc((size_t)t.w * t.h, 4); t.valid = t.rgba ? 1 : 0; }
    out->rgba = t.rgba; out->w = t.w; out->h = t.h;
    return out->rgba != nullptr;
}

// Bound texture as an RTexture for sampling (valid=0 if no CPU pixels yet).
void get_rtexture(RTexture *out) {
    auto it = g_textures.find(g_boundTex2D);
    if (it != g_textures.end() && it->second.rgba) {
        out->rgba = it->second.rgba; out->w = it->second.w; out->h = it->second.h;
        out->nearest = 1; out->valid = 1;
        out->format = it->second.packed ? RTEX_RGBA4444 : RTEX_RGBA8888;
        out->opaque = it->second.opaque;
    } else {
        out->rgba = nullptr; out->w = 0; out->h = 0; out->nearest = 1; out->valid = 0;
        out->format = RTEX_RGBA8888;
        out->opaque = 0;
    }
}

} // namespace

// ---- state-shadow hooks -----------------------------------------------------
void Blitter_OnTexImage2D(GLuint tex, int w, int h, GLenum fmt, GLenum type, const void *px) {
    if (!g_enabled) return;
    store_texture(tex ? tex : g_boundTex2D, w, h, fmt, type, px);
}
void Blitter_OnTexSubImage2D(GLuint, int, int, int, int, GLenum, GLenum, const void *) {
    // step 1: ignore (full re-decode not needed for logging); step 2 will patch.
}
void Blitter_OnTexParameter(GLuint, GLenum, GLint) {}
void Blitter_OnBindTexture(GLenum target, GLuint tex) {
    if (g_enabled && target == GL_TEXTURE_2D) g_boundTex2D = tex;
}
void Blitter_OnDeleteTexture(GLuint tex) {
    auto it = g_textures.find(tex);
    if (it != g_textures.end()) { free(it->second.rgba); g_textures.erase(it); }
}

void Blitter_OnBufferData(GLenum target, uint32_t size, const void *data) {
    if (!g_enabled) return;
    g_nBufData++;
    GLuint id = (target == GL_ELEMENT_ARRAY_BUFFER) ? g_elemBuffer : g_arrayBuffer;
    Buf &b = g_buffers[id];
    free(b.data); b.data = nullptr; b.size = 0;
    if (size) { b.data = (uint8_t *)malloc(size); if (b.data) { b.size = size;
        if (data) memcpy(b.data, data, size); } }
}
void Blitter_OnBufferSubData(GLenum target, uint32_t off, uint32_t size, const void *data) {
    if (!g_enabled || !data) return;
    GLuint id = (target == GL_ELEMENT_ARRAY_BUFFER) ? g_elemBuffer : g_arrayBuffer;
    Buf &b = g_buffers[id];
    if (b.data && off + size <= b.size) memcpy(b.data + off, data, size);
}
void Blitter_OnBindAttribLocation(GLuint program, GLuint index, const char *name) {
    if (g_enabled && name) g_attrLoc[program][name] = index;
}
void Blitter_OnBindBuffer(GLenum target, GLuint buf) {
    if (!g_enabled) return;
    g_nBindBuf++;
    if (target == GL_ARRAY_BUFFER)         g_arrayBuffer = buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g_elemBuffer = buf;
}
void Blitter_OnVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm,
                                   GLsizei stride, const void *ptr) {
    if (g_enabled) g_nVap++;
    if (!g_enabled || index >= 16) return;
    Attrib &a = g_attribs[index];
    a.size = size; a.type = type; a.norm = norm; a.stride = stride;
    a.offset = (uintptr_t)ptr; a.buffer = g_arrayBuffer;
}
void Blitter_OnEnableVertexAttrib(GLuint index, int enabled) {
    if (g_enabled) g_nEnVap++;
    if (g_enabled && index < 16) g_attribs[index].enabled = enabled;
}

void Blitter_OnBindFramebuffer(GLenum, GLuint fbo) { if (g_enabled) g_curFBO = fbo; }
void Blitter_OnFramebufferTexture2D(GLenum, GLuint tex) {
    if (g_enabled) g_fboColorTex[g_curFBO] = tex;   // attach to currently-bound FBO
}

void Blitter_OnUseProgram(GLuint program) { if (g_enabled) { g_nUseProg++; g_curProgram = program; } }
void Blitter_OnGetUniformLocation(GLuint program, const char *name, GLint loc) {
    if (!g_enabled) return;
    g_nGetULoc++;
    if (name && loc >= 0) g_uniformLoc[program][name] = loc;
}
void Blitter_OnUniformMatrix4fv(GLint loc, GLsizei count, const GLfloat *value) {
    if (g_enabled) g_nUniMat++;
    if (!g_enabled || !value) return;
    // GM uploads gm_Matrices[5] in one call; element [4] is WORLD_VIEW_PROJECTION.
    if (count >= 5) { memcpy(g_wvp, value + 4 * 16, sizeof(g_wvp)); g_wvpValid = 1; }
    if (loc >= 0)
        for (GLsizei i = 0; i < count; i++) {
            std::array<float,16> m;
            memcpy(m.data(), value + (size_t)i * 16, sizeof(m));
            g_matByLoc[loc + i] = m;
        }
}
void Blitter_OnBlendState(int enabled, GLenum src, GLenum dst) {
    if (!g_enabled) return;
    if (enabled >= 0) g_blendEnabled = enabled;   // <0 = leave unchanged (glBlendFunc)
    if (src) g_blendSrc = src;
    if (dst) g_blendDst = dst;
}
void Blitter_OnViewport(int x, int y, int w, int h) {
    if (!g_enabled) return;
    g_vpX = x; g_vpY = y; g_vpW = w; g_vpH = h;
}
void Blitter_OnScissor(int, int, int, int, int) {}
int Blitter_OnClear(GLbitfield) {
    if (!g_own) return 0;
    RSurface rt;
    if (get_render_target(&rt)) {
        uint64_t _t0 = g_prof ? bl_now_ns() : 0;
        RasterBackend_Select()->clear(&rt, 0, 0, 0, 0);   // clear to transparent black
        if (g_prof) g_pf_clear += bl_now_ns() - _t0;
    }
    return 1;   // cleared ourselves — caller skips the (slow) GL clear
}

// ---- draw decode + rasterize ------------------------------------------------
static std::vector<BVtx> s_verts;

static int handle_draw(const char *kind, GLenum mode, int count,
                       const uint8_t *indices, GLenum idx_type) {
    g_drawNo++;
    if (!g_enabled) return 0;

    if (g_drawNo == 1) {
        fprintf(stderr, "BLIT hooks fired: useprog=%u vap=%u envap=%u bindbuf=%u "
                "bufdata=%u getuloc=%u unimat=%u | shadow buffers=%zu mats=%zu\n",
                g_nUseProg, g_nVap, g_nEnVap, g_nBindBuf, g_nBufData, g_nGetULoc,
                g_nUniMat, g_buffers.size(), g_matByLoc.size());
    }

    int pidx = attr_index("in_Position");
    int tidx = attr_index("in_TextureCoord");
    int cidx = attr_index("in_Colour");
    if (pidx < 0 || tidx < 0) {   // fallback: infer by component layout
        for (int i = 0; i < 16; i++) {
            if (!g_attribs[i].enabled) continue;
            if (pidx < 0 && g_attribs[i].size == 3 && g_attribs[i].type == GL_FLOAT) pidx = i;
            else if (tidx < 0 && g_attribs[i].size == 2 && g_attribs[i].type == GL_FLOAT) tidx = i;
            else if (cidx < 0 && g_attribs[i].size == 4 && g_attribs[i].type == GL_UNSIGNED_BYTE) cidx = i;
        }
    }

    const float *mvp = get_mvp();

    // Decode each vertex to a screen-space BVtx (GL bottom-up pixel coords).
    s_verts.clear();
    float minx=1e9f, miny=1e9f, maxx=-1e9f, maxy=-1e9f;
    int decoded = 0;
    if (pidx >= 0 && mvp && g_attribs[pidx].enabled) {
        for (int k = 0; k < count; k++) {
            int vi = indices ? (idx_type == GL_UNSIGNED_SHORT ? ((const uint16_t*)indices)[k]
                                                              : ((const uint8_t*)indices)[k])
                             : k;
            float pos[4]; read_attrib(g_attribs[pidx], vi, pos); pos[3] = 1.0f;
            float clip[4]; mat4_mul_vec4(mvp, pos, clip);
            float w = clip[3] != 0 ? clip[3] : 1.0f;
            float ndcx = clip[0]/w, ndcy = clip[1]/w;
            BVtx bv;
            bv.x = g_vpX + (ndcx*0.5f + 0.5f) * g_vpW;
            bv.y = g_vpY + (ndcy*0.5f + 0.5f) * g_vpH;     // GL bottom-up
            bv.u = bv.v = 0.0f;
            bv.r = bv.g = bv.b = bv.a = 1.0f;
            if (tidx >= 0) { float uv[4]; read_attrib(g_attribs[tidx], vi, uv); bv.u = uv[0]; bv.v = uv[1]; }
            if (cidx >= 0) { float c[4];  read_attrib(g_attribs[cidx], vi, c);  bv.r=c[0]; bv.g=c[1]; bv.b=c[2]; bv.a=c[3]; }
            s_verts.push_back(bv);
            if (bv.x<minx)minx=bv.x; if (bv.x>maxx)maxx=bv.x;
            if (bv.y<miny)miny=bv.y; if (bv.y>maxy)maxy=bv.y;
            decoded++;
        }
    }

    // Rasterize into our surfaces (owning mode only).
    int rast = 0;
    const char *cullReason = nullptr;   // non-null => skipped as provably invisible
    if (g_own && decoded == count && mode == GL_TRIANGLES && count >= 3) {
        RSurface rt; RBlend blend;
        if (get_render_target(&rt) && get_rblend(&blend)) {
            RTexture tex; get_rtexture(&tex);
            if (g_notex) tex.valid = 0;   // probe: skip texture fetch -> samples white
            if (tex.valid || g_notex) {
                // ---- overdraw culling (env GMLOADER_BLITTER_CULL, default on) ----
                // Cheap per-draw short-circuits that drop draws which provably can't
                // change a visible pixel of the render target. The bbox is in the
                // same pixel space (viewport spans [0,w)x[0,h)) the rasterizer clips
                // to, so an off-target / zero-area bbox covers no pixel. The fully-
                // transparent test is restricted to RB_ALPHA (the only blend whose
                // output is exactly dst when src.a==0 — PREMULT/ADD can still add rgb).
                if (g_cull) {
                    if (maxx <= 0.0f || minx >= (float)rt.w ||
                        maxy <= 0.0f || miny >= (float)rt.h)
                        cullReason = "offtarget";
                    else if (maxx <= minx || maxy <= miny)
                        cullReason = "zeroarea";
                    else if (blend == RB_ALPHA) {
                        bool allClear = true;
                        for (const BVtx &bv : s_verts)
                            if (bv.a > (0.5f / 255.0f)) { allClear = false; break; }
                        if (allClear) cullReason = "transparent";
                    }
                }
                // Opaque fast-path (env GMLOADER_BLITTER_OPAQUE, default on): an
                // over/premult blend of a provably-opaque source (every texel
                // alpha==255 AND every vertex alpha==1) equals a plain copy, so
                // downgrade to RB_NONE and skip the per-pixel div255 blend math.
                if (!cullReason && g_opaque &&
                    (blend == RB_ALPHA || blend == RB_PREMULT) && tex.valid && tex.opaque) {
                    int vop = 1;
                    for (int v = 0; v < count; v++) if (s_verts[v].a < 0.998f) { vop = 0; break; }
                    if (vop) blend = RB_NONE;
                }
                if (cullReason) {
                    if (g_prof) g_pf_culled++;
                } else {
                    uint64_t _t0 = g_prof ? bl_now_ns() : 0;
                    RasterBackend_Select()->draw(&rt, &s_verts[0], count / 3, &tex, blend, 0.0f, g_boundTex2D);
                    if (g_prof) { g_pf_raster += bl_now_ns() - _t0; g_pf_draws++; g_pf_tris += count/3; }
                    rast = 1;
                }
            }
        }
    }

    if (g_drawNo <= LOG_FIRST) {
        Tex *bt = g_textures.count(g_boundTex2D) ? &g_textures[g_boundTex2D] : nullptr;
        fprintf(stderr, "BLIT draw#%llu %s rt=%s tex=%u(%dx%d val=%d) blend=%s "
                "decoded=%d/%d rast=%d cull=%s screen=[%.0f,%.0f..%.0f,%.0f]\n",
                (unsigned long long)g_drawNo, kind, g_curFBO ? "FBO" : "DEF",
                g_boundTex2D, bt?bt->w:0, bt?bt->h:0, bt?bt->valid:0, blend_name(),
                decoded, count, rast, cullReason ? cullReason : "-",
                decoded?minx:0, decoded?miny:0, decoded?maxx:0, decoded?maxy:0);
    }

    return g_own ? 1 : 0;   // owning mode: blitter owns the draw, skip GL
}

int Blitter_TryDrawArrays(GLenum mode, GLint first, GLsizei count) {
    (void)first;
    return handle_draw("arrays", mode, count, nullptr, 0);
}
int Blitter_TryDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    const uint8_t *ip = nullptr;
    if (g_buffers.count(g_elemBuffer)) {
        Buf &eb = g_buffers[g_elemBuffer];
        if (eb.data) ip = eb.data + (uintptr_t)indices;
    } else {
        ip = (const uint8_t *)indices;   // client-side indices
    }
    return handle_draw("elements", mode, count, ip, type);
}

const uint8_t *Blitter_PresentDefault(void) {
    if (g_own) {
        // Route present through the seam. backend_sw's present is a no-op
        // today (see raster_backend_sw.cpp) — the real RGB565 conversion
        // still happens via Blitter_ToRGB565, called directly by main.cpp's
        // frame loop exactly as before this refactor; this call site only
        // makes present() reachable through RasterBackend for later backends.
        RSurface rt = { g_defSurf, g_rw, g_rh };
        RasterBackend_Select()->present(&rt);
    }
    return g_own ? g_defSurf : nullptr;
}

void Blitter_ToRGB565(const uint8_t *src_rgba, uint16_t *dst) {
    // src is the render-size (g_rw x g_rh) GL bottom-up surface; the DDR buffer
    // is MISTER_WIDTH x MISTER_HEIGHT top-down. Center (letterbox) src into dst
    // 1:1 with a black border, flipping rows top<->bottom. No scaling => no
    // resampling artifacts.
    uint64_t _t0 = g_prof ? bl_now_ns() : 0;
    const int ox = (MISTER_WIDTH  - g_rw) / 2;
    const int oy = (MISTER_HEIGHT - g_rh) / 2;
    memset(dst, 0, (size_t)MISTER_WIDTH * MISTER_HEIGHT * 2);   // borders = black
    for (int y = 0; y < g_rh; y++) {
        const uint8_t *s = src_rgba + (size_t)(g_rh - 1 - y) * g_rw * 4;   // flip
        uint16_t *d = dst + (size_t)(y + oy) * MISTER_WIDTH + ox;
        for (int x = 0; x < g_rw; x++) {
            uint8_t r = s[x*4+0], g = s[x*4+1], b = s[x*4+2];
            d[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    if (g_prof) g_pf_present += bl_now_ns() - _t0;
}

void Blitter_ProfFrameEnd(uint64_t process_ns) {
    if (!g_prof) return;
    g_pf_frame++;
    uint64_t bl = g_pf_raster + g_pf_clear + g_pf_tex;
    uint64_t logic = process_ns > bl ? process_ns - bl : 0;   // GM VM + non-blitter GL
    if (g_pf_frame % 30 == 0) {
        fprintf(stderr, "BLITPROF f=%u draws=%u tris=%u culled=%u | raster=%.1f clear=%.1f "
                "texup=%.1f present=%.1f logic=%.1f ms%s\n",
                g_pf_frame, g_pf_draws, g_pf_tris, g_pf_culled, g_pf_raster/1e6, g_pf_clear/1e6,
                g_pf_tex/1e6, g_pf_present/1e6, logic/1e6, g_notex ? " [NOTEX]" : "");
    }
    g_pf_raster = g_pf_clear = g_pf_tex = g_pf_present = 0;
    g_pf_draws = g_pf_tris = g_pf_culled = 0;
}

#endif // MISTER_NATIVE_VIDEO
