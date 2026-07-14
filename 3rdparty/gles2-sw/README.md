# gles2-sw — Software GLES2 Implementation for ARM32

This directory holds a pre-built software OpenGL ES 2.0 implementation
(`libGLES_sw.so`) targeting ARM32 (Cortex-A9, `arm-linux-gnueabihf`).

## Interim: the stub binary IS vendored (as of PR #11)

The `libGLES_sw.so` currently checked in here is a small (~85 KB) prebuilt
ARM32 software-GLES **stub** — enough to satisfy the MiSTer link
(`-lGLES_sw`) so CI (`build-mister-arm`) can build without a separate
provisioning step. The `.gitignore` still ignores `*.so` generally but
un-ignores this one file (`!libGLES_sw.so`).

**Follow-up (tracked separately):** build the software-GLES renderer in CI as a
standalone artifact (Mesa llvmpipe / SwiftShader per the options below) and drop
the vendored copy. A full renderer is ~20–80 MB and is not appropriate for
long-term version control; the vendored stub is a stopgap only.

## How to obtain libGLES_sw.so

### Option 1: Mesa llvmpipe (recommended)

Build Mesa with llvmpipe for arm-linux-gnueabihf and extract the GLES2
library:

```sh
# Cross-compile Mesa targeting arm-linux-gnueabihf with llvmpipe
meson setup build \
    --cross-file=cross-arm-linux-gnueabihf.ini \
    -Dgallium-drivers=llvmpipe \
    -Dvulkan-drivers=[] \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=enabled \
    -Dprefix=/usr/arm-linux-gnueabihf
ninja -C build

# Copy the resulting library
cp build/src/gallium/targets/gles/libGLESv2.so \
    3rdparty/gles2-sw/libGLES_sw.so
```

A pre-built Debian cross package (`libgl1-mesa-dri:armhf`) may also contain a
suitable `.so` if your host is Debian/Ubuntu with multiarch enabled.

### Option 2: Google SwiftShader for ARM32

SwiftShader provides a Vulkan/GLES software rasteriser. Build from source
targeting arm-linux-gnueabihf:

```sh
git clone https://swiftshader.googlesource.com/SwiftShader
cd SwiftShader
mkdir build && cd build
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-armhf.cmake \
    -DSWIFTSHADER_BUILD_GLES=ON
make -j$(nproc)

cp libGLESv2.so \
    ../../3rdparty/gles2-sw/libGLES_sw.so
```

## Placement

Once obtained, place the file as:

```
3rdparty/gles2-sw/libGLES_sw.so
```

Then build with:

```sh
make MISTER_BUILD=1 ARCH=arm-linux-gnueabihf
```

## Runtime deployment

The binary is linked with `-Wl,-rpath,'$ORIGIN'`, which means at runtime
`libGLES_sw.so` is looked up relative to the `gmloadernext` binary itself.
When deploying to the MiSTer SD card, place `libGLES_sw.so` in the same
directory as the `gmloadernext` binary (typically `games/gmloader/`).
