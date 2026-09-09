# ── TAK-RE executable ────────────────────────────────────────────────────────

set(TAK_CORE_SOURCES
    core/memory.c
    core/io.c
    core/debug.c
    core/util.c
    core/game_loop.c
    core/battle_config.c
    core/parser/tdf_parser.c
    sound/sound.c
    sound/music.c
    sound/soundclass.c
    sound/game_sound.c
)

set(TAK_HPI_SOURCES
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
)

set(TAK_RENDER_SOURCES
    render/palette.c
    render/pal_lookup.c
    render/gaf.c
    render/blit.c
    render/bink_player.c
    render/font.c
    render/cursor.c
    render/ui.c
    render/main_menu.c
    render/terrain.c
    render/tak_gpu.c
    render/jpg_decode.c
    render/minimap.c
    render/units.c
    render/obj3d.c
    render/tex_atlas.c
    render/debug_panel.c
    render/cob.c
    render/cob_vm.c
)

set(TAK_GAME_SOURCES
    game/ai.c
    game/tnt.c
    game/world.c
    game/economy.c
    game/moveinfo.c
    game/pathing.c
    game/fog.c
    game/features.c
    game/mission.c
)

set(TAK_UI_SOURCES
    ui/gui_loader.c
    ui/gui_render.c
    ui/simple_screen.c
    ui/battle_setup.c
    ui/options.c
    ui/loading.c
    ui/credits.c
    ui/story.c
    ui/ingame.c
    ui/camera.c
    ui/hud.c
    ui/hud_text.c
    ui/story.c
    ui/multiplayer.c
    ui/ingame.c
    ui/camera.c
    ui/hud.c
    ui/hud_text.c
)

set(TAK_NET_SOURCES
    net/commands.c
)

# miniz — vendored zlib-compatible library (single .c/.h in third_party/)
set(TAK_THIRD_PARTY_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/miniz.c
)

set(TAK_PLATFORM_SOURCES
    platform/sdl2_platform.c
)

# The main executable. SDL2main provides the WinMain->main bridge on Windows,
# so we use a standard main() entry point for cross-platform compatibility.
# Every executable below (game, tests, tools) uses libm; miniaudio needs
# dl + pthread. Directory-wide so no target can miss it.
if(UNIX AND NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    link_libraries(m ${CMAKE_DL_LIBS} Threads::Threads)
endif()

add_executable(tak-re
    main.c
    ${TAK_CORE_SOURCES}
    ${TAK_HPI_SOURCES}
    ${TAK_GAME_SOURCES}
    ${TAK_NET_SOURCES}
    ${TAK_RENDER_SOURCES}
    ${TAK_UI_SOURCES}
    ${TAK_PLATFORM_SOURCES}
    ${TAK_THIRD_PARTY_SOURCES}
)

target_include_directories(tak-re PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

# ── SDL2 + FFmpeg + system libraries ────────────────────────────────────────
find_package(PkgConfig QUIET)
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)

target_link_libraries(tak-re PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

if(AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND SWSCALE_LIBRARY)
    target_include_directories(tak-re PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(tak-re PRIVATE
        ${AVCODEC_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY}
    )
    # Directory-wide: every target compiling bink_player.c (game +
    # UI test binaries) gets the real FFmpeg implementation.
    add_compile_definitions(TAK_HAVE_FFMPEG=1)
    message(STATUS "FFmpeg found -- Bink video playback enabled")
else()
    message(WARNING "FFmpeg not found -- Bink video playback disabled, using static sprites")
endif()

if(WIN32)
    target_link_libraries(tak-re PRIVATE
        user32          # Window management, messages, cursors
        gdi32           # GDI drawing, device contexts
        winmm           # Multimedia timers (timeGetTime, etc.)
        ws2_32          # Winsock 2 (networking — replaces original wsock32)
        advapi32        # Registry access (original stores config in HKCU\Software\Cavedog Entertainment\Kingdoms)
        shlwapi         # PathMatchSpecA (glob matching in util.c)
    )
endif()

# ── Compile definitions ─────────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Browser build reads assets from the preloaded MEMFS tree. The
    # extracted data dir is bundled at link time; the (empty) game dir
    # exists only so VFS_Init's directory scan succeeds with zero HPIs.
    target_compile_definitions(tak-re PRIVATE
        TAK_DATA_DIR="/data/extracted"
        TAK_GAME_DIR="/game"
        $<$<CONFIG:Debug>:TAK_DEBUG=1>
    )
    # Bundle ONLY the original .hpi archives (289 MB) — the loose
    # extracted tree (356 MB) is redundant with them and Emscripten's
    # file packager keeps the whole preloaded package buffer resident
    # in the tab for the session, so every bundled megabyte is a
    # megabyte of browser memory. All asset enumeration goes through
    # VFS_ListFiles, which reads HPI contents directly. Staged into the
    # build dir first — the GOG install path contains spaces, which
    # --preload-file's src@dst syntax cannot escape.
    # Data bundling is a DEV convenience only: it packs the player's own
    # archives into the page. Off automatically when no game dir is set
    # (CI, and any public build) — the engine then boots data-less and
    # expects the bring-your-own-data browser flow.
    file(GLOB TAK_HPI_FILES "${TAK_GAME_DIR}/*.hpi")
    if(TAK_HPI_FILES AND EXISTS "${TAK_DATA_DIR}")
        option(TAK_WASM_BUNDLE_DATA "Bundle local game data into the web build (dev only)" ON)
    else()
        option(TAK_WASM_BUNDLE_DATA "Bundle local game data into the web build (dev only)" OFF)
    endif()
    if(TAK_WASM_BUNDLE_DATA)
        if(NOT TAK_HPI_FILES)
            message(FATAL_ERROR "WASM: TAK_WASM_BUNDLE_DATA=ON but no .hpi archives at '${TAK_GAME_DIR}'")
        endif()
        set(TAK_WASM_HPI_DIR "${CMAKE_BINARY_DIR}/wasm-game")
        file(COPY ${TAK_HPI_FILES} DESTINATION "${TAK_WASM_HPI_DIR}")
        file(GLOB TAK_MUSIC_FILES "${TAK_GAME_DIR}/Music/*.wav")
        if(TAK_MUSIC_FILES)
            file(COPY ${TAK_MUSIC_FILES} DESTINATION "${TAK_WASM_HPI_DIR}/Music")
        else()
            message(WARNING "WASM: no Music/*.wav found — browser music disabled")
        endif()
        # Loose tree still bundled: battle_setup's map list and the
        # features registry read it directly (TODO: route through
        # VFS_ListFiles like canbuild, then drop this 356 MB from the
        # bundle).
        target_link_options(tak-re PRIVATE
            "SHELL:--preload-file ${TAK_WASM_HPI_DIR}@/game"
            "SHELL:--preload-file ${TAK_DATA_DIR}@/data/extracted")
        message(STATUS "WASM: bundling HPI archives from ${TAK_GAME_DIR}")
    else()
        message(STATUS "WASM: engine-only build, no game data bundled")
    endif()
    target_link_options(tak-re PRIVATE
        "SHELL:--pre-js ${CMAKE_SOURCE_DIR}/scripts/wasm_pre.js"
        "-sEXIT_RUNTIME=0"
        "-sASYNCIFY=0"
        "--profiling-funcs"
    )
    set_target_properties(tak-re PROPERTIES SUFFIX ".html")
else()
    target_compile_definitions(tak-re PRIVATE
        TAK_DATA_DIR="${TAK_DATA_DIR}"
        TAK_GAME_DIR="${TAK_GAME_DIR}"
        $<$<CONFIG:Debug>:TAK_DEBUG=1>
    )
endif()

# ── Post-build: copy SDL2.dll next to the executable (Windows) ──────────────
if(WIN32)
    add_custom_command(TARGET tak-re POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:tak-re>
        COMMENT "Copying SDL2.dll to output directory"
    )
endif()

# ── Tests + tools (native only) ─────────────────────────────────────────────
# The browser build produces just the game; ctest targets and inspectors
# stay desktop-side.
if(EMSCRIPTEN)
    return()
endif()

# ── TDF parser tests ─────────────────────────────────────────────────────────
# Uses the real tak_memory allocator. The historical tak_memory_stub.c is
# kept around but no longer compiled into any target.
add_executable(test_tdf_parser
    core/parser/test_tdf_parser.c
    core/parser/tdf_parser.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz.c
)

target_include_directories(test_tdf_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

if(WIN32)
    target_link_libraries(test_tdf_parser PRIVATE shlwapi)
endif()

# ── GUI loader tests ────────────────────────────────────────────────────────
add_executable(test_gui_loader
    ui/test_gui_loader.c
    ui/gui_loader.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/util.c
    core/io.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_gui_loader PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(test_gui_loader PRIVATE SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    target_link_libraries(test_gui_loader PRIVATE shlwapi)
    add_custom_command(TARGET test_gui_loader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_gui_loader>)
endif()
# ── UI screens smoke tests ─────────────────────────────────────────────────
add_executable(test_ui_screens
    ui/test_ui_screens.c
    ui/gui_loader.c
    ui/gui_render.c
    ui/battle_setup.c
    ui/options.c
    ui/loading.c
    ui/credits.c
    ui/story.c
    ui/ingame.c
    ui/camera.c
    ui/hud.c
    ui/hud_text.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/parser/tdf_parser.c
    core/memory.c
    core/util.c
    core/io.c
    core/game_loop.c
    core/battle_config.c
    sound/sound.c
    sound/soundclass.c
    sound/game_sound.c
    game/ai.c
    game/tnt.c
    game/world.c
    game/economy.c
    game/moveinfo.c
    game/pathing.c
    game/fog.c
    game/features.c
    game/mission.c
    render/palette.c
    render/pal_lookup.c
    render/gaf.c
    render/blit.c
    render/font.c
    render/ui.c
    render/bink_player.c
    render/terrain.c
    render/tak_gpu.c
    render/jpg_decode.c
    render/minimap.c
    render/units.c
    render/obj3d.c
    render/tex_atlas.c
    render/cob.c
    render/cob_vm.c
    render/debug_panel.c
    platform/sdl2_platform.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_ui_screens PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(test_ui_screens PRIVATE SDL2::SDL2 SDL2::SDL2main)
if(AVCODEC_LIBRARY)
    target_include_directories(test_ui_screens PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(test_ui_screens PRIVATE
        ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
endif()
if(WIN32)
    target_link_libraries(test_ui_screens PRIVATE shlwapi user32 gdi32 winmm ws2_32 advapi32)
    add_custom_command(TARGET test_ui_screens POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_ui_screens>)
endif()
target_compile_definitions(test_ui_screens PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)

target_compile_definitions(test_gui_loader PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)

# ── HPI decompression + public API tests ────────────────────────────────────
add_executable(test_hpi
    core/hpi/test_hpi.c
    core/hpi/hpi.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)

target_include_directories(test_hpi PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_definitions(test_hpi PRIVATE
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)

if(WIN32)
    target_link_libraries(test_hpi PRIVATE shlwapi)
endif()

# ── HPI archive open smoke test ─────────────────────────────────────────────
add_executable(test_hpi_open
    core/hpi/test_hpi_open.c
    core/hpi/hpi.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)

target_include_directories(test_hpi_open PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_definitions(test_hpi_open PRIVATE
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)

if(WIN32)
    target_link_libraries(test_hpi_open PRIVATE shlwapi)
endif()

# ── Cross-platform I/O tests ────────────────────────────────────────────────
add_executable(test_io
    core/test_io.c
    core/io.c
)

target_include_directories(test_io PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# ── HPI VFS tests ───────────────────────────────────────────────────────────
add_executable(test_hpi_vfs
    core/hpi/test_hpi_vfs.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)

target_include_directories(test_hpi_vfs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_definitions(test_hpi_vfs PRIVATE
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)

if(WIN32)
    target_link_libraries(test_hpi_vfs PRIVATE shlwapi)
endif()

# ── Game loop timer tests ───────────────────────────────────────────────────
add_executable(test_gameloop
    core/test_gameloop.c
    core/game_loop.c
)

target_include_directories(test_gameloop PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_gameloop PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

if(WIN32)
    add_custom_command(TARGET test_gameloop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_gameloop>
        COMMENT "Copying SDL2.dll for test_gameloop"
    )
endif()

# ── Memory allocator tests ───────────────────────────────────────────────────
add_executable(test_memory
    core/test_memory.c
    core/memory.c
    core/util.c
)

target_include_directories(test_memory PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(test_memory PRIVATE shlwapi)
endif()

# ── Palette tests ────────────────────────────────────────────────────────
add_executable(test_palette
    render/test_palette.c
    render/palette.c
    render/pal_lookup.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)

target_include_directories(test_palette PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_definitions(test_palette PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)

target_link_libraries(test_palette PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

if(WIN32)
    target_link_libraries(test_palette PRIVATE shlwapi)
    add_custom_command(TARGET test_palette POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_palette>
        COMMENT "Copying SDL2.dll for test_palette"
    )
endif()

# ── TNT loader tests ─────────────────────────────────────────────────────
add_executable(test_tnt
    game/test_tnt.c
    game/tnt.c
    render/palette.c
    render/pal_lookup.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)

target_include_directories(test_tnt PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_definitions(test_tnt PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)

target_link_libraries(test_tnt PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

if(WIN32)
    target_link_libraries(test_tnt PRIVATE shlwapi)
    add_custom_command(TARGET test_tnt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_tnt>
        COMMENT "Copying SDL2.dll for test_tnt"
    )
endif()

# ── 3DO parser tests (Phase C M2) ────────────────────────────────────────
add_executable(test_obj3d
    render/test_obj3d.c
    render/obj3d.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_obj3d PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_obj3d PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(test_obj3d PRIVATE shlwapi)
endif()

# ── JPG decode tests (M5) ────────────────────────────────────────────────
add_executable(test_jpg
    render/test_jpg.c
    render/jpg_decode.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_jpg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_jpg PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
target_link_libraries(test_jpg PRIVATE SDL2::SDL2 SDL2::SDL2main)
if(AVCODEC_LIBRARY)
    target_include_directories(test_jpg PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(test_jpg PRIVATE
        ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
endif()
if(WIN32)
    target_link_libraries(test_jpg PRIVATE shlwapi)
    add_custom_command(TARGET test_jpg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_jpg>
        COMMENT "Copying SDL2.dll for test_jpg"
    )
endif()

# ── Blit tests (Blit_RGBA_Opaque) ────────────────────────────────────────
add_executable(test_blit
    render/test_blit.c
    render/blit.c
)

target_include_directories(test_blit PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_blit PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

if(WIN32)
    add_custom_command(TARGET test_blit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_blit>
        COMMENT "Copying SDL2.dll for test_blit"
    )
endif()

# ── GAF loader tests ─────────────────────────────────────────────────────
add_executable(test_gaf
    render/test_gaf.c
    render/gaf.c
    render/palette.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)

target_include_directories(test_gaf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_definitions(test_gaf PRIVATE
    TAK_GAME_DIR="${TAK_GAME_DIR}"
    TAK_DATA_DIR="${TAK_DATA_DIR}"
)

target_link_libraries(test_gaf PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

if(WIN32)
    target_link_libraries(test_gaf PRIVATE shlwapi)
    add_custom_command(TARGET test_gaf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_gaf>
        COMMENT "Copying SDL2.dll for test_gaf"
    )
endif()

# ── Install rules ────────────────────────────────────────────────────────────
install(TARGETS tak-re RUNTIME DESTINATION bin)

# ── Phase C recon: cross-reference textures ───────────────────────────
add_executable(probe_textures
    ${CMAKE_SOURCE_DIR}/tools/probe_textures.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(probe_textures PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(probe_textures PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(probe_textures PRIVATE shlwapi)
endif()

# ── Phase C recon: dump a 3DO file ─────────────────────────────────────
add_executable(probe_3do
    ${CMAKE_SOURCE_DIR}/tools/probe_3do.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(probe_3do PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(probe_3do PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(probe_3do PRIVATE shlwapi)
endif()

# ── Phase C recon: list unit FBIs ──────────────────────────────────────
add_executable(probe_units
    ${CMAKE_SOURCE_DIR}/tools/probe_units.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(probe_units PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(probe_units PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(probe_units PRIVATE shlwapi)
endif()

# ── COB parser tests (Phase D M1) ─────────────────────────────────────
add_executable(test_cob
    render/test_cob.c
    render/cob.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_cob PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_cob PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(test_cob PRIVATE shlwapi)
endif()

# ── COB VM tests (Phase D M2+) ────────────────────────────────────────
add_executable(test_cob_vm
    render/test_cob_vm.c
    render/cob.c
    render/cob_vm.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_cob_vm PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_cob_vm PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(test_cob_vm PRIVATE shlwapi)
endif()

# ── Probe: list entries in an in-game HUD GAF ─────────────────────────
add_executable(probe_hud_gaf
    ${CMAKE_SOURCE_DIR}/tools/probe_hud_gaf.c
    render/gaf.c
    render/palette.c
    render/pal_lookup.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(probe_hud_gaf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(probe_hud_gaf PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
target_link_libraries(probe_hud_gaf PRIVATE SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    target_link_libraries(probe_hud_gaf PRIVATE shlwapi)
endif()

# ── Probe: dump COB script binary structure ───────────────────────────
add_executable(probe_cob
    ${CMAKE_SOURCE_DIR}/tools/probe_cob.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(probe_cob PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(probe_cob PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(probe_cob PRIVATE shlwapi)
endif()

# ── Probe: dump Zhon monarch texture palette indices ──────────────────
add_executable(cob_inspect
    ${CMAKE_SOURCE_DIR}/tools/cob_inspect.c
    render/cob.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(cob_inspect PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(cob_inspect PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(cob_inspect PRIVATE shlwapi)
endif()

add_executable(probe_zhn_pixels
    ${CMAKE_SOURCE_DIR}/tools/probe_zhn_pixels.c
    render/gaf.c
    render/palette.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(probe_zhn_pixels PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(probe_zhn_pixels PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
target_link_libraries(probe_zhn_pixels PRIVATE SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    target_link_libraries(probe_zhn_pixels PRIVATE shlwapi)
    add_custom_command(TARGET probe_zhn_pixels POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:probe_zhn_pixels>)
endif()

# ── Phase B2 recon tool (R1..R5) ───────────────────────────────────────
add_executable(recon_terrain
    ${CMAKE_SOURCE_DIR}/tools/recon_terrain.c
    game/tnt.c
    render/palette.c
    render/pal_lookup.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(recon_terrain PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(recon_terrain PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
target_link_libraries(recon_terrain PRIVATE SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    target_link_libraries(recon_terrain PRIVATE shlwapi)
    add_custom_command(TARGET recon_terrain POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:recon_terrain>)
endif()

# ── Economy unit tests ──────────────────────────────────────────────
add_executable(map_inspect
    ${CMAKE_SOURCE_DIR}/tools/map_inspect.c
    game/tnt.c
    render/palette.c
    render/pal_lookup.c
    core/parser/tdf_parser.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(map_inspect PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(map_inspect PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
target_link_libraries(map_inspect PRIVATE SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    target_link_libraries(map_inspect PRIVATE shlwapi)
    add_custom_command(TARGET map_inspect POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:map_inspect>)
endif()

add_executable(asset_validate
    ${CMAKE_SOURCE_DIR}/tools/asset_validate.c
    core/parser/tdf_parser.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(asset_validate PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(asset_validate PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(asset_validate PRIVATE shlwapi)
endif()

add_executable(test_mission
    game/test_mission.c
    game/mission.c
    core/parser/tdf_parser.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/io.c
    core/util.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_mission PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_mission PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(test_mission PRIVATE shlwapi)
endif()

add_executable(test_economy
    game/test_economy.c
    game/economy.c
)
target_include_directories(test_economy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

add_executable(test_pathing
    game/test_pathing.c
    game/pathing.c
    core/memory.c
)
target_include_directories(test_pathing PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(test_pathing PRIVATE SDL2::SDL2 SDL2::SDL2main)

add_executable(test_moveinfo
    game/test_moveinfo.c
    game/moveinfo.c
    core/parser/tdf_parser.c
    core/hpi/hpi.c
    core/hpi/hpi_vfs.c
    core/hpi/hpi_decompress.c
    core/memory.c
    core/util.c
    core/io.c
    ${TAK_THIRD_PARTY_SOURCES}
)
target_include_directories(test_moveinfo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_moveinfo PRIVATE
    TAK_DATA_DIR="${TAK_DATA_DIR}"
    TAK_GAME_DIR="${TAK_GAME_DIR}"
)
if(WIN32)
    target_link_libraries(test_moveinfo PRIVATE shlwapi)
endif()

add_executable(test_ai
    game/test_ai.c
    game/ai.c
)
target_include_directories(test_ai PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_ai PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(test_ai PRIVATE SDL2::SDL2)

add_executable(test_fog
    game/test_fog.c
    game/fog.c
    core/memory.c
)
target_include_directories(test_fog PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_fog PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(test_fog PRIVATE SDL2::SDL2)

add_executable(test_commands
    net/test_commands.c
    net/commands.c
)
target_include_directories(test_commands PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# test_bink -- diagnostic tool for .bik files. Subcommands: info / count / extract.
# Needs FFmpeg headers; skipped entirely when they're absent (CI).
if(AVCODEC_LIBRARY AND AVCODEC_INCLUDE_DIR)
    add_executable(test_bink render/test_bink.c)
    target_include_directories(test_bink PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(test_bink PRIVATE
        ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
endif()

if(BUILD_TESTING)
    add_test(NAME test_tdf_parser  COMMAND test_tdf_parser)
    add_test(NAME test_gui_loader  COMMAND test_gui_loader)
    add_test(NAME test_ui_screens  COMMAND test_ui_screens)
    add_test(NAME test_hpi         COMMAND test_hpi)
    add_test(NAME test_hpi_open    COMMAND test_hpi_open)
    add_test(NAME test_io          COMMAND test_io)
    add_test(NAME test_hpi_vfs     COMMAND test_hpi_vfs)
    add_test(NAME test_gameloop    COMMAND test_gameloop)
    add_test(NAME test_memory      COMMAND test_memory)
    add_test(NAME test_palette     COMMAND test_palette)
    add_test(NAME test_tnt         COMMAND test_tnt)
    add_test(NAME test_obj3d       COMMAND test_obj3d)
    add_test(NAME test_jpg         COMMAND test_jpg)
    add_test(NAME test_blit        COMMAND test_blit)
    add_test(NAME test_gaf         COMMAND test_gaf)
    add_test(NAME test_cob         COMMAND test_cob)
    add_test(NAME test_cob_vm      COMMAND test_cob_vm)
    add_test(NAME test_cob_vm_selftest COMMAND test_cob_vm --selftest)
    add_test(NAME test_cob_vm_corpus COMMAND test_cob_vm --corpus)
    add_test(NAME test_cob_inspect COMMAND cob_inspect --histogram --disasm --script Create scripts/araking.cob)
    add_test(NAME test_cob_inspect_all COMMAND cob_inspect --validate-all)
    add_test(NAME test_map_inspect COMMAND map_inspect --selftest)
    add_test(NAME test_map_inspect_all COMMAND map_inspect --validate-all)
    add_test(NAME test_map_inspect_missions COMMAND map_inspect --validate-missions)
    add_test(NAME test_campaign_index COMMAND map_inspect --validate-campaign-index)
    add_test(NAME test_asset_validate_units COMMAND asset_validate --units)
    add_test(NAME test_asset_validate_mission_units COMMAND asset_validate --mission-units)
    add_test(NAME test_mission     COMMAND test_mission)
    add_test(NAME test_economy     COMMAND test_economy)
    add_test(NAME test_pathing     COMMAND test_pathing)
    add_test(NAME test_moveinfo    COMMAND test_moveinfo)
    add_test(NAME test_ai          COMMAND test_ai)
    add_test(NAME test_fog         COMMAND test_fog)
    add_test(NAME test_commands    COMMAND test_commands)

    # Tests that load real game data. CI has none (and can't legally be
    # given any), so it runs `ctest --label-exclude needs-data`.
    # Maintainers run the full suite before merging.
    set_tests_properties(
        test_gui_loader test_ui_screens test_hpi test_hpi_open test_hpi_vfs
        test_palette test_tnt test_obj3d test_jpg test_gaf test_cob
        test_cob_vm test_cob_vm_corpus test_cob_inspect test_cob_inspect_all
        test_map_inspect test_map_inspect_all test_map_inspect_missions
        test_campaign_index
        test_asset_validate_units test_asset_validate_mission_units
        test_mission test_moveinfo
        PROPERTIES LABELS "needs-data")
endif()
