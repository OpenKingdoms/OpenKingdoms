#include "test_framework.h"
#include "test_hpi_builder.h"
#include "tak_maps.h"
#include "tak_tdf.h"
#include "tak_tnt.h"
#include "tak_palette.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* A map known to ship with TAK — has a real minimap and is the first map
 * most people ever look at. Used as the canonical "known good" fixture. */
#define KNOWN_GOOD_TNT   "maps/Maps/Ground War.tnt"
#define KNOWN_GOOD_W     160   /* from OTA: size=5x5, 5*32 = 160 tiles */
#define KNOWN_GOOD_H     160
#define EXPECTED_MM_W    126   /* verified empirically on TAK 0x4000 files */
#define EXPECTED_MM_H    126

static int vfs_ready = 0;
static uint32_t g_rgba_table[256];
static int g_rgba_ready = 0;

static void ensure_vfs(void) {
    if (!vfs_ready) {
        tak_mem_init();
        VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR);
        vfs_ready = 1;
    }
}

static const uint32_t *ensure_rgba_table(void) {
    ensure_vfs();
    if (!g_rgba_ready) {
        Palette pal;
        if (Palette_Load(&pal, "data/palettes/palette.pal") == 0) {
            for (int i = 0; i < 256; i++) {
                uint8_t r = pal.entries[i].r, g = pal.entries[i].g, b = pal.entries[i].b;
                g_rgba_table[i] = ((uint32_t)r) | ((uint32_t)g << 8) |
                                  ((uint32_t)b << 16) | (0xFFu << 24);
            }
        } else {
            for (int i = 0; i < 256; i++) g_rgba_table[i] = 0xFF000000u | i;
        }
        g_rgba_ready = 1;
    }
    return g_rgba_table;
}

/* ── Basic loading ───────────────────────────────────────────────────── */

TEST(load_returns_zero_on_success) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    int rc = TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba);
    ASSERT_EQ_INT(0, rc);
    TNT_Close(&tnt);
}

TEST(load_returns_negative_on_missing_file) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    int rc = TNT_Load(&tnt, "maps/Maps/DOES_NOT_EXIST.tnt", rgba);
    ASSERT(rc < 0);
}

TEST(load_returns_negative_on_null_out) {
    const uint32_t *rgba = ensure_rgba_table();
    int rc = TNT_Load(NULL, KNOWN_GOOD_TNT, rgba);
    ASSERT(rc < 0);
}

TEST(load_returns_negative_on_null_path) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    int rc = TNT_Load(&tnt, NULL, rgba);
    ASSERT(rc < 0);
}

TEST(load_returns_negative_on_null_rgba_table) {
    ensure_vfs();
    TNTFile tnt;
    int rc = TNT_Load(&tnt, KNOWN_GOOD_TNT, NULL);
    ASSERT(rc < 0);
}

/* ── Header field extraction ─────────────────────────────────────────── */

TEST(load_width_height_matches_ground_war) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    ASSERT_EQ_INT(KNOWN_GOOD_W, tnt.width_tiles);
    ASSERT_EQ_INT(KNOWN_GOOD_H, tnt.height_tiles);
    TNT_Close(&tnt);
}

/* ── Per-map layout regression tests ─────────────────────────────────
 * These are "grounded in the original data" tests — the expected values
 * come from hex-dumping the shipped TNT headers. If the loader's offsets
 * drift, these fire loudly. Picked for variety: different sizes, different
 * aspect ratios, different sea levels, different kingdoms. */

static void assert_layout(const char *vfs_path, int W, int H, int sea_level) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    int rc = TNT_Load(&tnt, vfs_path, rgba);
    if (rc != 0) {
        printf("FAIL: %s rc=%d\n", vfs_path, rc);
        ASSERT_EQ_INT(0, rc);
    }
    ASSERT_EQ_INT(W, tnt.width_tiles);
    ASSERT_EQ_INT(H, tnt.height_tiles);
    ASSERT_EQ_INT(sea_level, tnt.sea_level);
    /* Pointer-trust checks: every verified field must be non-NULL after
     * a successful load on a well-formed map. */
    ASSERT(tnt.tile_map          != NULL);
    ASSERT(tnt.heightmap         != NULL);
    ASSERT_EQ_INT(W + 1, tnt.height_w);
    ASSERT_EQ_INT(H + 1, tnt.height_h);
    ASSERT(tnt.feature_layer     != NULL);
    ASSERT(tnt.ingame_minimap_bg != NULL);
    ASSERT(tnt.minimap_rgba      != NULL);
    ASSERT(tnt.minimap_bg_w > 0 && tnt.minimap_bg_h > 0);
    /* Per-block render arrays: each block covers a 64x64-px (2x2-tile)
     * region of the map. All three parallel arrays must be populated
     * and the first chunk_id must look plausible (not sentinels). */
    ASSERT_EQ_INT(W / 2, tnt.blocks_w);
    ASSERT_EQ_INT(H / 2, tnt.blocks_h);
    ASSERT(tnt.block_chunk_ids != NULL);
    ASSERT(tnt.block_tex_x     != NULL);
    ASSERT(tnt.block_tex_y     != NULL);
    ASSERT(tnt.block_chunk_ids[0] != 0xFFFFFFFF);
    /* tex_x, tex_y for a 512x512 chunk are in 0..7. Larger values
     * signal a parse offset bug. */
    ASSERT(tnt.block_tex_x[0] <= 15);
    ASSERT(tnt.block_tex_y[0] <= 15);
    TNT_Close(&tnt);
}

TEST(layout_ground_war) {
    /* 160x160 square, sea level 58, kingdom=veruna. */
    assert_layout("maps/Maps/Ground War.tnt", 160, 160, 58);
}

TEST(layout_castle) {
    /* 544x544 square, sea level 40, kingdom=aramon. The biggest
     * shipped map, so it exercises the bounds checks at scale. */
    assert_layout("maps/Maps/CASTLE.tnt", 544, 544, 40);
}

TEST(layout_muntils_ford_guard) {
    /* 352x224 non-square, sea level 58. Aspect-ratio edge case, so
     * it proves W and H aren't being swapped anywhere. */
    assert_layout("maps/Maps/Muntil's Ford Guard.tnt", 352, 224, 58);
}

TEST(layout_takmission01) {
    /* 192x192 campaign mission, sea level 55 where its world says 40.
     * Lives under missions/missions/, not maps/Maps/, so it also
     * confirms the VFS path resolution generalizes. */
    assert_layout("missions/missions/takmission01_mt.tnt", 192, 192, 55);
}

/* ── Per-block render arrays (0x20 / 0x24 / 0x28) ────────────────────
 * The three parallel arrays that drive terrain rendering. Each block
 * is 2x2 tiles = 64x64 pixels. Count per array = (W/2) × (H/2).
 * Verified against the legacy UnitManager_UpdateAll and a raw hex
 * dump of Ground War. */

TEST(block_arrays_ground_war_counts) {
    /* 160/2 = 80 blocks per axis -> 6400 blocks. */
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, "maps/Maps/Ground War.tnt", rgba));
    ASSERT_EQ_INT(80, tnt.blocks_w);
    ASSERT_EQ_INT(80, tnt.blocks_h);
    ASSERT(tnt.block_chunk_ids != NULL);
    ASSERT(tnt.block_tex_x     != NULL);
    ASSERT(tnt.block_tex_y     != NULL);
    TNT_Close(&tnt);
}

TEST(block_arrays_castle_counts) {
    /* 544/2 = 272 blocks per axis -> 73984 blocks, the largest map. */
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, "maps/Maps/CASTLE.tnt", rgba));
    ASSERT_EQ_INT(272, tnt.blocks_w);
    ASSERT_EQ_INT(272, tnt.blocks_h);
    ASSERT(tnt.block_chunk_ids != NULL);
    TNT_Close(&tnt);
}

TEST(block_arrays_non_square_aspect) {
    /* Muntil's Ford Guard: 352/2 × 224/2 = 176 × 112 blocks.
     * Confirms W and H are not being swapped. */
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, "maps/Maps/Muntil's Ford Guard.tnt", rgba));
    ASSERT_EQ_INT(176, tnt.blocks_w);
    ASSERT_EQ_INT(112, tnt.blocks_h);
    ASSERT(tnt.block_chunk_ids != NULL);
    TNT_Close(&tnt);
}

TEST(block_tex_coords_ground_war_first_row) {
    /* Ground War's first-row blocks cycle tex_x = 0..7 per chunk-row,
     * tex_y = 0 across the entire top row. Confirmed against a hex
     * dump of the 0x24/0x28 arrays. This catches stride / offset
     * bugs in parsing. */
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, "maps/Maps/Ground War.tnt", rgba));
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_INT(i, (int)tnt.block_tex_x[i]);
        ASSERT_EQ_INT(0, (int)tnt.block_tex_y[i]);
    }
    TNT_Close(&tnt);
}

TEST(block_chunk_ids_have_variety) {
    /* A real map has at least a few distinct chunk_ids. Uniform memory
     * would indicate a wrong offset. */
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, "maps/Maps/CASTLE.tnt", rgba));
    int n = tnt.blocks_w * tnt.blocks_h;
    uint32_t seen[3] = {0};
    int distinct = 0;
    for (int i = 0; i < n && distinct < 3; i++) {
        uint32_t id = tnt.block_chunk_ids[i];
        int is_new = 1;
        for (int j = 0; j < distinct; j++) if (seen[j] == id) { is_new = 0; break; }
        if (is_new) seen[distinct++] = id;
    }
    ASSERT(distinct >= 3);
    TNT_Close(&tnt);
}

/* ── In-game minimap background (0x30 region) ────────────────────────
 * The 0x30 header field points to a pre-rendered map overview image used
 * as the in-battle minimap widget backdrop. Dimensions are authored
 * per-map; across shipped maps width varies (396..449) and height is
 * consistently 431. */

TEST(ingame_minimap_bg_is_populated) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    ASSERT(tnt.ingame_minimap_bg != NULL);
    ASSERT(tnt.minimap_bg_w > 0);
    ASSERT(tnt.minimap_bg_h > 0);
    TNT_Close(&tnt);
}

TEST(ingame_minimap_bg_has_nonzero_bytes) {
    /* A real overview image has pixel variety; uniform all-zero would
     * indicate we read the wrong offset or a 0-filled region. */
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    size_t n = (size_t)tnt.minimap_bg_w * (size_t)tnt.minimap_bg_h;
    int nonzero = 0;
    for (size_t i = 0; i < n && i < 8192; i++) {
        if (tnt.ingame_minimap_bg[i] != 0) nonzero++;
    }
    ASSERT(nonzero > 100);
    TNT_Close(&tnt);
}

/* ── Cross-map layout sanity ─────────────────────────────────────────
 * Iterate every shipped TNT and assert the new fields are populated
 * and their sizes fit within the raw buffer. Catches regressions if a
 * future header change breaks one map. */

/* Water sits at the height the map stores, so every map has to agree
 * with the world it belongs to or the change would move somebody's
 * coastline. It does, for every map in the base game, the V2 pack and
 * the 181 map packs. Sixteen Iron Plague maps authored their own sea
 * level and are named here, because for those the water does move, to
 * where the map says it should be. */
static const char *const sea_level_exceptions[] = {
    "alkhest quadrille", "black heart jungle", "crusader's keep",
    "haunted waterworks", "islands of the mer warrior", "isle of palms",
    "lake cuhmoniwanakilya", "lost lake", "moka's fingers",
    "no zhonian is an island", "nobia's temple", "rival hill",
    "sand river plain", "the drafis bridge", "the gardens of atys",
    "ulasem arena",
};

static int side_water_height(const char *kingdom) {
    if (!kingdom || !kingdom[0]) return -1;
    const char *paths[] = { "data/gamedata/sidedata.tdf", "gamedata/sidedata.tdf" };
    for (int p = 0; p < 2; p++) {
        TDFFile *tdf = TDF_Open(paths[p]);
        if (!tdf || TDF_Load(tdf) != 0) { if (tdf) TDF_Close(tdf); continue; }
        for (int i = 0; i < 8; i++) {
            char section[16];
            snprintf(section, sizeof(section), "SIDE%d", i);
            if (TDF_PushSection(tdf, section) != 0) continue;
            const char *name = TDF_ReadString(tdf, "name", "");
            if (name && tak_stricmp(name, kingdom) == 0) {
                int h = TDF_ReadInt(tdf, "waterheight", -1);
                TDF_Close(tdf);
                return h;
            }
            TDF_PopSection(tdf);
        }
        TDF_Close(tdf);
    }
    return -1;
}

static int map_kingdom(const char *key, char *out, size_t cap) {
    char path[256];
    if (TAK_Maps_FindFile(key, "ota", path, sizeof(path)) != 0) return -1;
    TDFFile *tdf = TDF_Open(path);
    if (!tdf || TDF_Load(tdf) != 0) { if (tdf) TDF_Close(tdf); return -1; }
    int rc = -1;
    if (TDF_PushSection(tdf, "GlobalHeader") == 0) {
        const char *k = TDF_ReadString(tdf, "kingdom", "");
        if (k && k[0]) { snprintf(out, cap, "%s", k); rc = 0; }
    }
    TDF_Close(tdf);
    return rc;
}

TEST(every_map_sea_level_agrees_with_its_world) {
    const uint32_t *rgba = ensure_rgba_table();
    TAK_MapEntry *maps = NULL;
    int count = 0;
    if (TAK_Maps_Scan(&maps, &count) != 0 || count == 0) {
        TAK_Maps_Free(maps);
        SKIP("no data dir");
    }
    int checked = 0, moved = 0, unexpected = 0;
    for (int i = 0; i < count; i++) {
        char kingdom[64] = "";
        char tnt_path[256];
        if (map_kingdom(maps[i].key, kingdom, sizeof(kingdom)) != 0) continue;
        int side = side_water_height(kingdom);
        if (side < 0) continue;
        if (TAK_Maps_FindFile(maps[i].key, "tnt", tnt_path, sizeof(tnt_path)) != 0) continue;
        TNTFile tnt;
        if (TNT_Load(&tnt, tnt_path, rgba) != 0) continue;
        checked++;
        if (tnt.sea_level != side) {
            moved++;
            int known = 0;
            for (size_t k = 0; k < sizeof(sea_level_exceptions) /
                                   sizeof(sea_level_exceptions[0]); k++) {
                if (tak_stricmp(maps[i].key, sea_level_exceptions[k]) == 0) known = 1;
            }
            if (!known) {
                printf("\n    %s sea %d, %s water %d", maps[i].key,
                       tnt.sea_level, kingdom, side);
                unexpected++;
            }
        }
        TNT_Close(&tnt);
    }
    TAK_Maps_Free(maps);
    ASSERT_EQ_INT(236, checked);
    ASSERT_EQ_INT(0, unexpected);
    ASSERT_EQ_INT(16, moved);
}

TEST(every_tnt_has_all_pointers_populated) {
    const uint32_t *rgba = ensure_rgba_table();
    int tried = 0, missing = 0;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s/maps/Maps/*.tnt", TAK_DATA_DIR);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("(no data dir) "); return; }
    do {
        char vfs_path[512];
        snprintf(vfs_path, sizeof(vfs_path), "maps/Maps/%s", fd.cFileName);
        TNTFile tnt;
        tried++;
        if (TNT_Load(&tnt, vfs_path, rgba) == 0) {
            if (!tnt.tile_map || !tnt.feature_layer || !tnt.ingame_minimap_bg) {
                printf("\n    missing pointer in %s (map=%p feat=%p mm_bg=%p)",
                        vfs_path,
                        (void*)tnt.tile_map, (void*)tnt.feature_layer,
                        (void*)tnt.ingame_minimap_bg);
                missing++;
            }
        }
        TNT_Close(&tnt);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    ASSERT(tried > 0);
    ASSERT_EQ_INT(0, missing);
}

/* ── Minimap extraction ──────────────────────────────────────────────── */

TEST(load_minimap_dims_are_126x126) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    ASSERT_EQ_INT(EXPECTED_MM_W, tnt.minimap_w);
    ASSERT_EQ_INT(EXPECTED_MM_H, tnt.minimap_h);
    TNT_Close(&tnt);
}

TEST(load_minimap_rgba_is_allocated) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    ASSERT(tnt.minimap_rgba != NULL);
    TNT_Close(&tnt);
}

TEST(load_minimap_has_nonzero_pixels) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    size_t n = (size_t)tnt.minimap_w * tnt.minimap_h;
    int nonzero = 0;
    for (size_t i = 0; i < n; i++) {
        /* alpha is always set in the table, so "all zero" would mean
         * the loop never ran or all indices decoded to 0x00000000. */
        if ((tnt.minimap_rgba[i] & 0x00FFFFFF) != 0) nonzero++;
    }
    ASSERT(nonzero > 100); /* a real minimap has tons of varied color */
    TNT_Close(&tnt);
}

TEST(load_minimap_has_variety) {
    /* A real minimap has multiple distinct colors. If we're reading the
     * wrong offset and getting a uniform region (like the 0-filled block
     * at the old bogus 0x28 offset), variety will be 0 or 1. */
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    size_t n = (size_t)tnt.minimap_w * tnt.minimap_h;
    uint32_t first = tnt.minimap_rgba[0];
    int distinct = 0;
    for (size_t i = 0; i < n; i++) {
        if (tnt.minimap_rgba[i] != first) distinct++;
    }
    ASSERT(distinct > 1000);
    TNT_Close(&tnt);
}

/* ── Close behavior ──────────────────────────────────────────────────── */

TEST(close_zeroes_struct) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    TNT_Close(&tnt);
    ASSERT(tnt.minimap_rgba == NULL);
    ASSERT(tnt.raw == NULL);
    ASSERT_EQ_INT(0, tnt.width_tiles);
    ASSERT_EQ_INT(0, tnt.height_tiles);
    ASSERT_EQ_INT(0, tnt.minimap_w);
    ASSERT_EQ_INT(0, tnt.minimap_h);
}

TEST(double_close_is_safe) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    TNT_Close(&tnt);
    TNT_Close(&tnt); /* must not double-free */
    ASSERT(tnt.minimap_rgba == NULL);
}

TEST(close_of_null_is_safe) {
    TNT_Close(NULL); /* must not crash */
    ASSERT(1);
}

TEST(reload_after_close_works) {
    const uint32_t *rgba = ensure_rgba_table();
    TNTFile tnt;
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    TNT_Close(&tnt);
    ASSERT_EQ_INT(0, TNT_Load(&tnt, KNOWN_GOOD_TNT, rgba));
    ASSERT(tnt.minimap_rgba != NULL);
    TNT_Close(&tnt);
}

/* ── Cross-map sanity — every shipped TNT loads ──────────────────────── */

/* Whether a loose .tnt exists under dir. The sweep below needs one, and
 * main asks the same question before it decides whether a skip from that
 * sweep is a problem or just this tree's shape. */
static int dir_has_any_tnt(const char *dir) {
#ifdef _WIN32
    char pattern[512];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*.tnt", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcmp(dot, ".tnt") == 0) { found = 1; break; }
    }
    closedir(d);
    return found;
#endif
}

static int load_every_tnt_in_dir(const char *dir, const uint32_t *rgba,
                                  int *out_tried, int *out_ok) {
    int tried = 0, ok = 0;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.tnt", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char vfs_path[512];
        /* strip ".tnt" extension check already handled by glob */
        snprintf(vfs_path, sizeof(vfs_path), "maps/Maps/%s", fd.cFileName);
        TNTFile tnt;
        tried++;
        if (TNT_Load(&tnt, vfs_path, rgba) == 0 && tnt.minimap_rgba != NULL) {
            ok++;
        }
        TNT_Close(&tnt);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 4 || strcasecmp(ent->d_name + n - 4, ".tnt") != 0) continue;
        char vfs_path[512];
        snprintf(vfs_path, sizeof(vfs_path), "maps/Maps/%s", ent->d_name);
        TNTFile tnt;
        tried++;
        if (TNT_Load(&tnt, vfs_path, rgba) == 0 && tnt.minimap_rgba != NULL) {
            ok++;
        }
        TNT_Close(&tnt);
    }
    closedir(d);
#endif
    *out_tried = tried;
    *out_ok = ok;
    return 1;
}

TEST(every_shipped_tnt_loads_with_minimap) {
    const uint32_t *rgba = ensure_rgba_table();
    int tried = 0, ok = 0;
    int found_dir = load_every_tnt_in_dir(
        TAK_DATA_DIR "/maps/Maps", rgba, &tried, &ok);
    if (!found_dir) {
        SKIP_MARK("no %s/maps/Maps dir", TAK_DATA_DIR);
        return;
    }
    /* We don't demand 100% because some maps can be malformed, but the
     * vast majority should succeed with a real minimap. */
    ASSERT(tried > 0);
    ASSERT(ok >= tried - 2);
}

/* ── main ────────────────────────────────────────────────────────────── */

/* A map pack can come from anywhere, so a .tnt too short to hold a
 * header has to be refused rather than read past the end of it. */
TEST(a_stub_tnt_is_refused) {
    const uint32_t *rgba = ensure_rgba_table();
    VFS_Shutdown();
    vfs_ready = 0;
#ifdef _WIN32
    _mkdir("test_tnt_stub_tmp");
    _mkdir("test_tnt_stub_tmp/Maps");
#else
    mkdir("test_tnt_stub_tmp", 0755);
    mkdir("test_tnt_stub_tmp/Maps", 0755);
#endif
    TestHPIEntry base[] = { { "gamedata/sidedata.tdf", "sides", 1, 0 } };
    /* The right magic and nothing else, so only a length check can
     * stop the loader reading past the end of it. */
    static const char stub_tnt[4] = { 0x00, 0x40, 0x00, 0x00 };
    TestHPIEntry pack[] = {
        { "kmap/Stub.ota", "[GlobalHeader]", 1, 0 },
        { "kmap/Stub.tnt", stub_tnt, 1, sizeof(stub_tnt) },
    };
    int w = test_write_hpi("test_tnt_stub_tmp/base.hpi", base, 1);
    w |= test_write_hpi("test_tnt_stub_tmp/Maps/Stub.kmp", pack, 2);
    int rc = -1;
    if (w == 0 && VFS_Init("test_tnt_stub_tmp", NULL) == 0) {
        TNTFile tnt;
        rc = TNT_Load(&tnt, "kmap/Stub.tnt", rgba);
        TNT_Close(&tnt);
        VFS_Shutdown();
    }
    remove("test_tnt_stub_tmp/base.hpi");
    remove("test_tnt_stub_tmp/Maps/Stub.kmp");
#ifdef _WIN32
    _rmdir("test_tnt_stub_tmp/Maps");
    _rmdir("test_tnt_stub_tmp");
#else
    rmdir("test_tnt_stub_tmp/Maps");
    rmdir("test_tnt_stub_tmp");
#endif
    ASSERT_EQ_INT(0, w);
    ASSERT(rc < 0);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* every_shipped_tnt_loads_with_minimap walks loose .tnt files on
     * disk, so it has nothing to read in a tree pointed at an archives
     * only data directory. Say that up front: the case still reports
     * SKIP and is named in the summary, it just does not fail a run that
     * was never going to be able to do it. */
    if (!dir_has_any_tnt(TAK_DATA_DIR "/maps/Maps")) {
        TEST_ALLOW_SKIPS("TAK_DATA_DIR has no loose maps/Maps, so the "
                         "shipped TNT sweep has nothing to walk");
    }

    TEST_SUITE("TNT_Load basic");
    RUN(load_returns_zero_on_success);
    RUN(load_returns_negative_on_missing_file);
    RUN(load_returns_negative_on_null_out);
    RUN(load_returns_negative_on_null_path);
    RUN(load_returns_negative_on_null_rgba_table);

    TEST_SUITE("TNT_Load header fields");
    RUN(load_width_height_matches_ground_war);

    TEST_SUITE("Per-map layout regression");
    RUN(layout_ground_war);
    RUN(layout_castle);
    RUN(layout_muntils_ford_guard);
    RUN(layout_takmission01);

    TEST_SUITE("Cross-map layout sanity");
    RUN(every_map_sea_level_agrees_with_its_world);
    RUN(a_stub_tnt_is_refused);
    RUN(every_tnt_has_all_pointers_populated);

    TEST_SUITE("TNT_Load per-block render arrays");
    RUN(block_arrays_ground_war_counts);
    RUN(block_arrays_castle_counts);
    RUN(block_arrays_non_square_aspect);
    RUN(block_tex_coords_ground_war_first_row);
    RUN(block_chunk_ids_have_variety);

    TEST_SUITE("TNT_Load in-game minimap background");
    RUN(ingame_minimap_bg_is_populated);
    RUN(ingame_minimap_bg_has_nonzero_bytes);

    TEST_SUITE("TNT_Load minimap");
    RUN(load_minimap_dims_are_126x126);
    RUN(load_minimap_rgba_is_allocated);
    RUN(load_minimap_has_nonzero_pixels);
    RUN(load_minimap_has_variety);

    TEST_SUITE("TNT_Close");
    RUN(close_zeroes_struct);
    RUN(double_close_is_safe);
    RUN(close_of_null_is_safe);
    RUN(reload_after_close_works);

    TEST_SUITE("Cross-map sanity");
    RUN(every_shipped_tnt_loads_with_minimap);

    if (vfs_ready) VFS_Shutdown();
    tak_mem_shutdown();

    TEST_REPORT();
}
