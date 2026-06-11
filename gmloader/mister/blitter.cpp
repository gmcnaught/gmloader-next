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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <map>
#include <string>
#include <array>

// ---- master gate ------------------------------------------------------------
static int g_enabled = 0;
void Blitter_Init(void) {
    const char *e = getenv("GMLOADER_BLITTER");
    g_enabled = (e && *e) ? 1 : 0;
    if (g_enabled) fprintf(stderr, "BLITTER decode-log enabled (no rasterize yet)\n");
}
int Blitter_Enabled(void) { return g_enabled; }

// ---- shadowed GL state ------------------------------------------------------
namespace {

struct Tex {
    int w = 0, h = 0;
    GLenum fmt = 0, type = 0;
    uint8_t *rgba = nullptr;   // CPU copy (RGBA8888), null if format unsupported
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
    free(t.rgba); t.rgba = nullptr; t.valid = 0;
    t.w = w; t.h = h; t.fmt = fmt; t.type = type;
    if (!px || w <= 0 || h <= 0) return;            // allocated-but-unfilled (FBO target)
    if (type == GL_UNSIGNED_BYTE && (fmt == GL_RGBA)) {
        size_t n = (size_t)w * h * 4;
        t.rgba = (uint8_t *)malloc(n);
        if (t.rgba) { memcpy(t.rgba, px, n); t.valid = 1; }
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
void Blitter_OnClear(GLbitfield) {}

// ---- draw decode (logs, then falls back to GL) ------------------------------
static int decode_and_log(const char *kind, GLenum mode, int count,
                          const uint8_t *indices, GLenum idx_type) {
    g_drawNo++;
    if (!g_enabled) return 0;

    if (g_drawNo == 1) {
        fprintf(stderr, "BLIT hooks fired: useprog=%u vap=%u envap=%u bindbuf=%u "
                "bufdata=%u getuloc=%u unimat=%u | shadow buffers=%zu mats=%zu\n",
                g_nUseProg, g_nVap, g_nEnVap, g_nBindBuf, g_nBufData, g_nGetULoc,
                g_nUniMat, g_buffers.size(), g_matByLoc.size());
    }

    // Resolve render target surface size.
    int rt_w = g_vpW, rt_h = g_vpH; GLuint rt_tex = 0;
    if (g_curFBO != 0) {
        rt_tex = g_fboColorTex.count(g_curFBO) ? g_fboColorTex[g_curFBO] : 0;
        if (rt_tex && g_textures.count(rt_tex)) { rt_w = g_textures[rt_tex].w; rt_h = g_textures[rt_tex].h; }
    }

    // Bound texture info.
    Tex *bt = g_textures.count(g_boundTex2D) ? &g_textures[g_boundTex2D] : nullptr;

    // Decode geometry: transform each vertex to screen space, track bbox + uv range.
    int pidx = attr_index("in_Position");
    int tidx = attr_index("in_TextureCoord");
    int cidx = attr_index("in_Colour");
    // Fallback heuristic if names weren't bound: first 3f attrib=pos, 2f=uv, 4ub=colour.
    if (pidx < 0 || tidx < 0) {
        for (int i = 0; i < 16; i++) {
            if (!g_attribs[i].enabled) continue;
            if (pidx < 0 && g_attribs[i].size == 3 && g_attribs[i].type == GL_FLOAT) pidx = i;
            else if (tidx < 0 && g_attribs[i].size == 2 && g_attribs[i].type == GL_FLOAT) tidx = i;
            else if (cidx < 0 && g_attribs[i].size == 4 && g_attribs[i].type == GL_UNSIGNED_BYTE) cidx = i;
        }
    }

    // Diagnostics: position attrib source — VBO id, or 0 = client array (ptr).
    GLuint    vb   = (pidx >= 0) ? g_attribs[pidx].buffer : 0;
    uintptr_t vptr = (pidx >= 0) ? g_attribs[pidx].offset : 0;
    const float *mvp = get_mvp();

    float minx=1e9f, miny=1e9f, maxx=-1e9f, maxy=-1e9f, minw=1e9f, maxw=-1e9f;
    float u0=1e9f, u1=-1e9f, v0=1e9f, v1=-1e9f;
    int decoded = 0;
    if (pidx >= 0 && mvp && g_attribs[pidx].enabled) {
        for (int k = 0; k < count; k++) {
            int vi = k;
            if (indices) {
                vi = (idx_type == GL_UNSIGNED_SHORT) ? ((const uint16_t *)indices)[k]
                                                     : ((const uint8_t *)indices)[k];
            }
            float pos[4]; read_attrib(g_attribs[pidx], vi, pos); pos[3]=1.0f;
            float clip[4]; mat4_mul_vec4(mvp, pos, clip);
            float w = clip[3] != 0 ? clip[3] : 1.0f;
            float ndcx = clip[0]/w, ndcy = clip[1]/w;
            float sx = g_vpX + (ndcx*0.5f+0.5f)*g_vpW;
            float sy = g_vpY + (ndcy*0.5f+0.5f)*g_vpH;
            if (sx<minx)minx=sx; if (sx>maxx)maxx=sx;
            if (sy<miny)miny=sy; if (sy>maxy)maxy=sy;
            if (w<minw)minw=w; if (w>maxw)maxw=w;
            if (tidx >= 0) {
                float uv[4]; read_attrib(g_attribs[tidx], vi, uv);
                if(uv[0]<u0)u0=uv[0]; if(uv[0]>u1)u1=uv[0];
                if(uv[1]<v0)v0=uv[1]; if(uv[1]>v1)v1=uv[1];
            }
            decoded++;
        }
    }

    if (g_drawNo <= LOG_FIRST) {
        fprintf(stderr,
            "BLIT draw#%llu %s mode=%d count=%d | rt=%s(%dx%d) tex=%u(%dx%d val=%d) "
            "blend=%s prog=%u attr[p=%d t=%d c=%d] mvp=%d vb=%u clientptr=%d | "
            "screen=[%.0f,%.0f..%.0f,%.0f] uv=[%.2f,%.2f..%.2f,%.2f] decoded=%d/%d\n",
            (unsigned long long)g_drawNo, kind, mode, count,
            g_curFBO ? "FBO" : "DEFAULT", rt_w, rt_h,
            g_boundTex2D, bt?bt->w:0, bt?bt->h:0, bt?bt->valid:0,
            blend_name(), g_curProgram, pidx, tidx, cidx,
            mvp ? 1 : 0, vb, (vb == 0 && vptr != 0) ? 1 : 0,
            decoded?minx:0, decoded?miny:0, decoded?maxx:0, decoded?maxy:0,
            decoded?u0:0, decoded?v0:0, decoded?u1:0, decoded?v1:0, decoded, count);
    }
    return 0;   // step 1: never handle — always fall back to GL
}

int Blitter_TryDrawArrays(GLenum mode, GLint first, GLsizei count) {
    (void)first;
    return decode_and_log("arrays", mode, count, nullptr, 0);
}
int Blitter_TryDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    // indices is a byte offset into the bound element buffer (VBO path).
    const uint8_t *ip = nullptr;
    if (g_buffers.count(g_elemBuffer)) {
        Buf &eb = g_buffers[g_elemBuffer];
        if (eb.data) ip = eb.data + (uintptr_t)indices;
    } else {
        ip = (const uint8_t *)indices;   // client-side indices
    }
    return decode_and_log("elements", mode, count, ip, type);
}

const uint8_t *Blitter_PresentDefault(void) { return nullptr; }  // step 2

#endif // MISTER_NATIVE_VIDEO
