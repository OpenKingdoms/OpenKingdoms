/*
 * terrain.c -- TAK terrain renderer (Phase B2).
 *
 * Draws the map as a grid of 64x64-pixel "blocks". Each block picks
 * a 64x64 sub-region out of a 512x512 chunk texture (the JPG files
 * in terrain.hpi). See docs/TERRAIN_GUIDE.md + the legacy
 * UnitManager_UpdateAll for the empirical basis of this layout.
 *
 * Graphics design:
 *   - Chunk textures are deduplicated. A map like Ground War has 6400
 *     blocks but only ~100 unique chunks; we upload each JPG once.
 *   - Per-block entries store a 16-bit chunk_idx into the dedup table,
 *     keeping the array at 4 bytes/block (cache-friendly).
 *   - Textures use SDL_TEXTUREACCESS_STATIC (upload once, sample many).
 *   - Per-frame render loop culls to the visible block rectangle and
 *     issues one SDL_RenderCopy per block. ~500 draw calls on a 1080p
 *     viewport — well within GPU budget. No CPU-side blitting.
 *   - Chunk loading is budgeted across Loading_Tick frames so the
 *     progress bar stays smooth on big maps.
 */

#include "tak_terrain.h"
#include "tak_gpu.h"
#include "tak_types.h"
#include "tak_hpi.h"
#include "tak_jpg.h"
#include "tak_memory.h"
#include "tak_world.h"
#include "tak_features.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── chunk-id dedup helpers ──────────────────────────────────────────
 *
 * Build a sorted unique chunk_id array via qsort. O(N log N) where N
 * is total blocks (~6400 for GW, ~74000 for CASTLE). Runs once at
 * init, not in any hot path. */

static int uint32_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Binary-search the sorted chunks[] table. Returns index or -1. */
static int find_chunk_idx(const TerrainChunkEntry *chunks, int n,
                           uint32_t chunk_id) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if      (chunks[mid].chunk_id < chunk_id) lo = mid + 1;
        else if (chunks[mid].chunk_id > chunk_id) hi = mid;
        else return mid;
    }
    return -1;
}

/* ── Init / Free ─────────────────────────────────────────────────── */

TerrainGrid *TerrainGrid_Init(const TNTFile *tnt) {
    if (!tnt || !tnt->block_chunk_ids || !tnt->block_tex_x || !tnt->block_tex_y)
        return NULL;
    if (tnt->blocks_w <= 0 || tnt->blocks_h <= 0) return NULL;

    TerrainGrid *g = (TerrainGrid *)tak_malloc(sizeof(TerrainGrid));
    if (!g) return NULL;
    memset(g, 0, sizeof(*g));
    g->blocks_w = tnt->blocks_w;
    g->blocks_h = tnt->blocks_h;

    size_t n = (size_t)g->blocks_w * (size_t)g->blocks_h;
    g->blocks = (TerrainBlock *)tak_malloc(n * sizeof(TerrainBlock));
    if (!g->blocks) { tak_free(g); return NULL; }

    /* Build sorted unique chunk_id list. Temporary scratch, freed
     * after we compress it into g->chunks[]. */
    uint32_t *scratch = (uint32_t *)tak_malloc(n * sizeof(uint32_t));
    if (!scratch) { tak_free(g->blocks); tak_free(g); return NULL; }
    memcpy(scratch, tnt->block_chunk_ids, n * sizeof(uint32_t));
    qsort(scratch, n, sizeof(uint32_t), uint32_cmp);

    /* Compact: write one entry per distinct chunk_id. */
    size_t uniq = 0;
    for (size_t i = 0; i < n; i++) {
        if (uniq == 0 || scratch[i] != scratch[uniq - 1]) {
            scratch[uniq++] = scratch[i];
        }
    }

    g->chunk_count = (int)uniq;
    g->chunks = (TerrainChunkEntry *)tak_malloc(uniq * sizeof(TerrainChunkEntry));
    if (!g->chunks) {
        tak_free(scratch); tak_free(g->blocks); tak_free(g);
        return NULL;
    }
    for (size_t i = 0; i < uniq; i++) {
        g->chunks[i].chunk_id = scratch[i];
        g->chunks[i].tex_w    = 0;
        g->chunks[i].tex_h    = 0;
        g->chunks[i].tex      = NULL;
    }
    tak_free(scratch);

    /* Diagnostic: report max tex_x/tex_y so anomalous maps (values
     * beyond what a typical chunk contains) show up in the log. The
     * renderer itself always samples 32-px sub-blocks per the
     * decomp invariant; these values are informational only. */
    uint8_t max_tx = 0, max_ty = 0;
    for (size_t i = 0; i < n; i++) {
        if (tnt->block_tex_x[i] > max_tx) max_tx = tnt->block_tex_x[i];
        if (tnt->block_tex_y[i] > max_ty) max_ty = tnt->block_tex_y[i];
    }

    /* Pre-resolve every block's chunk_idx. After this pass the render
     * loop never needs to hash/search — just g->chunks[block.chunk_idx]. */
    if (uniq > 0xFFFF) {
        /* Paranoia: chunk_idx is uint16. Max practical map is CASTLE
         * with ~1156 unique chunks; anything over 65535 means either
         * a corrupt TNT or a map format we don't support. */
        fprintf(stderr, "TerrainGrid_Init: %zu distinct chunks exceeds uint16\n", uniq);
        tak_free(g->chunks); tak_free(g->blocks); tak_free(g);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        int idx = find_chunk_idx(g->chunks, g->chunk_count,
                                  tnt->block_chunk_ids[i]);
        g->blocks[i].chunk_idx = (uint16_t)(idx >= 0 ? idx : 0);
        g->blocks[i].tex_x     = tnt->block_tex_x[i];
        g->blocks[i].tex_y     = tnt->block_tex_y[i];
    }

    fprintf(stderr, "TerrainGrid_Init: %d x %d blocks, %d unique chunks, "
                    "max tex=(%u,%u)\n",
            g->blocks_w, g->blocks_h, g->chunk_count,
            (unsigned)max_tx, (unsigned)max_ty);
    return g;
}

void TerrainGrid_Free(TerrainGrid *grid, TAK_Platform *plat) {
    if (!grid) return;
    if (grid->chunks) {
        for (int i = 0; i < grid->chunk_count; i++) {
            if (grid->chunks[i].tex) {
                GPU_FreeTexture(plat, grid->chunks[i].tex);
                grid->chunks[i].tex = NULL;
            }
        }
        tak_free(grid->chunks);
    }
    if (grid->blocks) tak_free(grid->blocks);
    tak_free(grid);
}

/* ── Per-chunk load ─────────────────────────────────────────────── */

int TerrainGrid_LoadChunk(TerrainGrid *grid, int chunk_idx, TAK_Platform *plat) {
    if (!grid || !plat || !grid->chunks) return -1;
    if (chunk_idx < 0 || chunk_idx >= grid->chunk_count) return -1;
    TerrainChunkEntry *e = &grid->chunks[chunk_idx];
    if (e->tex) return 0;  /* idempotent */

    char path[64];
    snprintf(path, sizeof(path), "terrain/%08x.jpg", e->chunk_id);

    void    *jpg_bytes = NULL;
    uint32_t jpg_size  = 0;
    if (VFS_ReadFile(path, &jpg_bytes, &jpg_size) != 0) {
        fprintf(stderr, "Terrain: VFS_ReadFile failed for %s\n", path);
        return -1;
    }

    uint32_t *pixels = NULL;
    int w = 0, h = 0;
    int rc = JPG_DecodeRGBA((const uint8_t *)jpg_bytes, (size_t)jpg_size,
                             &pixels, &w, &h);
    tak_free(jpg_bytes);
    if (rc != 0 || !pixels) {
        fprintf(stderr, "Terrain: JPG_DecodeRGBA failed for %s\n", path);
        return -1;
    }

    e->tex = GPU_UploadRGBA(plat, pixels, w, h);
    tak_free(pixels);
    if (!e->tex) {
        fprintf(stderr, "Terrain: GPU_UploadRGBA failed for %s\n", path);
        return -1;
    }
    e->tex_w = w;
    e->tex_h = h;
    /* Log any non-standard chunk size. Most maps use 512×512 chunks
     * uniformly; seeing 256 or 1024 means this map mixes sizes and
     * the invariant sub-block render is doing heavy lifting. */
    if (w != 512 || h != 512) {
        fprintf(stderr, "Terrain: chunk %08x is %dx%d (non-standard)\n",
                e->chunk_id, w, h);
    }
    return 0;
}

/* ── Per-frame render ───────────────────────────────────────────── */

/* Per the legacy engine (section_w >> 5 logic), a chunk is always
 * divided into 32-pixel sub-blocks regardless of chunk size. A 256×256
 * chunk has 8×8 sub-blocks, a 512×512 chunk has 16×16, a 1024×1024
 * chunk has 32×32. Block-to-chunk sampling is always at this 32-px
 * granularity, and tex_x / tex_y directly index into it:
 *     src = (tex_x * 32, tex_y * 32, 32, 32)
 * This invariant is map-independent — works for any shipped map and
 * any mix of chunk sizes on a single map. */
#define TAK_SUB_CHUNK_PX   32
#define TAK_BLOCK_MAP_PX   32   /* 2 tiles × 16 px/tile */

#define TAK_TILE_WORLD_PX  16

int Terrain_SampleHeight(const struct GameWorld *world,
                         int32_t world_x, int32_t world_y) {
    if (!world) return 0;
    const TNTFile *t = &world->tnt;
    if (!t->heightmap || t->height_w <= 1 || t->height_h <= 1) return 0;
    if (world_x < 0) world_x = 0;
    if (world_y < 0) world_y = 0;
    int max_x = (t->height_w - 1) * TAK_TILE_WORLD_PX - 1;
    int max_y = (t->height_h - 1) * TAK_TILE_WORLD_PX - 1;
    if (world_x > max_x) world_x = max_x;
    if (world_y > max_y) world_y = max_y;

    int tx = (int)(world_x / TAK_TILE_WORLD_PX);
    int tz = (int)(world_y / TAK_TILE_WORLD_PX);
    int fx = (int)(world_x % TAK_TILE_WORLD_PX);
    int fz = (int)(world_y % TAK_TILE_WORLD_PX);
    if (tx >= t->height_w - 1) tx = t->height_w - 2;
    if (tz >= t->height_h - 1) tz = t->height_h - 2;

    const uint8_t *hm = t->heightmap;
    int stride = t->height_w;
    /* Raw map elevation — no display bias (see tnt.c). Callers that
     * compare against sidedata waterheight use these units directly. */
    int h00 = (int)hm[tz * stride + tx];
    int h10 = (int)hm[tz * stride + tx + 1];
    int h01 = (int)hm[(tz + 1) * stride + tx];
    int h11 = (int)hm[(tz + 1) * stride + tx + 1];
    int hx0 = h00 + ((h10 - h00) * fx) / TAK_TILE_WORLD_PX;
    int hx1 = h01 + ((h11 - h01) * fx) / TAK_TILE_WORLD_PX;
    return hx0 + ((hx1 - hx0) * fz) / TAK_TILE_WORLD_PX;
}

static int feature_blocks_movement(const FeatureDef *fd) {
    /* The authored `blocking` attribute alone decides — mirrors the
     * legacy per-feature flag test (featuredef+0x13c bit 5,
     * legacy:219128). Shipped data authors blocking=1 on every
     * rock/tree/wall and blocking=0 on walkable-over decor (waves,
     * noise), so category names carry no extra signal and would
     * wrongly block modded walkable features. */
    return fd ? (fd->blocking != 0) : 0;
}

int Terrain_SlopeAllows(const struct GameWorld *world,
                        int32_t world_x, int32_t world_y,
                        int max_slope) {
    if (!world) return 0;
    if (world_x < 0 || world_y < 0 ||
        world_x >= world->map_pixels_w || world_y >= world->map_pixels_h)
        return 0;
    if (max_slope <= 0) max_slope = 12;

    /* Per-cell corner-delta slope test: the cell containing the point
     * is impassable when its four corner heights span more than
     * max_slope. Mirrors the legacy per-cell validity check
     * (legacy:219149-219165, corner extrema vs maxslope) —
     * the previous bilinear centre-vs-neighbour comparison under-
     * rejected saddle cells whose corners disagree. */
    const TNTFile *t = &world->tnt;
    if (t->heightmap && t->height_w > 1 && t->height_h > 1) {
        int tx = (int)(world_x / 16);
        int tz = (int)(world_y / 16);
        if (tx >= t->height_w - 1) tx = t->height_w - 2;
        if (tz >= t->height_h - 1) tz = t->height_h - 2;
        const uint8_t *hm = t->heightmap;
        int stride = t->height_w;
        int h00 = hm[tz * stride + tx];
        int h10 = hm[tz * stride + tx + 1];
        int h01 = hm[(tz + 1) * stride + tx];
        int h11 = hm[(tz + 1) * stride + tx + 1];
        int hmin = h00, hmax = h00;
        if (h10 < hmin) hmin = h10;
        if (h10 > hmax) hmax = h10;
        if (h01 < hmin) hmin = h01;
        if (h01 > hmax) hmax = h01;
        if (h11 < hmin) hmin = h11;
        if (h11 > hmax) hmax = h11;
        if (hmax - hmin > max_slope) return 0;
    }
    return 1;
}

int Terrain_IsWalkable(const struct GameWorld *world,
                       int32_t world_x, int32_t world_y,
                       int max_slope) {
    if (!Terrain_SlopeAllows(world, world_x, world_y, max_slope)) return 0;

    if (world->features && world->feature_count > 0) {
        for (int i = 0; i < world->feature_count; i++) {
            const FeatureDef *fd = Features_GetByIndex(world->features[i].global_idx);
            if (!feature_blocks_movement(fd)) continue;
            int fp_x = (fd->footprint_x > 0) ? fd->footprint_x : 1;
            int fp_z = (fd->footprint_z > 0) ? fd->footprint_z : 1;
            int32_t x0 = (int32_t)world->features[i].tile_x * 16;
            int32_t y0 = (int32_t)world->features[i].tile_z * 16;
            int32_t x1 = x0 + fp_x * 16;
            int32_t y1 = y0 + fp_z * 16;
            if (world_x >= x0 && world_x < x1 && world_y >= y0 && world_y < y1)
                return 0;
        }
    }
    return 1;
}

void Terrain_Render(const struct GameWorld *world, TAK_Platform *plat) {
    if (!world || !plat) return;
    const TerrainGrid *g = world->grid;
    if (!g || !g->blocks || !g->chunks) return;

    const int32_t cam_x      = world->cam_x;
    const int32_t cam_y      = world->cam_y;
    const int     viewport_w = world->viewport_w;
    const int     viewport_h = world->viewport_h;
    const int     blocks_w   = g->blocks_w;
    const int     blocks_h   = g->blocks_h;

    /* Viewport culling in block coords. */
    int first_bx = (int)(cam_x / TAK_BLOCK_MAP_PX);
    int first_by = (int)(cam_y / TAK_BLOCK_MAP_PX);
    int last_bx  = (int)((cam_x + viewport_w - 1) / TAK_BLOCK_MAP_PX);
    int last_by  = (int)((cam_y + viewport_h - 1) / TAK_BLOCK_MAP_PX);
    if (first_bx < 0)          first_bx = 0;
    if (first_by < 0)          first_by = 0;
    if (last_bx  >= blocks_w)  last_bx  = blocks_w - 1;
    if (last_by  >= blocks_h)  last_by  = blocks_h - 1;

    for (int by = first_by; by <= last_by; by++) {
        for (int bx = first_bx; bx <= last_bx; bx++) {
            const TerrainBlock *b = &g->blocks[by * blocks_w + bx];
            const TerrainChunkEntry *ce = &g->chunks[b->chunk_idx];
            if (!ce->tex) continue;

            int sx = b->tex_x * TAK_SUB_CHUNK_PX;
            int sy = b->tex_y * TAK_SUB_CHUNK_PX;

            /* Out-of-bounds guard: if the TNT's tex_x overflows this
             * specific chunk's texture (e.g. 256×256 chunk with a
             * block using tex_x=8), skip rather than sample garbage.
             * In practice this never fires on shipped maps. */
            if (sx + TAK_SUB_CHUNK_PX > ce->tex_w) continue;
            if (sy + TAK_SUB_CHUNK_PX > ce->tex_h) continue;

            SDL_Rect src = { sx, sy, TAK_SUB_CHUNK_PX, TAK_SUB_CHUNK_PX };
            SDL_Rect dst = {
                bx * TAK_BLOCK_MAP_PX - (int)cam_x,
                by * TAK_BLOCK_MAP_PX - (int)cam_y,
                TAK_BLOCK_MAP_PX,
                TAK_BLOCK_MAP_PX,
            };
            GPU_DrawToWindow(plat, ce->tex, &src, &dst);
        }
    }
}
