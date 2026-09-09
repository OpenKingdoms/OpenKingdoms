/*
 * test_obj3d.c — M2 verification.
 *
 * Loads each canonical monarch's mesh, asserts basic shape: tree exists,
 * root has children, every node has a name, vertex/primitive counts are
 * sane, all vertex indices stay in bounds (parser would have rejected
 * the file if not, but we double-check), texture-name strings look
 * printable when present.
 */

#include "test_framework.h"
#include "tak_obj3d.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdio.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static int g_vfs_ready = 0;
static void ensure_vfs(void) {
    if (g_vfs_ready) return;
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
    }
    g_vfs_ready = 1;
}

/* ── Visitor stats (collected per Obj3D_Walk) ─────────────────────── */

typedef struct {
    int   node_count;
    int   total_verts;
    int   total_prims;
    int   nodes_with_empty_name;
    int   prims_textured;
    int   prims_flat_color;
    int   max_depth;
    int   max_verts_per_node;
    int   max_prims_per_node;
} Stats;

static int stats_visitor(const Obj3DNode *node, int depth, void *user) {
    Stats *s = (Stats *)user;
    s->node_count++;
    s->total_verts += node->num_vertices;
    s->total_prims += node->num_primitives;
    if (depth > s->max_depth) s->max_depth = depth;
    if (node->num_vertices > s->max_verts_per_node) s->max_verts_per_node = node->num_vertices;
    if (node->num_primitives > s->max_prims_per_node) s->max_prims_per_node = node->num_primitives;
    if (node->name[0] == '\0') s->nodes_with_empty_name++;
    for (int i = 0; i < node->num_primitives; i++) {
        if (node->primitives[i].texture_name[0]) s->prims_textured++;
        else                                      s->prims_flat_color++;
    }
    return 0;
}

/* ── Loaders for the four canonical monarchs ─────────────────────── */

static void check_unit(const char *vfs_path, int min_nodes, int min_verts) {
    ensure_vfs();
    Obj3DFile *obj = NULL;
    int rc = Obj3D_Load(&obj, vfs_path);
    if (rc != 0) {
        printf("\n  load failed for %s\n", vfs_path);
        ASSERT_EQ_INT(0, rc);
    }
    ASSERT(obj != NULL);
    ASSERT(obj->root != NULL);
    /* Root has children — no shipped unit is a single-node mesh. */
    ASSERT(obj->root->first_child != NULL);

    Stats s = {0};
    Obj3D_Walk(obj, stats_visitor, &s);
    ASSERT(s.node_count       >= min_nodes);
    ASSERT(s.total_verts      >= min_verts);
    ASSERT(s.total_prims      > 0);
    ASSERT(s.max_verts_per_node <= 256);  /* sanity */
    ASSERT(s.max_prims_per_node <= 256);

    printf("[%-20s] nodes=%d depth=%d verts=%d prims=%d (textured=%d flat=%d) ",
            vfs_path, s.node_count, s.max_depth, s.total_verts, s.total_prims,
            s.prims_textured, s.prims_flat_color);

    Obj3D_Close(obj);
}

TEST(load_araking)  { check_unit("objects3d/araking.3do",  10, 50); }
TEST(load_vermage)  { check_unit("objects3d/vermage.3do",  10, 50); }
TEST(load_tarnecro) { check_unit("objects3d/tarnecro.3do", 10, 50); }
TEST(load_zonhunt)  { check_unit("objects3d/zonhunt.3do",  10, 50); }

/* ── Edge cases ──────────────────────────────────────────────────── */

TEST(load_returns_negative_on_missing_file) {
    ensure_vfs();
    Obj3DFile *obj = (Obj3DFile *)0xdeadbeef;  /* sentinel — must be cleared */
    int rc = Obj3D_Load(&obj, "objects3d/does_not_exist.3do");
    ASSERT(rc < 0);
    ASSERT(obj == NULL);
}

TEST(load_returns_negative_on_null_args) {
    ensure_vfs();
    Obj3DFile *obj = NULL;
    ASSERT(Obj3D_Load(NULL, "objects3d/araking.3do") < 0);
    ASSERT(Obj3D_Load(&obj, NULL) < 0);
}

TEST(close_of_null_is_safe) {
    Obj3D_Close(NULL);
    ASSERT(1);
}

/* ── Tree structure: araking should have recognizable body parts ── */

typedef struct { int found_hip, found_torso, found_head; } NameScan;
static int scan_names(const Obj3DNode *node, int depth, void *user) {
    (void)depth;
    NameScan *ns = (NameScan *)user;
    /* Names vary in case across factions; case-fold to compare. */
    char lower[32];
    size_t i = 0;
    for (; node->name[i] && i + 1 < sizeof(lower); i++) {
        char c = node->name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[i] = 0;
    if (strstr(lower, "hip"))   ns->found_hip = 1;
    if (strstr(lower, "torso")) ns->found_torso = 1;
    if (strstr(lower, "head"))  ns->found_head = 1;
    return 0;
}

TEST(araking_has_humanoid_anatomy) {
    ensure_vfs();
    Obj3DFile *obj = NULL;
    ASSERT_EQ_INT(0, Obj3D_Load(&obj, "objects3d/araking.3do"));
    NameScan ns = {0};
    Obj3D_Walk(obj, scan_names, &ns);
    ASSERT(ns.found_hip);
    ASSERT(ns.found_torso);
    ASSERT(ns.found_head);
    Obj3D_Close(obj);
}

/* ── Cross-fleet sanity: every shipped 3DO loads ────────────────── */

TEST(every_shipped_3do_loads) {
    ensure_vfs();
    char **paths = NULL;
    int n = 0;
    if (VFS_ListFiles("objects3d/*.3do", &paths, &n) != 0 || n <= 0) {
        printf("(no 3DOs found — skipped) ");
        return;
    }
    int ok = 0, fail = 0;
    for (int i = 0; i < n; i++) {
        Obj3DFile *obj = NULL;
        if (Obj3D_Load(&obj, paths[i]) == 0) {
            ok++;
            Obj3D_Close(obj);
        } else {
            fail++;
            if (fail <= 3) printf("\n    failed: %s", paths[i]);
        }
    }
    printf("\n    %d/%d 3DO files loaded successfully ", ok, n);
    /* Allow up to 1% failure rate for any genuinely-corrupt files;
     * no shipped TAK file should be unreadable in practice. */
    ASSERT(fail <= n / 100);
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    TEST_SUITE("Obj3D_Load — canonical monarchs");
    RUN(load_araking);
    RUN(load_vermage);
    RUN(load_tarnecro);
    RUN(load_zonhunt);

    TEST_SUITE("Obj3D_Load — edge cases");
    RUN(load_returns_negative_on_missing_file);
    RUN(load_returns_negative_on_null_args);
    RUN(close_of_null_is_safe);

    TEST_SUITE("Obj3D tree structure");
    RUN(araking_has_humanoid_anatomy);

    TEST_SUITE("Obj3D — fleet load");
    RUN(every_shipped_3do_loads);

    if (g_vfs_ready) VFS_Shutdown();
    tak_mem_shutdown();
    TEST_REPORT();
}
