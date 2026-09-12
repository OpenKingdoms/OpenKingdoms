/*
 * test_map_fingerprint.c -- the map content fingerprint.
 *
 * The cases that build maps out of literals need no game data and run in
 * CI. The ones that read shipped maps skip when there is no install.
 * The same file is compiled for the browser by
 * scripts/fingerprint-wasm-check.sh, which is how the golden values below
 * are held to the same answer on every target.
 */

#include "test_framework.h"
#include "tak_map_fingerprint.h"
#include "tak_maps.h"
#include "tak_sha256.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* ── Fixtures ───────────────────────────────────────────────────────── */

static const char SAMPLE_TNT[] =
    "\x00\x40\x00\x00" "\x20\x00\x00\x00" "\x20\x00\x00\x00" "\x28\x00\x00\x00"
    "height bytes and the rest of a map";
#define SAMPLE_TNT_LEN (sizeof(SAMPLE_TNT) - 1)

static const char SAMPLE_OTA[] =
    "[GlobalHeader]\n"
    "{\n"
    "missionname=Sample Field;\n"
    "missiondescription=6 x 6, 2 players;\n"
    "kingdom=aramon;\n"
    "numplayers=2;\n"
    "size=6 x 6;\n"
    "[Map Data]\n"
    "{\n"
    "Type=Network 1;\n"
    "[specials]\n"
    "{\n"
    "[special0]\n"
    "{\n"
    "specialwhat=StartPos1;\n"
    "XPos=113;\n"
    "ZPos=20;\n"
    "}\n"
    "[special1]\n"
    "{\n"
    "specialwhat=StartPos2;\n"
    "XPos=60;\n"
    "ZPos=153;\n"
    "}\n"
    "}\n"
    "}\n"
    "}\n";

/* The same map with a different name and blurb, different spacing, a
 * comment, upper case key names and its keys in another order. */
static const char SAMPLE_OTA_REDRESSED[] =
    "[globalheader]\r\n"
    "{\r\n"
    "// authored by somebody else\r\n"
    "  SIZE = 6 x 6 ;\r\n"
    "  NumPlayers=2;\r\n"
    "  Kingdom = aramon;\r\n"
    "  MissionName=A Different Label;\r\n"
    "  MissionDescription=Anything at all;\r\n"
    "[Map Data]\r\n"
    "{\r\n"
    "\tTYPE=Network 1;\r\n"
    "[SPECIALS]\r\n"
    "{\r\n"
    "[Special0]\r\n"
    "{\r\n"
    "ZPos = 20;\r\n"
    "XPos=113;\r\n"
    "SpecialWhat=StartPos1;\r\n"
    "}\r\n"
    "[Special1]\r\n"
    "{\r\n"
    "XPos=60;\r\n"
    "ZPos=153;\r\n"
    "specialwhat=StartPos2;\r\n"
    "}\r\n"
    "}\r\n"
    "}\r\n"
    "}\r\n";

static const char SAMPLE_CRT[] = "crater table bytes";
static const char SAMPLE_TDF[] =
    "[Water]\n{\ndepth=12;\ncolour=blue;\n}\n";

static void fill(TAK_MapFiles *f) {
    memset(f, 0, sizeof(*f));
    f->tnt = SAMPLE_TNT;  f->tnt_size = SAMPLE_TNT_LEN;
    f->ota = SAMPLE_OTA;  f->ota_size = sizeof(SAMPLE_OTA) - 1;
}

static void hex_of(const TAK_MapFiles *f, char out[TAK_MAP_FINGERPRINT_HEX]) {
    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
    out[0] = '\0';
    if (TAK_MapFingerprint_FromFiles(f, fp) != 0) return;
    TAK_MapFingerprint_ToHex(fp, out);
}

/* ── SHA-256 itself ─────────────────────────────────────────────────── */

TEST(sha256_matches_the_published_vectors) {
    uint8_t d[TAK_SHA256_BYTES];
    char hex[65];

    TAK_Sha256_Hash("abc", 3, d);
    TAK_Sha256_ToHex(d, hex);
    ASSERT_EQ_STR("ba7816bf8f01cfea414140de5dae2223"
                  "b00361a396177a9cb410ff61f20015ad", hex);

    TAK_Sha256_Hash("", 0, d);
    TAK_Sha256_ToHex(d, hex);
    ASSERT_EQ_STR("e3b0c44298fc1c149afbf4c8996fb924"
                  "27ae41e4649b934ca495991b7852b855", hex);

    TAK_Sha256_Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    TAK_Sha256_ToHex(d, hex);
    ASSERT_EQ_STR("248d6a61d20638b8e5c026930c3e6039"
                  "a33ce45964ff2167f6ecedd419db06c1", hex);

    /* Longer than one block, to exercise the buffering. */
    char million[1000];
    memset(million, 'a', sizeof(million));
    TAK_Sha256 ctx;
    TAK_Sha256_Init(&ctx);
    for (int i = 0; i < 1000; i++) TAK_Sha256_Update(&ctx, million, sizeof(million));
    TAK_Sha256_Final(&ctx, d);
    TAK_Sha256_ToHex(d, hex);
    ASSERT_EQ_STR("cdc76e5c9914fb9281a1c7e284d73e67"
                  "f1809a48a497200e046d39ccc7112cd0", hex);
}

/* ── The fingerprint itself ─────────────────────────────────────────── */

TEST(fingerprint_is_the_same_every_run) {
    TAK_MapFiles f;
    char a[TAK_MAP_FINGERPRINT_HEX], b[TAK_MAP_FINGERPRINT_HEX];
    fill(&f);
    hex_of(&f, a);
    hex_of(&f, b);
    ASSERT_EQ_INT(64, (int)strlen(a));
    ASSERT_EQ_STR(a, b);
}

/* This value is the cross-target anchor: the browser build runs the same
 * case through node and has to produce it too. */
TEST(fingerprint_of_the_sample_map_is_the_golden_value) {
    TAK_MapFiles f;
    char hex[TAK_MAP_FINGERPRINT_HEX];
    fill(&f);
    f.crt = SAMPLE_CRT; f.crt_size = sizeof(SAMPLE_CRT) - 1;
    f.tdf = SAMPLE_TDF; f.tdf_size = sizeof(SAMPLE_TDF) - 1;
    hex_of(&f, hex);
    ASSERT_EQ_STR("172416c9fb431bf7cfb5058751c88077b21b857faf2158b857e42f12050c9a37", hex);
}

TEST(a_changed_height_byte_changes_the_fingerprint) {
    TAK_MapFiles f;
    char before[TAK_MAP_FINGERPRINT_HEX], after[TAK_MAP_FINGERPRINT_HEX];
    fill(&f);
    hex_of(&f, before);

    char edited[SAMPLE_TNT_LEN];
    memcpy(edited, SAMPLE_TNT, SAMPLE_TNT_LEN);
    edited[20] = (char)(edited[20] + 1);
    f.tnt = edited;
    hex_of(&f, after);

    ASSERT_EQ_INT(64, (int)strlen(before));
    ASSERT(strcmp(before, after) != 0);
}

TEST(a_changed_name_or_blurb_does_not_change_it) {
    TAK_MapFiles f;
    char plain[TAK_MAP_FINGERPRINT_HEX], redressed[TAK_MAP_FINGERPRINT_HEX];
    fill(&f);
    hex_of(&f, plain);
    f.ota = SAMPLE_OTA_REDRESSED;
    f.ota_size = sizeof(SAMPLE_OTA_REDRESSED) - 1;
    hex_of(&f, redressed);
    ASSERT_EQ_STR(plain, redressed);
}

TEST(a_moved_start_position_does_change_it) {
    TAK_MapFiles f;
    char plain[TAK_MAP_FINGERPRINT_HEX], moved[TAK_MAP_FINGERPRINT_HEX];
    fill(&f);
    hex_of(&f, plain);

    char edited[sizeof(SAMPLE_OTA)];
    memcpy(edited, SAMPLE_OTA, sizeof(SAMPLE_OTA));
    char *xpos = strstr(edited, "XPos=113");
    ASSERT_NOT_NULL(xpos);
    xpos[7] = '4';                     /* 113 becomes 114 */
    f.ota = edited;
    hex_of(&f, moved);
    ASSERT(strcmp(plain, moved) != 0);
}

TEST(the_optional_parts_count) {
    TAK_MapFiles f;
    char bare[TAK_MAP_FINGERPRINT_HEX];
    char with_crt[TAK_MAP_FINGERPRINT_HEX];
    char with_tdf[TAK_MAP_FINGERPRINT_HEX];
    fill(&f);
    hex_of(&f, bare);
    f.crt = SAMPLE_CRT; f.crt_size = sizeof(SAMPLE_CRT) - 1;
    hex_of(&f, with_crt);
    f.crt = NULL; f.crt_size = 0;
    f.tdf = SAMPLE_TDF; f.tdf_size = sizeof(SAMPLE_TDF) - 1;
    hex_of(&f, with_tdf);
    ASSERT(strcmp(bare, with_crt) != 0);
    ASSERT(strcmp(bare, with_tdf) != 0);
    ASSERT(strcmp(with_crt, with_tdf) != 0);
}

TEST(missing_parts_are_refused) {
    TAK_MapFiles f;
    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
    memset(&f, 0, sizeof(f));
    ASSERT_EQ_INT(-1, TAK_MapFingerprint_FromFiles(&f, fp));
    f.tnt = SAMPLE_TNT; f.tnt_size = SAMPLE_TNT_LEN;
    ASSERT_EQ_INT(-1, TAK_MapFingerprint_FromFiles(&f, fp));
}

TEST(canonical_form_reads_as_the_definition_says) {
    char out[1024];
    int n = TAK_MapFingerprint_CanonicalTDF(SAMPLE_OTA, sizeof(SAMPLE_OTA) - 1,
                                            out, sizeof(out));
    ASSERT(n > 0);
    ASSERT_EQ_STR(
        "[globalheader]\n{\n"
        "kingdom=aramon\n"
        "numplayers=2\n"
        "size=6 x 6\n"
        "[map data]\n{\n"
        "type=Network 1\n"
        "[specials]\n{\n"
        "[special0]\n{\n"
        "specialwhat=StartPos1\n"
        "xpos=113\n"
        "zpos=20\n"
        "}\n"
        "[special1]\n{\n"
        "specialwhat=StartPos2\n"
        "xpos=60\n"
        "zpos=153\n"
        "}\n"
        "}\n"
        "}\n"
        "}\n", out);
}

/* ── Shipped maps ───────────────────────────────────────────────────── */

static int game_dir_exists(void) {
    struct stat st;
    return stat(TAK_GAME_DIR, &st) == 0;
}

static int open_game_vfs(void) {
    if (!game_dir_exists()) return -1;
    if (VFS_IsInitialized()) VFS_Shutdown();
    return VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR);
}

static void hex_of_map(const char *key, char out[TAK_MAP_FINGERPRINT_HEX]) {
    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
    out[0] = '\0';
    if (TAK_MapFingerprint_FromName(key, fp) != 0) return;
    TAK_MapFingerprint_ToHex(fp, out);
}

/* One map from an archive, one from the expansion, one from a map pack. */
TEST(shipped_maps_have_their_golden_fingerprints) {
    if (open_game_vfs() != 0) SKIP("no game data");
    char ground[TAK_MAP_FINGERPRINT_HEX];
    char rival[TAK_MAP_FINGERPRINT_HEX];
    char adamantine[TAK_MAP_FINGERPRINT_HEX];
    hex_of_map("ground war", ground);
    hex_of_map("rival hill", rival);
    hex_of_map("adamantine gate", adamantine);
    VFS_Shutdown();
    ASSERT_EQ_STR("f0aa1ca52036d8667cfcceb9198bc47535ceac59099eac86ea1befaa7533815a", ground);
    ASSERT_EQ_STR("ddaff3d4566577e415d710851e67e6d5adf86a94d5e867f0a41743f1b91ac001", rival);
    ASSERT_EQ_STR("fce18a0df3de36a51a703badcb001720b2dc0287109d8777f255a0408ddf8ea9", adamantine);
}

/* The same map out of its archive and as loose files on disk. */
TEST(an_archived_map_and_a_loose_copy_agree) {
    if (open_game_vfs() != 0) SKIP("no game data");

    char from_archive[TAK_MAP_FINGERPRINT_HEX];
    hex_of_map("ground war", from_archive);

    /* Copy the four files out, then hash them straight from disk. */
    static const char *const exts[4] = { "tnt", "ota", "crt", "tdf" };
    void *data[4] = { NULL, NULL, NULL, NULL };
    uint32_t size[4] = { 0, 0, 0, 0 };
    char disk_path[4][64];
    for (int i = 0; i < 4; i++) {
        char path[512];
        snprintf(disk_path[i], sizeof(disk_path[i]), "test_fp_loose_%s", exts[i]);
        if (TAK_Maps_FindFile("ground war", exts[i], path, sizeof(path)) != 0) continue;
        if (VFS_ReadFile(path, &data[i], &size[i]) != 0) continue;
        FILE *fp = fopen(disk_path[i], "wb");
        if (fp) { fwrite(data[i], 1, size[i], fp); fclose(fp); }
        VFS_FreeBuffer(data[i]);
        data[i] = NULL;
    }
    VFS_Shutdown();

    void *loose[4] = { NULL, NULL, NULL, NULL };
    size_t loose_size[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        FILE *fp = fopen(disk_path[i], "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n > 0) {
            loose[i] = malloc((size_t)n);
            if (loose[i] && fread(loose[i], 1, (size_t)n, fp) == (size_t)n)
                loose_size[i] = (size_t)n;
        }
        fclose(fp);
    }

    TAK_MapFiles files;
    memset(&files, 0, sizeof(files));
    files.tnt = loose[0]; files.tnt_size = loose_size[0];
    files.ota = loose[1]; files.ota_size = loose_size[1];
    files.crt = loose[2]; files.crt_size = loose_size[2];
    files.tdf = loose[3]; files.tdf_size = loose_size[3];
    char from_loose[TAK_MAP_FINGERPRINT_HEX];
    hex_of(&files, from_loose);

    for (int i = 0; i < 4; i++) { free(loose[i]); remove(disk_path[i]); }

    ASSERT_EQ_INT(64, (int)strlen(from_archive));
    ASSERT_EQ_STR(from_archive, from_loose);
}

/* Editing the blurb of a shipped map leaves its fingerprint alone, and
 * touching its terrain does not. */
TEST(a_shipped_map_ignores_its_blurb_and_notices_its_terrain) {
    if (open_game_vfs() != 0) SKIP("no game data");

    char path[512];
    void *tnt = NULL, *ota = NULL;
    uint32_t tnt_size = 0, ota_size = 0;
    TAK_Maps_FindFile("ground war", "tnt", path, sizeof(path));
    VFS_ReadFile(path, &tnt, &tnt_size);
    TAK_Maps_FindFile("ground war", "ota", path, sizeof(path));
    VFS_ReadFile(path, &ota, &ota_size);
    VFS_Shutdown();
    if (!tnt || !ota) {
        VFS_FreeBuffer(tnt);
        VFS_FreeBuffer(ota);
        SKIP("no game data");
    }

    TAK_MapFiles files;
    memset(&files, 0, sizeof(files));
    files.tnt = tnt; files.tnt_size = tnt_size;
    files.ota = ota; files.ota_size = ota_size;
    char plain[TAK_MAP_FINGERPRINT_HEX];
    hex_of(&files, plain);

    /* Rewrite the description in place, same length. */
    char *found = NULL;
    for (char *p = (char *)ota; p + 18 < (char *)ota + ota_size; p++) {
        if (tak_strnicmp(p, "missiondescription", 18) == 0) { found = p; break; }
    }
    char blurbed[TAK_MAP_FINGERPRINT_HEX] = "";
    if (found) {
        char *eq = found;
        while (eq < (char *)ota + ota_size && *eq != '=') eq++;
        for (char *p = eq + 1; p < (char *)ota + ota_size && *p != ';'; p++) *p = 'x';
        hex_of(&files, blurbed);
    }

    /* Now poke a height byte. */
    uint8_t *bytes = (uint8_t *)tnt;
    uint32_t height_off = *(uint32_t *)(bytes + 0x10);
    char terrain[TAK_MAP_FINGERPRINT_HEX] = "";
    if (height_off + 64 < tnt_size) {
        bytes[height_off + 64] = (uint8_t)(bytes[height_off + 64] + 1);
        hex_of(&files, terrain);
    }

    VFS_FreeBuffer(tnt);
    VFS_FreeBuffer(ota);

    ASSERT_EQ_INT(64, (int)strlen(plain));
    ASSERT_EQ_STR(plain, blurbed);
    ASSERT(strcmp(plain, terrain) != 0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    tak_mem_init();

    /* This suite is not labelled needs-data, so CI runs it with no game
     * install at all. There the shipped-map cases cannot run, and saying
     * so up front is what keeps their skip from reading as a pass. */
    if (!game_dir_exists()) {
        TEST_ALLOW_SKIPS("no game install, so the shipped-map cases "
                         "have nothing to read");
    }

    TEST_SUITE("SHA-256");
    RUN(sha256_matches_the_published_vectors);

    TEST_SUITE("Map fingerprint");
    RUN(fingerprint_is_the_same_every_run);
    RUN(fingerprint_of_the_sample_map_is_the_golden_value);
    RUN(a_changed_height_byte_changes_the_fingerprint);
    RUN(a_changed_name_or_blurb_does_not_change_it);
    RUN(a_moved_start_position_does_change_it);
    RUN(the_optional_parts_count);
    RUN(missing_parts_are_refused);
    RUN(canonical_form_reads_as_the_definition_says);

    TEST_SUITE("Shipped maps");
    RUN(shipped_maps_have_their_golden_fingerprints);
    RUN(an_archived_map_and_a_loose_copy_agree);
    RUN(a_shipped_map_ignores_its_blurb_and_notices_its_terrain);

    TEST_REPORT();
}
