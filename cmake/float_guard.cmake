# Integer-only guard for simulation modules.
#
# Lockstep multiplayer desyncs on a float in simulation state, so the
# files listed by the caller must not name float, double or math.h at
# all, comments included. The list lives in src/CMakeLists.txt and grows
# as each new simulation module lands.
#
#   cmake -DFILES=<path>|<path>|... -P cmake/float_guard.cmake

if(NOT DEFINED FILES)
    message(FATAL_ERROR "float guard: pass -DFILES=<path>|<path>")
endif()

string(REPLACE "|" ";" guard_files "${FILES}")
set(guard_bad "")

foreach(f IN LISTS guard_files)
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR
            "float guard: ${f} is missing. Fix the list in src/CMakeLists.txt.")
    endif()
    file(STRINGS "${f}" hits REGEX "float|double|math\\.h")
    if(hits)
        get_filename_component(name "${f}" NAME)
        foreach(line IN LISTS hits)
            message(STATUS "  ${name}: ${line}")
        endforeach()
        list(APPEND guard_bad "${name}")
    endif()
endforeach()

if(guard_bad)
    message(FATAL_ERROR
        "float guard: these must stay integer only: ${guard_bad}")
endif()

list(LENGTH guard_files guard_count)
message(STATUS "float guard: ${guard_count} file(s) are integer only")
