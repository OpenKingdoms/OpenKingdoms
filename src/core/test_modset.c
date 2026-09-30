/*
 * test_modset.c -- a mod's manifest, and what the lobby says about a
 * game whose data differs from ours.
 *
 * Data free. The scan case writes a few text files into a scratch folder
 * beside the binary and removes them afterwards.
 */

#include "test_framework.h"
#include "tak_modset.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int test_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int test_rmdir(const char *path) {
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static void write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(text, 1, strlen(text), fp); fclose(fp); }
}

#define ROOT "test_modset_tmp"

static void root_remove(void) {
    remove(ROOT "/Mods/Tough Swords/mod.tdf");
    remove(ROOT "/Mods/te.hpi");
    test_rmdir(ROOT "/Mods/Tough Swords");
    test_rmdir(ROOT "/Mods");
    remove(ROOT "/TAKEnhanced/Presets/tak-enhanced.preset.json");
    remove(ROOT "/TAKEnhanced/Presets/tak-enhanced.mod.tdf");
    test_rmdir(ROOT "/TAKEnhanced/Presets");
    test_rmdir(ROOT "/TAKEnhanced");
    test_rmdir(ROOT);
}

static TAK_ModSet set(const char *id, const char *name, const char *version, uint64_t fp) {
    TAK_ModSet m;
    memset(&m, 0, sizeof m);
    snprintf(m.id, sizeof m.id, "%s", id);
    snprintf(m.name, sizeof m.name, "%s", name);
    snprintf(m.version, sizeof m.version, "%s", version);
    m.fingerprint = fp;
    m.count = 1;
    return m;
}

TEST(a_manifest_names_the_mod_its_version_and_its_fingerprint) {
    TAK_ModSet m;
    memset(&m, 0, sizeof m);
    TAK_ModSet_ReadManifest("[MOD]\r\n{\r\n\tname=Tough Swords;\r\n\tversion=0.1;\r\n"
                            "\tfingerprint=00C0FFEE12345678;\r\n}\r\n", &m);
    ASSERT_EQ_STR("Tough Swords", m.name);
    ASSERT_EQ_STR("0.1", m.version);
    ASSERT(m.fingerprint == 0x00c0ffee12345678ull);

    /* What a set already has is kept, and a fingerprint that is not
     * sixteen hex digits or fewer is none. */
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "Kept");
    TAK_ModSet_ReadManifest("name=Other\nfingerprint=12345678901234567\n", &m);
    ASSERT_EQ_STR("Kept", m.name);
    ASSERT(m.fingerprint == 0);
    TAK_ModSet_ReadManifest("fingerprint=not hex\n", &m);
    ASSERT(m.fingerprint == 0);
    TAK_ModSet_ReadManifest("fingerprint= abc \n", &m);
    ASSERT(m.fingerprint == 0xabc);
}

/* A preset takes its manifest from beside it, and the manifest wins. */
TEST(the_scan_reads_a_folder_manifest_and_one_beside_a_preset) {
    root_remove();
    test_mkdir(ROOT);
    test_mkdir(ROOT "/Mods");
    test_mkdir(ROOT "/Mods/Tough Swords");
    test_mkdir(ROOT "/TAKEnhanced");
    test_mkdir(ROOT "/TAKEnhanced/Presets");
    write_text(ROOT "/Mods/te.hpi", "not really an archive");
    write_text(ROOT "/Mods/Tough Swords/mod.tdf",
               "name=Tough Swords;\nversion=0.1;\nfingerprint=1111222233334444;\n");
    write_text(ROOT "/TAKEnhanced/Presets/tak-enhanced.preset.json",
               "{ \"id\": \"tak-enhanced\", \"name\": \"TA:K Enhanced\", \"mods\": "
               "{ \"enabled\": true, \"selectedMods\": [ \"te.hpi\" ] } }");
    write_text(ROOT "/TAKEnhanced/Presets/tak-enhanced.mod.tdf",
               "name=TAK Enhanced;\nversion=1.4;\nfingerprint=e4a1000000000014;\n");
    TAK_ModSet sets[8];
    int n = TAK_ModSet_Scan(ROOT, sets, 8);
    const TAK_ModSet *te = TAK_ModSet_Find(sets, n, "tak-enhanced");
    const TAK_ModSet *ts = TAK_ModSet_Find(sets, n, "tough-swords");
    TAK_ModSet te_copy, ts_copy;
    memset(&te_copy, 0, sizeof te_copy);
    memset(&ts_copy, 0, sizeof ts_copy);
    if (te) te_copy = *te;
    if (ts) ts_copy = *ts;
    root_remove();
    ASSERT_EQ_INT(3, n);
    ASSERT_NOT_NULL(te);
    ASSERT_EQ_STR("TAK Enhanced", te_copy.name);
    ASSERT_EQ_STR("1.4", te_copy.version);
    ASSERT(te_copy.fingerprint == 0xe4a1000000000014ull);
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ_STR("Tough Swords", ts_copy.name);
    ASSERT(ts_copy.fingerprint == 0x1111222233334444ull);
    ASSERT(sets[0].fingerprint == 0);
}

TEST(the_active_set_is_named_whole_and_in_its_two_parts) {
    TAK_ModSet te = set("tak-enhanced", "TAK Enhanced", "1.4", 0xe4a1);
    TAK_ModSet_SetActive(&te);
    ASSERT_EQ_STR("TAK Enhanced 1.4", TAK_ModSet_ActiveName());
    ASSERT_EQ_STR("TAK Enhanced", TAK_ModSet_ActiveModName());
    ASSERT_EQ_STR("1.4", TAK_ModSet_ActiveVersion());
    ASSERT(TAK_ModSet_ActiveFingerprint() == 0xe4a1);
    TAK_ModSet_SetActive(NULL);
    ASSERT_EQ_STR("Vanilla", TAK_ModSet_ActiveModName());
    ASSERT_EQ_STR("", TAK_ModSet_ActiveVersion());
    ASSERT_EQ_INT(1, TAK_ModSet_IsVanilla());

    char label[64];
    TAK_ModSet_Label("TAK Enhanced", "1.4", label, sizeof label);
    ASSERT_EQ_STR("TAK Enhanced 1.4", label);
    TAK_ModSet_Label("Vanilla", "", label, sizeof label);
    ASSERT_EQ_STR("Vanilla", label);
    TAK_ModSet_Label("", "1.0", label, sizeof label);
    ASSERT_EQ_STR("", label);
}

/* A greyed row says which mod set it needs instead of only that the
 * data differs: whether we lack it, have another version, have it and
 * need only choose it, or chose it and hold a copy that differs (#284). */
TEST(a_greyed_row_names_the_mod_set_it_needs) {
    TAK_ModSet sets[4];
    sets[0] = set("vanilla", "Vanilla", "", 0);
    sets[1] = set("tak-enhanced", "TAK Enhanced", "1.3", 0);
    sets[2] = set("swords", "Tough Swords", "0.1", 0x5555);
    sets[3] = set("renamed", "My Copy", "", 0xe4a1000000000014ull);
    TAK_ModSet_SetInstalled(sets, 4);
    TAK_ModSet_SetActive(NULL);
    char out[192];

    /* A host from before protocol 4 named nothing. */
    TAK_ModSet_JoinAdvice("", "", 0x99, out, sizeof out);
    ASSERT_EQ_STR("That game's data differs from yours (Vanilla).", out);
    TAK_ModSet_JoinAdvice("Big Mod", "2", 0x99, out, sizeof out);
    ASSERT_EQ_STR("That game plays Big Mod 2, which you do not have.", out);
    TAK_ModSet_JoinAdvice("TAK Enhanced", "1.4", 0x77, out, sizeof out);
    ASSERT_EQ_STR("That game plays TAK Enhanced 1.4, and you have TAK Enhanced 1.3.", out);
    TAK_ModSet_JoinAdvice("tak enhanced", "1.3", 0x77, out, sizeof out);
    ASSERT_EQ_STR("That game plays tak enhanced 1.3. Choose it as your mod set to join.", out);
    /* The manifest's fingerprint finds it under any name. */
    TAK_ModSet_JoinAdvice("TAK Enhanced", "1.4", 0xe4a1000000000014ull, out, sizeof out);
    ASSERT_EQ_STR("That game plays TAK Enhanced 1.4, which is your My Copy. Choose it to join.", out);
    /* A named version whose manifest says other data is another build. */
    TAK_ModSet_JoinAdvice("Tough Swords", "0.1", 0x6666, out, sizeof out);
    ASSERT_EQ_STR("That game plays Tough Swords 0.1, and your copy of it differs.", out);
    /* Vanilla on other data, and vanilla from a modded game. */
    TAK_ModSet_JoinAdvice("Vanilla", "", 0x1234, out, sizeof out);
    ASSERT_EQ_STR("That game plays Vanilla on game data other than yours.", out);
    TAK_ModSet_SetActive(&sets[2]);
    TAK_ModSet_JoinAdvice("Vanilla", "", 0x1234, out, sizeof out);
    ASSERT_EQ_STR("That game plays Vanilla. Choose it as your mod set to join.", out);
    TAK_ModSet_JoinAdvice("Tough Swords", "0.1", 0x5555, out, sizeof out);
    ASSERT_EQ_STR("That game plays Tough Swords 0.1, and your copy of it differs.", out);

    /* Nothing scanned still knows the game itself and what is mounted. */
    TAK_ModSet_SetInstalled(NULL, 0);
    TAK_ModSet_JoinAdvice("Vanilla", "", 0x1234, out, sizeof out);
    ASSERT_EQ_STR("That game plays Vanilla. Choose it as your mod set to join.", out);
    TAK_ModSet_JoinAdvice("Tough Swords", "0.1", 0x1234, out, sizeof out);
    ASSERT_EQ_STR("That game plays Tough Swords 0.1, and your copy of it differs.", out);
    TAK_ModSet_SetActive(NULL);
}

int main(void) {
    TEST_SUITE("Mod sets: manifests and the lobby's words");
    RUN(a_manifest_names_the_mod_its_version_and_its_fingerprint);
    RUN(the_scan_reads_a_folder_manifest_and_one_beside_a_preset);
    RUN(the_active_set_is_named_whole_and_in_its_two_parts);
    RUN(a_greyed_row_names_the_mod_set_it_needs);
    TEST_REPORT();
}
