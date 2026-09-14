/*
 * test_ledger.c -- the leaderboard's store and its sums.
 *
 * Data free. The file cases use one scratch file beside the binary and
 * remove it afterwards.
 */

#include "test_framework.h"
#include "tak_net_ledger.h"

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

/* ── Identity ─────────────────────────────────────────────────────────── */

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

int main(void) {
    TEST_SUITE("The ledger");
    RUN(a_typed_name_is_one_player_however_it_is_typed);
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
    RUN(a_file_holds_the_records_across_a_reopen);
    RUN(a_torn_tail_is_dropped_and_the_file_made_whole);
    RUN(a_record_in_the_middle_that_will_not_read_is_skipped_not_fatal);
    RUN(a_corrupt_length_costs_one_record_and_the_file_is_made_whole_beside_itself);
    RUN(an_impossible_length_is_a_skip_not_a_torn_tail);
    RUN(an_empty_file_is_a_new_ledger);
    RUN(a_file_that_is_not_a_ledger_is_refused_and_left_alone);
    TEST_REPORT();
}
