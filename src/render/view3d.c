/*
 * view3d.c -- the 3D view, see tak_view3d.h.
 *
 * Terrain is a mesh at the heightmap's resolution, chunked the way the
 * classic renderer chunks its textures so every block samples the same
 * 32 pixel square of the same chunk image. Units and model features
 * come from the model store and are posed by the same piece state the
 * classic view poses them by. Sprite features stand up as billboards.
 * Water is a translucent plane at the map's water line. The camera is
 * tak_camera3d.h and every GL call is behind tak_gl3d.h.
 */

#include "tak_view3d.h"
#include "tak_gl3d.h"
#include "tak_model_store.h"
#include "tak_camera3d.h"
#include "tak_world.h"
#include "tak_terrain.h"
#include "tak_unit.h"
#include "tak_features.h"
#include "tak_fog.h"
#include "tak_hpi.h"
#include "tak_jpg.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V3_PI 3.14159265358979323846f

/* Block and tile geometry, the same numbers terrain.c draws by. */
#define V3_BLOCK_PX     32
#define V3_TILE_PX      16
#define V3_SUB_CHUNK_PX 32

/* Blocks per terrain mesh: nine vertices each under a 16 bit index. */
#define V3_BLOCKS_PER_MESH 7000
#define V3_REGION_BLOCKS   16
#define V3_MAX_MESHES      64

/* A sprite's pixel height stands up this much taller than it is wide,
 * since the classic view drew it foreshortened by the tilt. */
#define V3_SPRITE_RISE 1.6f

#define V3_FOG_REFRESH_FRAMES 6
#define V3_FPS_WINDOW 300

typedef struct TerrainSeg {
    int   mesh;
    int   first_index;
    int   index_count;
    int   chunk;
    float centre[3];
    float radius;
} TerrainSeg;

typedef struct SpriteTex {
    const FeatureDef *fd;
    GL3D_Texture *tex;
    int w, h, off_x, off_y;
} SpriteTex;

typedef struct BlockRef {
    int chunk, region, bx, by;
} BlockRef;

static struct {
    int ready;
    int inited_gl;
    TAK_Platform *plat;
    Camera3D cam;
    int32_t synced_cam_x, synced_cam_y;
    SDL_Rect viewport;
    float light[3];

    /* Terrain, built for one world's grid. */
    const GameWorld *built_for;
    const TerrainGrid *built_grid;
    GL3D_Mesh *meshes[V3_MAX_MESHES];
    int mesh_count;
    TerrainSeg *segs;
    int seg_count;
    GL3D_Texture **chunk_tex;
    int chunk_count;
    float map_w, map_h;

    /* Fog lookup. */
    GL3D_Texture *fog_tex;
    uint8_t *fog_bytes;
    int fog_w, fog_h;
    int fog_age;

    SpriteTex sprites[256];
    int sprite_count;

    /* Streamed geometry for water, billboards and rings. */
    float *stream_v;
    uint16_t *stream_i;
    int stream_vcap, stream_icap;

    /* Middle drag state and timing. */
    int last_mx, last_my, dragging;
    double render_ms_sum;
    int render_frames;
    double render_ms_last_avg;
    int render_frames_last;
    double window_t0;
    double fps_last;
    UnitNodeXform xforms[UNIT_MESH_MAX_NODES];
    float node_rows[UNIT_MESH_MAX_NODES * 12];
    uint8_t node_hidden[UNIT_MESH_MAX_NODES];
} v;

static View3DDrawCounts s_counts;

/* ── Helpers ──────────────────────────────────────────────────────── */

static float ground_height(void *ctx, float x, float z) {
    const GameWorld *w = (const GameWorld *)ctx;
    return (float)Terrain_SampleHeight(w, (int32_t)x, (int32_t)z);
}

static int ensure_stream(int verts, int indices) {
    if (verts > v.stream_vcap) {
        int cap = v.stream_vcap ? v.stream_vcap : 1024;
        while (cap < verts) cap *= 2;
        float *nv = (float *)tak_malloc(sizeof(float) * 9 * (size_t)cap);
        if (!nv) return -1;
        if (v.stream_v) tak_free(v.stream_v);
        v.stream_v = nv;
        v.stream_vcap = cap;
    }
    if (indices > v.stream_icap) {
        int cap = v.stream_icap ? v.stream_icap : 2048;
        while (cap < indices) cap *= 2;
        uint16_t *ni = (uint16_t *)tak_malloc(sizeof(uint16_t) * (size_t)cap);
        if (!ni) return -1;
        if (v.stream_i) tak_free(v.stream_i);
        v.stream_i = ni;
        v.stream_icap = cap;
    }
    return 0;
}

static void put_vert(float *o, float x, float y, float z, float u, float t,
                     float r, float g, float b, float a) {
    o[0] = x; o[1] = y; o[2] = z; o[3] = u; o[4] = t;
    o[5] = r; o[6] = g; o[7] = b; o[8] = a;
}

/* Sync the classic camera to the free camera's target, so the minimap
 * and the return to the classic view both land on the same spot. */
static void write_classic_cam(GameWorld *world) {
    if (!world) return;
    int32_t cx = (int32_t)v.cam.target_x - world->viewport_w / 2;
    int32_t cy = (int32_t)v.cam.target_z - world->viewport_h / 2;
    int32_t max_x = world->map_pixels_w - world->viewport_w;
    int32_t max_y = world->map_pixels_h - world->viewport_h;
    if (cx < 0) cx = 0; else if (cx > max_x) cx = max_x;
    if (cy < 0) cy = 0; else if (cy > max_y) cy = max_y;
    world->cam_x = cx;
    world->cam_y = cy;
    v.synced_cam_x = cx;
    v.synced_cam_y = cy;
}

static void retarget_from_classic(const GameWorld *world) {
    v.cam.target_x = (float)(world->cam_x + world->viewport_w / 2);
    v.cam.target_z = (float)(world->cam_y + world->viewport_h / 2);
    v.synced_cam_x = world->cam_x;
    v.synced_cam_y = world->cam_y;
    fprintf(stderr, "View3D: camera over (%.0f, %.0f)\n", v.cam.target_x, v.cam.target_z);
}

/* ── Terrain build ────────────────────────────────────────────────── */

static void free_terrain(void) {
    for (int i = 0; i < v.mesh_count; i++) GL3D_FreeMesh(v.meshes[i]);
    v.mesh_count = 0;
    if (v.segs) { tak_free(v.segs); v.segs = NULL; }
    v.seg_count = 0;
    if (v.chunk_tex) {
        for (int i = 0; i < v.chunk_count; i++) GL3D_FreeTexture(v.chunk_tex[i]);
        tak_free(v.chunk_tex);
        v.chunk_tex = NULL;
    }
    v.chunk_count = 0;
    if (v.fog_tex) { GL3D_FreeTexture(v.fog_tex); v.fog_tex = NULL; }
    if (v.fog_bytes) { tak_free(v.fog_bytes); v.fog_bytes = NULL; }
    v.fog_w = v.fog_h = 0;
    for (int i = 0; i < v.sprite_count; i++) GL3D_FreeTexture(v.sprites[i].tex);
    v.sprite_count = 0;
    v.built_for = NULL;
    v.built_grid = NULL;
}

static int block_cmp(const void *a, const void *b) {
    const BlockRef *x = (const BlockRef *)a, *y = (const BlockRef *)b;
    if (x->region != y->region) return x->region - y->region;
    if (x->chunk != y->chunk) return x->chunk - y->chunk;
    if (x->by != y->by) return x->by - y->by;
    return x->bx - y->bx;
}

/* The chunk images decoded again into textures of our own, with
 * mipmaps, so the classic renderer's textures are never touched. */
static void load_chunk_textures(const TerrainGrid *g) {
    v.chunk_count = g->chunk_count;
    v.chunk_tex = (GL3D_Texture **)tak_malloc(sizeof(GL3D_Texture *) * (size_t)g->chunk_count);
    if (!v.chunk_tex) { v.chunk_count = 0; return; }
    memset(v.chunk_tex, 0, sizeof(GL3D_Texture *) * (size_t)g->chunk_count);
    for (int i = 0; i < g->chunk_count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "terrain/%08x.jpg", g->chunks[i].chunk_id);
        void *bytes = NULL;
        uint32_t size = 0;
        if (VFS_ReadFile(path, &bytes, &size) != 0) continue;
        uint32_t *pixels = NULL;
        int w = 0, h = 0;
        int rc = JPG_DecodeRGBA((const uint8_t *)bytes, (size_t)size, &pixels, &w, &h);
        tak_free(bytes);
        if (rc != 0 || !pixels) continue;
        v.chunk_tex[i] = GL3D_UploadTextureRGBA(pixels, w, h, 1, 1);
        tak_free(pixels);
    }
}

static float height_at_tile(const TNTFile *t, int tx, int tz) {
    if (tx < 0) tx = 0;
    if (tz < 0) tz = 0;
    if (tx > t->height_w - 1) tx = t->height_w - 1;
    if (tz > t->height_h - 1) tz = t->height_h - 1;
    return (float)t->heightmap[tz * t->height_w + tx];
}

static int build_terrain(const GameWorld *world) {
    const TerrainGrid *g = world->grid;
    const TNTFile *t = &world->tnt;
    if (!g || !g->blocks || !g->chunks || !t->heightmap) return -1;
    v.map_w = (float)world->map_pixels_w;
    v.map_h = (float)world->map_pixels_h;
    load_chunk_textures(g);

    const int bw = g->blocks_w, bh = g->blocks_h;
    const int n = bw * bh;
    BlockRef *refs = (BlockRef *)tak_malloc(sizeof(BlockRef) * (size_t)n);
    if (!refs) return -1;
    const int regions_w = (bw + V3_REGION_BLOCKS - 1) / V3_REGION_BLOCKS;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            BlockRef *r = &refs[by * bw + bx];
            r->chunk = g->blocks[by * bw + bx].chunk_idx;
            r->region = (by / V3_REGION_BLOCKS) * regions_w + bx / V3_REGION_BLOCKS;
            r->bx = bx;
            r->by = by;
        }
    }
    qsort(refs, (size_t)n, sizeof(BlockRef), block_cmp);

    /* Worst case one segment per block, trimmed after. */
    v.segs = (TerrainSeg *)tak_malloc(sizeof(TerrainSeg) * (size_t)n);
    float *verts = (float *)tak_malloc(sizeof(float) * 8 * 9 * (size_t)V3_BLOCKS_PER_MESH);
    uint16_t *idx = (uint16_t *)tak_malloc(sizeof(uint16_t) * 24 * (size_t)V3_BLOCKS_PER_MESH);
    if (!v.segs || !verts || !idx) {
        if (verts) tak_free(verts);
        if (idx) tak_free(idx);
        tak_free(refs);
        return -1;
    }

    int mesh_blocks = 0;
    int i = 0;
    while (i < n) {
        /* One run of blocks sharing a region and a chunk. */
        int j = i;
        while (j < n && refs[j].region == refs[i].region && refs[j].chunk == refs[i].chunk) j++;
        int k = i;
        while (k < j) {
            if (mesh_blocks >= V3_BLOCKS_PER_MESH || v.mesh_count >= V3_MAX_MESHES) {
                if (v.mesh_count >= V3_MAX_MESHES) break;
                v.meshes[v.mesh_count++] = GL3D_UploadMesh(GL3D_LAYOUT_TERRAIN, verts,
                                                           mesh_blocks * 9, idx, mesh_blocks * 24);
                mesh_blocks = 0;
            }
            int room = V3_BLOCKS_PER_MESH - mesh_blocks;
            int take = j - k;
            if (take > room) take = room;
            TerrainSeg *seg = &v.segs[v.seg_count++];
            seg->mesh = v.mesh_count;
            seg->first_index = mesh_blocks * 24;
            seg->index_count = take * 24;
            seg->chunk = refs[k].chunk;
            float minx = 1e30f, minz = 1e30f, miny = 1e30f;
            float maxx = -1e30f, maxz = -1e30f, maxy = -1e30f;
            const TerrainChunkEntry *ce = &g->chunks[refs[k].chunk];
            float tex_w = ce->tex_w > 0 ? (float)ce->tex_w : 512.0f;
            float tex_h = ce->tex_h > 0 ? (float)ce->tex_h : 512.0f;
            for (int q = 0; q < take; q++) {
                const BlockRef *r = &refs[k + q];
                const TerrainBlock *b = &g->blocks[r->by * bw + r->bx];
                /* Half a texel in from the sub square's edge, so the
                 * filter never reads the neighbouring square. */
                float u0 = ((float)b->tex_x * V3_SUB_CHUNK_PX + 0.5f) / tex_w;
                float u1 = ((float)b->tex_x * V3_SUB_CHUNK_PX + V3_SUB_CHUNK_PX - 0.5f) / tex_w;
                float t0 = ((float)b->tex_y * V3_SUB_CHUNK_PX + 0.5f) / tex_h;
                float t1 = ((float)b->tex_y * V3_SUB_CHUNK_PX + V3_SUB_CHUNK_PX - 0.5f) / tex_h;
                int base = (mesh_blocks + q) * 9;
                for (int gz = 0; gz < 3; gz++) {
                    for (int gx = 0; gx < 3; gx++) {
                        int tx = r->bx * 2 + gx, tz = r->by * 2 + gz;
                        float x = (float)(tx * V3_TILE_PX);
                        float z = (float)(tz * V3_TILE_PX);
                        float y = height_at_tile(t, tx, tz);
                        float dx = (height_at_tile(t, tx + 1, tz) - height_at_tile(t, tx - 1, tz)) / (2.0f * V3_TILE_PX);
                        float dz = (height_at_tile(t, tx, tz + 1) - height_at_tile(t, tx, tz - 1)) / (2.0f * V3_TILE_PX);
                        float nx = -dx, ny = 1.0f, nz = -dz;
                        float len = sqrtf(nx * nx + ny * ny + nz * nz);
                        nx /= len; ny /= len; nz /= len;
                        float *o = verts + (size_t)(base + gz * 3 + gx) * 8;
                        o[0] = x; o[1] = y; o[2] = z;
                        o[3] = nx; o[4] = ny; o[5] = nz;
                        o[6] = u0 + (u1 - u0) * (float)gx * 0.5f;
                        o[7] = t0 + (t1 - t0) * (float)gz * 0.5f;
                        if (x < minx) minx = x; if (x > maxx) maxx = x;
                        if (y < miny) miny = y; if (y > maxy) maxy = y;
                        if (z < minz) minz = z; if (z > maxz) maxz = z;
                    }
                }
                uint16_t *o = idx + (size_t)(mesh_blocks + q) * 24;
                int w = 0;
                for (int gz = 0; gz < 2; gz++) {
                    for (int gx = 0; gx < 2; gx++) {
                        uint16_t a = (uint16_t)(base + gz * 3 + gx);
                        uint16_t bb = (uint16_t)(a + 1);
                        uint16_t c = (uint16_t)(a + 3);
                        uint16_t d = (uint16_t)(a + 4);
                        o[w++] = a; o[w++] = c; o[w++] = bb;
                        o[w++] = bb; o[w++] = c; o[w++] = d;
                    }
                }
            }
            seg->centre[0] = (minx + maxx) * 0.5f;
            seg->centre[1] = (miny + maxy) * 0.5f;
            seg->centre[2] = (minz + maxz) * 0.5f;
            float ex = (maxx - minx) * 0.5f, ey = (maxy - miny) * 0.5f, ez = (maxz - minz) * 0.5f;
            seg->radius = sqrtf(ex * ex + ey * ey + ez * ez);
            mesh_blocks += take;
            k += take;
        }
        i = j;
    }
    if (mesh_blocks > 0 && v.mesh_count < V3_MAX_MESHES) {
        v.meshes[v.mesh_count++] = GL3D_UploadMesh(GL3D_LAYOUT_TERRAIN, verts,
                                                   mesh_blocks * 9, idx, mesh_blocks * 24);
    }
    tak_free(verts);
    tak_free(idx);
    tak_free(refs);

    if (world->fog_w > 0 && world->fog_h > 0) {
        v.fog_w = world->fog_w;
        v.fog_h = world->fog_h;
        v.fog_bytes = (uint8_t *)tak_malloc((size_t)v.fog_w * (size_t)v.fog_h);
        if (v.fog_bytes) {
            memset(v.fog_bytes, 255, (size_t)v.fog_w * (size_t)v.fog_h);
            v.fog_tex = GL3D_UploadTextureGrey(v.fog_bytes, v.fog_w, v.fog_h);
        }
        v.fog_age = V3_FOG_REFRESH_FRAMES;
    }
    v.built_for = world;
    v.built_grid = g;
    fprintf(stderr, "View3D: terrain %d blocks, %d meshes, %d runs, %d chunk textures\n",
            n, v.mesh_count, v.seg_count, v.chunk_count);
    return 0;
}

static void refresh_fog(const GameWorld *world) {
    if (!v.fog_tex || !v.fog_bytes) return;
    if (++v.fog_age < V3_FOG_REFRESH_FRAMES) return;
    v.fog_age = 0;
    int cell = world->fog_cell_px > 0 ? world->fog_cell_px : 16;
    for (int fy = 0; fy < v.fog_h; fy++) {
        for (int fx = 0; fx < v.fog_w; fx++) {
            int32_t wx = fx * cell + cell / 2, wy = fy * cell + cell / 2;
            int s = Fog_StateAt(world, wx, wy);
            uint8_t b = s == TAK_FOG_UNEXPLORED ? 0
                      : (Fog_ShowsAt(world, wx, wy) ? 255 : 128);
            v.fog_bytes[fy * v.fog_w + fx] = b;
        }
    }
    GL3D_UpdateTextureGrey(v.fog_tex, v.fog_bytes, v.fog_w, v.fog_h);
}

/* ── Models ───────────────────────────────────────────────────────── */

/* The classic renderer's model to map mapping (transform_unit_verts):
 * roll about the forward axis, pitch the nose, then yaw with the
 * handedness mirror, scale to pixels, and stand at the position. */
static void model_matrix(float out[16], float x, float y, float z,
                         float heading, float pitch, float roll, float scale) {
    const float ch = cosf(heading), sh = sinf(heading);
    const float cp = cosf(pitch),   sp = sinf(pitch);
    const float cr = cosf(roll),    sr = sinf(roll);
    for (int col = 0; col < 3; col++) {
        float mx = col == 0 ? 1.0f : 0.0f;
        float my = col == 1 ? 1.0f : 0.0f;
        float mz = col == 2 ? 1.0f : 0.0f;
        float ax = cr * mx - sr * my;
        float ay = sr * mx + cr * my;
        float by = cp * ay - sp * mz;
        float bz = sp * ay + cp * mz;
        float rx = -(ch * ax + sh * bz);
        float rz = -(sh * ax - ch * bz);
        out[col * 4 + 0] = rx * scale;
        out[col * 4 + 1] = by * scale;
        out[col * 4 + 2] = rz * scale;
        out[col * 4 + 3] = 0.0f;
    }
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1.0f;
}

static void pack_rows(const UnitNodeXform *xf, int n) {
    for (int i = 0; i < n; i++) {
        float *r = v.node_rows + (size_t)i * 12;
        memcpy(r, xf[i].rot, sizeof(float) * 9);
        r[9] = xf[i].trans[0]; r[10] = xf[i].trans[1]; r[11] = xf[i].trans[2];
        v.node_hidden[i] = xf[i].hidden;
    }
}

static void draw_model_at(const GpuModel *m, const CobPiece *pieces, int all_pieces,
                          float x, float y, float z, float heading, float pitch,
                          float roll, float alpha) {
    int n = m->mesh->node_count;
    if (n > UNIT_MESH_MAX_NODES) n = UNIT_MESH_MAX_NODES;
    /* A piece state belongs to the shipped model: the script addresses
     * pieces by the position they take in it. An artist's model has its
     * own pieces and its own count, so it draws at rest until a name
     * map joins the two. */
    if (m->from_gltf) pieces = NULL;
    Units_ComposeNodeXforms(m->mesh, pieces, v.xforms, !all_pieces);
    pack_rows(v.xforms, n);
    float mat[16];
    model_matrix(mat, x, y, z, heading, pitch, roll, Units_GetTAScale());
    GL3D_DrawModel(m->gl, mat, v.node_rows, v.node_hidden, n,
                   m->batches, m->batch_count, alpha);
}

static void draw_units(const GameWorld *world, const float planes[6][4]) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE && u->alive != UNIT_ALIVE_DYING) continue;
        /* A frame under half built is not drawn (legacy:197310). */
        if (u->under_construction && u->max_health > 0 &&
            u->health * 2 < u->max_health) continue;
        if (!Units_IsVisibleToLocalPlayer(u)) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!def) continue;
        const GpuModel *m = ModelStore_Get(def->objectname, u->team_color_idx);
        if (!m) continue;
        float h = (float)Terrain_SampleHeight(world, u->world_x, u->world_y) + u->flight_alt;
        float c[3] = { (float)u->world_x, h + m->height_px * 0.5f, (float)u->world_y };
        if (!Camera3D_SphereInFrustum(planes, c, m->radius_px)) continue;
        float alpha = 1.0f;
        if (u->under_construction && u->max_health > 0) {
            float t = (float)u->health / (float)u->max_health;
            alpha = (t - 0.5f) * 2.0f;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
        }
        if (u->magic_death) {
            alpha = (float)u->magic_death_fade / (float)UNIT_MAGIC_DEATH_TICKS;
        }
        draw_model_at(m, u->cob ? u->cob->pieces : NULL, 0,
                      (float)u->world_x, h, (float)u->world_y,
                      u->heading, u->pitch, u->roll, alpha);
        s_counts.units++;
    }
}

static SpriteTex *sprite_for(const GameWorld *world, const FeatureDef *fd) {
    for (int i = 0; i < v.sprite_count; i++) {
        if (v.sprites[i].fd == fd) return v.sprites[i].tex ? &v.sprites[i] : NULL;
    }
    if (v.sprite_count >= (int)(sizeof(v.sprites) / sizeof(v.sprites[0]))) return NULL;
    SpriteTex *s = &v.sprites[v.sprite_count++];
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    const uint32_t *pixels = NULL;
    if (Units_FeatureSpriteFrame(fd, world->features_rgba, 0, &pixels,
                                 &s->w, &s->h, &s->off_x, &s->off_y) <= 0 || !pixels)
        return NULL;
    s->tex = GL3D_UploadTextureRGBA(pixels, s->w, s->h, 1, 1);
    return s->tex ? s : NULL;
}

typedef struct Billboard {
    GL3D_Texture *tex;
    float x, z, top, bottom, off_x, w, shade;
    int flat;   /* a pad lies on the ground rather than standing up */
    float u0, u1, v1;   /* the strip cell this frame lies in */
} Billboard;

static int billboard_cmp(const void *a, const void *b) {
    const Billboard *x = (const Billboard *)a, *y = (const Billboard *)b;
    return (x->tex > y->tex) - (x->tex < y->tex);
}

static void draw_billboards(const Billboard *bb, int n) {
    if (n <= 0 || ensure_stream(n * 4, n * 6) != 0) return;
    /* Billboards face the camera about the vertical. */
    float rx = cosf(v.cam.yaw), rz = -sinf(v.cam.yaw);
    int start = 0;
    while (start < n) {
        int end = start;
        while (end < n && bb[end].tex == bb[start].tex) end++;
        int nv = 0, ni = 0;
        for (int i = start; i < end; i++) {
            const Billboard *b = &bb[i];
            float *o = v.stream_v + (size_t)nv * 9;
            float s = b->shade;
            if (b->flat) {
                /* On the ground, north up, its foreshortened height
                 * stretched back out (legacy:197689). */
                float x0 = b->x - b->off_x, x1 = x0 + b->w;
                float z0 = b->z - (b->top - b->bottom) * 0.5f;
                float z1 = b->z + (b->top - b->bottom) * 0.5f;
                float y = b->bottom;
                put_vert(o + 0,  x0, y, z0, b->u0, 0.0f, s, s, s, 1.0f);
                put_vert(o + 9,  x1, y, z0, b->u1, 0.0f, s, s, s, 1.0f);
                put_vert(o + 18, x1, y, z1, b->u1, b->v1, s, s, s, 1.0f);
                put_vert(o + 27, x0, y, z1, b->u0, b->v1, s, s, s, 1.0f);
            } else {
                float lx = b->x - rx * b->off_x, lz = b->z - rz * b->off_x;
                float hx = b->x + rx * (b->w - b->off_x), hz = b->z + rz * (b->w - b->off_x);
                put_vert(o + 0,  lx, b->top, lz,    b->u0, 0.0f, s, s, s, 1.0f);
                put_vert(o + 9,  hx, b->top, hz,    b->u1, 0.0f, s, s, s, 1.0f);
                put_vert(o + 18, hx, b->bottom, hz, b->u1, b->v1, s, s, s, 1.0f);
                put_vert(o + 27, lx, b->bottom, lz, b->u0, b->v1, s, s, s, 1.0f);
            }
            uint16_t *q = v.stream_i + ni;
            q[0] = (uint16_t)nv; q[1] = (uint16_t)(nv + 1); q[2] = (uint16_t)(nv + 2);
            q[3] = (uint16_t)nv; q[4] = (uint16_t)(nv + 2); q[5] = (uint16_t)(nv + 3);
            nv += 4; ni += 6;
        }
        GL3D_DrawSprites(v.stream_v, nv, v.stream_i, ni, bb[start].tex, 1, 0);
        start = end;
    }
}

static void draw_features(const GameWorld *world, const float planes[6][4]) {
    if (!world->features || world->feature_count <= 0) return;
    Billboard *bb = (Billboard *)tak_malloc(sizeof(Billboard) * (size_t)world->feature_count);
    if (!bb) return;
    int nb = 0;
    for (int i = 0; i < world->feature_count; i++) {
        const struct MapFeature *mf = &world->features[i];
        const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
        if (!fd) continue;
        if (fd->object[0]) {
            if (Fog_StateAt(world, mf->world_x, mf->world_y) == TAK_FOG_UNEXPLORED) continue;
            int c = (mf->color_idx >= 0 && mf->color_idx <= 11) ? mf->color_idx : 0;
            const GpuModel *m = ModelStore_Get(fd->object, c);
            if (!m) continue;
            float h = (float)Terrain_SampleHeight(world, mf->world_x, mf->world_y);
            float centre[3] = { (float)mf->world_x, h + m->height_px * 0.5f, (float)mf->world_y };
            if (!Camera3D_SphereInFrustum(planes, centre, m->radius_px)) continue;
            const float to_rad = 6.2831853f / 65536.0f;
            draw_model_at(m, NULL, 1, (float)mf->world_x, h, (float)mf->world_y,
                          (float)mf->heading * to_rad, (float)mf->pitch * to_rad,
                          (float)mf->roll * to_rad, 1.0f);
            continue;
        }
        int fp_x = fd->footprint_x > 0 ? fd->footprint_x : 1;
        int fp_z = fd->footprint_z > 0 ? fd->footprint_z : 1;
        int32_t wx = mf->tile_x * 16 + fp_x * 8;
        int32_t wy = mf->tile_z * 16 + fp_z * 8;
        if (Fog_StateAt(world, wx, wy) == TAK_FOG_UNEXPLORED) continue;
        /* A sprite has no object name to find a model by, so an
         * artist's model for it goes by the sequence name: the standing
         * stones around a mana site are models3d/verhenge01.glb and so
         * on. One stands where the picture would have lain. */
        const GpuModel *am = fd->seqname[0] ? ModelStore_GetArtists(fd->seqname) : NULL;
        if (am) {
            float h = (float)Terrain_SampleHeight(world, wx, wy);
            float centre[3] = { (float)wx, h + am->height_px * 0.5f, (float)wy };
            if (!Camera3D_SphereInFrustum(planes, centre, am->radius_px)) continue;
            draw_model_at(am, NULL, 1, (float)wx, h, (float)wy, 0.0f, 0.0f, 0.0f, 1.0f);
            s_counts.features++;
            continue;
        }
        SpriteTex *s = sprite_for(world, fd);
        if (!s) continue;
        float h = (float)Terrain_SampleHeight(world, wx, wy);
        Billboard *b = &bb[nb];
        b->tex = s->tex;
        b->x = (float)wx;
        b->z = (float)wy;
        b->flat = tak_stricmp(fd->category, "mana") == 0;
        if (b->flat) {
            /* A pad's picture is the ground it covers, seen at the tilt. */
            b->bottom = h + 0.5f;
            b->top = b->bottom + (float)s->h / Units_GetTanTilt();
        } else {
            b->top = h + (float)s->off_y * V3_SPRITE_RISE;
            b->bottom = h - (float)(s->h - s->off_y) * 0.5f;
        }
        b->off_x = (float)s->off_x;
        b->w = (float)s->w;
        b->u0 = 0.0f; b->u1 = 1.0f; b->v1 = 1.0f;
        b->shade = Fog_ShowsAt(world, wx, wy) ? 1.0f : 0.5f;
        float centre[3] = { b->x, (b->top + b->bottom) * 0.5f, b->z };
        if (!Camera3D_SphereInFrustum(planes, centre, (float)(s->w + s->h))) continue;
        nb++;
        s_counts.features++;
    }
    if (nb > 1) qsort(bb, (size_t)nb, sizeof(Billboard), billboard_cmp);
    draw_billboards(bb, nb);
    tak_free(bb);
}

/* A flat ring on the ground around each selected unit. */
static void draw_selection_rings(const GameWorld *world) {
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    if (n <= 0) return;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    const int segs = 28;
    if (ensure_stream(n * segs * 2, n * segs * 6) != 0) return;
    int nv = 0, ni = 0;
    for (int k = 0; k < n; k++) {
        int h = sel[k];
        if (h < 0 || h >= count) continue;
        const Unit *u = &units[h];
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        const GpuModel *m = def ? ModelStore_Get(def->objectname, u->team_color_idx) : NULL;
        float r = m ? m->foot_radius_px + 4.0f : 24.0f;
        float g = 1.0f, rr = 0.2f;
        if (u->player_id != Units_LocalPlayer()) { rr = 1.0f; g = 0.3f; }
        for (int s = 0; s < segs; s++) {
            float a = (float)s / (float)segs * 2.0f * V3_PI;
            float ca = cosf(a), sa = sinf(a);
            float x0 = (float)u->world_x + ca * r, z0 = (float)u->world_y + sa * r;
            float x1 = (float)u->world_x + ca * (r + 3.0f), z1 = (float)u->world_y + sa * (r + 3.0f);
            float y0 = (float)Terrain_SampleHeight(world, (int32_t)x0, (int32_t)z0) + 1.5f;
            float y1 = (float)Terrain_SampleHeight(world, (int32_t)x1, (int32_t)z1) + 1.5f;
            put_vert(v.stream_v + (size_t)(nv + 2 * s) * 9, x0, y0, z0, 0, 0, rr, g, 0.2f, 1.0f);
            put_vert(v.stream_v + (size_t)(nv + 2 * s + 1) * 9, x1, y1, z1, 0, 0, rr, g, 0.2f, 1.0f);
        }
        for (int s = 0; s < segs; s++) {
            int a = nv + 2 * s, b = nv + 2 * ((s + 1) % segs);
            uint16_t *q = v.stream_i + ni;
            q[0] = (uint16_t)a; q[1] = (uint16_t)(a + 1); q[2] = (uint16_t)b;
            q[3] = (uint16_t)(a + 1); q[4] = (uint16_t)(b + 1); q[5] = (uint16_t)b;
            ni += 6;
        }
        nv += segs * 2;
    }
    if (nv > 0) GL3D_DrawSprites(v.stream_v, nv, v.stream_i, ni, NULL, 0, 0);
}

static void draw_water(const GameWorld *world) {
    if (world->water_height <= 0) return;
    if (ensure_stream(4, 6) != 0) return;
    float y = (float)world->water_height;
    float r = 0.16f, g = 0.36f, b = 0.62f, a = 0.55f;
    put_vert(v.stream_v + 0,  0.0f,    y, 0.0f,    0, 0, r, g, b, a);
    put_vert(v.stream_v + 9,  v.map_w, y, 0.0f,    0, 0, r, g, b, a);
    put_vert(v.stream_v + 18, v.map_w, y, v.map_h, 0, 0, r, g, b, a);
    put_vert(v.stream_v + 27, 0.0f,    y, v.map_h, 0, 0, r, g, b, a);
    static const uint16_t q[6] = { 0, 1, 2, 0, 2, 3 };
    memcpy(v.stream_i, q, sizeof(q));
    GL3D_DrawSprites(v.stream_v, 4, v.stream_i, 6, NULL, 0, 1);
}

/* ── The view ─────────────────────────────────────────────────────── */

/* A weapon's sprite strip on the GPU, one per art slot. */
typedef struct EffectTex {
    int             sprite_idx;
    GL3D_Texture   *tex;
    ProjSpriteStrip strip;
} EffectTex;
static EffectTex s_effect_tex[128];
static int       s_effect_tex_count;

static void free_effect_tex(void) {
    for (int i = 0; i < s_effect_tex_count; i++) {
        if (s_effect_tex[i].tex) GL3D_FreeTexture(s_effect_tex[i].tex);
    }
    memset(s_effect_tex, 0, sizeof(s_effect_tex));
    s_effect_tex_count = 0;
}

static EffectTex *effect_tex_for(int sprite_idx) {
    for (int i = 0; i < s_effect_tex_count; i++) {
        if (s_effect_tex[i].sprite_idx == sprite_idx)
            return s_effect_tex[i].tex ? &s_effect_tex[i] : NULL;
    }
    if (s_effect_tex_count >= (int)(sizeof(s_effect_tex) / sizeof(s_effect_tex[0]))) return NULL;
    EffectTex *e = &s_effect_tex[s_effect_tex_count++];
    memset(e, 0, sizeof(*e));
    e->sprite_idx = sprite_idx;
    if (Units_ProjectileSpriteStrip(sprite_idx, &e->strip) <= 0) return NULL;
    e->tex = GL3D_UploadTextureRGBA(e->strip.pixels, e->strip.cell_w * e->strip.num_frames,
                                    e->strip.cell_h, 0, 1);
    return e->tex ? e : NULL;
}

/* A frame of a strip stood up at a point, anchored the way the classic
 * blit anchors it: the frame's top left sits (ox, oy) from the point. */
static void put_billboard(Billboard *b, const EffectTex *e, int frame,
                          float x, float y, float z) {
    const ProjSpriteStrip *st = &e->strip;
    float sw = (float)(st->cell_w * st->num_frames);
    b->tex = e->tex;
    b->flat = 0;
    b->shade = 1.0f;
    b->x = x;
    b->z = z;
    b->off_x = (float)st->ox[frame];
    b->w = (float)st->fw[frame];
    b->top = y + (float)st->oy[frame];
    b->bottom = b->top - (float)st->fh[frame];
    b->u0 = (float)(frame * st->cell_w) / sw;
    b->u1 = (float)(frame * st->cell_w + st->fw[frame]) / sw;
    b->v1 = (float)st->fh[frame] / (float)st->cell_h;
}

/* Projectiles in flight and the impact sprites where they landed.
 * Frame choice matches the classic passes. */
static void draw_effects(const GameWorld *world, const float planes[6][4]) {
    int pn = 0, en = 0;
    const Projectile *ps = Units_GetProjectiles(&pn);
    const ProjectileEffect *es = Units_GetProjectileEffects(&en);
    if (pn + en <= 0) return;
    Billboard *bb = (Billboard *)tak_malloc(sizeof(Billboard) * (size_t)(pn + en));
    if (!bb) return;
    int nb = 0;
    for (int i = 0; i < pn; i++) {
        const Projectile *p = &ps[i];
        if (!p->alive || p->is_beam || p->hidden) continue;
        if (!Units_ProjectileVisible(world, p)) continue;
        float c[3] = { (float)p->world_x, p->height, (float)p->world_y };
        if (p->art_kind == UNIT_WEAPON_ART_MODEL && p->art_idx >= 0) {
            const char *name = Units_ProjectileModelName(p->art_idx);
            int colour = p->color_idx > 11 ? 0 : p->color_idx;
            const GpuModel *m = name ? ModelStore_Get(name, colour) : NULL;
            if (!m) continue;
            if (!Camera3D_SphereInFrustum(planes, c, m->radius_px)) continue;
            draw_model_at(m, NULL, 1, c[0], c[1], c[2], p->heading, p->pitch, p->roll, 1.0f);
            s_counts.projectiles++;
            continue;
        }
        if (p->art_kind == UNIT_WEAPON_ART_SPRITE && p->art_idx >= 0) {
            EffectTex *e = effect_tex_for(p->art_idx);
            if (!e) continue;
            int nf = e->strip.num_frames;
            int frame = nf > 1 ? (int)((p->age_ticks / 2) % (uint16_t)nf) : 0;
            if (e->strip.fw[frame] <= 0 || e->strip.fh[frame] <= 0) continue;
            if (!Camera3D_SphereInFrustum(planes, c, (float)(e->strip.cell_w + e->strip.cell_h))) continue;
            put_billboard(&bb[nb++], e, frame, c[0], c[1], c[2]);
            s_counts.projectiles++;
            continue;
        }
        /* No art of its own: the bright dot the classic view draws. */
        if (p->visual_kind == UNIT_PROJECTILE_VIS_REMOTE) continue;
        if (!Camera3D_SphereInFrustum(planes, c, 8.0f)) continue;
        Billboard *b = &bb[nb++];
        b->tex = NULL; b->flat = 0; b->shade = 1.0f;
        b->x = c[0]; b->z = c[2];
        b->off_x = 3.0f; b->w = 6.0f;
        b->top = c[1] + 3.0f; b->bottom = c[1] - 3.0f;
        b->u0 = 0.0f; b->u1 = 1.0f; b->v1 = 1.0f;
        s_counts.projectiles++;
    }
    for (int i = 0; i < en; i++) {
        const ProjectileEffect *e = &es[i];
        if (!e->alive || e->delay_ticks) continue;
        if (!Fog_ShowsAt(world, e->world_x, e->world_y)) continue;
        EffectTex *et = effect_tex_for(e->sprite_idx);
        if (!et) continue;
        int nf = et->strip.num_frames;
        int frame = e->age_ticks / (e->ticks_per_frame ? e->ticks_per_frame : 2);
        if (frame >= nf) {
            if (!e->loops || nf <= 0) continue;
            frame %= nf;
        }
        if (et->strip.fw[frame] <= 0 || et->strip.fh[frame] <= 0) continue;
        float c[3] = { (float)e->world_x, (float)e->height, (float)e->world_y };
        if (!Camera3D_SphereInFrustum(planes, c, (float)(et->strip.cell_w + et->strip.cell_h))) continue;
        put_billboard(&bb[nb++], et, frame, c[0], c[1], c[2]);
        s_counts.effects++;
    }
    if (nb > 1) qsort(bb, (size_t)nb, sizeof(Billboard), billboard_cmp);
    draw_billboards(bb, nb);
    tak_free(bb);
}

/* Beams: a jagged ribbon from muzzle to target in three widths, the
 * outer ones fainter, jittered afresh each frame the way the classic
 * rays flicker. */
#define V3_BEAM_SEGS 8
static void draw_beams(const GameWorld *world) {
    int pn = 0;
    const Projectile *ps = Units_GetProjectiles(&pn);
    int beams = 0;
    for (int i = 0; i < pn; i++) if (ps[i].alive && ps[i].is_beam) beams++;
    if (beams <= 0) return;
    const int per_pass_v = (V3_BEAM_SEGS + 1) * 2, per_pass_i = V3_BEAM_SEGS * 6;
    if (ensure_stream(beams * 3 * per_pass_v, beams * 3 * per_pass_i) != 0) return;
    int ucount = 0;
    const Unit *units = Units_GetActive(&ucount);
    static unsigned flicker;
    flicker++;
    float rx = cosf(v.cam.yaw), rz = -sinf(v.cam.yaw);
    static const float half_w[3] = { 3.0f, 2.0f, 1.0f };
    static const float alpha[3] = { 0.35f, 0.65f, 1.0f };
    int nv = 0, ni = 0;
    for (int i = 0; i < pn; i++) {
        const Projectile *p = &ps[i];
        if (!p->alive || !p->is_beam || p->visual_kind == UNIT_PROJECTILE_VIS_FLAME) continue;
        if (!Units_ProjectileVisible(world, p)) continue;
        float ax = (float)p->src_x, az = (float)p->src_y;
        float ay = (float)p->src_height + 12.0f;
        float bx = (float)p->world_x, bz = (float)p->world_y;
        float by = (float)Terrain_SampleHeight(world, p->world_x, p->world_y) + 8.0f;
        if (p->target >= 0 && p->target < ucount) by += units[p->target].flight_alt;
        float jitter[V3_BEAM_SEGS + 1];
        for (int s = 0; s <= V3_BEAM_SEGS; s++) {
            unsigned h = (flicker * 2654435761u) ^ ((unsigned)i * 40503u) ^ ((unsigned)s * 97u);
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            jitter[s] = (s == 0 || s == V3_BEAM_SEGS) ? 0.0f : ((float)(h % 1000u) / 1000.0f - 0.5f) * 10.0f;
        }
        for (int pass = 0; pass < 3; pass++) {
            int ci = 2 - pass;   /* outer, middle, inner */
            float r = p->beam_rgb[ci][0] / 255.0f, gg = p->beam_rgb[ci][1] / 255.0f,
                  b = p->beam_rgb[ci][2] / 255.0f, a = alpha[pass], hw = half_w[pass];
            int base = nv;
            for (int s = 0; s <= V3_BEAM_SEGS; s++) {
                float t = (float)s / (float)V3_BEAM_SEGS;
                float x = ax + (bx - ax) * t + rx * jitter[s];
                float y = ay + (by - ay) * t;
                float z = az + (bz - az) * t + rz * jitter[s];
                put_vert(v.stream_v + (size_t)(nv + 2 * s) * 9, x - rx * hw, y, z - rz * hw, 0, 0, r, gg, b, a);
                put_vert(v.stream_v + (size_t)(nv + 2 * s + 1) * 9, x + rx * hw, y, z + rz * hw, 0, 0, r, gg, b, a);
            }
            for (int s = 0; s < V3_BEAM_SEGS; s++) {
                int q0 = base + 2 * s;
                uint16_t *q = v.stream_i + ni;
                q[0] = (uint16_t)q0; q[1] = (uint16_t)(q0 + 1); q[2] = (uint16_t)(q0 + 2);
                q[3] = (uint16_t)(q0 + 1); q[4] = (uint16_t)(q0 + 3); q[5] = (uint16_t)(q0 + 2);
                ni += 6;
            }
            nv += per_pass_v;
        }
        s_counts.beams++;
    }
    if (nv > 0) GL3D_DrawSprites(v.stream_v, nv, v.stream_i, ni, NULL, 0, 1);
}

static int v3_init(TAK_Platform *plat) {
    if (v.ready) return 0;
    if (!plat || !plat->window || !plat->renderer) return -1;
    if (GL3D_Init(plat->window, plat->renderer) != 0) {
        fprintf(stderr, "View3D: no usable GL context, staying classic\n");
        return -1;
    }
    v.inited_gl = 1;
    v.plat = plat;
    v.light[0] = -0.35f; v.light[1] = 0.80f; v.light[2] = 0.48f;
    float len = sqrtf(v.light[0] * v.light[0] + v.light[1] * v.light[1] + v.light[2] * v.light[2]);
    v.light[0] /= len; v.light[1] /= len; v.light[2] /= len;
    Camera3D_ClassicPreset(&v.cam, 0.0f, 0.0f, 16, 9);
    v.ready = 1;
    return 0;
}

static void v3_shutdown(TAK_Platform *plat) {
    (void)plat;
    if (!v.inited_gl) return;
    free_terrain();
    ModelStore_Clear();
    free_effect_tex();
    if (v.stream_v) tak_free(v.stream_v);
    if (v.stream_i) tak_free(v.stream_i);
    GL3D_Shutdown();
    memset(&v, 0, sizeof(v));
}

static void v3_render(const GameWorld *world, TAK_Platform *plat,
                      const SDL_Rect *viewport) {
    if (!v.ready || !world || !world->loaded || !plat) return;
    memset(&s_counts, 0, sizeof(s_counts));
    double t0 = (double)SDL_GetPerformanceCounter();
    if (v.built_for != world || v.built_grid != world->grid) {
        free_terrain();
        ModelStore_Clear();
        free_effect_tex();
        if (build_terrain(world) != 0) return;
        retarget_from_classic(world);
    }
    if (world->cam_x != v.synced_cam_x || world->cam_y != v.synced_cam_y) {
        /* Somebody else moved the classic camera: the minimap, a test. */
        retarget_from_classic(world);
    }
    v.viewport = viewport ? *viewport : (SDL_Rect){ 0, 0, plat->window_w, plat->window_h };
    if (v.viewport.w < 1) v.viewport.w = 1;
    if (v.viewport.h < 1) v.viewport.h = 1;
    v.cam.aspect = (float)v.viewport.w / (float)v.viewport.h;
    Camera3D_Clamp(&v.cam, v.map_w, v.map_h, ground_height, (void *)world);

    float vp[16], eye[3], planes[6][4];
    Camera3D_ViewProj(&v.cam, vp);
    Camera3D_Eye(&v.cam, eye);
    Camera3D_FrustumPlanes(vp, planes);
    refresh_fog(world);

    const float sky[3] = { 0.55f, 0.68f, 0.86f };
    GL3D_BeginFrame(plat->renderer, plat->window_w, plat->window_h, &v.viewport, sky);
    GL3D_SetCamera(vp, eye, v.light);
    GL3D_SetFog(v.fog_tex, v.map_w, v.map_h);
    GL3D_SetTime((float)SDL_GetTicks() / 1000.0f);
    for (int i = 0; i < v.seg_count; i++) {
        const TerrainSeg *s = &v.segs[i];
        if (s->mesh >= v.mesh_count || !v.meshes[s->mesh]) continue;
        if (!Camera3D_SphereInFrustum(planes, s->centre, s->radius)) continue;
        GL3D_Texture *tex = s->chunk < v.chunk_count ? v.chunk_tex[s->chunk] : NULL;
        GL3D_DrawTerrain(v.meshes[s->mesh], tex, s->first_index, s->index_count);
    }
    draw_features(world, planes);
    draw_selection_rings(world);
    draw_units(world, planes);
    draw_effects(world, planes);
    draw_beams(world);
    draw_water(world);
    GL3D_EndFrame();

    double freq = (double)SDL_GetPerformanceFrequency();
    double now = (double)SDL_GetPerformanceCounter();
    double ms = (now - t0) * 1000.0 / freq;
    if (v.render_frames == 0) v.window_t0 = t0;
    v.render_ms_sum += ms;
    if (++v.render_frames >= V3_FPS_WINDOW) {
        double secs = (now - v.window_t0) / freq;
        v.render_ms_last_avg = v.render_ms_sum / (double)v.render_frames;
        v.render_frames_last = v.render_frames;
        v.fps_last = secs > 0.0 ? (double)v.render_frames / secs : 0.0;
        fprintf(stderr, "View3D: %d frames, render %.2f ms average, %.0f frames per second\n",
                v.render_frames, v.render_ms_last_avg, v.fps_last);
        v.render_ms_sum = 0.0;
        v.render_frames = 0;
    }
}

static int v3_pointer_to_world(const GameWorld *world, const TAK_Platform *plat,
                               int wx, int wy, int32_t *out_x, int32_t *out_y) {
    (void)plat;
    if (!v.ready || !world || !world->loaded) return 0;
    if (wx < v.viewport.x || wy < v.viewport.y ||
        wx >= v.viewport.x + v.viewport.w || wy >= v.viewport.y + v.viewport.h)
        return 0;
    float o[3], d[3], gx = 0.0f, gz = 0.0f;
    Camera3D_PointerRay(&v.cam, v.viewport.w, v.viewport.h,
                        (float)(wx - v.viewport.x), (float)(wy - v.viewport.y), o, d);
    if (!Camera3D_RayHitGround(o, d, v.cam.far_z, ground_height, (void *)world, &gx, &gz))
        return 0;
    if (gx < 0.0f || gz < 0.0f || gx >= v.map_w || gz >= v.map_h) return 0;
    /* Hand back the flat reading the order path lifts onto the ground
     * itself, the inverse of the classic lift (legacy:197689). */
    float h = ground_height((void *)world, gx, gz);
    if (out_x) *out_x = (int32_t)gx;
    if (out_y) *out_y = (int32_t)(gz - h * Units_GetTanTilt());
    return 1;
}

static int v3_pointer_to_unit(const GameWorld *world, const TAK_Platform *plat,
                              int wx, int wy) {
    int32_t x = 0, y = 0;
    if (!v3_pointer_to_world(world, plat, wx, wy, &x, &y)) return -1;
    return Units_PickAt(x, y, 48);
}

static void v3_scroll(GameWorld *world, int32_t dx, int32_t dy) {
    if (!v.ready || !world || (!dx && !dy)) return;
    /* A screen pixel of scroll covers more ground the further out the
     * camera sits, relative to the classic preset's distance. */
    float scale = v.cam.dist / 1000.0f;
    if (scale < 0.25f) scale = 0.25f;
    Camera3D_Pan(&v.cam, (float)dx * scale, -(float)dy * scale);
    Camera3D_Clamp(&v.cam, v.map_w, v.map_h, ground_height, world);
    write_classic_cam(world);
}

static const TAK_View k_view3d = {
    "3d",
    v3_init,
    v3_shutdown,
    v3_render,
    v3_pointer_to_world,
    v3_pointer_to_unit,
    v3_scroll,
};

const TAK_View *View_3D(void) { return &k_view3d; }
View3DDrawCounts View3D_DebugDrawCounts(void) {
    return s_counts;
}

int View3D_IsReady(void) { return v.ready; }
Camera3D *View3D_Camera(void) { return &v.cam; }

double View3D_RenderMsAverage(int *out_frames) {
    if (out_frames) *out_frames = v.render_frames_last;
    return v.render_ms_last_avg;
}

void View3D_EnterFrom(const GameWorld *world) {
    if (!v.ready || !world) return;
    int vw = world->viewport_w > 0 ? world->viewport_w : 16;
    int vh = world->viewport_h > 0 ? world->viewport_h : 9;
    Camera3D_ClassicPreset(&v.cam, (float)(world->cam_x + vw / 2),
                           (float)(world->cam_y + vh / 2), vw, vh);
    v.synced_cam_x = world->cam_x;
    v.synced_cam_y = world->cam_y;
    v.dragging = 0;
    fprintf(stderr, "View3D: camera over (%.0f, %.0f)\n", v.cam.target_x, v.cam.target_z);
}

void View3D_LeaveTo(GameWorld *world) {
    if (!v.ready || !world) return;
    write_classic_cam(world);
}

void View3D_Input(GameWorld *world, const TAK_Platform *plat,
                  const uint8_t *keys, const uint8_t *prev_keys,
                  float frame_dt, int mouse_x, int mouse_y,
                  int middle_down, int wheel_dy) {
    (void)plat;
    if (!v.ready || !world || !keys) return;
    float dyaw = 0.0f, dpitch = 0.0f, zoom = 1.0f;
    const float turn = 1.7f * frame_dt, tilt = 1.1f * frame_dt;
    if (keys[SDL_SCANCODE_Q]) dyaw += turn;
    if (keys[SDL_SCANCODE_E]) dyaw -= turn;
    if (keys[SDL_SCANCODE_R]) dpitch -= tilt;
    if (keys[SDL_SCANCODE_F]) dpitch += tilt;
    if (keys[SDL_SCANCODE_Z]) zoom *= powf(0.35f, frame_dt);
    if (keys[SDL_SCANCODE_X]) zoom *= powf(1.0f / 0.35f, frame_dt);
    if (wheel_dy) zoom *= powf(0.82f, (float)wheel_dy);
    if (middle_down) {
        if (v.dragging) {
            dyaw   -= (float)(mouse_x - v.last_mx) * 0.006f;
            dpitch += (float)(mouse_y - v.last_my) * 0.006f;
        }
        v.dragging = 1;
        v.last_mx = mouse_x;
        v.last_my = mouse_y;
    } else {
        v.dragging = 0;
    }
    if (keys[SDL_SCANCODE_HOME] && prev_keys && !prev_keys[SDL_SCANCODE_HOME]) {
        Camera3D_ClassicPreset(&v.cam, v.cam.target_x, v.cam.target_z,
                               v.viewport.w, v.viewport.h);
    }
    Camera3D_Orbit(&v.cam, dyaw, dpitch);
    Camera3D_Zoom(&v.cam, zoom);
    Camera3D_Clamp(&v.cam, v.map_w, v.map_h, ground_height, world);
    write_classic_cam(world);
}
