#include "tak_tnt.h"
#include "tak_memory.h"
#include "tak_hpi.h"
#include "tak_types.h"
#include <stdio.h>
#include <string.h>

#define TNT_MAGIC_NUM 0x00004000

/* Header fields run from 0x00 (magic) to 0x30 (the overview image
 * pointer), so a file shorter than this has no header at all. */
#define TNT_HEADER_BYTES 0x34

static void tnt_build_derived_heightmap(TNTFile *out) {
    if (!out || !out->tile_map || out->width_tiles <= 0 || out->height_tiles <= 0)
        return;
    int W = out->width_tiles;
    int H = out->height_tiles;
    size_t n = (size_t)(W + 1) * (size_t)(H + 1);
    uint8_t *hm = (uint8_t *)tak_malloc(n);
    if (!hm) return;

    for (int z = 0; z <= H; z++) {
        for (int x = 0; x <= W; x++) {
            int sx = (x >= W) ? W - 1 : x;
            int sz = (z >= H) ? H - 1 : z;
            /* Elevation is the map byte VERBATIM (legacy copies it
             * straight into the cell record, legacy:225022,
             * and takes min/max of the raw quad :224592-224622).
             * The old (v & 0x3F - 32)/2 + clamp aliased heights: on
             * Angvir's Maze floor 62 became 47 while wall 125 became
             * 46 — walls read LOWER than floors and phantom cliffs
             * appeared mid-corridor, fencing off passages and bridges. */
            hm[z * (W + 1) + x] = (uint8_t)out->tile_map[sz * W + sx];
        }
    }
    out->heightmap = hm;
    out->height_w = W + 1;
    out->height_h = H + 1;
}

int  TNT_Load(TNTFile *out, const char *path, const uint32_t *rgba_table) {
    if (!out || !path || !rgba_table) return -1;
    memset(out, 0, sizeof(*out));

    uint32_t tnt_size = 0;
    uint8_t *tnt_buffer = NULL;
    void *tnt_raw = NULL;
    if (VFS_ReadFile(path, &tnt_raw, &tnt_size) == 0 &&
        (tnt_buffer = (uint8_t *)tnt_raw) != NULL) {
        /* The header runs to 0x34. A map pack from anywhere can hold a
         * stub, so check before reading any of it. */
        if (tnt_size < TNT_HEADER_BYTES) {
            fprintf(stderr, "TNT: %s is %u bytes, too short for a header\n",
                    path, tnt_size);
            tak_free(tnt_buffer);
            return -1;
        }
        if ((int)*(uint32_t*)tnt_buffer != TNT_MAGIC_NUM) {
            fprintf(stderr, "Unexpected magic number read from TNT file%s\n", path);
            tak_free(tnt_buffer);
            return -1;
        }

        out->width_tiles  = (int)*(uint32_t*)(tnt_buffer + 0x04);
        out->height_tiles = (int)*(uint32_t*)(tnt_buffer + 0x08);
        /* 0x0C is the height water reaches on this map (legacy:224912). */
        out->sea_level    = (int)*(uint32_t*)(tnt_buffer + 0x0C);
        out->raw      = tnt_buffer;
        out->raw_size = (size_t)tnt_size;

        /* Pointer-and-bounds-check each sub-section. Layout verified
         * empirically across Ground War / takmission01_mt / CASTLE /
         * Muntil's Ford Guard (all block sizes matched the hypotheses
         * W*H and W*H*2). If a header pointer plus its
         * expected size escapes the file we leave the field NULL so the
         * caller can skip that feature rather than read past EOF. */
        const int W  = out->width_tiles;
        const int H  = out->height_tiles;

        if (W > 0 && H > 0) {
            /* 0x10 -> tile_map: W*H bytes, uint8 per cell. Empirically
             * verified: Ground War shows uniform 0x3E (62), Angvir's
             * Maze shows varied 61..125 forming the maze. */
            uint32_t off_map = *(uint32_t*)(tnt_buffer + 0x10);
            size_t   sz_map  = (size_t)W * (size_t)H;
            if ((size_t)off_map + sz_map <= tnt_size) {
                out->tile_map = tnt_buffer + off_map;
                tnt_build_derived_heightmap(out);
            } else {
                fprintf(stderr, "TNT: %s tile_map out of range (off=0x%x, need %zu)\n",
                        path, off_map, sz_map);
            }

            /* 0x14 -> feature_layer: W*H uint16s, sentinel-marked
             * (0xFFFF = no feature). Not consumed in Phase B. */
            uint32_t off_feat = *(uint32_t*)(tnt_buffer + 0x14);
            size_t   sz_feat  = (size_t)W * (size_t)H * 2;
            if ((size_t)off_feat + sz_feat <= tnt_size) {
                out->feature_layer = (const uint16_t*)(tnt_buffer + off_feat);
            } else {
                fprintf(stderr, "TNT: %s feature_layer out of range (off=0x%x, need %zu)\n",
                        path, off_feat, sz_feat);
            }

            /* 0x18 -> per-map FEATURE NAMES table.
             * 0x1C -> uint32 count of entries in the table.
             *
             * Verified empirically by hex-dumping Per Mare Per Terras:
             *   header[0x1C] = 0x15 (=21)
             *   table size   = (next_section - 0x18) / 21 = 132 bytes/rec
             * Each record:
             *   [+0..+3]  uint32 record index (0,1,2,...)
             *   [+4..+131] null-terminated ASCII name (≤128 bytes)
             * The cells in feature_layer are LOCAL indices into this
             * table; the engine maps each value through this table to
             * a feature def name, then through the global registry. */
            uint32_t off_names = *(uint32_t*)(tnt_buffer + 0x18);
            uint32_t cnt_names = *(uint32_t*)(tnt_buffer + 0x1C);
            const size_t REC_SIZE = 132;
            if (off_names > 0 && cnt_names > 0 &&
                off_names + (size_t)cnt_names * REC_SIZE <= tnt_size) {
                char (*names)[64] = (char (*)[64])tak_malloc((size_t)cnt_names * 64);
                if (names) {
                    for (uint32_t i = 0; i < cnt_names; i++) {
                        const uint8_t *rec = tnt_buffer + off_names + (size_t)i * REC_SIZE;
                        size_t k = 0;
                        while (k < 63 && rec[4 + k] != 0) {
                            names[i][k] = (char)rec[4 + k];
                            k++;
                        }
                        names[i][k] = '\0';
                    }
                    out->feature_names = names;
                    out->num_feature_names = (int)cnt_names;
                    fprintf(stderr,
                        "TNT: %s feature_names = %u entries (first: %s)\n",
                        path, cnt_names, cnt_names > 0 ? names[0] : "(none)");
                }
            } else if (cnt_names > 0) {
                fprintf(stderr,
                    "TNT: %s feature_names table out of range "
                    "(off=0x%x, count=%u, file=%u)\n",
                    path, off_names, cnt_names, tnt_size);
            }

            /* 0x20 / 0x24 / 0x28 -> three parallel per-block arrays.
             * Block = 2x2 tile = 64x64 px region of the map.
             * Count per array = (W/2) * (H/2).
             *   0x20: uint32 chunk_id per block
             *   0x24: uint8  tex_x per block (0..7 sub-block within chunk)
             *   0x28: uint8  tex_y per block (0..7)
             * Both the 0x20 offset and the intra-chunk coords were
             * confirmed against the legacy renderer and a raw hex
             * dump of Ground War. */
            int bw = W / 2;
            int bh = H / 2;
            if (bw > 0 && bh > 0) {
                size_t n = (size_t)bw * (size_t)bh;
                uint32_t off_ids   = *(uint32_t*)(tnt_buffer + 0x20);
                uint32_t off_tex_x = *(uint32_t*)(tnt_buffer + 0x24);
                uint32_t off_tex_y = *(uint32_t*)(tnt_buffer + 0x28);
                size_t sz_ids = n * sizeof(uint32_t);
                size_t sz_bx  = n;
                size_t sz_by  = n;
                if ((size_t)off_ids   + sz_ids <= tnt_size &&
                    (size_t)off_tex_x + sz_bx  <= tnt_size &&
                    (size_t)off_tex_y + sz_by  <= tnt_size) {
                    out->block_chunk_ids = (const uint32_t *)(tnt_buffer + off_ids);
                    out->block_tex_x     = tnt_buffer + off_tex_x;
                    out->block_tex_y     = tnt_buffer + off_tex_y;
                    out->blocks_w        = bw;
                    out->blocks_h        = bh;
                } else {
                    fprintf(stderr, "TNT: %s per-block arrays out of range "
                            "(0x20=0x%x 0x24=0x%x 0x28=0x%x, need %zu/%zu/%zu)\n",
                            path, off_ids, off_tex_x, off_tex_y,
                            sz_ids, sz_bx, sz_by);
                }
            }
        }

        /* 0x30 -> in-game minimap background image:
         *   [4]  uint32 width  (pixels)
         *   [4]  uint32 height (pixels)
         *   [w*h] 8-bit palette indices — a pre-rendered map overview
         * authored at build time. Used as the backdrop for the in-battle
         * minimap widget. NOT a tile atlas; real terrain art lives in
         * terrain.hpi as JPG chunks. Observed dims: w varies 396..449,
         * h always 431 across shipped maps. */
        uint32_t off_mm_bg = *(uint32_t*)(tnt_buffer + 0x30);
        if ((size_t)off_mm_bg + 8 > tnt_size) {
            fprintf(stderr, "TNT: %s minimap_bg header past EOF (off=0x%x, size=%u)\n",
                    path, off_mm_bg, tnt_size);
        } else {
            uint32_t mw = *(uint32_t*)(tnt_buffer + off_mm_bg);
            uint32_t mh = *(uint32_t*)(tnt_buffer + off_mm_bg + 4);
            size_t   sz = (size_t)mw * (size_t)mh;
            if (mw == 0 || mh == 0 || mw > 4096 || mh > 4096 ||
                (size_t)off_mm_bg + 8 + sz > tnt_size) {
                fprintf(stderr, "TNT: %s minimap_bg dims invalid (%u x %u, need %zu)\n",
                        path, mw, mh, sz);
            } else {
                out->ingame_minimap_bg = tnt_buffer + off_mm_bg + 8;
                out->minimap_bg_w      = (int)mw;
                out->minimap_bg_h      = (int)mh;
            }
        }

        /* TAK 0x4000 format: minimap pointer is at header offset 0x2C.
         * At that pointer: uint32 width, uint32 height, then width*height
         * bytes of 8-bit palette indices. Typical dims are 126x126.
         * (HPIView's TNTHEADER / docs put it at 0x28 — that's for TA's
         * older 0x2000 format. Verified empirically on TAK 0x4000 files.) */
        uint32_t minimap_data_offset = *(uint32_t*)(tnt_buffer + 0x2C);
        if (minimap_data_offset + 8 > (uint32_t)tnt_size) {
            fprintf(stderr, "No minimap available for this TNT file %s\n", path);
            return 0;
        }

        uint32_t minimap_width  = *(uint32_t *)(tnt_buffer + minimap_data_offset);
        uint32_t minimap_height = *(uint32_t *)(tnt_buffer + minimap_data_offset + 4);
        if (minimap_width == 0 || minimap_height == 0 || minimap_width > 512 || minimap_height > 512) {
            fprintf(stderr, "TNT: %s minimap dims out of range (%u x %u)\n",
                    path, minimap_width, minimap_height);
            return 0;
        }

        size_t minimap_rgba_area = (size_t)minimap_height * minimap_width;
        if (minimap_data_offset + 8 + minimap_rgba_area > (size_t)tnt_size) {
            fprintf(stderr, "TNT: %s minimap pixel data truncated\n", path);
            return 0;
        }

        const uint8_t *minimap_rgba_data_offset = tnt_buffer + minimap_data_offset + 8;
        uint32_t *minimap_rgba = (uint32_t*)tak_malloc(minimap_rgba_area * sizeof(uint32_t));
        if (!minimap_rgba) {
            fprintf(stderr, "Unable to load minimap data from TNT file%s\n", path);
            tak_free(tnt_buffer);
            return -1;
        }
        for (size_t i = 0; i < minimap_rgba_area; i ++) {
            minimap_rgba[i] = rgba_table[minimap_rgba_data_offset[i]];
        }
        
        out->minimap_rgba = minimap_rgba;
        out->minimap_h = minimap_height;
        out->minimap_w = minimap_width;

        return 0;
    } else {
        fprintf(stderr, "Unable to read TNT file from VFS %s\n", path);
        return -1;
    }
}

void TNT_Close(TNTFile *tnt) {
    if (!tnt) return;
    if (tnt->minimap_rgba) tak_free(tnt->minimap_rgba);
    if (tnt->feature_names) tak_free(tnt->feature_names);
    if (tnt->heightmap) tak_free(tnt->heightmap);
    if (tnt->raw)          tak_free(tnt->raw);
    memset(tnt, 0, sizeof(*tnt));
}
