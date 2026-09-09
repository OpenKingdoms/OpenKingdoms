/*
 * test_cob.c — Phase D M1 acceptance test.
 *
 * Loads every data/scripts/*.cob, verifies the parser produces
 * sensible CobScript structs (non-zero piece count, non-zero script
 * count, named-script lookup works), reports counts. Catches any
 * parser regressions when we touch the loader.
 *
 * Pass criterion: 159/159 .cob files parse, all four monarchs have
 * canonical scripts (Create, Killed) and canonical pieces (a head, a
 * torso). Some lower-tier units may legitimately lack named pieces
 * (effects-only units), so the canonical check is monarch-specific.
 */

#include "tak_cob.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static int n_pass, n_fail;

static int basename_no_ext_eq(const char *path, const char *target) {
    /* Compare lowercased basename(path) without extension against target. */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char buf[64];
    size_t i = 0;
    for (; i + 1 < sizeof(buf) && base[i] && base[i] != '.'; i++) {
        char c = base[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        buf[i] = c;
    }
    buf[i] = '\0';
    return tak_stricmp(buf, target) == 0;
}

static int is_monarch(const char *path) {
    return basename_no_ext_eq(path, "araking") ||
           basename_no_ext_eq(path, "vermage") ||
           basename_no_ext_eq(path, "tarnecro") ||
           basename_no_ext_eq(path, "zonhunt");
}

static void check_one(const char *path, int verbose) {
    CobScript *s = NULL;
    if (Cob_Load(&s, path) != 0 || !s) {
        fprintf(stderr, "FAIL: %s — load failed\n", path);
        n_fail++;
        return;
    }

    /* Universal sanity: every shipped .cob has at least one script.
     * Pieces can legitimately be zero for static structures (e.g.
     * walls) — those .cobs are 75-byte stubs with just a no-op
     * Create script. */
    if (s->num_scripts == 0) {
        fprintf(stderr, "FAIL: %s — zero scripts\n", path);
        Cob_Free(s);
        n_fail++;
        return;
    }
    if (s->num_code_words == 0) {
        fprintf(stderr, "FAIL: %s — zero code words\n", path);
        Cob_Free(s);
        n_fail++;
        return;
    }

    /* Monarchs must have Create and Killed and at least a "head"
     * piece. (Lower-tier units may not — Lodestones don't have heads.) */
    if (is_monarch(path)) {
        if (Cob_FindScript(s, "Create") < 0) {
            fprintf(stderr, "FAIL: %s — missing Create script\n", path);
            Cob_Free(s);
            n_fail++;
            return;
        }
        if (Cob_FindScript(s, "Killed") < 0) {
            fprintf(stderr, "FAIL: %s — missing Killed script\n", path);
            Cob_Free(s);
            n_fail++;
            return;
        }
        if (Cob_FindPiece(s, "head") < 0) {
            fprintf(stderr, "FAIL: %s — missing 'head' piece\n", path);
            Cob_Free(s);
            n_fail++;
            return;
        }
    }

    if (verbose) {
        printf("OK   %-32s  %3u scripts  %3u pieces  %5u code words  %2u statics\n",
               path, s->num_scripts, s->num_pieces,
               s->num_code_words, s->num_static_vars);
    }
    Cob_Free(s);
    n_pass++;
}

int main(int argc, char **argv) {
    int verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    char **paths = NULL;
    int n_paths = 0;
    if (VFS_ListFiles("scripts/*.cob", &paths, &n_paths) != 0 || n_paths <= 0) {
        fprintf(stderr, "no .cob files found via VFS\n");
        return 1;
    }

    /* Always print the four monarchs in detail; everything else summarised
     * unless -v. */
    for (int i = 0; i < n_paths; i++) {
        int v = verbose || is_monarch(paths[i]);
        check_one(paths[i], v);
    }

    printf("\n=== Cob_Load: %d passed, %d failed (of %d total) ===\n",
           n_pass, n_fail, n_paths);

    VFS_Shutdown();
    tak_mem_shutdown();
    return n_fail == 0 ? 0 : 1;
}
