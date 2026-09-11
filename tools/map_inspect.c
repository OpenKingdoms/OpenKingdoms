#include "tak_hpi.h"
#include "tak_map_fingerprint.h"
#include "tak_maps.h"
#include "tak_memory.h"
#include "tak_palette.h"
#include "tak_tdf.h"
#include "tak_tnt.h"
#include "tak_util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

typedef struct MapStart {
    int player;
    int x;
    int z;
} MapStart;

typedef struct MapInspect {
    char ota_path[256];
    char tnt_path[256];
    char fingerprint[TAK_MAP_FINGERPRINT_HEX];
    char mission_name[128];
    char description[256];
    char kingdom[64];
    char size_text[64];
    int size_x;
    int size_y;
    int num_players;
    int tnt_width_tiles;
    int tnt_height_tiles;
    int minimap_w;
    int minimap_h;
    int minimap_bg_w;
    int minimap_bg_h;
    int feature_name_count;
    int blocks_w;
    int blocks_h;
    int terrain_byte_min;
    int terrain_byte_max;
    int terrain_byte_distinct;
    int terrain_byte_zero_count;
    int terrain_byte_ff_count;
    int derived_height_min;
    int derived_height_max;
    int derived_height_distinct;
    int feature_cell_count;
    int feature_empty_count;
    int feature_continuation_count;
    MapStart starts[16];
    int start_count;
} MapInspect;

static uint32_t g_rgba_table[256];
static int g_rgba_ready = 0;

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int parse_size_text(const char *text, int *out_x, int *out_y) {
    int x = 0, y = 0;
    if (!text) return -1;
    if (sscanf(text, " %d x %d", &x, &y) != 2) return -1;
    if (x <= 0 || y <= 0) return -1;
    *out_x = x;
    *out_y = y;
    return 0;
}

static int parse_start_player(const char *what) {
    const char *p;
    if (!what) return -1;
    p = strstr(what, "StartPos");
    if (!p) return -1;
    p += 8;
    if (!isdigit((unsigned char)*p)) return -1;
    return atoi(p);
}

static void derive_tnt_path(const char *ota_path, char *out, size_t out_cap) {
    copy_str(out, out_cap, ota_path);
    size_t n = strlen(out);
    if (n >= 4 && tak_stricmp(out + n - 4, ".ota") == 0) {
        memcpy(out + n - 4, ".tnt", 5);
    }
}

static void strip_extension(const char *path, char *out, size_t out_cap) {
    const char *base = path;
    const char *slash;
    copy_str(out, out_cap, "");
    if (!path || !out || out_cap == 0) return;
    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    if (slash) base = slash + 1;
    copy_str(out, out_cap, base);
    size_t n = strlen(out);
    if (n > 4 && out[n - 4] == '.') out[n - 4] = '\0';
}

static int load_rgba(uint32_t rgba_table[256]) {
    Palette pal;
    if (Palette_Load(&pal, "data/palettes/palette.pal") != 0) {
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        uint8_t r = pal.entries[i].r;
        uint8_t g = pal.entries[i].g;
        uint8_t b = pal.entries[i].b;
        rgba_table[i] = ((uint32_t)r) | ((uint32_t)g << 8) |
                        ((uint32_t)b << 16) | 0xFF000000u;
    }
    return 0;
}

static const uint32_t *get_rgba(void) {
    if (!g_rgba_ready) {
        if (load_rgba(g_rgba_table) != 0) return NULL;
        g_rgba_ready = 1;
    }
    return g_rgba_table;
}

static int inspect_map(const char *ota_path, MapInspect *out) {
    TDFFile *tdf;
    const uint32_t *rgba;
    TNTFile tnt;

    if (!ota_path || !out) return -1;
    memset(out, 0, sizeof(*out));
    copy_str(out->ota_path, sizeof(out->ota_path), ota_path);
    derive_tnt_path(ota_path, out->tnt_path, sizeof(out->tnt_path));

    /* The content fingerprint, over the files that decide how the
     * map plays (see tak_map_fingerprint.h). */
    char key[128];
    strip_extension(ota_path, key, sizeof(key));
    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
    if (TAK_MapFingerprint_FromName(key, fp) == 0)
        TAK_MapFingerprint_ToHex(fp, out->fingerprint);

    tdf = TDF_Open(ota_path);
    if (!tdf) return -1;
    if (TDF_Load(tdf) != 0) {
        TDF_Close(tdf);
        return -1;
    }

    if (TDF_PushSection(tdf, "GlobalHeader") != 0) {
        TDF_Close(tdf);
        return -1;
    }

    copy_str(out->mission_name, sizeof(out->mission_name),
             TDF_ReadString(tdf, "missionname", ""));
    copy_str(out->description, sizeof(out->description),
             TDF_ReadString(tdf, "missiondescription", ""));
    copy_str(out->kingdom, sizeof(out->kingdom),
             TDF_ReadString(tdf, "kingdom", ""));
    copy_str(out->size_text, sizeof(out->size_text),
             TDF_ReadString(tdf, "size", ""));
    out->num_players = TDF_ReadInt(tdf, "numplayers", 0);
    parse_size_text(out->size_text, &out->size_x, &out->size_y);

    if (TDF_PushSection(tdf, "Map Data") == 0) {
        if (TDF_PushSection(tdf, "specials") == 0) {
            for (int i = 0; i < 64 && out->start_count < 16; i++) {
                char section[32];
                snprintf(section, sizeof(section), "special%d", i);
                if (TDF_PushSection(tdf, section) != 0) continue;

                int player = parse_start_player(
                    TDF_ReadString(tdf, "specialwhat", ""));
                if (player > 0) {
                    MapStart *sp = &out->starts[out->start_count++];
                    sp->player = player;
                    sp->x = TDF_ReadInt(tdf, "XPos", 0);
                    sp->z = TDF_ReadInt(tdf, "ZPos", 0);
                }
                TDF_PopSection(tdf);
            }
            TDF_PopSection(tdf);
        }
        TDF_PopSection(tdf);
    }
    TDF_PopSection(tdf);
    TDF_Close(tdf);

    rgba = get_rgba();
    if (!rgba) return -1;
    if (TNT_Load(&tnt, out->tnt_path, rgba) != 0) return -1;
    out->tnt_width_tiles = tnt.width_tiles;
    out->tnt_height_tiles = tnt.height_tiles;
    out->minimap_w = tnt.minimap_w;
    out->minimap_h = tnt.minimap_h;
    out->minimap_bg_w = tnt.minimap_bg_w;
    out->minimap_bg_h = tnt.minimap_bg_h;
    out->feature_name_count = tnt.num_feature_names;
    out->blocks_w = tnt.blocks_w;
    out->blocks_h = tnt.blocks_h;
    out->terrain_byte_min = 255;
    out->terrain_byte_max = 0;
    out->derived_height_min = 255;
    out->derived_height_max = 0;
    if (tnt.tile_map && tnt.width_tiles > 0 && tnt.height_tiles > 0) {
        uint8_t seen[256] = {0};
        uint8_t seen_height[256] = {0};
        size_t n = (size_t)tnt.width_tiles * (size_t)tnt.height_tiles;
        for (size_t i = 0; i < n; i++) {
            int v = tnt.tile_map[i];
            int h = ((v & 0x3F) - 32) / 2;
            if (h < -16) h = -16;
            if (h > 31) h = 31;
            if (v < out->terrain_byte_min) out->terrain_byte_min = v;
            if (v > out->terrain_byte_max) out->terrain_byte_max = v;
            if (!seen[v]) {
                seen[v] = 1;
                out->terrain_byte_distinct++;
            }
            if (v == 0) out->terrain_byte_zero_count++;
            if (v == 255) out->terrain_byte_ff_count++;
            if (h < out->derived_height_min) out->derived_height_min = h;
            if (h > out->derived_height_max) out->derived_height_max = h;
            if (!seen_height[(uint8_t)(h + 128)]) {
                seen_height[(uint8_t)(h + 128)] = 1;
                out->derived_height_distinct++;
            }
        }
    }
    if (tnt.feature_layer && tnt.width_tiles > 0 && tnt.height_tiles > 0) {
        size_t n = (size_t)tnt.width_tiles * (size_t)tnt.height_tiles;
        for (size_t i = 0; i < n; i++) {
            uint16_t v = tnt.feature_layer[i];
            if (v < 0xFFF0u) out->feature_cell_count++;
            else if (v == 0xFFFFu) out->feature_empty_count++;
            else out->feature_continuation_count++;
        }
    }
    TNT_Close(&tnt);
    return 0;
}

static int has_path_prefix_ci(const char *path, const char *prefix) {
    size_t n;
    if (!path || !prefix) return 0;
    n = strlen(prefix);
    return tak_strnicmp(path, prefix, n) == 0;
}

static int validate_map(const MapInspect *m, int strict_skirmish,
                        char *err, size_t err_cap) {
    int expected_w;
    int expected_h;

    if (!m) return -1;
    if (m->size_x <= 0 || m->size_y <= 0) {
        snprintf(err, err_cap, "missing or invalid OTA size");
        return -1;
    }
    expected_w = m->size_x * 32;
    expected_h = m->size_y * 32;
    if (strict_skirmish) {
        /* The OTA size is the tile count in 32-tile blocks, rounded up:
         * shipped Iron Plague maps such as 300x220 are labelled 10x7. */
        int w_ok = m->tnt_width_tiles <= expected_w && m->tnt_width_tiles > expected_w - 32;
        int h_ok = m->tnt_height_tiles <= expected_h && m->tnt_height_tiles > expected_h - 32;
        if (!w_ok || !h_ok) {
            snprintf(err, err_cap, "OTA size %dx%d implies %dx%d tiles, TNT is %dx%d",
                     m->size_x, m->size_y, expected_w, expected_h,
                     m->tnt_width_tiles, m->tnt_height_tiles);
            return -1;
        }
    } else {
        if (m->tnt_width_tiles <= 0 || m->tnt_height_tiles <= 0 ||
            m->tnt_width_tiles > expected_w || m->tnt_height_tiles > expected_h) {
            snprintf(err, err_cap, "campaign TNT %dx%d outside OTA size bucket %dx%d",
                     m->tnt_width_tiles, m->tnt_height_tiles, expected_w, expected_h);
            return -1;
        }
    }
    if (m->blocks_w != m->tnt_width_tiles / 2 ||
        m->blocks_h != m->tnt_height_tiles / 2) {
        snprintf(err, err_cap, "invalid TNT block grid %dx%d for tiles %dx%d",
                 m->blocks_w, m->blocks_h, m->tnt_width_tiles, m->tnt_height_tiles);
        return -1;
    }
    if (m->minimap_w <= 0 || m->minimap_h <= 0 ||
        m->minimap_bg_w <= 0 || m->minimap_bg_h <= 0) {
        snprintf(err, err_cap, "missing minimap data");
        return -1;
    }
    if (strict_skirmish && m->num_players > 0 && m->start_count < m->num_players) {
        snprintf(err, err_cap, "only %d starts for %d players",
                 m->start_count, m->num_players);
        return -1;
    }
    for (int i = 0; i < m->start_count; i++) {
        if (m->starts[i].x < 0 || m->starts[i].z < 0 ||
            m->starts[i].x >= m->tnt_width_tiles ||
            m->starts[i].z >= m->tnt_height_tiles) {
            snprintf(err, err_cap, "start %d out of bounds at %d,%d",
                     m->starts[i].player, m->starts[i].x, m->starts[i].z);
            return -1;
        }
    }
    if (err && err_cap) err[0] = '\0';
    return 0;
}

static int validate_all_with_prefix(const char *label, const char *prefix,
                                    int strict_skirmish) {
    char **paths = NULL;
    int count = 0;
    int checked = 0;
    int failures = 0;

    /* Ask for the prefixed form outright: a root-level "*.ota" only
     * matches loose files, while the VFS maps a loose-style pattern
     * onto the archives' own layout. */
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s*.ota", prefix);
    if (VFS_ListFiles(pattern, &paths, &count) != 0) {
        fprintf(stderr, "map_inspect: VFS_ListFiles('%s') failed\n", pattern);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        if (!has_path_prefix_ci(paths[i], prefix)) {
            tak_free(paths[i]);
            continue;
        }

        MapInspect m;
        char err[256];
        checked++;
        if (inspect_map(paths[i], &m) != 0) {
            fprintf(stderr, "FAIL %s: inspect failed\n", paths[i]);
            failures++;
        } else if (validate_map(&m, strict_skirmish, err, sizeof(err)) != 0) {
            fprintf(stderr, "FAIL %s: %s\n", paths[i], err);
            failures++;
        }
        tak_free(paths[i]);
    }
    tak_free(paths);

    printf("map_inspect %s: checked=%d failures=%d\n",
           label, checked, failures);
    if (checked == 0) return 2;
    return failures == 0 ? 0 : 1;
}

/* Every map a skirmish can choose, wherever it lives: the maps
 * folder in an archive or loose, and the map packs in Maps. */
static int validate_all_maps(void) {
    TAK_MapEntry *entries = NULL;
    int count = 0;
    int checked = 0;
    int failures = 0;

    if (TAK_Maps_Scan(&entries, &count) != 0) {
        fprintf(stderr, "map_inspect: map scan failed\n");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        char ota_path[256];
        MapInspect m;
        char err[256];
        if (TAK_Maps_FindFile(entries[i].key, "ota",
                              ota_path, sizeof(ota_path)) != 0) {
            fprintf(stderr, "FAIL %s: no .ota\n", entries[i].key);
            failures++;
            continue;
        }
        checked++;
        if (inspect_map(ota_path, &m) != 0) {
            fprintf(stderr, "FAIL %s: inspect failed\n", ota_path);
            failures++;
        } else if (validate_map(&m, 1, err, sizeof(err)) != 0) {
            fprintf(stderr, "FAIL %s: %s\n", ota_path, err);
            failures++;
        }
    }
    TAK_Maps_Free(entries);

    printf("map_inspect validate-all: checked=%d failures=%d\n",
           checked, failures);
    if (checked == 0) return 2;
    return failures == 0 ? 0 : 1;
}

static int validate_all_missions(void) {
    return validate_all_with_prefix("validate-missions", "missions/missions/", 0);
}

static int validate_campaign_index(void) {
    TDFFile *tdf = TDF_Open("data/camps/book of darien.tdf");
    char side_copy[64];
    int count = 0;
    int failures = 0;

    if (!tdf || TDF_Load(tdf) != 0) {
        fprintf(stderr, "campaign-index: failed to load book of darien.tdf\n");
        if (tdf) TDF_Close(tdf);
        return 1;
    }
    if (TDF_PushSection(tdf, "HEADER") != 0) {
        fprintf(stderr, "campaign-index: missing HEADER\n");
        TDF_Close(tdf);
        return 1;
    }
    const char *side = TDF_ReadString(tdf, "campaignside", "");
    copy_str(side_copy, sizeof(side_copy), side);
    if (!side_copy[0]) {
        fprintf(stderr, "campaign-index: missing campaignside\n");
        failures++;
    }
    TDF_PopSection(tdf);

    for (int i = 0; i < 256; i++) {
        char section[32];
        char ota_path[256];
        char tnt_path[256];
        char stem[128];
        char err[256];
        MapInspect m;

        snprintf(section, sizeof(section), "MISSION%d", i);
        if (TDF_PushSection(tdf, section) != 0) break;

        const char *mission_file = TDF_ReadString(tdf, "missionfile", "");
        const char *mission_name = TDF_ReadString(tdf, "missionname", "");
        if (!mission_file || !*mission_file || !mission_name || !*mission_name) {
            fprintf(stderr, "campaign-index: %s missing missionfile/missionname\n",
                    section);
            failures++;
            TDF_PopSection(tdf);
            continue;
        }

        strip_extension(mission_file, stem, sizeof(stem));
        if (tak_stricmp(stem, mission_name) != 0) {
            fprintf(stderr, "campaign-index: %s name mismatch file=%s name=%s\n",
                    section, mission_file, mission_name);
            failures++;
        }

        snprintf(ota_path, sizeof(ota_path), "missions/missions/%s", mission_file);
        copy_str(tnt_path, sizeof(tnt_path), ota_path);
        derive_tnt_path(ota_path, tnt_path, sizeof(tnt_path));

        if (VFS_FileExists(ota_path) != 0) {
            fprintf(stderr, "campaign-index: missing %s\n", ota_path);
            failures++;
        } else if (VFS_FileExists(tnt_path) != 0) {
            fprintf(stderr, "campaign-index: missing %s\n", tnt_path);
            failures++;
        } else if (inspect_map(ota_path, &m) != 0) {
            fprintf(stderr, "campaign-index: inspect failed for %s\n", ota_path);
            failures++;
        } else if (validate_map(&m, 0, err, sizeof(err)) != 0) {
            fprintf(stderr, "campaign-index: %s invalid: %s\n", ota_path, err);
            failures++;
        }

        count++;
        TDF_PopSection(tdf);
    }

    TDF_Close(tdf);
    printf("campaign-index: side=%s missions=%d failures=%d\n",
           side_copy, count, failures);
    if (count != 48) return 2;
    return failures == 0 ? 0 : 1;
}

static void print_json_string(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') {
            putchar('\\');
            putchar(*s);
        } else if ((unsigned char)*s < 32) {
            printf("\\u%04x", (unsigned char)*s);
        } else {
            putchar(*s);
        }
    }
    putchar('"');
}

static void print_json(const MapInspect *m) {
    printf("{\n");
    printf("  \"ota\": "); print_json_string(m->ota_path); printf(",\n");
    printf("  \"tntPath\": "); print_json_string(m->tnt_path); printf(",\n");
    printf("  \"missionName\": "); print_json_string(m->mission_name); printf(",\n");
    printf("  \"fingerprint\": "); print_json_string(m->fingerprint); printf(",\n");
    printf("  \"description\": "); print_json_string(m->description); printf(",\n");
    printf("  \"kingdom\": "); print_json_string(m->kingdom); printf(",\n");
    printf("  \"numPlayers\": %d,\n", m->num_players);
    printf("  \"size\": { \"text\": "); print_json_string(m->size_text);
    printf(", \"x\": %d, \"y\": %d },\n", m->size_x, m->size_y);
    printf("  \"tnt\": { \"widthTiles\": %d, \"heightTiles\": %d, "
           "\"blocksW\": %d, \"blocksH\": %d, \"minimapW\": %d, "
           "\"minimapH\": %d, \"minimapBgW\": %d, \"minimapBgH\": %d, "
           "\"featureNames\": %d },\n",
           m->tnt_width_tiles, m->tnt_height_tiles, m->blocks_w, m->blocks_h,
           m->minimap_w, m->minimap_h, m->minimap_bg_w, m->minimap_bg_h,
           m->feature_name_count);
    printf("  \"terrainBytes\": { \"min\": %d, \"max\": %d, "
           "\"distinct\": %d, \"zeroCount\": %d, \"ffCount\": %d },\n",
           m->terrain_byte_min, m->terrain_byte_max,
           m->terrain_byte_distinct, m->terrain_byte_zero_count,
           m->terrain_byte_ff_count);
    printf("  \"derivedHeight\": { \"min\": %d, \"max\": %d, "
           "\"distinct\": %d },\n",
           m->derived_height_min, m->derived_height_max,
           m->derived_height_distinct);
    printf("  \"featureCells\": { \"placed\": %d, \"empty\": %d, "
           "\"continuation\": %d },\n",
           m->feature_cell_count, m->feature_empty_count,
           m->feature_continuation_count);
    printf("  \"starts\": [");
    for (int i = 0; i < m->start_count; i++) {
        if (i) printf(", ");
        printf("{ \"player\": %d, \"x\": %d, \"z\": %d }",
               m->starts[i].player, m->starts[i].x, m->starts[i].z);
    }
    printf("]\n}\n");
}

static int selftest(void) {
    MapInspect m;
    if (inspect_map("maps/Maps/Ground War.ota", &m) != 0) return 1;
    if (strlen(m.fingerprint) != 64) return 10;
    if (m.num_players != 4) return 2;
    if (m.size_x != 5 || m.size_y != 5) return 3;
    if (m.tnt_width_tiles != 160 || m.tnt_height_tiles != 160) return 4;
    if (m.start_count != 4) return 5;
    if (m.minimap_w <= 0 || m.minimap_h <= 0) return 6;
    if (m.terrain_byte_min != 62 || m.terrain_byte_max != 128) return 7;
    if (m.terrain_byte_distinct != 58) return 8;
    if (m.feature_cell_count <= 0 || m.feature_empty_count <= 0) return 9;
    print_json(&m);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = "maps/Maps/Ground War.ota";
    MapInspect m;
    int rc;

    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "map_inspect: VFS init failed\n");
        return 1;
    }

    /* --fingerprint prints one line per map, for diffing two installs. */
    if (argc > 1 && strcmp(argv[1], "--fingerprint") == 0) {
        TAK_MapEntry *entries = NULL;
        int count = 0;
        if (TAK_Maps_Scan(&entries, &count) != 0) {
            VFS_Shutdown();
            return 1;
        }
        for (int i = 0; i < count; i++) {
            uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
            char hex[TAK_MAP_FINGERPRINT_HEX];
            if (TAK_MapFingerprint_FromName(entries[i].key, fp) != 0) {
                printf("%-64s %s\n", "(unreadable)", entries[i].key);
                continue;
            }
            TAK_MapFingerprint_ToHex(fp, hex);
            printf("%s  %s\n", hex, entries[i].key);
        }
        TAK_Maps_Free(entries);
        VFS_Shutdown();
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        rc = selftest();
        VFS_Shutdown();
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "--validate-all") == 0) {
        rc = validate_all_maps();
        VFS_Shutdown();
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "--validate-missions") == 0) {
        rc = validate_all_missions();
        VFS_Shutdown();
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "--validate-campaign-index") == 0) {
        rc = validate_campaign_index();
        VFS_Shutdown();
        return rc;
    }

    if (argc > 1) path = argv[1];
    rc = inspect_map(path, &m);
    if (rc == 0) print_json(&m);
    VFS_Shutdown();
    return rc == 0 ? 0 : 1;
}
