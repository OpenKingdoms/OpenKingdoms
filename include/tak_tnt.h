#ifndef TAK_TNT_H
#define TAK_TNT_H

#include "tak_types.h"

/* Map render data lives in THREE parallel per-block arrays at TNT
 * header offsets 0x20, 0x24, 0x28. Each block covers a 64×64-pixel
 * region of the map (= 2×2 terrain tiles of 32 px each), not a full
 * 512×512 chunk. The total block count is (W_tiles/2) × (H_tiles/2).
 *
 * Each block picks a 64×64 sub-region of a specific 512×512 chunk
 * texture by combining:
 *   block_chunk_ids[i]  uint32  which terrain/<%08x>.jpg to sample
 *   block_tex_x[i]      uint8   sub-block x within the chunk (0..7)
 *   block_tex_y[i]      uint8   sub-block y within the chunk (0..7)
 *
 * The draw primitive is therefore 64×64, and a 512×512 chunk covers
 * 8×8 = 64 blocks. Many adjacent blocks share the same chunk_id so
 * the texture cache should dedupe. */

/* One decoded map. Minimap pixels are RGBA32 ready to blit; the caller
 * frees via TNT_Close. */
typedef struct TNTFile {
    int        width_tiles;     /* map width / 16 */
    int        height_tiles;    /* map height / 16 */
    /* Header field 0x0C: the height water reaches on this map, in
     * the same units as the terrain bytes. The original keeps it
     * for the whole battle and every depth test reads it
     * (legacy:224912). Shipped maps use 39 to 78. */
    int        sea_level;

    uint32_t  *minimap_rgba;    /* heap — minimap_w * minimap_h pixels */
    int        minimap_w;       /* always 252 in TAK (validate below) */
    int        minimap_h;       /* always 252 */

    /* Raw buffer held so sub-sections (heightmap, tile data) can be
     * parsed lazily. This is left NULL in the stripped-down minimap pass. */
    uint8_t   *raw;
    size_t     raw_size;
    
    /* Tile map: one byte per cell, row-major. Value selects the terrain
     * graphic for that cell — believed to be an index into the shared
     * terrain.hpi JPG chunk library (to be confirmed in Phase B2). Lives
     * at header offset 0x10 — this was previously guessed as tile_attr,
     * but empirical dumps (Ground War = uniform 0x3E=62, Angvir's Maze =
     * varied 61..125 forming maze shape) confirmed it's the real tile
     * map. NULL if bounds check failed. */
    const uint8_t  *tile_map;
    uint8_t        *heightmap;       /* (width_tiles+1) * (height_tiles+1), owned */
    int             height_w;
    int             height_h;

    /* Header offset 0x14: W*H uint16s, dominated by 0xFFFF/0xFFFC
     * sentinels. Likely a features/specials overlay layer (TA's
     * PtrMapAttr equivalent, promoted to uint16 for TAK). Phase D
     * concern — unused in Phase B rendering. NULL if bounds failed. */
    const uint16_t *feature_layer;

    /* Per-map feature NAME table — at TNT header offset 0x18.
     * Each entry is 128 bytes: 4-byte zero header + null-terminated
     * ASCII name (up to ~120 chars) + padding. Names like
     * "VerMana03" / "AraHenge04" / "VerBuild03" identify which
     * FeatureDef from data/features/<world>/*.tdf the local TNT IDs
     * (in feature_layer) reference. The legacy engine maps each
     * TNT cell value through this table to get the global feature
     * name, then looks the name up in the global feature registry
     * to spawn the actual feature (rocks, lodestones, trees etc.).
     *
     * num_feature_names is the entry count; feature_names[i] is the
     * name (truncated to 64 chars for safety). The local feature_layer
     * value `id` resolves to feature_names[id] (when id < count). */
    char  (*feature_names)[64];
    int     num_feature_names;

    /* In-battle minimap background: a pre-rendered ~1/12-scale overview
     * image of the whole map, stored at header field 0x30 as 8 bytes
     * (uint32 width, uint32 height) followed by width*height bytes of
     * 8-bit palette indices. Used as the backdrop for the in-game
     * minimap widget (and probably other UI views of the full map).
     *
     * NOT a tile atlas — earlier code misnamed it. Actual terrain tile
     * graphics live in terrain.hpi as 512x512 JPG chunks. This image
     * is just the map-preview authored at build time.
     *
     *   ingame_minimap_bg     -> first pixel of the overview image
     *   minimap_bg_w/_h       -> overview image dimensions in pixels
     *     (observed: w varies 396..449, h always 431 across shipped maps) */
    uint8_t   *ingame_minimap_bg;
    int        minimap_bg_w;
    int        minimap_bg_h;


    /* Per-block render arrays parsed from TNT offsets 0x20 / 0x24 / 0x28.
     * All three have (blocks_w * blocks_h) entries; stride per entry is
     * 4 bytes for block_chunk_ids, 1 byte for the two coord arrays.
     * Pointers are views into `raw` — no copies, no ownership. NULL if
     * the bounds check in TNT_Load failed. */
    const uint32_t *block_chunk_ids;   /* 0x20: uint32 per block */
    const uint8_t  *block_tex_x;       /* 0x24: uint8 per block (0..7) */
    const uint8_t  *block_tex_y;       /* 0x28: uint8 per block (0..7) */
    int             blocks_w;          /* = width_tiles / 2 */
    int             blocks_h;          /* = height_tiles / 2 */
} TNTFile;

/* Load a .tnt file through VFS (`maps/Maps/<name>.tnt`). Requires a
 * 256-color palette for the minimap conversion — pass the shared game
 * palette. Returns 0 on success. */
int  TNT_Load(TNTFile *out, const char *path, const uint32_t *rgba_table);
void TNT_Close(TNTFile *tnt);

#endif /* TAK_TNT_H */
