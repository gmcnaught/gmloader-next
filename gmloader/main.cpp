#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <zip.h>

#include "platform.h"
#include "so_util.h"
#include "io_util.h"
#include "jni.h"
#include "jni_internals.h"
#include "classes/RunnerJNILib.h"
#include "khronos/gles2.h"
#include "libyoyo.h"
#include "configuration.h"
#include "texture.h"
#include "video.h"
#include "splash.h"

#ifdef MISTER_NATIVE_VIDEO
#include <dlfcn.h>
#include <libgen.h>
#include "mister/native_video_writer.h"
#include "mister/frame_capture.h"
// Global handle to bundled libGLES_sw.so — also used by egl.cpp and gles2.cpp via extern
void* g_gles_handle = nullptr;
#endif


int relaunch_flag = 0;
char *program_name = nullptr;
const char* gc_workdir = nullptr;
bool override_apk = false;

/*
      Don't touch this incantation. It serves no practical
    reason that you can grep this source code for, nothing
    mentions this on the code base, but disturbing what lies
    here will give you headaches, nausea and sleep loss.
      If you *properly* understand why this fixes a stack smash
    on shader loading code, then please, go ahead and explain
    it to me proper, thanks.
*/
thread_local int tls0[2<<12] = {};
int foo() { return tls0[0]++; }

#define CONFIG_FILE     "config.json"

extern DynLibFunction symtable_libc[];
extern DynLibFunction symtable_zlib[];
extern DynLibFunction symtable_gles2[];

extern double FORCE_PLATFORM;

DynLibFunction *so_static_patches[32] = {
    NULL,
};

DynLibFunction *so_dynamic_libraries[32] = {
    symtable_libc,
    symtable_zlib,
    symtable_gles2,
    NULL
};

so_module *libyoyo = NULL;

int RunnerJNILib_MoveTaskToBackCalled = 0;

static fs::path get_absolute_path(const char* path, fs::path work_dir){

    fs::path fs_path = fs::path(path);
    
    if ( fs_path.is_relative() ){
        fs_path = work_dir / fs_path;
    }

    return fs_path;
}

static void crash_handler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t fault_addr = (uintptr_t)info->si_addr;
    const char *name = sig == SIGSEGV ? "SIGSEGV" :
                       sig == SIGILL  ? "SIGILL"  :
                       sig == SIGBUS  ? "SIGBUS"  : "SIGNAL";
#if defined(__arm__)
    uintptr_t pc = uc->uc_mcontext.arm_pc;
    uintptr_t lr = uc->uc_mcontext.arm_lr;
    fprintf(stderr, "%s: fault_addr=%p PC=%p LR=%p\n", name,
            (void*)fault_addr, (void*)pc, (void*)lr);
    fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
            uc->uc_mcontext.arm_r0, uc->uc_mcontext.arm_r1,
            uc->uc_mcontext.arm_r2, uc->uc_mcontext.arm_r3);
    fprintf(stderr, "  r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
            uc->uc_mcontext.arm_r4, uc->uc_mcontext.arm_r5,
            uc->uc_mcontext.arm_r6, uc->uc_mcontext.arm_r7);
    fprintf(stderr, "  sp=%08lx ip=%08lx fp=%08lx\n",
            uc->uc_mcontext.arm_sp, uc->uc_mcontext.arm_ip,
            uc->uc_mcontext.arm_fp);
    // Dump the instruction at PC so we can see what faulted
    fprintf(stderr, "  insn@PC: %08x\n", *(uint32_t*)(pc & ~3));
#else
    fprintf(stderr, "%s: fault_addr=%p\n", name, (void*)fault_addr);
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char *argv[])
{
    struct sigaction sa = {};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    // Store the program name from argv[0]
    if (argc > 0 && argv[0]) {
        program_name = argv[0];
    } else 
    {
        fatal_error("Main: Could not determine program name from argv[0]\n");
        return -1;
    }

    gmloader_config.init_defaults();

    fs::path work_dir, config_file_path, save_dir, apk_path;
    work_dir = fs::canonical(fs::current_path()) / "";

    // Check for -a (apk_path override) and -c (config file)
    std::string override_apk_path;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            override_apk_path = argv[i + 1];
            warning("Main: Using apk_path override from args: '%s'\n", override_apk_path.c_str());
            i++; // Skip the value
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file_path = work_dir / argv[i + 1];
            if (gmloader_config.parse_file(config_file_path.c_str()) < 0) {
                warning("Error while loading the config file\n");
            }
        }
    }

    // Apply apk_path override if provided
    if (!override_apk_path.empty()) {
        gmloader_config.apk_path = override_apk_path;
        override_apk = true;
    }

    char platform_ov[32];
    strncpy(platform_ov, gmloader_config.force_platform.c_str(), sizeof(platform_ov) - 1);
    platform_ov[sizeof(platform_ov) - 1] = '\0';

    std::unordered_map<std::string, int> platform_map = {
        {"os_unknown", os_unknown},
        {"os_windows", os_windows},
        {"os_macosx", os_macosx},
        {"os_ios", os_ios},
        {"os_android", os_android},
        {"os_linux", os_linux},
        {"os_psvita", os_psvita},
        {"os_ps4", os_ps4},
        {"os_xboxone", os_xboxone},
        {"os_tvos", os_tvos},
        {"os_switch", os_switch}
    };

    std::string platform_str(platform_ov);
    std::transform(platform_str.begin(), platform_str.end(), platform_str.begin(), ::tolower);
    auto it = platform_map.find(platform_str);
    if (it != platform_map.end()) {
        FORCE_PLATFORM = it->second;
    } else {
        warning("Unexpected platform '%s'.\n", platform_ov);
        strcpy(platform_ov, "os_unknown");
        FORCE_PLATFORM = os_unknown;
    }

    save_dir = get_absolute_path(gmloader_config.save_dir.c_str(), work_dir) / "";
    apk_path = get_absolute_path(gmloader_config.apk_path.c_str(), work_dir);

    int err;
    zip_t *apk = zip_open(apk_path.c_str(), ZIP_RDONLY, &err);
    if (apk == NULL) {
        zip_error_t zerror;
        zip_error_init_with_code(&zerror, err);
        fatal_error("Failure opening APK '%s': %s\n", apk_path.c_str(), zip_error_strerror(&zerror));
        zip_error_fini(&zerror);
        return -1;
    }

    // Create the Fake JavaVM environment
    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK) {
        fatal_error("Error initializing JNI Interface!\n");
        return -1;
    }

    const char *dumpdir = getenv("GMLOADER_DUMP_DIR");
    const char *alt_searchpath = getenv("GMLOADER_LIB_PATH");
    so_set_options(dumpdir, alt_searchpath);

    warning("Loading images...\n");
    libyoyo = so_load_module("libyoyo.so", apk, (void*)vm);
    if (libyoyo == NULL) {
        fatal_error("Could not load libyoyo.so!\n");
        return -1;
    }
    warning("DBG: so_load_module done\n");

    patch_libyoyo(libyoyo);
    warning("DBG: patch_libyoyo done\n");
    if(gmloader_config.disable_depth == 1) {
        disable_depth();
    }
    patch_input(libyoyo);
    patch_gamepad(libyoyo);
    patch_mouse(libyoyo);
    patch_fmod(libyoyo);
    patch_display_mouse_lock(libyoyo);
    patch_gameframe(libyoyo);
    patch_psn(libyoyo);
    patch_steam(libyoyo);
    patch_texture(libyoyo);
    patch_lua(libyoyo);
    warning("DBG: all patches done\n");

    warning("DBG: before NewStringUTF apk_path\n");
    String *apk_path_arg = (String *)env->NewStringUTF(apk_path.c_str());
    warning("DBG: before NewStringUTF save_dir\n");
    String *save_dir_arg = (String *)env->NewStringUTF(save_dir.c_str());
    warning("DBG: before NewStringUTF pkg_dir\n");
    String *pkg_dir_arg = (String *)env->NewStringUTF("com.johnny.loader");
    warning("DBG: after NewStringUTF calls\n");
    printf("apk_path %s save_dir %s pkg_dir %s\n", apk_path_arg->str, save_dir_arg->str, pkg_dir_arg->str);

#ifdef MISTER_NATIVE_VIDEO
    // Build path to bundled libGLES_sw.so relative to the binary (same dir as argv[0])
    {
        warning("DBG: entering MISTER_NATIVE_VIDEO block\n");
        char argv0_buf[4096];
        strncpy(argv0_buf, argv[0], sizeof(argv0_buf) - 1);
        argv0_buf[sizeof(argv0_buf) - 1] = '\0';
        char *bin_dir = dirname(argv0_buf);
        char lib_path[4096];
        char mesa_dir[4096];
        snprintf(mesa_dir, sizeof(mesa_dir), "%s/mesa", bin_dir);

        // Standalone Mesa 21.3 (no GLVND, no X11/Wayland/GBM, softpipe/no-LLVM).
        // EGL_PLATFORM=surfaceless: use the Mesa surfaceless headless backend.
        // LIBGL_DRIVERS_PATH: DRI loader finds swrast_dri.so here.
        setenv("EGL_PLATFORM", "surfaceless", 0);
        setenv("LIBGL_DRIVERS_PATH", mesa_dir, 0);
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
        warning("DBG: set EGL_PLATFORM=surfaceless, LIBGL_DRIVERS_PATH=%s\n", mesa_dir);

        // Pre-load Mesa runtime libs from mesa/ subdir so the dynamic linker finds
        // them before attempting system paths (MiSTer has no system Mesa).
        static const char* const preload_libs[] = {
            "libdrm.so.2", "libglapi.so.0", "libEGL.so.1", NULL
        };
        for (int i = 0; preload_libs[i]; i++) {
            // Try mesa/ subdir first
            snprintf(lib_path, sizeof(lib_path), "%s/%s", mesa_dir, preload_libs[i]);
            void *h = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
            if (h) {
                warning("DBG: preloaded mesa/%s ok\n", preload_libs[i]);
            } else {
                warning("DBG: mesa/%s fail: %s\n", preload_libs[i], dlerror());
                // Fall back to bin_dir
                snprintf(lib_path, sizeof(lib_path), "%s/%s", bin_dir, preload_libs[i]);
                h = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
                warning("DBG: bin_dir/%s %s\n", preload_libs[i], h ? "ok" : "fail");
            }
        }
        snprintf(lib_path, sizeof(lib_path), "%s/libGLES_sw.so", bin_dir);
        warning("DBG: dlopen libGLES_sw.so at %s\n", lib_path);
        g_gles_handle = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
        if (!g_gles_handle) {
            fatal_error("Cannot load libGLES_sw.so: %s\n", dlerror());
            return -1;
        }
        warning("Loaded bundled GLES library: %s\n", lib_path);
    }
#endif

    // In MISTER_NATIVE_VIDEO mode the real display is the DDR framebuffer; use SDL's
    // dummy video driver so SDL_Init doesn't fail looking for a display device.
    // Audio and joystick still go through real drivers.
#ifdef MISTER_NATIVE_VIDEO
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    warning("DBG: SDL_VIDEODRIVER=dummy for MISTER_NATIVE_VIDEO\n");
#endif

    // Initialize SDL with video, audio, joystick, and controller support
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0) {
        fatal_error("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    if(gmloader_config.show_cursor == 0) {
        if (SDL_ShowCursor(SDL_DISABLE) < 0) {
            warning("Cannot disable cursor: %s\n", SDL_GetError());
        } else {
            printf("Cursor disabled\n");
        }
    }

    SDL_Window *sdl_win;
#ifndef MISTER_NATIVE_VIDEO
    SDL_GLContext sdl_ctx;
#endif

    SDL_DisplayMode mode = {};
    if (SDL_GetDesktopDisplayMode(0, &mode) != 0) {
        warning("Unable to query display mode, using defaults.");
        mode.w = 640;
        mode.h = 480;
    }

#ifdef MISTER_NATIVE_VIDEO
    // Under MISTER_NATIVE_VIDEO the SDL window is for input/audio only — no OpenGL flag
    sdl_win = SDL_CreateWindow("GMLoader", 0, 0, mode.w, mode.h,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
#else
    sdl_win = SDL_CreateWindow("GMLoader", 0, 0, mode.w, mode.h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
#endif
    if (sdl_win == NULL) {
        fatal_error("Failed to create SDL Window: %s\n", SDL_GetError());
        return -1;
    }

#ifndef MISTER_NATIVE_VIDEO
    // Basic OpenGL ES 2.x setup — skip entirely under MISTER_NATIVE_VIDEO to
    // avoid SDL warnings about setting GL attributes on a non-GL window
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    sdl_ctx = SDL_GL_CreateContext(sdl_win);
    if (sdl_ctx == NULL) {
        fatal_error("Failed to create OpenGL Context: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_MakeCurrent(sdl_win, sdl_ctx);
#else
    // EGL headless pbuffer context via bundled libGLES_sw.so
    {
        // Resolve core EGL entry points. libGLES_sw.so is a GLES2 dispatch wrapper
        // and does not export EGL symbols; those live in the preloaded libEGL.so.1
        // (loaded with RTLD_GLOBAL above).  Use RTLD_DEFAULT to search the global
        // symbol namespace rather than a specific handle.
        auto pfnGetDisplay   = (EGLDisplay(*)(EGLNativeDisplayType))dlsym(RTLD_DEFAULT, "eglGetDisplay");
        auto pfnInitialize   = (EGLBoolean(*)(EGLDisplay, EGLint*, EGLint*))dlsym(RTLD_DEFAULT, "eglInitialize");
        auto pfnChooseConfig = (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))dlsym(RTLD_DEFAULT, "eglChooseConfig");
        auto pfnCreateCtx    = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))dlsym(RTLD_DEFAULT, "eglCreateContext");
        auto pfnCreatePbuf   = (EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint*))dlsym(RTLD_DEFAULT, "eglCreatePbufferSurface");
        auto pfnMakeCurrent  = (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))dlsym(RTLD_DEFAULT, "eglMakeCurrent");
        auto pfnBindAPI      = (EGLBoolean(*)(EGLenum))dlsym(RTLD_DEFAULT, "eglBindAPI");
        warning("DBG: EGL syms: GetDisplay=%p Init=%p Choose=%p\n",
                (void*)pfnGetDisplay, (void*)pfnInitialize, (void*)pfnChooseConfig);

        if (!pfnGetDisplay || !pfnInitialize || !pfnChooseConfig ||
            !pfnCreateCtx  || !pfnCreatePbuf  || !pfnMakeCurrent) {
            fatal_error("Failed to resolve core EGL symbols from libGLES_sw.so\n");
            return -1;
        }

        // This Mesa has EGL_EXT_platform_device but not surfaceless.
        // Extension function pointers must be fetched via eglGetProcAddress (not dlsym).
        // Use eglQueryDevicesEXT + eglGetPlatformDisplayEXT(DEVICE_EXT) for headless.
#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif
        auto pfnGetProcAddr = (void*(*)(const char*))dlsym(RTLD_DEFAULT, "eglGetProcAddress");
        auto pfnQueryDevicesEXT       = pfnGetProcAddr ?
            (EGLBoolean(*)(EGLint, void**, EGLint*))pfnGetProcAddr("eglQueryDevicesEXT") : nullptr;
        auto pfnGetPlatformDisplayEXT = pfnGetProcAddr ?
            (EGLDisplay(*)(EGLenum, void*, const EGLint*))pfnGetProcAddr("eglGetPlatformDisplayEXT") : nullptr;
        warning("DBG: eglGetProcAddress=%p QueryDevices=%p GetPlatformDisplay=%p\n",
                (void*)pfnGetProcAddr, (void*)pfnQueryDevicesEXT, (void*)pfnGetPlatformDisplayEXT);

        EGLDisplay egl_dpy = EGL_NO_DISPLAY;
        if (pfnQueryDevicesEXT && pfnGetPlatformDisplayEXT) {
            void* egl_devices[4] = {};
            EGLint num_devices = 0;
            if (pfnQueryDevicesEXT(4, egl_devices, &num_devices) && num_devices > 0) {
                warning("DBG: EGL_PLATFORM_DEVICE: %d device(s)\n", num_devices);
                egl_dpy = pfnGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, egl_devices[0], NULL);
                warning("DBG: eglGetPlatformDisplayEXT(DEVICE_0) = %p\n", (void*)egl_dpy);
            } else {
                warning("DBG: eglQueryDevicesEXT returned %d devices\n", num_devices);
            }
        }
        auto pfnGetError = (EGLint(*)(void))dlsym(RTLD_DEFAULT, "eglGetError");
        #ifndef EGL_PLATFORM_SURFACELESS_MESA
        #define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
        #endif
        // Preferred headless path for a software Mesa with no /dev/dri:
        // the surfaceless platform via eglGetPlatformDisplay.
        if (egl_dpy == EGL_NO_DISPLAY && pfnGetPlatformDisplayEXT) {
            egl_dpy = pfnGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
            warning("DBG: eglGetPlatformDisplay(SURFACELESS_MESA) = %p\n", (void*)egl_dpy);
        }
        if (egl_dpy == EGL_NO_DISPLAY) {
            warning("DBG: falling back to eglGetDisplay(DEFAULT)\n");
            egl_dpy = pfnGetDisplay(EGL_DEFAULT_DISPLAY);
            warning("DBG: eglGetDisplay(DEFAULT) = %p\n", (void*)egl_dpy);
        }

        EGLint egl_major = 0, egl_minor = 0;
        // NOTE: fatal_error() only prints — it does NOT exit. Without an explicit
        // return, a failed eglInitialize cascades through every following EGL call
        // and ends in a null GL context -> SIGSEGV deep inside Mesa. Bail out here.
        if (!pfnInitialize(egl_dpy, &egl_major, &egl_minor)) {
            EGLint e = pfnGetError ? pfnGetError() : 0;
            fatal_error("eglInitialize failed (dpy=%p eglGetError=0x%04x)\n", (void*)egl_dpy, e);
            return -1;
        }
        if (pfnBindAPI) pfnBindAPI(EGL_OPENGL_ES_API);

        static const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,   8,  EGL_GREEN_SIZE,  8,
            EGL_BLUE_SIZE,  8,  EGL_ALPHA_SIZE,  8,
            EGL_DEPTH_SIZE, 16, EGL_NONE
        };
        EGLConfig egl_cfg; EGLint ncfg = 0;
        pfnChooseConfig(egl_dpy, cfg_attribs, &egl_cfg, 1, &ncfg);
        if (ncfg == 0) { fatal_error("eglChooseConfig: no matching config\n"); return -1; }

        static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext egl_ctx = pfnCreateCtx(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (egl_ctx == EGL_NO_CONTEXT) { fatal_error("eglCreateContext failed\n"); return -1; }

        static const EGLint pbuf_attribs[] = { EGL_WIDTH, MISTER_WIDTH, EGL_HEIGHT, MISTER_HEIGHT, EGL_NONE };
        EGLSurface egl_surf = pfnCreatePbuf(egl_dpy, egl_cfg, pbuf_attribs);
        if (egl_surf == EGL_NO_SURFACE) { fatal_error("eglCreatePbufferSurface failed\n"); return -1; }

        if (!pfnMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) {
            fatal_error("eglMakeCurrent failed\n");
            return -1;
        }

        warning("EGL headless context created: %d.%d, pbuffer %dx%d\n",
                egl_major, egl_minor, MISTER_WIDTH, MISTER_HEIGHT);
    }

    if (!NativeVideoWriter_Init()) {
        warning("NativeVideoWriter: /dev/mem unavailable, using SDL fallback\n");
    }
    SDL_Renderer* sdl_renderer = nullptr;
    SDL_Texture*  sdl_texture  = nullptr;
    if (!NativeVideoWriter_IsActive()) {
        sdl_renderer = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_SOFTWARE);
        if (!sdl_renderer)
            fatal_error("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        sdl_texture = SDL_CreateTexture(sdl_renderer,
                                        SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        MISTER_WIDTH, MISTER_HEIGHT);
        if (!sdl_texture)
            fatal_error("SDL_CreateTexture failed: %s\n", SDL_GetError());
    }
#endif

    // The libraries are loaded by SDL2.0, and the API entry points by the following
    // functions using the GLAD generated headers.
    load_egl_funcs();
    load_gles2_funcs();
    const char *dump_shaders = getenv("GMLOADER_DUMP_SHADERS");
    set_gles2_shader_override_dir(gmloader_config.shader_dir.c_str(), dump_shaders && *dump_shaders != '\0');

    int cont = 1;
    int w, h;

    #ifdef VIDEO_SUPPORT
    if (video_init(sdl_win, save_dir.c_str()) != 0)
    {
        fatal_error("Could not initialize Video Playback.\n");
        return -1;
    }
    #endif

    int sw, sh;
    SDL_GetWindowSize(sdl_win, &sw, &sh);
    splash_render(apk, sw, sh, sdl_win);

    RunnerJNILib::Startup(env, 0, apk_path_arg, save_dir_arg, pkg_dir_arg, 4, 0);
    setup_ended = 1;

#ifdef MISTER_NATIVE_VIDEO
    {
        int init_w, init_h;
        SDL_GetWindowSize(sdl_win, &init_w, &init_h);
        FrameCapture_Init(init_w, init_h);
    }
#endif

    while (cont != 0 && cont != 2 && RunnerJNILib_MoveTaskToBackCalled == 0 && relaunch_flag == 0) {
        #ifdef VIDEO_SUPPORT
        video_process();
        #endif
        if (update_inputs(sdl_win) != 1)
            break;
        SDL_GetWindowSize(sdl_win, &w, &h);
#ifdef MISTER_NATIVE_VIDEO
        cont = RunnerJNILib::Process(env, 0, MISTER_WIDTH, MISTER_HEIGHT, 0, 0, 0, 0, 0, 60);
        if (RunnerJNILib::canFlip(env, 0)) {
            FrameCapture_ReadFrame();

            // glReadPixels returns bottom-row-first; flip to top-row-first for FPGA/SDL
            {
                static uint8_t flip_row[MISTER_WIDTH * 4];
                uint8_t* buf = const_cast<uint8_t*>(FrameCapture_GetRGBA());
                for (int top = 0, bot = MISTER_HEIGHT - 1; top < bot; top++, bot--) {
                    uint8_t* row_top = buf + top * MISTER_WIDTH * 4;
                    uint8_t* row_bot = buf + bot * MISTER_WIDTH * 4;
                    memcpy(flip_row, row_top, MISTER_WIDTH * 4);
                    memcpy(row_top, row_bot, MISTER_WIDTH * 4);
                    memcpy(row_bot, flip_row, MISTER_WIDTH * 4);
                }
            }

            if (NativeVideoWriter_IsActive()) {
                static uint16_t rgb565_buf[MISTER_WIDTH * MISTER_HEIGHT];
                FrameCapture_ConvertToRGB565(rgb565_buf);
                NativeVideoWriter_WriteFrame(rgb565_buf, MISTER_WIDTH, MISTER_HEIGHT,
                                            MISTER_WIDTH * 2);
            } else {
                SDL_UpdateTexture(sdl_texture, NULL,
                                  FrameCapture_GetRGBA(), MISTER_WIDTH * 4);
                SDL_RenderClear(sdl_renderer);
                SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, NULL);
                SDL_RenderPresent(sdl_renderer);
            }
        }
#else
        cont = RunnerJNILib::Process(env, 0, w, h, 0, 0, 0, 0, 0, 60);
        if (RunnerJNILib::canFlip(env, 0))
            SDL_GL_SwapWindow(sdl_win);
#endif
    }

#ifdef MISTER_NATIVE_VIDEO
    NativeVideoWriter_Shutdown();
    if (sdl_texture)  SDL_DestroyTexture(sdl_texture);
    if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
#endif
#ifndef MISTER_NATIVE_VIDEO
    SDL_GL_DeleteContext(sdl_ctx);
#endif
    SDL_DestroyWindow(sdl_win);
    SDL_Quit();

    // Check for relaunch
    if (relaunch_flag && gc_workdir) {
        warning("Main: Relaunch triggered. workdir='%s'\n", gc_workdir);

        // Extract subfolder prefix from original apk_path (e.g., "assets/")
        std::string orig_apk_path = gmloader_config.apk_path;
        std::string prefix;
        size_t last_slash = orig_apk_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            prefix = orig_apk_path.substr(0, last_slash + 1); // Include the slash
        }

        // Remove leading and trailing slashes from gc_workdir (may or may not have any)
        std::string workdir_clean = gc_workdir;
        if (!workdir_clean.empty() && workdir_clean.front() == '/')
            workdir_clean = workdir_clean.substr(1);
        if (!workdir_clean.empty() && workdir_clean.back() == '/')
            workdir_clean = workdir_clean.substr(0, workdir_clean.length() - 1);
        std::string new_apk_path = prefix + workdir_clean;

        // Check if the override is valid
        bool use_override = (new_apk_path.find("..") == std::string::npos && !workdir_clean.empty());
        if (use_override) {
            gmloader_config.apk_path = new_apk_path;
            warning("Main: Updated config: apk_path='%s', save_dir='%s'\n", 
                    gmloader_config.apk_path.c_str(), gmloader_config.save_dir.c_str());
        } else {
            warning("Main: Ignoring override='%s' (workdir='%s'), using original apk_path='%s'\n", 
                    new_apk_path.c_str(), gc_workdir, gmloader_config.apk_path.c_str());
        }

        // Relaunch: use -a only if override is valid, otherwise just -c
        char apk_path_arg[1024];
        char config_path_arg[1024];
        char *argv_relaunch[6];
        int arg_count = 0;
        argv_relaunch[arg_count++] = program_name;
        argv_relaunch[arg_count++] = (char*)"-c";
        snprintf(config_path_arg, sizeof(config_path_arg), "%s", config_file_path.c_str());
        argv_relaunch[arg_count++] = config_path_arg;
        if (use_override) {
            snprintf(apk_path_arg, sizeof(apk_path_arg), "%s", gmloader_config.apk_path.c_str());
            argv_relaunch[arg_count++] = (char*)"-a";
            argv_relaunch[arg_count++] = apk_path_arg;
        }
        argv_relaunch[arg_count] = nullptr;

        warning("Main: Relaunching '%s' with apk_path='%s', save_dir='%s'\n", 
                program_name, gmloader_config.apk_path.c_str(), gmloader_config.save_dir.c_str());
        fflush(stdout);
        fflush(stderr);
        if (execv(program_name, argv_relaunch) == -1) {
            fatal_error("Main: Failed to relaunch '%s': %s\n", program_name, strerror(errno));
            return -1;
        }
    }

    return 0;
}
