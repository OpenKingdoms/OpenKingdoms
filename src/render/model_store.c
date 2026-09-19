/*
 * model_store.c -- the model store, see tak_model_store.h.
 *
 * A baked UnitMesh keeps its vertices in node local space with a node
 * index per vertex, which is what a per piece transform on the GPU
 * needs. This turns one into a MODEL layout buffer with a flat normal
 * per triangle and splits each texture batch into runs whose node
 * range fits one draw's uniform budget.
 *
 * An artist's model goes through the same steps in the MODEL_PBR
 * layout, with the normals and tangents it brought. Its pictures do
 * not depend on the team colour, so they are decoded and sent to the
 * card once and shared by every colour of the model.
 */

#include "tak_model_store.h"
#include "tak_gltf.h"
#include "tak_hpi.h"
#include "tak_model_gltf.h"
#include "tak_gpu.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MODEL_STORE_CAP 512
#define SHARED_TEX_CAP  512

static GpuModel *g_models[MODEL_STORE_CAP];
static int       g_model_count;

/* One picture of one artist's model, on the card, whatever colour asked. */
typedef struct SharedTex {
    char          name[TAK_UNITDEF_OBJ_MAX];  /* the model's lowercased name */
    int           image;                      /* its index in the file */
    GL3D_Texture *tex;
} SharedTex;

static SharedTex g_shared[SHARED_TEX_CAP];
static int       g_shared_count;

/* Names asked for through ModelStore_GetArtists that had no model, so
 * the folder is not asked again every frame for a feature it will never
 * have. Cleared with the models. */
#define MODEL_STORE_MISS_CAP 512
static char g_missed[MODEL_STORE_MISS_CAP][TAK_UNITDEF_OBJ_MAX];
static int  g_missed_count;

int ModelStore_Count(void) { return g_model_count; }

static void free_model(GpuModel *m) {
    if (!m) return;
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
    for (int i = 0; i < g_shared_count; i++)
        if (g_shared[i].tex) GL3D_FreeTexture(g_shared[i].tex);
    g_shared_count = 0;
    g_missed_count = 0;
}

static GL3D_Texture *shared_find(const char *lower, int image) {
    for (int i = 0; i < g_shared_count; i++)
        if (g_shared[i].image == image && strcmp(g_shared[i].name, lower) == 0)
            return g_shared[i].tex;
    return NULL;
}

static int shared_add(const char *lower, int image, GL3D_Texture *tex) {
    if (g_shared_count >= SHARED_TEX_CAP) return -1;
    SharedTex *s = &g_shared[g_shared_count++];
    snprintf(s->name, sizeof(s->name), "%s", lower);
    s->image = image;
    s->tex = tex;
    return 0;
}

/* Whether a colour of this artist's model has been built before, in
 * which case its pictures are already on the card. */
static int model_seen(const char *lower) {
    for (int i = 0; i < g_model_count; i++)
        if (g_models[i]->from_gltf && tak_stricmp(g_models[i]->name, lower) == 0)
            return 1;
    return 0;
}

/* Split one batch into node ranges of at most `budget` nodes, which
 * is what one draw can hold uniforms for. Vertices come in node
 * order, so a range is contiguous. A triangle reaching outside the
 * range ends the run: the shader addresses rows from node_lo, and a
 * corner past node_hi would read off the end of them.
 *
 * `proto` carries what every run of this batch draws with. */
static int add_batches(GpuModel *m, const UnitMesh *src, int b, int budget,
                       const GL3D_ModelBatch *proto) {
    const UnitMeshBatch *sb = &src->batches[b];
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
        *out = *proto;
        out->first_index = first;
        out->index_count = i - first;
        out->node_lo = lo;
        out->node_hi = hi;
        first = i;
    }
    return 0;
}

/* Turns a mesh into GPU buffers. `protos`, when given, is what each of
 * the mesh's batches draws with, and the mesh goes up in the PBR
 * layout with the normals and tangents it brought. */
static GpuModel *finish(GpuModel *m, UnitMesh *src, const GL3D_ModelBatch *protos) {
    const char *name = m->name;
    const int V = src->vert_count;
    const GL3D_Layout layout = protos ? GL3D_LAYOUT_MODEL_PBR : GL3D_LAYOUT_MODEL;
    const int floats = GL3D_LayoutFloats(layout);
    float *verts = (float *)tak_malloc(sizeof(float) * (size_t)floats * (size_t)V);
    float *normals = src->normals ? NULL : (float *)tak_malloc(sizeof(float) * 3 * (size_t)V);
    if (verts) memset(verts, 0, sizeof(float) * (size_t)floats * (size_t)V);
    if (!verts || (!src->normals && !normals)) {
        if (verts) tak_free(verts);
        if (normals) tak_free(normals);
        free_model(m);
        return NULL;
    }

    /* Flat normals for a mesh that brought none: every triangle's face
     * normal summed onto its vertices, which a fan of one primitive
     * shares. */
    if (normals) {
        memset(normals, 0, sizeof(float) * 3 * (size_t)V);
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
    }
    const float *nsrc = src->normals ? src->normals : normals;

    /* Which vertices belong to a textured batch: those keep the texture
     * and get a white tint, the rest carry their palette colour. */
    for (int b = 0; b < src->batch_count; b++) {
        const UnitMeshBatch *sb = &src->batches[b];
        int textured = protos ? (protos[b].tex != NULL) : (sb->atlas_tex != NULL);
        for (int i = sb->first_index; i < sb->first_index + sb->index_count; i++) {
            int v = src->indices[i];
            float *o = verts + (size_t)v * floats;
            o[0] = src->positions[3 * v + 0];
            o[1] = src->positions[3 * v + 1];
            o[2] = src->positions[3 * v + 2];
            float nx = nsrc[3 * v], ny = nsrc[3 * v + 1], nz = nsrc[3 * v + 2];
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
            if (floats > 13) {
                if (src->tangents) {
                    o[13] = src->tangents[4 * v + 0];
                    o[14] = src->tangents[4 * v + 1];
                    o[15] = src->tangents[4 * v + 2];
                    o[16] = src->tangents[4 * v + 3];
                } else {
                    o[13] = o[14] = o[15] = o[16] = 0.0f;
                }
            }
        }
    }
    if (normals) tak_free(normals);

    m->gl = GL3D_UploadMesh(layout, verts, V, src->indices, src->tri_count * 3);
    tak_free(verts);
    if (!m->gl) { free_model(m); return NULL; }

    int budget = GL3D_MaxNodesPerDraw();
    for (int b = 0; b < src->batch_count; b++) {
        GL3D_ModelBatch proto;
        memset(&proto, 0, sizeof(proto));
        if (protos) proto = protos[b];
        else proto.sdl_tex = GPU_TextureSDL(src->batches[b].atlas_tex);
        if (add_batches(m, src, b, budget, &proto) != 0) {
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

/* The texture for one picture of the model: the one already on the
 * card, or the picture sent up now and kept for the next colour. NULL
 * when there is no such picture. */
static GL3D_Texture *picture(const char *lower, const GltfModel *g, int img) {
    if (img < 0) return NULL;
    GL3D_Texture *t = shared_find(lower, img);
    if (t) return t;
    if (img >= g->image_count) return NULL;
    const GltfImage *pic = &g->images[img];
    if (!pic->rgba || pic->w <= 0 || pic->h <= 0) return NULL;
    t = GL3D_UploadTextureRGBA(pic->rgba, pic->w, pic->h, 1, 1);
    if (!t) return NULL;
    if (shared_add(lower, img, t) != 0) {
        GL3D_FreeTexture(t);
        return NULL;
    }
    return t;
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

    /* The second colour of a model finds its pictures on the card
     * already and reads geometry alone. */
    const int with_images = model_seen(lower) ? GLTF_GEOMETRY_ONLY : GLTF_WITH_IMAGES;

    char path[TAK_UNITDEF_OBJ_MAX + 24];
    snprintf(path, sizeof(path), "models3d/%s.glb", lower);
    GltfModel *g = NULL;
    if (Gltf_LoadEx(&g, path, with_images) != 0 || !g) {
        /* Nothing in the archives or the data folder. A release binary
         * is built without a data folder at all, so the place a player
         * would actually put a model is the game folder itself. */
        void *bytes = NULL;
        uint32_t size = 0;
        if (VFS_ReadGameFile(path, &bytes, &size) != 0 || !bytes) return NULL;
        int rc = Gltf_LoadFromMemoryEx(&g, (const uint8_t *)bytes, (size_t)size, with_images);
        tak_free(bytes);
        if (rc != 0 || !g) return NULL;
    }

    GltfBatch bi[UNIT_MESH_MAX_BATCHES];
    memset(bi, 0, sizeof(bi));
    UnitMesh *src = Gltf_ToUnitMesh(g, name, Units_GetTeamColorRGBA(color_idx), bi);
    if (!src) { Gltf_Free(g); return NULL; }

    GpuModel *m = (GpuModel *)tak_malloc(sizeof(GpuModel));
    if (!m) { Gltf_FreeUnitMesh(src); Gltf_Free(g); return NULL; }
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->color_idx = color_idx;
    m->mesh = src;
    m->from_gltf = 1;

    GL3D_ModelBatch protos[UNIT_MESH_MAX_BATCHES];
    memset(protos, 0, sizeof(protos));
    for (int b = 0; b < src->batch_count; b++) {
        GL3D_ModelBatch *pr = &protos[b];
        pr->tex          = picture(lower, g, bi[b].base_image);
        pr->normal_tex   = src->tangents ? picture(lower, g, bi[b].normal_image) : NULL;
        pr->mr_tex       = picture(lower, g, bi[b].mr_image);
        pr->emissive_tex = picture(lower, g, bi[b].emissive_image);
        memcpy(pr->emissive, bi[b].emissive, sizeof(pr->emissive));
        pr->metallic     = bi[b].metallic;
        pr->roughness    = bi[b].roughness;
        pr->normal_scale = bi[b].normal_scale;
        pr->alpha_cutoff = bi[b].alpha_cutoff;
        pr->blend        = bi[b].blend;
        pr->double_sided = bi[b].double_sided;
    }
    Gltf_Free(g);

    GpuModel *done = finish(m, src, protos);
    if (done) {
        fprintf(stderr, "ModelStore: %s from %s, %d piece(s), %d verts%s\n",
                name, path, src->node_count, src->vert_count,
                with_images ? "" : ", pictures shared");
    }
    return done;
}

const GpuModel *ModelStore_GetArtists(const char *name) {
    if (!name || !name[0] || !GL3D_Available()) return NULL;
    for (int i = 0; i < g_model_count; i++) {
        if (g_models[i]->from_gltf && g_models[i]->color_idx == 0 &&
            tak_stricmp(g_models[i]->name, name) == 0)
            return g_models[i];
    }
    for (int i = 0; i < g_missed_count; i++)
        if (tak_stricmp(g_missed[i], name) == 0) return NULL;
    if (g_model_count >= MODEL_STORE_CAP) return NULL;
    GpuModel *m = build_gltf(name, 0);
    if (!m) {
        if (g_missed_count < MODEL_STORE_MISS_CAP)
            snprintf(g_missed[g_missed_count++], TAK_UNITDEF_OBJ_MAX, "%s", name);
        return NULL;
    }
    g_models[g_model_count++] = m;
    return m;
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
