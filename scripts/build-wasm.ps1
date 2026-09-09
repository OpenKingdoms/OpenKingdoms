# build-wasm.ps1 — configure + build the TAK-RE browser target.
#
# Prereqs: Emscripten SDK installed (C:\Projects\emsdk by default) and
# the extracted game data present at data/extracted (355 MB — bundled
# into tak-re.data at link time).
#
# Usage:
#   powershell -File scripts\build-wasm.ps1 [-EmsdkDir C:\path\to\emsdk] [-Config Release]
#
# Output: build-wasm/src/tak-re.html (+ .js/.wasm/.data). Serve with:
#   python -m http.server -d build-wasm/src 8080
# then open http://localhost:8080/tak-re.html

param(
    [string]$EmsdkDir = "C:\Projects\emsdk",
    [string]$Config = "Release",
    # Engine-only build with no game data bundled (what CI and GitHub Pages
    # build). The page then asks the player for their own game folder.
    [switch]$Public
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# Bring emsdk into this session (defines EMSDK, adds emcc to PATH).
& "$EmsdkDir\emsdk_env.ps1" | Out-Null

$Toolchain = "$EmsdkDir\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"
if (-not (Test-Path $Toolchain)) {
    throw "Emscripten toolchain not found at $Toolchain"
}

$Bundle = if ($Public) { "OFF" } else { "ON" }
$BuildDir = Join-Path $RepoRoot $(if ($Public) { "build-wasm-public" } else { "build-wasm" })
cmake -S $RepoRoot -B $BuildDir -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain" `
    -DCMAKE_BUILD_TYPE=$Config `
    -DTAK_WASM_BUNDLE_DATA=$Bundle `
    -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build $BuildDir --target tak-re
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Write-Host ""
Write-Host "Browser build ready: $BuildDir\src\tak-re.html"
Write-Host "Serve it with:  python -m http.server -d $BuildDir\src 8080"
