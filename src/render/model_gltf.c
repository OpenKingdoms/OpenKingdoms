/*
 * model_gltf.c -- an artist's glTF turned into a unit mesh.
 *
 * The mesh shapes downstream take on trust a great deal that the 3DO
 * bake gives them by construction: pieces in an order where a parent
 * comes first, one piece to a triangle, batches that cover every
 * triangle in piece order, indices inside the vertices. Those files
 * are ours. A model from a folder is not, so this builds to those
 * shapes and then checks that it did before anyone indexes the result.
 */
#include "tak_model_gltf.h"
#include "tak_memory.h"

#include <stdio.h>
#include <string.h>

void Gltf_FreeUnitMesh(UnitMesh *m) {
    if (!m) return;
    if (m->positions)     tak_free(m->positions);
    if (m->uvs)           tak_free(m->uvs);
    if (m->colors)        tak_free(m->colors);
    if (m->indices)       tak_free(m->indices);
    if (m->vert_node_idx) tak_free(m->vert_node_idx);
    if (m->tri_seq)       tak_free(m->tri_seq);
    tak_free(m);
}

int Gltf_ValidateMesh(const UnitMesh *m, const char *name) {
#define BAD(what) do { \
        fprintf(stderr, "Model %s refused, %s\n", name ? name : "?", what); \
        return -1; } while (0)
    if (!m) BAD("nothing was built");
    if (m->node_count < 1 || m->node_count > UNIT_MESH_MAX_NODES) BAD("its piece count");
    if (m->vert_count < 1 || m->vert_count > 65535) BAD("its vertex count");
    if (m->tri_count < 1) BAD("its triangle count");
    if (!m->positions || !m->uvs || !m->colors || !m->indices || !m->vert_node_idx)
        BAD("an array it should have");
    if (m->batch_count < 1 || m->batch_count > UNIT_MESH_MAX_BATCHES) BAD("its batch count");
    if (m->nodes[0].parent != -1) BAD("a root with a parent");
    for (int i = 0; i < m->node_count; i++)
        if (m->nodes[i].parent < -1 || m->nodes[i].parent >= i)
            BAD("a piece whose parent does not come before it");
    for (int v = 0; v < m->vert_count; v++)
        if (m->vert_node_idx[v] >= (uint16_t)m->node_count) BAD("a vertex on no piece");

    const int total = m->tri_count * 3;
    for (int i = 0; i < total; i++)
        if (m->indices[i] >= (uint16_t)m->vert_count) BAD("an index past its vertices");
    /* One triangle sits on one piece. A draw covers a range of pieces
     * and the shader addresses rows from its low end, so a corner on
     * another piece would read past them. */
    for (int t = 0; t < m->tri_count; t++) {
        uint16_t a = m->vert_node_idx[m->indices[3 * t]];
        if (m->vert_node_idx[m->indices[3 * t + 1]] != a ||
            m->vert_node_idx[m->indices[3 * t + 2]] != a)
            BAD("a triangle across two pieces");
    }

    int covered = 0;
    for (int b = 0; b < m->batch_count; b++) {
        const UnitMeshBatch *sb = &m->batches[b];
        if (sb->first_index < 0 || sb->index_count <= 0) BAD("an empty batch");
        if (sb->first_index % 3 || sb->index_count % 3) BAD("a batch off a triangle");
        if (sb->first_index > total - sb->index_count) BAD("a batch past its indices");
        uint16_t last = m->vert_node_idx[m->indices[sb->first_index]];
        for (int i = sb->first_index; i < sb->first_index + sb->index_count; i += 3) {
            uint16_t n = m->vert_node_idx[m->indices[i]];
            if (n < last) BAD("a batch out of piece order");
            last = n;
        }
        covered += sb->index_count;
    }
    if (covered != total) BAD("batches that do not cover its triangles");
#undef BAD
    return 0;
}

static uint32_t pack_rgba(const float c[4]) {
    uint32_t out = 0;
    for (int k = 0; k < 4; k++) {
        float v = c[k] < 0.0f ? 0.0f : (c[k] > 1.0f ? 1.0f : c[k]);
        out |= (uint32_t)(v * 255.0f + 0.5f) << (k * 8);
    }
    return out;
}

/* glTF is right handed and the frame here is not, so one axis turns
 * around and every triangle winds the other way to match. Both happen
 * together or the model draws inside out. */
UnitMesh *Gltf_ToUnitMesh(const GltfModel *g, const char *name,
                          uint32_t team_rgba, int *image_of_batch) {
    if (!g || !image_of_batch) return NULL;
    if (g->node_count < 1 || g->node_count > UNIT_MESH_MAX_NODES) return NULL;
    if (g->prim_count < 1) return NULL;

    /* A batch for each picture the model uses, and one for the parts
     * that use none. */
    int batch_count = 0;
    for (int img = -1; img < g->image_count; img++) {
        int used = 0;
        for (int p = 0; p < g->prim_count && !used; p++)
            if (g->prims[p].image == img) used = 1;
        if (!used) continue;
        if (batch_count >= UNIT_MESH_MAX_BATCHES) {
            fprintf(stderr, "Model %s refused, more pictures than one model holds\n",
                    name ? name : "?");
            return NULL;
        }
        image_of_batch[batch_count++] = img;
    }
    if (batch_count < 1) return NULL;

    long long verts = 0, tris = 0;
    for (int p = 0; p < g->prim_count; p++) {
        verts += g->prims[p].vert_count;
        tris += g->prims[p].tri_count;
    }
    if (verts < 1 || verts > 65535 || tris < 1) {
        fprintf(stderr, "Model %s refused, %lld vertices is past what one model holds\n",
                name ? name : "?", verts);
        return NULL;
    }

    UnitMesh *m = (UnitMesh *)tak_malloc(sizeof(UnitMesh));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->vert_count = (int)verts;
    m->tri_count = (int)tris;
    m->positions     = (float *)tak_malloc(sizeof(float) * 3 * (size_t)verts);
    m->uvs           = (float *)tak_malloc(sizeof(float) * 2 * (size_t)verts);
    m->colors        = (uint32_t *)tak_malloc(sizeof(uint32_t) * (size_t)verts);
    m->vert_node_idx = (uint16_t *)tak_malloc(sizeof(uint16_t) * (size_t)verts);
    m->indices       = (uint16_t *)tak_malloc(sizeof(uint16_t) * 3 * (size_t)tris);
    if (!m->positions || !m->uvs || !m->colors || !m->vert_node_idx || !m->indices) {
        Gltf_FreeUnitMesh(m);
        return NULL;
    }

    const float scale = TA_UNITS_PER_PIXEL * g->scale_hint;

    /* The pieces, and where each one sits once its parents are followed. */
    float world[UNIT_MESH_MAX_NODES][3];
    m->node_count = g->node_count;
    for (int i = 0; i < g->node_count; i++) {
        UnitMeshNode *n = &m->nodes[i];
        snprintf(n->name, sizeof(n->name), "%s", g->nodes[i].name);
        n->parent = (int16_t)g->nodes[i].parent;
        if (n->parent < -1 || n->parent >= i) { Gltf_FreeUnitMesh(m); return NULL; }
        n->offset[0] =  g->nodes[i].offset[0] * scale;
        n->offset[1] =  g->nodes[i].offset[1] * scale;
        n->offset[2] = -g->nodes[i].offset[2] * scale;
        for (int k = 0; k < 3; k++)
            world[i][k] = n->offset[k] + (n->parent < 0 ? 0.0f : world[n->parent][k]);
    }

    for (int k = 0; k < 3; k++) {
        m->aabb_min[k] = 1e30f;
        m->aabb_max[k] = -1e30f;
    }

    int vw = 0, iw = 0;
    for (int b = 0; b < batch_count; b++) {
        m->batches[b].atlas_tex = NULL;
        m->batches[b].first_index = iw;
        /* Piece order inside a batch, which is what lets one draw cover
         * a contiguous range of them. */
        for (int node = 0; node < g->node_count; node++) {
            for (int p = 0; p < g->prim_count; p++) {
                const GltfPrim *pr = &g->prims[p];
                if (pr->image != image_of_batch[b] || pr->node != node) continue;
                uint32_t colour = pr->team_color ? team_rgba : pack_rgba(pr->base_color);
                int base = vw;
                for (int v = 0; v < pr->vert_count; v++, vw++) {
                    m->positions[3 * vw + 0] =  pr->pos[3 * v + 0] * scale;
                    m->positions[3 * vw + 1] =  pr->pos[3 * v + 1] * scale;
                    m->positions[3 * vw + 2] = -pr->pos[3 * v + 2] * scale;
                    m->uvs[2 * vw + 0] = pr->uv[2 * v + 0];
                    m->uvs[2 * vw + 1] = pr->uv[2 * v + 1];
                    m->colors[vw] = colour;
                    m->vert_node_idx[vw] = (uint16_t)node;
                    /* The bounds are in the folded space the cull and
                     * the selection ring read, not the piece's own. */
                    for (int k = 0; k < 3; k++) {
                        float w = m->positions[3 * vw + k] + world[node][k];
                        if (w < m->aabb_min[k]) m->aabb_min[k] = w;
                        if (w > m->aabb_max[k]) m->aabb_max[k] = w;
                    }
                }
                for (int t = 0; t < pr->tri_count; t++) {
                    m->indices[iw++] = (uint16_t)(base + pr->idx[3 * t + 0]);
                    m->indices[iw++] = (uint16_t)(base + pr->idx[3 * t + 2]);
                    m->indices[iw++] = (uint16_t)(base + pr->idx[3 * t + 1]);
                }
            }
        }
        m->batches[b].index_count = iw - m->batches[b].first_index;
        if (m->batches[b].index_count <= 0) { Gltf_FreeUnitMesh(m); return NULL; }
        m->batch_count++;
    }

    if (Gltf_ValidateMesh(m, name) != 0) { Gltf_FreeUnitMesh(m); return NULL; }
    return m;
}
