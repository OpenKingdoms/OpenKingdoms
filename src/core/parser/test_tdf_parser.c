/*
 * test_tdf_parser.c — Unit tests for the tree-based TDF parser (TDFFile API)
 *
 * Tests exercise the public API from tak_tdf.h:
 *   TDF_Open, TDF_Load, TDF_Close,
 *   TDF_PushSection, TDF_PopSection, TDF_OpenSection,
 *   TDF_ReadInt, TDF_ReadFloat, TDF_ReadString
 *
 * Each test writes a temp .tdf file, opens/loads it, and navigates the tree.
 * Most tests will FAIL until tdf_parse_string (called by TDF_Load) is implemented.
 */

#include "test_framework.h"
#include "tak_tdf.h"
#include "tak_hpi.h"
#include "tak_memory.h"

#include <stdio.h>
#include <string.h>

/* ── Helper: write a string to a temp .tdf file ──────────────────────────── */

static const char *TEMP_TDF = "_test_temp.tdf";

static int write_temp_tdf(const char *content) {
    FILE *fp = fopen(TEMP_TDF, "wb");
    if (!fp) return 0;
    fputs(content, fp);
    fclose(fp);
    return 1;
}

static void cleanup_temp(void) {
    remove(TEMP_TDF);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  TDF_Open / TDF_Close basics
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(open_nonexistent_file_returns_null) {
    TDFFile *tdf = TDF_Open("__no_such_file_ever__.tdf");
    ASSERT_NULL(tdf);
}

TEST(open_valid_file_returns_handle) {
    write_temp_tdf("[HEADER]\n{\n    side=Aramon;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(close_null_does_not_crash) {
    /* Should be a no-op, not a segfault */
    TDF_Close(NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  TDF_Load: parse basics
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(load_single_empty_section) {
    write_temp_tdf("[HEADER]\n{\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    int rc = TDF_Load(tdf);
    ASSERT_EQ_INT(0, rc);
    int result = TDF_PushSection(tdf, "HEADER");
    ASSERT_EQ_INT(0, result);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(load_section_with_one_entry) {
    write_temp_tdf(
        "[HEADER]\n"
        "{\n"
        "    campaignside=Aramon;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    int rc = TDF_Load(tdf);
    ASSERT_EQ_INT(0, rc);
    /* Navigate into HEADER and read the value */
    ASSERT(TDF_PushSection(tdf, "HEADER") > -1);
    ASSERT_EQ_STR("Aramon", TDF_ReadString(tdf, "campaignside", NULL));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(load_multiple_sections) {
    write_temp_tdf(
        "[HEADER]\n"
        "{\n"
        "    campaignside=Aramon;\n"
        "}\n"
        "\n"
        "[MISSION0]\n"
        "{\n"
        "    missionfile=MyFirstMap.ota;\n"
        "    missionname=The Beginning;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    int rc = TDF_Load(tdf);
    ASSERT_EQ_INT(0, rc);

    ASSERT(TDF_PushSection(tdf, "HEADER") > -1);
    ASSERT_EQ_STR("Aramon", TDF_ReadString(tdf, "campaignside", NULL));
    TDF_PopSection(tdf);

    ASSERT(TDF_PushSection(tdf, "MISSION0") > -1);
    ASSERT_EQ_STR("MyFirstMap.ota", TDF_ReadString(tdf, "missionfile", NULL));
    ASSERT_EQ_STR("The Beginning", TDF_ReadString(tdf, "missionname", NULL));
    TDF_PopSection(tdf);

    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Key-value parsing details
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(values_are_trimmed) {
    write_temp_tdf(
        "[S]\n"
        "{\n"
        "    key =  some value ;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "S") > -1);
    ASSERT_EQ_STR("some value", TDF_ReadString(tdf, "key", NULL));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(values_with_spaces_preserved) {
    write_temp_tdf(
        "[MISSION0]\n"
        "{\n"
        "    missionname=The Beginning;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "MISSION0") > -1);
    ASSERT_EQ_STR("The Beginning", TDF_ReadString(tdf, "missionname", NULL));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Comments and whitespace
 * ══════════════════════════════════════════════════════════════════════════ */

/* Files from anywhere reach this parser now that map packs do, so a
 * malformed one has to come back as a tree, not as a crash. */
TEST(malformed_braces_do_not_walk_off_the_stack) {
    const char *src =
        "[a]\n"
        "}\n"                       /* close with nothing open */
        "{\n"                       /* open with no section */
        "stray=1;\n"
        "[b]\n"
        "{\n"
        "key=2;\n"
        "}\n";
    ASSERT(write_temp_tdf(src));
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "b"));
    ASSERT_EQ_INT(2, TDF_ReadInt(tdf, "key", -1));
    TDF_Close(tdf);
    cleanup_temp();
}

/* Nesting past the parser stack is ignored rather than trusted. */
TEST(nesting_past_the_stack_is_ignored) {
    char src[4096];
    size_t n = 0;
    for (int i = 0; i < 40; i++)
        n += (size_t)snprintf(src + n, sizeof(src) - n, "[s%d]\n{\n", i);
    n += (size_t)snprintf(src + n, sizeof(src) - n, "deep=7;\n");
    for (int i = 0; i < 40; i++)
        n += (size_t)snprintf(src + n, sizeof(src) - n, "}\n");
    ASSERT(write_temp_tdf(src));
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "s0"));
    TDF_Close(tdf);
    cleanup_temp();
}
TEST(line_comments_are_ignored) {
    write_temp_tdf(
        "[HEADER]\n"
        "{\n"
        "    // This is a comment\n"
        "    campaignside=Aramon;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "HEADER") > -1);
    ASSERT_EQ_STR("Aramon", TDF_ReadString(tdf, "campaignside", NULL));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

/* A value may hold a '=' or a ';' of its own, as several entries in
 * english/translate/messages.tdf do. The file still loads, the '=' stays
 * in the value and the ';' ends it. */
TEST(values_may_contain_separator_characters) {
    write_temp_tdf("[MSG]\n{\n"
                   "    // a comment; with = signs\n"
                   "    English = Your version = %s (%i);\n"
                   "    French = Vous etes elimine; suite ?;\n"
                   "    Next = ok;\n"
                   "}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "MSG"));
    ASSERT_EQ_STR("Your version = %s (%i)", TDF_ReadString(tdf, "English", ""));
    ASSERT_EQ_STR("Vous etes elimine", TDF_ReadString(tdf, "French", ""));
    ASSERT_EQ_STR("ok", TDF_ReadString(tdf, "Next", ""));
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(blank_lines_are_ignored) {
    write_temp_tdf(
        "\n"
        "\n"
        "[HEADER]\n"
        "{\n"
        "\n"
        "    campaignside=Aramon;\n"
        "\n"
        "}\n"
        "\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "HEADER") > -1);
    ASSERT_EQ_STR("Aramon", TDF_ReadString(tdf, "campaignside", NULL));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Value readers (int, float, defaults)
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(read_int_value) {
    write_temp_tdf(
        "[UNITINFO]\n"
        "{\n"
        "    maxdamage=1100;\n"
        "    buildcost=325;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "UNITINFO") > -1);
    ASSERT_EQ_INT(1100, TDF_ReadInt(tdf, "maxdamage", 0));
    ASSERT_EQ_INT(325, TDF_ReadInt(tdf, "buildcost", 0));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(read_int_returns_default_for_missing_key) {
    write_temp_tdf("[S]\n{\n    x=1;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "S") > -1);
    ASSERT_EQ_INT(42, TDF_ReadInt(tdf, "nonexistent", 42));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(read_string_returns_default_for_missing_key) {
    write_temp_tdf("[S]\n{\n    x=1;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "S") > -1);
    ASSERT_EQ_STR("fallback", TDF_ReadString(tdf, "nope", "fallback"));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Section navigation
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(push_section_finds_existing) {
    write_temp_tdf(
        "[MISSION0]\n"
        "{\n"
        "    missionfile=map.ota;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT(TDF_PushSection(tdf, "MISSION0") > -1);
    ASSERT_EQ_STR("map.ota", TDF_ReadString(tdf, "missionfile", NULL));
    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(push_section_returns_zero_for_missing) {
    write_temp_tdf("[A]\n{\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(-1, TDF_PushSection(tdf, "NOPE"));
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(open_section_by_index) {
    write_temp_tdf(
        "[FIRST]\n{\n    a=1;\n}\n"
        "[SECOND]\n{\n    b=2;\n}\n"
        "[THIRD]\n{\n    c=3;\n}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    /* OpenSection(0) should enter the first top-level section */
    ASSERT_EQ_INT(0, TDF_OpenSection(tdf, 1));
    ASSERT_EQ_STR("2", TDF_ReadString(tdf, "b", NULL));
    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Nested subsections (tree model)
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(parse_nested_subsection) {
    /* From test.tdf: [MISSION0] contains [MISSIONSUBSECTION] inside it.
     * The tree model represents this as a child section node under MISSION0. */
    write_temp_tdf(
        "[MISSION0]\n"
        "{\n"
        "    missionfile=MyFirstMap.ota;\n"
        "    missionname=The Beginning;\n"
        "\n"
        "    [MISSIONSUBSECTION]\n"
        "    {\n"
        "        missionname=Subsection Mission;\n"
        "    }\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));

    /* Read MISSION0's own entries */
    ASSERT(TDF_PushSection(tdf, "MISSION0") > -1);
    ASSERT_EQ_STR("MyFirstMap.ota", TDF_ReadString(tdf, "missionfile", NULL));
    ASSERT_EQ_STR("The Beginning", TDF_ReadString(tdf, "missionname", NULL));

    /* Descend into the nested subsection */
    ASSERT(TDF_PushSection(tdf, "MISSIONSUBSECTION") > -1);
    ASSERT_EQ_STR("Subsection Mission", TDF_ReadString(tdf, "missionname", NULL));
    TDF_PopSection(tdf);

    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(parse_deeply_nested_weapon_damage) {
    /* Real TA:K pattern: WEAPON1 contains a DAMAGE subsection */
    write_temp_tdf(
        "[WEAPON1]\n"
        "{\n"
        "    name=Bow and Arrows;\n"
        "    range=450;\n"
        "    reloadtime=3;\n"
        "\n"
        "    [DAMAGE]\n"
        "    {\n"
        "        default=213;\n"
        "    }\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));

    ASSERT(TDF_PushSection(tdf, "WEAPON1") > -1);
    ASSERT_EQ_INT(450, TDF_ReadInt(tdf, "range", 0));
    ASSERT_EQ_STR("Bow and Arrows", TDF_ReadString(tdf, "name", NULL));

    /* Descend into DAMAGE */
    ASSERT(TDF_PushSection(tdf, "DAMAGE") > -1);
    ASSERT_EQ_INT(213, TDF_ReadInt(tdf, "default", 0));
    TDF_PopSection(tdf);

    TDF_PopSection(tdf);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(parse_full_test_tdf_structure) {
    /* Mirrors the complete test.tdf file */
    write_temp_tdf(
        "[HEADER]\n"
        "{\n"
        "    // The side the player plays as\n"
        "    campaignside=Aramon;\n"
        "}\n"
        "\n"
        "[MISSION0]\n"
        "{\n"
        "    missionfile=MyFirstMap.ota;\n"
        "    missionname=The Beginning;\n"
        "    briefing=brief1.txt;\n"
        "\n"
        "    [MISSIONSUBSECTION]\n"
        "    {\n"
        "        missionname=Subsection Mission;\n"
        "    }\n"
        "}\n"
        "\n"
        "[MISSION1]\n"
        "{\n"
        "    missionfile=MySecondMap.ota;\n"
        "    missionname=The Counterattack;\n"
        "    briefing=brief2.txt;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    ASSERT_EQ_INT(0, TDF_Load(tdf));

    ASSERT(TDF_PushSection(tdf, "HEADER") > -1);
    ASSERT_EQ_STR("Aramon", TDF_ReadString(tdf, "campaignside", NULL));
    TDF_PopSection(tdf);

    ASSERT(TDF_PushSection(tdf, "MISSION0") > -1);
    ASSERT_EQ_STR("The Beginning", TDF_ReadString(tdf, "missionname", NULL));
    ASSERT(TDF_PushSection(tdf, "MISSIONSUBSECTION") > -1);
    ASSERT_EQ_STR("Subsection Mission", TDF_ReadString(tdf, "missionname", NULL));
    TDF_PopSection(tdf);
    TDF_PopSection(tdf);

    ASSERT(TDF_PushSection(tdf, "MISSION1") > -1);
    ASSERT_EQ_STR("The Counterattack", TDF_ReadString(tdf, "missionname", NULL));
    TDF_PopSection(tdf);

    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Error cases
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(load_unclosed_section_returns_error) {
    write_temp_tdf("[HEADER]\n{\n    key=val;\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    int rc = TDF_Load(tdf);
    ASSERT_EQ_INT(-1, rc);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(load_missing_open_brace_returns_error) {
    write_temp_tdf("[HEADER]\n    key=val;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_NOT_NULL(tdf);
    int rc = TDF_Load(tdf);
    ASSERT_EQ_INT(-1, rc);
    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  TDF_ReadStringList
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(read_string_list_splits_comma_separated) {
    write_temp_tdf(
        "[UNITINFO]\n"
        "{\n"
        "    types=Hammer, Anvil, Shield;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "UNITINFO"));

    char **items = NULL;
    int count = 0;
    int rc = TDF_ReadStringList(tdf, "types", &items, &count);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(3, count);
    ASSERT_EQ_STR("Hammer", items[0]);
    ASSERT_EQ_STR("Anvil",  items[1]);
    ASSERT_EQ_STR("Shield", items[2]);

    TDF_FreeStringList(items, count);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(read_string_list_single_item) {
    write_temp_tdf("[S]\n{\n    x=onlyone;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "S"));

    char **items = NULL;
    int count = 0;
    ASSERT_EQ_INT(0, TDF_ReadStringList(tdf, "x", &items, &count));
    ASSERT_EQ_INT(1, count);
    ASSERT_EQ_STR("onlyone", items[0]);

    TDF_FreeStringList(items, count);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(read_string_list_empty_items_dropped) {
    write_temp_tdf("[S]\n{\n    x=a,,b;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "S"));

    char **items = NULL;
    int count = 0;
    ASSERT_EQ_INT(0, TDF_ReadStringList(tdf, "x", &items, &count));
    ASSERT_EQ_INT(2, count);
    ASSERT_EQ_STR("a", items[0]);
    ASSERT_EQ_STR("b", items[1]);

    TDF_FreeStringList(items, count);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(read_string_list_trims_whitespace) {
    write_temp_tdf("[S]\n{\n    x=  foo  ,  bar   ;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "S"));

    char **items = NULL;
    int count = 0;
    ASSERT_EQ_INT(0, TDF_ReadStringList(tdf, "x", &items, &count));
    ASSERT_EQ_INT(2, count);
    ASSERT_EQ_STR("foo", items[0]);
    ASSERT_EQ_STR("bar", items[1]);

    TDF_FreeStringList(items, count);
    TDF_Close(tdf);
    cleanup_temp();
}

TEST(read_string_list_missing_key_returns_minus_one) {
    write_temp_tdf("[S]\n{\n    other=foo;\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "S"));

    char **items = (char **)0xDEADBEEF;
    int count = 42;
    int rc = TDF_ReadStringList(tdf, "nope", &items, &count);
    ASSERT_EQ_INT(-1, rc);
    ASSERT_NULL(items);
    ASSERT_EQ_INT(0, count);

    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Iteration: TDF_GetFirst/NextSection, TDF_GetFirst/NextKey
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(iterate_sections) {
    write_temp_tdf(
        "[ROOT]\n{\n"
        "  [ALPHA]\n  {\n  }\n"
        "  [BETA]\n  {\n  }\n"
        "  [GAMMA]\n  {\n  }\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "ROOT"));

    const char *s = TDF_GetFirstSection(tdf);
    ASSERT_EQ_STR("ALPHA", s);
    s = TDF_GetNextSection(tdf);
    ASSERT_EQ_STR("BETA", s);
    s = TDF_GetNextSection(tdf);
    ASSERT_EQ_STR("GAMMA", s);
    s = TDF_GetNextSection(tdf);
    ASSERT_NULL(s);

    TDF_Close(tdf);
    cleanup_temp();
}

TEST(iterate_keys_skips_sections) {
    write_temp_tdf(
        "[ROOT]\n{\n"
        "  hp=100;\n"
        "  armor=50;\n"
        "  [CHILD]\n  {\n  }\n"
        "  speed=25;\n"
        "}\n"
    );
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "ROOT"));

    const char *k = TDF_GetFirstKey(tdf);
    ASSERT_EQ_STR("hp", k);
    k = TDF_GetNextKey(tdf);
    ASSERT_EQ_STR("armor", k);
    k = TDF_GetNextKey(tdf);
    /* [CHILD] should be skipped by the key iterator */
    ASSERT_EQ_STR("speed", k);
    k = TDF_GetNextKey(tdf);
    ASSERT_NULL(k);

    TDF_Close(tdf);
    cleanup_temp();
}

TEST(iterate_first_section_on_empty_returns_null) {
    write_temp_tdf("[EMPTY]\n{\n}\n");
    TDFFile *tdf = TDF_Open(TEMP_TDF);
    ASSERT_EQ_INT(0, TDF_Load(tdf));
    ASSERT_EQ_INT(0, TDF_PushSection(tdf, "EMPTY"));
    ASSERT_NULL(TDF_GetFirstSection(tdf));
    ASSERT_NULL(TDF_GetFirstKey(tdf));
    TDF_Close(tdf);
    cleanup_temp();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    /* TDF_Open now reads through the VFS. Point the loose-data root at
     * the current working directory so the tests' temp TDF files (e.g.
     * "_test_temp.tdf") are reachable via VFS_ReadFile. */
    tak_mem_init();
    VFS_Init(".", ".");

    TEST_SUITE("TDF_Open / TDF_Close");
    RUN(open_nonexistent_file_returns_null);
    RUN(open_valid_file_returns_handle);
    RUN(close_null_does_not_crash);

    TEST_SUITE("TDF_Load: parse basics");
    RUN(load_single_empty_section);
    RUN(load_section_with_one_entry);
    RUN(load_multiple_sections);

    TEST_SUITE("Key-value details");
    RUN(values_are_trimmed);
    RUN(values_with_spaces_preserved);

    TEST_SUITE("Comments and whitespace");
    RUN(line_comments_are_ignored);
    RUN(malformed_braces_do_not_walk_off_the_stack);
    RUN(nesting_past_the_stack_is_ignored);
    RUN(blank_lines_are_ignored);
    RUN(values_may_contain_separator_characters);

    TEST_SUITE("Value readers");
    RUN(read_int_value);
    RUN(read_int_returns_default_for_missing_key);
    RUN(read_string_returns_default_for_missing_key);

    TEST_SUITE("Section navigation");
    RUN(push_section_finds_existing);
    RUN(push_section_returns_zero_for_missing);
    RUN(open_section_by_index);

    TEST_SUITE("Nested subsections");
    RUN(parse_nested_subsection);
    RUN(parse_deeply_nested_weapon_damage);
    RUN(parse_full_test_tdf_structure);

    TEST_SUITE("Error handling");
    RUN(load_unclosed_section_returns_error);
    RUN(load_missing_open_brace_returns_error);

    TEST_SUITE("TDF_ReadStringList");
    RUN(read_string_list_splits_comma_separated);
    RUN(read_string_list_single_item);
    RUN(read_string_list_empty_items_dropped);
    RUN(read_string_list_trims_whitespace);
    RUN(read_string_list_missing_key_returns_minus_one);

    TEST_SUITE("Iteration");
    RUN(iterate_sections);
    RUN(iterate_keys_skips_sections);
    RUN(iterate_first_section_on_empty_returns_null);

    VFS_Shutdown();
    tak_mem_shutdown();

    TEST_REPORT();
}
