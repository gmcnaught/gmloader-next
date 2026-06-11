# gmloader-next — MiSTer FPGA SDL Buffer Output

## Build

Cross-compiles for ARM32 inside Docker. Docker is at `/opt/homebrew/bin/docker`.

```bash
# Build gmloader ARM32 binary (use linux/amd64, NOT arm32v7 — QEMU not available)
# Note: build_mister_arm.sh has multiarch setup in wrong order; use inline build:
/opt/homebrew/bin/docker run --rm --platform linux/amd64 \
  -v "$(pwd):/src" -w /src debian:bullseye-slim bash -c '
  apt-get update -qq && dpkg --add-architecture armhf && apt-get update -qq
  apt-get install -y -qq build-essential git python3 python3-clang pkg-config make \
    binutils-arm-linux-gnueabihf gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
    libstdc++-10-dev-armhf-cross linux-libc-dev-armhf-cross \
    libsdl2-dev:armhf zlib1g-dev:armhf ca-certificates
  touch thunks/thunk_gen_dyn.h  # force rebuild of header-dependent files
  make -f Makefile.gmloader ARCH=arm-linux-gnueabihf MISTER_BUILD=1 MISTER_NATIVE_VIDEO=1 \
    "LLVM_INC=/usr/arm-linux-gnueabihf/include /usr/arm-linux-gnueabihf/include/c++/10/arm-linux-gnueabihf" \
    -j$(nproc)'
# Output: build/arm-linux-gnueabihf/gmloader/gmloadernext.armhf
# Deploy: scp build/arm-linux-gnueabihf/gmloader/gmloadernext.armhf root@192.168.20.81:/media/fat/games/gmloader/gmloader
```

For `MISTER_NATIVE_VIDEO` builds pass `MISTER_NATIVE_VIDEO=1` to `make`.

## Mesa (soft GL for MISTER_NATIVE_VIDEO)

Mesa 21.3.9 cross-built for armhf. Build script: `/Users/gmcnaught/mesa-build/build_inner.sh`.
Run in `arm64v8/ubuntu:focal` (NOT bookworm — focal targets glibc 2.31; bookworm produces GLIBC_2.34+ symbols that MiSTer's Buildroot cannot load).
Verify after build: `objdump -p <lib>.so | grep GLIBC` — must show only `GLIBC_2.31` or lower.

Build output: `/Users/gmcnaught/mesa-build/mesa-built/`. Deploy the **standalone surfaceless**
build (`-Dglvnd=false -Degl-native-platform=surfaceless -Dgallium-drivers=swrast`), NOT a GLVND one.
A GLVND libEGL pulls in `libGLdispatch.so.0` and has **no surfaceless platform** → every
`eglGetDisplay`/`eglGetPlatformDisplay` returns nil → null GL context → Mesa SIGSEGV.
Sanity check the deployed libEGL: `tools/egltest.c` (cross-compile, run on MiSTer) must list
`EGL_MESA_platform_surfaceless` in client extensions. Correct libEGL.so.1 md5: `dd4c60ee…`.

## MiSTer Deploy — full runtime

MiSTer IP `192.168.20.81`; deploy root `/media/fat/games/gmloader/`. The whole tree can vanish if
the SD card is reflashed — the full set must be present (reconstruct from sources below):

```
gmloader              # fresh 11MB armhf build (build/.../gmloadernext.armhf), chmod +x
gmloader.json         # apk_path = "mygame.apk"
mygame.apk            # PortMaster: ports/maldita.castilla/maldita.castilla/malditacastilla.apk
                      #   (APK has only assets/splash.png — NO game data)
lib/armeabi-v7a/      # replacement libs; only libstdc++.so matters (from games/gmloader/libs/).
                      #   Loader finds non-builtin DT_NEEDED libs here (cwd-relative).
                      #   libc/dl/log/android/z/m are builtin and skipped.
saves/game.droid      # 49MB game data, from PortMaster .../maldita.castilla/gamedata/
saves/options.ini     #   (runner looks for game.droid in saves/)
mesa/                 # standalone surfaceless Mesa from mesa-built/: libEGL.so.1, libGLESv2.so.2,
                      #   libglapi.so.0, swrast_dri.so, libdrm.so.2 (copy the .X.Y.Z files to soname)
libGLES_sw.so         # = mesa-built/libGLESv2.so.2 (clean, needs only libglapi). NOT the fat
                      #   games/gmloader/libGLES_sw.so (drags in libgbm/X11/wayland — absent on device)
```

Device already has libexpat/libstdc++/libgcc_s/libz in `/usr/lib`. No `libgbm.so.1`, no `/dev/dri`.
SSH key auth: if scp fails with "Permission denied", the key needs (re)adding to the MiSTer.
Overwriting `gmloader` fails with "dest open Failure" / ETXTBSY if it's still running —
`ssh … 'pkill -9 -f "gmloader -c"'` first (no `pgrep` on MiSTer; use `ps w | grep`).

## Running on MiSTer

```bash
# Config is NOT auto-discovered — must pass -c explicitly:
ssh root@192.168.20.81 'cd /media/fat/games/gmloader && \
  LD_LIBRARY_PATH=/media/fat/games/gmloader/mesa:/media/fat/games/gmloader \
  ./gmloader -c gmloader.json'
```

Verify frames reach DDR: while it runs, `busybox devmem 0x3A000000` should **increment** (the
NativeVideoWriter frame counter). Success markers in the log: `OpenGL: version string OpenGL ES
3.1 Mesa 21.3.9`, `Entering main loop.`

## GLES/EGL Gotchas (MISTER_NATIVE_VIDEO)

- `eglGetProcAddress` fallback must use `dlsym(RTLD_DEFAULT, "eglGetProcAddress")` — libGLESv2.so.2 does NOT export it; only libEGL.so.1 does.
- GameMaker Android runner calls `eglGetProcAddress("glGenFramebuffersOES")` etc. Mesa doesn't export OES-suffixed names. Add aliases in `symtable_gles2` (see `thunks/khronos/gles2.cpp` end of `load_gles2_funcs()`).
- To debug unknown NULL extension lookups: add `fprintf(stderr, "DBG eglGetProcAddress NULL: '%s'\n", procname)` in `eglGetProcAddress_impl` before returning NULL.
- Runner (libyoyo.so) resolves `eglGetProcAddress` via Mesa's libEGL.so.1 (RTLD_GLOBAL), bypassing our thunk — logging in `eglGetProcAddress_impl` won't catch these calls.
- To find which static GL symbols Mesa can't provide: add `fprintf(stderr, "DBG resolve NULL: %.64s\n", symbol)` in `resolve_thunked` in `thunks/thunk_gen_dyn.h` when `f == NULL`. Filter output with `grep "DBG resolve NULL" | grep -v "AMD\|ANGLE\|APPLE\|EXT\|NV\|OES"` to see only core function failures.
- libyoyo.so crash in `Graphics::SetRenderState` (ELF offset 0x15d670): caused by GL1 functions (glAlphaFunc, glShadeModel, etc.) absent in GLES2 — `dlsym_impl` returned NULL, GLFuncImport stored NULL in BSS FuncPtr_ slots, SetRenderState crashed on BLX. Fixed in `thunks/libc/misc.cpp` `dlsym_impl`: returns `gl_missing_stub` (no-op) for unresolved `gl*` names instead of NULL. The address 0x334bbc is the PLT GOT (.got.plt), NOT a GL dispatch table — patching it does nothing.
- GDB doesn't work for crash diagnosis — hook trampolines use SIGILL; even `handle SIGILL nostop pass` kills the program. Use `fprintf` logging instead. `main.cpp` installs a `crash_handler` for SIGILL/SIGBUS/SIGSEGV that dumps regs + the faulting instruction — read PC relative to the libyoyo base (0x40000000) to locate the fault.
- `Function_Add` rehook SIGILLs on pre-2024.14 runners (e.g. 2018 GMS1.x v1.4.1567). The reentrant rehook only exists to dedup on 2024.14+ runners. `patch_libyoyo` only installs it when the modern const-char* mangling `_Z12Function_AddPKc...` is present; older runners use the char* mangling `_Z12Function_AddPc...` and are left alone.
- Headless EGL on MiSTer: there is **no `/dev/dri`**, so `eglQueryDevicesEXT` returns 0 devices. The only working path is the Mesa **surfaceless** platform + swrast (`EGL_PLATFORM=surfaceless`, `LIBGL_DRIVERS_PATH=mesa`, set by main.cpp). Requires the standalone (non-GLVND) Mesa build (see Mesa section).
- `fatal_error()` (loader/platform.h) only prints — it does NOT exit. Failure paths that must abort need an explicit `return -1`/`exit()`; otherwise errors cascade (e.g. a failed `eglInitialize` → null context → SIGSEGV deep in Mesa).
