/*
 * test_turnclock.c -- turns close on the relay's clock and wait for nobody.
 *
 * Data free. Time is passed in, so every case runs in virtual milliseconds
 * and a thirty second countdown costs nothing. Everything the clock sends
 * is captured and decoded, so each assertion is about what a client would
 * actually receive.
 */

#include "test_framework.h"
#include "tak_net_turn.h"
#include "tak_bytes.h"

#include <string.h>

/* ── Capture ──────────────────────────────────────────────────────────── */

#define MAX_SENT 16384
typedef struct Sent { int sim; uint8_t type; uint32_t len; uint32_t off; } Sent;
static Sent    sent[MAX_SENT];
static uint8_t sent_bytes[8u << 20];
static size_t  sent_used;
static int     nsent;

static void sink(void *user, int sim, const uint8_t *f, size_t n) {
    (void)user;
    if (nsent >= MAX_SENT || sent_used + n > sizeof(sent_bytes)) return;
    sent[nsent].sim = sim;
    sent[nsent].type = f[0];
    sent[nsent].len = (uint32_t)n;
    sent[nsent].off = (uint32_t)sent_used;
    memcpy(sent_bytes + sent_used, f, n);
    sent_used += n;
    nsent++;
}

static TAK_TurnClock    clk;
static TAK_TurnLog      tlog;
static uint8_t          arena[1u << 20];
static TAK_TurnLogEntry entries[1u << 16];
static uint8_t          cmd_body[TAK_NET_CMD_BYTES_MAX];
static uint8_t          buf[TAK_NET_FRAME_MAX];

static void setup(uint16_t timeout, uint8_t left_as, uint8_t host_only) {
    TAK_TurnClockCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.turn_ms = TAK_NET_TURN_MS;
    cfg.speed = TAK_NET_SPEED_NORMAL;
    cfg.timeout_secs = timeout;
    cfg.left_as = left_as;
    cfg.host_paces_only = host_only;
    TAK_TurnLog_Init(&tlog, arena, sizeof(arena), entries, 1u << 16);
    TAK_TurnClock_Init(&clk, &cfg, &tlog, sink, NULL);
    nsent = 0;
    sent_used = 0;
}

static int split_at(int i, TAK_NetFrame *f) {
    return TAK_Net_Split(sent_bytes + sent[i].off, sent[i].len, f);
}

static int count_sent(uint8_t type, int sim) {
    int n = 0;
    for (int i = 0; i < nsent; i++)
        if (sent[i].type == type && sent[i].sim == sim) n++;
    return n;
}

/* The broadcast TURN for a turn number. */
static int find_turn(uint32_t turn, TAK_MsgTurn *t) {
    for (int i = 0; i < nsent; i++) {
        TAK_NetFrame f;
        if (sent[i].type != TAK_MSG_TURN || sent[i].sim != TAK_TURN_SIM_ALL) continue;
        if (split_at(i, &f) || TAK_Msg_TurnDecode(t, f.payload, f.payload_len)) continue;
        if (t->turn == turn) return 1;
    }
    return 0;
}

/* The first broadcast turn at or after `from` carrying a given system
 * command. Fills *blob with its body. */
static int find_system(uint8_t syscmd, uint32_t from, const uint8_t **blob) {
    static TAK_MsgTurn t;
    for (int i = 0; i < nsent; i++) {
        TAK_NetFrame f;
        if (sent[i].type != TAK_MSG_TURN || sent[i].sim != TAK_TURN_SIM_ALL) continue;
        if (split_at(i, &f) || TAK_Msg_TurnDecode(&t, f.payload, f.payload_len)) continue;
        if (t.turn < from) continue;
        for (int e = 0; e < t.entry_count; e++) {
            if (t.entry[e].seat != TAK_NET_SEAT_SERVER) continue;
            for (int k = 0; k < t.entry[e].count; k++)
                if (t.entry[e].cmd[k].data[0] == syscmd) {
                    *blob = t.entry[e].cmd[k].data;
                    return 1;
                }
        }
    }
    return 0;
}

static int last_pace(TAK_MsgPace *p) {
    for (int i = nsent - 1; i >= 0; i--) {
        TAK_NetFrame f;
        if (sent[i].type != TAK_MSG_PACE || split_at(i, &f)) continue;
        return TAK_Msg_PaceDecode(p, f.payload, f.payload_len) == 0;
    }
    return 0;
}

static int last_status(uint8_t seat, TAK_MsgPlayerStatus *st) {
    for (int i = nsent - 1; i >= 0; i--) {
        TAK_NetFrame f;
        if (sent[i].type != TAK_MSG_PLAYER_STATUS || split_at(i, &f)) continue;
        if (TAK_Msg_PlayerStatusDecode(st, f.payload, f.payload_len)) continue;
        if (st->seat == seat) return 1;
    }
    return 0;
}

static int send_cmd(int sim, uint32_t seq, uint8_t fill, uint16_t len) {
    TAK_MsgCmd m;
    memset(&m, 0, sizeof(m));
    memset(cmd_body, fill, len);
    m.client_seq = seq;
    m.count = 1;
    m.cmd[0].data = cmd_body;
    m.cmd[0].len = len;
    return TAK_TurnClock_Command(&clk, sim, &m);
}

static int ack(int sim, uint32_t last_turn, uint32_t tick, uint64_t hash,
               uint64_t now) {
    TAK_MsgAck a;
    a.last_turn = last_turn;
    a.hash_tick = tick;
    a.state_hash = hash;
    return TAK_TurnClock_Ack(&clk, sim, &a, now);
}

/* Run the clock from `from` to `to` in 10 ms steps. Every sim in `keep`
 * acknowledges the newest turn and is heard, so only the ones left out
 * fall behind or go quiet. */
static void run(uint64_t from, uint64_t to, uint32_t keep) {
    for (uint64_t t = from; t <= to; t += 10) {
        for (int s = 0; s < TAK_TURN_SIMS_MAX; s++) {
            if (!(keep & (1u << s)) || !clk.sim[s].in_use) continue;
            if (clk.head > 0) ack(s, clk.head - 1, TAK_NET_NO_HASH, 0, t);
            else TAK_TurnClock_Heard(&clk, s, t);
        }
        TAK_TurnClock_Advance(&clk, t);
    }
}

/* ── The log ──────────────────────────────────────────────────────────── */

TEST(the_log_coalesces_empty_turns_and_replays_them_as_a_range) {
    TAK_TurnLog_Init(&tlog, arena, sizeof(arena), entries, 1u << 16);
    for (uint32_t t = 0; t < 100; t++) ASSERT_EQ_INT(0, TAK_TurnLog_Append(&tlog, t, NULL, 0));

    static uint8_t body[5] = { 1, 2, 3, 4, 5 };
    TAK_MsgTurn one;
    memset(&one, 0, sizeof(one));
    one.turn = 100; one.empty_run = 1; one.entry_count = 1;
    one.entry[0].seat = 2; one.entry[0].count = 1;
    one.entry[0].cmd[0].data = body; one.entry[0].cmd[0].len = 5;
    size_t n = TAK_Msg_TurnEncode(&one, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_TurnLog_Append(&tlog, 100, buf, n));
    for (uint32_t t = 101; t <= 150; t++) ASSERT_EQ_INT(0, TAK_TurnLog_Append(&tlog, t, NULL, 0));
    ASSERT_EQ_INT(3, (int)tlog.entry_count);

    static uint8_t out[TAK_NET_FRAME_MAX];
    uint32_t covered = 0;
    TAK_NetFrame f;
    TAK_MsgTurn t;
    size_t m = TAK_TurnLog_Frame(&tlog, 0, out, sizeof(out), &covered);
    ASSERT(m > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(out, m, &f));
    ASSERT_EQ_INT(0, TAK_Msg_TurnDecode(&t, f.payload, f.payload_len));
    ASSERT_EQ_INT(0, (int)t.turn);
    ASSERT_EQ_INT(100, (int)t.empty_run);
    ASSERT_EQ_INT(100, (int)covered);

    m = TAK_TurnLog_Frame(&tlog, 40, out, sizeof(out), &covered);
    ASSERT_EQ_INT(0, TAK_Net_Split(out, m, &f));
    ASSERT_EQ_INT(0, TAK_Msg_TurnDecode(&t, f.payload, f.payload_len));
    ASSERT_EQ_INT(40, (int)t.turn);
    ASSERT_EQ_INT(60, (int)covered);

    m = TAK_TurnLog_Frame(&tlog, 100, out, sizeof(out), &covered);
    ASSERT_EQ_INT((int)n, (int)m);
    ASSERT(memcmp(out, buf, n) == 0);
    ASSERT_EQ_INT(1, (int)covered);

    m = TAK_TurnLog_Frame(&tlog, 120, out, sizeof(out), &covered);
    ASSERT(m > 0);
    ASSERT_EQ_INT(31, (int)covered);
    ASSERT_EQ_INT(0, (int)TAK_TurnLog_Frame(&tlog, 151, out, sizeof(out), &covered));
}

TEST(the_log_refuses_out_of_order_turns_and_stays_full) {
    static uint8_t tiny[16];
    static TAK_TurnLogEntry few[4];
    TAK_TurnLog_Init(&tlog, tiny, sizeof(tiny), few, 4);
    ASSERT_EQ_INT(-1, TAK_TurnLog_Append(&tlog, 1, NULL, 0));
    memset(buf, 7, 20);
    ASSERT_EQ_INT(-1, TAK_TurnLog_Append(&tlog, 0, buf, 20));
    ASSERT_EQ_INT(1, tlog.full);
    ASSERT_EQ_INT(-1, TAK_TurnLog_Append(&tlog, 0, NULL, 0));
}

/* ── The cadence ──────────────────────────────────────────────────────── */

TEST(turns_close_on_the_clock_and_wait_for_nobody) {
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    ASSERT(TAK_TurnClock_AddSim(&clk, 101, 0, 0) >= 0);
    ASSERT(TAK_TurnClock_AddSim(&clk, 102, 1, 0) >= 0);
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(1, count_sent(TAK_MSG_GO, TAK_TURN_SIM_ALL));
    /* Nobody acknowledges anything, and the turns come anyway. */
    for (uint64_t t = 0; t <= 1000; t += 10) TAK_TurnClock_Advance(&clk, t);
    ASSERT_EQ_INT(20, (int)clk.head);
    ASSERT_EQ_INT(20, count_sent(TAK_MSG_TURN, TAK_TURN_SIM_ALL));
    ASSERT_EQ_INT(20, (int)tlog.next_turn);
}

TEST(a_late_command_lands_in_the_next_open_turn) {
    TAK_MsgTurn t;
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    TAK_TurnClock_Start(&clk, 0);
    TAK_TurnClock_Advance(&clk, 49);
    ASSERT_EQ_INT(0, send_cmd(a, 1, 0xa1, 4));
    TAK_TurnClock_Advance(&clk, 50);
    TAK_TurnClock_Advance(&clk, 100);
    /* Turn 1 has already gone, so this one waits for turn 2. */
    ASSERT_EQ_INT(0, send_cmd(b, 1, 0xb1, 4));
    TAK_TurnClock_Advance(&clk, 150);

    ASSERT(find_turn(0, &t));
    ASSERT_EQ_INT(1, t.entry_count);
    ASSERT_EQ_INT(0, t.entry[0].seat);
    ASSERT_EQ_INT(0xa1, t.entry[0].cmd[0].data[0]);
    ASSERT(find_turn(1, &t));
    ASSERT_EQ_INT(0, t.entry_count);
    ASSERT(find_turn(2, &t));
    ASSERT_EQ_INT(1, t.entry_count);
    ASSERT_EQ_INT(1, t.entry[0].seat);
}

TEST(each_command_carries_its_senders_seat_in_rising_order) {
    TAK_MsgTurn t;
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 3, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 6, 0);
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(0, send_cmd(b, 1, 0xbb, 3));   /* seat 6 arrives first */
    ASSERT_EQ_INT(0, send_cmd(a, 1, 0xaa, 2));
    TAK_TurnClock_Advance(&clk, 50);
    ASSERT(find_turn(0, &t));
    ASSERT_EQ_INT(2, t.entry_count);
    ASSERT_EQ_INT(3, t.entry[0].seat);
    ASSERT_EQ_INT(0xaa, t.entry[0].cmd[0].data[0]);
    ASSERT_EQ_INT(6, t.entry[1].seat);
    ASSERT_EQ_INT(0xbb, t.entry[1].cmd[0].data[0]);
}

TEST(a_duplicated_command_frame_changes_nothing) {
    TAK_MsgTurn t;
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(0, send_cmd(a, 5, 1, 4));
    ASSERT_EQ_INT(0, send_cmd(a, 5, 1, 4));      /* the same frame again */
    ASSERT_EQ_INT(0, send_cmd(a, 4, 1, 4));      /* an older one, late */
    TAK_TurnClock_Advance(&clk, 50);
    ASSERT(find_turn(0, &t));
    ASSERT_EQ_INT(1, t.entry_count);
    ASSERT_EQ_INT(1, t.entry[0].count);
}

TEST(a_seat_over_its_budget_and_a_watcher_are_refused) {
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int w = TAK_TurnClock_AddSim(&clk, 900, TAK_NET_SEAT_NONE, 0);
    ASSERT_EQ_INT(TAK_REJECT_GAME_CLOSED, send_cmd(a, 1, 1, 4));   /* before GO */
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(0, send_cmd(a, 1, 1, TAK_NET_CMD_BYTES_MAX));
    ASSERT_EQ_INT(TAK_REJECT_RATE_LIMITED, send_cmd(a, 2, 1, 1));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, send_cmd(w, 1, 1, 4));
    /* The next turn has a fresh budget. */
    TAK_TurnClock_Advance(&clk, 50);
    ASSERT_EQ_INT(0, send_cmd(a, 3, 1, 1));
}

TEST(an_ack_for_a_turn_not_yet_closed_is_refused) {
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    TAK_TurnClock_Start(&clk, 0);
    TAK_TurnClock_Advance(&clk, 50);
    ASSERT_EQ_INT(1, (int)clk.head);
    ASSERT_EQ_INT(-1, ack(a, 1, TAK_NET_NO_HASH, 0, 50));
    ASSERT_EQ_INT(0, ack(a, 0, TAK_NET_NO_HASH, 0, 50));
    /* A hash off the 60 tick grid, or for ticks not yet simulated. */
    ASSERT_EQ_INT(-1, ack(a, 0, 30, 1, 50));
    ASSERT_EQ_INT(-1, ack(a, 0, 60, 1, 50));
}

/* ── Hash comparison ──────────────────────────────────────────────────── */

TEST(an_outlier_is_named_at_its_tick_and_sent_back_to_turn_zero) {
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    int c = TAK_TurnClock_AddSim(&clk, 103, 2, 0);
    TAK_TurnClock_Start(&clk, 0);
    for (uint64_t t = 0; t <= 1000; t += 10) TAK_TurnClock_Advance(&clk, t);
    ASSERT_EQ_INT(20, (int)clk.head);   /* 60 ticks */

    ASSERT_EQ_INT(0, ack(a, 19, 60, 111, 1000));
    ASSERT_EQ_INT(0, ack(b, 19, 60, 111, 1000));
    ASSERT_EQ_INT(0, clk.desync.count);         /* still waiting for c */
    int before = nsent;
    ASSERT_EQ_INT(0, ack(c, 19, 60, 999, 1000));

    ASSERT_EQ_INT(1, (int)clk.desync.count);
    ASSERT_EQ_INT(60, (int)clk.desync.tick);
    ASSERT_EQ_INT(1 << c, (int)clk.desync.outliers);
    ASSERT_EQ_INT(0, clk.desync.halted);
    ASSERT_EQ_INT(TAK_PSTATUS_CATCHING_UP, clk.sim[c].status);

    /* c alone is told to start again from turn zero, then gets the log,
     * twenty empty turns in one frame. */
    int go_at = -1;
    for (int i = before; i < nsent; i++)
        if (sent[i].sim == c && sent[i].type == TAK_MSG_GO) { go_at = i; break; }
    ASSERT(go_at >= 0);
    TAK_NetFrame f;
    TAK_MsgGo go;
    ASSERT_EQ_INT(0, split_at(go_at, &f));
    ASSERT_EQ_INT(0, TAK_Msg_GoDecode(&go, f.payload, f.payload_len));
    ASSERT_EQ_INT(0, (int)go.first_turn);
    ASSERT(go_at + 1 < nsent);
    ASSERT_EQ_INT(c, sent[go_at + 1].sim);
    TAK_MsgTurn t;
    ASSERT_EQ_INT(0, split_at(go_at + 1, &f));
    ASSERT_EQ_INT(0, TAK_Msg_TurnDecode(&t, f.payload, f.payload_len));
    ASSERT_EQ_INT(0, (int)t.turn);
    ASSERT_EQ_INT(20, (int)t.empty_run);

    /* Replayed and now agreeing, c is back in the game. */
    ASSERT_EQ_INT(0, ack(c, 19, 60, 111, 1010));
    ASSERT_EQ_INT(1, (int)clk.desync.count);
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, clk.sim[c].status);
}

TEST(two_worlds_that_disagree_halt_the_match) {
    TAK_MsgPace p;
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    TAK_TurnClock_Start(&clk, 0);
    for (uint64_t t = 0; t <= 1000; t += 10) TAK_TurnClock_Advance(&clk, t);
    ASSERT_EQ_INT(0, ack(a, 19, 60, 1, 1000));
    ASSERT_EQ_INT(0, ack(b, 19, 60, 2, 1000));
    ASSERT_EQ_INT(1, clk.desync.halted);
    ASSERT_EQ_INT((1 << a) | (1 << b), (int)clk.desync.outliers);
    ASSERT(last_pace(&p));
    ASSERT_EQ_INT(TAK_PACE_DESYNC_HALT, p.reason);
    ASSERT_EQ_INT(1, p.paused);
    uint32_t head = clk.head;
    run(1010, 3000, (1u << a) | (1u << b));
    ASSERT_EQ_INT((int)head, (int)clk.head);   /* no turn after a halt */
}

TEST(a_catching_up_player_is_checked_against_the_agreed_hash) {
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    int c = TAK_TurnClock_AddSim(&clk, 103, TAK_NET_SEAT_NONE, 0);
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(0, TAK_TurnClock_Reconnect(&clk, c, 0, 0));
    for (uint64_t t = 0; t <= 1000; t += 10) TAK_TurnClock_Advance(&clk, t);
    /* c is replaying, so the two live worlds settle tick 60 alone. */
    ASSERT_EQ_INT(0, ack(a, 19, 60, 5, 1000));
    ASSERT_EQ_INT(0, ack(b, 19, 60, 5, 1000));
    ASSERT_EQ_INT(0, clk.desync.count);
    /* c's own hash for that tick arrives later and is compared then. */
    ASSERT_EQ_INT(0, ack(c, 19, 60, 6, 1000));
    ASSERT_EQ_INT(1, (int)clk.desync.count);
    ASSERT_EQ_INT(1 << c, (int)clk.desync.outliers);
    ASSERT_EQ_INT(0, ack(c, 19, 60, 5, 1010));
    ASSERT_EQ_INT(1, (int)clk.desync.count);
}

/* ── The governor ─────────────────────────────────────────────────────── */

TEST(the_governor_slows_turns_for_a_player_who_falls_behind) {
    TAK_MsgPace p;
    TAK_MsgPlayerStatus st;
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    TAK_TurnClock_Start(&clk, 0);
    uint64_t first_slow = 0;
    for (uint64_t t = 0; t <= 6000; t += 10) {
        if (clk.head > 0) ack(a, clk.head - 1, TAK_NET_NO_HASH, 0, t);
        if (t % 1000 == 0) TAK_TurnClock_Heard(&clk, b, t);   /* pings, no acks */
        TAK_TurnClock_Advance(&clk, t);
        if (!first_slow && TAK_TurnClock_Period(&clk) > TAK_NET_TURN_MS) first_slow = t;
    }
    /* More than half a second behind from 550 ms, held for two seconds. */
    ASSERT(first_slow >= 2550);
    ASSERT(first_slow <= 2600);
    ASSERT(last_pace(&p));
    ASSERT_EQ_INT(TAK_PACE_WAITING_FOR_PLAYER, p.reason);
    ASSERT_EQ_INT(1, p.seat);
    ASSERT_EQ_INT(0, p.paused);              /* slower, never stopped */
    ASSERT(p.turn_period_ms > TAK_NET_TURN_MS);
    ASSERT(last_status(1, &st));
    ASSERT_EQ_INT(TAK_PSTATUS_LAGGING, st.status);
    /* Continuous: far fewer turns than a full rate would have closed. */
    ASSERT((int)clk.head < 6000 / TAK_NET_TURN_MS - 20);

    /* b catches up and the governor lets go. */
    ack(b, clk.head - 1, TAK_NET_NO_HASH, 0, 6010);
    run(6010, 6100, (1u << a) | (1u << b));
    ASSERT_EQ_INT(TAK_NET_TURN_MS, TAK_TurnClock_Period(&clk));
    ASSERT(last_pace(&p));
    ASSERT_EQ_INT(TAK_PACE_NORMAL, p.reason);
    ASSERT(last_status(1, &st));
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, st.status);
}

TEST(a_lagging_watcher_never_slows_the_players) {
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int w = TAK_TurnClock_AddSim(&clk, 900, TAK_NET_SEAT_NONE, 0);
    TAK_TurnClock_Start(&clk, 0);
    for (uint64_t t = 0; t <= 6000; t += 10) {
        if (clk.head > 0) ack(a, clk.head - 1, TAK_NET_NO_HASH, 0, t);
        if (t % 1000 == 0) TAK_TurnClock_Heard(&clk, w, t);
        TAK_TurnClock_Advance(&clk, t);
        ASSERT_EQ_INT(TAK_NET_TURN_MS, TAK_TurnClock_Period(&clk));
    }
    ASSERT_EQ_INT(120, (int)clk.head);
}

/* ── Silence, the countdown and the drop ─────────────────────────────── */

TEST(silence_pauses_the_match_counts_down_and_the_computer_takes_over) {
    TAK_MsgPace p;
    TAK_MsgPlayerStatus st;
    const uint8_t *blob = NULL;
    setup(30, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    (void)b;
    TAK_TurnClock_Start(&clk, 0);
    run(0, 4990, 1u << a);
    /* Behind for over two seconds it counts as lagging, but not lost. */
    ASSERT_EQ_INT(TAK_PSTATUS_LAGGING, clk.sim[b].status);
    run(5000, 5000, 1u << a);
    ASSERT_EQ_INT(TAK_PSTATUS_LOST, clk.sim[b].status);
    ASSERT(last_status(1, &st));
    ASSERT_EQ_INT(TAK_PSTATUS_LOST, st.status);
    ASSERT_EQ_INT(30, st.countdown_secs);
    ASSERT(last_pace(&p));
    ASSERT_EQ_INT(1, p.paused);
    ASSERT_EQ_INT(TAK_PACE_WAITING_FOR_PLAYER, p.reason);
    ASSERT_EQ_INT(1, p.seat);

    uint32_t frozen = clk.head;
    run(5010, 20000, 1u << a);
    ASSERT_EQ_INT((int)frozen, (int)clk.head);     /* nobody plays on */
    ASSERT(last_status(1, &st));
    ASSERT_EQ_INT(15, st.countdown_secs);          /* the server counts */

    run(20010, 35000, 1u << a);
    ASSERT_EQ_INT(TAK_PSTATUS_DROPPED, clk.sim[b].status);
    run(35010, 35200, 1u << a);
    ASSERT(clk.head > frozen);                     /* and the match resumes */
    ASSERT(find_system(TAK_SYS_PLAYER_LEFT, frozen, &blob));
    ASSERT_EQ_INT(1, blob[1]);
    ASSERT_EQ_INT(TAK_LEFT_COMPUTER_TAKES_OVER, blob[2]);
}

TEST(a_player_back_inside_the_window_resumes_the_match_at_once) {
    TAK_MsgPace p;
    setup(30, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    TAK_TurnClock_Start(&clk, 0);
    run(0, 5000, 1u << a);
    ASSERT_EQ_INT(TAK_PSTATUS_LOST, clk.sim[b].status);
    uint32_t frozen = clk.head;
    TAK_TurnClock_Heard(&clk, b, 8000);
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, clk.sim[b].status);
    ASSERT(last_pace(&p));
    ASSERT_EQ_INT(0, p.paused);
    run(8000, 8500, (1u << a) | (1u << b));
    ASSERT(clk.head > frozen);
    const uint8_t *blob = NULL;
    ASSERT(!find_system(TAK_SYS_PLAYER_LEFT, 0, &blob));
}

TEST(the_host_may_reject_a_lost_player_early) {
    const uint8_t *blob = NULL;
    setup(150, TAK_LEFT_ARMY_REMOVED, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, TAK_TurnClock_Reject(&clk, 1, 100));
    run(0, 5000, 1u << a);
    uint32_t frozen = clk.head;
    ASSERT_EQ_INT(0, TAK_TurnClock_Reject(&clk, 1, 6000));
    ASSERT_EQ_INT(TAK_PSTATUS_DROPPED, clk.sim[b].status);
    run(6000, 6200, 1u << a);
    ASSERT(find_system(TAK_SYS_PLAYER_LEFT, frozen, &blob));
    ASSERT_EQ_INT(TAK_LEFT_ARMY_REMOVED, blob[2]);
}

TEST(a_dropped_player_replays_the_log_and_reclaims_the_seat) {
    const uint8_t *blob = NULL;
    setup(30, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(0, send_cmd(a, 1, 0x42, 6));     /* something worth replaying */
    run(0, 35100, 1u << a);
    ASSERT_EQ_INT(TAK_PSTATUS_DROPPED, clk.sim[b].status);

    int before = nsent;
    uint32_t head_at_return = clk.head;
    ASSERT_EQ_INT(0, TAK_TurnClock_Reconnect(&clk, b, 0, 36000));
    ASSERT_EQ_INT(TAK_PSTATUS_CATCHING_UP, clk.sim[b].status);
    /* GO for turn zero, then the whole log, to b alone. */
    uint32_t replayed = 0;
    int saw_go = 0;
    for (int i = before; i < nsent; i++) {
        TAK_NetFrame f;
        TAK_MsgTurn t;
        if (sent[i].sim != b) continue;
        if (sent[i].type == TAK_MSG_GO) { saw_go = 1; continue; }
        if (sent[i].type != TAK_MSG_TURN) continue;
        ASSERT_EQ_INT(0, split_at(i, &f));
        ASSERT_EQ_INT(0, TAK_Msg_TurnDecode(&t, f.payload, f.payload_len));
        ASSERT_EQ_INT((int)replayed, (int)t.turn);
        replayed += t.entry_count ? 1u : t.empty_run;
    }
    ASSERT(saw_go);
    ASSERT_EQ_INT((int)head_at_return, (int)replayed);
    /* No orders against a world still being replayed. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, send_cmd(b, 1, 1, 4));

    /* Everyone else keeps playing while b catches up. */
    run(36000, 36500, 1u << a);
    ASSERT(clk.head > head_at_return);

    uint32_t caught = clk.head;
    ASSERT_EQ_INT(0, ack(b, clk.head - 1, TAK_NET_NO_HASH, 0, 36510));
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, clk.sim[b].status);
    run(36510, 36700, (1u << a) | (1u << b));
    ASSERT(find_system(TAK_SYS_SEAT_RECLAIM, caught, &blob));
    ASSERT_EQ_INT(1, blob[1]);
    ASSERT_EQ_INT(102, (int)tak_get_u32(blob + 2));
    ASSERT_EQ_INT(0, send_cmd(b, 2, 1, 4));        /* the seat is theirs again */
}

/* ── Pacing ───────────────────────────────────────────────────────────── */

TEST(pause_and_speed_change_the_pace_and_nothing_in_the_turns) {
    TAK_MsgPace p;
    TAK_MsgTurn t;
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 1);   /* host only, a public room */
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    uint32_t both = (1u << a) | (1u << b);
    TAK_TurnClock_Start(&clk, 0);
    run(0, 500, both);

    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, TAK_TurnClock_SetPaused(&clk, b, 1, 0, 500));
    ASSERT_EQ_INT(0, TAK_TurnClock_SetPaused(&clk, a, 1, 0, 500));
    ASSERT(last_pace(&p));
    ASSERT_EQ_INT(TAK_PACE_PAUSED, p.reason);
    ASSERT_EQ_INT(1, p.paused);
    uint32_t held = clk.head;
    run(510, 1500, both);
    ASSERT_EQ_INT((int)held, (int)clk.head);
    ASSERT_EQ_INT(0, TAK_TurnClock_SetPaused(&clk, a, 0, 0, 1500));
    run(1510, 2000, both);
    ASSERT(clk.head > held);

    ASSERT_EQ_INT(0, TAK_TurnClock_SetSpeed(&clk, a, TAK_NET_SPEED_MAX, 0, 2000));
    ASSERT_EQ_INT(25, TAK_TurnClock_Period(&clk));
    ASSERT_EQ_INT(0, TAK_TurnClock_SetSpeed(&clk, a, TAK_NET_SPEED_MIN, 0, 2000));
    ASSERT_EQ_INT(250, TAK_TurnClock_Period(&clk));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  TAK_TurnClock_SetSpeed(&clk, a, TAK_NET_SPEED_MAX + 1, 0, 2000));
    ASSERT(last_pace(&p));
    ASSERT_EQ_INT(TAK_NET_SPEED_MIN, p.speed_level);
    run(2010, 3000, both);

    /* N-004: none of it reached the turn stream, so none of it can reach
     * a simulation or its hash. */
    for (uint32_t turn = 0; turn < clk.head; turn++) {
        ASSERT(find_turn(turn, &t));
        ASSERT_EQ_INT(0, t.entry_count);
    }
}

TEST(a_server_stall_does_not_burst_turns_at_the_clients) {
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    TAK_TurnClock_Start(&clk, 0);
    TAK_TurnClock_Heard(&clk, a, 4000);
    ASSERT(TAK_TurnClock_Advance(&clk, 4000) <= TAK_TURN_BURST_MAX);
    ASSERT_EQ_INT(0, TAK_TurnClock_Advance(&clk, 4010));
    ASSERT_EQ_INT(1, TAK_TurnClock_Advance(&clk, 4050));
}

TEST(a_resigning_player_stays_on_as_a_watcher) {
    const uint8_t *blob = NULL;
    setup(60, TAK_LEFT_COMPUTER_TAKES_OVER, 0);
    int a = TAK_TurnClock_AddSim(&clk, 101, 0, 0);
    int b = TAK_TurnClock_AddSim(&clk, 102, 1, 0);
    TAK_TurnClock_Start(&clk, 0);
    ASSERT_EQ_INT(0, TAK_TurnClock_Resign(&clk, b));
    TAK_TurnClock_Advance(&clk, 50);
    ASSERT(find_system(TAK_SYS_PLAYER_LEFT, 0, &blob));
    ASSERT_EQ_INT(1, blob[1]);
    ASSERT_EQ_INT(TAK_LEFT_RESIGNED, blob[2]);
    ASSERT_EQ_INT(TAK_NET_SEAT_NONE, clk.sim[b].seat);
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, send_cmd(b, 1, 1, 4));
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, clk.sim[b].status);
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, TAK_TurnClock_Resign(&clk, b));
    (void)a;
}

int main(void) {
    TEST_SUITE("Relay turn clock");
    RUN(the_log_coalesces_empty_turns_and_replays_them_as_a_range);
    RUN(the_log_refuses_out_of_order_turns_and_stays_full);
    RUN(turns_close_on_the_clock_and_wait_for_nobody);
    RUN(a_late_command_lands_in_the_next_open_turn);
    RUN(each_command_carries_its_senders_seat_in_rising_order);
    RUN(a_duplicated_command_frame_changes_nothing);
    RUN(a_seat_over_its_budget_and_a_watcher_are_refused);
    RUN(an_ack_for_a_turn_not_yet_closed_is_refused);
    RUN(an_outlier_is_named_at_its_tick_and_sent_back_to_turn_zero);
    RUN(two_worlds_that_disagree_halt_the_match);
    RUN(a_catching_up_player_is_checked_against_the_agreed_hash);
    RUN(the_governor_slows_turns_for_a_player_who_falls_behind);
    RUN(a_lagging_watcher_never_slows_the_players);
    RUN(silence_pauses_the_match_counts_down_and_the_computer_takes_over);
    RUN(a_player_back_inside_the_window_resumes_the_match_at_once);
    RUN(the_host_may_reject_a_lost_player_early);
    RUN(a_dropped_player_replays_the_log_and_reclaims_the_seat);
    RUN(pause_and_speed_change_the_pace_and_nothing_in_the_turns);
    RUN(a_server_stall_does_not_burst_turns_at_the_clients);
    RUN(a_resigning_player_stays_on_as_a_watcher);
    TEST_REPORT();
}
