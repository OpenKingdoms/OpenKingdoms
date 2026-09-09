/*
 * recon_terrain.c -- Phase B2 recon tool. Runs R1..R5 to nail down the
 * terrain chunk-record layout in .tnt files and cross-check against
 * terrain.hpi.
 *
 * Not part of the shipping runtime — CMake target "recon_terrain".
 *
 * Output is human-readable; cut&paste straight into
 * docs/PHASE_B2_TERRAIN.md section 7 ("Recon log").
 */

#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_tnt.h"
#include "tak_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* ── terrain.hpi filename set ────────────────────────────────────────
 * All files in terrain.hpi whose basename is exactly 8 hex digits get
 * parsed as uint32 chunk IDs and stored in a sorted array. Lookups are
 * binary-search (O(log n)) so we can probe thousands of candidate IDs
 * cheaply during R1. */

static uint32_t *g_terrain_ids = NULL;
static size_t    g_terrain_n   = 0;
static int       g_have_dds    = 0;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Return 1 and write out_id if basename looks like 8 hex + matching ext. */
static int parse_chunk_filename(const char *path, const char *ext,
                                 uint32_t *out_id) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t el = strlen(ext);
    size_t nl = strlen(name);
    if (nl != 8 + el) return 0;
    if (tak_stricmp(name + 8, ext) != 0) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        int n = hex_nibble(name[i]);
        if (n < 0) return 0;
        v = (v << 4) | (uint32_t)n;
    }
    *out_id = v;
    return 1;
}

static int uint32_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int id_in_set(uint32_t id) {
    if (!g_terrain_ids) return 0;
    size_t lo = 0, hi = g_terrain_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if      (g_terrain_ids[mid] < id) lo = mid + 1;
        else if (g_terrain_ids[mid] > id) hi = mid;
        else return 1;
    }
    return 0;
}

static int load_terrain_set(void) {
    char **paths = NULL;
    int n = 0;
    if (VFS_ListFiles("terrain/*", &paths, &n) != 0 || n <= 0) {
        fprintf(stderr, "VFS_ListFiles('terrain/*') failed\n");
        return -1;
    }
    g_terrain_ids = (uint32_t *)malloc(sizeof(uint32_t) * n);
    size_t jpg = 0, dds = 0;
    for (int i = 0; i < n; i++) {
        uint32_t id;
        if (parse_chunk_filename(paths[i], ".jpg", &id)) {
            g_terrain_ids[jpg++] = id;
        } else if (parse_chunk_filename(paths[i], ".dds", &id)) {
            dds++;
            g_have_dds = 1;
        }
    }
    g_terrain_n = jpg;
    qsort(g_terrain_ids, g_terrain_n, sizeof(uint32_t), uint32_cmp);
    /* dedupe — shouldn't be any but play safe */
    size_t w = 0;
    for (size_t r = 0; r < g_terrain_n; r++) {
        if (w == 0 || g_terrain_ids[r] != g_terrain_ids[w - 1])
            g_terrain_ids[w++] = g_terrain_ids[r];
    }
    g_terrain_n = w;
    printf("[terrain.hpi] %d entries, %zu unique .jpg chunks, %zu .dds\n",
           n, g_terrain_n, dds);
    return 0;
}

/* ── R1: find chunk-records pointer in TNT header ────────────────────
 * For each candidate offset 0x00..0x7C (every 4 bytes), treat the uint32
 * there as a pointer into the .tnt buffer. At that pointer, read
 * expected_n records of 8 bytes each, interpret the first 4 bytes of each
 * record as a chunk ID, and count how many hit the terrain.hpi set.
 *
 * The real offset should give near-100% hit rate. All others give ~0%. */

typedef struct R1Hit {
    uint32_t header_offset;
    uint32_t records_pointer;
    int      hits;
    int      total;
} R1Hit;

static void r1_probe_map(const char *vfs_path,
                          int expected_chunks_w, int expected_chunks_h,
                          R1Hit *best, int print_all) {
    void *buf_v = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(vfs_path, &buf_v, &size) != 0) {
        printf("  [R1] cannot read %s\n", vfs_path);
        return;
    }
    const uint8_t *buf = (const uint8_t *)buf_v;
    int expected_n = expected_chunks_w * expected_chunks_h;

    R1Hit local_best = { 0, 0, 0, expected_n };
    printf("  [R1] %s  (expecting %d records)\n", vfs_path, expected_n);

    for (uint32_t off = 0; off + 4 <= 0x80 && off + 4 <= size; off += 4) {
        uint32_t ptr = *(const uint32_t *)(buf + off);
        if (ptr == 0) continue;
        uint32_t need = (uint32_t)expected_n * 8;
        if ((uint64_t)ptr + need > size) continue;

        int hits = 0;
        for (int i = 0; i < expected_n; i++) {
            uint32_t id = *(const uint32_t *)(buf + ptr + (size_t)i * 8);
            if (id_in_set(id)) hits++;
        }
        if (print_all && hits > 0) {
            printf("    header+0x%02x -> ptr 0x%08x : %d/%d hits\n",
                   off, ptr, hits, expected_n);
        }
        if (hits > local_best.hits) {
            local_best.header_offset   = off;
            local_best.records_pointer = ptr;
            local_best.hits            = hits;
        }
    }
    if (local_best.hits > 0) {
        printf("    BEST: header+0x%02x -> 0x%08x (%d/%d hits)\n",
               local_best.header_offset, local_best.records_pointer,
               local_best.hits, local_best.total);
    } else {
        printf("    no offset produced hits.\n");
    }

    if (best && local_best.hits > best->hits) *best = local_best;
    tak_free(buf_v);
}

/* ── R3: inspect JPG magic + SOFn markers for 10 chunks ─────────────── */

static void r3_inspect_chunk(const char *vfs_path) {
    void *buf = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(vfs_path, &buf, &size) != 0) {
        printf("    %-32s cannot read\n", vfs_path);
        return;
    }
    const uint8_t *p = (const uint8_t *)buf;
    if (size < 4 || p[0] != 0xFF || p[1] != 0xD8) {
        printf("    %-32s bad magic %02x %02x (size=%u)\n",
               vfs_path, p[0], p[1], size);
        tak_free(buf);
        return;
    }
    /* Walk markers looking for SOF0..SOF15 (excluding DHT/DAC/COM). */
    size_t i = 2;
    int w = -1, h = -1, precision = -1, comps = -1;
    while (i + 3 < size) {
        if (p[i] != 0xFF) break;
        uint8_t m = p[i + 1];
        i += 2;
        if (m == 0xD9) break;             /* EOI */
        if (m >= 0xD0 && m <= 0xD7) continue; /* RSTn, no payload */
        if (m == 0x01) continue;
        if (i + 2 > size) break;
        uint16_t seglen = ((uint16_t)p[i] << 8) | p[i + 1];
        /* SOF0..SOF3, SOF5..SOF7, SOF9..SOF11, SOF13..SOF15 */
        int is_sof = (m >= 0xC0 && m <= 0xCF)
                   && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (is_sof && seglen >= 8 && i + 8 <= size) {
            precision = p[i + 2];
            h = ((int)p[i + 3] << 8) | p[i + 4];
            w = ((int)p[i + 5] << 8) | p[i + 6];
            comps = p[i + 7];
            break;
        }
        i += seglen;
    }
    printf("    %-32s %u bytes, %dx%d, %d-bit, %d comps\n",
           vfs_path, size, w, h, precision, comps);
    tak_free(buf);
}

/* ── R5: histogram of second uint32 in chunk records ────────────────── */

static void r5_distribution(const char *vfs_path, uint32_t records_offset,
                             int n_records) {
    void *buf = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(vfs_path, &buf, &size) != 0) return;
    const uint8_t *b = (const uint8_t *)buf;
    if ((size_t)records_offset + (size_t)n_records * 8 > size) {
        tak_free(buf);
        return;
    }
    /* Collect, sort, find distinct count + show first 8 distinct. */
    uint32_t *vals = (uint32_t *)malloc(sizeof(uint32_t) * n_records);
    for (int i = 0; i < n_records; i++) {
        vals[i] = *(const uint32_t *)(b + records_offset + (size_t)i * 8 + 4);
    }
    qsort(vals, n_records, sizeof(uint32_t), uint32_cmp);
    int distinct = 0;
    uint32_t sample[8]; int ns = 0;
    for (int i = 0; i < n_records; i++) {
        if (i == 0 || vals[i] != vals[i - 1]) {
            distinct++;
            if (ns < 8) sample[ns++] = vals[i];
        }
    }
    printf("    %-40s  N=%d distinct=%d   first: ",
           vfs_path, n_records, distinct);
    for (int i = 0; i < ns; i++) printf("%08x ", sample[i]);
    printf("\n");
    free(vals);
    tak_free(buf);
}

/* ── driver ─────────────────────────────────────────────────────────── */

typedef struct MapSpec {
    const char *vfs_path;
    int         w_tiles;
    int         h_tiles;
    const char *label;
} MapSpec;

int main(void) {
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    if (load_terrain_set() != 0) return 1;

    MapSpec maps[] = {
        { "maps/Maps/Ground War.tnt",      160, 160, "Ground War"    },
        { "maps/Maps/Angvir's Maze.tnt",   128, 128, "Angvir's Maze" },
        { "maps/Maps/CASTLE.tnt",          544, 544, "CASTLE"        },
        { "maps/Maps/Muntil's Ford Guard.tnt", 352, 224, "Muntil"    },
    };
    int nmaps = (int)(sizeof(maps) / sizeof(maps[0]));

    /* R4 first, it's just a printout. */
    printf("\n=== R4: .dds alongside .jpg ===\n");
    printf("  g_have_dds = %d\n", g_have_dds);

    /* R1 across all maps. For the chunks_w/h we use tiles / 16 because
     * each chunk covers 512 px = 16 × 32-px tiles. Will reconcile with
     * the decomp's "W/2 * H/2" if both interpretations hit. */
    printf("\n=== R1 + R2: chunk-records pointer probe ===\n");
    R1Hit best_overall = {0};
    for (int i = 0; i < nmaps; i++) {
        int cw = maps[i].w_tiles / 16;
        int ch = maps[i].h_tiles / 16;
        R1Hit best = {0};
        r1_probe_map(maps[i].vfs_path, cw, ch, &best, /*print_all=*/1);
        if (best.hits > best_overall.hits) best_overall = best;
    }

    /* R5: second uint32 variability across maps at the discovered
     * offset — only if R1 found something. */
    printf("\n=== R5: second uint32 distribution ===\n");
    if (best_overall.hits > 0) {
        for (int i = 0; i < nmaps; i++) {
            void *buf = NULL;
            uint32_t size = 0;
            if (VFS_ReadFile(maps[i].vfs_path, &buf, &size) != 0) continue;
            uint32_t ptr = *(const uint32_t *)((uint8_t *)buf + best_overall.header_offset);
            int n = (maps[i].w_tiles / 16) * (maps[i].h_tiles / 16);
            tak_free(buf);
            r5_distribution(maps[i].vfs_path, ptr, n);
        }
    } else {
        printf("  (skipped — R1 found no offset)\n");
    }

    /* R3: inspect 10 chunks. Prefer ones referenced by Ground War if we
     * found its records; otherwise the first 10 in the sorted set. */
    printf("\n=== R3: chunk format (first 10 JPGs) ===\n");
    for (size_t i = 0; i < 10 && i < g_terrain_n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "terrain/%08x.jpg", g_terrain_ids[i]);
        r3_inspect_chunk(path);
    }

    free(g_terrain_ids);
    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
