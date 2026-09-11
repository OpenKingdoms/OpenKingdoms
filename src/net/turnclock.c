/*
 * turnclock.c -- turns closed on the relay's own cadence, and the log that
 * replays them.
 *
 * Nothing here waits for a client. A turn closes when its time comes and
 * goes out to everyone. What a slow client costs is decided by the
 * governor, what a silent one costs by the reject countdown, and what a
 * wrong one costs by the hash comparison. The shape is in tak_net_turn.h.
 */

#include "tak_net_turn.h"
#include "tak_bytes.h"

#include <string.h>

/* ── The turn log ─────────────────────────────────────────────────────── */

void TAK_TurnLog_Init(TAK_TurnLog *log, void *arena, size_t arena_cap,
                      TAK_TurnLogEntry *entries, uint32_t entry_cap) {
    memset(log, 0, sizeof(*log));
    log->arena = (uint8_t *)arena;
    log->arena_cap = arena ? arena_cap : 0;
    log->entry = entries;
    log->entry_cap = entries ? entry_cap : 0;
}

int TAK_TurnLog_Append(TAK_TurnLog *log, uint32_t turn,
                       const uint8_t *frame, size_t len) {
    if (log->full || turn != log->next_turn) return -1;
    if (len == 0) {
        if (log->entry_count > 0) {
            TAK_TurnLogEntry *last = &log->entry[log->entry_count - 1];
            if (last->length == 0 && last->turn + last->run == turn &&
                last->run < 0xffffu) {
                last->run++;
                log->next_turn++;
                return 0;
            }
        }
        if (log->entry_count >= log->entry_cap) { log->full = 1; return -1; }
        TAK_TurnLogEntry *e = &log->entry[log->entry_count++];
        e->turn = turn; e->run = 1; e->offset = 0; e->length = 0;
        log->next_turn++;
        return 0;
    }
    if (!frame || log->entry_count >= log->entry_cap ||
        len > log->arena_cap - log->arena_used) {
        log->full = 1;
        return -1;
    }
    memcpy(log->arena + log->arena_used, frame, len);
    TAK_TurnLogEntry *e = &log->entry[log->entry_count++];
    e->turn = turn;
    e->run = 1;
    e->offset = (uint32_t)log->arena_used;
    e->length = (uint32_t)len;
    log->arena_used += len;
    log->next_turn++;
    return 0;
}

static const TAK_TurnLogEntry *log_find(const TAK_TurnLog *log, uint32_t turn) {
    uint32_t lo = 0, hi = log->entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const TAK_TurnLogEntry *e = &log->entry[mid];
        if (turn < e->turn) hi = mid;
        else if (turn >= e->turn + e->run) lo = mid + 1;
        else return e;
    }
    return NULL;
}

size_t TAK_TurnLog_Frame(const TAK_TurnLog *log, uint32_t from_turn,
                         void *out, size_t cap, uint32_t *covered) {
    if (covered) *covered = 0;
    const TAK_TurnLogEntry *e = log_find(log, from_turn);
    if (!e) return 0;
    if (e->length) {
        if (cap < e->length) return 0;
        memcpy(out, log->arena + e->offset, e->length);
        if (covered) *covered = 1;
        return e->length;
    }
    uint32_t left = e->turn + e->run - from_turn;
    TAK_MsgTurn t;
    memset(&t, 0, sizeof(t));
    t.turn = from_turn;
    t.empty_run = (uint16_t)(left > 0xffffu ? 0xffffu : left);
    size_t n = TAK_Msg_TurnEncode(&t, out, cap);
    if (n && covered) *covered = t.empty_run;
    return n;
}

/* ── Small helpers ────────────────────────────────────────────────────── */

static int valid_sim(const TAK_TurnClock *c, int sim) {
    return sim >= 0 && sim < TAK_TURN_SIMS_MAX && c->sim[sim].in_use;
}

static void send_to(TAK_TurnClock *c, int sim, const uint8_t *f, size_t n) {
    if (n && c->send) c->send(c->user, sim, f, n);
}

static uint16_t base_period(const TAK_TurnClock *c) {
    uint32_t speed = c->cfg.speed ? c->cfg.speed : TAK_NET_SPEED_NORMAL;
    uint32_t p = (uint32_t)c->cfg.turn_ms * TAK_NET_SPEED_NORMAL / speed;
    if (p < 1) p = 1;
    if (p > 0xffffu) p = 0xffffu;
    return (uint16_t)p;
}

uint16_t TAK_TurnClock_Period(const TAK_TurnClock *c) {
    uint32_t p = (uint32_t)base_period(c) * c->governor_q4 / 4u;
    if (p < 1) p = 1;
    if (p > 0xffffu) p = 0xffffu;
    return (uint16_t)p;
}

/* Simulations whose hash must be in before a tick is judged: everyone
 * simulating live. A lost or catching up player is checked against the
 * agreed hash later instead. */
static uint32_t live_mask(const TAK_TurnClock *c) {
    uint32_t m = 0;
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        const TAK_TurnSim *s = &c->sim[i];
        if (!s->in_use) continue;
        if (s->status == TAK_PSTATUS_CONNECTED || s->status == TAK_PSTATUS_LAGGING)
            m |= 1u << i;
    }
    return m;
}

static int lost_seat(const TAK_TurnClock *c) {
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        const TAK_TurnSim *s = &c->sim[i];
        if (s->in_use && s->seat != TAK_NET_SEAT_NONE &&
            s->status == TAK_PSTATUS_LOST) return s->seat;
    }
    return -1;
}

static int producing(const TAK_TurnClock *c) {
    return c->started && !c->paused && !c->halted && !c->ended &&
           lost_seat(c) < 0;
}

static void emit_status(TAK_TurnClock *c, int sim, uint16_t countdown) {
    TAK_TurnSim *s = &c->sim[sim];
    if (s->seat == TAK_NET_SEAT_NONE) return;   /* a watcher has no row */
    TAK_MsgPlayerStatus m;
    m.seat = s->seat;
    m.status = s->status;
    m.countdown_secs = countdown;
    send_to(c, TAK_TURN_SIM_ALL, c->frame,
            TAK_Msg_PlayerStatusEncode(&m, c->frame, sizeof(c->frame)));
}

/* Tell everyone how turns are being produced, when that changed. */
static void update_pace(TAK_TurnClock *c) {
    uint8_t reason = TAK_PACE_NORMAL, seat = TAK_NET_SEAT_NONE, paused = 0;
    int lost = lost_seat(c);
    if (c->halted) {
        reason = TAK_PACE_DESYNC_HALT; paused = 1;
    } else if (lost >= 0) {
        reason = TAK_PACE_WAITING_FOR_PLAYER; seat = (uint8_t)lost; paused = 1;
    } else if (c->paused) {
        reason = TAK_PACE_PAUSED; paused = 1;
    } else if (c->governor_q4 > 4) {
        reason = TAK_PACE_WAITING_FOR_PLAYER; seat = c->governor_seat;
    }
    uint16_t period = TAK_TurnClock_Period(c);
    if (reason == c->pace_reason && seat == c->pace_seat &&
        paused == c->pace_paused && period == c->pace_period &&
        c->cfg.speed == c->pace_speed) return;
    c->pace_reason = reason;
    c->pace_seat = seat;
    c->pace_paused = paused;
    c->pace_period = period;
    c->pace_speed = c->cfg.speed;
    TAK_MsgPace m;
    m.speed_level = c->cfg.speed;
    m.paused = paused;
    m.reason = reason;
    m.seat = seat;
    m.turn_period_ms = period;
    send_to(c, TAK_TURN_SIM_ALL, c->frame,
            TAK_Msg_PaceEncode(&m, c->frame, sizeof(c->frame)));
}

static int queue(TAK_TurnPending *p, const uint8_t *blob, uint16_t len) {
    if (len == 0 || p->count >= TAK_NET_CMDS_PER_MSG ||
        (size_t)p->used + len > TAK_NET_CMD_BYTES_MAX) return -1;
    memcpy(p->bytes + p->used, blob, len);
    p->len[p->count++] = len;
    p->used = (uint16_t)(p->used + len);
    return 0;
}

static void fill_entry(TAK_TurnEntry *e, uint8_t seat, const TAK_TurnPending *p) {
    uint16_t off = 0;
    e->seat = seat;
    e->count = p->count;
    for (uint8_t i = 0; i < p->count; i++) {
        e->cmd[i].data = p->bytes + off;
        e->cmd[i].len = p->len[i];
        off = (uint16_t)(off + p->len[i]);
    }
}

/* ── Closing a turn ───────────────────────────────────────────────────── */

static void close_turn(TAK_TurnClock *c) {
    TAK_MsgTurn t;
    memset(&t, 0, sizeof(t));
    t.turn = c->head;
    t.empty_run = 1;
    /* Seats in rising order, the server last, so the same inputs give the
     * same bytes on any relay. */
    for (int seat = 0; seat < TAK_NET_SEATS; seat++)
        if (c->pending[seat].count)
            fill_entry(&t.entry[t.entry_count++], (uint8_t)seat, &c->pending[seat]);
    if (c->system.count)
        fill_entry(&t.entry[t.entry_count++], TAK_NET_SEAT_SERVER, &c->system);

    size_t n = TAK_Msg_TurnEncode(&t, c->frame, sizeof(c->frame));
    /* A full log leaves the match running but makes rejoin impossible,
     * which TAK_TurnClock_Reconnect then reports. */
    (void)TAK_TurnLog_Append(c->log, c->head,
                             t.entry_count ? c->frame : NULL,
                             t.entry_count ? n : 0);
    send_to(c, TAK_TURN_SIM_ALL, c->frame, n);

    for (int seat = 0; seat < TAK_NET_SEATS; seat++) {
        c->pending[seat].count = 0;
        c->pending[seat].used = 0;
    }
    c->system.count = 0;
    c->system.used = 0;
    c->head++;
}

/* ── Hash comparison ──────────────────────────────────────────────────── */

static void begin_catch_up(TAK_TurnClock *c, int sim, uint32_t from_turn,
                           uint64_t now_ms);

static void record_consensus(TAK_TurnClock *c, uint32_t tick, uint64_t hash) {
    TAK_TurnConsensus *h =
        &c->history[(tick / TAK_NET_HASH_TICKS) % TAK_TURN_HASH_HISTORY];
    h->set = 1;
    h->tick = tick;
    h->hash = hash;
}

static void note_desync(TAK_TurnClock *c, uint32_t tick, uint32_t sims,
                        int halted) {
    if (c->desync.count == 0 || c->desync.tick != tick) c->desync.outliers = 0;
    c->desync.count++;
    c->desync.tick = tick;
    c->desync.outliers |= sims;
    if (halted) c->desync.halted = 1;
}

/* A simulation that disagrees with the agreed world goes back to turn zero
 * and replays the log, which is the resync. */
static void flag_outlier(TAK_TurnClock *c, int sim, uint32_t tick,
                         uint64_t now_ms) {
    note_desync(c, tick, 1u << sim, 0);
    begin_catch_up(c, sim, 0, now_ms);
}

static void judge(TAK_TurnClock *c, TAK_TurnHashRow *row, uint64_t now_ms) {
    uint32_t have = row->have;
    int n = 0, best_count = 0;
    uint64_t best = 0;
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        if (!(have & (1u << i))) continue;
        n++;
        int count = 0;
        for (int k = 0; k < TAK_TURN_SIMS_MAX; k++)
            if ((have & (1u << k)) && row->hash[k] == row->hash[i]) count++;
        if (count > best_count) { best_count = count; best = row->hash[i]; }
    }
    uint32_t tick = row->tick;
    row->used = 0;
    if (n == 0) return;
    if (best_count == n) { record_consensus(c, tick, best); return; }

    if (n >= 3 && best_count * 2 > n) {
        /* Three or more worlds and a clear majority: the majority plays on
         * and each outlier is resynced. */
        record_consensus(c, tick, best);
        for (int i = 0; i < TAK_TURN_SIMS_MAX; i++)
            if ((have & (1u << i)) && row->hash[i] != best)
                flag_outlier(c, i, tick, now_ms);
        return;
    }
    /* Two worlds, or no majority. There is no telling which is right, so
     * the match halts with a report rather than playing on diverged. */
    note_desync(c, tick, have, 1);
    c->halted = 1;
    update_pace(c);
}

static void rejudge(TAK_TurnClock *c, uint64_t now_ms) {
    uint32_t live = live_mask(c);
    for (int r = 0; r < TAK_TURN_HASH_ROWS; r++) {
        TAK_TurnHashRow *row = &c->row[r];
        if (row->used && (row->have & live) == live) judge(c, row, now_ms);
    }
}

static void report_hash(TAK_TurnClock *c, int sim, uint32_t tick,
                        uint64_t hash, uint64_t now_ms) {
    const TAK_TurnConsensus *h =
        &c->history[(tick / TAK_NET_HASH_TICKS) % TAK_TURN_HASH_HISTORY];
    if (h->set && h->tick == tick) {
        /* Already agreed: a late or catching up player is checked now. */
        if (hash != h->hash) flag_outlier(c, sim, tick, now_ms);
        return;
    }

    TAK_TurnHashRow *row = NULL, *free_row = NULL, *oldest = NULL;
    for (int r = 0; r < TAK_TURN_HASH_ROWS; r++) {
        TAK_TurnHashRow *x = &c->row[r];
        if (!x->used) { if (!free_row) free_row = x; continue; }
        if (x->tick == tick) { row = x; break; }
        if (!oldest || x->tick < oldest->tick) oldest = x;
    }
    if (!row) {
        if (!free_row) {
            /* Nobody is that far behind and still live, so judge the oldest
             * with whoever reported and check the rest against it later. */
            judge(c, oldest, now_ms);
            free_row = oldest;
        }
        row = free_row;
        memset(row, 0, sizeof(*row));
        row->used = 1;
        row->tick = tick;
    }
    row->hash[sim] = hash;
    row->have |= 1u << sim;
    uint32_t live = live_mask(c);
    if ((row->have & live) == live) judge(c, row, now_ms);
}

/* ── Setup ────────────────────────────────────────────────────────────── */

void TAK_TurnClock_Init(TAK_TurnClock *c, const TAK_TurnClockCfg *cfg,
                        TAK_TurnLog *log, TAK_TurnSend send, void *user) {
    memset(c, 0, sizeof(*c));
    if (cfg) c->cfg = *cfg;
    if (c->cfg.turn_ms == 0) c->cfg.turn_ms = TAK_NET_TURN_MS;
    if (c->cfg.speed < TAK_NET_SPEED_MIN || c->cfg.speed > TAK_NET_SPEED_MAX)
        c->cfg.speed = TAK_NET_SPEED_NORMAL;
    c->log = log;
    c->send = send;
    c->user = user;
    c->governor_q4 = 4;
    c->governor_seat = TAK_NET_SEAT_NONE;
    c->pace_seat = TAK_NET_SEAT_NONE;
}

int TAK_TurnClock_AddSim(TAK_TurnClock *c, uint32_t client_id, uint8_t seat,
                         uint64_t now_ms) {
    if (seat != TAK_NET_SEAT_NONE && seat >= TAK_NET_SEATS) return -1;
    int free_sim = -1;
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        TAK_TurnSim *s = &c->sim[i];
        if (!s->in_use) { if (free_sim < 0) free_sim = i; continue; }
        if (s->client_id == client_id) return -1;
        if (seat != TAK_NET_SEAT_NONE && s->seat == seat) return -1;
    }
    if (free_sim < 0) return -1;
    TAK_TurnSim *s = &c->sim[free_sim];
    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    s->seat = seat;
    s->client_id = client_id;
    s->status = TAK_PSTATUS_CONNECTED;
    s->last_heard_ms = now_ms;
    return free_sim;
}

int TAK_TurnClock_SimOf(const TAK_TurnClock *c, uint32_t client_id) {
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++)
        if (c->sim[i].in_use && c->sim[i].client_id == client_id) return i;
    return -1;
}

static void send_go(TAK_TurnClock *c, int sim, uint32_t first_turn) {
    TAK_MsgGo go;
    go.first_turn = first_turn;
    send_to(c, sim, c->frame, TAK_Msg_GoEncode(&go, c->frame, sizeof(c->frame)));
}

void TAK_TurnClock_Start(TAK_TurnClock *c, uint64_t now_ms) {
    c->started = 1;
    c->head = 0;
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        TAK_TurnSim *s = &c->sim[i];
        if (!s->in_use) continue;
        s->last_heard_ms = now_ms;
        s->done_turns = 0;
    }
    send_go(c, TAK_TURN_SIM_ALL, 0);
    c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
    update_pace(c);
}

/* ── Membership ───────────────────────────────────────────────────────── */

static void drop_sim(TAK_TurnClock *c, int sim, uint8_t left_as,
                     uint64_t now_ms) {
    TAK_TurnSim *s = &c->sim[sim];
    s->status = TAK_PSTATUS_DROPPED;
    s->behind = 0;
    s->reclaim = 0;
    if (s->seat != TAK_NET_SEAT_NONE) {
        uint8_t blob[8];
        size_t n = TAK_Sys_PlayerLeft(s->seat, left_as, blob, sizeof(blob));
        (void)TAK_TurnClock_System(c, blob, (uint16_t)n);
        emit_status(c, sim, 0);
    }
    rejudge(c, now_ms);
    update_pace(c);
}

static void begin_catch_up(TAK_TurnClock *c, int sim, uint32_t from_turn,
                           uint64_t now_ms) {
    TAK_TurnSim *s = &c->sim[sim];
    s->status = TAK_PSTATUS_CATCHING_UP;
    s->done_turns = from_turn;
    s->last_heard_ms = now_ms;
    s->behind = 0;
    send_go(c, sim, from_turn);
    uint32_t t = from_turn;
    while (t < c->head) {
        uint32_t covered = 0;
        size_t n = TAK_TurnLog_Frame(c->log, t, c->frame, sizeof(c->frame),
                                     &covered);
        if (!n || !covered) break;
        send_to(c, sim, c->frame, n);
        t += covered;
    }
    emit_status(c, sim, 0);
    rejudge(c, now_ms);
}

void TAK_TurnClock_Heard(TAK_TurnClock *c, int sim, uint64_t now_ms) {
    if (!valid_sim(c, sim)) return;
    TAK_TurnSim *s = &c->sim[sim];
    s->last_heard_ms = now_ms;
    if (s->status == TAK_PSTATUS_LOST) {
        /* Back inside the window: the match resumes at once. */
        s->status = TAK_PSTATUS_CONNECTED;
        s->countdown_shown = 0;
        emit_status(c, sim, 0);
        c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
        update_pace(c);
    }
}

void TAK_TurnClock_Disconnect(TAK_TurnClock *c, int sim, uint64_t now_ms) {
    if (!valid_sim(c, sim)) return;
    TAK_TurnSim *s = &c->sim[sim];
    if (s->status == TAK_PSTATUS_DROPPED) return;
    if (s->seat == TAK_NET_SEAT_NONE) {
        /* A watcher leaving costs the players nothing. */
        s->status = TAK_PSTATUS_DROPPED;
        rejudge(c, now_ms);
        return;
    }
    s->status = TAK_PSTATUS_LOST;
    s->lost_deadline_ms = now_ms + (uint64_t)c->cfg.timeout_secs * 1000u;
    s->countdown_shown = c->cfg.timeout_secs;
    emit_status(c, sim, s->countdown_shown);
    rejudge(c, now_ms);
    update_pace(c);
}

int TAK_TurnClock_Reconnect(TAK_TurnClock *c, int sim, uint32_t from_turn,
                            uint64_t now_ms) {
    if (!valid_sim(c, sim) || !c->started) return -1;
    if (from_turn > c->head) return -1;
    /* Every turn from here to the head has to still be in the log. */
    if (c->log->full || c->log->next_turn != c->head) return -1;
    TAK_TurnSim *s = &c->sim[sim];
    int was_dropped = (s->status == TAK_PSTATUS_DROPPED);
    s->reclaim = (uint8_t)(was_dropped && s->seat != TAK_NET_SEAT_NONE &&
                           c->cfg.left_as == TAK_LEFT_COMPUTER_TAKES_OVER);
    if (was_dropped && !s->reclaim) s->seat = TAK_NET_SEAT_NONE;  /* watches */
    begin_catch_up(c, sim, from_turn, now_ms);
    c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
    update_pace(c);
    return 0;
}

int TAK_TurnClock_Reject(TAK_TurnClock *c, uint8_t seat, uint64_t now_ms) {
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        TAK_TurnSim *s = &c->sim[i];
        if (s->in_use && s->seat == seat && s->status == TAK_PSTATUS_LOST) {
            drop_sim(c, i, c->cfg.left_as, now_ms);
            c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
            return 0;
        }
    }
    return TAK_REJECT_NOT_ALLOWED;
}

int TAK_TurnClock_Resign(TAK_TurnClock *c, int sim) {
    if (!valid_sim(c, sim)) return TAK_REJECT_NOT_ALLOWED;
    TAK_TurnSim *s = &c->sim[sim];
    if (s->seat == TAK_NET_SEAT_NONE || s->status == TAK_PSTATUS_DROPPED)
        return TAK_REJECT_NOT_ALLOWED;
    uint8_t blob[8];
    size_t n = TAK_Sys_PlayerLeft(s->seat, TAK_LEFT_RESIGNED, blob, sizeof(blob));
    if (TAK_TurnClock_System(c, blob, (uint16_t)n) != 0)
        return TAK_REJECT_RATE_LIMITED;
    /* A defeated player may stay and watch, so the simulation stays and
     * keeps reporting its hash. It just no longer holds a seat. */
    emit_status(c, sim, 0);
    s->seat = TAK_NET_SEAT_NONE;
    return 0;
}

/* ── Input ────────────────────────────────────────────────────────────── */

int TAK_TurnClock_Command(TAK_TurnClock *c, int sim, const TAK_MsgCmd *cmd) {
    if (!valid_sim(c, sim)) return TAK_REJECT_NOT_ALLOWED;
    if (!c->started || c->ended) return TAK_REJECT_GAME_CLOSED;
    TAK_TurnSim *s = &c->sim[sim];
    if (s->seat == TAK_NET_SEAT_NONE) return TAK_REJECT_NOT_ALLOWED;
    /* Orders issued against a world still being replayed would be issued
     * against a stale view, and a dropped seat belongs to the computer. */
    if (s->status == TAK_PSTATUS_CATCHING_UP || s->status == TAK_PSTATUS_DROPPED)
        return TAK_REJECT_NOT_ALLOWED;
    if (s->seq_seen && cmd->client_seq <= s->last_seq) return 0;

    TAK_TurnPending *p = &c->pending[s->seat];
    size_t total = 0;
    for (uint8_t i = 0; i < cmd->count; i++) total += cmd->cmd[i].len;
    if (p->count + cmd->count > TAK_NET_CMDS_PER_MSG ||
        p->used + total > TAK_NET_CMD_BYTES_MAX) return TAK_REJECT_RATE_LIMITED;
    for (uint8_t i = 0; i < cmd->count; i++)
        if (queue(p, cmd->cmd[i].data, cmd->cmd[i].len) != 0)
            return TAK_REJECT_RATE_LIMITED;
    s->last_seq = cmd->client_seq;
    s->seq_seen = 1;
    return 0;
}

int TAK_TurnClock_System(TAK_TurnClock *c, const uint8_t *blob, uint16_t len) {
    return queue(&c->system, blob, len) == 0 ? 0 : TAK_REJECT_RATE_LIMITED;
}

int TAK_TurnClock_Ack(TAK_TurnClock *c, int sim, const TAK_MsgAck *ack,
                      uint64_t now_ms) {
    if (!valid_sim(c, sim) || !c->started) return -1;
    /* Nobody has simulated a turn that has not been closed. */
    if (ack->last_turn >= c->head) return -1;
    TAK_TurnSim *s = &c->sim[sim];
    TAK_TurnClock_Heard(c, sim, now_ms);
    uint32_t done = ack->last_turn + 1u;
    if (done > s->done_turns) s->done_turns = done;

    if (ack->hash_tick != TAK_NET_NO_HASH) {
        if (ack->hash_tick % TAK_NET_HASH_TICKS != 0) return -1;
        if (ack->hash_tick > s->done_turns * (uint32_t)TAK_NET_TURN_TICKS) return -1;
        report_hash(c, sim, ack->hash_tick, ack->state_hash, now_ms);
    }

    if (s->status == TAK_PSTATUS_CATCHING_UP &&
        c->head - s->done_turns <= TAK_TURN_LAG_START / 2) {
        s->status = TAK_PSTATUS_CONNECTED;
        emit_status(c, sim, 0);
        if (s->reclaim) {
            /* SEAT_RECLAIM: the returning player takes the army back from
             * the computer on one tick, the same on every client. */
            uint8_t blob[8];
            size_t n = TAK_Sys_SeatReclaim(s->seat, s->client_id, blob, sizeof(blob));
            (void)TAK_TurnClock_System(c, blob, (uint16_t)n);
            s->reclaim = 0;
        }
    }
    return 0;
}

/* ── Pacing ───────────────────────────────────────────────────────────── */

static int may_pace(const TAK_TurnClock *c, int sim, uint8_t host_seat) {
    if (sim < 0) return 1;                     /* the server itself */
    if (!valid_sim(c, sim)) return 0;
    const TAK_TurnSim *s = &c->sim[sim];
    if (s->seat == TAK_NET_SEAT_NONE) return 0; /* watchers do not pace */
    if (c->cfg.host_paces_only && s->seat != host_seat) return 0;
    return 1;
}

int TAK_TurnClock_SetPaused(TAK_TurnClock *c, int sim, int paused,
                            uint8_t host_seat, uint64_t now_ms) {
    if (!may_pace(c, sim, host_seat)) return TAK_REJECT_NOT_ALLOWED;
    c->paused = (uint8_t)(paused ? 1 : 0);
    if (!c->paused) c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
    update_pace(c);
    return 0;
}

int TAK_TurnClock_SetSpeed(TAK_TurnClock *c, int sim, uint8_t speed,
                           uint8_t host_seat, uint64_t now_ms) {
    if (!may_pace(c, sim, host_seat)) return TAK_REJECT_NOT_ALLOWED;
    if (speed < TAK_NET_SPEED_MIN || speed > TAK_NET_SPEED_MAX)
        return TAK_REJECT_NOT_ALLOWED;
    c->cfg.speed = speed;
    c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
    update_pace(c);
    return 0;
}

/* ── Time ─────────────────────────────────────────────────────────────── */

static void run_countdowns(TAK_TurnClock *c, uint64_t now_ms) {
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        TAK_TurnSim *s = &c->sim[i];
        if (!s->in_use || s->status == TAK_PSTATUS_DROPPED) continue;
        if (s->status != TAK_PSTATUS_LOST && now_ms >= s->last_heard_ms &&
            now_ms - s->last_heard_ms >= TAK_TURN_LOST_MS) {
            TAK_TurnClock_Disconnect(c, i, now_ms);
            continue;
        }
        if (s->status != TAK_PSTATUS_LOST) continue;
        if (s->seat == TAK_NET_SEAT_NONE || now_ms >= s->lost_deadline_ms) {
            drop_sim(c, i, c->cfg.left_as, now_ms);
            c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
            continue;
        }
        uint16_t left = (uint16_t)((s->lost_deadline_ms - now_ms + 999u) / 1000u);
        if (left != s->countdown_shown) {
            s->countdown_shown = left;
            emit_status(c, i, left);
        }
    }
}

static void run_governor(TAK_TurnClock *c, uint64_t now_ms) {
    uint32_t worst = 0;
    uint8_t worst_seat = TAK_NET_SEAT_NONE;
    for (int i = 0; i < TAK_TURN_SIMS_MAX; i++) {
        TAK_TurnSim *s = &c->sim[i];
        /* Seated players only. A spectator falling behind just watches
         * later, it never slows the people playing. */
        if (!s->in_use || s->seat == TAK_NET_SEAT_NONE) continue;
        if (s->status != TAK_PSTATUS_CONNECTED && s->status != TAK_PSTATUS_LAGGING)
            continue;
        uint32_t behind = c->head - s->done_turns;
        if (behind > TAK_TURN_LAG_START) {
            if (!s->behind) { s->behind = 1; s->behind_since_ms = now_ms; }
            if (now_ms >= s->behind_since_ms &&
                now_ms - s->behind_since_ms >= TAK_TURN_LAG_HOLD_MS &&
                s->status != TAK_PSTATUS_LAGGING) {
                s->status = TAK_PSTATUS_LAGGING;
                emit_status(c, i, 0);
            }
        } else {
            /* Back under the line before the hold ran out: it starts over.
             * A player already lagging is let go only well under it. */
            s->behind = 0;
            if (s->status == TAK_PSTATUS_LAGGING && behind <= TAK_TURN_LAG_START / 2) {
                s->status = TAK_PSTATUS_CONNECTED;
                emit_status(c, i, 0);
            }
        }
        if (s->status == TAK_PSTATUS_LAGGING && behind >= worst) {
            worst = behind;
            worst_seat = s->seat;
        }
    }
    uint32_t q4 = 4;
    if (worst_seat != TAK_NET_SEAT_NONE) {
        /* The original's continuous throttle: the further behind, the
         * slower the turns, rather than a hard stop. */
        q4 = 4u * worst / TAK_TURN_LAG_START;
        if (q4 < 4) q4 = 4;
        if (q4 > 4u * TAK_TURN_GOVERNOR_MAX) q4 = 4u * TAK_TURN_GOVERNOR_MAX;
    }
    c->governor_q4 = (uint8_t)q4;
    c->governor_seat = worst_seat;
}

int TAK_TurnClock_Advance(TAK_TurnClock *c, uint64_t now_ms) {
    if (!c->started || c->ended) return 0;
    run_countdowns(c, now_ms);
    run_governor(c, now_ms);
    update_pace(c);
    if (!producing(c)) {
        c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
        return 0;
    }
    int closed = 0;
    while (now_ms >= c->next_close_ms && closed < TAK_TURN_BURST_MAX) {
        close_turn(c);
        closed++;
        c->next_close_ms += TAK_TurnClock_Period(c);
    }
    if (now_ms >= c->next_close_ms)
        c->next_close_ms = now_ms + TAK_TurnClock_Period(c);
    return closed;
}
