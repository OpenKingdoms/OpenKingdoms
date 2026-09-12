/*
 * test_sides.c -- The side table read from gamedata/sidedata.tdf, the
 * side button and the side setter. Synthetic archives only, so it runs
 * without game data.
 */

#include "test_framework.h"
#include "test_hpi_builder.h"
#include "tak_sides.h"
#include "tak_dataset.h"
#include "tak_hpi.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define sd_mkdir(p) _mkdir(p)
#define sd_rmdir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define sd_mkdir(p) mkdir(p, 0755)
#define sd_rmdir(p) rmdir(p)
#endif

#define SD_DIR "test_sides_tmp"

#define SD_KINGDOMS \
    "[SIDE0]{name=ARAMON;nameprefix=ARA;commander=ARAKING;" \
    "logogaf=colorlogos2;logoart=arateam;buildsparklygaf=aramonbuild;" \
    "buildsparklyanim=aramonbuild;resurrectsparklygaf=aramonbuild;" \
    "resurrectsparklyanim=aramonbuild;}" \
    "[SIDE1]{name=TAROS;nameprefix=TAR;commander=TARNECRO;}" \
    "[SIDE2]{name=VERUNA;nameprefix=VER;commander=VERMAGE;}" \
    "[SIDE3]{name=ZHON;nameprefix=ZON;commander=ZONHUNT;}"

#define SD_UNPLAYABLE \
    "[SIDE4]{name=LIFEFORMS;nameprefix=LIF;}" \
    "[SIDE5]{name=NONPLAYERCHARACTERS;nameprefix=NPC;}" \
    "[SIDE6]{name=WANDERING_MONSTERS;nameprefix=MON;}"

#define SD_CREON \
    "[SIDE7]{name=CREON;nameprefix=CRE;commander=CRESAGE;" \
    "logogaf=colorlogos2;logoart=creteam;buildsparklygaf=creonbuild;" \
    "buildsparklyanim=creonbuild;resurrectsparklygaf=creonbuild;" \
    "resurrectsparklyanim=creonbuild;}"

static const char sd_base_sides[] = SD_KINGDOMS SD_UNPLAYABLE;
static const char sd_ip_sides[] = SD_KINGDOMS SD_UNPLAYABLE SD_CREON;
/* A made-up fifth playable side right after Zhon. */
static const char sd_fifth_side[] = SD_KINGDOMS
    "[SIDE4]{name=ELVES;nameprefix=ELF;commander=ELFKING;}";

/* The TDF reader takes one construct to a line, as .tdf files are
 * written, so spread a compact "[A]{k=v;}" fixture over lines. */
static const char *sd_lines(const char *compact) {
    static char buf[4][2048];
    static int slot = 0;
    char *out = buf[slot];
    slot = (slot + 1) % 4;
    size_t n = 0;
    for (const char *c = compact; *c && n + 3 < sizeof(buf[0]); c++) {
        if (*c == '{' || *c == '}') out[n++] = 10;
        out[n++] = *c;
        if (*c == ']' || *c == '{' || *c == '}' || *c == ';') out[n++] = 10;
    }
    out[n] = 0;
    return out;
}

static void sd_cleanup(void) {
    VFS_Shutdown();
    remove(SD_DIR "/data.hpi");
    remove(SD_DIR "/IPData.hpi");
    sd_rmdir(SD_DIR);
}

/* data.hpi holds the base sidedata. IPData.hpi, when `sides` is given,
 * holds a newer sidedata and, when `expansion`, the two campaign files
 * that make the install Iron Plague. */
static int sd_mount(const char *sides, int expansion) {
    sd_cleanup();
    sd_mkdir(SD_DIR);
    TestHPIEntry base[1] = {
        { "gamedata/sidedata.tdf", sd_lines(sd_base_sides), 1000, 0 }
    };
    if (test_write_hpi(SD_DIR "/data.hpi", base, 1) != 0) return -1;
    TestHPIEntry ip[3];
    int n = 0;
    if (sides) {
        TestHPIEntry e = { "gamedata/sidedata.tdf", sd_lines(sides), 2000, 0 };
        ip[n++] = e;
    }
    if (expansion) {
        TestHPIEntry c = { "camps/the iron plague.tdf", "[HEADER]{}", 2000, 0 };
        TestHPIEntry a = { "camps/ipalt.tdf", "[HEADER]{}", 2000, 0 };
        ip[n++] = c;
        ip[n++] = a;
    }
    if (n > 0 && test_write_hpi(SD_DIR "/IPData.hpi", ip, n) != 0) return -1;
    return VFS_Init(SD_DIR, NULL);
}

static const char *sd_name(int side) {
    static char buf[64];
    Sides_DisplayName(side, buf, sizeof(buf));
    return buf;
}

TEST(base_sides_offer_the_four_kingdoms) {
    ASSERT_EQ_INT(0, sd_mount(NULL, 0));
    ASSERT_EQ_INT(7, Sides_Count());
    ASSERT_EQ_INT(1, Sides_Next(0));
    ASSERT_EQ_INT(2, Sides_Next(1));
    ASSERT_EQ_INT(3, Sides_Next(2));
    ASSERT_EQ_INT(0, Sides_Next(3));
    ASSERT_EQ_INT(0, Sides_Cycle(3, TAK_SIDES_SKIRMISH, 0));
    ASSERT_EQ_INT(0, Sides_IsPlayable(4));
    ASSERT_EQ_INT(0, Sides_IsPlayable(6));
    ASSERT_EQ_STR("Aramon", sd_name(0));
    ASSERT_EQ_STR("Zhon", sd_name(3));
    ASSERT_EQ_STR("", sd_name(7));
    ASSERT_EQ_INT(-1, Sides_FindByPrefix("CRE"));
    const TakSideInfo *ara = Sides_Get(0);
    ASSERT_NOT_NULL(ara);
    ASSERT_EQ_STR("ARA", ara->prefix);
    ASSERT_EQ_STR("ARAKING", ara->commander);
    ASSERT_EQ_STR("arateam", ara->logoart);
    ASSERT_EQ_STR("aramonbuild", ara->buildsparkle);
    ASSERT_EQ_STR("aramonbuild", ara->resurrectsparkle);
    sd_cleanup();
}

TEST(iron_plague_sides_add_creon_as_side_seven) {
    ASSERT_EQ_INT(0, sd_mount(sd_ip_sides, 1));
    ASSERT_EQ_INT(8, Sides_Count());
    ASSERT_EQ_INT(7, Sides_Next(3));
    ASSERT_EQ_INT(0, Sides_Next(7));
    ASSERT_EQ_INT(7, Sides_Cycle(3, TAK_SIDES_SKIRMISH, 0));
    ASSERT_EQ_INT(0, Sides_Cycle(7, TAK_SIDES_SKIRMISH, 0));
    ASSERT_EQ_STR("Creon", sd_name(7));
    ASSERT_EQ_INT(7, Sides_FindByPrefix("cre"));
    ASSERT_EQ_INT(7, Sides_FindByName("creon"));
    const TakSideInfo *cre = Sides_Get(7);
    ASSERT_NOT_NULL(cre);
    ASSERT_EQ_STR("CRESAGE", cre->commander);
    ASSERT_EQ_STR("creteam", cre->logoart);
    ASSERT_EQ_STR("creonbuild", cre->buildsparkle);
    ASSERT_EQ_STR("creonbuild", cre->resurrectsparkle_anim);
    sd_cleanup();
}

/* The list is whatever the data says: a fifth side with a commander is
 * reached by the button, and like any side past the fourth it is kept
 * only when the expansion is present (legacy:134933-134936). */
TEST(a_fifth_playable_side_from_data_is_offered) {
    ASSERT_EQ_INT(0, sd_mount(sd_fifth_side, 1));
    ASSERT_EQ_INT(5, Sides_Count());
    ASSERT_EQ_INT(4, Sides_Next(3));
    ASSERT_EQ_INT(0, Sides_Next(4));
    ASSERT_EQ_INT(4, Sides_Cycle(3, TAK_SIDES_SKIRMISH, 0));
    ASSERT_EQ_STR("Elves", sd_name(4));

    ASSERT_EQ_INT(0, sd_mount(sd_fifth_side, 0));
    ASSERT_EQ_INT(4, Sides_Next(3));
    ASSERT_EQ_INT(0, Sides_Cycle(3, TAK_SIDES_SKIRMISH, 0));
    sd_cleanup();
}

TEST(the_setter_follows_the_game_mode) {
    ASSERT_EQ_INT(0, sd_mount(sd_ip_sides, 0));
    ASSERT_EQ_INT(7, Sides_Set(7, TAK_SIDES_CAMPAIGN, 0));
    ASSERT_EQ_INT(0, Sides_Set(7, TAK_SIDES_SKIRMISH, 0));
    ASSERT_EQ_INT(0, Sides_Set(7, TAK_SIDES_MULTIPLAYER, 1));
    ASSERT_EQ_INT(3, Sides_Set(3, TAK_SIDES_SKIRMISH, 0));

    ASSERT_EQ_INT(0, sd_mount(sd_ip_sides, 1));
    ASSERT_EQ_INT(7, Sides_Set(7, TAK_SIDES_SKIRMISH, 0));
    ASSERT_EQ_INT(0, Sides_Set(7, TAK_SIDES_MULTIPLAYER, 0));
    ASSERT_EQ_INT(7, Sides_Set(7, TAK_SIDES_MULTIPLAYER, 1));
    ASSERT_EQ_INT(3, Sides_Set(3, TAK_SIDES_MULTIPLAYER, 0));
    sd_cleanup();
}

/* -pretendnoexpansion leaves SIDE7 in the table, so the button still
 * reaches it and the setter turns it back to Aramon. */
TEST(pretend_no_expansion_turns_creon_back) {
    ASSERT_EQ_INT(0, sd_mount(sd_ip_sides, 1));
    TAK_DataSet_SetPretendNoExpansion(1);
    int next = Sides_Next(3);
    int cycled = Sides_Cycle(3, TAK_SIDES_SKIRMISH, 0);
    TAK_DataSet_SetPretendNoExpansion(0);
    sd_cleanup();
    ASSERT_EQ_INT(7, next);
    ASSERT_EQ_INT(0, cycled);
}

TEST(a_remount_reloads_the_side_table) {
    ASSERT_EQ_INT(0, sd_mount(NULL, 0));
    int base = Sides_Count();
    ASSERT_EQ_INT(0, sd_mount(sd_ip_sides, 1));
    int ip = Sides_Count();
    sd_cleanup();
    ASSERT_EQ_INT(7, base);
    ASSERT_EQ_INT(8, ip);
}

/* A mission's PlayerN line names a side anywhere in its text, and the
 * first side in SIDEn order wins (legacy:169026-169095). */
TEST(a_player_line_names_its_side) {
    ASSERT_EQ_INT(0, sd_mount(sd_ip_sides, 1));
    int creon = Sides_FindInText("logo 6 CREON");
    int creon_foe = Sides_FindInText(
        "strategic opponent logo 6 CREON creon massattack 2300");
    int veruna = Sides_FindInText(
        "strategic opponent logo 0 VERUNA veruna massattack 2400");
    int aramon = Sides_FindInText("logo 3 aramon");
    int first = Sides_FindInText("zhon and aramon");
    int none = Sides_FindInText("passive neutral logo 8");
    int empty = Sides_FindInText("");
    sd_cleanup();
    ASSERT_EQ_INT(7, creon);
    ASSERT_EQ_INT(7, creon_foe);
    ASSERT_EQ_INT(2, veruna);
    ASSERT_EQ_INT(0, aramon);
    ASSERT_EQ_INT(0, first);
    ASSERT_EQ_INT(-1, none);
    ASSERT_EQ_INT(-1, empty);
}

int main(void) {
    TEST_SUITE("Side table");
    RUN(base_sides_offer_the_four_kingdoms);
    RUN(iron_plague_sides_add_creon_as_side_seven);
    RUN(a_fifth_playable_side_from_data_is_offered);
    RUN(the_setter_follows_the_game_mode);
    RUN(pretend_no_expansion_turns_creon_back);
    RUN(a_remount_reloads_the_side_table);
    RUN(a_player_line_names_its_side);
    TEST_REPORT();
}
