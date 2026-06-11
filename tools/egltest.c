// Standalone EGL headless probe for the MiSTer Mesa build.
// Loads libEGL.so.1 via dlopen, reports client platform extensions, and tries
// each headless display path, printing eglGetError() so we know exactly what
// the deployed Mesa supports — before deciding what to rebuild.
//
// cc -o egltest egltest.c -ldl
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>

typedef void* EGLDisplay;
typedef void* EGLNativeDisplayType;
typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef unsigned int EGLenum;
#define EGL_NO_DISPLAY        ((EGLDisplay)0)
#define EGL_DEFAULT_DISPLAY   ((EGLNativeDisplayType)0)
#define EGL_EXTENSIONS        0x3055
#define EGL_VERSION           0x3054
#define EGL_VENDOR            0x3053
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_PLATFORM_DEVICE_EXT       0x313F
#define EGL_PLATFORM_GBM_KHR          0x31D7

int main(void) {
    void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("dlopen libEGL.so.1 FAILED: %s\n", dlerror()); return 1; }

    EGLDisplay (*eglGetDisplay)(EGLNativeDisplayType) = dlsym(h, "eglGetDisplay");
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*) = dlsym(h, "eglInitialize");
    const char* (*eglQueryString)(EGLDisplay, EGLint) = dlsym(h, "eglQueryString");
    EGLint (*eglGetError)(void) = dlsym(h, "eglGetError");
    void* (*eglGetProcAddress)(const char*) = dlsym(h, "eglGetProcAddress");

    printf("eglGetDisplay=%p eglInitialize=%p eglQueryString=%p eglGetProcAddress=%p\n",
           (void*)eglGetDisplay, (void*)eglInitialize, (void*)eglQueryString, (void*)eglGetProcAddress);

    // Client (no-display) extensions: tells us which platforms are compiled in.
    const char *client_exts = eglQueryString ? eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) : NULL;
    printf("CLIENT EXTENSIONS: %s\n", client_exts ? client_exts : "(null)");

    EGLDisplay (*eglGetPlatformDisplay)(EGLenum, void*, const EGLint*) =
        eglGetProcAddress ? (EGLDisplay(*)(EGLenum,void*,const EGLint*))eglGetProcAddress("eglGetPlatformDisplayEXT") : NULL;
    printf("eglGetPlatformDisplayEXT=%p\n", (void*)eglGetPlatformDisplay);

    struct { const char *name; EGLenum plat; } plats[] = {
        {"SURFACELESS_MESA", EGL_PLATFORM_SURFACELESS_MESA},
        {"DEVICE_EXT",       EGL_PLATFORM_DEVICE_EXT},
        {"GBM_KHR",          EGL_PLATFORM_GBM_KHR},
    };
    for (int i = 0; i < 3; i++) {
        if (!eglGetPlatformDisplay) break;
        EGLDisplay d = eglGetPlatformDisplay(plats[i].plat, EGL_DEFAULT_DISPLAY, NULL);
        EGLint ge = eglGetError ? eglGetError() : 0;
        printf("getPlatformDisplay(%s)=%p err=0x%04x", plats[i].name, (void*)d, ge);
        if (d != EGL_NO_DISPLAY && eglInitialize) {
            EGLint mj=0, mn=0;
            EGLBoolean ok = eglInitialize(d, &mj, &mn);
            EGLint ie = eglGetError ? eglGetError() : 0;
            printf("  -> initialize ok=%d %d.%d err=0x%04x", ok, mj, mn, ie);
            if (ok) printf("  VERSION='%s' VENDOR='%s'",
                           eglQueryString(d, EGL_VERSION), eglQueryString(d, EGL_VENDOR));
        }
        printf("\n");
    }

    // Plain eglGetDisplay(DEFAULT)
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("eglGetDisplay(DEFAULT)=%p\n", (void*)d);
    if (d != EGL_NO_DISPLAY) {
        EGLint mj=0, mn=0;
        EGLBoolean ok = eglInitialize(d, &mj, &mn);
        printf("  initialize ok=%d %d.%d err=0x%04x\n", ok, mj, mn, eglGetError ? eglGetError() : 0);
        if (ok) printf("  VERSION='%s' VENDOR='%s'\n",
                       eglQueryString(d, EGL_VERSION), eglQueryString(d, EGL_VENDOR));
    }
    return 0;
}
