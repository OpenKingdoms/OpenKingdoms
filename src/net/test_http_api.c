/*
 * test_http_api.c -- the relay's JSON API, fed request bytes by hand.
 *
 * Data free and socket free. The interesting cases are the empty
 * ledger, which the page meets on a fresh deployment, and the shapes
 * the page reads: a row, a game, a player's history.
 */

#include "test_framework.h"
#include "tak_net_http.h"

#include <stdio.h>
#include <string.h>

static TAK_Ledger g_l;
static char       g_out[TAK_HTTP_RESPONSE_MAX + 1024];

static size_t answer(const char *req) {
    size_t n = TAK_Http_Answer(&g_l, (const uint8_t *)req, strlen(req),
                               g_out, sizeof g_out);
    if (n < sizeof g_out) g_out[n] = '\0';
    return n;
}

static const char *body(void) {
    const char *p = strstr(g_out, "\r\n\r\n");
    return p ? p + 4 : "";
}

static int has(const char *needle) { return strstr(g_out, needle) != NULL; }

static void seat(TAK_LedgerMatch *m, uint8_t s, const char *name, int human,
                 int standing, int32_t last_tick, int32_t score) {
    TAK_LedgerSeat *x = &m->seat[m->seat_count++];
    memset(x, 0, sizeof(*x));
    x->seat = s;
    x->kind = human ? TAK_NSLOT_HUMAN : TAK_NSLOT_COMPUTER;
    x->standing = (uint8_t)standing;
    x->player_id = human ? TAK_Ledger_PlayerId(name) : 0;
    snprintf(x->name, sizeof x->name, "%s", name);
    x->units_built = 10 + s;
    x->kills = 3 + s;
    x->losses = 2 + s;
    x->score = score;
    x->last_alive_tick = last_tick;
}

static void record(uint64_t ended, const char *map, const char *winner,
                   const char *loser, int computer) {
    TAK_LedgerMatch m;
    memset(&m, 0, sizeof m);
    m.started_ms = ended - 1000;
    m.ended_ms = ended;
    m.end_tick = 7200;
    m.unit_cap = 500;
    snprintf(m.map_name, sizeof m.map_name, "%s", map);
    seat(&m, 0, winner, 1, 1, 7200, 100);
    seat(&m, 1, loser, 1, 0, 5000, 20);
    if (computer) seat(&m, 2, "Computer", 0, 0, 100, 0);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
}

static void some_games(void) {
    TAK_Ledger_Init(&g_l);
    record(1000, "one", "Zach", "Lokken", 0);
    record(2000, "two", "Zach", "Elsin", 1);
    record(3000, "three", "Lokken", "Zach", 0);
}

/* ── An empty deployment ──────────────────────────────────────────────── */

TEST(an_empty_ledger_gives_an_empty_table_with_cors) {
    TAK_Ledger_Init(&g_l);
    ASSERT(answer("GET /api/leaderboard HTTP/1.1\r\nHost: r\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200 OK\r\n"));
    ASSERT(has("Content-Type: application/json"));
    ASSERT(has("Access-Control-Allow-Origin: *\r\n"));
    ASSERT(has("Connection: close\r\n"));
    ASSERT_EQ_STR("{\"players\":[],\"total\":0,\"offset\":0,\"limit\":100,\"games\":0,\"disputed\":0,\"version\":0}", body());
    /* The length header says exactly what follows. */
    char want[64];
    snprintf(want, sizeof want, "Content-Length: %u\r\n", (unsigned)strlen(body()));
    ASSERT(has(want));

    ASSERT(answer("GET /api/games HTTP/1.1\r\n\r\n") > 0);
    ASSERT_EQ_STR("{\"games\":[],\"total\":0,\"offset\":0,\"limit\":25}", body());
    ASSERT(answer("GET /api/health HTTP/1.1\r\n\r\n") > 0);
    ASSERT_EQ_STR("{\"ok\":true,\"games\":0,\"disputed\":0,\"refused\":0,\"version\":0}", body());
}

/* The table says how many games it left out, so the omission shows. */
TEST(the_table_and_health_count_disputed_games) {
    some_games();
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 2, 0));
    ASSERT(answer("GET /api/leaderboard HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"games\":3,\"disputed\":1,"));
    /* Elsin only played the disputed game, so Elsin is not a row. */
    ASSERT(has("\"total\":2,"));
    ASSERT(!has("\"name\":\"Elsin\""));
    ASSERT(has("\"name\":\"Zach\",\"games\":2,\"wins\":1,\"losses\":1"));
    g_l.refused = 4;
    ASSERT(answer("GET /api/health HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"games\":3,\"disputed\":1,\"refused\":4,"));
    /* The game itself still answers, flagged. */
    ASSERT(answer("GET /api/games/2 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"disputed\":true"));
}

/* ── The table ────────────────────────────────────────────────────────── */

TEST(the_table_lists_players_wins_first_and_pages) {
    some_games();
    ASSERT(answer("GET /api/leaderboard HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"total\":3"));
    ASSERT(has("\"games\":3"));
    const char *zach = strstr(g_out, "\"name\":\"Zach\"");
    const char *lokken = strstr(g_out, "\"name\":\"Lokken\"");
    const char *elsin = strstr(g_out, "\"name\":\"Elsin\"");
    ASSERT_NOT_NULL(zach);
    ASSERT_NOT_NULL(lokken);
    ASSERT_NOT_NULL(elsin);
    ASSERT(zach < lokken);
    ASSERT(lokken < elsin);
    ASSERT(!has("Computer"));
    ASSERT(has("\"wins\":2,\"losses\":1"));
    /* Ids are sixteen hex digits, which the page puts in a URL. */
    char id[32];
    snprintf(id, sizeof id, "\"id\":\"%016llx\"",
             (unsigned long long)TAK_Ledger_PlayerId("zach"));
    ASSERT(has(id));

    ASSERT(answer("GET /api/leaderboard?limit=2&offset=0 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"name\":\"Zach\""));
    ASSERT(has("\"name\":\"Lokken\""));
    ASSERT(!has("\"name\":\"Elsin\""));
    ASSERT(has("\"total\":3,\"offset\":0,\"limit\":2"));
    ASSERT(answer("GET /api/leaderboard?offset=2&limit=2 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(!has("\"name\":\"Zach\""));
    ASSERT(has("\"name\":\"Elsin\""));
    /* A silly limit is clamped rather than refused. */
    ASSERT(answer("GET /api/leaderboard?limit=99999 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"limit\":200"));
    ASSERT(answer("GET /api/leaderboard?limit=0 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"limit\":1"));
}

/* ── One player ───────────────────────────────────────────────────────── */

TEST(a_player_page_carries_their_row_and_games_newest_first) {
    some_games();
    char req[128];
    snprintf(req, sizeof req, "GET /api/players/%016llx?limit=2 HTTP/1.1\r\n\r\n",
             (unsigned long long)TAK_Ledger_PlayerId("ZACH"));
    ASSERT(answer(req) > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT(has("\"player\":{\"id\":"));
    ASSERT(has("\"name\":\"Zach\",\"games\":3,\"wins\":2,\"losses\":1"));
    const char *three = strstr(g_out, "\"map\":\"three\"");
    const char *two = strstr(g_out, "\"map\":\"two\"");
    ASSERT_NOT_NULL(three);
    ASSERT_NOT_NULL(two);
    ASSERT(three < two);
    ASSERT(!has("\"map\":\"one\""));
    ASSERT(has("\"total\":3,\"offset\":0,\"limit\":2"));
    /* Each game names every seat, so the page can show who else played. */
    ASSERT(has("\"name\":\"Elsin\""));
    ASSERT(has("\"kind\":\"computer\""));
}

TEST(an_unknown_player_or_game_is_404) {
    some_games();
    ASSERT(answer("GET /api/players/0000000000000001 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 404 Not Found"));
    ASSERT_EQ_STR("{\"error\":\"not found\"}", body());
    ASSERT(has("Access-Control-Allow-Origin: *"));
    ASSERT(answer("GET /api/players/zach HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 404"));
    ASSERT(answer("GET /api/games/9 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 404"));
    ASSERT(answer("GET /api/games/1x HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 404"));
    ASSERT(answer("GET / HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 404"));
    ASSERT(answer("GET /api/nothing HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 404"));
}

/* ── One game ─────────────────────────────────────────────────────────── */

TEST(a_game_page_carries_every_seat_with_place_and_result) {
    some_games();
    ASSERT(answer("GET /api/games/2 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT(has("{\"game\":{\"id\":2,\"map\":\"two\",\"started_ms\":1000,\"ended_ms\":2000,\"end_tick\":7200,"));
    ASSERT(has("\"unit_cap\":500,\"reports\":1,\"disputed\":false,\"seats\":["));
    ASSERT(has("\"seat\":0,\"kind\":\"human\",\"name\":\"Zach\",\"player\":\""));
    ASSERT(has("\"standing\":true,\"eliminated\":false,\"place\":1,\"result\":\"won\",\"units_built\":10,\"kills\":3,\"losses\":2,\"score\":100,\"time_ticks\":7200}"));
    ASSERT(has("\"name\":\"Elsin\""));
    ASSERT(has("\"place\":2,\"result\":\"lost\""));
    ASSERT(has("\"seat\":2,\"kind\":\"computer\",\"name\":\"Computer\",\"player\":null"));
    ASSERT(has("\"place\":3,\"result\":\"lost\""));
}

TEST(recent_games_come_newest_first) {
    some_games();
    ASSERT(answer("GET /api/games?limit=2 HTTP/1.1\r\n\r\n") > 0);
    const char *three = strstr(g_out, "\"id\":3,\"map\":\"three\"");
    const char *two = strstr(g_out, "\"id\":2,\"map\":\"two\"");
    ASSERT_NOT_NULL(three);
    ASSERT_NOT_NULL(two);
    ASSERT(three < two);
    ASSERT(!has("\"map\":\"one\""));
    ASSERT(has("\"total\":3,\"offset\":0,\"limit\":2"));
    ASSERT(answer("GET /api/games?offset=2 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"map\":\"one\""));
    ASSERT(!has("\"map\":\"two\""));
    ASSERT(answer("GET /api/games?offset=99 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"games\":[]"));
}

/* ── Odds and ends ────────────────────────────────────────────────────── */

TEST(options_answers_the_preflight_with_no_body_and_a_post_is_refused) {
    TAK_Ledger_Init(&g_l);
    ASSERT(answer("OPTIONS /api/leaderboard HTTP/1.1\r\nOrigin: https://openkingdoms.net\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 204 No Content"));
    ASSERT(has("Content-Length: 0\r\n"));
    ASSERT(has("Access-Control-Allow-Methods: GET, HEAD, OPTIONS"));
    ASSERT_EQ_STR("", body());
    ASSERT(answer("HEAD /api/health HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT_EQ_STR("", body());
    ASSERT(answer("POST /api/games HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 405"));
    ASSERT_EQ_STR("{\"error\":\"read only\"}", body());
}

TEST(a_name_with_a_quote_in_it_is_escaped) {
    TAK_Ledger_Init(&g_l);
    record(1000, "the \"pit\"", "Say \"hi\"\\", "Tab\tby", 0);
    ASSERT(answer("GET /api/games/1 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"map\":\"the \\\"pit\\\"\""));
    ASSERT(has("\"name\":\"Say \\\"hi\\\"\\\\\""));
    ASSERT(has("\"name\":\"Tab\\u0009by\""));
}

TEST(health_reports_a_version_that_moves_with_every_record) {
    TAK_Ledger_Init(&g_l);
    ASSERT(answer("GET /api/health HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"version\":0"));
    record(1000, "one", "A", "B", 0);
    ASSERT(answer("GET /api/health HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"games\":1,\"disputed\":0,\"refused\":0,\"version\":1"));
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 1, 1));
    ASSERT(answer("GET /health HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("\"version\":2"));
}

/* The limits are sized to the buffer. A full ledger of eight seat games,
 * every name escaped six wide and every number at its widest, still
 * fits the largest page of each route. */
TEST(the_largest_page_of_a_full_ledger_fits_the_answer) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    for (uint32_t g = 0; g < TAK_LEDGER_MATCHES_MAX; g++) {
        memset(&m, 0, sizeof m);
        m.started_ms = 0xfffffffffffffffeull;
        m.ended_ms = 0xffffffffffffffffull;
        m.end_tick = 0xffffffffu;
        m.options = 0xffffffffu;
        m.unit_cap = 0xffff;
        memset(m.map_name, 1, TAK_NET_MAP_NAME_MAX - 1);
        for (int s = 0; s < TAK_NET_SEATS; s++) {
            TAK_LedgerSeat *x = &m.seat[m.seat_count++];
            memset(x, 0, sizeof *x);
            x->seat = (uint8_t)s;
            x->kind = TAK_NSLOT_HUMAN;
            x->side = x->colour = x->team = 255;
            x->standing = (uint8_t)(s == 0);
            /* Three hundred players, so the table has more than a page. */
            uint32_t who = (g * TAK_NET_SEATS + (uint32_t)s) % 300;
            memset(x->name, 1, TAK_NET_NAME_MAX - 1);
            x->name[13] = (char)(1 + who / 17);
            x->name[14] = (char)(1 + who % 17);
            x->player_id = TAK_Ledger_PlayerId(x->name);
            x->units_built = x->kills = x->losses = x->score = x->last_alive_tick = INT32_MIN;
        }
        TAK_Ledger_Place(&m);
        ASSERT(TAK_Ledger_Record(&g_l, &m) != 0);
    }
    ASSERT_EQ_INT(TAK_LEDGER_MATCHES_MAX, (int)g_l.count);

    ASSERT(answer("GET /api/leaderboard?limit=200 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT(has("\"total\":300,\"offset\":0,\"limit\":200"));
    ASSERT(answer("GET /api/games?limit=25 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    char req[128];
    snprintf(req, sizeof req, "GET /api/players/%016llx?limit=25 HTTP/1.1\r\n\r\n",
             (unsigned long long)g_l.match[0].seat[0].player_id);
    ASSERT(answer(req) > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT(answer("GET /api/games/8192 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    /* Asking for more than a page is clamped, never a 500. */
    ASSERT(answer("GET /api/leaderboard?limit=500 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT(has("\"limit\":200"));
    ASSERT(answer("GET /api/games?limit=50 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT(has("\"limit\":25"));
}

TEST(an_answer_that_cannot_fit_is_a_500_not_a_cut_off_body) {
    TAK_Ledger_Init(&g_l);
    for (int i = 0; i < 50; i++) record(1000 + (uint64_t)i, "a map with a long name for the test", "Zach", "Lokken", 1);
    char small[512];
    size_t n = TAK_Http_Answer(&g_l, (const uint8_t *)"GET /api/games?limit=50 HTTP/1.1\r\n\r\n", 36,
                               small, sizeof small);
    /* The body outgrows a 512 byte buffer: nothing is handed back
     * rather than half a document. */
    ASSERT_EQ_INT(0, (int)n);
    /* And with room for the page it comes back whole. */
    ASSERT(answer("GET /api/games?limit=50 HTTP/1.1\r\n\r\n") > 0);
    ASSERT(has("HTTP/1.1 200"));
    ASSERT(g_out[strlen(g_out) - 1] == '}');
}

int main(void) {
    TEST_SUITE("The JSON API");
    RUN(an_empty_ledger_gives_an_empty_table_with_cors);
    RUN(the_table_and_health_count_disputed_games);
    RUN(the_table_lists_players_wins_first_and_pages);
    RUN(a_player_page_carries_their_row_and_games_newest_first);
    RUN(an_unknown_player_or_game_is_404);
    RUN(a_game_page_carries_every_seat_with_place_and_result);
    RUN(recent_games_come_newest_first);
    RUN(options_answers_the_preflight_with_no_body_and_a_post_is_refused);
    RUN(a_name_with_a_quote_in_it_is_escaped);
    RUN(health_reports_a_version_that_moves_with_every_record);
    RUN(the_largest_page_of_a_full_ledger_fits_the_answer);
    RUN(an_answer_that_cannot_fit_is_a_500_not_a_cut_off_body);
    TEST_REPORT();
}
