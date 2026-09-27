/* test_trace.c - which functions a test case entered, for the coverage
 * map scripts/coverage-map.py builds. Compiled in only when the build
 * is configured with TAK_TEST_TRACE=ON (gcc or clang, Linux), which also
 * instruments every function with -finstrument-functions.
 *
 * The hooks record each distinct function address in a set. The test
 * framework clears the set before a case and appends it to the file
 * named by TAK_TRACE_OUT after, one line per case:
 *   <case name> <hex address> <hex address> ...
 * Addresses are link-time addresses (the trace build is linked -no-pie),
 * so nm on the binary maps them to source files. */
#include "tak_test_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTRACE __attribute__((no_instrument_function))

#define TRACE_SLOTS (1u << 16)   /* power of two, well above any case */

static uintptr_t g_slots[TRACE_SLOTS];
static unsigned g_count;

NOTRACE void __cyg_profile_func_enter(void *fn, void *site);
NOTRACE void __cyg_profile_func_exit(void *fn, void *site);

void __cyg_profile_func_enter(void *fn, void *site) {
    (void)site;
    uintptr_t a = (uintptr_t)fn;
    unsigned i = (unsigned)((a >> 4) * 2654435761u) & (TRACE_SLOTS - 1);
    while (g_slots[i]) {
        if (g_slots[i] == a) return;
        i = (i + 1) & (TRACE_SLOTS - 1);
    }
    if (g_count >= TRACE_SLOTS - 1) return;
    g_slots[i] = a;
    g_count++;
}

void __cyg_profile_func_exit(void *fn, void *site) {
    (void)fn;
    (void)site;
}

static int g_wrote_case;

NOTRACE void TAK_TestTrace_Begin(void) {
    memset(g_slots, 0, sizeof g_slots);
    g_count = 0;
}

NOTRACE void TAK_TestTrace_End(const char *case_name) {
    const char *path = getenv("TAK_TRACE_OUT");
    if (!path || !path[0]) return;
    g_wrote_case = 1;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fputs(case_name, f);
    for (unsigned i = 0; i < TRACE_SLOTS; i++) {
        if (g_slots[i]) fprintf(f, " %lx", (unsigned long)g_slots[i]);
    }
    fputc('\n', f);
    fclose(f);
}

/* A test with its own harness never marks cases, so everything it ran
 * goes out at exit as one case named "*". */
NOTRACE static void trace_at_exit(void) {
    if (!g_wrote_case && g_count) TAK_TestTrace_End("*");
}

NOTRACE __attribute__((constructor)) static void trace_init(void) {
    atexit(trace_at_exit);
}
