/*
 * model_store.c -- the 3DO source of the model store, see
 * tak_model_store.h.
 *
 * A baked UnitMesh keeps its vertices in node local space with a node
 * index per vertex, which is what a per piece transform on the GPU
 * needs. This turns one into a MODEL layout buffer with a flat normal
 * per triangle and splits each texture batch into runs whose node
 * range fits one draw's uniform budget.
 */

#include "tak_model_store.h"
#include "tak_gltf.h"
#include "tak_model_gltf.h"
#include "tak_gpu.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MODEL_STORE_CAP 512

static GpuModel *g_models[MODEL_STORE_CAP];
static int       g_model_count;

int ModelStore_Count(void) { return g_model_count; }

static void free_model(GpuModel *m) {
    if (!m) return;
    for (int i = 0; i < m->own_tex_count; i++)
        if (m->own_tex[i]) GL3D_FreeTexture(m->own_tex[i]);
    if (m->gl) GL3D_FreeMesh(m->gl);
    /* A mesh built here is freed here; a baked one goes back to
     * the bake that made it. */
    if (m->mesh) {
        if (m->from_gltf) Gltf_FreeUnitMesh(m->mesh);
        else Units_FreeBakedMesh(m->mesh);
    }
    tak_free(m);
}

void ModelStore_Clear(void) {
    for (int i = 0; i < g_model_count; i++) free_model(g_models[i]);
    g_model_count = 0;
}

/* Split one batch into node ranges of at most `budget` nodes, which
 * is what one draw can hold uniforms for. Vertices come in node
 * order, so a range is contiguous. A triangle reaching outside the
 * range ends the run: the shader addresses rows from node_lo, and a
 * corner past node_hi would read off the end of them.
 *
 * `gl_tex` is the texture for a model that owns its own, NULL for
 * one drawing from the shared atlas. */
static int add_batches(GpuModel *m, const UnitMesh *src, int b, int budget,
                       GL3D_Texture *gl_tex) {
    const UnitMeshBatch *sb = &src->batches[b];
    SDL_Texture *sdl_tex = gl_tex ? NULL : GPU_TextureSDL(sb->atlas_tex);
    int first = sb->first_index;
    int end = sb->first_index + sb->index_count;
    while (first < end) {
        int lo = src->vert_node_idx[src->indices[first]];
        int hi = lo;
        int i = first;
        for (; i + 2 < end; i += 3) {
            int a = src->vert_node_idx[src->indices[i]];
            int b2 = src->vert_node_idx[src->indices[i + 1]];
            int c = src->vert_node_idx[src->indices[i + 2]];
            int n_lo = a < b2 ? (a < c ? a : c) : (b2 < c ? b2 : c);
            int n_hi = a > b2 ? (a > c ? a : c) : (b2 > c ? b2 : c);
            int want_lo = n_lo < lo ? n_lo : lo;
            int want_hi = n_hi > hi ? n_hi : hi;
            if (want_hi - want_lo >= budget) break;
            lo = want_lo;
            hi = want_hi;
        }
        /* A batch whose first triangle alone will not fit is one no
         * draw can take. Ending the run here would not advance. */
        if (i == first) return -1;
        if (m->batch_count >= MODEL_STORE_MAX_BATCHES) return -1;
        GL3D_ModelBatch *out = &m->batches[m->batch_count++];
        out->sdl_tex = sdl_tex;
        out->tex = gl_tex;
        out->first_index = first;
        out->index_count = i - first;
        out->node_lo = lo;
        out->node_hi = hi;
        first = i;
    }
    return 0;
}

static GpuModel *finish(GpuModel *m, UnitMesh *src, GL3D_Texture **tex_per_batch);

static GpuModel *build_3do(const char *name, int color_idx) {
    UnitMesh *src = Units_BakeObjectMesh(name, color_idx);
    if (!src || src->vert_count <= 0 || src->tri_count <= 0) {
        if (src) Units_FreeBakedMesh(src);
        return NULL;
    }
    GpuModel *m = (GpuModel *)tak_malloc(sizeof(GpuModel));
    if (!m) { Units_FreeBakedMesh(src); return NULL; }
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->color_idx = color_idx;
    m->mesh = src;
    return finish(m, src, NULL);
}

/* Turns a baked mesh into GPU buffers. `tex_per_batch`, when given,
 * is this model's own texture for each of the mesh's batches. */
static GpuModel *finish(GpuModel *m, UnitMesh *src, GL3D_Texture **tex_per_batch) {
    const char *name = m->name;
    const int V = src->vert_count;
    const int floats = GL3D_LayoutFloats(GL3D_LAYOUT_MODEL);
    float *verts = (float *)tak_malloc(sizeof(float) * (size_t)floats * (size_t)V);
    float *normals = (float *)tak_malloc(sizeof(float) * 3 * (size_t)V);
    if (verts) memset(verts, 0, sizeof(float) * (size_t)floats * (size_t)V);
    if (!verts || !normals) {
        if (verts) tak_free(verts);
        if (normals) tak_free(normals);
        free_model(m);
        return NULL;
    }
    memset(normals, 0, sizeof(float) * 3 * (size_t)V);

    /* Flat normals: every triangle's face normal summed onto its
     * vertices, which a fan of one primitive shares. */
    for (int t = 0; t < src->tri_count; t++) {
        int i0 = src->indices[3 * t], i1 = src->indices[3 * t + 1], i2 = src->indices[3 * t + 2];
        const float *p0 = &src->positions[3 * i0];
        const float *p1 = &src->positions[3 * i1];
        const float *p2 = &src->positions[3 * i2];
        float ex = p1[0] - p0[0], ey = p1[1] - p0[1], ez = p1[2] - p0[2];
        float fx = p2[0] - p0[0], fy = p2[1] - p0[1], fz = p2[2] - p0[2];
        float nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len <= 0.0f) continue;
        nx /= len; ny /= len; nz /= len;
        int idx[3] = { i0, i1, i2 };
        for (int k = 0; k < 3; k++) {
            normals[3 * idx[k] + 0] += nx;
            normals[3 * idx[k] + 1] += ny;
            normals[3 * idx[k] + 2] += nz;
        }
    }

    /* Which vertices belong to a textured batch: those keep the texture
     * and get a white tint, the rest carry their palette colour. */
    for (int b = 0; b < src->batch_count; b++) {
        const UnitMeshBatch *sb = &src->batches[b];
        int textured = sb->atlas_tex != NULL;
        for (int i = sb->first_index; i < sb->first_index + sb->index_count; i++) {
            int v = src->indices[i];
            float *o = verts + (size_t)v * floats;
            o[0] = src->positions[3 * v + 0];
            o[1] = src->positions[3 * v + 1];
            o[2] = src->positions[3 * v + 2];
            float nx = normals[3 * v], ny = normals[3 * v + 1], nz = normals[3 * v + 2];
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (len > 0.0f) { nx /= len; ny /= len; nz /= len; } else { ny = 1.0f; }
            o[3] = nx; o[4] = ny; o[5] = nz;
            o[6] = src->uvs[2 * v + 0];
            o[7] = src->uvs[2 * v + 1];
            uint32_t c = src->colors[v];
            if (textured) {
                o[8] = o[9] = o[10] = 1.0f;
                o[11] = (float)((c >> 24) & 0xFF) / 255.0f;
            } else {
                o[8]  = (float)(c & 0xFF) / 255.0f;
                o[9]  = (float)((c >> 8) & 0xFF) / 255.0f;
                o[10] = (float)((c >> 16) & 0xFF) / 255.0f;
                o[11] = (float)((c >> 24) & 0xFF) / 255.0f;
            }
            o[12] = (float)src->vert_node_idx[v];
        }
    }
    tak_free(normals);

    m->gl = GL3D_UploadMesh(GL3D_LAYOUT_MODEL, verts, V, src->indices, src->tri_count * 3);
    tak_free(verts);
    if (!m->gl) { free_model(m); return NULL; }

    int budget = GL3D_MaxNodesPerDraw();
    for (int b = 0; b < src->batch_count; b++) {
        if (add_batches(m, src, b, budget,
                        tex_per_batch ? tex_per_batch[b] : NULL) != 0) {
            fprintf(stderr, "ModelStore: %s has too many draw runs\n", name);
            break;
        }
    }

    memcpy(m->aabb_min, src->aabb_min, sizeof(m->aabb_min));
    memcpy(m->aabb_max, src->aabb_max, sizeof(m->aabb_max));
    float ta = Units_GetTAScale();
    float r = 0.0f;
    for (int k = 0; k < 3; k++) {
        float a = fabsf(src->aabb_min[k]), bb = fabsf(src->aabb_max[k]);
        if (a > r) r = a;
        if (bb > r) r = bb;
    }
    m->radius_px = r * ta * 1.75f;
    m->height_px = src->aabb_max[1] * ta;
    float fr = 0.0f;
    const int axes[2] = { 0, 2 };
    for (int k = 0; k < 2; k++) {
        float a = fabsf(src->aabb_min[axes[k]]), bb = fabsf(src->aabb_max[axes[k]]);
        if (a > fr) fr = a;
        if (bb > fr) fr = bb;
    }
    m->foot_radius_px = fr * ta;
    return m;
}

/* The artist's model for an object name, or NULL when there is none
 * and the shipped one should stand in. */
static GpuModel *build_gltf(const char *name, int color_idx) {
    if (!name || !name[0]) return NULL;
    char lower[TAK_UNITDEF_OBJ_MAX];
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < sizeof(lower); p++, n++) {
        /* An object name is a bare name. Anything that could steer a
         * path out of the folder ends it here. */
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '.') return NULL;
        lower[n] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    }
    lower[n] = '\0';

    char path[TAK_UNITDEF_OBJ_MAX + 24];
    snprintf(path, sizeof(path), "models3d/%s.glb", lower);
    GltfModel *g = NULL;
    if (Gltf_Load(&g, path) != 0 || !g) return NULL;

    int image_of_batch[UNIT_MESH_MAX_BATCHES];
    for (int i = 0; i < UNIT_MESH_MAX_BATCHES; i++) image_of_batch[i] = -1;
    UnitMesh *src = Gltf_ToUnitMesh(g, name, Units_GetTeamColorRGBA(color_idx),
                                    image_of_batch);
    if (!src) { Gltf_Free(g); return NULL; }

    GpuModel *m = (GpuModel *)tak_malloc(sizeof(GpuModel));
    if (!m) { Gltf_FreeUnitMesh(src); Gltf_Free(g); return NULL; }
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->color_idx = color_idx;
    m->mesh = src;
    m->from_gltf = 1;

    /* A picture for each batch that draws one. The model holds them,
     * because the run splitter hands one to several batches and it
     * must be let go once. */
    GL3D_Texture *tex[UNIT_MESH_MAX_BATCHES];
    memset(tex, 0, sizeof(tex));
    int failed = 0;
    for (int b = 0; b < src->batch_count && !failed; b++) {
        int img = image_of_batch[b];
        if (img < 0 || img >= g->image_count) continue;
        const GltfImage *pic = &g->images[img];
        GL3D_Texture *t = GL3D_UploadTextureRGBA(pic->rgba, pic->w, pic->h, 1, 1);
        if (!t) { failed = 1; break; }
        tex[b] = t;
        m->own_tex[m->own_tex_count++] = t;
    }
    Gltf_Free(g);
    if (failed) {
        fprintf(stderr, "ModelStore: %s refused, a picture would not go to the card\n", name);
        free_model(m);
        return NULL;
    }
    GpuModel *done = finish(m, src, tex);
    if (done) {
        fprintf(stderr, "ModelStore: %s from %s, %d piece(s), %d verts\n",
                name, path, src->node_count, src->vert_count);
    }
    return done;
}

const GpuModel *ModelStore_Get(const char *object_name, int color_idx) {
    if (!object_name || !object_name[0] || !GL3D_Available()) return NULL;
    if (color_idx < 0 || color_idx > 11) color_idx = 0;
    for (int i = 0; i < g_model_count; i++) {
        if (g_models[i]->color_idx == color_idx &&
            tak_stricmp(g_models[i]->name, object_name) == 0)
            return g_models[i];
    }
    if (g_model_count >= MODEL_STORE_CAP) return NULL;
    /* An artist's model wins when there is one, and the shipped
     * model stands in whenever there is not or it will not do. */
    GpuModel *m = build_gltf(object_name, color_idx);
    if (!m) m = build_3do(object_name, color_idx);
    if (!m) return NULL;
    g_models[g_model_count++] = m;
    return m;
}
