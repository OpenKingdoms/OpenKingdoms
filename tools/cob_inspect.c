#include "tak_cob.h"
#include "tak_hpi.h"
#include "tak_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define COB_OPCODE_MASK 0x100ff000u

typedef struct OpInfo {
    uint32_t op;
    const char *name;
    int operands;
} OpInfo;

/* Names and inline-operand counts mirror the authoritative legacy-cited
 * table in src/render/cob_vm.c (OP_* defines). Keep the two in sync. */
static const OpInfo g_ops[] = {
    {0x10001000u, "MOVE", 2},
    {0x10002000u, "TURN", 2},
    {0x10003000u, "SPIN", 2},
    {0x10004000u, "STOP-SPIN", 2},
    {0x10005000u, "SHOW", 1},
    {0x10006000u, "HIDE", 1},
    {0x10007000u, "CACHE-PIECE", 1},
    {0x10008000u, "DON'T-CACHE-PIECE", 1},
    {0x10009000u, "SHADOW", 1},
    {0x1000a000u, "DON'T-SHADOW", 1},
    {0x1000b000u, "MOVE-NOW", 2},
    {0x1000c000u, "TURN-NOW", 2},
    {0x1000d000u, "SHADE", 1},
    {0x1000e000u, "DON'T-SHADE", 1},
    {0x1000f000u, "EMIT-SFX", 1},
    {0x10011000u, "WAIT-FOR-TURN", 2},
    {0x10012000u, "WAIT-FOR-MOVE", 2},
    {0x10013000u, "SLEEP", 0},
    {0x10021000u, "PUSH-CONSTANT", 1},
    {0x10022000u, "ALLOC-LOCAL", 0},
    {0x10023000u, "POP-VAR", 1},
    {0x10024000u, "POP-STACK", 0},
    {0x10031000u, "ADD", 0},
    {0x10032000u, "SUB", 0},
    {0x10033000u, "MUL", 0},
    {0x10034000u, "DIV", 0},
    {0x10035000u, "AND", 0},
    {0x10036000u, "OR", 0},
    {0x10037000u, "XOR", 0},
    {0x10038000u, "NOT", 0},
    {0x10039000u, "SHL", 0},
    {0x1003a000u, "SHR", 0},
    {0x1003b000u, "MOD", 0},
    {0x10041000u, "RAND", 0},
    {0x10042000u, "GET-UNIT-VALUE", 0},
    {0x10043000u, "GET-WITH-ARGS", 0},
    {0x10044000u, "GET-HOST-QUERY", 0},
    {0x10045000u, "GET-HOST-QUERY0", 0},
    {0x10051000u, "LT", 0},
    {0x10052000u, "LE", 0},
    {0x10053000u, "GT", 0},
    {0x10054000u, "GE", 0},
    {0x10055000u, "EQ", 0},
    {0x10056000u, "NE", 0},
    {0x10057000u, "LAND", 0},
    {0x10058000u, "LOR", 0},
    {0x10059000u, "LXOR", 0},
    {0x1005a000u, "LNOT", 0},
    {0x10061000u, "START-SCRIPT", 2},
    {0x10062000u, "CALL-SCRIPT", 2},
    {0x10063000u, "HOST-ARGS-63", 2},
    {0x10064000u, "JUMP", 1},
    {0x10065000u, "RETURN", 0},
    {0x10066000u, "JUMP-IF-FALSE", 1},
    {0x10067000u, "SIGNAL", 0},
    {0x10068000u, "SET-SIGNAL-MASK", 0},
    {0x10071000u, "EXPLODE", 1},
    {0x10072000u, "PLAY-SOUND", 1},
    {0x10073000u, "MISSION-COMMAND", 2},
    {0x10074000u, "SOUND-CMD-74", 2},
    {0x10075000u, "HOST-MARK-75", 1},
    {0x10082000u, "SET-UNIT-VALUE", 0},
    {0x10083000u, "ATTACH-UNIT", 0},
    {0x10084000u, "DROP-UNIT", 0},
};

static const OpInfo *op_info(uint32_t raw) {
    uint32_t op = raw & COB_OPCODE_MASK;
    int n = (int)(sizeof(g_ops) / sizeof(g_ops[0]));
    for (int i = 0; i < n; i++) {
        if (g_ops[i].op == op) return &g_ops[i];
    }
    return NULL;
}

static int script_end_pc(const CobScript *cob, int script_idx) {
    uint32_t start = cob->script_offsets[script_idx];
    uint32_t end = cob->num_code_words;
    for (int i = 0; i < cob->num_scripts; i++) {
        uint32_t off = cob->script_offsets[i];
        if (off > start && off < end) end = off;
    }
    return (int)end;
}

static void print_summary(const char *path, const CobScript *cob) {
    printf("COB %s\n", path);
    printf("  version=%u scripts=%u pieces=%u code_words=%u static_vars=%u\n",
           cob->version, cob->num_scripts, cob->num_pieces,
           cob->num_code_words, cob->num_static_vars);
}

static void print_tables(const CobScript *cob) {
    printf("scripts:\n");
    for (int i = 0; i < cob->num_scripts; i++) {
        printf("  [%02d] pc=%u name=%s\n", i, cob->script_offsets[i],
               cob->script_names[i] ? cob->script_names[i] : "");
    }
    printf("pieces:\n");
    for (int i = 0; i < cob->num_pieces; i++) {
        printf("  [%02d] name=%s\n", i,
               cob->piece_names[i] ? cob->piece_names[i] : "");
    }
}

static void print_histogram(const CobScript *cob) {
    int n_ops = (int)(sizeof(g_ops) / sizeof(g_ops[0]));
    int *counts = (int *)calloc((size_t)n_ops, sizeof(int));
    int unknown = 0;
    if (!counts) return;
    for (uint32_t pc = 0; pc < cob->num_code_words;) {
        const OpInfo *info = op_info(cob->code[pc]);
        if (!info) {
            unknown++;
            pc++;
            continue;
        }
        for (int i = 0; i < n_ops; i++) {
            if (&g_ops[i] == info) {
                counts[i]++;
                break;
            }
        }
        pc += 1u + (uint32_t)info->operands;
    }
    printf("opcode histogram:\n");
    for (int i = 0; i < n_ops; i++) {
        if (counts[i]) printf("  %-20s %d\n", g_ops[i].name, counts[i]);
    }
    printf("  %-20s %d\n", "UNKNOWN/DATA", unknown);
    free(counts);
}

static int call_function_id_at(const CobScript *cob, uint32_t pc, uint32_t *out_call_pc) {
    uint32_t raw = cob->code[pc];
    const OpInfo *info = op_info(raw);
    if (!info || info->op != 0x10021000u) return -1;
    uint32_t mode = raw & 7u;
    int32_t id = -1;
    if (mode == 1 && pc + 1 < cob->num_code_words) {
        id = (int32_t)cob->code[pc + 1];
    } else {
        uint32_t inl = raw & ~COB_OPCODE_MASK;
        if (inl != 0) id = (int32_t)(inl >> 16);
    }
    uint32_t next_pc = pc + 1u + (uint32_t)info->operands;
    if (next_pc >= cob->num_code_words) return -1;
    const OpInfo *next = op_info(cob->code[next_pc]);
    if (!next || (next->op != 0x10042000u && next->op != 0x10043000u)) return -1;
    if (out_call_pc) *out_call_pc = next_pc;
    return id;
}

static int script_index_for_pc(const CobScript *cob, uint32_t pc) {
    int best = -1;
    uint32_t best_off = 0;
    for (int i = 0; i < cob->num_scripts; i++) {
        uint32_t off = cob->script_offsets[i];
        if (off <= pc && (best < 0 || off >= best_off)) {
            best = i;
            best_off = off;
        }
    }
    return best;
}

static void collect_call_function_ids(const CobScript *cob, int *ids, int id_cap) {
    if (!cob || !ids || id_cap <= 0) return;
    for (uint32_t pc = 0; pc < cob->num_code_words;) {
        uint32_t raw = cob->code[pc];
        const OpInfo *info = op_info(raw);
        if (!info) {
            pc++;
            continue;
        }
        uint32_t call_pc = 0;
        int id = call_function_id_at(cob, pc, &call_pc);
        if (id >= 0 && id < id_cap) ids[id]++;
        pc += 1u + (uint32_t)info->operands;
    }
}

static void print_call_function_ids_one(const char *path, const CobScript *cob) {
    int ids[256] = {0};
    collect_call_function_ids(cob, ids, 256);
    printf("call-function ids: %s\n", path);
    for (int i = 0; i < 256; i++) {
        if (ids[i]) printf("  fn=%d count=%d\n", i, ids[i]);
    }
}

static int print_call_function_ids_all(void) {
    char **paths = NULL;
    int count = 0;
    int ids[256] = {0};
    int failures = 0;
    if (VFS_ListFiles("scripts/*.cob", &paths, &count) != 0 || count <= 0) {
        fprintf(stderr, "cob_inspect: no scripts/*.cob files found\n");
        return 1;
    }
    for (int i = 0; i < count; i++) {
        CobScript *cob = NULL;
        if (Cob_Load(&cob, paths[i]) != 0 || !cob) {
            fprintf(stderr, "cob_inspect: failed %s\n", paths[i]);
            failures++;
        } else {
            collect_call_function_ids(cob, ids, 256);
        }
        Cob_Free(cob);
        tak_free(paths[i]);
    }
    tak_free(paths);
    printf("call-function ids across %d COB file(s), failures=%d\n", count, failures);
    for (int i = 0; i < 256; i++) {
        if (ids[i]) printf("  fn=%d count=%d\n", i, ids[i]);
    }
    return failures ? 1 : 0;
}

static void print_call_sites_one(const char *path, const CobScript *cob) {
    for (uint32_t pc = 0; pc < cob->num_code_words;) {
        uint32_t raw = cob->code[pc];
        const OpInfo *info = op_info(raw);
        if (!info) {
            pc++;
            continue;
        }
        uint32_t call_pc = 0;
        int id = call_function_id_at(cob, pc, &call_pc);
        if (id >= 0) {
            int si = script_index_for_pc(cob, pc);
            printf("%s script=%s pc=%u call_pc=%u fn=%d\n",
                   path,
                   (si >= 0 && cob->script_names[si]) ? cob->script_names[si] : "?",
                   pc, call_pc, id);
        }
        pc += 1u + (uint32_t)info->operands;
    }
}

static int print_call_sites_all(void) {
    char **paths = NULL;
    int count = 0;
    int failures = 0;
    if (VFS_ListFiles("scripts/*.cob", &paths, &count) != 0 || count <= 0) {
        fprintf(stderr, "cob_inspect: no scripts/*.cob files found\n");
        return 1;
    }
    for (int i = 0; i < count; i++) {
        CobScript *cob = NULL;
        if (Cob_Load(&cob, paths[i]) != 0 || !cob) {
            fprintf(stderr, "cob_inspect: failed %s\n", paths[i]);
            failures++;
        } else {
            print_call_sites_one(paths[i], cob);
        }
        Cob_Free(cob);
        tak_free(paths[i]);
    }
    tak_free(paths);
    return failures ? 1 : 0;
}

static void disasm_script(const CobScript *cob, int script_idx) {
    int end_pc = script_end_pc(cob, script_idx);
    uint32_t pc = cob->script_offsets[script_idx];
    printf("disasm script[%d] %s pc=%u..%d\n", script_idx,
           cob->script_names[script_idx] ? cob->script_names[script_idx] : "",
           pc, end_pc);
    while (pc < cob->num_code_words && (int)pc < end_pc) {
        uint32_t raw = cob->code[pc];
        const OpInfo *info = op_info(raw);
        uint32_t inl = raw & ~COB_OPCODE_MASK;
        if (!info) {
            printf("  %06u: 0x%08x UNKNOWN\n", pc, raw);
            pc++;
            continue;
        }
        printf("  %06u: 0x%08x %-20s", pc, raw, info->name);
        if (inl) printf(" inline=0x%x", inl);
        for (int i = 0; i < info->operands; i++) {
            uint32_t op_pc = pc + 1u + (uint32_t)i;
            if (op_pc < cob->num_code_words) {
                printf(" arg%d=%u", i, cob->code[op_pc]);
            } else {
                printf(" arg%d=<oob>", i);
            }
        }
        printf("\n");
        pc += 1u + (uint32_t)info->operands;
    }
}

static int validate_all(void) {
    char **paths = NULL;
    int count = 0;
    int failures = 0;
    if (VFS_ListFiles("scripts/*.cob", &paths, &count) != 0 || count <= 0) {
        fprintf(stderr, "cob_inspect: no scripts/*.cob files found\n");
        return 1;
    }
    for (int i = 0; i < count; i++) {
        CobScript *cob = NULL;
        if (Cob_Load(&cob, paths[i]) != 0 || !cob) {
            fprintf(stderr, "cob_inspect: failed %s\n", paths[i]);
            failures++;
        }
        Cob_Free(cob);
        tak_free(paths[i]);
    }
    tak_free(paths);
    printf("validated %d COB file(s), failures=%d\n", count, failures);
    return failures ? 1 : 0;
}

static void usage(void) {
    printf("usage: cob_inspect [--validate-all] [--tables] [--histogram] "
           "[--disasm] [--call-functions] [--call-sites] [--script NAME] [vfs_path]\n");
}

int main(int argc, char **argv) {
    const char *path = "scripts/araking.cob";
    const char *script = NULL;
    int tables = 0, histogram = 0, disasm = 0, validate = 0, call_functions = 0, call_sites = 0;
    int rc = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tables") == 0) tables = 1;
        else if (strcmp(argv[i], "--histogram") == 0) histogram = 1;
        else if (strcmp(argv[i], "--disasm") == 0) disasm = 1;
        else if (strcmp(argv[i], "--call-functions") == 0) call_functions = 1;
        else if (strcmp(argv[i], "--call-sites") == 0) call_sites = 1;
        else if (strcmp(argv[i], "--validate-all") == 0) validate = 1;
        else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) script = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else path = argv[i];
    }

    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "cob_inspect: VFS_Init failed\n");
        goto done_mem;
    }

    if (validate) {
        rc = call_sites ? print_call_sites_all()
           : call_functions ? print_call_function_ids_all()
           : validate_all();
        goto done_vfs;
    }

    CobScript *cob = NULL;
    if (Cob_Load(&cob, path) != 0 || !cob) {
        fprintf(stderr, "cob_inspect: failed to load %s\n", path);
        goto done_vfs;
    }

    print_summary(path, cob);
    if (!tables && !histogram && !disasm && !call_functions && !call_sites) tables = histogram = 1;
    if (tables) print_tables(cob);
    if (histogram) print_histogram(cob);
    if (call_functions) print_call_function_ids_one(path, cob);
    if (call_sites) print_call_sites_one(path, cob);
    if (disasm) {
        int idx = script ? Cob_FindScript(cob, script) : 0;
        if (idx < 0) {
            fprintf(stderr, "cob_inspect: script not found: %s\n", script);
            Cob_Free(cob);
            goto done_vfs;
        }
        disasm_script(cob, idx);
    }
    Cob_Free(cob);
    rc = 0;

done_vfs:
    VFS_Shutdown();
done_mem:
    tak_mem_shutdown();
    return rc;
}
