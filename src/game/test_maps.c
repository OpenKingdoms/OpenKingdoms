/*
 * test_maps.c -- which maps the chooser may offer and where their files
 * come from. Fixtures are archives written on the fly, so this needs no
 * game data.
 */

#include "test_framework.h"
#include "test_hpi_builder.h"
#include "tak_maps.h"
#include "tak_map_share.h"
#include "tak_net_protocol.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"
#include "tak_bytes.h"

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
 * for the chooser (legacy:167671), so they stay out of the list even
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
 * Maps\<name>.kmp before Maps\<name>.ota (legacy:168765). */
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

/* ── A map sent from one player to another ──────────────────────────── */

static const char *RECV_DIR = "test_maps_tmp/received";
static const char *RECV_KMP = "test_maps_tmp/received/Player Isle.kmp";
static const char *IP_HPI = "test_maps_tmp/IPData.hpi";
static const char *OWN_KMP = "test_maps_tmp/Maps/Home Made.kmp";

static const char ISLE_OTA[] =
    "[GlobalHeader]\n{\nmissionname=Player Isle;\nnumplayers=2;\n}\n";
static const char ISLE_TNT[] = "the terrain of a map a player made";
static const char ISLE_TNT_2[] = "the same map with its terrain edited";
static const char ISLE_CRT[] = "craters";
static const char ISLE_TXT[] = "Two players, one island.";

static void isle_files(TAK_MapShareFiles *f, const char *tnt) {
    memset(f, 0, sizeof *f);
    f->map.tnt = tnt;      f->map.tnt_size = strlen(tnt);
    f->map.ota = ISLE_OTA; f->map.ota_size = sizeof ISLE_OTA - 1;
    f->map.crt = ISLE_CRT; f->map.crt_size = sizeof ISLE_CRT - 1;
    f->txt = ISLE_TXT;     f->txt_size = sizeof ISLE_TXT - 1;
}

static void share_fixture_end(void) {
    VFS_Shutdown();
    VFS_SetReceivedMapDir(NULL);
    remove(RECV_KMP);
    tak_test_rmdir(RECV_DIR);
    remove(IP_HPI);
    remove(OWN_KMP);
    fixture_end();
}

/* What comes over the wire is checked against the room's fingerprint
 * before anything is kept: the same map reads back whole, and another
 * map, a cut short bundle or one that claims too much is refused. */
TEST(a_sent_map_reads_back_whole_and_any_other_bundle_is_refused) {
    TAK_MapShareFiles f, back;
    isle_files(&f, ISLE_TNT);
    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES], other[TAK_MAP_FINGERPRINT_BYTES];
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromFiles(&f.map, fp));
    TAK_MapShareFiles g;
    isle_files(&g, ISLE_TNT_2);
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromFiles(&g.map, other));

    uint8_t *wire = NULL;
    uint32_t len = 0;
    ASSERT_EQ_INT(0, TAK_MapShare_Pack(&f, &wire, &len));
    ASSERT(len > 8 && len <= TAK_NET_MAP_MAX);
    ASSERT_EQ_INT(0, TAK_MapShare_Unpack(wire, len, fp, &back));
    ASSERT_EQ_INT((int)(sizeof ISLE_TNT - 1), (int)back.map.tnt_size);
    ASSERT(memcmp(back.map.tnt, ISLE_TNT, sizeof ISLE_TNT - 1) == 0);
    ASSERT(memcmp(back.map.crt, ISLE_CRT, sizeof ISLE_CRT - 1) == 0);
    ASSERT(memcmp(back.txt, ISLE_TXT, sizeof ISLE_TXT - 1) == 0);
    ASSERT(back.map.tdf == NULL);
    TAK_MapShare_FreeFiles(&back);

    /* Some other map than the room's. */
    ASSERT_EQ_INT(-2, TAK_MapShare_Unpack(wire, len, other, &back));
    /* Cut short, one byte changed, or claiming more than any map. */
    ASSERT_EQ_INT(-1, TAK_MapShare_Unpack(wire, len - 1, fp, &back));
    wire[len - 3] ^= 0x40;
    ASSERT(TAK_MapShare_Unpack(wire, len, fp, &back) != 0);
    wire[len - 3] ^= 0x40;
    tak_put_u32(wire + 4, TAK_MAP_SHARE_RAW_MAX + 1);
    ASSERT_EQ_INT(-1, TAK_MapShare_Unpack(wire, len, fp, &back));
    tak_put_u32(wire + 4, 12);
    ASSERT_EQ_INT(-1, TAK_MapShare_Unpack(wire, len, fp, &back));
    tak_free(wire);

    /* A map without its terrain is no map to send. */
    f.map.tnt = NULL;
    ASSERT(TAK_MapShare_Pack(&f, &wire, &len) != 0);
    ASSERT(wire == NULL);
}

/* A checked map is kept as a map pack in the player's own folder, read
 * from there at once and after a restart, and a newer one of that name
 * replaces it. */
TEST(a_received_map_is_kept_as_a_pack_and_read_from_it) {
    fixture_begin();
    tak_test_mkdir(RECV_DIR);
    TestHPIEntry base[] = { { "maps/Alpha Field.ota", "[GlobalHeader]", 100 },
                            { "maps/Alpha Field.tnt", "tnt", 100 } };
    int w = test_write_hpi(BASE_HPI, base, 2);
    if (w != 0) { share_fixture_end(); ASSERT_EQ_INT(0, w); return; }
    VFS_SetReceivedMapDir(RECV_DIR);
    ASSERT_EQ_INT(0, VFS_Init(GAME_DIR, NULL));

    TAK_MapShareFiles f;
    isle_files(&f, ISLE_TNT);
    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES], got[TAK_MAP_FINGERPRINT_BYTES];
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromFiles(&f.map, fp));
    ASSERT(TAK_MapFingerprint_FromName("Player Isle", got) != 0);
    ASSERT_EQ_INT(0, TAK_MapShare_Keep("test_maps_tmp/received/", "Player Isle", &f));
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromName("Player Isle", got));
    ASSERT_EQ_INT(0, memcmp(fp, got, sizeof fp));
    char path[256];
    ASSERT_EQ_INT(0, TAK_Maps_FindFile("Player Isle", "ota", path, sizeof path));
    ASSERT_EQ_INT(VFS_ORIGIN_RECEIVED, VFS_FileOrigin(path, NULL, 0));
    ASSERT_EQ_INT(0, TAK_MapShare_HasOwn("Player Isle"));
    ASSERT_EQ_INT(1, TAK_MapShare_HasOwn("Alpha Field"));

    /* The host edited it since: the new copy goes over the old one. */
    isle_files(&f, ISLE_TNT_2);
    uint8_t fp2[TAK_MAP_FINGERPRINT_BYTES];
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromFiles(&f.map, fp2));
    ASSERT_EQ_INT(0, TAK_MapShare_Keep("test_maps_tmp/received/", "Player Isle", &f));
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromName("Player Isle", got));
    ASSERT_EQ_INT(0, memcmp(fp2, got, sizeof fp2));

    /* And it is there after a restart, listed with the rest. */
    VFS_Shutdown();
    ASSERT_EQ_INT(0, VFS_Init(GAME_DIR, NULL));
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromName("Player Isle", got));
    ASSERT_EQ_INT(0, memcmp(fp2, got, sizeof fp2));
    TAK_MapEntry *maps = NULL;
    int count = 0;
    TAK_Maps_Scan(&maps, &count);
    int listed = 0;
    for (int i = 0; i < count; i++) if (tak_stricmp(maps[i].key, "player isle") == 0) listed = 1;
    TAK_Maps_Free(maps);
    ASSERT_EQ_INT(1, listed);
    share_fixture_end();
}

static void from_hex(const char *hex, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(hex + i * 2, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* Only a map a player made is offered. One out of the game's own
 * archives, or one the original's releases ship, is named instead. */
TEST(only_a_map_a_player_made_is_offered) {
    fixture_begin();
    TestHPIEntry base[] = { { "maps/Alpha Field.ota", "[GlobalHeader]", 100 },
                            { "maps/Alpha Field.tnt", "base tnt", 100 } };
    TestHPIEntry ip[] = { { "maps/Creon Pass.ota", "[GlobalHeader]", 100 },
                          { "maps/Creon Pass.tnt", "ip tnt", 100 } };
    TestHPIEntry own[] = { { "kmap/Home Made.ota", "[GlobalHeader]\n{\nnumplayers=2;\n}\n", 100 },
                           { "kmap/Home Made.tnt", "a player's terrain", 100 } };
    int w = test_write_hpi(BASE_HPI, base, 2);
    w |= test_write_hpi(IP_HPI, ip, 2);
    w |= test_write_hpi(OWN_KMP, own, 2);
    if (w != 0) { share_fixture_end(); ASSERT_EQ_INT(0, w); return; }
    ASSERT_EQ_INT(0, VFS_Init(GAME_DIR, NULL));

    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromName("Alpha Field", fp));
    ASSERT_EQ_INT(TAK_MAPSHARE_GAME, TAK_MapShare_Status("Alpha Field", fp, NULL, NULL));
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromName("Creon Pass", fp));
    ASSERT_EQ_INT(TAK_MAPSHARE_EXPANSION, TAK_MapShare_Status("Creon Pass", fp, NULL, NULL));
    ASSERT_EQ_INT(0, TAK_MapFingerprint_FromName("Home Made", fp));
    uint8_t *bundle = NULL;
    uint32_t len = 0;
    ASSERT_EQ_INT(TAK_MAPSHARE_OFFERED, TAK_MapShare_Status("Home Made", fp, &bundle, &len));
    ASSERT(bundle != NULL);
    TAK_MapShareFiles back;
    ASSERT_EQ_INT(0, TAK_MapShare_Unpack(bundle, len, fp, &back));
    TAK_MapShare_FreeFiles(&back);
    tak_free(bundle);
    /* A print that is not this copy's cannot be sent as it. */
    fp[0] ^= 1;
    ASSERT_EQ_INT(TAK_MAPSHARE_UNREADABLE, TAK_MapShare_Status("Home Made", fp, NULL, NULL));
    share_fixture_end();

    /* By fingerprint, wherever a copy came from: the game's Ground War,
     * the expansion's Rival Hill, Cavedog's Adamantine Gate pack. */
    from_hex("f0aa1ca52036d8667cfcceb9198bc47535ceac59099eac86ea1befaa7533815a", fp, 32);
    ASSERT_EQ_INT(TAK_MAPSHARE_GAME, TAK_MapShare_Shipped(fp));
    from_hex("ddaff3d4566577e415d710851e67e6d5adf86a94d5e867f0a41743f1b91ac001", fp, 32);
    ASSERT_EQ_INT(TAK_MAPSHARE_EXPANSION, TAK_MapShare_Shipped(fp));
    from_hex("fce18a0df3de36a51a703badcb001720b2dc0287109d8777f255a0408ddf8ea9", fp, 32);
    ASSERT_EQ_INT(TAK_MAPSHARE_MAP_PACK, TAK_MapShare_Shipped(fp));
    fp[5] ^= 1;
    ASSERT_EQ_INT(0, TAK_MapShare_Shipped(fp));
}

/* A received map is written under the room's map name, so only a name
 * that is a plain file name is taken. */
TEST(a_map_name_that_cannot_be_a_plain_file_name_is_refused) {
    ASSERT_EQ_INT(1, TAK_MapShare_NameOk("Aibel's Seaport"));
    ASSERT_EQ_INT(1, TAK_MapShare_NameOk("Dan'gel (2), the return!"));
    ASSERT_EQ_INT(1, TAK_MapShare_NameOk("Console Wars"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk(""));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("../escape"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("a/b"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("a\\b"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("c:map"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk(".hidden"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("trailing "));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("con"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("NUL.txt"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("com1"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("LPT9 map"));
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk("caf\xc3\xa9"));
    char longest[TAK_NET_MAP_NAME_MAX + 1];
    memset(longest, 'a', sizeof longest - 1);
    longest[sizeof longest - 1] = '\0';
    ASSERT_EQ_INT(0, TAK_MapShare_NameOk(longest));
    longest[TAK_NET_MAP_NAME_MAX - 1] = '\0';
    ASSERT_EQ_INT(1, TAK_MapShare_NameOk(longest));
}

int main(void) {
    tak_mem_init();
    TEST_SUITE("Map sources");
    RUN(scan_finds_maps_from_the_archive_and_the_pack);
    RUN(scan_never_offers_a_campaign_map);
    RUN(scan_has_no_upper_limit);
    RUN(a_map_pack_wins_over_the_maps_folder);
    TEST_SUITE("Maps sent between players");
    RUN(a_sent_map_reads_back_whole_and_any_other_bundle_is_refused);
    RUN(a_received_map_is_kept_as_a_pack_and_read_from_it);
    RUN(only_a_map_a_player_made_is_offered);
    RUN(a_map_name_that_cannot_be_a_plain_file_name_is_refused);
    TEST_REPORT();
}
