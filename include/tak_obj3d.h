#ifndef TAK_OBJ3D_H
#define TAK_OBJ3D_H

#include "tak_types.h"

/* ── 3DO mesh loader ──────────────────────────────────────────────────
 *
 * Phase C M2: parse a `.3do` file (TA/TAK textured-mesh format) into a
 * heap-allocated tree we can walk. No rendering — just data. The
 * mesh-baking step (M6) consumes this tree and flattens it into the
 * triangle lists `UnitMesh` holds.
 *
 * Format references and recon notes live in docs/PHASE_C_3DO.md §2-§3.
 * Short version (verified against shipped TAK monarchs in R2):
 *
 *   Object3D header, 52 bytes:
 *     uint32 version, num_vertices, num_primitives, offset_selection;
 *     int32  x, y, z;                  // offset from parent in TA units
 *     uint32 offset_name, unused;
 *     uint32 offset_vertices, offset_primitives, offset_sibling, offset_child;
 *
 *   Primitive, 32 bytes:
 *     uint32 color_idx, num_vert_indices, zero_at_0x08;
 *     uint32 offset_vert_list, offset_texture;
 *     uint32 unknown1, unknown2, unknown3;        // probably baked lighting
 *
 * Vertex coords on disk are int32 x,y,z in TA units (1/65536 per
 * meter-ish). We convert to float at parse time so downstream code
 * doesn't have to. */

typedef struct Obj3DPrimitive {
    /* Texture name to look up via TexAtlas. Empty string means
     * "flat-colour mode" — use color_idx against the faction palette. */
    char       texture_name[32];

    /* Palette index if FLAT_COLOR. Garbage if textured (the original
     * 3DO format leaves this field populated even for textured
     * primitives). */
    uint32_t   color_idx;

    /* Vertex indices into the containing node's vertex array. 3 = tri,
     * 4 = quad, occasionally more. The renderer fan-triangulates at
     * mesh-bake time. */
    int        num_vert_indices;
    uint16_t  *vert_indices;

    /* Baked Gouraud lighting per vertex. The original 3DO format has
     * three "unknown" uint32 fields per primitive that R2 found are
     * non-zero in 70-80% of prims. Best fit (Spring RTS reference +
     * empirical inspection): each is a 4-byte color in RGBA layout
     * for one of the prim's first three vertices. The renderer
     * interpolates these across triangle interiors via SDL's per-
     * vertex color stream, giving the smooth Gouraud-style shading
     * the original engine displayed.
     *
     * For triangles: lit[0..2] map directly.
     * For quads:     v3 reuses lit[2] (closest gradient continuation).
     * For 0-value fields: fall back to white at bake time. */
    uint32_t   lit_v0;
    uint32_t   lit_v1;
    uint32_t   lit_v2;
} Obj3DPrimitive;

typedef struct Obj3DVertex {
    float x, y, z;       /* TA units, converted from int32 at parse */
} Obj3DVertex;

typedef struct Obj3DNode {
    char           name[32];

    /* Offset from parent (in TA units). Root is always (0, 0, 0). */
    int32_t        offset_x, offset_y, offset_z;

    int            num_vertices;
    Obj3DVertex   *vertices;

    int            num_primitives;
    Obj3DPrimitive *primitives;

    /* Selection-mesh marker (decomp finding, the legacy reference ~197794):
     * The 3DO node header at +0x0C is a flag for "the first primitive
     * of this node is the selection mesh — skip it during normal
     * rendering." If this is 0xFFFFFFFF (-1), there is no selection
     * mesh and all primitives render. Otherwise, prim[0] is the
     * selection mesh and the renderer starts at prim[1].
     *
     * Previously we only skipped the root node's primitives (the
     * "ground polygon" pattern). The per-node selection mesh is the
     * actual mechanism — it can appear on ANY node, not just root. */
    uint32_t       selection_marker;

    /* Tree links. NULL on leaf / last sibling respectively. The
     * renderer walks `first_child` recursively, then `next_sibling`
     * iteratively. */
    struct Obj3DNode *first_child;
    struct Obj3DNode *next_sibling;
} Obj3DNode;

typedef struct Obj3DFile {
    Obj3DNode *root;
} Obj3DFile;

/* Parse `objects3d/<vfs_path>` into a heap-allocated tree. On success
 * *out is set to a non-NULL Obj3DFile that the caller releases via
 * Obj3D_Close. Returns 0 on success, -1 on any failure (file missing,
 * malformed bytes, allocation failure). */
int  Obj3D_Load(Obj3DFile **out, const char *vfs_path);

/* Free the tree and all its descendants. NULL-safe. */
void Obj3D_Close(Obj3DFile *obj);

/* Walk every node in the tree (depth-first, parent before children).
 * Visitor returns 0 to continue, non-zero to abort the walk early.
 * The visitor sees `depth=0` for the root. Useful for tests, debug
 * dumps, and the eventual mesh-bake. */
typedef int (*Obj3DVisitor)(const Obj3DNode *node, int depth, void *user);
int  Obj3D_Walk(const Obj3DFile *obj, Obj3DVisitor v, void *user);

#endif /* TAK_OBJ3D_H */
