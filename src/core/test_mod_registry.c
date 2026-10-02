/*
 * test_mod_registry.c -- the mod registry: its file, the check on a
 * download, the install and removal, and the relay's download stream.
 *
 * Data free. Zips are built in memory, the game is a synthetic archive,
 * and everything written goes in scratch folders beside the binary.
 */

#include "test_framework.h"
#include "test_hpi_builder.h"
#include "tak_mod_registry.h"
#include "tak_mod_install.h"
#include "tak_mod_proxy.h"
#include "tak_modset.h"
#include "tak_data_fingerprint.h"
#include "tak_net_http.h"
#include "tak_sha256.h"
#include "tak_hpi.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef TAK_SOURCE_DIR
#define TAK_SOURCE_DIR "."
#endif

/* ── helpers ───────────────────────────────────────────────────────── */

static void rm_tree(const char *path) {
#ifdef _WIN32
    char pattern[600];
    snprintf(pattern, sizeof pattern, "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            char full[600];
            snprintf(full, sizeof full, "%s/%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(full);
            else remove(full);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    _rmdir(path);
#else
    DIR *d = opendir(path);
    if (d) {
        for (struct dirent *e; (e = readdir(d)) != NULL;) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char full[600];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(full);
            else remove(full);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

static int make_dir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp) fclose(fp);
    return fp != NULL;
}

static char *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = '\0';
    if (len) *len = (size_t)n;
    fclose(fp);
    return buf;
}

typedef struct { const char *name; const char *text; } ZipFile;

static void *make_zip(const ZipFile *f, int n, size_t *len) {
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_heap(&z, 0, 0)) return NULL;
    for (int i = 0; i < n; i++)
        mz_zip_writer_add_mem(&z, f[i].name, f[i].text, strlen(f[i].text), MZ_DEFAULT_COMPRESSION);
    void *buf = NULL;
    if (!mz_zip_writer_finalize_heap_archive(&z, &buf, len)) buf = NULL;
    mz_zip_writer_end(&z);
    return buf;
}

static void entry_for(TAK_ModEntry *e, const char *id, const char *modset,
                      const void *bytes, size_t len, uint64_t fingerprint) {
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->name, sizeof e->name, "Tough Swords");
    snprintf(e->version, sizeof e->version, "0.2");
    snprintf(e->author, sizeof e->author, "Someone");
    snprintf(e->page, sizeof e->page, "https://example.org/tough-swords");
    snprintf(e->modset, sizeof e->modset, "%s", modset);
    snprintf(e->url, sizeof e->url, "https://example.org/%s.zip", id);
    e->size = len;
    if (bytes) TAK_Sha256_Hash(bytes, len, e->sha256);
    e->fingerprint = fingerprint;
}

#define GOOD_ENTRY(id) \
    "{\"id\":\"" id "\",\"name\":\"Tough Swords\",\"version\":\"0.2\",\"author\":\"Someone\"," \
    "\"page\":\"https://example.org/ts\",\"modset\":\"tough-swords\"," \
    "\"url\":\"https://example.org/ts.zip\",\"size\":1234," \
    "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"," \
    "\"fingerprint\":\"0123456789abcdef\"}"

/* ── the registry file ─────────────────────────────────────────────── */

TEST(the_shipped_registry_is_valid) {
    size_t len = 0;
    char *text = slurp(TAK_SOURCE_DIR "/web/mods/registry.json", &len);
    ASSERT_NOT_NULL(text);
    static TAK_ModRegistry r;
    int n = TAK_ModRegistry_Parse(text, len, &r);
    free(text);
    if (r.errors) printf("\n    %s\n", r.first_error);
    ASSERT_EQ_INT(0, r.errors);
    ASSERT(n >= 1);
    const TAK_ModEntry *test = TAK_ModRegistry_Find(&r, "ok-registry-test");
    ASSERT_NOT_NULL(test);
    ASSERT(TAK_ModEntry_OneClick(test));
    ASSERT_EQ_STR("https://openkingdoms.net/mods/ok-registry-test-1.0.zip", test->url);
    /* What the site serves beside the registry is what it lists. */
    size_t zl = 0;
    char *zip = slurp(TAK_SOURCE_DIR "/web/mods/ok-registry-test-1.0.zip", &zl);
    ASSERT_NOT_NULL(zip);
    char why[256];
    int rc = TAK_ModEntry_Check(test, zip, zl, 0, why, sizeof why);
    free(zip);
    if (rc) printf("\n    %s\n", why);
    ASSERT_EQ_INT(0, rc);
}

TEST(an_entry_with_a_bad_field_is_left_out_and_said_why) {
    static const struct { const char *entry; const char *why; } bad[] = {
        { "{\"id\":\"Caps\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"manual\":\"m\"}", "id must" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"http://x.org\",\"manual\":\"m\"}", "page must" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\"}", "url or manual" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"manual\":\"m\",\"url\":\"https://x.org/a.zip\"}", "not both" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"\",\"page\":\"https://x.org\",\"manual\":\"m\"}", "author" },
        { "{\"id\":\"m\",\"name\":\"a name longer than a room can carry\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"manual\":\"m\"}", "too long" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"modset\":\"m\",\"url\":\"ftp://x.org/a.zip\",\"size\":1,"
          "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\",\"fingerprint\":\"0123456789abcdef\"}", "url must" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"modset\":\"m\",\"url\":\"https://x.org/a.zip\",\"size\":67108865,"
          "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\",\"fingerprint\":\"0123456789abcdef\"}", "over the limit" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"modset\":\"m\",\"url\":\"https://x.org/a.zip\",\"size\":1.5,"
          "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\",\"fingerprint\":\"0123456789abcdef\"}", "whole number" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"modset\":\"m\",\"url\":\"https://x.org/a.zip\",\"size\":1,"
          "\"sha256\":\"0011\",\"fingerprint\":\"0123456789abcdef\"}", "sha256" },
        { "{\"id\":\"m\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"modset\":\"m\",\"url\":\"https://x.org/a.zip\",\"size\":1,"
          "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\",\"fingerprint\":\"0000000000000000\"}", "zero" },
        { GOOD_ENTRY("ts"), "twice" },
    };
    static TAK_ModRegistry r;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char text[2048];
        snprintf(text, sizeof text, "{\"registry\":1,\"about\":{\"x\":[1,2]},\"mods\":[%s,%s]}",
                 GOOD_ENTRY("ts"), bad[i].entry);
        int n = TAK_ModRegistry_Parse(text, strlen(text), &r);
        if (n != 1 || r.errors != 1 || !strstr(r.first_error, bad[i].why))
            printf("\n    case %d: kept %d, %d errors, \"%s\"\n", (int)i, n, r.errors, r.first_error);
        ASSERT_EQ_INT(1, n);
        ASSERT_EQ_INT(1, r.errors);
        ASSERT_NOT_NULL(strstr(r.first_error, bad[i].why));
    }
    ASSERT_EQ_INT(1234, (int)r.mod[0].size);
    ASSERT(r.mod[0].fingerprint == 0x0123456789abcdefULL);
    ASSERT_EQ_INT(0x00, r.mod[0].sha256[0]);
    ASSERT_EQ_INT(0xff, r.mod[0].sha256[31]);
}

TEST(text_that_is_not_a_registry_is_refused) {
    static TAK_ModRegistry r;
    static const char *const not_one[] = {
        "", "[]", "{\"registry\":2,\"mods\":[]}", "{\"mods\":[]}", "{\"registry\":1}",
        "{\"registry\":1,\"mods\":[]} trailing", "{\"registry\":1,\"mods\":[{\"id\":\"a\"]}",
        "{\"registry\":1,\"mods\":[{\"id\":\"\\ud800\"}]}",
    };
    for (size_t i = 0; i < sizeof not_one / sizeof not_one[0]; i++)
        ASSERT_EQ_INT(-1, TAK_ModRegistry_Parse(not_one[i], strlen(not_one[i]), &r));
    const char *empty = "{ \"registry\" : 1 , \"mods\" : [ ] }";
    ASSERT_EQ_INT(0, TAK_ModRegistry_Parse(empty, strlen(empty), &r));
}

TEST(a_room_finds_its_entry_by_fingerprint_then_by_name) {
    static TAK_ModRegistry r;
    const char *text = "{\"registry\":1,\"mods\":[" GOOD_ENTRY("ts") "]}";
    ASSERT_EQ_INT(1, TAK_ModRegistry_Parse(text, strlen(text), &r));
    ASSERT_NOT_NULL(TAK_ModRegistry_ForRoom(&r, "Something Else", "9", 0x0123456789abcdefULL));
    ASSERT_NOT_NULL(TAK_ModRegistry_ForRoom(&r, "tough swords", "0.2", 0x1111));
    ASSERT_NULL(TAK_ModRegistry_ForRoom(&r, "Tough Swords", "0.3", 0x1111));
    ASSERT_NULL(TAK_ModRegistry_ForRoom(&r, "Vanilla", "", 0x1111));
}

/* ── the check on a download ───────────────────────────────────────── */

TEST(a_good_download_passes_and_a_tampered_one_is_refused) {
    ZipFile f[] = { { "Mods/Tough Swords/units/arasword.fbi", "[UNITINFO]\n{\nMaxDamage=1200;\n}\n" } };
    size_t len = 0;
    unsigned char *zip = (unsigned char *)make_zip(f, 1, &len);
    ASSERT_NOT_NULL(zip);
    TAK_ModEntry e;
    entry_for(&e, "tough-swords", "tough-swords", zip, len, 0x5d0e44a1abcdef12ULL);
    char why[256];
    ASSERT_EQ_INT(0, TAK_ModEntry_Check(&e, zip, len, 0x5d0e44a1abcdef12ULL, why, sizeof why));
    ASSERT_EQ_INT(0, TAK_ModEntry_Check(&e, zip, len, 0, why, sizeof why));
    zip[len / 2] ^= 0x01;
    ASSERT_EQ_INT(-1, TAK_ModEntry_Check(&e, zip, len, 0x5d0e44a1abcdef12ULL, why, sizeof why));
    ASSERT_NOT_NULL(strstr(why, "does not match the registry's fingerprint"));
    zip[len / 2] ^= 0x01;
    ASSERT_EQ_INT(-1, TAK_ModEntry_Check(&e, zip, len - 1, 0, why, sizeof why));
    ASSERT_NOT_NULL(strstr(why, "bytes and the registry says"));
    ASSERT_EQ_INT(-1, TAK_ModEntry_Check(&e, zip, len, 0x1234, why, sizeof why));
    ASSERT_NOT_NULL(strstr(why, "plays other data than that game"));
    mz_free(zip);
}

/* ── install and remove ────────────────────────────────────────────── */

#define ROOT  "test_modreg_tmp"
#define ROOT2 "test_modreg_tmp2"
#define GAME  "test_modreg_game_tmp"

static const ZipFile k_folder_mod[] = {
    { "Mods/Tough Swords/features/zz/okstone.tdf", "[OKSTONE]\n{\nheight=12;\n}\n" },
    { "Mods/Tough Swords/readme.txt", "loose and kept, it is under Mods" },
    { "Kingdoms.exe", "not ours to replace" },
    { "Keys.tdf", "the player's keys stay" },
    { "TAKEnhanced/TAKEnhanced.dll", "not read here" },
    { "README.txt", "left out" },
};

TEST(an_install_takes_only_mods_and_presets_and_writes_a_manifest) {
    rm_tree(ROOT);
    make_dir(ROOT);
    size_t len = 0;
    void *zip = make_zip(k_folder_mod, 6, &len);
    TAK_ModEntry e;
    entry_for(&e, "tough-swords", "tough-swords", zip, len, 0x5d0e44a1abcdef12ULL);
    char why[256];
    int rc = TAK_ModInstall_Zip(ROOT, &e, zip, len, why, sizeof why);
    mz_free(zip);
    if (rc) printf("\n    %s\n", why);
    ASSERT_EQ_INT(0, rc);
    ASSERT(exists(ROOT "/Mods/Tough Swords/features/zz/okstone.tdf"));
    ASSERT(exists(ROOT "/Mods/Tough Swords/readme.txt"));
    ASSERT(exists(ROOT "/Mods/tough-swords.registry.txt"));
    ASSERT(!exists(ROOT "/Kingdoms.exe"));
    ASSERT(!exists(ROOT "/Keys.tdf"));
    ASSERT(!exists(ROOT "/TAKEnhanced/TAKEnhanced.dll"));
    ASSERT(!exists(ROOT "/README.txt"));
    char version[16];
    ASSERT_EQ_INT(1, TAK_ModInstall_Installed(ROOT, "tough-swords", version, sizeof version));
    ASSERT_EQ_STR("0.2", version);
    TAK_ModSet sets[4];
    int n = TAK_ModSet_Scan(ROOT, sets, 4);
    const TAK_ModSet *s = TAK_ModSet_Find(sets, n, "tough-swords");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_STR("Tough Swords", s->name);
    ASSERT_EQ_STR("0.2", s->version);
    ASSERT(s->fingerprint == 0x5d0e44a1abcdef12ULL);
    rm_tree(ROOT);
}

TEST(a_preset_mod_gets_its_manifest_beside_the_preset) {
    rm_tree(ROOT);
    make_dir(ROOT);
    ZipFile f[] = {
        { "Mods/", "" },
        { "Mods/Fix One.hpi", "an archive" },
        { "TAKEnhanced/Presets/vanilla.preset.json", "{\"id\":\"vanilla\",\"mods\":{\"enabled\":false}}" },
        { "TAKEnhanced/Presets/te.preset.json",
          "{\"id\":\"TE-Set\",\"name\":\"TE\",\"mods\":{\"enabled\":true,\"selectedMods\":[\"Fix One.hpi\"]}}" },
    };
    size_t len = 0;
    void *zip = make_zip(f, 4, &len);
    TAK_ModEntry e;
    entry_for(&e, "te", "te-set", zip, len, 0x77ULL);
    char why[256];
    int rc = TAK_ModInstall_Zip(ROOT, &e, zip, len, why, sizeof why);
    mz_free(zip);
    if (rc) printf("\n    %s\n", why);
    ASSERT_EQ_INT(0, rc);
    ASSERT(exists(ROOT "/Mods/Fix One.hpi"));
    ASSERT(exists(ROOT "/TAKEnhanced/Presets/te.mod.tdf"));
    TAK_ModSet sets[4];
    int n = TAK_ModSet_Scan(ROOT, sets, 4);
    const TAK_ModSet *s = TAK_ModSet_Find(sets, n, "te-set");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_STR("Tough Swords", s->name);
    ASSERT_EQ_STR("0.2", s->version);
    ASSERT(s->fingerprint == 0x77ULL);
    ASSERT_EQ_INT(0, TAK_ModInstall_Remove(ROOT, "te", why, sizeof why));
    ASSERT(!exists(ROOT "/Mods/Fix One.hpi"));
    ASSERT(!exists(ROOT "/TAKEnhanced/Presets/te.mod.tdf"));
    ASSERT(!exists(ROOT "/Mods/te.registry.txt"));
    rm_tree(ROOT);
}

TEST(a_zip_that_climbs_out_of_its_folder_is_refused) {
    char rel[400];
    static const char *const bad[] = {
        "Mods/../../evil.txt", "/Mods/x.hpi", "C:/Mods/x.hpi", "Mods\\..\\x.hpi",
        "Mods//x.hpi", "Mods/./x.hpi", "Mods/a\x01.hpi",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        ASSERT_EQ_INT(-1, TAK_ModInstall_Rel(bad[i], rel, sizeof rel));
    ASSERT_EQ_INT(1, TAK_ModInstall_Rel("mods\\A b\\c.hpi", rel, sizeof rel));
    ASSERT_EQ_STR("Mods/A b/c.hpi", rel);
    ASSERT_EQ_INT(1, TAK_ModInstall_Rel("takenhanced/presets/p.json", rel, sizeof rel));
    ASSERT_EQ_STR("TAKEnhanced/Presets/p.json", rel);
    ASSERT_EQ_INT(0, TAK_ModInstall_Rel("TAKEnhanced/TAKEnhanced.dll", rel, sizeof rel));
    ASSERT_EQ_INT(0, TAK_ModInstall_Rel("Mods/", rel, sizeof rel));

    rm_tree(ROOT);
    make_dir(ROOT);
    ZipFile f[] = { { "Mods/Tough Swords/a.tdf", "fine" }, { "Mods/../../evil.txt", "climbs" } };
    size_t len = 0;
    void *zip = make_zip(f, 2, &len);
    TAK_ModEntry e;
    entry_for(&e, "tough-swords", "tough-swords", zip, len, 1);
    char why[256];
    ASSERT_EQ_INT(-1, TAK_ModInstall_Zip(ROOT, &e, zip, len, why, sizeof why));
    mz_free(zip);
    ASSERT_NOT_NULL(strstr(why, "outside the game folder"));
    ASSERT(!exists(ROOT "/Mods/Tough Swords/a.tdf"));
    ASSERT(!exists(ROOT "/Mods/tough-swords.registry.txt"));
    rm_tree(ROOT);
}

TEST(a_zip_without_the_mod_set_it_promises_is_refused) {
    rm_tree(ROOT);
    make_dir(ROOT);
    size_t len = 0;
    void *zip = make_zip(k_folder_mod, 6, &len);
    TAK_ModEntry e;
    entry_for(&e, "tough-swords", "blunt-swords", zip, len, 1);
    char why[256];
    ASSERT_EQ_INT(-1, TAK_ModInstall_Zip(ROOT, &e, zip, len, why, sizeof why));
    ASSERT_NOT_NULL(strstr(why, "has no mod set blunt-swords"));
    ASSERT(!exists(ROOT "/Mods/Tough Swords/readme.txt"));
    ASSERT_EQ_INT(-1, TAK_ModInstall_Zip(ROOT, &e, "not a zip", 9, why, sizeof why));
    ASSERT_NOT_NULL(strstr(why, "not a zip"));
    mz_free(zip);
    rm_tree(ROOT);
}

TEST(remove_deletes_what_the_install_wrote_and_keeps_shared_files) {
    rm_tree(ROOT);
    make_dir(ROOT);
    ZipFile a[] = { { "Mods/Shared.hpi", "both ship this" }, { "Mods/A Mod/a.tdf", "a" } };
    ZipFile b[] = { { "Mods/Shared.hpi", "both ship this" }, { "Mods/B Mod/b.tdf", "b" } };
    size_t la = 0, lb = 0;
    void *za = make_zip(a, 2, &la), *zb = make_zip(b, 2, &lb);
    TAK_ModEntry ea, eb;
    entry_for(&ea, "a", "a-mod", za, la, 1);
    entry_for(&eb, "b", "b-mod", zb, lb, 2);
    char why[256];
    ASSERT_EQ_INT(0, TAK_ModInstall_Zip(ROOT, &ea, za, la, why, sizeof why));
    ASSERT_EQ_INT(0, TAK_ModInstall_Zip(ROOT, &eb, zb, lb, why, sizeof why));
    mz_free(za);
    mz_free(zb);
    ASSERT_EQ_INT(0, TAK_ModInstall_Remove(ROOT, "a", why, sizeof why));
    ASSERT(exists(ROOT "/Mods/Shared.hpi"));
    ASSERT(!exists(ROOT "/Mods/A Mod/a.tdf"));
    ASSERT(!exists(ROOT "/Mods/A Mod/mod.tdf"));
    ASSERT(exists(ROOT "/Mods/B Mod/b.tdf"));
    ASSERT_EQ_INT(0, TAK_ModInstall_Installed(ROOT, "a", NULL, 0));
    ASSERT_EQ_INT(-1, TAK_ModInstall_Remove(ROOT, "a", why, sizeof why));
    ASSERT_EQ_INT(0, TAK_ModInstall_Remove(ROOT, "b", why, sizeof why));
    ASSERT(!exists(ROOT "/Mods/Shared.hpi"));
    ASSERT(!exists(ROOT "/Mods/B Mod/b.tdf"));
    rm_tree(ROOT);
}

/* The room compares data fingerprints, so a changed file in a mod is a
 * mod the room turns away, even one that got past the download check. */
static int mounted_content(const char *root, const char *set_id, uint64_t *content) {
    TAK_ModSet sets[4];
    int n = TAK_ModSet_Scan(root, sets, 4);
    const TAK_ModSet *s = TAK_ModSet_Find(sets, n, set_id);
    if (!s) return -1;
    const char *paths[TAK_MODSET_PATHS];
    for (int i = 0; i < s->count; i++) paths[i] = s->path[i];
    VFS_SetModArchives(paths, s->count);
    int rc = VFS_Init(GAME, NULL);
    TAK_DataFingerprint fp;
    if (rc == 0) rc = TAK_DataFingerprint_Compute(&fp);
    VFS_Shutdown();
    VFS_SetModArchives(NULL, 0);
    if (rc == 0) *content = fp.content;
    return rc;
}

TEST(a_tampered_mod_changes_the_data_fingerprint_the_room_checks) {
    rm_tree(GAME);
    rm_tree(ROOT);
    rm_tree(ROOT2);
    make_dir(GAME);
    make_dir(ROOT);
    make_dir(ROOT2);
    TestHPIEntry base[] = {
        { "units/aramon.fbi", "[UNITINFO]\r\n{\r\n\tMaxDamage=900;\r\n}\r\n", 1000, 0 },
        { "features/trees/tree.tdf", "[TREE]\r\n{\r\n}\r\n", 1000, 0 },
    };
    ASSERT_EQ_INT(0, test_write_hpi(GAME "/data.hpi", base, 2));
    ZipFile good[] = { { "Mods/Tough Swords/features/zz/okstone.tdf", "[OKSTONE]\n{\nheight=12;\n}\n" } };
    ZipFile bad[] = { { "Mods/Tough Swords/features/zz/okstone.tdf", "[OKSTONE]\n{\nheight=13;\n}\n" } };
    size_t lg = 0, lb = 0;
    void *zg = make_zip(good, 1, &lg), *zb = make_zip(bad, 1, &lb);
    TAK_ModEntry e;
    entry_for(&e, "tough-swords", "tough-swords", zg, lg, 1);
    char why[256];
    ASSERT_EQ_INT(0, TAK_ModInstall_Zip(ROOT, &e, zg, lg, why, sizeof why));
    ASSERT_EQ_INT(0, TAK_ModInstall_Zip(ROOT2, &e, zb, lb, why, sizeof why));
    mz_free(zg);
    mz_free(zb);
    uint64_t vanilla = 0, ours = 0, again = 0, tampered = 0;
    ASSERT_EQ_INT(0, mounted_content(ROOT, "vanilla", &vanilla));
    ASSERT_EQ_INT(0, mounted_content(ROOT, "tough-swords", &ours));
    ASSERT_EQ_INT(0, mounted_content(ROOT, "tough-swords", &again));
    ASSERT_EQ_INT(0, mounted_content(ROOT2, "tough-swords", &tampered));
    ASSERT(ours != vanilla);
    ASSERT(ours == again);
    ASSERT(tampered != ours);
    rm_tree(GAME);
    rm_tree(ROOT);
    rm_tree(ROOT2);
}

/* ── the relay's download stream ───────────────────────────────────── */

typedef struct {
    char   out[4096];
    size_t len, take;
    int    closed;
} Sink;

static size_t sink_write(void *ctx, uint32_t conn, const void *bytes, size_t len, int last) {
    Sink *s = (Sink *)ctx;
    (void)conn;
    if (s->take && len > s->take) len = s->take;
    if (len > sizeof s->out - 1 - s->len) len = sizeof s->out - 1 - s->len;
    memcpy(s->out + s->len, bytes, len);
    s->len += len;
    s->out[s->len] = '\0';
    if (last) s->closed = 1;
    return len;
}

static TAK_ModRegistry g_reg;

static void small_registry(void) {
    const char *text = "{\"registry\":1,\"mods\":[" GOOD_ENTRY("ts") ","
        "{\"id\":\"by-hand\",\"name\":\"n\",\"version\":\"1\",\"author\":\"a\",\"page\":\"https://x.org\",\"manual\":\"m\"}]}";
    TAK_ModRegistry_Parse(text, strlen(text), &g_reg);
}

static int begin(TAK_ModProxy *p, uint32_t conn, const char *req, char *out, size_t cap, size_t *n) {
    return TAK_ModProxy_Begin(p, conn, (const uint8_t *)req, strlen(req), out, cap, n);
}

TEST(only_registry_ids_are_fetched_never_a_url) {
    small_registry();
    static TAK_ModProxy p;
    TAK_ModProxy_Init(&p, &g_reg, 1);
    char out[1024];
    size_t n = 0;
    const char *dl = "GET /api/mods/ts/download HTTP/1.1\r\n\r\n";
    ASSERT(TAK_ModProxy_IsDownload((const uint8_t *)dl, strlen(dl)));
    static const char *const not_dl[] = {
        "GET /api/mods HTTP/1.1\r\n\r\n", "GET /api/mods/ts HTTP/1.1\r\n\r\n",
        "GET /api/mods/ts/downloads HTTP/1.1\r\n\r\n", "POST /api/mods/ts/download HTTP/1.1\r\n\r\n",
        "GET /api/mods//download HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof not_dl / sizeof not_dl[0]; i++)
        ASSERT(!TAK_ModProxy_IsDownload((const uint8_t *)not_dl[i], strlen(not_dl[i])));
    ASSERT_EQ_INT(-1, begin(&p, 1, "GET /api/mods/nope/download HTTP/1.1\r\n\r\n", out, sizeof out, &n));
    ASSERT_NOT_NULL(strstr(out, "404"));
    ASSERT_NOT_NULL(strstr(out, "not in the registry"));
    ASSERT_NOT_NULL(strstr(out, "Access-Control-Allow-Origin: *"));
    ASSERT_EQ_INT(-1, begin(&p, 1, "GET /api/mods/https:%2F%2Fevil.example%2Fx.zip/download HTTP/1.1\r\n\r\n",
                            out, sizeof out, &n));
    ASSERT_NOT_NULL(strstr(out, "404"));
    ASSERT_EQ_INT(-1, begin(&p, 1, "GET /api/mods/by-hand/download HTTP/1.1\r\n\r\n", out, sizeof out, &n));
    ASSERT_NOT_NULL(strstr(out, "installs by hand"));
    /* A url in the query is not read: the registry's is fetched. */
    int slot = begin(&p, 1, "GET /api/mods/ts/download?url=https://evil.example/x HTTP/1.1\r\n\r\n",
                     out, sizeof out, &n);
    ASSERT(slot >= 0);
    ASSERT_EQ_STR("https://example.org/ts.zip", TAK_ModProxy_Url(&p, slot));
    ASSERT_EQ_INT(1234, (int)TAK_ModProxy_Size(&p, slot));
}

TEST(a_download_streams_with_cors_once_its_first_byte_arrives) {
    small_registry();
    static TAK_ModProxy p;
    TAK_ModProxy_Init(&p, &g_reg, 1);
    char out[1024];
    size_t n = 0;
    int slot = begin(&p, 7, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n);
    ASSERT(slot >= 0);
    Sink s;
    memset(&s, 0, sizeof s);
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT_EQ_INT(0, (int)s.len);
    static char body[1234];
    memset(body, 'z', sizeof body);
    ASSERT_EQ_INT(1000, (int)TAK_ModProxy_Data(&p, slot, body, 1000));
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT_NOT_NULL(strstr(s.out, "HTTP/1.1 200 OK\r\n"));
    ASSERT_NOT_NULL(strstr(s.out, "Content-Length: 1234\r\n"));
    ASSERT_NOT_NULL(strstr(s.out, "Access-Control-Allow-Origin: *\r\n"));
    ASSERT(!s.closed);
    ASSERT_EQ_INT(234, (int)TAK_ModProxy_Data(&p, slot, body, 234));
    TAK_ModProxy_Done(&p, slot, 1, NULL);
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT(s.closed);
    const char *start = strstr(s.out, "\r\n\r\n");
    ASSERT_NOT_NULL(start);
    ASSERT_EQ_INT(1234, (int)(s.len - (size_t)(start + 4 - s.out)));
    ASSERT_NULL(TAK_ModProxy_Url(&p, slot));
}

TEST(a_download_past_its_listed_size_is_cut_off) {
    small_registry();
    static TAK_ModProxy p;
    TAK_ModProxy_Init(&p, &g_reg, 1);
    char out[1024];
    size_t n = 0;
    static char body[2000];
    memset(body, 'z', sizeof body);
    /* Over at once: nothing went out, so the page reads why. */
    int slot = begin(&p, 1, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n);
    ASSERT(TAK_ModProxy_Data(&p, slot, body, 1235) == TAK_MODPROXY_ABORT);
    TAK_ModProxy_Done(&p, slot, 0, "aborted");
    Sink s;
    memset(&s, 0, sizeof s);
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT_NOT_NULL(strstr(s.out, "502"));
    ASSERT_NOT_NULL(strstr(s.out, "larger than the registry says"));
    ASSERT(s.closed);
    /* Over part way: closed short of the length promised. */
    slot = begin(&p, 2, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n);
    ASSERT_EQ_INT(1000, (int)TAK_ModProxy_Data(&p, slot, body, 1000));
    ASSERT(TAK_ModProxy_Data(&p, slot, body, 300) == TAK_MODPROXY_ABORT);
    memset(&s, 0, sizeof s);
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT(s.closed);
    const char *start = strstr(s.out, "\r\n\r\n");
    ASSERT_NOT_NULL(start);
    ASSERT_EQ_INT(1000, (int)(s.len - (size_t)(start + 4 - s.out)));
    /* Short: the same. */
    slot = begin(&p, 3, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n);
    ASSERT_EQ_INT(10, (int)TAK_ModProxy_Data(&p, slot, body, 10));
    TAK_ModProxy_Done(&p, slot, 1, NULL);
    memset(&s, 0, sizeof s);
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT(s.closed);
    ASSERT_NOT_NULL(strstr(s.out, "Content-Length: 1234"));
}

TEST(an_upstream_failure_is_an_answer_the_page_can_read) {
    small_registry();
    static TAK_ModProxy p;
    TAK_ModProxy_Init(&p, &g_reg, 1);
    char out[1024];
    size_t n = 0;
    int slot = begin(&p, 1, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n);
    TAK_ModProxy_Done(&p, slot, 0, "the mod's host answered \"404\"");
    Sink s;
    memset(&s, 0, sizeof s);
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT_NOT_NULL(strstr(s.out, "HTTP/1.1 502"));
    ASSERT_NOT_NULL(strstr(s.out, "Access-Control-Allow-Origin: *"));
    ASSERT_NOT_NULL(strstr(s.out, "{\"error\":\"the mod's host answered '404'\"}"));
    ASSERT(s.closed);
}

TEST(downloads_past_the_slots_are_turned_away_and_head_needs_none) {
    small_registry();
    static TAK_ModProxy p;
    TAK_ModProxy_Init(&p, &g_reg, 1);
    char out[1024];
    size_t n = 0;
    for (int i = 0; i < TAK_MODPROXY_SLOTS; i++)
        ASSERT(begin(&p, (uint32_t)(i + 1), "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n) >= 0);
    ASSERT_EQ_INT(-1, begin(&p, 99, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n));
    ASSERT_NOT_NULL(strstr(out, "503"));
    ASSERT_EQ_INT(-1, begin(&p, 99, "HEAD /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n));
    ASSERT_NOT_NULL(strstr(out, "200 OK"));
    ASSERT_NOT_NULL(strstr(out, "Content-Length: 1234"));
    ASSERT(TAK_ModProxy_Cancel(&p, 2) >= 0);
    ASSERT_EQ_INT(-1, TAK_ModProxy_Cancel(&p, 2));
    ASSERT(begin(&p, 99, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n) >= 0);
    TAK_ModProxy_Init(&p, &g_reg, 0);
    ASSERT_EQ_INT(-1, begin(&p, 1, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n));
    ASSERT_NOT_NULL(strstr(out, "501"));
}

TEST(a_full_stage_pauses_the_fetch_until_it_drains) {
    small_registry();
    g_reg.mod[0].size = 1u << 20;
    static TAK_ModProxy p;
    TAK_ModProxy_Init(&p, &g_reg, 1);
    char out[1024];
    size_t n = 0;
    int slot = begin(&p, 1, "GET /api/mods/ts/download HTTP/1.1\r\n\r\n", out, sizeof out, &n);
    static char chunk[16384];
    size_t took = 0;
    int pushed = 0;
    while ((took = TAK_ModProxy_Data(&p, slot, chunk, sizeof chunk)) == sizeof chunk) pushed++;
    ASSERT(took == TAK_MODPROXY_PAUSE);
    ASSERT_EQ_INT((int)(TAK_MODPROXY_STAGE / sizeof chunk), pushed);
    ASSERT_EQ_INT(0, TAK_ModProxy_Resume(&p, slot));
    /* A slow reader takes the head a byte at a time and the stage waits. */
    Sink s;
    memset(&s, 0, sizeof s);
    s.take = 1;
    TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
    ASSERT_EQ_INT(0, TAK_ModProxy_Resume(&p, slot));
    s.take = 0;
    int resumed = 0;
    for (int i = 0; i < 1000 && !resumed; i++) {
        s.len = 0;
        TAK_ModProxy_Drain(&p, sink_write, &s, 1000);
        resumed = TAK_ModProxy_Resume(&p, slot);
    }
    ASSERT(resumed);
    ASSERT_EQ_INT((int)sizeof chunk, (int)TAK_ModProxy_Data(&p, slot, chunk, sizeof chunk));
}

TEST(the_relay_serves_its_registry_and_each_rooms_fingerprint) {
    const char *reg = "{\"registry\":1,\"mods\":[]}";
    TAK_Http_SetModRegistry(reg, strlen(reg));
    static TAK_Ledger l;
    TAK_Ledger_Init(&l);
    static TAK_HttpLive live;
    memset(&live, 0, sizeof live);
    live.count = 1;
    snprintf(live.room[0].room.code, sizeof live.room[0].room.code, "ABCD");
    snprintf(live.room[0].room.mod_name, sizeof live.room[0].room.mod_name, "TA:K Enhanced");
    live.room[0].room.content_hash = 0x87f178e645a5a86cULL;
    static char out[TAK_HTTP_RESPONSE_MAX + 1024];
    const char *q = "GET /api/mods HTTP/1.1\r\n\r\n";
    size_t n = TAK_Http_AnswerLive(&l, &live, (const uint8_t *)q, strlen(q), out, sizeof out - 1);
    out[n] = '\0';
    ASSERT_NOT_NULL(strstr(out, "200 OK"));
    ASSERT_NOT_NULL(strstr(out, "\r\n\r\n{\"registry\":1,\"mods\":[]}"));
    q = "GET /api/rooms HTTP/1.1\r\n\r\n";
    n = TAK_Http_AnswerLive(&l, &live, (const uint8_t *)q, strlen(q), out, sizeof out - 1);
    out[n] = '\0';
    ASSERT_NOT_NULL(strstr(out, "\"fingerprint\":\"87f178e645a5a86c\""));
    TAK_Http_SetModRegistry(NULL, 0);
}

int main(void) {
    TEST_SUITE("Mod registry: the file, the check, installs and the relay's stream");
    RUN(the_shipped_registry_is_valid);
    RUN(an_entry_with_a_bad_field_is_left_out_and_said_why);
    RUN(text_that_is_not_a_registry_is_refused);
    RUN(a_room_finds_its_entry_by_fingerprint_then_by_name);
    RUN(a_good_download_passes_and_a_tampered_one_is_refused);
    RUN(an_install_takes_only_mods_and_presets_and_writes_a_manifest);
    RUN(a_preset_mod_gets_its_manifest_beside_the_preset);
    RUN(a_zip_that_climbs_out_of_its_folder_is_refused);
    RUN(a_zip_without_the_mod_set_it_promises_is_refused);
    RUN(remove_deletes_what_the_install_wrote_and_keeps_shared_files);
    RUN(a_tampered_mod_changes_the_data_fingerprint_the_room_checks);
    RUN(only_registry_ids_are_fetched_never_a_url);
    RUN(a_download_streams_with_cors_once_its_first_byte_arrives);
    RUN(a_download_past_its_listed_size_is_cut_off);
    RUN(an_upstream_failure_is_an_answer_the_page_can_read);
    RUN(downloads_past_the_slots_are_turned_away_and_head_needs_none);
    RUN(a_full_stage_pauses_the_fetch_until_it_drains);
    RUN(the_relay_serves_its_registry_and_each_rooms_fingerprint);
    TEST_REPORT();
}
