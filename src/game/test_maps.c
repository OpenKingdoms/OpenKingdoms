/*
 * test_maps.c -- which maps the chooser may offer and where their files
 * come from. Fixtures are archives written on the fly, so this needs no
 * game data.
 */

#include "test_framework.h"
#include "test_hpi_builder.h"
#include "tak_maps.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define tak_test_mkdir(p) _mkdir(p)
#define tak_test_rmdir(p) _rmdir(p)
#else
#include <unistd.h>
#include <sys/stat.h>
#define tak_test_mkdir(p) mkdir(p, 0755)
#define tak_test_rmdir(p) rmdir(p)
#endif

static const char *GAME_DIR = "test_maps_tmp";
static const char *MAPS_DIR = "test_maps_tmp/Maps";
static const char *BASE_HPI = "test_maps_tmp/base.hpi";
static const char *MISSIONS_HPI = "test_maps_tmp/missions.hpi";
static const char *PACK_KMP = "test_maps_tmp/Maps/Beta Ridge.kmp";
static const char *CLASH_KMP = "test_maps_tmp/Maps/Contested.kmp";

static void fixture_begin(void) {
    VFS_Shutdown();
    tak_test_mkdir(GAME_DIR);
    tak_test_mkdir(MAPS_DIR);
}

static void fixture_end(void) {
    VFS_Shutdown();
    remove(BASE_HPI);
    remove(MISSIONS_HPI);
    remove(PACK_KMP);
    remove(CLASH_KMP);
    tak_test_rmdir(MAPS_DIR);
    tak_test_rmdir(GAME_DIR);
}

/* The maps folder in an archive and a map pack in Maps are both map
 * sources, and the list is sorted by name. */
TEST(scan_finds_maps_from_the_archive_and_the_pack) {
    fixture_begin();
    TestHPIEntry base[] = {
        { "maps/Alpha Field.ota", "[GlobalHeader]", 100 },
        { "maps/Alpha Field.tnt", "tnt", 100 },
    };
    TestHPIEntry pack[] = { { "kmap/Beta Ridge.ota", "[GlobalHeader]", 200 } };
    int w = test_write_hpi(BASE_HPI, base, 2);
    w |= test_write_hpi(PACK_KMP, pack, 1);
    if (w != 0) { fixture_end(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(GAME_DIR, NULL);
    TAK_MapEntry *maps = NULL;
    int count = -1;
    int scan_rc = TAK_Maps_Scan(&maps, &count);
    char first[96] = "", second[96] = "";
    int first_source = -1, second_source = -1;
    if (count >= 2) {
        snprintf(first, sizeof(first), "%s", maps[0].key);
        snprintf(second, sizeof(second), "%s", maps[1].key);
        first_source = maps[0].source;
        second_source = maps[1].source;
    }
    TAK_Maps_Free(maps);
    fixture_end();

    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(0, scan_rc);
    ASSERT_EQ_INT(2, count);
    ASSERT_EQ_STR("alpha field", first);
    ASSERT_EQ_INT(TAK_MAP_SOURCE_MAPS, first_source);
    ASSERT_EQ_STR("beta ridge", second);
    ASSERT_EQ_INT(TAK_MAP_SOURCE_PACK, second_source);
}

/* Campaign maps live in the missions folder. The original never scans it
 * for the chooser (legacy:167670), so they stay out of the list even
 * though the loader can still find them. */
TEST(scan_never_offers_a_campaign_map) {
    fixture_begin();
    TestHPIEntry base[] = { { "maps/Alpha Field.ota", "[GlobalHeader]", 100 } };
    TestHPIEntry missions[] = {
        { "missions/takmission01_mt.ota", "[GlobalHeader]", 100 },
        { "missions/takmission01_mt.tnt", "tnt", 100 },
    };
    int w = test_write_hpi(BASE_HPI, base, 1);
    w |= test_write_hpi(MISSIONS_HPI, missions, 2);
    if (w != 0) { fixture_end(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(GAME_DIR, NULL);
    TAK_MapEntry *maps = NULL;
    int count = -1;
    TAK_Maps_Scan(&maps, &count);
    int listed_campaign = 0;
    for (int i = 0; i < count; i++) {
        if (tak_stricmp(maps[i].key, "takmission01_mt") == 0) listed_campaign = 1;
    }
    TAK_Maps_Free(maps);

    char found[256];
    int loadable = TAK_Maps_FindFile("takmission01_mt", "tnt", found, sizeof(found));
    fixture_end();

    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(1, count);
    ASSERT_EQ_INT(0, listed_campaign);
    /* Still loadable by name, which is how the campaign reaches it. */
    ASSERT_EQ_INT(0, loadable);
}

/* No cap: a player with hundreds of maps sees all of them. */
TEST(scan_has_no_upper_limit) {
    fixture_begin();
    enum { MANY = 300 };
    static char names[MANY][64];
    static TestHPIEntry entries[MANY];
    for (int i = 0; i < MANY; i++) {
        snprintf(names[i], sizeof(names[i]), "maps/Field %03d.ota", i);
        entries[i].path = names[i];
        entries[i].data = "[GlobalHeader]";
        entries[i].date = 100;
    }
    int w = test_write_hpi(BASE_HPI, entries, MANY);
    if (w != 0) { fixture_end(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(GAME_DIR, NULL);
    TAK_MapEntry *maps = NULL;
    int count = -1;
    TAK_Maps_Scan(&maps, &count);
    TAK_Maps_Free(maps);
    fixture_end();

    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(MANY, count);
}

/* Same name in both sources: the pack wins, the way the original tries
 * Maps\<name>.kmp before Maps\<name>.ota (legacy:168763). */
TEST(a_map_pack_wins_over_the_maps_folder) {
    fixture_begin();
    TestHPIEntry base[] = {
        { "maps/Contested.ota", "[GlobalHeader]", 100 },
        { "maps/Contested.tnt", "from the maps folder", 100 },
    };
    TestHPIEntry pack[] = {
        { "kmap/Contested.ota", "[GlobalHeader]", 100 },
        { "kmap/Contested.tnt", "from the pack", 100 },
    };
    int w = test_write_hpi(BASE_HPI, base, 2);
    w |= test_write_hpi(CLASH_KMP, pack, 2);
    if (w != 0) { fixture_end(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(GAME_DIR, NULL);
    TAK_MapEntry *maps = NULL;
    int count = -1;
    TAK_Maps_Scan(&maps, &count);
    int source = (count == 1) ? maps[0].source : -1;
    TAK_Maps_Free(maps);

    char found[256];
    int find_rc = TAK_Maps_FindFile("contested", "tnt", found, sizeof(found));
    void *data = NULL;
    uint32_t size = 0;
    char got[32] = "";
    if (VFS_ReadFile(found, &data, &size) == 0 && data) {
        int n = (size < sizeof(got) - 1) ? (int)size : (int)sizeof(got) - 1;
        memcpy(got, data, (size_t)n);
        got[n] = '\0';
    }
    VFS_FreeBuffer(data);
    fixture_end();

    ASSERT_EQ_INT(0, rc);
    /* One row per name, as the original keys its list by name. */
    ASSERT_EQ_INT(1, count);
    ASSERT_EQ_INT(TAK_MAP_SOURCE_PACK, source);
    ASSERT_EQ_INT(0, find_rc);
    ASSERT_EQ_STR("kmap/contested.tnt", found);
    ASSERT_EQ_STR("from the pack", got);
}

int main(void) {
    tak_mem_init();
    TEST_SUITE("Map sources");
    RUN(scan_finds_maps_from_the_archive_and_the_pack);
    RUN(scan_never_offers_a_campaign_map);
    RUN(scan_has_no_upper_limit);
    RUN(a_map_pack_wins_over_the_maps_folder);
    TEST_REPORT();
}
