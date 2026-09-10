/*
 * test_cob_vm.c — Phase D M2 acceptance test.
 *
 * Loads a unit's .cob, runs its Create script, dumps the resulting
 * piece state and surviving thread count. The verbose output is the
 * primary diagnostic — when an opcode hits "unknown", we know what
 * to implement next.
 *
 * Usage:
 *   test_cob_vm                                   -- run araking Create
 *   test_cob_vm -v                                -- + opcode trace
 *   test_cob_vm -s walk                           -- run a different script
 *   test_cob_vm -t 60                             -- run for 60 sim ticks
 *   test_cob_vm -p                                -- print piece state every tick
 *   test_cob_vm scripts/foo.cob -s walk -t 30 -p
 */

#include "tak_cob.h"
#include "tak_cob_vm.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

extern void Cob_SetTrace(int on);

/* Opcode values mirror the legacy-verified table in cob_vm.c
 * (legacy:306300-307004). */
#define T_OP_MOVE             0x10001000u
#define T_OP_TURN             0x10002000u
#define T_OP_SPIN             0x10003000u
#define T_OP_STOP_SPIN        0x10004000u
#define T_OP_DONT_SHADOW      0x1000a000u
#define T_OP_SHADE            0x1000d000u
#define T_OP_DONT_SHADE       0x1000e000u
#define T_OP_EMIT_SFX         0x1000f000u
#define T_OP_POP_VAR_STATIC   0x10023004u
#define T_OP_GET_HOST_QUERY   0x10044000u
#define T_OP_CALL_SCRIPT      0x10062000u
#define T_OP_RETURN           0x10065000u
#define T_OP_JUMP_IF_FALSE    0x10066000u
#define T_OP_SIGNAL           0x10067000u
#define T_OP_SET_SIGNAL_MASK  0x10068000u
#define T_OP_EXPLODE          0x10071000u
#define T_OP_HOST_MARK_75     0x10075000u
#define T_OP_SET_UNIT_VALUE   0x10082000u
#define T_OP_ATTACH_UNIT      0x10083000u
#define T_OP_GET_UNIT_VALUE   0x10042000u
#define T_OP_PUSH_CONSTANT    0x10021001u
#define T_OP_ALLOC_LOCAL      0x10022000u
#define T_OP_POP_VAR_LOCAL    0x10023002u

static int selftest_script(CobScript *s, const uint32_t *code,
                           uint32_t n_code, uint32_t n_static,
                           uint16_t n_pieces) {
    static char *script_names[] = { "Test" };
    static uint32_t script_offsets[] = { 0 };
    static char *piece_names[] = { "piece0" };
    memset(s, 0, sizeof(*s));
    s->version = 6;
    s->num_static_vars = n_static;
    s->code = (uint32_t *)code;
    s->num_code_words = n_code;
    s->script_names = script_names;
    s->script_offsets = script_offsets;
    s->num_scripts = 1;
    s->num_pieces = n_pieces;
    s->piece_names = piece_names;
    return 0;
}

static int32_t g_selftest_set_port = -1;
static int32_t g_selftest_set_value = 0;
static void selftest_set_unit_value(void *user, int port, int32_t value) {
    (void)user;
    g_selftest_set_port = port;
    g_selftest_set_value = value;
}

static int run_selftests(void) {
    int failed = 0;
    tak_mem_init();

    {
        /* EXPLODE (0x10071000): inline piece, pops explosion type. */
        uint32_t code[] = { T_OP_PUSH_CONSTANT, 3,
                            T_OP_EXPLODE, 0,
                            T_OP_RETURN };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 1);
        CobEngine e;
        const char *nodes[] = { "piece0" };
        if (Cob_EngineInit(&e, &s, 1, nodes) != 0) return 1;
        Cob_StartThread(&e, 0, NULL, 0);
        Cob_RunAllThreads(&e);
        if (!e.pieces[0].exploded || Cob_AliveThreadCount(&e) != 0) {
            fprintf(stderr, "selftest EXPLODE failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* GET-HOST-QUERY pops one, pushes host result (0 with no host);
         * POP-VAR static mode stores it. */
        uint32_t code[] = { T_OP_GET_HOST_QUERY, T_OP_POP_VAR_STATIC, 0,
                            T_OP_RETURN };
        CobScript s;
        selftest_script(&s, code, 4, 1, 0);
        CobEngine e;
        if (Cob_EngineInit(&e, &s, 0, NULL) != 0) return 1;
        int32_t args[] = { 123 };
        Cob_StartThread(&e, 0, args, 1);
        Cob_RunAllThreads(&e);
        if (e.static_vars[0] != 0 || Cob_AliveThreadCount(&e) != 0) {
            fprintf(stderr, "selftest GET_HOST_QUERY failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* Host no-op opcodes keep PC/stack aligned: shade pair (inline
         * piece), EMIT-SFX (inline piece + pop), DONT-SHADOW (inline
         * piece), HOST-MARK-75 (inline arg). */
        uint32_t code[] = {
            T_OP_SHADE, 0,
            T_OP_DONT_SHADE, 0,
            T_OP_EMIT_SFX, 0,
            T_OP_DONT_SHADOW, 0,
            T_OP_HOST_MARK_75, 0,
            T_OP_RETURN
        };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 1);
        CobEngine e;
        const char *nodes[] = { "piece0" };
        if (Cob_EngineInit(&e, &s, 1, nodes) != 0) return 1;
        int32_t args[] = { 7 };   /* consumed by EMIT-SFX */
        Cob_StartThread(&e, 0, args, 1);
        Cob_RunAllThreads(&e);
        if (Cob_AliveThreadCount(&e) != 0 ||
            !e.pieces[0].shade_off || !e.pieces[0].shadow_off) {
            fprintf(stderr, "selftest host no-op opcodes failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* SET-VALUE (0x10082000): push port, push value; host receives
         * (port, value). */
        uint32_t code[] = {
            T_OP_PUSH_CONSTANT, 18,
            T_OP_PUSH_CONSTANT, 1,
            T_OP_SET_UNIT_VALUE,
            T_OP_RETURN
        };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 0);
        CobEngine e;
        if (Cob_EngineInit(&e, &s, 0, NULL) != 0) return 1;
        g_selftest_set_port = -1;
        g_selftest_set_value = 0;
        Cob_EngineSetHostSetter(&e, selftest_set_unit_value);
        Cob_StartThread(&e, 0, NULL, 0);
        Cob_RunAllThreads(&e);
        if (g_selftest_set_port != 18 || g_selftest_set_value != 1 ||
            Cob_AliveThreadCount(&e) != 0) {
            fprintf(stderr, "selftest SET_UNIT_VALUE failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* ATTACH-UNIT pops three values, no other effect. */
        uint32_t code[] = {
            T_OP_ATTACH_UNIT,
            T_OP_RETURN
        };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 1);
        CobEngine e;
        const char *nodes[] = { "piece0" };
        if (Cob_EngineInit(&e, &s, 1, nodes) != 0) return 1;
        int32_t args[] = { 0, 2, 99 };
        Cob_StartThread(&e, 0, args, 3);
        Cob_RunAllThreads(&e);
        if (Cob_AliveThreadCount(&e) != 0) {
            fprintf(stderr, "selftest ATTACH_UNIT failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        uint32_t code[] = {
            T_OP_PUSH_CONSTANT, 77,
            T_OP_RETURN
        };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 0);
        CobEngine e;
        if (Cob_EngineInit(&e, &s, 0, NULL) != 0) return 1;
        int slot = Cob_StartThread(&e, 0, NULL, 0);
        Cob_RunAllThreads(&e);
        int32_t ret = 0;
        if (Cob_AliveThreadCount(&e) != 0 ||
            !Cob_GetThreadReturn(&e, slot, &ret) || ret != 77) {
            fprintf(stderr, "selftest RETURN value failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* Cob_RunScriptSync: run now, read the out-arg back. This is the
         * shape of every shipped QueryBuildInfo: `piecenum = 1` then a
         * return compiles to ALLOC-LOCAL, PUSH 1, POP-VAR local 0. The
         * out-arg is seeded to -1 so an untouched slot stays "no piece"
         * (legacy:306142-306208). */
        uint32_t code[] = {
            T_OP_ALLOC_LOCAL,
            T_OP_PUSH_CONSTANT, 1,
            T_OP_POP_VAR_LOCAL, 0,
            T_OP_RETURN
        };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 0);
        CobEngine e;
        if (Cob_EngineInit(&e, &s, 0, NULL) != 0) return 1;
        int32_t qa[4] = { -1, 0, 0, 0 };
        int rc = Cob_RunScriptSync(&e, "Test", qa, 4);
        if (rc != 0 || qa[0] != 1 || qa[1] != 0 ||
            Cob_AliveThreadCount(&e) != 0) {
            fprintf(stderr,
                    "selftest RunScriptSync failed (rc=%d out=%d alive=%d)\n",
                    rc, qa[0], Cob_AliveThreadCount(&e));
            failed = 1;
        }
        /* A script the unit does not define leaves the seed alone. */
        int32_t qb[4] = { -1, 0, 0, 0 };
        if (Cob_RunScriptSync(&e, "QueryBuildInfo", qb, 4) != -1 ||
            qb[0] != -1) {
            fprintf(stderr, "selftest RunScriptSync missing-script failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* SIGNAL kills every thread whose mask matches — including the
         * signalling thread itself (the legacy VM scans all 16 slots). */
        uint32_t code[] = { T_OP_SET_SIGNAL_MASK, T_OP_SIGNAL, T_OP_RETURN };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 0);
        CobEngine e;
        if (Cob_EngineInit(&e, &s, 0, NULL) != 0) return 1;
        e.threads[1].alive = 1;
        e.threads[1].signal_mask = 0x0004;
        e.active_thread_count = 1;
        int32_t args[] = { 0x0004, 0x0004 };
        Cob_StartThread(&e, 0, args, 2);
        Cob_RunAllThreads(&e);
        if (e.threads[1].alive || Cob_AliveThreadCount(&e) != 0) {
            fprintf(stderr, "selftest signal-kill opcode failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* JUMP-IF-FALSE branches on ZERO (legacy:306937-306941).
         * cond=0 jumps over the poison word to RETURN; a wrong
         * fall-through hits opcode 0xdeadbeef and terminates with an
         * unknown-opcode diagnostic. */
        uint32_t code[] = {
            T_OP_PUSH_CONSTANT, 0,
            T_OP_JUMP_IF_FALSE, 5,
            0xdeadbeefu,
            T_OP_PUSH_CONSTANT, 42,
            T_OP_RETURN
        };
        CobScript s;
        selftest_script(&s, code, (uint32_t)(sizeof(code) / sizeof(code[0])), 0, 0);
        CobEngine e;
        if (Cob_EngineInit(&e, &s, 0, NULL) != 0) return 1;
        Cob_ResetDiagnostics();
        int slot = Cob_StartThread(&e, 0, NULL, 0);
        Cob_RunAllThreads(&e);
        int32_t ret = 0;
        if (Cob_GetUnknownOpcodeCount() != 0 ||
            !Cob_GetThreadReturn(&e, slot, &ret) || ret != 42) {
            fprintf(stderr, "selftest JUMP_IF_FALSE failed\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    {
        /* CALL-SCRIPT blocks the caller until the child RETURNs. The
         * child sleeps one tick, so after the first run the caller must
         * still be alive and waiting; after enough ticks both finish. */
        static uint32_t code[] = {
            /* script 0 "Test" @0: call script 1 with 0 args, then return */
            T_OP_CALL_SCRIPT, 1, 0,
            T_OP_RETURN,
            /* script 1 "Child" @4: sleep 100ms, return */
            T_OP_PUSH_CONSTANT, 100,
            0x10013000u /* SLEEP */,
            T_OP_RETURN
        };
        static char *script_names[] = { "Test", "Child" };
        static uint32_t script_offsets[] = { 0, 4 };
        CobScript s;
        memset(&s, 0, sizeof(s));
        s.version = 6;
        s.code = code;
        s.num_code_words = (uint32_t)(sizeof(code) / sizeof(code[0]));
        s.script_names = script_names;
        s.script_offsets = script_offsets;
        s.num_scripts = 2;
        CobEngine e;
        if (Cob_EngineInit(&e, &s, 0, NULL) != 0) return 1;
        Cob_StartThread(&e, 0, NULL, 0);
        Cob_RunAllThreads(&e);
        if (Cob_AliveThreadCount(&e) != 2) {
            fprintf(stderr, "selftest CALL_SCRIPT: caller did not block "
                    "(alive=%d)\n", Cob_AliveThreadCount(&e));
            failed = 1;
        }
        for (int tick = 0; tick < 12 && Cob_AliveThreadCount(&e) > 0; tick++) {
            Cob_RunAllThreads(&e);
        }
        if (Cob_AliveThreadCount(&e) != 0) {
            fprintf(stderr, "selftest CALL_SCRIPT: did not finish\n");
            failed = 1;
        }
        Cob_EngineFree(&e);
    }

    tak_mem_shutdown();
    if (!failed) printf("COB VM selftests passed\n");
    return failed ? 1 : 0;
}

static int piece_state_count(const CobEngine *e) {
    int touched = 0;
    for (int i = 0; i < e->piece_count; i++) {
        const CobPiece *p = &e->pieces[i];
        if (p->hidden || p->exploded ||
            p->rot[0] || p->rot[1] || p->rot[2] ||
            p->rot_target[0] || p->rot_target[1] || p->rot_target[2] ||
            p->rot_speed[0] || p->rot_speed[1] || p->rot_speed[2] ||
            p->pos[0] || p->pos[1] || p->pos[2] ||
            p->pos_target[0] || p->pos_target[1] || p->pos_target[2] ||
            p->pos_speed[0] || p->pos_speed[1] || p->pos_speed[2]) {
            touched++;
        }
    }
    return touched;
}

static int run_one_script_smoke(const char *path, const char *script_name,
                                int ticks, int *out_touched) {
    CobScript *script = NULL;
    if (Cob_Load(&script, path) != 0 || !script) return 0;
    int script_idx = Cob_FindScript(script, script_name);
    if (script_idx < 0) {
        Cob_Free(script);
        return 0;
    }

    CobEngine engine;
    int rc = Cob_EngineInit(&engine, script, script->num_pieces,
                             (const char *const *)script->piece_names);
    if (rc != 0) {
        Cob_Free(script);
        return -1;
    }
    if (Cob_StartThread(&engine, script_idx, NULL, 0) < 0) {
        Cob_EngineFree(&engine);
        Cob_Free(script);
        return -1;
    }
    for (int t = 0; t < ticks; t++) {
        Cob_AnimatePieces(&engine);
        Cob_RunAllThreads(&engine);
    }
    if (out_touched) *out_touched = piece_state_count(&engine);
    Cob_EngineFree(&engine);
    Cob_Free(script);
    return 1;
}

static int run_corpus_smoke(void) {
    int failures = 0;
    int ran = 0;
    int no_effect = 0;
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    char **paths = NULL;
    int n_paths = 0;
    if (VFS_ListFiles("scripts/*.cob", &paths, &n_paths) != 0 || n_paths <= 0) {
        fprintf(stderr, "no .cob files found via VFS\n");
        VFS_Shutdown();
        tak_mem_shutdown();
        return 1;
    }

    static const char *scripts[] = {
        "Create", "walk", "Killed", "StartBuilding", "FireWeapon", "AimWeapon"
    };
    for (int i = 0; i < n_paths; i++) {
        for (int s = 0; s < (int)(sizeof(scripts) / sizeof(scripts[0])); s++) {
            Cob_ResetDiagnostics();
            int touched = 0;
            int rc = run_one_script_smoke(paths[i], scripts[s], 120, &touched);
            if (rc == 0) continue;
            ran++;
            int unknown = Cob_GetUnknownOpcodeCount();
            if (rc < 0 || unknown > 0) {
                fprintf(stderr, "FAIL %s:%s rc=%d unknown=%d\n",
                        paths[i], scripts[s], rc, unknown);
                failures++;
            } else if (touched == 0) {
                no_effect++;
                printf("NOEFFECT %s:%s\n", paths[i], scripts[s]);
            }
        }
    }

    printf("COB corpus smoke: ran=%d failures=%d no_effect=%d files=%d\n",
           ran, failures, no_effect, n_paths);
    VFS_Shutdown();
    tak_mem_shutdown();
    return failures == 0 ? 0 : 1;
}

static void dump_pieces(const CobEngine *e, const CobScript *s) {
    int touched = 0;
    for (int i = 0; i < e->piece_count; i++) {
        const CobPiece *p = &e->pieces[i];
        int has_state = p->hidden || p->exploded
                     || p->rot[0] || p->rot[1] || p->rot[2]
                     || p->rot_target[0] || p->rot_target[1] || p->rot_target[2]
                     || p->rot_speed[0] || p->rot_speed[1] || p->rot_speed[2]
                     || p->pos[0] || p->pos[1] || p->pos[2];
        if (has_state) {
            const char *name = (i < (int)s->num_pieces) ? s->piece_names[i] : "?";
            printf("  piece[%2d] %-18s%s%s rot=(%d,%d,%d) tgt=(%d,%d,%d) spd=(%d,%d,%d)\n",
                   i, name,
                   p->hidden ? " HIDDEN" : "",
                   p->exploded ? " EXPLODED" : "",
                   p->rot[0], p->rot[1], p->rot[2],
                   p->rot_target[0], p->rot_target[1], p->rot_target[2],
                   p->rot_speed[0], p->rot_speed[1], p->rot_speed[2]);
            touched++;
        }
    }
    printf("  (%d pieces with non-default state of %d total)\n",
           touched, e->piece_count);
}

int main(int argc, char **argv) {
    int verbose = 0;
    int print_per_tick = 0;
    int total_ticks = 10;
    const char *script_name = "Create";
    const char *path = "scripts/araking.cob";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            return run_selftests();
        } else if (strcmp(argv[i], "--corpus") == 0) {
            return run_corpus_smoke();
        } else if (strcmp(argv[i], "-p") == 0) {
            print_per_tick = 1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            script_name = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            total_ticks = atoi(argv[++i]);
        } else {
            path = argv[i];
        }
    }

    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    CobScript *script = NULL;
    if (Cob_Load(&script, path) != 0 || !script) {
        fprintf(stderr, "Failed to load %s\n", path);
        return 1;
    }

    printf("=== %s ===\n", path);
    printf("  scripts=%u  pieces=%u  code_words=%u  static_vars=%u\n",
           script->num_scripts, script->num_pieces,
           script->num_code_words, script->num_static_vars);

    int script_idx = Cob_FindScript(script, script_name);
    if (script_idx < 0) {
        fprintf(stderr, "No script '%s' in %s\n", script_name, path);
        return 1;
    }
    printf("  Running '%s' @ word %u for %d ticks\n\n",
           script_name, script->script_offsets[script_idx], total_ticks);

    /* For the standalone test, use the script's own piece names as
     * node names — every piece binds to itself. Real game passes the
     * mesh's node names. */
    CobEngine engine;
    if (Cob_EngineInit(&engine, script,
                        script->num_pieces,
                        (const char *const *)script->piece_names) != 0) {
        fprintf(stderr, "Cob_EngineInit failed\n");
        return 1;
    }

    Cob_SetTrace(verbose);

    if (Cob_StartThread(&engine, script_idx, NULL, 0) < 0) {
        fprintf(stderr, "Failed to start '%s' thread\n", script_name);
        return 1;
    }

    /* Sim tick: animate pieces (advance current toward target), then
     * run all threads (which may set new targets / start new threads). */
    int ticks = 0;
    for (; ticks < total_ticks; ticks++) {
        Cob_AnimatePieces(&engine);
        Cob_RunAllThreads(&engine);
        if (verbose) {
            printf("  -- end of tick %d, alive threads = %d\n",
                   ticks + 1, Cob_AliveThreadCount(&engine));
        }
        if (print_per_tick) {
            printf("--- tick %d ---\n", ticks + 1);
            dump_pieces(&engine, script);
        }
    }

    printf("\n=== Final state after %d ticks ===\n", ticks);
    printf("Alive threads: %d\n", Cob_AliveThreadCount(&engine));
    printf("Piece state:\n");
    dump_pieces(&engine, script);

    /* Static vars touched? */
    int static_touched = 0;
    for (uint32_t i = 0; i < script->num_static_vars; i++) {
        if (engine.static_vars[i] != 0) {
            printf("  static[%u] = %d\n", i, engine.static_vars[i]);
            static_touched++;
        }
    }
    if (static_touched) {
        printf("  (%d static vars set)\n", static_touched);
    }

    Cob_EngineFree(&engine);
    Cob_Free(script);
    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
