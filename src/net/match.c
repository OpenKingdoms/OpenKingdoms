/*
 * match.c -- the simulation running on the server's turns.
 *
 * See tak_net_match.h for the three things that change in a match.
 */

#include "tak_net_match.h"

#include "tak_command_queue.h"

#include <stdio.h>
#include <string.h>

/* The protocol asks for a state hash every 60 ticks, which is what the
 * server compares between clients to catch a desync. */
#define MATCH_HASH_EVERY 60

/* The relay takes TAK_NET_CMD_BYTES_MAX bytes and TAK_NET_CMDS_PER_MSG
 * commands from a seat a turn and refuses the rest without a word. A
 * seat sends at most this much for each turn it is handed, so three
 * sends bunched into one of the relay's turns still fit. What does not
 * fit waits here for the next. */
#define MATCH_SEND_BYTES  1300
#define MATCH_SEND_CMDS   20
#define MATCH_BACKLOG     (128u * 1024u)
/* Turns go into the command queue no further ahead of the simulation
 * than this, and not while the queue is this full. A rejoin or a drop
 * in holds the whole log, and the queue holds TAK_CMD_QUEUE_MAX. */
#define MATCH_PUMP_AHEAD_TICKS  120u
#define MATCH_PUMP_QUEUE_ROOM   (TAK_CMD_QUEUE_MAX / 2)

static struct {
    TAK_NetClient *client;
    uint8_t        live;
    uint8_t        seat;
    uint8_t        turn_ticks;
    /* Turns taken from the client, so the tick limit is this times the
     * ticks a turn covers. */
    uint32_t       turns_taken;
    uint32_t       last_turn;
    /* The verdict went to the server. Once a match, whoever asks. */
    uint8_t        reported;
    /* The local seat's commands not yet sent, oldest first, and what
     * went out since the turn count last moved. */
    uint8_t        out[MATCH_BACKLOG];
    size_t         out_len;
    int            out_count;
    uint32_t       send_turn;
    size_t         send_bytes;
    int            send_cmds;
} g_match;

void TAK_Match_Begin(TAK_NetClient *client, uint8_t seat, uint8_t turn_ticks) {
    memset(&g_match, 0, sizeof g_match);
    g_match.client = client;
    g_match.seat = seat;
    /* A turn of zero ticks would never let the simulation move, so a
     * server that says nothing gets the design's own three. */
    g_match.turn_ticks = turn_ticks ? turn_ticks : 3;
    g_match.live = 1;
    g_match.last_turn = 0;
    /* Every command now arrives already stamped with the tick its turn
     * owns, so the queue adds no delay of its own. */
    TAK_CmdQueue_Reset(0);
}

void TAK_Match_End(void) {
    memset(&g_match, 0, sizeof g_match);
    g_match.seat = TAK_NET_SEAT_NONE;
}

int TAK_Match_IsLive(void) { return g_match.live != 0; }

uint8_t TAK_Match_Seat(void) {
    return g_match.live ? g_match.seat : TAK_NET_SEAT_NONE;
}

uint32_t TAK_Match_TickLimit(void) {
    return g_match.turns_taken * (uint32_t)g_match.turn_ticks;
}

int TAK_Match_CanAdvance(void) {
    if (!g_match.live) return 1;
    return TAK_CmdQueue_Tick() < TAK_Match_TickLimit();
}

/* The protocol counts seats from zero and the simulation counts
 * players from one, so a turn entry is translated on the way in. They
 * are not the same number and nothing else in either layer knows that.
 * The queue refuses a seat of zero outright, which is how this was
 * found: every order from the first seat vanished in silence. */
static uint8_t match_seat_to_player(uint8_t seat) {
    return (uint8_t)(seat + 1);
}

int TAK_Match_SubmitLocal(const TAK_GameCommand *cmd) {
    if (!g_match.live || !g_match.client || !cmd) return -1;
    uint8_t buf[TAK_COMMAND_MAX_BYTES];
    size_t len = 0;
    if (TAK_CommandSerialize(cmd, buf, sizeof buf, &len) != 0) return -1;
    if (len == 0 || g_match.out_len + len > sizeof g_match.out) return -1;

    /* Held until the pump sends it, at most one message a frame, paced
     * to what the relay takes. */
    memcpy(g_match.out + g_match.out_len, buf, len);
    g_match.out_len += len;
    g_match.out_count++;
    return 0;
}

/* What is held goes out as one CMD, as much of it as this turn's share
 * allows, oldest first. The client does not say which seat it is: the
 * server stamps that, and that is the rule that stops one player
 * forging another's orders. */
static void flush_local(void) {
    if (g_match.out_count == 0 || !g_match.client) return;
    if (g_match.turns_taken != g_match.send_turn) {
        g_match.send_turn = g_match.turns_taken;
        g_match.send_bytes = 0;
        g_match.send_cmds = 0;
    }
    static TAK_GameCommand tmp;
    TAK_CmdBlob blob[TAK_NET_CMDS_PER_MSG];
    int n = 0;
    size_t off = 0;
    while (off < g_match.out_len && n < TAK_NET_CMDS_PER_MSG) {
        size_t used = 0;
        if (TAK_CommandDeserialize(&tmp, g_match.out + off,
                                   g_match.out_len - off, &used) != 0) {
            /* Nothing this build wrote reads back wrong: drop the rest
             * rather than send it half. */
            g_match.out_len = off;
            break;
        }
        /* One command bigger than a share still goes, on its own. */
        int first = g_match.send_bytes == 0 && n == 0;
        if (!first && (g_match.send_bytes + used > MATCH_SEND_BYTES ||
                       g_match.send_cmds + 1 > MATCH_SEND_CMDS)) break;
        blob[n].data = g_match.out + off;
        blob[n].len = (uint16_t)used;
        off += used;
        n++;
        g_match.send_bytes += used;
        g_match.send_cmds++;
    }
    if (n > 0) (void)TAK_NetClient_SendCommands(g_match.client, blob, n);
    memmove(g_match.out, g_match.out + off, g_match.out_len - off);
    g_match.out_len -= off;
    g_match.out_count -= n;
    if (g_match.out_len == 0) g_match.out_count = 0;
}

int TAK_Match_Unsent(void) { return g_match.out_count; }

/* The relay's own entry in a turn, as the command every simulation
 * applies on that turn's tick. Who plays a seat changes here and only
 * here, so every machine hands it over on the same tick. */
static void submit_system(const uint8_t *blob, uint16_t len, uint32_t tick) {
    TAK_SysCmd sys;
    if (TAK_Sys_Decode(&sys, blob, len) != 0 || sys.seat >= TAK_NET_SEATS) return;
    TAK_GameCommand cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = TAK_CMD_SEAT_CONTROL;
    cmd.seat = match_seat_to_player(sys.seat);
    cmd.tick = tick;
    switch (sys.type) {
    case TAK_SYS_PLAYER_LEFT:
        cmd.arg = sys.arg == TAK_LEFT_COMPUTER_TAKES_OVER ? TAK_SEAT_TO_COMPUTER
                : sys.arg == TAK_LEFT_ARMY_REMOVED        ? TAK_SEAT_ARMY_REMOVED
                                                          : TAK_SEAT_RESIGNED;
        break;
    case TAK_SYS_SEAT_RECLAIM:
    case TAK_SYS_SEAT_TAKEOVER:
        cmd.arg = TAK_SEAT_TO_HUMAN;
        break;
    default:
        return;
    }
    (void)TAK_CmdQueue_SubmitAt(&cmd);
}

/* Whether the next held turn may go into the queue now. */
static int pump_has_room(void) {
    if (TAK_CmdQueue_Pending() >= MATCH_PUMP_QUEUE_ROOM) return 0;
    return TAK_Match_TickLimit() < TAK_CmdQueue_Tick() + MATCH_PUMP_AHEAD_TICKS;
}

int TAK_Match_Pump(void) {
    if (!g_match.live || !g_match.client) return 0;
    flush_local();

    int taken = 0;
    TAK_NetTurn turn;
    while (pump_has_room() && TAK_NetClient_TakeTurn(g_match.client, &turn)) {
        uint32_t tick = turn.turn * (uint32_t)g_match.turn_ticks;
        for (int e = 0; e < turn.entry_count; e++) {
            if (turn.entry[e].seat == TAK_NET_SEAT_SERVER) {
                for (int k = 0; k < turn.entry[e].count; k++)
                    submit_system(turn.entry[e].data[k], turn.entry[e].len[k], tick);
                continue;
            }
            for (int k = 0; k < turn.entry[e].count; k++) {
                TAK_GameCommand cmd;
                size_t used = 0;
                if (TAK_CommandDeserialize(&cmd, turn.entry[e].data[k],
                                           turn.entry[e].len[k], &used) != 0) {
                    /* A command this build cannot read is a command it
                     * must not guess at: every other client is about to
                     * apply it and this one would diverge. */
                    continue;
                }
                /* Only the relay hands a seat over. A player's own copy
                 * is a forgery, dropped the same way everywhere. */
                if (cmd.type == TAK_CMD_SEAT_CONTROL) continue;
                cmd.seat = match_seat_to_player(turn.entry[e].seat);
                cmd.tick = tick;
                (void)TAK_CmdQueue_SubmitAt(&cmd);
            }
        }
        g_match.turns_taken = turn.turn + 1;
        g_match.last_turn = turn.turn;
        taken++;
    }
    return taken;
}

uint32_t TAK_Match_TicksBehind(void) {
    if (!g_match.live || !g_match.client) return 0;
    uint32_t have = (g_match.turns_taken + TAK_NetClient_TurnsHeld(g_match.client)) *
                    (uint32_t)g_match.turn_ticks;
    uint32_t now = TAK_CmdQueue_Tick();
    return have > now ? have - now : 0;
}

static const char *seat_name(const TAK_NetClient *c, uint8_t seat) {
    return c->start.slot[seat].name[0] ? c->start.slot[seat].name : "a player";
}

int TAK_Match_Waiting(char *out, size_t cap) {
    if (!out || cap == 0) return TAK_MATCH_FLOWING;
    out[0] = '\0';
    if (!g_match.live || !g_match.client) return TAK_MATCH_FLOWING;
    const TAK_NetClient *c = g_match.client;
    uint8_t seat = TAK_NET_SEAT_NONE;
    int secs = -1;
    if (c->status.status == TAK_PSTATUS_LOST && c->status.seat < TAK_NET_SEATS) {
        seat = c->status.seat;
        secs = (int)c->status.countdown_secs;
    } else if (c->pace.reason == TAK_PACE_WAITING_FOR_PLAYER && c->pace.paused &&
               c->pace.seat < TAK_NET_SEATS) {
        seat = c->pace.seat;
    }
    if (seat != TAK_NET_SEAT_NONE && seat != g_match.seat) {
        if (secs >= 0) snprintf(out, cap, "Waiting for %s, %d s", seat_name(c, seat), secs);
        else snprintf(out, cap, "Waiting for %s", seat_name(c, seat));
        return TAK_MATCH_STALLED;
    }
    /* The turns still come, slower, while the governor holds the room
     * back for the seats it says lag: the pace's seat first, then any
     * other the server has marked lagging. */
    uint8_t lag[TAK_NET_SEATS];
    int n = 0;
    if (c->pace.reason == TAK_PACE_WAITING_FOR_PLAYER && !c->pace.paused &&
        c->pace.seat < TAK_NET_SEATS && c->pace.seat != g_match.seat)
        lag[n++] = c->pace.seat;
    for (uint8_t s = 0; s < TAK_NET_SEATS; s++) {
        if (s == g_match.seat || (n && lag[0] == s)) continue;
        if (c->seat_status[s] == TAK_PSTATUS_LAGGING) lag[n++] = s;
    }
    if (n == 0) return TAK_MATCH_FLOWING;
    size_t used = (size_t)snprintf(out, cap, "Slowing down to wait for %s",
                                   seat_name(c, lag[0]));
    for (int i = 1; i < n && used < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%s%s",
                                 i + 1 == n ? " and " : ", ", seat_name(c, lag[i]));
    return TAK_MATCH_SLOWED;
}

int TAK_Match_WantsHash(uint32_t tick) {
    return g_match.live && g_match.client && g_match.turn_ticks &&
           (tick % MATCH_HASH_EVERY) == 0;
}

void TAK_Match_TickDone(uint32_t tick, uint32_t state_hash) {
    if (!g_match.live || !g_match.client) return;
    uint32_t hash_tick = TAK_NET_NO_HASH;
    uint64_t hash = 0;
    if (TAK_Match_WantsHash(tick)) {
        hash_tick = tick;
        /* The protocol's field is 64 bits and the simulation hash is 32
         * today, so it is widened here rather than the protocol
         * narrowed. A wider hash later needs no change on the wire. */
        hash = (uint64_t)state_hash;
    }
    (void)TAK_NetClient_Ack(g_match.client, g_match.last_turn, hash_tick, hash);
}

int TAK_Match_ReportResult(TAK_MsgMatchResult *m) {
    if (!g_match.live || !g_match.client || !m || g_match.reported) return -1;
    /* The server checks the match id against the one it named in
     * START_GAME, and the stats version says which tally set this is. */
    m->match_id = g_match.client->start.match_id;
    m->stats_version = TAK_NET_STATS_VERSION;
    if (TAK_NetClient_ReportMatchResult(g_match.client, m) != 0) return -1;
    g_match.reported = 1;
    return 0;
}

int TAK_Match_Reported(void) { return g_match.live && g_match.reported; }
