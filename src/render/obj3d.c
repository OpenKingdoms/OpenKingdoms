/*
 * obj3d.c — TA/TAK 3DO mesh-file parser.
 *
 * Reads a .3do file from VFS into a heap-allocated tree. The tree
 * owns all its data (string copies, vertex arrays, primitive arrays);
 * the raw file buffer is freed at the end of Obj3D_Load. This makes
 * memory ownership trivial: one Obj3D_Close releases everything.
 *
 * Format spec, conventions, and recon notes are in
 * docs/PHASE_C_3DO.md §2 and §11. The 52/32-byte struct layouts and
 * the "offset_texture == 0 means flat-colour" rule are baked into
 * this file's logic.
 */

#include "tak_obj3d.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdio.h>
#include <string.h>

/* ── On-disk header layouts ──────────────────────────────────────── */

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
} DiskObj3DHeader;

typedef struct {
    uint32_t color_idx;
    uint32_t num_vert_indices;
    uint32_t zero_at_0x08;
    uint32_t offset_vert_list;
    uint32_t offset_texture;
    uint32_t unknown1;
    uint32_t unknown2;
    uint32_t unknown3;
} DiskObj3DPrim;

typedef struct {
    int32_t x, y, z;
} DiskObj3DVertex;

/* ── Bounds-checked file readers ─────────────────────────────────── */

static const uint8_t *bcheck(const uint8_t *buf, uint32_t buf_size,
                              uint32_t off, uint32_t len) {
    if ((uint64_t)off + len > buf_size) return NULL;
    return buf + off;
}

/* Copy a NUL-terminated string from `buf+off` into dst (cap bytes).
 * Truncates if too long, always NUL-terminates. Returns 0 on success,
 * -1 if the string runs past EOF. */
static int copy_string(const uint8_t *buf, uint32_t buf_size,
                        uint32_t off, char *dst, size_t cap) {
    if (off >= buf_size || cap == 0) {
        if (cap > 0) dst[0] = '\0';
        return -1;
    }
    size_t n = 0;
    while (n + 1 < cap && off + n < buf_size && buf[off + n] != 0) {
        dst[n] = (char)buf[off + n];
        n++;
    }
    dst[n] = '\0';
    /* Verify we actually saw a NUL within the file. */
    while (off + n < buf_size && buf[off + n] != 0 && n < cap) n++;
    if (off + n >= buf_size) return -1;
    return 0;
}

/* ── Per-node parser ─────────────────────────────────────────────── */

static void free_primitive(Obj3DPrimitive *p) {
    if (p->vert_indices) tak_free(p->vert_indices);
}

static void free_node(Obj3DNode *n) {
    if (!n) return;
    if (n->first_child)  free_node(n->first_child);
    if (n->next_sibling) free_node(n->next_sibling);
    if (n->vertices)     tak_free(n->vertices);
    if (n->primitives) {
        for (int i = 0; i < n->num_primitives; i++) free_primitive(&n->primitives[i]);
        tak_free(n->primitives);
    }
    tak_free(n);
}

/* Parse one node at `off`, recursively parse children + siblings.
 * Returns the new heap node on success or NULL on any failure. */
static Obj3DNode *parse_node(const uint8_t *buf, uint32_t buf_size,
                              uint32_t off) {
    const DiskObj3DHeader *h =
        (const DiskObj3DHeader *)bcheck(buf, buf_size, off,
                                         sizeof(DiskObj3DHeader));
    if (!h) {
        fprintf(stderr, "Obj3D: header at 0x%x out of bounds\n", off);
        return NULL;
    }

    /* Defensive: catch obviously-bogus counts before allocating. The
     * largest shipped 3DO node is ~50 vertices and ~25 primitives;
     * 10000 of either is far past anything legitimate. */
    if (h->num_vertices > 10000 || h->num_primitives > 10000) {
        fprintf(stderr, "Obj3D: node at 0x%x has implausible counts "
                "(verts=%u prims=%u); aborting parse\n",
                off, h->num_vertices, h->num_primitives);
        return NULL;
    }

    Obj3DNode *n = (Obj3DNode *)tak_malloc(sizeof(Obj3DNode));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));

    n->offset_x = h->x;
    n->offset_y = h->y;
    n->offset_z = h->z;
    n->selection_marker = h->offset_selection;

    /* Name (best-effort; empty if missing). */
    if (h->offset_name) {
        copy_string(buf, buf_size, h->offset_name, n->name, sizeof(n->name));
    }

    /* Vertices: int32 triples → float. Each vertex stays in its
     * source TA-units scale; the renderer multiplies by TA_SCALE
     * during projection. */
    if (h->num_vertices > 0) {
        size_t need = (size_t)h->num_vertices * sizeof(DiskObj3DVertex);
        const DiskObj3DVertex *dv =
            (const DiskObj3DVertex *)bcheck(buf, buf_size, h->offset_vertices, (uint32_t)need);
        if (!dv) {
            fprintf(stderr, "Obj3D: vertex array at 0x%x out of bounds\n",
                    h->offset_vertices);
            free_node(n);
            return NULL;
        }
        n->vertices = (Obj3DVertex *)tak_malloc(h->num_vertices * sizeof(Obj3DVertex));
        if (!n->vertices) { free_node(n); return NULL; }
        n->num_vertices = (int)h->num_vertices;
        for (int i = 0; i < n->num_vertices; i++) {
            n->vertices[i].x = (float)dv[i].x;
            n->vertices[i].y = (float)dv[i].y;
            n->vertices[i].z = (float)dv[i].z;
        }
    }

    /* Primitives. Each one carries a copy of its texture name and
     * its own vertex-index list. */
    if (h->num_primitives > 0) {
        size_t need = (size_t)h->num_primitives * sizeof(DiskObj3DPrim);
        const DiskObj3DPrim *dp =
            (const DiskObj3DPrim *)bcheck(buf, buf_size, h->offset_primitives, (uint32_t)need);
        if (!dp) {
            fprintf(stderr, "Obj3D: prim array at 0x%x out of bounds\n",
                    h->offset_primitives);
            free_node(n);
            return NULL;
        }
        n->primitives = (Obj3DPrimitive *)tak_malloc(h->num_primitives * sizeof(Obj3DPrimitive));
        if (!n->primitives) { free_node(n); return NULL; }
        memset(n->primitives, 0, h->num_primitives * sizeof(Obj3DPrimitive));
        n->num_primitives = (int)h->num_primitives;

        for (int i = 0; i < n->num_primitives; i++) {
            Obj3DPrimitive *po = &n->primitives[i];
            const DiskObj3DPrim *pi = &dp[i];

            po->color_idx = pi->color_idx;
            po->num_vert_indices = (int)pi->num_vert_indices;
            /* Carry through baked-lighting fields. R2-FOLLOWUP found
             * these non-zero in ~80% of shipped prims; tak_unit's
             * mesh-bake interprets each as one vertex's RGBA color
             * for Gouraud-style shading at render time. */
            po->lit_v0 = pi->unknown1;
            po->lit_v1 = pi->unknown2;
            po->lit_v2 = pi->unknown3;

            /* Texture name — empty string when offset_texture == 0
             * (flat-colour mode). */
            if (pi->offset_texture != 0) {
                copy_string(buf, buf_size, pi->offset_texture,
                             po->texture_name, sizeof(po->texture_name));
            } else {
                po->texture_name[0] = '\0';
            }

            /* Vertex indices: uint16 array, num_vert_indices long. */
            if (pi->num_vert_indices == 0) {
                po->vert_indices = NULL;
                continue;
            }
            if (pi->num_vert_indices > 64) {
                /* Defensive — primitives in shipped TAK are typically
                 * 3-4 verts. >64 means we're misreading the field. */
                fprintf(stderr, "Obj3D: prim has %u vert indices (suspicious)\n",
                        pi->num_vert_indices);
                free_node(n);
                return NULL;
            }
            size_t idx_bytes = (size_t)pi->num_vert_indices * sizeof(uint16_t);
            const uint8_t *idx_src = bcheck(buf, buf_size,
                                             pi->offset_vert_list, (uint32_t)idx_bytes);
            if (!idx_src) {
                fprintf(stderr, "Obj3D: prim vert list at 0x%x out of bounds\n",
                        pi->offset_vert_list);
                free_node(n);
                return NULL;
            }
            po->vert_indices = (uint16_t *)tak_malloc(idx_bytes);
            if (!po->vert_indices) { free_node(n); return NULL; }
            memcpy(po->vert_indices, idx_src, idx_bytes);

            /* Validate every index points within this node's vertex
             * array. Out-of-range = corrupt 3DO; bail rather than
             * crash later in the renderer. */
            for (int k = 0; k < po->num_vert_indices; k++) {
                if (po->vert_indices[k] >= n->num_vertices) {
                    fprintf(stderr, "Obj3D: prim vertex index %u >= node verts %d\n",
                            po->vert_indices[k], n->num_vertices);
                    free_node(n);
                    return NULL;
                }
            }
        }
    }

    /* Recurse into child + sibling. Failures bubble up by freeing
     * what we built and returning NULL. */
    if (h->offset_child != 0) {
        n->first_child = parse_node(buf, buf_size, h->offset_child);
        if (!n->first_child) { free_node(n); return NULL; }
    }
    if (h->offset_sibling != 0) {
        n->next_sibling = parse_node(buf, buf_size, h->offset_sibling);
        if (!n->next_sibling) { free_node(n); return NULL; }
    }

    return n;
}

/* ── Public API ──────────────────────────────────────────────────── */

int Obj3D_Load(Obj3DFile **out, const char *vfs_path) {
    if (!out || !vfs_path) return -1;
    *out = NULL;

    void *raw = NULL;
    uint32_t raw_size = 0;
    if (VFS_ReadFile(vfs_path, &raw, &raw_size) != 0) {
        fprintf(stderr, "Obj3D_Load: VFS_ReadFile failed for %s\n", vfs_path);
        return -1;
    }
    if (raw_size < sizeof(DiskObj3DHeader)) {
        fprintf(stderr, "Obj3D_Load: %s too small (%u bytes)\n", vfs_path, raw_size);
        tak_free(raw);
        return -1;
    }

    Obj3DFile *obj = (Obj3DFile *)tak_malloc(sizeof(Obj3DFile));
    if (!obj) {
        tak_free(raw);
        return -1;
    }
    obj->root = parse_node((const uint8_t *)raw, raw_size, 0);

    /* Raw bytes are no longer needed — strings + arrays got copied
     * out into the tree. */
    tak_free(raw);

    if (!obj->root) {
        tak_free(obj);
        return -1;
    }
    *out = obj;
    return 0;
}

void Obj3D_Close(Obj3DFile *obj) {
    if (!obj) return;
    if (obj->root) free_node(obj->root);
    tak_free(obj);
}

/* Internal recursion helper: returns 1 if visitor aborted. */
static int walk_recursive(const Obj3DNode *n, int depth,
                           Obj3DVisitor v, void *user) {
    if (!n) return 0;
    if (v(n, depth, user) != 0) return 1;
    if (walk_recursive(n->first_child, depth + 1, v, user)) return 1;
    if (walk_recursive(n->next_sibling, depth, v, user)) return 1;
    return 0;
}

int Obj3D_Walk(const Obj3DFile *obj, Obj3DVisitor v, void *user) {
    if (!obj || !v) return -1;
    return walk_recursive(obj->root, 0, v, user);
}
