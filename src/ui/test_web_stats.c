/*
 * test_web_stats.c -- what the engine tells the page about play.
 *
 * The page is replaced by a sink that keeps the last event, so each
 * case reads exactly what the page's counting script would be handed.
 */

#include "test_framework.h"
#include "tak_web_stats.h"

#include <stdio.h>
#include <string.h>

static char g_kind[16];
static char g_json[1024];
static int  g_events;

static void sink(const char *kind, const char *json) {
    snprintf(g_kind, sizeof g_kind, "%s", kind);
    snprintf(g_json, sizeof g_json, "%s", json);
    g_events++;
}

static void reset(void) {
    g_kind[0] = g_json[0] = '\0';
    g_events = 0;
    WebStats_SetSink(sink);
}

/* Seat 1 a person on `side`, then `ai` computer seats at `level`. */
static void lineup(BattleConfig *cfg, int side, int ai, int level) {
    BattleConfig_SetDefaults(cfg);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) cfg->players[i].kind = TAK_SLOT_CLOSED;
    cfg->players[0].kind = TAK_SLOT_HUMAN;
    cfg->players[0].side = side;
    for (int i = 1; i <= ai; i++) {
        cfg->players[i].kind = TAK_SLOT_AI;
        cfg->players[i].side = TAK_SIDE_TAROS;
        cfg->players[i].ai_difficulty = level;
    }
}

static WebStatsResult result(int over, int local, int ticks) {
    WebStatsResult r;
    memset(&r, 0, sizeof r);
    r.over = over;
    r.local = local;
    r.ticks = ticks;
    return r;
}

TEST(a_desktop_build_tells_nobody) {
    reset();
    WebStats_SetSink(NULL);
    BattleConfig cfg;
    lineup(&cfg, TAK_SIDE_ZHON, 1, 2);
    WebStats_Starting("skirmish", NULL);
    WebStats_BattleBegan(&cfg, "Two Rivers", 1, 0, 0, NULL);
    WebStatsResult r = result(1, 1, 600);
    WebStats_BattleEnded(&r, NULL);
    WebStats_Error("nothing hears this");
    ASSERT_EQ_INT(0, g_events);
}

TEST(a_skirmish_names_its_map_its_computer_players_and_the_side_played) {
    reset();
    BattleConfig cfg;
    lineup(&cfg, TAK_SIDE_ZHON, 2, 2);
    cfg.remastered = 1;
    WebStats_Starting("skirmish", NULL);
    WebStats_BattleBegan(&cfg, "Tropical Paradise", 1, 0, 0, NULL);
    ASSERT_EQ_STR("start", g_kind);
    ASSERT_EQ_STR("{\"mode\":\"skirmish\",\"map\":\"Tropical Paradise\",\"ai\":2,\"lvl\":\"hard\","
                  "\"rb\":1,\"mod\":\"vanilla\",\"side\":\"zhon\"}", g_json);
    /* Computer players at more than one level are mixed, and a mod set
     * is named. */
    cfg.players[2].ai_difficulty = 0;
    cfg.remastered = 0;
    WebStats_Starting("skirmish", NULL);
    WebStats_BattleBegan(&cfg, "Tropical Paradise", 1, 0, 0, "TA:K Enhanced 2.1");
    ASSERT(strstr(g_json, "\"lvl\":\"mixed\",\"rb\":0,\"mod\":\"TA:K Enhanced 2.1\"") != NULL);
}

TEST(a_campaign_mission_names_its_campaign_and_mission) {
    reset();
    BattleConfig cfg;
    lineup(&cfg, TAK_SIDE_ARAMON, 0, 0);
    WebStats_Starting("campaign", NULL);
    WebStats_Mission("Aramon", "aram01");
    WebStats_BattleBegan(&cfg, "aram01", 1, 0, 0, NULL);
    ASSERT_EQ_STR("{\"mode\":\"campaign\",\"camp\":\"Aramon\",\"mission\":\"aram01\","
                  "\"mod\":\"vanilla\",\"side\":\"aramon\"}", g_json);
}

TEST(a_multiplayer_battle_says_how_it_was_entered) {
    reset();
    BattleConfig cfg;
    lineup(&cfg, TAK_SIDE_VERUNA, 1, 0);
    cfg.players[2].kind = TAK_SLOT_HUMAN;
    cfg.players[2].side = TAK_SIDE_ZHON;
    WebStats_Starting("multiplayer", "host");
    WebStats_BattleBegan(&cfg, "Two Rivers", 1, 1, 0, NULL);
    ASSERT_EQ_STR("{\"mode\":\"multiplayer\",\"sub\":\"host\",\"map\":\"Two Rivers\",\"ai\":1,"
                  "\"lvl\":\"easy\",\"humans\":2,\"rb\":0,\"mod\":\"vanilla\",\"side\":\"veruna\"}",
                  g_json);
    /* A watcher plays no side. */
    WebStats_Starting("multiplayer", "watch");
    WebStats_BattleBegan(&cfg, "Two Rivers", 1, 1, 0, NULL);
    ASSERT(strstr(g_json, "\"sub\":\"watch\"") != NULL);
    ASSERT(strstr(g_json, "\"side\":\"none\"") != NULL);
    /* A network battle no start path named is one somebody joined. */
    WebStats_Starting("skirmish", NULL);
    WebStats_BattleBegan(&cfg, "Two Rivers", 3, 1, 0, NULL);
    ASSERT(strstr(g_json, "{\"mode\":\"multiplayer\",\"sub\":\"join\",") != NULL);
    ASSERT(strstr(g_json, "\"side\":\"zhon\"") != NULL);
}

TEST(a_replay_is_a_replay_whatever_was_named_before) {
    reset();
    BattleConfig cfg;
    lineup(&cfg, TAK_SIDE_TAROS, 1, 3);
    WebStats_Starting("campaign", NULL);
    WebStats_BattleBegan(&cfg, "Two Rivers", 1, 0, 1, NULL);
    ASSERT(strstr(g_json, "{\"mode\":\"replay\",\"map\":\"Two Rivers\",") != NULL);
    ASSERT(strstr(g_json, "\"lvl\":\"brutal\"") != NULL);
    /* A loaded battle says where it was loaded from. */
    WebStats_Starting("load", "skirmish");
    WebStats_BattleBegan(&cfg, "Two Rivers", 1, 0, 0, NULL);
    ASSERT(strstr(g_json, "{\"mode\":\"load\",\"sub\":\"skirmish\",\"map\":\"Two Rivers\",") != NULL);
}

TEST(the_end_says_how_the_battle_went_and_how_long_it_took) {
    reset();
    BattleConfig cfg;
    lineup(&cfg, TAK_SIDE_ZHON, 1, 1);
    WebStatsResult r;
    struct { int over, local; const char *how, *out; } k_case[] = {
        { 1, 1, NULL, "win" }, { 1, -1, NULL, "loss" }, { 1, 0, NULL, "draw" },
        { 0, 0, NULL, "quit" }, { 0, 0, "desync", "desync" }, { 1, 1, "restart", "restart" },
        { 0, 0, "disconnect", "disconnect" },
        /* Beaten, watched for a while and left before the others finished. */
        { 0, -1, NULL, "loss" },
    };
    for (size_t i = 0; i < sizeof k_case / sizeof k_case[0]; i++) {
        WebStats_Starting("skirmish", NULL);
        WebStats_BattleBegan(&cfg, "Two Rivers", 1, 0, 0, NULL);
        r = result(k_case[i].over, k_case[i].local, 60 * 754 + 59);
        WebStats_BattleEnded(&r, k_case[i].how);
        char want[128];
        snprintf(want, sizeof want, "{\"mode\":\"skirmish\",\"out\":\"%s\",\"secs\":754}", k_case[i].out);
        ASSERT_EQ_STR("end", g_kind);
        ASSERT_EQ_STR(want, g_json);
    }
    /* A mission goes by its own verdict. */
    WebStats_Starting("campaign", NULL);
    WebStats_Mission("Zhon", "zhon03");
    WebStats_BattleBegan(&cfg, "zhon03", 1, 0, 0, NULL);
    r = result(0, 0, 120);
    r.defeat = 1;
    WebStats_BattleEnded(&r, NULL);
    ASSERT_EQ_STR("{\"mode\":\"campaign\",\"out\":\"loss\",\"secs\":2}", g_json);
    /* A replay or a watcher sees a battle end, it wins nothing. */
    WebStats_BattleBegan(&cfg, "Two Rivers", 1, 0, 1, NULL);
    r = result(1, 1, 600);
    WebStats_BattleEnded(&r, NULL);
    ASSERT_EQ_STR("{\"mode\":\"replay\",\"out\":\"over\",\"secs\":10}", g_json);
    /* No battle under way, no end to tell. */
    int before = g_events;
    WebStats_BattleEnded(&r, NULL);
    ASSERT_EQ_INT(before, g_events);
}

TEST(text_from_maps_and_errors_is_escaped_and_cut) {
    reset();
    WebStats_Error("the save \"autosave\" could not be read\tfrom C:\\games");
    ASSERT_EQ_STR("error", g_kind);
    ASSERT_EQ_STR("{\"msg\":\"the save \\\"autosave\\\" could not be read\\u0009from C:\\\\games\"}", g_json);
    char long_text[600];
    memset(long_text, 'm', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = '\0';
    WebStats_Error(long_text);
    ASSERT(strlen(g_json) < 200);
    BattleConfig cfg;
    lineup(&cfg, TAK_SIDE_ZHON, 0, 0);
    WebStats_Starting("skirmish", NULL);
    WebStats_BattleBegan(&cfg, "The \"Big\" One", 1, 0, 0, NULL);
    ASSERT(strstr(g_json, "\"map\":\"The \\\"Big\\\" One\",\"ai\":0,\"rb\":0,") != NULL);
    /* Text that escapes to six bytes a byte is cut by what it writes, so
     * every field after it still arrives whole. */
    char wide[400];
    for (int i = 0; i + 2 < (int)sizeof wide; i += 2) { wide[i] = (char)0xc3; wide[i + 1] = (char)0xa9; }
    wide[sizeof wide - 2] = '\0';
    WebStats_Starting("skirmish", NULL);
    WebStats_BattleBegan(&cfg, wide, 1, 0, 0, wide);
    size_t n = strlen(g_json);
    ASSERT(n > 20);
    ASSERT_EQ_STR(",\"side\":\"zhon\"}", g_json + n - strlen(",\"side\":\"zhon\"}"));
}

int main(void) {
    TEST_SUITE("What the page is told about play");
    RUN(a_desktop_build_tells_nobody);
    RUN(a_skirmish_names_its_map_its_computer_players_and_the_side_played);
    RUN(a_campaign_mission_names_its_campaign_and_mission);
    RUN(a_multiplayer_battle_says_how_it_was_entered);
    RUN(a_replay_is_a_replay_whatever_was_named_before);
    RUN(the_end_says_how_the_battle_went_and_how_long_it_took);
    RUN(text_from_maps_and_errors_is_escaped_and_cut);
    TEST_REPORT();
}
