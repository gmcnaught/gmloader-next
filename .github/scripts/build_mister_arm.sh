#!/bin/bash
# build_mister_arm.sh — Build gmloader-next ARM32 binary for MiSTer FPGA
#
# Runs inside arm32v7/debian:bullseye-slim Docker container (QEMU-emulated).
# Called by GitHub Actions CI workflow (.github/workflows/build.yml).
#
# Expects the repository checkout to be mounted at /src (working dir).
set -e

ARCH=arm-linux-gnueabihf

echo "=== Installing build dependencies ==="
apt-get update -qq
apt-get install -y -qq \
    build-essential \
    git \
    python3 \
    python3-clang \
    pkg-config \
    make \
    binutils-${ARCH} \
    gcc-${ARCH} \
    g++-${ARCH} \
    libstdc++-10-dev-armhf-cross \
    linux-libc-dev-armhf-cross \
    libsdl2-dev:armhf \
    libzip-dev:armhf \
    zlib1g-dev:armhf \
    ca-certificates \
    file
# Note: libbsd is compiled from submodule sources (3rdparty/libbsd),
# and libzip is compiled from submodule sources (3rdparty/libzip),
# so their -dev packages above are optional fallbacks.
apt-get clean

# Verify cross-toolchain is present
which ${ARCH}-gcc || { echo "ERROR: cross-compiler ${ARCH}-gcc not found"; exit 1; }

echo "=== Cross-toolchain versions ==="
${ARCH}-gcc --version
${ARCH}-g++ --version

echo "=== Building gmloader-next for MiSTer (armhf) ==="
cd /src

# Enable multi-arch for armhf packages
dpkg --add-architecture armhf
apt-get update -qq

make -f Makefile.gmloader \
    ARCH=${ARCH} \
    MISTER_BUILD=1 \
    LLVM_FILE=/usr/lib/llvm-11/lib/libclang-11.so \
    LLVM_INC=/usr/${ARCH}/include/c++/10/${ARCH} \
    -j$(nproc)

BINARY=build/${ARCH}/gmloader/gmloadernext.armhf

echo "=== Binary info ==="
ls -lh "${BINARY}"
# Diagnostic only — never fail the build if `file` is unavailable.
file "${BINARY}" || echo "(file utility unavailable; skipping type info)"

echo "=== Verifying MISTER_BUILD linkage ==="
# NativeVideoWriter_WriteFrame is a non-static symbol from
# gmloader/mister/native_video_writer.c, compiled ONLY when MISTER_BUILD=1
# (Makefile.gmloader:149-155). If MISTER_BUILD is ever lost, the binary still
# links and still runs — it just has no fabric video path at all, which is a
# silent, plausible-looking failure. Assert it positively, before the strip
# below removes the symbol table.
if ! ${ARCH}-nm "${BINARY}" | grep -q ' T NativeVideoWriter_WriteFrame'; then
    echo "ERROR: NativeVideoWriter_WriteFrame not found in ${BINARY}." >&2
    echo "       MISTER_BUILD=1 did not take effect — this binary has no" >&2
    echo "       MiSTer video path and must not be shipped." >&2
    exit 1
fi
echo "OK: MiSTer sources linked (NativeVideoWriter_WriteFrame present)"

echo "=== Stripping binary ==="
${ARCH}-strip "${BINARY}"
ls -lh "${BINARY}"

echo "=== Copying binary to deploy location ==="
mkdir -p games/gmloader
cp "${BINARY}" games/gmloader/gmloader
chmod +x games/gmloader/gmloader

echo "=== Deploy artifact ==="
ls -lh games/gmloader/gmloader
echo "=== Build complete ==="
