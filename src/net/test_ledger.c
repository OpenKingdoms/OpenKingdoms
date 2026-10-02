/*
 * test_ledger.c -- the leaderboard's store and its sums.
 *
 * Data free. The file cases use one scratch file beside the binary and
 * remove it afterwards.
 */

#include "test_framework.h"
#include "tak_net_ledger.h"
#include "tak_net_player.h"

#include <stdio.h>
#include <string.h>

/* Four and a half megabytes, which is more than a stack wants. */
static TAK_Ledger g_l;

#define SCRATCH "test_ledger_scratch.bin"

static void seat(TAK_LedgerMatch *m, uint8_t s, const char *name, int human,
                 int standing, int32_t last_tick, int32_t score) {
    TAK_LedgerSeat *x = &m->seat[m->seat_count++];
    memset(x, 0, sizeof(*x));
    x->seat = s;
    x->kind = human ? TAK_NSLOT_HUMAN : TAK_NSLOT_COMPUTER;
    x->side = (uint8_t)(s % 4);
    x->colour = s;
    x->team = 0;
    x->standing = (uint8_t)standing;
    x->player_id = human ? TAK_Ledger_PlayerId(name) : 0;
    snprintf(x->name, sizeof x->name, "%s", name);
    x->units_built = 10 + s;
    x->kills = 3 + s;
    x->losses = 2 + s;
    x->score = score;
    x->last_alive_tick = last_tick;
}

static void match(TAK_LedgerMatch *m, uint64_t ended, const char *map) {
    memset(m, 0, sizeof(*m));
    m->relay_match_id = 9;
    m->started_ms = ended - 60000;
    m->ended_ms = ended;
    m->end_tick = 3600;
    m->options = TAK_ROOMOPT_LINE_OF_SIGHT;
    m->unit_cap = 500;
    m->stats_version = TAK_NET_STATS_VERSION;
    snprintf(m->map_name, sizeof m->map_name, "%s", map);
    for (int i = 0; i < TAK_NET_FINGERPRINT_BYTES; i++) m->map_fingerprint[i] = (uint8_t)(i * 7);
}

/* A seat keyed by the device with token `t`, all bytes t. */
static void device_seat(TAK_LedgerMatch *m, uint8_t s, const char *name, uint8_t t,
                        int standing, int32_t last_tick, int32_t score) {
    seat(m, s, name, 1, standing, last_tick, score);
    uint8_t token[TAK_NET_TOKEN_BYTES];
    memset(token, t, sizeof token);
    m->seat[m->seat_count - 1].ident = TAK_LEDGER_IDENT_DEVICE;
    m->seat[m->seat_count - 1].player_id = TAK_Player_FromToken(token);
}

static uint64_t device(uint8_t t) {
    uint8_t token[TAK_NET_TOKEN_BYTES];
    memset(token, t, sizeof token);
    return TAK_Player_FromToken(token);
}

/* ── Identity ─────────────────────────────────────────────────────────── */

/* The id is pinned, because the game works out its own to link the
 * player's page and has to reach the same one the relay does. */
TEST(a_device_is_one_player_by_a_one_way_id_of_its_token) {
    uint8_t token[TAK_NET_TOKEN_BYTES];
    for (int i = 0; i < TAK_NET_TOKEN_BYTES; i++) token[i] = (uint8_t)(i + 1);
    ASSERT(TAK_Player_FromToken(token) == 0x3f1788d638c56192ull);
    token[15] ^= 1;
    ASSERT(TAK_Player_FromToken(token) != 0x3f1788d638c56192ull);
    memset(token, 0, sizeof token);
    ASSERT(TAK_Player_FromToken(token) == 0);
    ASSERT(device(1) != 0 && device(1) != device(2));
}

TEST(a_typed_name_is_one_player_however_it_is_typed) {
    uint64_t z = TAK_Ledger_PlayerId("Zach");
    ASSERT(z != 0);
    ASSERT(TAK_Ledger_PlayerId(" zach ") == z);
    ASSERT(TAK_Ledger_PlayerId("ZACH") == z);
    ASSERT(TAK_Ledger_PlayerId("\tZach") == z);
    ASSERT(TAK_Ledger_PlayerId("Zach2") != z);
    ASSERT(TAK_Ledger_PlayerId("Zac h") != z);
    ASSERT(TAK_Ledger_PlayerId("") == 0);
    ASSERT(TAK_Ledger_PlayerId("   ") == 0);
    ASSERT(TAK_Ledger_PlayerId(NULL) == 0);
}

/* ── Placing ──────────────────────────────────────────────────────────── */

TEST(everyone_standing_shares_first_and_the_rest_rank_by_when_they_fell) {
    TAK_LedgerMatch m;
    match(&m, 1000, "Vain Blessings");
    seat(&m, 0, "A", 1, 1, 500, 0);
    seat(&m, 1, "B", 1, 1, 500, 0);
    seat(&m, 2, "C", 1, 0, 300, 0);
    seat(&m, 3, "D", 1, 0, 200, 0);
    seat(&m, 4, "E", 1, 0, 300, 0);
    TAK_Ledger_Place(&m);
    ASSERT_EQ_INT(1, m.seat[0].place);
    ASSERT_EQ_INT(1, m.seat[1].place);
    ASSERT_EQ_INT(3, m.seat[2].place);
    ASSERT_EQ_INT(5, m.seat[3].place);
    ASSERT_EQ_INT(3, m.seat[4].place);
    ASSERT_EQ_INT(TAK_LEDGER_WON, m.seat[0].result);
    ASSERT_EQ_INT(TAK_LEDGER_WON, m.seat[1].result);
    ASSERT_EQ_INT(TAK_LEDGER_LOST, m.seat[2].result);
    ASSERT_EQ_INT(TAK_LEDGER_LOST, m.seat[3].result);
    /* A fallen seat that lasted longer than a standing one is still
     * below it: standing beats any fall. */
    m.seat[2].last_alive_tick = 900;
    TAK_Ledger_Place(&m);
    ASSERT_EQ_INT(3, m.seat[2].place);
    ASSERT_EQ_INT(4, m.seat[4].place);
}

TEST(nobody_standing_means_everyone_lost) {
    TAK_LedgerMatch m;
    match(&m, 1000, "map");
    seat(&m, 0, "A", 1, 0, 100, 0);
    seat(&m, 1, "B", 1, 0, 400, 0);
    TAK_Ledger_Place(&m);
    ASSERT_EQ_INT(TAK_LEDGER_LOST, m.seat[0].result);
    ASSERT_EQ_INT(TAK_LEDGER_LOST, m.seat[1].result);
    ASSERT_EQ_INT(2, m.seat[0].place);
    ASSERT_EQ_INT(1, m.seat[1].place);
}

/* ── Records ──────────────────────────────────────────────────────────── */

TEST(a_record_gets_the_next_id_and_can_be_found) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    match(&m, 1000, "first");
    seat(&m, 0, "A", 1, 1, 100, 0);
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Record(&g_l, &m));
    match(&m, 2000, "second");
    seat(&m, 0, "A", 1, 1, 100, 0);
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Record(&g_l, &m));
    const TAK_LedgerMatch *f = TAK_Ledger_Find(&g_l, 2);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ_STR("second", f->map_name);
    ASSERT_EQ_INT(1, f->reports);
    ASSERT_NOT_NULL(TAK_Ledger_Find(&g_l, 1));
    ASSERT_NULL(TAK_Ledger_Find(&g_l, 3));
    ASSERT_NULL(TAK_Ledger_Find(&g_l, 0));
    ASSERT_EQ_INT(2, (int)g_l.version);
}

TEST(a_record_survives_its_own_codec) {
    TAK_LedgerMatch a, b;
    match(&a, 123456789012ull, "Crossing the Styx");
    seat(&a, 0, "Zach", 1, 1, 3600, 12345);
    seat(&a, 1, "Computer", 0, 0, 1800, -5);
    seat(&a, 5, "Lokken", 1, 0, 2400, 999);
    a.id = 42;
    a.reports = 3;
    a.disputed = 1;
    TAK_Ledger_Place(&a);
    uint8_t buf[4096];
    size_t n = TAK_Ledger_EncodeMatch(&a, buf, sizeof buf);
    ASSERT(n > 7);
    ASSERT_EQ_INT(TAK_LEDGER_TAG_MATCH, buf[0]);
    /* Tag, length, payload, checksum. The payload is what decodes. */
    size_t payload = n - 3 - 4;
    ASSERT_EQ_INT((int)payload, (int)(buf[1] | (buf[2] << 8)));
    ASSERT_EQ_INT(0, TAK_Ledger_DecodeMatch(&b, buf + 3, payload));
    ASSERT(memcmp(&a, &b, sizeof a) == 0);
    /* Short by a byte, or long by one, is refused. */
    ASSERT(TAK_Ledger_DecodeMatch(&b, buf + 3, payload - 1) != 0);
    ASSERT(TAK_Ledger_DecodeMatch(&b, buf + 3, payload + 1) != 0);
    /* A whole record loads, and the same bytes with one flipped do not. */
    TAK_Ledger_Init(&g_l);
    ASSERT_EQ_INT((int)n, (int)TAK_Ledger_Load(&g_l, buf, n, 0));
    ASSERT_EQ_INT(1, (int)g_l.count);
    buf[20] ^= 0x40;
    TAK_Ledger_Init(&g_l);
    ASSERT_EQ_INT((int)n, (int)TAK_Ledger_Load(&g_l, buf, n, 0));
    ASSERT_EQ_INT(0, (int)g_l.count);
    ASSERT_EQ_INT((int)n, (int)g_l.bad_bytes);
    buf[20] ^= 0x40;
    /* And a seat count past the cap. */
    a.seat_count = TAK_NET_SEATS + 1;
    ASSERT_EQ_INT(0, (int)TAK_Ledger_EncodeMatch(&a, buf, sizeof buf));
}

TEST(a_full_ledger_refuses_rather_than_forgetting) {
    TAK_Ledger_Init(&g_l);
    g_l.count = TAK_LEDGER_MATCHES_MAX;
    g_l.next_id = TAK_LEDGER_MATCHES_MAX + 1;
    TAK_LedgerMatch m;
    match(&m, 1000, "map");
    seat(&m, 0, "A", 1, 1, 100, 0);
    ASSERT_EQ_INT(0, (int)TAK_Ledger_Record(&g_l, &m));
    ASSERT_EQ_INT(1, (int)g_l.refused);
}

TEST(same_tallies_notices_a_changed_number) {
    TAK_LedgerMatch a, b;
    match(&a, 1000, "map");
    seat(&a, 0, "A", 1, 1, 100, 50);
    seat(&a, 1, "B", 1, 0, 90, 20);
    b = a;
    ASSERT_EQ_INT(1, TAK_Ledger_SameTallies(&a, &b));
    b.seat[1].kills++;
    ASSERT_EQ_INT(0, TAK_Ledger_SameTallies(&a, &b));
    b = a;
    b.seat[0].standing = 0;
    ASSERT_EQ_INT(0, TAK_Ledger_SameTallies(&a, &b));
    b = a;
    b.end_tick++;
    ASSERT_EQ_INT(0, TAK_Ledger_SameTallies(&a, &b));
}

/* ── Sums ─────────────────────────────────────────────────────────────── */

/* Three games. Zach wins two, Lokken wins one, a computer sat in one
 * and is never a row. Zach's later game spells the name in capitals,
 * which is how the table shows it afterwards. */
static void three_games(void) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    match(&m, 1000, "one");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    seat(&m, 1, "Lokken", 1, 0, 400, 30);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    match(&m, 2000, "two");
    seat(&m, 0, "Lokken", 1, 1, 700, 80);
    seat(&m, 1, "Zach", 1, 0, 500, 40);
    seat(&m, 2, "Computer", 0, 0, 100, 5);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    match(&m, 3000, "three");
    seat(&m, 3, "ZACH", 1, 1, 900, 200);
    seat(&m, 4, "lokken", 1, 0, 800, 60);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
}

TEST(the_table_sums_every_seat_by_player_and_puts_wins_first) {
    three_games();
    static TAK_LedgerRow rows[16];
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Table(&g_l, rows, 16));
    ASSERT_EQ_STR("ZACH", rows[0].name);
    ASSERT(rows[0].player_id == TAK_Ledger_PlayerId("zach"));
    ASSERT_EQ_INT(3, (int)rows[0].games);
    ASSERT_EQ_INT(2, (int)rows[0].wins);
    ASSERT_EQ_INT(1, (int)rows[0].losses);
    ASSERT_EQ_INT(340, (int)rows[0].score);
    ASSERT_EQ_INT(10 + 11 + 13, (int)rows[0].units_built);
    ASSERT_EQ_INT(3 + 4 + 6, (int)rows[0].kills);
    ASSERT_EQ_INT(2 + 3 + 5, (int)rows[0].units_lost);
    ASSERT_EQ_INT(600 + 500 + 900, (int)rows[0].ticks_alive);
    ASSERT_EQ_INT(1000, (int)rows[0].first_played_ms);
    ASSERT_EQ_INT(3000, (int)rows[0].last_played_ms);
    ASSERT_EQ_STR("lokken", rows[1].name);
    ASSERT_EQ_INT(1, (int)rows[1].wins);
    ASSERT_EQ_INT(2, (int)rows[1].losses);
    ASSERT_EQ_INT(170, (int)rows[1].score);
    /* One row asked for on its own is the same row. */
    TAK_LedgerRow one;
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("LOKKEN"), &one));
    ASSERT(memcmp(&one, &rows[1], sizeof one) == 0);
    ASSERT_EQ_INT(0, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("nobody"), &one));
    /* A cap smaller than the players still fills what it can. */
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Table(&g_l, rows, 1));
}

/* A disputed game has no agreed numbers, so it counts for nobody: not a
 * game, not a win, not a point. It is still there to look at. */
TEST(a_disputed_game_is_left_out_of_every_sum) {
    three_games();
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 3, 0));
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Disputed(&g_l));
    static TAK_LedgerRow rows[16];
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Table(&g_l, rows, 16));
    /* Zach loses the third game's win and its capitals, and now ties
     * Lokken on wins, where the score decides. */
    ASSERT_EQ_STR("Zach", rows[0].name);
    ASSERT_EQ_INT(1, (int)rows[0].wins);
    ASSERT_EQ_INT(1, (int)rows[0].losses);
    ASSERT_EQ_INT(2, (int)rows[0].games);
    ASSERT_EQ_INT(140, (int)rows[0].score);
    ASSERT_EQ_INT(2000, (int)rows[0].last_played_ms);
    ASSERT_EQ_STR("Lokken", rows[1].name);
    ASSERT_EQ_INT(1, (int)rows[1].wins);
    ASSERT_EQ_INT(110, (int)rows[1].score);
    TAK_LedgerRow one;
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("zach"), &one));
    ASSERT_EQ_INT(2, (int)one.games);
    /* The game itself is still found and still listed in a history. */
    ASSERT_NOT_NULL(TAK_Ledger_Find(&g_l, 3));
    uint32_t ids[8], total = 0;
    ASSERT_EQ_INT(3, (int)TAK_Ledger_History(&g_l, TAK_Ledger_PlayerId("zach"), 0, ids, 8, &total));
    ASSERT_EQ_INT(3, (int)ids[0]);
    /* A player whose only game is disputed has no row at all. */
    TAK_LedgerMatch m;
    match(&m, 4000, "four");
    seat(&m, 0, "Solo", 1, 1, 100, 5);
    seat(&m, 1, "Zach", 1, 0, 50, 5);
    TAK_Ledger_Place(&m);
    ASSERT_EQ_INT(4, (int)TAK_Ledger_Record(&g_l, &m));
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 4, 0));
    ASSERT_EQ_INT(0, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("solo"), &one));
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Table(&g_l, rows, 16));
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Disputed(&g_l));
}

TEST(ties_on_wins_go_to_score_then_games) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    match(&m, 1000, "a");
    seat(&m, 0, "Low", 1, 1, 600, 10);
    seat(&m, 1, "High", 1, 1, 600, 90);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    static TAK_LedgerRow rows[4];
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Table(&g_l, rows, 4));
    ASSERT_EQ_STR("High", rows[0].name);
    ASSERT_EQ_STR("Low", rows[1].name);
}

TEST(history_is_newest_first_and_pages) {
    TAK_Ledger_Init(&g_l);
    for (int i = 1; i <= 5; i++) {
        TAK_LedgerMatch m;
        match(&m, (uint64_t)i * 1000, "map");
        if (i != 3) seat(&m, 0, "Zach", 1, 1, 100, 0);
        seat(&m, 1, "Other", 1, 0, 50, 0);
        TAK_Ledger_Place(&m);
        TAK_Ledger_Record(&g_l, &m);
    }
    uint64_t z = TAK_Ledger_PlayerId("zach");
    uint32_t ids[8], total = 0;
    ASSERT_EQ_INT(2, (int)TAK_Ledger_History(&g_l, z, 0, ids, 2, &total));
    ASSERT_EQ_INT(4, (int)total);
    ASSERT_EQ_INT(5, (int)ids[0]);
    ASSERT_EQ_INT(4, (int)ids[1]);
    ASSERT_EQ_INT(2, (int)TAK_Ledger_History(&g_l, z, 2, ids, 8, &total));
    ASSERT_EQ_INT(2, (int)ids[0]);
    ASSERT_EQ_INT(1, (int)ids[1]);
    ASSERT_EQ_INT(0, (int)TAK_Ledger_History(&g_l, z, 4, ids, 8, &total));
    ASSERT_EQ_INT(4, (int)total);
    ASSERT_EQ_INT(0, (int)TAK_Ledger_History(&g_l, 0, 0, ids, 8, &total));
    ASSERT_EQ_INT(0, (int)total);
}

TEST(an_empty_ledger_answers_with_nothing) {
    TAK_Ledger_Init(&g_l);
    static TAK_LedgerRow rows[4];
    TAK_LedgerRow one;
    uint32_t ids[4], total = 7;
    ASSERT_EQ_INT(0, (int)TAK_Ledger_Table(&g_l, rows, 4));
    ASSERT_EQ_INT(0, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("Zach"), &one));
    ASSERT_EQ_INT(0, (int)TAK_Ledger_History(&g_l, 1, 0, ids, 4, &total));
    ASSERT_EQ_INT(0, (int)total);
    ASSERT_NULL(TAK_Ledger_Find(&g_l, 1));
}

/* A record from before devices keeps its first format byte for byte,
 * and one with a device seat carries each seat's ident. */
TEST(a_device_record_survives_its_codec_and_an_old_one_keeps_its_format) {
    TAK_LedgerMatch a, b;
    uint8_t buf[4096];
    match(&a, 5000, "old");
    seat(&a, 0, "Zach", 1, 1, 600, 100);
    seat(&a, 1, "Computer", 0, 0, 100, 0);
    TAK_Ledger_Place(&a);
    size_t n = TAK_Ledger_EncodeMatch(&a, buf, sizeof buf);
    ASSERT(n > 0);
    ASSERT_EQ_INT(TAK_LEDGER_TAG_MATCH, buf[0]);

    match(&a, 5000, "new");
    device_seat(&a, 0, "Zach", 7, 1, 600, 100);
    seat(&a, 1, "Computer", 0, 0, 100, 0);
    TAK_Ledger_Place(&a);
    size_t m = TAK_Ledger_EncodeMatch(&a, buf, sizeof buf);
    ASSERT_EQ_INT((int)n + 2, (int)m);
    ASSERT_EQ_INT(TAK_LEDGER_TAG_MATCH_DEVICE, buf[0]);
    size_t payload = m - 3 - 4;
    ASSERT_EQ_INT(0, TAK_Ledger_DecodeMatchTag(&b, buf[0], buf + 3, payload));
    ASSERT(memcmp(&a, &b, sizeof a) == 0);
    /* Read as the old tag it is two bytes too long. */
    ASSERT(TAK_Ledger_DecodeMatch(&b, buf + 3, payload) != 0);
}

/* Rows from before devices are frozen. A device that plays under an old
 * name, however many old names, gets its own row and nothing else, and
 * the old rows read exactly as they did. */
TEST(an_old_name_row_stays_its_own_and_no_device_takes_it_over) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    for (int g = 0; g < 3; g++) {
        match(&m, 1000 + (uint64_t)g, "before");
        seat(&m, 0, "Zed", 1, 1, 600, 100);
        seat(&m, 1, "Bob", 1, 0, 400, 30);
        TAK_Ledger_Place(&m);
        TAK_Ledger_Record(&g_l, &m);
    }
    TAK_LedgerRow zed0, bob0, row;
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("Zed"), &zed0));
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("Bob"), &bob0));

    /* Device 9 plays as Zed and as Bob, against a second device of its own. */
    match(&m, 2000, "after");
    device_seat(&m, 0, "Zed", 9, 0, 300, 5);
    device_seat(&m, 1, "Other", 8, 1, 600, 50);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    match(&m, 3000, "after");
    device_seat(&m, 0, "Bob", 9, 1, 600, 50);
    device_seat(&m, 1, "Other", 8, 0, 300, 5);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);

    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("Zed"), &row));
    ASSERT(memcmp(&row, &zed0, sizeof row) == 0);
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, TAK_Ledger_PlayerId("Bob"), &row));
    ASSERT(memcmp(&row, &bob0, sizeof row) == 0);
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, device(9), &row));
    ASSERT_EQ_INT(2, (int)row.games);
    ASSERT_EQ_INT(1, (int)row.wins);
    ASSERT_EQ_STR("Bob", row.name);
    uint32_t ids[8], total = 0;
    ASSERT_EQ_INT(2, (int)TAK_Ledger_History(&g_l, device(9), 0, ids, 8, &total));
    ASSERT_EQ_INT(3, (int)TAK_Ledger_History(&g_l, TAK_Ledger_PlayerId("Zed"), 0, ids, 8, &total));
    static TAK_LedgerRow rows[8];
    ASSERT_EQ_INT(4, (int)TAK_Ledger_Table(&g_l, rows, 8));
    /* Every row's games are games it has on its own page. */
    for (int r = 0; r < 4; r++) {
        TAK_Ledger_History(&g_l, rows[r].player_id, 0, ids, 8, &total);
        ASSERT_EQ_INT((int)total, (int)rows[r].games);
        ASSERT_EQ_INT((int)rows[r].games, (int)(rows[r].wins + rows[r].losses));
    }
}

/* Two tabs of one browser share a token, so one device can sit in two
 * seats of one game. It cannot both win and lose it, so that game counts
 * for neither seat, and nothing is counted twice. */
TEST(a_device_in_two_seats_of_one_game_counts_it_for_neither) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    match(&m, 1000, "both sides");
    device_seat(&m, 0, "Zed", 1, 1, 600, 100);
    device_seat(&m, 1, "Bob", 1, 0, 300, 5);
    device_seat(&m, 2, "Elsin", 2, 0, 200, 5);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    ASSERT(TAK_Ledger_SeatPlayer(&m, 0) == 0);
    ASSERT(TAK_Ledger_SeatPlayer(&m, 1) == 0);
    ASSERT(TAK_Ledger_SeatPlayer(&m, 2) == device(2));
    TAK_LedgerRow row;
    ASSERT_EQ_INT(0, TAK_Ledger_RowFor(&g_l, device(1), &row));
    static TAK_LedgerRow rows[8];
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Table(&g_l, rows, 8));
    ASSERT(rows[0].player_id == device(2));

    match(&m, 2000, "fair");
    device_seat(&m, 0, "Zed", 1, 1, 600, 100);
    device_seat(&m, 1, "Elsin", 2, 0, 300, 5);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, device(1), &row));
    ASSERT_EQ_INT(1, (int)row.games);
    ASSERT_EQ_INT(1, (int)row.wins);
    ASSERT_EQ_INT(100, (int)row.score);
}

TEST(a_player_goes_by_the_name_they_last_played_under) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    char name[TAK_NET_NAME_MAX];
    ASSERT_EQ_INT(0, TAK_Ledger_CurrentName(&g_l, device(1), name));
    match(&m, 1000, "a");
    device_seat(&m, 0, "Early", 1, 1, 600, 1);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    match(&m, 2000, "b");
    device_seat(&m, 0, "Later", 1, 1, 600, 1);
    device_seat(&m, 1, "Other", 2, 0, 300, 1);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    ASSERT_EQ_INT(1, TAK_Ledger_CurrentName(&g_l, device(1), name));
    ASSERT_EQ_STR("Later", name);
    ASSERT_EQ_INT(1, TAK_Ledger_CurrentName(&g_l, device(2), name));
    ASSERT_EQ_STR("Other", name);
}

TEST(games_filter_by_player_name_map_and_date) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    match(&m, 1000, "Two Castles");
    device_seat(&m, 0, "Zach", 1, 1, 600, 1);
    device_seat(&m, 1, "Lokken", 2, 0, 300, 1);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    match(&m, 2000, "Vain Blessings");
    device_seat(&m, 0, "Zachary", 1, 1, 600, 1);
    seat(&m, 1, "Computer", 0, 0, 300, 1);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    match(&m, 3000, "two rivers");
    device_seat(&m, 0, "Elsin", 3, 1, 600, 1);
    device_seat(&m, 1, "Lokken", 2, 0, 300, 1);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);

    uint32_t ids[8], total = 0;
    TAK_LedgerFilter f;
    memset(&f, 0, sizeof f);
    ASSERT_EQ_INT(3, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    ASSERT_EQ_INT(3, (int)TAK_Ledger_Games(&g_l, NULL, 0, ids, 8, &total));
    f.map = "TWO";
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    ASSERT_EQ_INT(3, (int)ids[0]);
    ASSERT_EQ_INT(1, (int)ids[1]);
    f.from_ms = 1500;
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    ASSERT_EQ_INT(3, (int)ids[0]);
    f.map = NULL;
    f.to_ms = 2000;
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    ASSERT_EQ_INT(2, (int)ids[0]);

    memset(&f, 0, sizeof f);
    f.name = "zach";
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    /* A computer seat's name is not a player's. */
    f.name = "comp";
    ASSERT_EQ_INT(0, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    /* Or a game of someone whose name now holds it, by id. */
    uint64_t also[1] = { device(3) };
    f.name = "nobody types this";
    f.also = also;
    f.also_count = 1;
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    ASSERT_EQ_INT(3, (int)ids[0]);

    memset(&f, 0, sizeof f);
    f.player = device(2);
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Games(&g_l, &f, 1, ids, 8, &total));
    ASSERT_EQ_INT(2, (int)total);
    ASSERT_EQ_INT(1, (int)ids[0]);
}

/* ── The file ─────────────────────────────────────────────────────────── */

TEST(a_file_holds_the_records_across_a_reopen) {
    remove(SCRATCH);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(0, (int)g_l.count);
    TAK_LedgerMatch m;
    match(&m, 5000, "kept");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    seat(&m, 1, "Lokken", 1, 0, 400, 30);
    TAK_Ledger_Place(&m);
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Record(&g_l, &m));
    match(&m, 6000, "also kept");
    seat(&m, 0, "Zach", 1, 0, 100, 1);
    seat(&m, 1, "Lokken", 1, 1, 400, 30);
    TAK_Ledger_Place(&m);
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Record(&g_l, &m));
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 1, 1));
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 1, 1));
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 2, 0));
    ASSERT_EQ_INT(-1, TAK_Ledger_Confirm(&g_l, 9, 1));
    TAK_LedgerMatch before1 = *TAK_Ledger_Find(&g_l, 1);
    TAK_LedgerMatch before2 = *TAK_Ledger_Find(&g_l, 2);
    TAK_Ledger_Close(&g_l);

    memset(&g_l, 0xcc, sizeof g_l);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_EQ_INT(0, (int)g_l.bad_records);
    ASSERT_EQ_INT(3, (int)g_l.next_id);
    const TAK_LedgerMatch *a = TAK_Ledger_Find(&g_l, 1);
    const TAK_LedgerMatch *b = TAK_Ledger_Find(&g_l, 2);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_INT(3, a->reports);
    ASSERT_EQ_INT(0, a->disputed);
    ASSERT_EQ_INT(1, b->reports);
    ASSERT_EQ_INT(1, b->disputed);
    ASSERT(memcmp(a, &before1, sizeof before1) == 0);
    ASSERT(memcmp(b, &before2, sizeof before2) == 0);
    /* And the next record goes on the end. */
    match(&m, 7000, "third");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    ASSERT_EQ_INT(3, (int)TAK_Ledger_Record(&g_l, &m));
    TAK_Ledger_Close(&g_l);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(3, (int)g_l.count);
    TAK_Ledger_Close(&g_l);
    remove(SCRATCH);
}

static uint32_t test_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xffffffffu;
}

/* Device records come back on open and when the file is made whole
 * beside itself. A tag 4 record, which only a build that never shipped
 * wrote, is skipped by its length and changes nothing. */
TEST(device_records_survive_a_reopen_and_a_rewrite_and_tag_4_is_inert) {
    remove(SCRATCH);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    TAK_LedgerMatch m;
    match(&m, 1000, "before");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    match(&m, 2000, "after");
    device_seat(&m, 0, "Zach", 1, 1, 600, 100);
    TAK_Ledger_Place(&m);
    TAK_Ledger_Record(&g_l, &m);
    TAK_Ledger_Close(&g_l);

    /* The unshipped claim: Zach's name for device 1. */
    uint8_t rec[3 + 16 + 4];
    rec[0] = 4;
    rec[1] = 16;
    rec[2] = 0;
    uint64_t name = TAK_Ledger_PlayerId("Zach"), who = device(1);
    for (int k = 0; k < 8; k++) {
        rec[3 + k] = (uint8_t)(name >> (8 * k));
        rec[11 + k] = (uint8_t)(who >> (8 * k));
    }
    uint32_t crc = test_crc32(rec, 19);
    for (int k = 0; k < 4; k++) rec[19 + k] = (uint8_t)(crc >> (8 * k));
    FILE *f = fopen(SCRATCH, "ab");
    ASSERT_NOT_NULL(f);
    fwrite(rec, 1, sizeof rec, f);
    fclose(f);

    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_EQ_INT(1, (int)g_l.bad_records);
    ASSERT_EQ_INT(0, (int)g_l.bad_bytes);
    ASSERT_EQ_INT(TAK_LEDGER_IDENT_DEVICE, TAK_Ledger_Find(&g_l, 2)->seat[0].ident);
    TAK_LedgerRow row;
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, device(1), &row));
    ASSERT_EQ_INT(1, (int)row.games);
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, name, &row));
    ASSERT_EQ_INT(1, (int)row.games);
    TAK_Ledger_Close(&g_l);

    /* A stray byte on the end makes the next open rewrite the file. */
    f = fopen(SCRATCH, "ab");
    ASSERT_NOT_NULL(f);
    fputc(0x5a, f);
    fclose(f);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(1, (int)g_l.bad_bytes);
    TAK_Ledger_Close(&g_l);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(0, (int)g_l.bad_bytes);
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_EQ_INT(TAK_LEDGER_IDENT_DEVICE, TAK_Ledger_Find(&g_l, 2)->seat[0].ident);
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, device(1), &row));
    ASSERT_EQ_INT(1, (int)row.games);
    TAK_Ledger_Close(&g_l);
    remove(SCRATCH);
}

TEST(a_torn_tail_is_dropped_and_the_file_made_whole) {
    remove(SCRATCH);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    TAK_LedgerMatch m;
    match(&m, 5000, "whole");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Record(&g_l, &m));
    TAK_Ledger_Close(&g_l);
    FILE *f = fopen(SCRATCH, "rb");
    ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long whole = ftell(f);
    fclose(f);

    /* The process died half way through the next record. */
    f = fopen(SCRATCH, "ab");
    ASSERT_NOT_NULL(f);
    uint8_t torn[7] = { TAK_LEDGER_TAG_MATCH, 200, 0, 1, 2, 3, 4 };
    fwrite(torn, 1, sizeof torn, f);
    fclose(f);

    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(1, (int)g_l.count);
    ASSERT_EQ_STR("whole", TAK_Ledger_Find(&g_l, 1)->map_name);
    TAK_Ledger_Close(&g_l);
    f = fopen(SCRATCH, "rb");
    ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    ASSERT_EQ_INT((int)whole, (int)ftell(f));
    fclose(f);
    remove(SCRATCH);
}

/* The checksum of a hand made record, the way the writer computes it. */
static uint32_t crc_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xffffffffu;
}

static size_t sealed(uint8_t *out, uint8_t tag, const uint8_t *body, size_t n) {
    out[0] = tag;
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)(n >> 8);
    memcpy(out + 3, body, n);
    uint32_t c = crc_of(out, 3 + n);
    out[3 + n] = (uint8_t)c;
    out[4 + n] = (uint8_t)(c >> 8);
    out[5 + n] = (uint8_t)(c >> 16);
    out[6 + n] = (uint8_t)(c >> 24);
    return 7 + n;
}

TEST(a_record_in_the_middle_that_will_not_read_is_skipped_not_fatal) {
    TAK_Ledger_Init(&g_l);
    uint8_t buf[8192];
    TAK_LedgerMatch m;
    match(&m, 1000, "good");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    m.id = 1;
    size_t n1 = TAK_Ledger_EncodeMatch(&m, buf, sizeof buf);
    /* A sound record with a seat count nobody can hold. */
    uint8_t nine[5] = { 9, 9, 9, 9, 9 };
    size_t nb = sealed(buf + n1, TAK_LEDGER_TAG_MATCH, nine, sizeof nine);
    /* A sound record with a tag from some later version. */
    uint8_t later[3] = { 1, 2, 3 };
    size_t nf = sealed(buf + n1 + nb, 77, later, sizeof later);
    m.id = 2;
    size_t n2 = TAK_Ledger_EncodeMatch(&m, buf + n1 + nb + nf, sizeof buf);
    size_t total = n1 + nb + nf + n2;
    ASSERT_EQ_INT((int)total, (int)TAK_Ledger_Load(&g_l, buf, total, 0));
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_EQ_INT(2, (int)g_l.bad_records);
    ASSERT_EQ_INT(0, (int)g_l.bad_bytes);
}

/* One wrong length byte used to declare the rest of the file torn, or
 * shift every later record. Now it costs that record and no other. */
TEST(a_corrupt_length_costs_one_record_and_the_file_is_made_whole_beside_itself) {
    remove(SCRATCH);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    TAK_LedgerMatch m;
    const char *maps[3] = { "first", "second", "third" };
    for (int i = 0; i < 3; i++) {
        match(&m, 1000u * (uint64_t)(i + 1), maps[i]);
        seat(&m, 0, "Zach", 1, 1, 600, 100);
        seat(&m, 1, "Lokken", 1, 0, 400, 30);
        TAK_Ledger_Place(&m);
        ASSERT_EQ_INT(i + 1, (int)TAK_Ledger_Record(&g_l, &m));
    }
    ASSERT_EQ_INT(0, TAK_Ledger_Confirm(&g_l, 3, 1));
    TAK_Ledger_Close(&g_l);

    /* Find the second record and break its length byte. */
    FILE *f = fopen(SCRATCH, "rb");
    ASSERT_NOT_NULL(f);
    static uint8_t file[16384];
    size_t len = fread(file, 1, sizeof file, f);
    fclose(f);
    size_t first = 12 + 3 + (size_t)(file[13] | (file[14] << 8)) + 4;
    ASSERT_EQ_INT(TAK_LEDGER_TAG_MATCH, file[first]);
    file[first + 1] ^= 0x33;
    f = fopen(SCRATCH, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(file, 1, len, f);
    fclose(f);

    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_NOT_NULL(TAK_Ledger_Find(&g_l, 1));
    ASSERT_NULL(TAK_Ledger_Find(&g_l, 2));
    const TAK_LedgerMatch *third = TAK_Ledger_Find(&g_l, 3);
    ASSERT_NOT_NULL(third);
    ASSERT_EQ_STR("third", third->map_name);
    /* The confirmation after the bad record was found and applied. */
    ASSERT_EQ_INT(2, third->reports);
    ASSERT(g_l.bad_bytes > 0);
    TAK_Ledger_Close(&g_l);

    /* The file was rewritten without the bad bytes, and nothing was
     * left beside it. A third open finds it clean. */
    f = fopen(SCRATCH ".tmp", "rb");
    ASSERT_NULL(f);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_EQ_INT(0, (int)g_l.bad_bytes);
    ASSERT_EQ_INT(0, (int)g_l.bad_records);
    ASSERT_EQ_INT(4, (int)g_l.next_id);
    TAK_Ledger_Close(&g_l);
    remove(SCRATCH);
}

/* A length no record could have is stepped over like any other bad
 * byte, never taken as a torn tail that swallows the rest. */
TEST(an_impossible_length_is_a_skip_not_a_torn_tail) {
    remove(SCRATCH);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    TAK_LedgerMatch m;
    match(&m, 1000, "before");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Record(&g_l, &m));
    TAK_Ledger_Close(&g_l);
    /* A record header claiming 65535 bytes, then a good record. */
    FILE *f = fopen(SCRATCH, "ab");
    ASSERT_NOT_NULL(f);
    uint8_t huge[3] = { TAK_LEDGER_TAG_MATCH, 0xff, 0xff };
    fwrite(huge, 1, sizeof huge, f);
    match(&m, 2000, "after");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    m.id = 2;
    uint8_t rec[4096];
    size_t n = TAK_Ledger_EncodeMatch(&m, rec, sizeof rec);
    fwrite(rec, 1, n, f);
    fclose(f);

    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_EQ_STR("after", TAK_Ledger_Find(&g_l, 2)->map_name);
    ASSERT_EQ_INT(3, (int)g_l.bad_bytes);
    TAK_Ledger_Close(&g_l);
    remove(SCRATCH);
}

/* A volume mounted fresh, or a touch, leaves an empty file. That is a
 * new ledger, not a broken one. */
TEST(an_empty_file_is_a_new_ledger) {
    remove(SCRATCH);
    FILE *f = fopen(SCRATCH, "wb");
    ASSERT_NOT_NULL(f);
    fclose(f);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(0, (int)g_l.count);
    TAK_LedgerMatch m;
    match(&m, 1000, "first");
    seat(&m, 0, "Zach", 1, 1, 600, 100);
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Record(&g_l, &m));
    TAK_Ledger_Close(&g_l);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(1, (int)g_l.count);
    TAK_Ledger_Close(&g_l);
    remove(SCRATCH);
}

TEST(a_file_that_is_not_a_ledger_is_refused_and_left_alone) {
    remove(SCRATCH);
    FILE *f = fopen(SCRATCH, "wb");
    ASSERT_NOT_NULL(f);
    fputs("this is somebody's notes\n", f);
    fclose(f);
    ASSERT_EQ_INT(-1, TAK_Ledger_Open(&g_l, SCRATCH));
    f = fopen(SCRATCH, "rb");
    ASSERT_NOT_NULL(f);
    char line[64] = { 0 };
    ASSERT_NOT_NULL(fgets(line, sizeof line, f));
    fclose(f);
    ASSERT_EQ_STR("this is somebody's notes\n", line);
    remove(SCRATCH);
    ASSERT_EQ_INT(-1, TAK_Ledger_Open(&g_l, ""));
}

/* ── Tables ───────────────────────────────────────────────────────────── */

static void mod(TAK_LedgerMatch *m, const char *name, const char *version, uint64_t content) {
    snprintf(m->mod_name, sizeof m->mod_name, "%s", name);
    snprintf(m->mod_version, sizeof m->mod_version, "%s", version);
    m->content_hash = content;
}

/* A match filed under a mod set carries it on the newer tag, and a match
 * with none keeps the tag and the bytes it had before tables. */
TEST(a_mod_record_survives_its_codec_and_a_file_and_an_unfiled_one_keeps_its_format) {
    TAK_LedgerMatch a, b;
    uint8_t buf[4096];
    match(&a, 5000, "new");
    device_seat(&a, 0, "Zach", 7, 1, 600, 100);
    seat(&a, 1, "Computer", 0, 0, 100, 0);
    TAK_Ledger_Place(&a);
    size_t plain = TAK_Ledger_EncodeMatch(&a, buf, sizeof buf);
    ASSERT_EQ_INT(TAK_LEDGER_TAG_MATCH_DEVICE, buf[0]);

    mod(&a, "TAK Enhanced", "1.4", 0xe4a1000000000014ull);
    size_t n = TAK_Ledger_EncodeMatch(&a, buf, sizeof buf);
    ASSERT_EQ_INT((int)plain + 32 + 16 + 8, (int)n);
    ASSERT_EQ_INT(TAK_LEDGER_TAG_MATCH_MOD, buf[0]);
    ASSERT_EQ_INT(0, TAK_Ledger_DecodeMatchTag(&b, buf[0], buf + 3, n - 7));
    ASSERT(memcmp(&a, &b, sizeof a) == 0);
    ASSERT(TAK_Ledger_DecodeMatchTag(&b, TAK_LEDGER_TAG_MATCH_DEVICE, buf + 3, n - 7) != 0);

    /* A name-keyed seat in a mod game still writes its ident. */
    match(&a, 5000, "named");
    seat(&a, 0, "Zach", 1, 1, 600, 100);
    mod(&a, "Vanilla", "", 0x1234ull);
    n = TAK_Ledger_EncodeMatch(&a, buf, sizeof buf);
    ASSERT_EQ_INT(TAK_LEDGER_TAG_MATCH_MOD, buf[0]);
    ASSERT_EQ_INT(0, TAK_Ledger_DecodeMatchTag(&b, buf[0], buf + 3, n - 7));
    ASSERT(memcmp(&a, &b, sizeof a) == 0);

    remove(SCRATCH);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    match(&a, 6000, "one");
    device_seat(&a, 0, "Zach", 7, 1, 600, 100);
    mod(&a, "TAK Enhanced", "1.4", 0xe4a1000000000014ull);
    TAK_Ledger_Place(&a);
    ASSERT(TAK_Ledger_Record(&g_l, &a) != 0);
    match(&a, 7000, "two");
    device_seat(&a, 0, "Zach", 7, 1, 600, 100);
    TAK_Ledger_Place(&a);
    ASSERT(TAK_Ledger_Record(&g_l, &a) != 0);
    TAK_Ledger_Close(&g_l);
    ASSERT_EQ_INT(0, TAK_Ledger_Open(&g_l, SCRATCH));
    ASSERT_EQ_INT(2, (int)g_l.count);
    ASSERT_EQ_INT(0, (int)g_l.bad_records);
    ASSERT_EQ_STR("TAK Enhanced", g_l.match[0].mod_name);
    ASSERT_EQ_STR("1.4", g_l.match[0].mod_version);
    ASSERT(g_l.match[0].content_hash == 0xe4a1000000000014ull);
    ASSERT_EQ_STR("", g_l.match[1].mod_name);
    ASSERT(g_l.match[1].content_hash == 0);
    TAK_Ledger_Close(&g_l);
    remove(SCRATCH);
}

TEST(a_table_id_is_the_mod_name_folded_and_the_fingerprint) {
    TAK_LedgerMatch m;
    char id[TAK_LEDGER_TABLE_ID_MAX];
    match(&m, 1000, "x");
    TAK_Ledger_TableId(&m, id);
    ASSERT_EQ_STR("earlier", id);
    mod(&m, "Vanilla", "", 0x0123456789abcdefull);
    TAK_Ledger_TableId(&m, id);
    ASSERT_EQ_STR("vanilla-0123456789abcdef", id);
    mod(&m, "  TA:K  Enhanced!! ", "1.3.6", 0xfull);
    TAK_Ledger_TableId(&m, id);
    ASSERT_EQ_STR("ta-k-enhanced-000000000000000f", id);
    /* A host before protocol 4 names nothing, and is kept by its data. */
    mod(&m, "", "", 0xabcull);
    TAK_Ledger_TableId(&m, id);
    ASSERT_EQ_STR("unnamed-0000000000000abc", id);
    mod(&m, "\"<>&", "", 0xabcull);
    TAK_Ledger_TableId(&m, id);
    ASSERT_EQ_STR("unnamed-0000000000000abc", id);
    mod(&m, "0123456789012345678901234567890", "", 1);
    TAK_Ledger_TableId(&m, id);
    ASSERT_EQ_STR("0123456789012345678901234567890-0000000000000001", id);
}

/* Vanilla on its own table, each mod set on its own, the records from
 * before tables on theirs, and a player's sums kept apart between them. */
TEST(each_mod_set_keeps_its_own_table_and_vanilla_comes_first) {
    TAK_Ledger_Init(&g_l);
    TAK_LedgerMatch m;
    /* From before tables: no mod, no fingerprint. */
    match(&m, 1000, "Old");
    device_seat(&m, 0, "Zach", 1, 1, 600, 10);
    device_seat(&m, 1, "Elsin", 2, 0, 300, 5);
    TAK_Ledger_Place(&m);
    ASSERT(TAK_Ledger_Record(&g_l, &m) != 0);
    /* Two mod games and one vanilla. */
    for (int i = 0; i < 2; i++) {
        match(&m, 2000 + (uint64_t)i, "Modded");
        device_seat(&m, 0, "Zach", 1, 0, 300, 1);
        device_seat(&m, 1, "Lokken", 3, 1, 600, 2);
        mod(&m, "TAK Enhanced", i ? "1.4" : "1.4b", 0xe4a1ull);
        TAK_Ledger_Place(&m);
        ASSERT(TAK_Ledger_Record(&g_l, &m) != 0);
    }
    match(&m, 3000, "Plain");
    device_seat(&m, 0, "Zach", 1, 1, 600, 7);
    device_seat(&m, 1, "Elsin", 2, 0, 300, 3);
    mod(&m, "Vanilla", "", 0x7a11ull);
    TAK_Ledger_Place(&m);
    ASSERT(TAK_Ledger_Record(&g_l, &m) != 0);

    TAK_LedgerTable t[8];
    ASSERT_EQ_INT(3, (int)TAK_Ledger_Tables(&g_l, t, 8));
    ASSERT_EQ_STR("vanilla-0000000000007a11", t[0].id);
    ASSERT_EQ_INT(1, t[0].vanilla);
    ASSERT_EQ_INT(1, (int)t[0].games);
    ASSERT_EQ_STR("tak-enhanced-000000000000e4a1", t[1].id);
    ASSERT_EQ_INT(2, (int)t[1].games);
    /* The newest game names the table. */
    ASSERT_EQ_STR("1.4", t[1].mod_version);
    ASSERT(t[1].last_played_ms == 2001);
    ASSERT_EQ_STR("earlier", t[2].id);
    ASSERT_EQ_INT(1, (int)t[2].games);
    /* A cap keeps the first tables seen. */
    ASSERT_EQ_INT(1, (int)TAK_Ledger_Tables(&g_l, t, 1));
    ASSERT_EQ_STR("earlier", t[0].id);

    static TAK_LedgerRow rows[8];
    ASSERT_EQ_INT(3, (int)TAK_Ledger_Table(&g_l, rows, 8));
    ASSERT_EQ_INT(2, (int)TAK_Ledger_TableIn(&g_l, "tak-enhanced-000000000000e4a1", rows, 8));
    ASSERT_EQ_STR("Lokken", rows[0].name);
    ASSERT_EQ_INT(2, (int)rows[0].wins);
    ASSERT_EQ_INT(2, (int)TAK_Ledger_TableIn(&g_l, "vanilla-0000000000007a11", rows, 8));
    ASSERT_EQ_STR("Zach", rows[0].name);
    ASSERT_EQ_INT(0, (int)TAK_Ledger_TableIn(&g_l, "vanilla-0000000000000000", rows, 8));

    TAK_LedgerRow row;
    ASSERT_EQ_INT(1, TAK_Ledger_RowFor(&g_l, device(1), &row));
    ASSERT_EQ_INT(4, (int)row.games);
    ASSERT_EQ_INT(1, TAK_Ledger_RowIn(&g_l, "tak-enhanced-000000000000e4a1", device(1), &row));
    ASSERT_EQ_INT(2, (int)row.games);
    ASSERT_EQ_INT(0, (int)row.wins);
    ASSERT_EQ_INT(1, TAK_Ledger_RowIn(&g_l, "earlier", device(1), &row));
    ASSERT_EQ_INT(1, (int)row.wins);
    ASSERT_EQ_INT(0, TAK_Ledger_RowIn(&g_l, "tak-enhanced-000000000000e4a1", device(2), &row));

    TAK_LedgerFilter f;
    memset(&f, 0, sizeof f);
    f.table = "tak-enhanced-000000000000e4a1";
    uint32_t ids[8], total = 0;
    ASSERT_EQ_INT(2, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
    ASSERT_EQ_INT(3, (int)ids[0]);
    ASSERT_EQ_INT(2, (int)ids[1]);
    f.player = device(2);
    ASSERT_EQ_INT(0, (int)TAK_Ledger_Games(&g_l, &f, 0, ids, 8, &total));
}

int main(void) {
    TEST_SUITE("The ledger");
    RUN(a_typed_name_is_one_player_however_it_is_typed);
    RUN(a_device_is_one_player_by_a_one_way_id_of_its_token);
    RUN(everyone_standing_shares_first_and_the_rest_rank_by_when_they_fell);
    RUN(nobody_standing_means_everyone_lost);
    RUN(a_record_gets_the_next_id_and_can_be_found);
    RUN(a_record_survives_its_own_codec);
    RUN(a_full_ledger_refuses_rather_than_forgetting);
    RUN(same_tallies_notices_a_changed_number);
    RUN(the_table_sums_every_seat_by_player_and_puts_wins_first);
    RUN(a_disputed_game_is_left_out_of_every_sum);
    RUN(ties_on_wins_go_to_score_then_games);
    RUN(history_is_newest_first_and_pages);
    RUN(an_empty_ledger_answers_with_nothing);
    RUN(a_device_record_survives_its_codec_and_an_old_one_keeps_its_format);
    RUN(an_old_name_row_stays_its_own_and_no_device_takes_it_over);
    RUN(a_device_in_two_seats_of_one_game_counts_it_for_neither);
    RUN(a_player_goes_by_the_name_they_last_played_under);
    RUN(games_filter_by_player_name_map_and_date);
    RUN(a_file_holds_the_records_across_a_reopen);
    RUN(device_records_survive_a_reopen_and_a_rewrite_and_tag_4_is_inert);
    RUN(a_torn_tail_is_dropped_and_the_file_made_whole);
    RUN(a_record_in_the_middle_that_will_not_read_is_skipped_not_fatal);
    RUN(a_corrupt_length_costs_one_record_and_the_file_is_made_whole_beside_itself);
    RUN(an_impossible_length_is_a_skip_not_a_torn_tail);
    RUN(an_empty_file_is_a_new_ledger);
    RUN(a_file_that_is_not_a_ledger_is_refused_and_left_alone);
    RUN(a_mod_record_survives_its_codec_and_a_file_and_an_unfiled_one_keeps_its_format);
    RUN(a_table_id_is_the_mod_name_folded_and_the_fingerprint);
    RUN(each_mod_set_keeps_its_own_table_and_vanilla_comes_first);
    TEST_REPORT();
}
