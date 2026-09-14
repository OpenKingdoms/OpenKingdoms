/*
 * match.c -- the simulation running on the server's turns.
 *
 * See tak_net_match.h for the three things that change in a match.
 */

#include "tak_net_match.h"

#include "tak_command_queue.h"

#include <string.h>

/* The protocol asks for a state hash every 60 ticks, which is what the
 * server compares between clients to catch a desync. */
#define MATCH_HASH_EVERY 60

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
    /* One CMD holds up to 64 commands, and the local seat rarely sends
     * more than a handful in a tick. */
    uint8_t        out[TAK_NET_CMD_BYTES_MAX];
    size_t         out_len;
    int            out_count;
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
    uint8_t buf[512];
    size_t len = 0;
    if (TAK_CommandSerialize(cmd, buf, sizeof buf, &len) != 0) return -1;
    if (len == 0 || g_match.out_len + len > sizeof g_match.out) return -1;
    if (g_match.out_count >= TAK_NET_CMDS_PER_MSG) return -1;

    /* Held only until the end of this frame. Sending one message a
     * frame rather than one a click is what keeps a busy minute of
     * ordering from becoming a message storm. */
    memcpy(g_match.out + g_match.out_len, buf, len);
    g_match.out_len += len;
    g_match.out_count++;
    return 0;
}

/* Everything held goes out as one CMD. The client does not say which
 * seat it is: the server stamps that, and that is the rule that stops
 * one player forging another's orders. */
static void flush_local(void) {
    if (g_match.out_count == 0 || !g_match.client) return;
    TAK_CmdBlob blob[TAK_NET_CMDS_PER_MSG];
    int n = 0;
    size_t off = 0;
    while (off < g_match.out_len && n < TAK_NET_CMDS_PER_MSG) {
        TAK_GameCommand tmp;
        size_t used = 0;
        if (TAK_CommandDeserialize(&tmp, g_match.out + off,
                                   g_match.out_len - off, &used) != 0) {
            break;
        }
        blob[n].data = g_match.out + off;
        blob[n].len = (uint16_t)used;
        off += used;
        n++;
    }
    if (n > 0) (void)TAK_NetClient_SendCommands(g_match.client, blob, n);
    g_match.out_len = 0;
    g_match.out_count = 0;
}

int TAK_Match_Pump(void) {
    if (!g_match.live || !g_match.client) return 0;
    flush_local();

    int taken = 0;
    TAK_NetTurn turn;
    while (TAK_NetClient_TakeTurn(g_match.client, &turn)) {
        uint32_t tick = turn.turn * (uint32_t)g_match.turn_ticks;
        for (int e = 0; e < turn.entry_count; e++) {
            /* The relay injects its own entries on TAK_NET_SEAT_SERVER,
             * a player leaving and the end of a match among them. Those
             * are not game commands and this build does not act on them
             * yet, so they are skipped rather than mis-read as one. */
            if (turn.entry[e].seat == TAK_NET_SEAT_SERVER) continue;
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

void TAK_Match_TickDone(uint32_t tick, uint32_t state_hash) {
    if (!g_match.live || !g_match.client) return;
    uint32_t hash_tick = TAK_NET_NO_HASH;
    uint64_t hash = 0;
    if (g_match.turn_ticks && (tick % MATCH_HASH_EVERY) == 0) {
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
