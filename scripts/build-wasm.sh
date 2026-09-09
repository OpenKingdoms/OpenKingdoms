#!/usr/bin/env bash
# Configure and build the browser (WebAssembly) target.
#
# Usage:
#   scripts/build-wasm.sh [--public] [--emsdk DIR] [--config Release|Debug] [--build-dir DIR]
#
#   --public     Engine-only build: no game data bundled into the page. This is
#                what CI and GitHub Pages build. The page then asks the player
#                for their own game folder (see docs/ASSETS.md).
#   (default)    Development build: if TAK_GAME_DIR points at an install and
#                the extracted data tree exists, both are packed into the page
#                so it boots straight into the game.
#
# Needs the Emscripten SDK (https://emscripten.org) and Ninja. Output:
#   <build-dir>/src/tak-re.html (+ .js/.wasm)
# Serve it with:  python -m http.server -d <build-dir>/src 8080

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMSDK_DIR="${EMSDK:-}"
CONFIG=Release
BUILD_DIR=""
BUNDLE=ON

while [[ $# -gt 0 ]]; do
    case "$1" in
        --public)     BUNDLE=OFF; shift ;;
        --emsdk)      EMSDK_DIR="$2"; shift 2 ;;
        --config)     CONFIG="$2"; shift 2 ;;
        --build-dir)  BUILD_DIR="$2"; shift 2 ;;
        -h|--help)    sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$BUILD_DIR" ]]; then
    if [[ "$BUNDLE" == OFF ]]; then BUILD_DIR="$ROOT/build-wasm-public"; else BUILD_DIR="$ROOT/build-wasm"; fi
fi

# Bring emcc onto PATH if it isn't already.
if ! command -v emcc >/dev/null 2>&1; then
    for cand in "$EMSDK_DIR" "$ROOT/../emsdk" "$HOME/emsdk" "/c/Projects/emsdk"; do
        if [[ -n "$cand" && -f "$cand/emsdk_env.sh" ]]; then
            # emsdk_env.sh reads unset variables, so relax -u/-e around it.
            set +eu
            # shellcheck disable=SC1091
            source "$cand/emsdk_env.sh" >/dev/null 2>&1
            set -eu
            # emsdk_env.sh can decline to touch PATH in a non-interactive
            # shell; the two directories it would add are all we need.
            if ! command -v emcc >/dev/null 2>&1; then
                export EMSDK="$cand"
                export PATH="$cand:$cand/upstream/emscripten:$PATH"
            fi
            break
        fi
    done
fi
command -v emcc >/dev/null 2>&1 || { echo "emcc not found: install emsdk and pass --emsdk DIR or activate it" >&2; exit 1; }

TOOLCHAIN="$(dirname "$(command -v emcc)")/cmake/Modules/Platform/Emscripten.cmake"
[[ -f "$TOOLCHAIN" ]] || { echo "Emscripten toolchain file not found at $TOOLCHAIN" >&2; exit 1; }

# Ninja: on PATH, or the copy Visual Studio ships.
NINJA_ARG=()
if ! command -v ninja >/dev/null 2>&1; then
    vs_ninja=$(ls "/c/Program Files/Microsoft Visual Studio/"*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe 2>/dev/null | head -1 || true)
    [[ -n "$vs_ninja" ]] && NINJA_ARG=(-DCMAKE_MAKE_PROGRAM="$vs_ninja")
fi

echo "==> Configuring ($CONFIG, bundle data: $BUNDLE) in $BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja "${NINJA_ARG[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DTAK_WASM_BUNDLE_DATA="$BUNDLE" \
    -DBUILD_TESTING=OFF

echo "==> Building"
cmake --build "$BUILD_DIR" --target tak-re

echo
echo "Browser build ready: $BUILD_DIR/src/tak-re.html"
echo "Serve it with:  python -m http.server -d $BUILD_DIR/src 8080"
