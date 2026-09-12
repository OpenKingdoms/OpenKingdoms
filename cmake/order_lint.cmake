# No screen orders a unit directly.
#
# Every player action is a command, queued and applied on its tick, so
# the screens hand what was clicked to the command emitter and never
# call an order function themselves. This test fails if one does.
#
#   cmake -DSRC=<src dir> -P cmake/order_lint.cmake
#
# loading.c is exempt: it applies the mission file's scripted orders
# while the world is built, before tick zero, the same on every
# machine, and no player input reaches it.
#
# perf_probe.c is exempt for the same reason and one more: it is a
# measuring harness that drives synthetic units to time the engine, no
# player input reaches it, and putting its scenario through the queue
# would measure the queue instead of the thing being measured.

if(NOT DEFINED SRC)
    message(FATAL_ERROR "order lint: pass -DSRC=<src dir>")
endif()

file(GLOB lint_files "${SRC}/ui/*.c" "${SRC}/render/minimap.c")
set(lint_exempt "loading.c" "perf_probe.c")

# Units_Command* and Units_Order* are the order functions, and the
# factory, gate and build calls change the simulation just the same.
set(lint_pattern
    "Units_Command[A-Za-z]*[ \t]*\\(|Units_Order[A-Za-z]*[ \t]*\\(|Units_Factory(Enqueue|DequeueDef|CancelCurrent|SetRally)[ \t]*\\(|Units_SetGateOpen[ \t]*\\(|Units_ToggleSelectedGate[ \t]*\\(|Units_BeginBuilding[A-Za-z]*[ \t]*\\(|Units_StopUnit[ \t]*\\(|Units_SetOwner[ \t]*\\(")

set(lint_bad "")
set(lint_count 0)
foreach(f IN LISTS lint_files)
    get_filename_component(name "${f}" NAME)
    if(name MATCHES "^test_")
        continue()
    endif()
    list(FIND lint_exempt "${name}" at)
    if(NOT at EQUAL -1)
        continue()
    endif()
    math(EXPR lint_count "${lint_count} + 1")
    file(STRINGS "${f}" hits REGEX "${lint_pattern}")
    if(hits)
        foreach(line IN LISTS hits)
            message(STATUS "  ${name}: ${line}")
        endforeach()
        list(APPEND lint_bad "${name}")
    endif()
endforeach()

if(lint_bad)
    list(REMOVE_DUPLICATES lint_bad)
    message(FATAL_ERROR
        "order lint: these screens order units directly instead of "
        "emitting a command: ${lint_bad}")
endif()

message(STATUS "order lint: ${lint_count} screen file(s) emit commands only")
