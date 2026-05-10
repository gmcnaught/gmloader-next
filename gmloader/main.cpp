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

int main(int argc, char *argv[])
{
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

    patch_libyoyo(libyoyo);
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

    String *apk_path_arg = (String *)env->NewStringUTF(apk_path.c_str());
    String *save_dir_arg = (String *)env->NewStringUTF(save_dir.c_str());
    String *pkg_dir_arg = (String *)env->NewStringUTF("com.johnny.loader");
    printf("apk_path %s save_dir %s pkg_dir %s\n", apk_path_arg->str, save_dir_arg->str, pkg_dir_arg->str);

#ifdef MISTER_NATIVE_VIDEO
    // Build path to bundled libGLES_sw.so relative to the binary (same dir as argv[0])
    {
        char argv0_buf[4096];
        strncpy(argv0_buf, argv[0], sizeof(argv0_buf) - 1);
        argv0_buf[sizeof(argv0_buf) - 1] = '\0';
        char *bin_dir = dirname(argv0_buf);
        char lib_path[4096];
        snprintf(lib_path, sizeof(lib_path), "%s/libGLES_sw.so", bin_dir);
        g_gles_handle = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
        if (!g_gles_handle) {
            fatal_error("Cannot load libGLES_sw.so: %s\n", dlerror());
            return -1;
        }
        warning("Loaded bundled GLES library: %s\n", lib_path);
    }
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
        // Resolve core EGL entry points directly from the bundled library
        auto pfnGetDisplay   = (EGLDisplay(*)(EGLNativeDisplayType))dlsym(g_gles_handle, "eglGetDisplay");
        auto pfnInitialize   = (EGLBoolean(*)(EGLDisplay, EGLint*, EGLint*))dlsym(g_gles_handle, "eglInitialize");
        auto pfnChooseConfig = (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))dlsym(g_gles_handle, "eglChooseConfig");
        auto pfnCreateCtx    = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))dlsym(g_gles_handle, "eglCreateContext");
        auto pfnCreatePbuf   = (EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint*))dlsym(g_gles_handle, "eglCreatePbufferSurface");
        auto pfnMakeCurrent  = (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))dlsym(g_gles_handle, "eglMakeCurrent");
        auto pfnBindAPI      = (EGLBoolean(*)(EGLenum))dlsym(g_gles_handle, "eglBindAPI");

        if (!pfnGetDisplay || !pfnInitialize || !pfnChooseConfig ||
            !pfnCreateCtx  || !pfnCreatePbuf  || !pfnMakeCurrent) {
            fatal_error("Failed to resolve core EGL symbols from libGLES_sw.so\n");
            return -1;
        }

        EGLDisplay egl_dpy = pfnGetDisplay(EGL_NO_DISPLAY);
        EGLint egl_major, egl_minor;
        if (!pfnInitialize(egl_dpy, &egl_major, &egl_minor))
            fatal_error("eglInitialize failed\n");
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
        if (ncfg == 0) fatal_error("eglChooseConfig: no matching config\n");

        static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext egl_ctx = pfnCreateCtx(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (egl_ctx == EGL_NO_CONTEXT) fatal_error("eglCreateContext failed\n");

        static const EGLint pbuf_attribs[] = { EGL_WIDTH, MISTER_WIDTH, EGL_HEIGHT, MISTER_HEIGHT, EGL_NONE };
        EGLSurface egl_surf = pfnCreatePbuf(egl_dpy, egl_cfg, pbuf_attribs);
        if (egl_surf == EGL_NO_SURFACE) fatal_error("eglCreatePbufferSurface failed\n");

        if (!pfnMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx))
            fatal_error("eglMakeCurrent failed\n");

        warning("EGL headless context created: %d.%d, pbuffer %dx%d\n",
                egl_major, egl_minor, MISTER_WIDTH, MISTER_HEIGHT);
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

    while (cont != 0 && cont != 2 && RunnerJNILib_MoveTaskToBackCalled == 0 && relaunch_flag == 0) {
        #ifdef VIDEO_SUPPORT
        video_process();
        #endif
        if (update_inputs(sdl_win) != 1)
            break;
        SDL_GetWindowSize(sdl_win, &w, &h);
        cont = RunnerJNILib::Process(env, 0, w, h, 0, 0, 0, 0, 0, 60);
#ifndef MISTER_NATIVE_VIDEO
        if (RunnerJNILib::canFlip(env, 0))
            SDL_GL_SwapWindow(sdl_win);
#endif
    }

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
