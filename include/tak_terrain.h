#ifndef TAK_TERRAIN_H
#define TAK_TERRAIN_H

#include "tak_tnt.h"
#include "tak_gpu.h"
#include <SDL.h>

/* Forward declaration — breaks the tak_terrain <-> tak_world include
 * cycle. The prototype below uses `struct GameWorld *` rather than the
 * typedef, so terrain.h doesn't need to see the full definition. Files
 * that actually dereference the GameWorld include tak_world.h directly. */
struct GameWorld;

/* One 64x64-pixel map block. 4 bytes per block keeps the array
 * cache-friendly (CASTLE: 272x272 blocks = ~288 KB). Every field
 * is pre-computed at TerrainGrid_Init — no per-frame hashing. */
typedef struct TerrainBlock {
    uint16_t chunk_idx;    /* index into TerrainGrid.chunks[]           */
    uint8_t  tex_x;        /* 0..7 sub-block within the chunk texture   */
    uint8_t  tex_y;        /* 0..7                                      */
} TerrainBlock;

/* One deduplicated chunk texture. Multiple TerrainBlocks reference the
 * same entry via chunk_idx, so each JPG is decoded and uploaded once
 * regardless of how many blocks use it. tex_w/tex_h are the real
 * texture dimensions (usually 512×512, but TAK also ships 256×256 and
 * 1024×1024 chunks) and are needed by the renderer to compute the
 * per-map sub-block size. */
typedef struct TerrainChunkEntry {
    uint32_t      chunk_id;
    int           tex_w;
    int           tex_h;
    GPU_Texture  *tex;     /* NULL until TerrainGrid_LoadChunk runs */
} TerrainChunkEntry;

typedef struct TerrainGrid {
    int                 blocks_w;
    int                 blocks_h;
    TerrainBlock       *blocks;       /* blocks_w * blocks_h */

    /* Sorted by chunk_id so block-to-entry resolution was O(log N)
     * at init; at render time we already have the index, so lookup
     * is O(1). */
    int                 chunk_count;
    TerrainChunkEntry  *chunks;
} TerrainGrid;

/* Allocate + populate a TerrainGrid from a loaded TNT. Deduplicates
 * chunk IDs into the chunks[] table and pre-resolves every block's
 * chunk_idx. Returns NULL if the TNT has no per-block arrays. */
TerrainGrid *TerrainGrid_Init(const TNTFile *tnt);

/* Destroy every uploaded GPU texture, free both arrays, free the grid.
 * Safe on NULL. plat is needed to release GPU textures. */
void TerrainGrid_Free(TerrainGrid *grid, TAK_Platform *plat);

/* Decode + upload one chunk by **chunk entry index** (0..chunk_count-1).
 * Idempotent on already-loaded entries. Load progress is measured
 * against chunk_count, not block count — load once per unique JPG. */
int TerrainGrid_LoadChunk(TerrainGrid *grid, int chunk_idx, TAK_Platform *plat);

/* Per-frame terrain render. Visible blocks are computed from the
 * current camera + viewport, each produces one textured quad via
 * SDL_RenderCopy. On a 1080p window that's ~500 draw calls, well
 * within the GPU's budget. */
void Terrain_Render(const struct GameWorld *world, TAK_Platform *plat);

/* Bilinear terrain-height sample in world pixels. Return value is a
 * screen/model-space height offset in pixels relative to nominal ground. */
int Terrain_SampleHeight(const struct GameWorld *world,
                         int32_t world_x, int32_t world_y);

/* Movement/build passability against map bounds, authored blocking
 * features, and steep height steps. max_slope is in sampled height
 * units; values <= 0 use a conservative default. */
int Terrain_IsWalkable(const struct GameWorld *world,
                       int32_t world_x, int32_t world_y,
                       int max_slope);

/* Bounds + corner-delta slope alone, without the blocking-feature
 * half. Build placement runs the two separately because the yardmap
 * decides per cell whether features count (legacy:218831). */
int Terrain_SlopeAllows(const struct GameWorld *world,
                        int32_t world_x, int32_t world_y,
                        int max_slope);

#endif /* TAK_TERRAIN_H */
