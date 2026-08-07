# GL runtime closure (vendored)

The surfaceless-Mesa closure the gmloader engine needs at
`games/gmloader/mesa/` on-device (loaded via
`LD_LIBRARY_PATH=<gamedir>/mesa:<gamedir>`). Mesa is MIT-licensed.

This is the **llvmpipe** build (`-Dgallium-drivers=swrast -Dllvm=enabled
-Dshared-llvm=false`), which is what `192.168.20.81` actually runs. See
CLAUDE.md "Mesa llvmpipe build" for how it is produced.

## The six files, and why each is here

| file | why |
|---|---|
| `libEGL.so.1` | surfaceless platform; there is no `/dev/dri` on MiSTer |
| `libGLESv2.so.2` | the GLES2 the engine talks to. **Also shipped as `libGLES_sw.so`** — see below |
| `libglapi.so.0` | the only `NEEDED` of `libGLESv2.so.2` |
| `libdrm.so.2` | `NEEDED` of `libEGL.so.1` and `swrast_dri.so` |
| `swrast_dri.so` | the rasteriser (34 MB: LLVM is statically linked) |
| `libtinfo.so.6` | direct `NEEDED` of the static-LLVM `swrast_dri.so`, and **MiSTer does not have it**. Without it Mesa loads far enough to create an EGL context and then `MESA-LOADER: failed to open swrast`, `eglInitialize failed`, dead engine |

Everything else these libraries need (`libexpat`, `libstdc++`, `libgcc_s`,
`libz`, `libm`, `libpthread`, `libdl`, `libc`) is already on the device.

## `libGLES_sw.so` is a copy of `libGLESv2.so.2`

The engine `dlopen`s `./libGLES_sw.so` as its bundled GLES library, and that
file must be **this Mesa build's `libGLESv2.so.2`** — whose only `NEEDED` is
`libglapi.so.0`.

It must NOT be `3rdparty/gles2-sw/libGLES_sw.so`. That build pulls
`libGLdispatch.so.0` (and behind it gbm/X11/wayland, none of which exist here),
and even when `libGLdispatch.so.0` is supplied it yields
`OpenGL: version string (null)` and a SIGSEGV in the runner's `GR_D3D_Init`.
Device-verified on `.62`, 2026-08-07. CLAUDE.md has said "NOT the fat
`libGLES_sw.so`" since the runtime was first assembled by hand; the release
bundle shipped it anyway from v0.1.0 through v0.2.0, which is why neither of
those releases could start.

`maldita.castilla-mister`'s `scripts/release/assemble_bundle.sh` therefore
copies `runtime/mesa/libGLESv2.so.2` to BOTH `games/gmloader/libGLES_sw.so`
and `games/gmloader/mesa/libGLESv2.so.2`, so the two can never drift.

## Not vendored here

- `libstdc++.so` -> `lib/armeabi-v7a/libstdc++.so` (already tracked)
- `libGLdispatch.so.0`, `libGLX_mesa.so.0` — present in `.81`'s deploy tree but
  vestigial. Nothing in the closure above references either; `libGLX_mesa.so.0`
  is the X11/GLX loader and is unreachable under `EGL_PLATFORM=surfaceless`.

## History

Vendored 2026-07-30 from a known-good deploy tree, but from the **softpipe**
build and missing `libtinfo.so.6` — a set that had never run anywhere.
Replaced 2026-08-07 with the llvmpipe closure from `.81`, verified by a clean
install on `.62`: engine up, `C_SUBMIT ≈ C_DONE`, 0 fabric timeouts,
`OpenGL: version string OpenGL ES 3.2 Mesa 21.3.9`, game rendering.
