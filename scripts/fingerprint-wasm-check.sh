#!/usr/bin/env bash
# Run the map fingerprint tests through the browser toolchain.
#
# A fingerprint has to come out the same everywhere or two players on
# different builds would be told their maps differ. This compiles the same
# test file with Emscripten and runs it under node, so the golden values in
# src/game/test_map_fingerprint.c have to hold there too.
#
# Usage: scripts/fingerprint-wasm-check.sh [--emsdk DIR] [--out DIR]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMSDK_DIR="${EMSDK:-}"
OUT="$ROOT/build-wasm-fingerprint"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --emsdk) EMSDK_DIR="$2"; shift 2 ;;
        --out)   OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# Bring emcc onto PATH if it isn't already, the same way build-wasm.sh does.
if ! command -v emcc >/dev/null 2>&1; then
    for cand in "$EMSDK_DIR" "$ROOT/../emsdk" "$HOME/emsdk" "/c/Projects/emsdk"; do
        if [[ -n "$cand" && -f "$cand/emsdk_env.sh" ]]; then
            set +eu
            # shellcheck disable=SC1091
            source "$cand/emsdk_env.sh" >/dev/null 2>&1
            set -eu
            if ! command -v emcc >/dev/null 2>&1; then
                export EMSDK="$cand"
                export PATH="$cand:$cand/upstream/emscripten:$PATH"
            fi
            break
        fi
    done
fi
command -v emcc >/dev/null 2>&1 || { echo "emcc not found: install emsdk and pass --emsdk DIR" >&2; exit 1; }
command -v node >/dev/null 2>&1 || { echo "node not found" >&2; exit 1; }

mkdir -p "$OUT"

echo "==> Building the fingerprint tests for node"
emcc -O1 -std=gnu11 \
    -I "$ROOT/include" -I "$ROOT/third_party" \
    "$ROOT/src/game/test_map_fingerprint.c" \
    "$ROOT/src/game/map_fingerprint.c" \
    "$ROOT/src/game/maps.c" \
    "$ROOT/src/core/sha256.c" \
    "$ROOT/src/core/hpi/hpi.c" \
    "$ROOT/src/core/hpi/hpi_vfs.c" \
    "$ROOT/src/core/hpi/hpi_decompress.c" \
    "$ROOT/src/core/memory.c" \
    "$ROOT/src/core/io.c" \
    "$ROOT/src/core/util.c" \
    "$ROOT/third_party/miniz.c" \
    -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 \
    -o "$OUT/test_map_fingerprint.js"

echo "==> Running them under node"
node "$OUT/test_map_fingerprint.js"
