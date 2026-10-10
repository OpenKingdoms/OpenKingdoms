# No test may reach the player's own preference directory, which holds
# the options.cfg a test run would rewrite. Three checks, none of which
# needs a judgement call:
#
#   1. Every registered test runs with TAK_CONFIG_DIR naming a folder of
#      its own inside the build tree.
#   2. Every program that compiles the paths module is the game or a test
#      build, and the game is not a test build. A test build never asks
#      the platform where the preference directory is.
#   3. No source but src/core/paths.c names a call or a variable that
#      finds the platform's per-user folders.
#
#   cmake -DMANIFEST=<file> -DROOT=<source tree> -DBUILD=<build tree>
#         -P cmake/prefs_guard.cmake
#
# The manifest is written at configure time by src/CMakeLists.txt, after
# every test and target in that directory exists.

foreach(v MANIFEST ROOT BUILD)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "prefs guard: pass -D${v}=...")
    endif()
endforeach()
if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR "prefs guard: ${MANIFEST} is missing, reconfigure the tree")
endif()

set(bad "")
set(seen_dirs "")
set(test_count 0)
file(STRINGS "${MANIFEST}" rows)
foreach(row IN LISTS rows)
    string(REPLACE "|" ";" f "${row}")
    list(GET f 0 kind)
    list(GET f 1 name)
    list(LENGTH f nf)
    set(val "")
    if(nf GREATER 2)
        list(GET f 2 val)
    endif()
    if(kind STREQUAL "test")
        math(EXPR test_count "${test_count} + 1")
        if(val STREQUAL "")
            list(APPEND bad "test ${name} has no TAK_CONFIG_DIR")
        else()
            string(FIND "${val}" "${BUILD}/" at)
            if(NOT at EQUAL 0)
                list(APPEND bad "test ${name} keeps its options outside the build tree: ${val}")
            endif()
            if(val IN_LIST seen_dirs)
                list(APPEND bad "test ${name} shares its options folder: ${val}")
            endif()
            list(APPEND seen_dirs "${val}")
        endif()
    elseif(kind STREQUAL "binary")
        if(name STREQUAL "tak-re")
            if(val STREQUAL "1")
                list(APPEND bad "the game is built as a test build and would lose the player's options")
            endif()
        elseif(NOT val STREQUAL "1")
            list(APPEND bad "${name} compiles the paths module but is not a test build")
        endif()
    endif()
endforeach()
if(test_count EQUAL 0)
    list(APPEND bad "the manifest lists no tests")
endif()

# The platform's per-user folders, by every name the engine could reach
# them through. Comments count, so a reader never has to decide whether
# a mention is a call.
set(platform_re "SDL_GetPrefPath|SHGetFolderPath|SHGetKnownFolderPath|CSIDL_APPDATA|FOLDERID_RoamingAppData|\"APPDATA\"|\"LOCALAPPDATA\"|XDG_DATA_HOME|XDG_CONFIG_HOME|NSApplicationSupportDirectory")
file(GLOB_RECURSE sources
    "${ROOT}/src/*.c" "${ROOT}/src/*.h"
    "${ROOT}/include/*.h" "${ROOT}/tools/*.c")
foreach(src IN LISTS sources)
    file(RELATIVE_PATH rel "${ROOT}" "${src}")
    if(rel STREQUAL "src/core/paths.c")
        continue()
    endif()
    file(STRINGS "${src}" hits REGEX "${platform_re}")
    if(hits)
        list(APPEND bad "${rel} names the platform's per-user folders, only src/core/paths.c may")
    endif()
endforeach()

if(bad)
    foreach(b IN LISTS bad)
        message(STATUS "  ${b}")
    endforeach()
    list(LENGTH bad n)
    message(FATAL_ERROR "prefs guard: ${n} way(s) a test could reach the player's own options")
endif()
message(STATUS "prefs guard: ${test_count} test(s) keep their options in the build tree")
