#!/usr/bin/env bash
# Build FFmpeg for the browser: the Bink decoder and demuxer and nothing
# else, as static WebAssembly archives the game links against. The same
# component selection as the desktop release build in
# .github/workflows/release.yml.
#
# Usage:
#   scripts/build-ffmpeg-wasm.sh [--prefix DIR] [--version 7.1.1] [--source DIR]
#
#   --prefix DIR   Where the headers and archives are installed
#                  (default: <repo>/build-ffmpeg-wasm). Pass the same
#                  directory to CMake as CMAKE_FIND_ROOT_PATH, which
#                  scripts/build-wasm.sh does.
#   --version V    The FFmpeg release to fetch from ffmpeg.org.
#   --source DIR   An unpacked source tree to use instead of fetching.
#
# Needs emcc on PATH (scripts/build-wasm.sh puts it there), GNU make,
# curl and tar. JOBS caps the parallel compile (default: every core).
# Does nothing when the prefix already holds this build:
# <prefix>/ffmpeg-wasm.stamp names the FFmpeg and Emscripten versions
# and this script's own hash, so a changed configure line builds again.
# CI caches the prefix on the same three.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="$ROOT/build-ffmpeg-wasm"
VERSION=7.1.1
SRC=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)   PREFIX="$2"; shift 2 ;;
        --version)  VERSION="$2"; shift 2 ;;
        --source)   SRC="$2"; shift 2 ;;
        -h|--help)  sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

for tool in emcc make curl tar; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool not found on PATH" >&2; exit 1; }
done

mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
STAMP="$PREFIX/ffmpeg-wasm.stamp"
if command -v sha256sum >/dev/null 2>&1; then SELF=$(sha256sum "${BASH_SOURCE[0]}" | cut -c1-16)
else SELF=$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -c1-16); fi
WANT="ffmpeg $VERSION, emscripten $(emcc -dumpversion), script $SELF"
if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$WANT" && -f "$PREFIX/lib/libavcodec.a" ]]; then
    echo "==> FFmpeg for the browser is already at $PREFIX ($WANT)"
    exit 0
fi

if [[ -z "$SRC" ]]; then
    SRC="$PREFIX-src/ffmpeg-$VERSION"
    if [[ ! -f "$SRC/configure" ]]; then
        mkdir -p "$PREFIX-src"
        echo "==> Fetching ffmpeg-$VERSION.tar.gz"
        curl -fsSL -o "$PREFIX-src/ffmpeg-$VERSION.tar.gz" \
            "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.gz"
        tar xzf "$PREFIX-src/ffmpeg-$VERSION.tar.gz" -C "$PREFIX-src"
    fi
fi

# configure wants a host compiler even though nothing here is built with
# it, and a Windows box with only emsdk has none but emcc.
HOSTCC=()
command -v gcc >/dev/null 2>&1 || command -v cc >/dev/null 2>&1 || HOSTCC=(--host-cc=emcc)

echo "==> Configuring FFmpeg $VERSION for wasm in $SRC"
cd "$SRC"
./configure --prefix="$PREFIX" \
    --target-os=none --arch=x86_32 --enable-cross-compile \
    --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm "${HOSTCC[@]}" \
    --disable-asm --disable-inline-asm --disable-x86asm --disable-runtime-cpudetect \
    --disable-pthreads --disable-w32threads --disable-os2threads --disable-stripping \
    --disable-everything --disable-programs --disable-doc \
    --disable-network --disable-autodetect --disable-debug \
    --disable-shared --enable-static --enable-small --disable-swscale-alpha \
    --disable-avdevice --disable-avfilter --disable-postproc \
    --enable-decoder=bink --enable-decoder=binkaudio_rdft --enable-decoder=binkaudio_dct \
    --enable-demuxer=bink --enable-protocol=file

echo "==> Building"
make -j"${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
make install
printf '%s\n' "$WANT" > "$STAMP"
echo "==> FFmpeg for the browser installed at $PREFIX ($WANT)"
