/*
 * model_gltf.c -- an artist's glTF turned into a unit mesh.
 *
 * The mesh shapes downstream take on trust a great deal that the 3DO
 * bake gives them by construction: pieces in an order where a parent
 * comes first, one piece to a triangle, batches that cover every
 * triangle in piece order, indices inside the vertices. Those files
 * are ours. A model from a folder is not, so this builds to those
 * shapes and then checks that it did before anyone indexes the result.
 *
 * A batch is one material's worth of triangles. Materials that blend
 * come after the solid ones so the solid parts are on the card when the
 * glass is drawn over them.
 */
#include "tak_model_gltf.h"
#include "tak_memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void Gltf_FreeUnitMesh(UnitMesh *m) {
    if (!m) return;
    if (m->positions)     tak_free(m->positions);
    if (m->normals)       tak_free(m->normals);
    if (m->tangents)      tak_free(m->tangents);
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

/* Two materials draw as one batch when everything the shader is told
 * agrees. The base colour and the team colour ride on the vertices,
 * so they do not come into it. */
static int same_surface(const GltfSurface *a, const GltfSurface *b) {
    return a->image == b->image && a->normal_image == b->normal_image &&
           a->mr_image == b->mr_image && a->emissive_image == b->emissive_image &&
           a->blend == b->blend && a->double_sided == b->double_sided &&
           a->metallic == b->metallic && a->roughness == b->roughness &&
           a->normal_scale == b->normal_scale && a->alpha_cutoff == b->alpha_cutoff &&
           memcmp(a->emissive, b->emissive, sizeof(a->emissive)) == 0;
}

static void batch_of_surface(GltfBatch *out, const GltfSurface *s) {
    out->base_image     = s->image;
    out->normal_image   = s->normal_image;
    out->mr_image       = s->mr_image;
    out->emissive_image = s->emissive_image;
    out->metallic       = s->metallic;
    out->roughness      = s->roughness;
    out->normal_scale   = s->normal_scale;
    out->alpha_cutoff   = s->alpha_cutoff;
    out->blend          = s->blend;
    out->double_sided   = s->double_sided;
    memcpy(out->emissive, s->emissive, sizeof(out->emissive));
}

static void normalize3(float *v) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-12f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

/* Face normals summed onto the vertices of the range, for a file that
 * brought none of its own. */
static void normals_for_range(UnitMesh *m, int v0, int v1, int i0, int i1) {
    memset(m->normals + 3 * v0, 0, sizeof(float) * 3 * (size_t)(v1 - v0));
    for (int i = i0; i + 2 < i1; i += 3) {
        int a = m->indices[i], b = m->indices[i + 1], c = m->indices[i + 2];
        const float *pa = &m->positions[3 * a], *pb = &m->positions[3 * b], *pc = &m->positions[3 * c];
        float ex = pb[0] - pa[0], ey = pb[1] - pa[1], ez = pb[2] - pa[2];
        float fx = pc[0] - pa[0], fy = pc[1] - pa[1], fz = pc[2] - pa[2];
        float n[3] = { ey * fz - ez * fy, ez * fx - ex * fz, ex * fy - ey * fx };
        normalize3(n);
        int idx[3] = { a, b, c };
        for (int k = 0; k < 3; k++) {
            m->normals[3 * idx[k] + 0] += n[0];
            m->normals[3 * idx[k] + 1] += n[1];
            m->normals[3 * idx[k] + 2] += n[2];
        }
    }
    for (int v = v0; v < v1; v++) {
        float *n = &m->normals[3 * v];
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12f) { n[0] /= len; n[1] /= len; n[2] /= len; }
        else { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
    }
}

/* Tangents from the way the picture lies across each triangle, for a
 * file that has a normal map but brought no tangents. The fourth
 * number says which way the picture's up runs: glTF puts the top of
 * an image at v = 0, so it runs against the direction v grows. */
static int tangents_for_range(UnitMesh *m, int v0, int v1, int i0, int i1) {
    const int n = v1 - v0;
    float *acc_t = (float *)tak_malloc(sizeof(float) * 3 * (size_t)n);
    float *acc_b = (float *)tak_malloc(sizeof(float) * 3 * (size_t)n);
    if (!acc_t || !acc_b) {
        if (acc_t) tak_free(acc_t);
        if (acc_b) tak_free(acc_b);
        return -1;
    }
    memset(acc_t, 0, sizeof(float) * 3 * (size_t)n);
    memset(acc_b, 0, sizeof(float) * 3 * (size_t)n);
    for (int i = i0; i + 2 < i1; i += 3) {
        int a = m->indices[i], b = m->indices[i + 1], c = m->indices[i + 2];
        const float *pa = &m->positions[3 * a], *pb = &m->positions[3 * b], *pc = &m->positions[3 * c];
        const float *ua = &m->uvs[2 * a], *ub = &m->uvs[2 * b], *uc = &m->uvs[2 * c];
        float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
        float e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
        float du1 = ub[0] - ua[0], dv1 = ub[1] - ua[1];
        float du2 = uc[0] - ua[0], dv2 = uc[1] - ua[1];
        float det = du1 * dv2 - du2 * dv1;
        if (fabsf(det) < 1e-12f) continue;
        float r = 1.0f / det;
        int idx[3] = { a, b, c };
        for (int k = 0; k < 3; k++) {
            float *t = &acc_t[3 * (idx[k] - v0)], *bt = &acc_b[3 * (idx[k] - v0)];
            for (int j = 0; j < 3; j++) {
                t[j]  += (e1[j] * dv2 - e2[j] * dv1) * r;
                bt[j] += (e2[j] * du1 - e1[j] * du2) * r;
            }
        }
    }
    for (int v = v0; v < v1; v++) {
        const float *nn = &m->normals[3 * v];
        float *t = &acc_t[3 * (v - v0)];
        const float *bg = &acc_b[3 * (v - v0)];
        /* Make the tangent lie on the surface. */
        float d = nn[0] * t[0] + nn[1] * t[1] + nn[2] * t[2];
        t[0] -= nn[0] * d; t[1] -= nn[1] * d; t[2] -= nn[2] * d;
        float len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (len < 1e-12f) {
            /* A vertex the picture does not stretch across. Any
             * direction on the surface will do. */
            float up[3] = { fabsf(nn[1]) < 0.9f ? 0.0f : 1.0f, fabsf(nn[1]) < 0.9f ? 1.0f : 0.0f, 0.0f };
            t[0] = up[1] * nn[2] - up[2] * nn[1];
            t[1] = up[2] * nn[0] - up[0] * nn[2];
            t[2] = up[0] * nn[1] - up[1] * nn[0];
            normalize3(t);
        } else {
            t[0] /= len; t[1] /= len; t[2] /= len;
        }
        float cx = nn[1] * t[2] - nn[2] * t[1];
        float cy = nn[2] * t[0] - nn[0] * t[2];
        float cz = nn[0] * t[1] - nn[1] * t[0];
        float side = cx * bg[0] + cy * bg[1] + cz * bg[2];
        m->tangents[4 * v + 0] = t[0];
        m->tangents[4 * v + 1] = t[1];
        m->tangents[4 * v + 2] = t[2];
        m->tangents[4 * v + 3] = side < 0.0f ? 1.0f : -1.0f;
    }
    tak_free(acc_t);
    tak_free(acc_b);
    return 0;
}

/* glTF is right handed and the frame here is not, so one axis turns
 * around and every triangle winds the other way to match. Both happen
 * together or the model draws inside out. Normals and tangents turn
 * the same axis around, and a tangent's handedness turns with it. */
UnitMesh *Gltf_ToUnitMesh(const GltfModel *g, const char *name,
                          uint32_t team_rgba, GltfBatch *batches) {
    if (!g || !batches) return NULL;
    if (g->node_count < 1 || g->node_count > UNIT_MESH_MAX_NODES) return NULL;
    if (g->prim_count < 1) return NULL;

    /* One batch for each distinct surface, the solid ones first. */
    GltfSurface surf[UNIT_MESH_MAX_BATCHES];
    int surf_of_prim[GLTF_MAX_PRIMS];
    int surf_count = 0;
    int want_tangents = 0;
    for (int p = 0; p < g->prim_count; p++) {
        const GltfSurface *s = &g->prims[p].surface;
        if (s->normal_image >= 0 || g->prims[p].tan) want_tangents = 1;
        int found = -1;
        for (int k = 0; k < surf_count && found < 0; k++)
            if (same_surface(&surf[k], s)) found = k;
        if (found < 0) {
            if (surf_count >= UNIT_MESH_MAX_BATCHES) {
                fprintf(stderr, "Model %s refused, more materials than one model holds\n",
                        name ? name : "?");
                return NULL;
            }
            surf[surf_count] = *s;
            found = surf_count++;
        }
        surf_of_prim[p] = found;
    }
    int order[UNIT_MESH_MAX_BATCHES];
    int batch_count = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < surf_count; k++)
            if ((surf[k].blend ? 1 : 0) == pass) order[batch_count++] = k;

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
    m->normals       = (float *)tak_malloc(sizeof(float) * 3 * (size_t)verts);
    m->uvs           = (float *)tak_malloc(sizeof(float) * 2 * (size_t)verts);
    m->colors        = (uint32_t *)tak_malloc(sizeof(uint32_t) * (size_t)verts);
    m->vert_node_idx = (uint16_t *)tak_malloc(sizeof(uint16_t) * (size_t)verts);
    m->indices       = (uint16_t *)tak_malloc(sizeof(uint16_t) * 3 * (size_t)tris);
    if (want_tangents) m->tangents = (float *)tak_malloc(sizeof(float) * 4 * (size_t)verts);
    if (!m->positions || !m->normals || !m->uvs || !m->colors || !m->vert_node_idx ||
        !m->indices || (want_tangents && !m->tangents)) {
        Gltf_FreeUnitMesh(m);
        return NULL;
    }
    if (m->tangents) memset(m->tangents, 0, sizeof(float) * 4 * (size_t)verts);

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
        const int which = order[b];
        batch_of_surface(&batches[b], &surf[which]);
        m->batches[b].atlas_tex = NULL;
        m->batches[b].first_index = iw;
        /* Piece order inside a batch, which is what lets one draw cover
         * a contiguous range of them. */
        for (int node = 0; node < g->node_count; node++) {
            for (int p = 0; p < g->prim_count; p++) {
                const GltfPrim *pr = &g->prims[p];
                if (surf_of_prim[p] != which || pr->node != node) continue;
                const GltfSurface *s = &pr->surface;
                uint32_t colour = s->team_color ? team_rgba : pack_rgba(s->base_color);
                const int base = vw, ibase = iw;
                for (int v = 0; v < pr->vert_count; v++, vw++) {
                    m->positions[3 * vw + 0] =  pr->pos[3 * v + 0] * scale;
                    m->positions[3 * vw + 1] =  pr->pos[3 * v + 1] * scale;
                    m->positions[3 * vw + 2] = -pr->pos[3 * v + 2] * scale;
                    if (pr->nrm) {
                        m->normals[3 * vw + 0] =  pr->nrm[3 * v + 0];
                        m->normals[3 * vw + 1] =  pr->nrm[3 * v + 1];
                        m->normals[3 * vw + 2] = -pr->nrm[3 * v + 2];
                    }
                    if (m->tangents && pr->tan) {
                        m->tangents[4 * vw + 0] =  pr->tan[4 * v + 0];
                        m->tangents[4 * vw + 1] =  pr->tan[4 * v + 1];
                        m->tangents[4 * vw + 2] = -pr->tan[4 * v + 2];
                        m->tangents[4 * vw + 3] = -pr->tan[4 * v + 3];
                    }
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
                if (!pr->nrm) normals_for_range(m, base, vw, ibase, iw);
                /* A tangent the file brought is made to lie on the
                 * surface, so one that leans along the normal cannot
                 * leave the shader a zero bitangent to normalise. */
                if (m->tangents && pr->tan) {
                    for (int v = base; v < vw; v++) {
                        const float *nn = &m->normals[3 * v];
                        float *t = &m->tangents[4 * v];
                        float d = nn[0] * t[0] + nn[1] * t[1] + nn[2] * t[2];
                        t[0] -= nn[0] * d; t[1] -= nn[1] * d; t[2] -= nn[2] * d;
                        float len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                        if (len > 1e-6f) { t[0] /= len; t[1] /= len; t[2] /= len; }
                        else { t[0] = t[1] = t[2] = 0.0f; t[3] = 0.0f; }
                    }
                }
                if (m->tangents && !pr->tan && s->normal_image >= 0) {
                    if (tangents_for_range(m, base, vw, ibase, iw) != 0) {
                        Gltf_FreeUnitMesh(m);
                        return NULL;
                    }
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
