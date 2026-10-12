/*
 * test_stats.c -- the site's visit counts, fed batches by hand.
 *
 * File and clock free apart from a scratch folder: the time is passed
 * in, so a day rolls over when a test says so.
 */

#include "test_framework.h"
#include "tak_net_stats.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#define rmdir_one(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0700)
#define rmdir_one(p) rmdir(p)
#endif

#define DAY_MS   86400000ull
/* 2026-10-12, noon UTC. */
#define TODAY    20738u
#define NOON     ((uint64_t)TODAY * DAY_MS + DAY_MS / 2)
#define DIR      "test_stats_tmp"

static TAK_Stats g_s;
static char      g_json[200 * 1024];

static const char *ID_A = "aaaaaaaaaaaaaaaaaaaaaa";
static const char *ID_B = "bbbbbbbbbbbbbbbbbbbbbb";

static TAK_StatsResult post_from(const char *json, const char *source, uint64_t now) {
    return TAK_Stats_Post(&g_s, json, strlen(json), source, "iad", now);
}

static TAK_StatsResult post(const char *json, uint64_t now) {
    return post_from(json, "198.51.100.7", now);
}

/* One batch from visitor `id` holding the events in `ev`. */
static TAK_StatsResult batch(const char *id, int nv, const char *ev, uint64_t now) {
    char json[TAK_STATS_BODY_MAX];
    snprintf(json, sizeof json,
             "{\"v\":1,\"id\":\"%s\",\"nv\":%d,"
             "\"env\":{\"br\":\"firefox\",\"os\":\"linux\",\"dev\":\"desktop\",\"scr\":\"1920-2559\"},"
             "\"ev\":[%s]}", id, nv, ev);
    return post(json, now);
}

static uint32_t count(TAK_StatDim dim, const char *value) {
    return TAK_Stats_Count(&g_s.today, dim, value);
}

static void day_path(uint32_t day, char *out, size_t cap) {
    char text[11];
    TAK_Stats_DayText(day, text);
    snprintf(out, cap, DIR "/%s.txt", text);
}

static void clean_dir(void) {
    char path[128];
    for (uint32_t d = TODAY - 100; d <= TODAY + 3; d++) {
        day_path(d, path, sizeof path);
        remove(path);
        strcat(path, ".tmp");
        remove(path);
    }
    remove(DIR "/visitors.bin");
    remove(DIR "/visitors.bin.tmp");
    rmdir_one(DIR);
}

/* The scratch folder, whether or not a delete of it is still pending,
 * which on Windows it can be while something else has a file open. */
static int make_scratch(void) {
    (void)mkdir_one(DIR);
    FILE *f = fopen(DIR "/probe", "wb");
    if (!f) return -1;
    fclose(f);
    remove(DIR "/probe");
    return 0;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* A whole file into g_json. */
static size_t slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { g_json[0] = '\0'; return 0; }
    size_t n = fread(g_json, 1, sizeof g_json - 1, f);
    fclose(f);
    g_json[n] = '\0';
    return n;
}

static size_t report(uint32_t days, uint64_t now) {
    size_t n = TAK_Stats_ReportJson(&g_s, days, now, g_json, sizeof g_json - 1);
    g_json[n] = '\0';
    return n;
}

static int has(const char *needle) { return strstr(g_json, needle) != NULL; }

TEST(a_day_number_reads_as_its_date) {
    char text[11];
    TAK_Stats_DayText(TODAY, text);
    ASSERT_EQ_STR("2026-10-12", text);
    TAK_Stats_DayText(0, text);
    ASSERT_EQ_STR("1970-01-01", text);
    TAK_Stats_DayText(19782, text);   /* a leap day */
    ASSERT_EQ_STR("2024-02-29", text);
}

TEST(a_batch_counts_its_views_steps_and_battles) {
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1,
        "{\"k\":\"view\",\"page\":\"landing\",\"s\":1},"
        "{\"k\":\"step\",\"step\":\"visit\"},"
        "{\"k\":\"step\",\"step\":\"files\"},"
        "{\"k\":\"start\",\"mode\":\"skirmish\",\"map\":\"Tropical Paradise\",\"ai\":3,"
        "\"lvl\":\"hard\",\"rb\":1,\"mod\":\"vanilla\",\"side\":\"zhon\"},"
        "{\"k\":\"end\",\"mode\":\"skirmish\",\"out\":\"win\",\"secs\":1260}", NOON));
    ASSERT_EQ_INT(1, (int)g_s.today.visitors);
    ASSERT_EQ_INT(1, (int)g_s.today.new_visitors);
    ASSERT_EQ_INT(1, (int)g_s.today.sessions);
    ASSERT_EQ_INT(1, (int)g_s.today.views);
    ASSERT_EQ_INT(5, (int)g_s.today.events);
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_PAGE, "landing"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_STEP, "visit"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_STEP, "files"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MODE, "skirmish"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MAP, "Tropical Paradise"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_AI, "3"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_LEVEL, "hard"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_REMASTERED, "on"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MOD, "vanilla"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_SIDE, "zhon"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_OUTCOME, "skirmish:win"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_LENGTH, "skirmish:15-30m"));
    ASSERT_EQ_INT(1260, (int)count(TAK_STAT_SECS, "skirmish"));
    /* The visitor's surroundings, once for the day. */
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_BROWSER, "firefox"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_OS, "linux"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_DEVICE, "desktop"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_SCREEN, "1920-2559"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_REGION, "iad"));
}

TEST(each_kind_of_battle_counts_what_it_is_about) {
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0,
        "{\"k\":\"start\",\"mode\":\"campaign\",\"camp\":\"Aramon\",\"mission\":\"aram01\",\"map\":\"aram01\",\"side\":\"aramon\"},"
        "{\"k\":\"start\",\"mode\":\"multiplayer\",\"sub\":\"dropin\",\"map\":\"Two Rivers\",\"humans\":3,\"ai\":1,\"lvl\":\"easy\",\"rb\":0,\"side\":\"taros\"},"
        "{\"k\":\"start\",\"mode\":\"load\",\"sub\":\"campaign\",\"map\":\"aram02\"},"
        "{\"k\":\"start\",\"mode\":\"replay\",\"map\":\"Two Rivers\"},"
        "{\"k\":\"end\",\"mode\":\"multiplayer\",\"out\":\"desync\",\"secs\":30},"
        "{\"k\":\"end\",\"mode\":\"campaign\",\"out\":\"left\",\"secs\":95}", NOON));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_CAMPAIGN, "Aramon"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MISSION, "aram01"));
    /* A mission is counted as a mission and not again as a map. */
    ASSERT_EQ_INT(0, (int)count(TAK_STAT_MAP, "aram01"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MP, "dropin"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_HUMANS, "3"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_LEVEL, "easy"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_REMASTERED, "off"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_LOAD, "campaign"));
    ASSERT_EQ_INT(2, (int)count(TAK_STAT_MAP, "Two Rivers"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MODE, "replay"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_OUTCOME, "multiplayer:desync"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_LENGTH, "multiplayer:under 1m"));
    /* A page closed in the middle of a battle ends it as left. */
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_OUTCOME, "campaign:left"));
    /* A campaign start names no computer players to count. */
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_AI, "1"));
    ASSERT_EQ_INT(0, (int)count(TAK_STAT_AI, "0"));
}

TEST(a_visitor_is_counted_once_a_day_however_many_batches) {
    TAK_Stats_Init(&g_s);
    const char *ev = "{\"k\":\"view\",\"page\":\"leaderboard\"}";
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1, ev, NOON));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1, ev, NOON + 1000));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0, ev, NOON + 2000));
    ASSERT_EQ_INT(1, (int)g_s.today.visitors);
    ASSERT_EQ_INT(1, (int)g_s.today.new_visitors);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_B, 0, ev, NOON + 3000));
    ASSERT_EQ_INT(2, (int)g_s.today.visitors);
    ASSERT_EQ_INT(1, (int)g_s.today.new_visitors);
    ASSERT_EQ_INT(4, (int)count(TAK_STAT_PAGE, "leaderboard"));
    /* No view began a session. */
    ASSERT_EQ_INT(0, (int)g_s.today.sessions);
    /* A visitor's browser is counted once a day, not once a batch. */
    ASSERT_EQ_INT(2, (int)count(TAK_STAT_BROWSER, "firefox"));
}

TEST(the_next_day_counts_the_same_visitor_again) {
    clean_dir();
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON));
    const char *ev = "{\"k\":\"view\",\"page\":\"landing\",\"s\":1}";
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1, ev, NOON));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0, ev, NOON + DAY_MS));
    ASSERT_EQ_INT((int)(TODAY + 1), (int)g_s.today.day);
    ASSERT_EQ_INT(1, (int)g_s.today.visitors);
    ASSERT_EQ_INT(0, (int)g_s.today.new_visitors);
    ASSERT_EQ_INT(1, (int)g_s.seen_count);
    /* The day before is in the report, with its own visitor. */
    ASSERT(report(2, NOON + DAY_MS) > 0);
    ASSERT(has("{\"day\":\"2026-10-12\",\"visitors\":1,\"new\":1,"));
    ASSERT(has("{\"day\":\"2026-10-13\",\"visitors\":1,\"new\":0,"));
    TAK_Stats_Close(&g_s, NOON + DAY_MS);
    clean_dir();
}

TEST(values_outside_the_allowed_set_are_counted_as_other) {
    TAK_Stats_Init(&g_s);
    char json[1024];
    snprintf(json, sizeof json,
             "{\"v\":1,\"id\":\"%s\",\"env\":{\"br\":\"netscape\",\"os\":\"amiga\",\"dev\":\"fridge\",\"scr\":\"9000\"},"
             "\"ev\":[{\"k\":\"view\",\"page\":\"admin\"},{\"k\":\"step\",\"step\":\"cheat\"},"
             "{\"k\":\"start\",\"mode\":\"godmode\",\"side\":\"martians\",\"lvl\":\"impossible\",\"ai\":99},"
             "{\"k\":\"end\",\"mode\":\"skirmish\",\"out\":\"rage\",\"secs\":-5}]}", ID_A);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, TAK_Stats_Post(&g_s, json, strlen(json), NULL, "Not A Region", NOON));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_BROWSER, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_OS, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_DEVICE, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_SCREEN, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_REGION, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_PAGE, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_STEP, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MODE, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_SIDE, "other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_OUTCOME, "skirmish:other"));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_LENGTH, "skirmish:under 1m"));
    ASSERT_EQ_INT(0, (int)count(TAK_STAT_PAGE, "admin"));
    /* With no region named, the visitor is counted under none. */
    TAK_Stats_Init(&g_s);
    snprintf(json, sizeof json, "{\"v\":1,\"id\":\"%s\",\"ev\":[{\"k\":\"view\",\"page\":\"mods\"}]}", ID_A);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, TAK_Stats_Post(&g_s, json, strlen(json), NULL, NULL, NOON));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_REGION, "none"));
}

TEST(free_text_is_cleaned_and_each_column_is_capped) {
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0,
        "{\"k\":\"start\",\"mode\":\"skirmish\",\"map\":\"  Vain\\tBless\\u0007ings\\n  \"}", NOON));
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_MAP, "Vain Blessings"));
    /* Past the column's cap, a new map is other. */
    char ev[128];
    int took = 0;
    for (int i = 0; i < 300; i++) {
        snprintf(ev, sizeof ev, "{\"k\":\"start\",\"mode\":\"skirmish\",\"map\":\"Map %d\"}", i);
        char source[32];
        snprintf(source, sizeof source, "10.0.%d.%d", i / 200, i % 200);
        char json[512];
        snprintf(json, sizeof json, "{\"v\":1,\"id\":\"%s\",\"ev\":[%s]}", ID_A, ev);
        if (post_from(json, source, NOON) == TAK_STATS_TAKEN) took++;
    }
    ASSERT_EQ_INT(300, took);
    ASSERT(count(TAK_STAT_MAP, "other") > 0);
    ASSERT(g_s.today.per_dim[TAK_STAT_MAP] <= 121);
    ASSERT_EQ_INT(301, (int)count(TAK_STAT_MODE, "skirmish"));
    /* An error message long enough to fill the value is cut, not refused. */
    char big[600];
    memset(big, 'x', sizeof big);
    big[sizeof big - 1] = '\0';
    char json[1024];
    snprintf(json, sizeof json, "{\"v\":1,\"id\":\"%s\",\"ev\":[{\"k\":\"error\",\"msg\":\"%s\",\"fatal\":1}]}",
             ID_B, big);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, post_from(json, "192.0.2.1", NOON));
    big[TAK_STATS_VALUE_MAX - 1] = '\0';
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_CRASH, big));
}

TEST(a_body_that_is_not_a_batch_counts_nothing) {
    static const char *bad[] = {
        "",
        "null",
        "[]",
        "{",
        "{\"v\":1,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\",\"ev\":[{\"k\":\"view\",\"page\":\"landing\"}]",
        "{\"v\":2,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\",\"ev\":[]}",
        "{\"v\":1,\"id\":\"short\",\"ev\":[]}",
        "{\"v\":1,\"id\":\"has spaces in it, which no id does\",\"ev\":[]}",
        "{\"v\":1,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\",\"ev\":{}}",
        "{\"v\":1,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\"}",
        "{\"v\":1,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\",\"ev\":[[[[[[]]]]]]}",
        "{\"v\":1,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\",\"ev\":[]} trailing",
        "{\"v\":1,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\",\"ev\":[\"view\"]}",
    };
    TAK_Stats_Init(&g_s);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (post_from(bad[i], "203.0.113.9", NOON) != TAK_STATS_MALFORMED) {
            printf("\n    taken: %s\n", bad[i]);
            ASSERT(0);
        }
    }
    ASSERT_EQ_INT((int)(sizeof bad / sizeof bad[0]), (int)g_s.today.refused);
    ASSERT_EQ_INT(0, (int)g_s.today.visitors);
    ASSERT_EQ_INT(0, (int)g_s.today.events);
    ASSERT_EQ_INT(0, (int)g_s.today.count);
    /* A body with a NUL in it is not read past the NUL as text. */
    const char nul[] = "{\"v\":1,\"id\":\"aaaaaaaaaaaaaaaaaaaaaa\",\"ev\":[]}\0{}";
    ASSERT_EQ_INT(TAK_STATS_MALFORMED, TAK_Stats_Post(&g_s, nul, sizeof nul - 1, NULL, NULL, NOON));
}

TEST(events_past_the_cap_and_of_unknown_kinds_are_dropped) {
    TAK_Stats_Init(&g_s);
    char ev[4096] = "";
    for (int i = 0; i < 30; i++) {
        if (i) strcat(ev, ",");
        strcat(ev, i == 3 ? "{\"k\":\"telemetry\",\"x\":1}" : "{\"k\":\"view\",\"page\":\"mods\"}");
    }
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0, ev, NOON));
    ASSERT_EQ_INT((int)TAK_STATS_BATCH_EVENTS - 1, (int)count(TAK_STAT_PAGE, "mods"));
    ASSERT_EQ_INT(30 - (int)TAK_STATS_BATCH_EVENTS + 1, (int)g_s.today.dropped);
    ASSERT_EQ_INT((int)TAK_STATS_BATCH_EVENTS - 1, (int)g_s.today.events);
    /* A body past the cap is not a batch. */
    static char huge[TAK_STATS_BODY_MAX + 64];
    memset(huge, ' ', sizeof huge);
    memcpy(huge, "{\"v\":1", 6);
    ASSERT_EQ_INT(TAK_STATS_MALFORMED, TAK_Stats_Post(&g_s, huge, sizeof huge, NULL, NULL, NOON));
}

TEST(a_source_that_sends_too_much_is_limited_for_the_minute) {
    TAK_Stats_Init(&g_s);
    const char *ev = "{\"k\":\"view\",\"page\":\"landing\"}";
    char json[512];
    snprintf(json, sizeof json, "{\"v\":1,\"id\":\"%s\",\"ev\":[%s]}", ID_A, ev);
    uint64_t t = NOON - NOON % TAK_STATS_WINDOW_MS;
    for (unsigned i = 0; i < TAK_STATS_SOURCE_BATCHES; i++)
        ASSERT_EQ_INT(TAK_STATS_TAKEN, post_from(json, "198.51.100.7", t + i));
    ASSERT_EQ_INT(TAK_STATS_LIMITED, post_from(json, "198.51.100.7", t + 100));
    ASSERT_EQ_INT(1, (int)g_s.today.limited);
    /* Someone else is not held up by it. */
    ASSERT_EQ_INT(TAK_STATS_TAKEN, post_from(json, "198.51.100.8", t + 100));
    /* The next minute starts over. */
    ASSERT_EQ_INT(TAK_STATS_TAKEN, post_from(json, "198.51.100.7", t + TAK_STATS_WINDOW_MS));
    /* Events are limited too, a full batch at a time. */
    char many[2048] = "", full[TAK_STATS_BODY_MAX];
    for (unsigned i = 0; i < TAK_STATS_BATCH_EVENTS; i++) {
        if (i) strcat(many, ",");
        strcat(many, ev);
    }
    snprintf(full, sizeof full, "{\"v\":1,\"id\":\"%s\",\"ev\":[%s]}", ID_A, many);
    int taken = 0;
    for (int i = 0; i < 20; i++)
        if (post_from(full, "198.51.100.9", t + 2 * TAK_STATS_WINDOW_MS) == TAK_STATS_TAKEN) taken++;
    ASSERT_EQ_INT((int)(TAK_STATS_SOURCE_EVENTS / TAK_STATS_BATCH_EVENTS), taken);
}

TEST(the_report_needs_the_token_and_turns_a_guesser_away) {
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_HasToken(&g_s));
    ASSERT_EQ_INT(0, TAK_Stats_Authorized(&g_s, "Bearer ", NULL, NOON));
    TAK_Stats_SetToken(&g_s, "tooshort");
    ASSERT_EQ_INT(0, TAK_Stats_HasToken(&g_s));
    TAK_Stats_SetToken(&g_s, "k3J9-wq2Lx8vT5mN0pR7sY4u");
    ASSERT_EQ_INT(1, TAK_Stats_HasToken(&g_s));
    ASSERT_EQ_INT(1, TAK_Stats_Authorized(&g_s, "Bearer k3J9-wq2Lx8vT5mN0pR7sY4u", "192.0.2.1", NOON));
    ASSERT_EQ_INT(1, TAK_Stats_Authorized(&g_s, "bearer  k3J9-wq2Lx8vT5mN0pR7sY4u ", "192.0.2.1", NOON));
    ASSERT_EQ_INT(0, TAK_Stats_Authorized(&g_s, "Bearer k3J9-wq2Lx8vT5mN0pR7sY4", "192.0.2.1", NOON));
    ASSERT_EQ_INT(0, TAK_Stats_Authorized(&g_s, "Bearer k3J9-wq2Lx8vT5mN0pR7sY4uX", "192.0.2.1", NOON));
    ASSERT_EQ_INT(0, TAK_Stats_Authorized(&g_s, "Basic k3J9-wq2Lx8vT5mN0pR7sY4u", "192.0.2.1", NOON));
    ASSERT_EQ_INT(0, TAK_Stats_Authorized(&g_s, NULL, "192.0.2.1", NOON));
    for (int i = 4; i < (int)TAK_STATS_AUTH_FAILURES; i++)
        ASSERT_EQ_INT(0, TAK_Stats_Authorized(&g_s, "Bearer guess", "192.0.2.1", NOON));
    /* Out of guesses for the minute, even with the right token. */
    ASSERT_EQ_INT(-1, TAK_Stats_Authorized(&g_s, "Bearer k3J9-wq2Lx8vT5mN0pR7sY4u", "192.0.2.1", NOON));
    ASSERT_EQ_INT(1, TAK_Stats_Authorized(&g_s, "Bearer k3J9-wq2Lx8vT5mN0pR7sY4u", "192.0.2.2", NOON));
    ASSERT_EQ_INT(1, TAK_Stats_Authorized(&g_s, "Bearer k3J9-wq2Lx8vT5mN0pR7sY4u", "192.0.2.1",
                                          NOON + TAK_STATS_WINDOW_MS));
}

TEST(only_the_named_origins_may_post_once_any_is_named) {
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(1, TAK_Stats_OriginAllowed(&g_s, "https://example.org"));
    ASSERT_EQ_INT(1, TAK_Stats_OriginAllowed(&g_s, NULL));
    ASSERT_EQ_INT(0, TAK_Stats_AllowOrigin(&g_s, "https://openkingdoms.net"));
    ASSERT_EQ_INT(1, TAK_Stats_OriginAllowed(&g_s, "https://openkingdoms.net"));
    ASSERT_EQ_INT(0, TAK_Stats_OriginAllowed(&g_s, "https://openkingdoms.net.evil.example"));
    ASSERT_EQ_INT(0, TAK_Stats_OriginAllowed(&g_s, "http://openkingdoms.net"));
    ASSERT_EQ_INT(0, TAK_Stats_OriginAllowed(&g_s, NULL));
    ASSERT_EQ_INT(0, TAK_Stats_OriginAllowed(&g_s, ""));
}

TEST(a_day_is_written_whole_and_names_no_visitor) {
    clean_dir();
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1,
        "{\"k\":\"view\",\"page\":\"landing\",\"s\":1},"
        "{\"k\":\"start\",\"mode\":\"skirmish\",\"map\":\"Tropical Paradise\"}", NOON));
    /* Nothing is written until the minute is up. */
    char path[128];
    day_path(TODAY, path, sizeof path);
    TAK_Stats_Tick(&g_s, NOON + 1000);
    ASSERT_EQ_INT(0, file_exists(path));
    TAK_Stats_Tick(&g_s, NOON + TAK_STATS_FLUSH_MS + 1);
    ASSERT_EQ_INT(1, file_exists(path));
    strcat(path, ".tmp");
    ASSERT_EQ_INT(0, file_exists(path));
    day_path(TODAY, path, sizeof path);
    ASSERT(slurp(path) > 0);
    ASSERT(has("okstats 1\n"));
    ASSERT(has("visitors 1\n"));
    ASSERT(has("map\tTropical Paradise\t1\n"));
    ASSERT(!has(ID_A));
    /* The visitors are not on disk while the relay runs. */
    ASSERT_EQ_INT(0, file_exists(DIR "/visitors.bin"));
    clean_dir();
}

TEST(a_restart_the_same_day_keeps_its_counts_and_its_visitors) {
    clean_dir();
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1, "{\"k\":\"view\",\"page\":\"landing\",\"s\":1}", NOON));
    TAK_Stats_Close(&g_s, NOON + 10);
    ASSERT_EQ_INT(1, file_exists(DIR "/visitors.bin"));

    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON + 5000));
    /* Read back once and gone from the disk. */
    ASSERT_EQ_INT(0, file_exists(DIR "/visitors.bin"));
    ASSERT_EQ_INT(1, (int)g_s.today.visitors);
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_PAGE, "landing"));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0, "{\"k\":\"view\",\"page\":\"landing\"}", NOON + 6000));
    ASSERT_EQ_INT(1, (int)g_s.today.visitors);
    ASSERT_EQ_INT(2, (int)count(TAK_STAT_PAGE, "landing"));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_B, 0, "{\"k\":\"view\",\"page\":\"landing\"}", NOON + 7000));
    ASSERT_EQ_INT(2, (int)g_s.today.visitors);
    TAK_Stats_Close(&g_s, NOON + 8000);
    clean_dir();
}

TEST(a_restart_on_a_later_day_forgets_the_visitors) {
    clean_dir();
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1, "{\"k\":\"view\",\"page\":\"landing\",\"s\":1}", NOON));
    TAK_Stats_Close(&g_s, NOON + 10);

    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON + DAY_MS));
    ASSERT_EQ_INT(0, file_exists(DIR "/visitors.bin"));
    ASSERT_EQ_INT(0, (int)g_s.today.visitors);
    ASSERT_EQ_INT(0, (int)g_s.seen_count);
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0, "{\"k\":\"view\",\"page\":\"landing\"}", NOON + DAY_MS));
    ASSERT_EQ_INT(1, (int)g_s.today.visitors);
    /* Yesterday's file is still the record of yesterday. */
    ASSERT(report(2, NOON + DAY_MS) > 0);
    ASSERT(has("{\"day\":\"2026-10-12\",\"visitors\":1,"));
    TAK_Stats_Close(&g_s, NOON + DAY_MS);
    clean_dir();
}

TEST(a_write_cut_short_by_a_crash_leaves_the_last_whole_day) {
    clean_dir();
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1, "{\"k\":\"view\",\"page\":\"mods\",\"s\":1}", NOON));
    ASSERT_EQ_INT(0, TAK_Stats_Flush(&g_s));
    /* A crash in the middle of the next write leaves a part file. */
    char path[128];
    day_path(TODAY, path, sizeof path);
    strcat(path, ".tmp");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("okstats 1\nday 2026-10-12\nvisitors 99", f);
    fclose(f);

    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON + 1000));
    ASSERT_EQ_INT(1, (int)g_s.today.visitors);
    ASSERT_EQ_INT(1, (int)count(TAK_STAT_PAGE, "mods"));
    clean_dir();
}

TEST(a_damaged_day_file_is_read_as_far_as_it_makes_sense) {
    clean_dir();
    ASSERT_EQ_INT(0, make_scratch());
    char path[128];
    day_path(TODAY - 1, path, sizeof path);
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("okstats 1\nday 2026-10-11\nvisitors 7\nsessions x\n"
          "page\tlanding\t5\nnonsense line\nnodim\tvalue\t3\npage\tmods\n"
          "map\tTwo Rivers\t4\nmap\tTwo Rivers\t2\nwhatever 12\n", f);
    fclose(f);
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON));
    ASSERT(report(2, NOON) > 0);
    ASSERT(has("{\"day\":\"2026-10-11\",\"visitors\":7,"));
    ASSERT(has("\"page\":{\"landing\":5}"));
    ASSERT(has("\"map\":{\"Two Rivers\":6}"));
    clean_dir();
}

TEST(the_report_covers_the_days_asked_and_merges_their_counts) {
    clean_dir();
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON - 2 * DAY_MS));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 1,
        "{\"k\":\"view\",\"page\":\"landing\",\"s\":1},"
        "{\"k\":\"start\",\"mode\":\"skirmish\",\"map\":\"Two Rivers\",\"ai\":1,\"lvl\":\"easy\"}",
        NOON - 2 * DAY_MS));
    ASSERT_EQ_INT(TAK_STATS_TAKEN, batch(ID_A, 0,
        "{\"k\":\"view\",\"page\":\"landing\",\"s\":1},"
        "{\"k\":\"start\",\"mode\":\"campaign\",\"camp\":\"Zhon\",\"mission\":\"zhon01\"},"
        "{\"k\":\"error\",\"msg\":\"the replay could not begin\"}", NOON));
    ASSERT(report(7, NOON) > 0);
    ASSERT(has("\"from\":\"2026-10-06\",\"to\":\"2026-10-12\",\"days\":7"));
    ASSERT(has("{\"day\":\"2026-10-06\",\"visitors\":0,"));
    ASSERT(has("{\"day\":\"2026-10-10\",\"visitors\":1,\"new\":1,\"sessions\":1,\"views\":1,"
               "\"battles\":1,\"ended\":0,\"errors\":0,\"modes\":{\"skirmish\":1}}"));
    ASSERT(has("{\"day\":\"2026-10-12\",\"visitors\":1,\"new\":0,\"sessions\":1,\"views\":1,"
               "\"battles\":1,\"ended\":0,\"errors\":1,\"modes\":{\"campaign\":1}}"));
    ASSERT(has("\"totals\":{\"visitors\":2,\"new\":1,\"sessions\":2,\"views\":2,"));
    ASSERT(has("\"page\":{\"landing\":2}"));
    ASSERT(has("\"mode\":{\"campaign\":1,\"skirmish\":1}"));
    ASSERT(has("\"error\":{\"the replay could not begin\":1}"));
    /* Out of range asks are brought into range. */
    ASSERT(report(0, NOON) > 0);
    ASSERT(has("\"days\":1,"));
    ASSERT(report(5000, NOON) > 0);
    ASSERT(has("\"days\":90,"));
    TAK_Stats_Close(&g_s, NOON);
    clean_dir();
}

/* Ninety days, every one holding as many values as a day can, still
 * makes one answer the relay can send. */
TEST(the_report_of_ninety_full_days_fits_the_answer) {
    clean_dir();
    ASSERT_EQ_INT(0, make_scratch());
    static const char *free_dims[] = { "campaign", "mission", "map", "mod", "error", "crash", "region" };
    char path[128];
    for (uint32_t d = TODAY - 89; d < TODAY; d++) {
        day_path(d, path, sizeof path);
        FILE *f = fopen(path, "wb");
        ASSERT_NOT_NULL(f);
        char text[11];
        TAK_Stats_DayText(d, text);
        fprintf(f, "okstats 1\nday %s\nvisitors 4000000000\nsessions 4000000000\n", text);
        for (size_t k = 0; k < sizeof free_dims / sizeof free_dims[0]; k++)
            for (int i = 0; i < 130; i++)
                fprintf(f, "%s\t%u %d \"quoted\\\" and long enough to fill most of a value %0*d\t4000000000\n",
                        free_dims[k], (unsigned)d, i, 20, i);
        fclose(f);
    }
    TAK_Stats_Init(&g_s);
    ASSERT_EQ_INT(0, TAK_Stats_Open(&g_s, DIR, NOON));
    size_t n = report(90, NOON);
    ASSERT(n > 0);
    ASSERT(n < 120u * 1024u);
    ASSERT(has("\"days\":90,"));
    clean_dir();
}

TEST(the_visitor_count_stops_at_its_cap_and_says_so) {
    TAK_Stats_Init(&g_s);
    char id[32], json[256], source[32];
    uint32_t cap = TAK_STATS_VISITORS_MAX / 4 * 3;
    /* Fifty sources a minute from an hour into the day, inside every
     * limit and all on the one day. */
    uint64_t start = (uint64_t)TODAY * DAY_MS + 3600000u;
    for (uint32_t i = 0; i < cap + 10; i++) {
        snprintf(id, sizeof id, "visitor%016u", (unsigned)i);
        snprintf(json, sizeof json, "{\"v\":1,\"id\":\"%s\",\"ev\":[{\"k\":\"view\",\"page\":\"landing\"}]}", id);
        snprintf(source, sizeof source, "s%u", (unsigned)(i % 50));
        if (post_from(json, source, start + (uint64_t)(i / 50) * TAK_STATS_WINDOW_MS) != TAK_STATS_TAKEN) {
            printf("\n    batch %u not taken\n", (unsigned)i);
            ASSERT(0);
            return;
        }
    }
    ASSERT_EQ_INT((int)cap, (int)g_s.today.visitors);
    ASSERT_EQ_INT(1, (int)g_s.today.capped);
    ASSERT_EQ_INT((int)(cap + 10), (int)count(TAK_STAT_PAGE, "landing"));
}

int main(void) {
    TEST_SUITE("Visit counts");
    RUN(a_day_number_reads_as_its_date);
    RUN(a_batch_counts_its_views_steps_and_battles);
    RUN(each_kind_of_battle_counts_what_it_is_about);
    RUN(a_visitor_is_counted_once_a_day_however_many_batches);
    RUN(the_next_day_counts_the_same_visitor_again);
    RUN(values_outside_the_allowed_set_are_counted_as_other);
    RUN(free_text_is_cleaned_and_each_column_is_capped);
    RUN(a_body_that_is_not_a_batch_counts_nothing);
    RUN(events_past_the_cap_and_of_unknown_kinds_are_dropped);
    RUN(a_source_that_sends_too_much_is_limited_for_the_minute);
    RUN(the_report_needs_the_token_and_turns_a_guesser_away);
    RUN(only_the_named_origins_may_post_once_any_is_named);
    RUN(a_day_is_written_whole_and_names_no_visitor);
    RUN(a_restart_the_same_day_keeps_its_counts_and_its_visitors);
    RUN(a_restart_on_a_later_day_forgets_the_visitors);
    RUN(a_write_cut_short_by_a_crash_leaves_the_last_whole_day);
    RUN(a_damaged_day_file_is_read_as_far_as_it_makes_sense);
    RUN(the_report_covers_the_days_asked_and_merges_their_counts);
    RUN(the_report_of_ninety_full_days_fits_the_answer);
    RUN(the_visitor_count_stops_at_its_cap_and_says_so);
    TEST_REPORT();
}
