cmake_minimum_required(VERSION 3.20)
project(TAK-RE
    VERSION 0.1.0
    DESCRIPTION "Total Annihilation: Kingdoms - Reverse Engineered"
    LANGUAGES C
)

include(CTest)

# ── SDL2 (cross-platform window, input, timers) ────────────────────────────
# Native: acquired via vcpkg. Run: cmake --preset win-x86 (Windows) or
# cmake --preset mac (macOS).
# Emscripten: SDL2 ships as a browser port — synthesize the imported
# targets so the rest of the tree links SDL2::SDL2 unchanged.
if(EMSCRIPTEN)
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        INTERFACE_COMPILE_OPTIONS "-sUSE_SDL=2"
        INTERFACE_LINK_OPTIONS    "-sUSE_SDL=2")
    add_library(SDL2::SDL2main INTERFACE IMPORTED)
    # Browser-wide link flags: bigger heap that can grow (355 MB asset
    # tree + game state), async main loop via emscripten_set_main_loop.
    # GROWABLE_ARRAYBUFFERS=0: with growth enabled the default resizable
    # ArrayBuffer heap makes Chrome reject heap views passed to
    # texSubImage2D ("value must not be resizable") — the legacy
    # buffer-replacement growth path keeps WebGL uploads working.
    # INITIAL_MEMORY=1GB: with GROWABLE_ARRAYBUFFERS=0 every heap grow
    # REPLACES the buffer (old+new alive during the copy). Growing
    # 256→512 MB mid-skirmish inside a tab already holding the 644 MB
    # asset package spikes past the tab limit → OOM crash. Starting at
    # 1 GB avoids runtime growth entirely; growth stays enabled as a
    # backstop.
    add_link_options(
        "-sALLOW_MEMORY_GROWTH=1"
        "-sGROWABLE_ARRAYBUFFERS=0"
        "-sINITIAL_MEMORY=1073741824"
        "-sSTACK_SIZE=4194304"
    )
else()
    find_package(SDL2 CONFIG REQUIRED)
endif()

# ── Build options ────────────────────────────────────────────────────────────
option(TAK_SERVER "Build headless dedicated server (no rendering/audio)" OFF)

# ── Global compiler settings ─────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    # Match the original MSVC 6.0 ABI as closely as possible:
    # - Pack structs to 1-byte alignment where we use #pragma pack
    # - Full warnings, treat as errors in CI but not during dev
    add_compile_options(/W3 /wd4996)  # Disable deprecation warnings for POSIX names
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS       # Allow sprintf, fopen, etc.
        WIN32_LEAN_AND_MEAN            # Slim windows.h
        NOMINMAX                       # Prevent min/max macros
    )
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ── Original game directory ──────────────────────────────────────────────────
# The player's own TA:K install (HPI archives). Never bundled, never in the
# repo — see docs/ASSETS.md. Empty by default; pass -DTAK_GAME_DIR=<path>
# or set the TAK_GAME_DIR environment variable.
set(TAK_GAME_DIR "$ENV{TAK_GAME_DIR}" CACHE PATH
    "Path to the original TA:K installation (HPI archives)")
if(NOT TAK_GAME_DIR)
    # Dev convenience: pick up a default GOG install if one is present.
    foreach(_cand
            "C:/GOG Games/Total Annihilation Kingdoms"
            "C:/Program Files (x86)/GOG Galaxy/Games/Total Annihilation Kingdoms"
            "$ENV{HOME}/Games/Total Annihilation Kingdoms")
        if(EXISTS "${_cand}")
            set(TAK_GAME_DIR "${_cand}" CACHE PATH
                "Path to the original TA:K installation (HPI archives)" FORCE)
            break()
        endif()
    endforeach()
endif()
if(TAK_GAME_DIR)
    message(STATUS "Original game directory: ${TAK_GAME_DIR}")
else()
    message(STATUS "Original game directory: not set (data-dependent tests will fail)")
endif()

# ── Data directory (extracted HPI archives for development) ──────────────────
set(TAK_DATA_DIR "${CMAKE_SOURCE_DIR}/data/extracted" CACHE PATH
    "Path to extracted game data directory")
message(STATUS "Game data directory: ${TAK_DATA_DIR}")

# ── Third-party libraries from the original game ────────────────────────────
#
# The original executable dynamically loads these DLLs at runtime via
# LoadLibrary + GetProcAddress. They fall into categories:
#
# ┌─────────────────────────────────────────────────────────────────────────┐
# │ CATEGORY A: Cavedog game DLLs (WILL be replaced by our rewrite)       │
# │                                                                        │
# │   Rover.dll      (200 KB)  — Network/patching subsystem               │
# │   bymaia.dll     (741 KB)  — Boneyards auto-update system             │
# │   dplayerx.dll   (156 KB)  — DirectPlay multiplayer wrapper           │
# │   patchw32.dll   (142 KB)  — Patching system                          │
# │   drvmgt.dll     (32 KB)   — Driver management                        │
# │   upgrade.dll    (300 KB)  — Upgrade utility                          │
# │                                                                        │
# │   These contain Cavedog-written code that we will rewrite in C.        │
# │   We do NOT link against them — they exist only as RE reference.       │
# ├─────────────────────────────────────────────────────────────────────────┤
# │ CATEGORY B: Third-party middleware (will be REPLACED with modern libs) │
# │                                                                        │
# │   mss32.dll      (322 KB)  — Miles Sound System (RAD Game Tools)      │
# │   mss16.dll      (272 KB)  — Miles Sound System 16-bit                │
# │   MP3DEC.ASI     (116 KB)  — MP3 decoder plugin for Miles             │
# │   binkw32.dll    (168 KB)  — Bink Video codec (RAD Game Tools)        │
# │                                                                        │
# │   Replacements:                                                        │
# │     mss32 -> miniaudio.h (single-header, zero-dep, plays WAV natively)│
# │     binkw32 -> skip cutscenes initially, later use FFmpeg             │
# │                                                                        │
# │   We do NOT link against these. The _AIL_* call signatures the         │
# │   original uses are documented for writing the shim layer.             │
# ├─────────────────────────────────────────────────────────────────────────┤
# │ CATEGORY C: Graphics wrappers (NOT needed — we use modern rendering)   │
# │                                                                        │
# │   glide.dll      (1.3 MB)  — nGlide (modern 3dfx emulation)          │
# │   glide2x.dll    (1.3 MB)  — nGlide Glide 2.x                        │
# │   glide3x.dll    (1.3 MB)  — nGlide Glide 3.x                        │
# │   3DfxSpl*.dll   (1.1 MB)  — 3DFX splitter variants                  │
# │                                                                        │
# │   The original loads Glide3x.dll via LoadLibrary. We won't use Glide   │
# │   at all — our renderer will use OpenGL or SDL2.                       │
# ├─────────────────────────────────────────────────────────────────────────┤
# │ CATEGORY D: GOG/platform (IGNORED — not part of the game)             │
# │                                                                        │
# │   goggame.dll    (282 KB)  — GOG Galaxy integration                   │
# │   GameuxInstallHelper.dll  — Windows Games Explorer helper             │
# │                                                                        │
# │   Not game code. Completely irrelevant to the rewrite.                 │
# └─────────────────────────────────────────────────────────────────────────┘
#
# SYSTEM DLLs the original loads dynamically (already on every Windows box):
#   ddraw.dll, dsound.dll, dinput.dll, winmm.dll, wsock32.dll,
#   kernel32.dll, user32.dll, gdi32.dll, dsetup.dll,
#   IMAGEHLP.DLL, psapi.dll, MAPI32.DLL, snmpapi.dll, inetmib1.dll

# ── zlib (via vendored miniz) ────────────────────────────────────────────────
# HPI archives use zlib (deflate) compression. We vendor miniz (third_party/)
# as a drop-in zlib replacement — no external dependency needed.

# ── Source tree ──────────────────────────────────────────────────────────────
add_subdirectory(src)
