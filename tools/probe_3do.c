/*
 * probe_3do.c — Phase C R2 recon. Read objects3d/<name>.3do via VFS,
 * parse the 40-byte Object3D header at offset 0, dump the tree.
 *
 * Spec reference (Spring RTS TaLoader.cpp, also community wiki):
 *
 *   struct Object3D {
 *     uint32 version;            // 1 for TA / TAK
 *     uint32 num_vertices;
 *     uint32 num_primitives;
 *     uint32 offset_selection;   // pointer to selection geometry
 *     int32  x, y, z;            // offset from parent (in TA units, 1/65536)
 *     uint32 offset_name;        // string pointer
 *     uint32 unused;
 *     uint32 offset_vertices;    // int32 x,y,z triples
 *     uint32 offset_primitives;
 *     uint32 offset_sibling;     // 0 = last
 *     uint32 offset_child;       // 0 = leaf
 *   };
 *
 *   struct Primitive {
 *     uint32 color_idx;
 *     uint32 num_vert_indices;
 *     uint16 reserved;
 *     uint16 is_colored;         // !=0 means use color_idx, no texture
 *     uint32 offset_vert_list;   // uint16 indices into the node's vertex array
 *     uint32 offset_texture;     // string pointer (empty for flat color)
 *     uint32 unused1, unused2;
 *   };
 *
 * If the dump produces sane vertex/primitive counts and a recognizable
 * tree of body parts, the spec matches TAK 1:1. If not, the deltas
 * surface here and we adjust the parser.
 *
 * Usage: probe_3do <objectname>     (default: araking)
 */

#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

typedef struct {
    uint32_t version;
    uint32_t num_vertices;
    uint32_t num_primitives;
    uint32_t offset_selection;
    int32_t  x, y, z;
    uint32_t offset_name;
    uint32_t unused;
    uint32_t offset_vertices;
    uint32_t offset_primitives;
    uint32_t offset_sibling;
    uint32_t offset_child;
} Obj3DHeader;

/* 32 bytes total. Confirmed against Spring RTS's TA 3DO loader and
 * the empirical layout of araking — earlier draft had this at 28
 * bytes which read garbage from the second primitive onwards. The
 * "is textured?" check is just `offset_texture != 0` (no separate
 * boolean flag in the struct). */
typedef struct {
    uint32_t color_idx;
    uint32_t num_vert_indices;
    uint32_t always_zero;
    uint32_t offset_vert_list;
    uint32_t offset_texture;
    uint32_t unused1;
    uint32_t unused2;
    uint32_t unused3;
} Obj3DPrim;

static const uint8_t *g_buf = NULL;
static uint32_t       g_size = 0;

/* Bounds-checked read. Returns NULL if offset/length out of range. */
static const uint8_t *at(uint32_t off, uint32_t len) {
    if ((uint64_t)off + len > g_size) return NULL;
    return g_buf + off;
}

/* Read a NUL-terminated string at offset (bounded by file size). */
static const char *str_at(uint32_t off) {
    if (off >= g_size) return "<oob>";
    const char *s = (const char *)g_buf + off;
    /* Walk to make sure we have a NUL within the file — defensive. */
    for (uint32_t i = off; i < g_size; i++) {
        if (g_buf[i] == 0) return s;
    }
    return "<unterminated>";
}

static void indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

/* Recursively walk one node + its siblings + children.
 * Caller is responsible for not invoking on a null link — we DO walk
 * a node at offset 0 (the root). */
static void walk(uint32_t off, int depth, int *out_node_count) {
    const Obj3DHeader *h = (const Obj3DHeader *)at(off, sizeof(Obj3DHeader));
    if (!h) {
        indent(depth);
        printf("(node header at 0x%x out of bounds)\n", off);
        return;
    }
    (*out_node_count)++;

    const char *name = str_at(h->offset_name);

    indent(depth);
    printf("'%s' v=%u p=%u offset=(%d,%d,%d) ver=%u",
           name, h->num_vertices, h->num_primitives,
           h->x, h->y, h->z, h->version);
    if (h->offset_selection) printf(" sel=0x%x", h->offset_selection);
    printf("\n");

    /* Quick sanity: are vertex and primitive counts reasonable? */
    if (h->num_vertices > 100000 || h->num_primitives > 10000) {
        indent(depth);
        printf("  WARN: implausible counts — header layout likely wrong\n");
    }

    /* Dump first few primitives. */
    if (h->num_primitives > 0) {
        const Obj3DPrim *prims = (const Obj3DPrim *)at(h->offset_primitives,
            h->num_primitives * sizeof(Obj3DPrim));
        if (!prims) {
            indent(depth);
            printf("  primitives at 0x%x out of bounds\n", h->offset_primitives);
        } else {
            uint32_t show = h->num_primitives < 3 ? h->num_primitives : 3;
            for (uint32_t i = 0; i < show; i++) {
                const Obj3DPrim *p = &prims[i];
                indent(depth);
                int textured = (p->offset_texture != 0);
                printf("  prim[%u] color=%u verts=%u %s tex='%s'\n",
                       i, p->color_idx, p->num_vert_indices,
                       textured ? "textured" : "FLAT_COLOR",
                       textured ? str_at(p->offset_texture) : "(none)");
            }
            if (h->num_primitives > show) {
                indent(depth);
                printf("  ... %u more primitives\n", h->num_primitives - show);
            }
        }
    }

    /* Recurse into children, then siblings. Skip null links. */
    if (h->offset_child   != 0) walk(h->offset_child,   depth + 1, out_node_count);
    if (h->offset_sibling != 0) walk(h->offset_sibling, depth,     out_node_count);
}

int main(int argc, char **argv) {
    const char *unit = argc > 1 ? argv[1] : "araking";
    char vfs_path[128];
    snprintf(vfs_path, sizeof(vfs_path), "objects3d/%s.3do", unit);

    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    void *raw = NULL;
    uint32_t sz = 0;
    if (VFS_ReadFile(vfs_path, &raw, &sz) != 0) {
        fprintf(stderr, "VFS_ReadFile failed for %s\n", vfs_path);
        return 1;
    }
    g_buf  = (const uint8_t *)raw;
    g_size = sz;

    printf("=== %s (%u bytes) ===\n", vfs_path, sz);
    printf("Hex preview (first 64 bytes):\n  ");
    for (uint32_t i = 0; i < 64 && i < sz; i++) {
        printf("%02x ", g_buf[i]);
        if ((i & 0xf) == 0xf) printf("\n  ");
    }
    printf("\n\nTree:\n");

    int nodes = 0;
    walk(0, 0, &nodes);
    printf("\n%d nodes total.\n", nodes);

    tak_free(raw);
    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
