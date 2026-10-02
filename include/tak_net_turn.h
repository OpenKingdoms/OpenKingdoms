#ifndef TAK_NET_TURN_H
#define TAK_NET_TURN_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_protocol.h"

/*
 * The turn clock and the turn log. Design in docs/MULTIPLAYER.md.
 *
 * The relay closes a turn on its own cadence and broadcasts it without
 * waiting for any client. A command that arrives late lands in the next
 * open turn, so one player's jitter never stalls the others. Speed and
 * pause are how fast turns are produced, never simulation state.
 *
 * Like the room, this is a pure state machine: no sockets, no clock of its
 * own and no allocation. The caller passes the time in and hands over the
 * log's storage, and everything it wants sent comes out through one
 * callback.
 */

/* Everyone who runs a simulation: eight seats and the watchers. */
#define TAK_TURN_SIMS_MAX      (TAK_NET_SEATS + TAK_NET_WATCHERS_MAX)
#define TAK_TURN_SIM_ALL       (-1)

/* Game speed as server pacing, N-004. Our own scale: at the normal level a
 * turn closes every turn_ms, and a level twice as high runs twice as fast. */
#define TAK_NET_SPEED_MIN      1
#define TAK_NET_SPEED_MAX      10
#define TAK_NET_SPEED_NORMAL   5

/* The governor. A seated player more than half a second behind for two
 * seconds slows turn production, on a linear curve up to four times the
 * normal period. The original waited four seconds, which suited stale
 * replicas but not a player issuing orders against a stale view. */
#define TAK_TURN_LAG_START     10      /* turns, half a second at 50 ms */
#define TAK_TURN_LAG_HOLD_MS   2000u
#define TAK_TURN_GOVERNOR_MAX  4
/* Silence this long means the connection is lost. */
#define TAK_TURN_LOST_MS       5000u
/* After a server stall, close at most this many turns in one go and let
 * the rest of the debt go, rather than bursting turns at the clients. */
#define TAK_TURN_BURST_MAX     4

/* A paced catch up keeps at most this many turns, and about this many
 * bytes of them, sent ahead of what the simulation has acknowledged. A
 * browser page holds 256 KB of messages, and a log from a long match is
 * far more than that. */
#define TAK_TURN_PACE_TURNS    128u
#define TAK_TURN_PACE_BYTES    (96u << 10)

/* Hash rows waiting for every reporter, and the agreed hashes kept for
 * checking a player who is catching up. 2048 hash ticks is 34 minutes. */
#define TAK_TURN_HASH_ROWS     8
#define TAK_TURN_HASH_HISTORY  2048

/* ── The turn log ─────────────────────────────────────────────────────── */

typedef struct TAK_TurnLogEntry {
    uint32_t turn;     /* first turn the entry covers */
    uint32_t run;      /* turns covered, more than one only for empty runs */
    uint32_t offset;   /* the stored TURN frame, for a turn with commands */
    uint32_t length;   /* 0 for an empty run */
} TAK_TurnLogEntry;

typedef struct TAK_TurnLog {
    uint8_t          *arena;
    size_t            arena_cap;
    size_t            arena_used;
    TAK_TurnLogEntry *entry;
    uint32_t          entry_cap;
    uint32_t          entry_count;
    uint32_t          next_turn;   /* turns before this one are logged */
    int               full;        /* sticky: storage ran out */
} TAK_TurnLog;

void TAK_TurnLog_Init(TAK_TurnLog *log, void *arena, size_t arena_cap,
                      TAK_TurnLogEntry *entries, uint32_t entry_cap);

/* Append the next turn in order. `frame` is the whole encoded TURN frame,
 * or NULL with len 0 for an empty turn, which joins a run with the empty
 * turns before it. Returns 0, or -1 when out of order or out of room. */
int TAK_TurnLog_Append(TAK_TurnLog *log, uint32_t turn,
                       const uint8_t *frame, size_t len);

/* The frame that replays `from_turn`. An empty turn comes back as one
 * TURN covering the rest of its run, which is where consecutive empty
 * turns collapse into a range. Writes *covered with the number of turns
 * the frame accounts for. Returns the frame length, or 0 when the turn is
 * not logged or does not fit. */
size_t TAK_TurnLog_Frame(const TAK_TurnLog *log, uint32_t from_turn,
                         void *out, size_t cap, uint32_t *covered);

/* ── The clock ────────────────────────────────────────────────────────── */

/* The one way out. `sim` is a simulation index, or TAK_TURN_SIM_ALL for
 * everyone in the match. */
typedef void (*TAK_TurnSend)(void *user, int sim,
                             const uint8_t *frame, size_t len);

typedef struct TAK_TurnClockCfg {
    uint16_t turn_ms;         /* TAK_NET_TURN_MS */
    uint8_t  speed;           /* TAK_NET_SPEED_* */
    uint16_t timeout_secs;    /* the room's reject countdown */
    uint8_t  left_as;         /* TAK_NetLeftAs when a countdown runs out */
    uint8_t  host_paces_only; /* only the host may pause or change speed */
} TAK_TurnClockCfg;

typedef struct TAK_TurnSim {
    uint8_t  in_use;
    uint8_t  seat;              /* TAK_NET_SEAT_NONE for a watcher */
    uint8_t  status;            /* TAK_NetPlayerStatus */
    uint8_t  reclaim;           /* reclaim its army once caught up */
    uint8_t  takeover;          /* a drop in: take the seat once caught up */
    uint8_t  behind;            /* past the governor's threshold */
    uint8_t  seq_seen;
    /* Replays the log no faster than it acknowledges, set for a client
     * that can take it that way. Streaming while that replay has not
     * reached the head, and live turns wait in the log until it has. */
    uint8_t  paced;
    uint8_t  streaming;
    /* A watcher's hash for a tick the players have not agreed yet. It is
     * checked once they have, and never counts towards the agreement. */
    uint8_t  pend_set;
    uint32_t pend_tick;
    uint64_t pend_hash;
    uint32_t sent_turns;        /* the next logged turn a stream sends */
    uint32_t client_id;
    uint32_t done_turns;        /* turns fully simulated */
    uint32_t last_seq;          /* newest CMD sequence taken */
    uint64_t last_heard_ms;
    uint64_t behind_since_ms;
    uint64_t lost_deadline_ms;  /* when a lost player is dropped */
    uint16_t countdown_shown;
} TAK_TurnSim;

typedef struct TAK_TurnPending {
    uint8_t  count;
    uint16_t used;
    uint16_t len[TAK_NET_CMDS_PER_MSG];
    uint8_t  bytes[TAK_NET_CMD_BYTES_MAX];
} TAK_TurnPending;

typedef struct TAK_TurnHashRow {
    uint8_t  used;
    uint32_t tick;
    uint32_t have;                         /* sims that reported */
    uint64_t hash[TAK_TURN_SIMS_MAX];
} TAK_TurnHashRow;

typedef struct TAK_TurnConsensus {
    uint8_t  set;
    uint32_t tick;
    uint64_t hash;
} TAK_TurnConsensus;

/* What the last desync looked like. The relay reads it to write its
 * report. A halt stops the match. An outlier is sent back to turn zero. */
typedef struct TAK_TurnDesync {
    uint32_t count;
    uint32_t tick;
    uint32_t outliers;          /* sims that disagreed */
    uint8_t  halted;
} TAK_TurnDesync;

#define TAK_TURN_SEAT_STAYED 0xffffffffu

typedef struct TAK_TurnClock {
    TAK_TurnClockCfg  cfg;
    TAK_TurnLog      *log;
    TAK_TurnSend      send;
    void             *user;

    uint8_t           started;
    uint8_t           paused;         /* by a player, N-004 */
    uint8_t           halted;         /* by a desync with no majority */
    uint8_t           ended;
    uint32_t          head;           /* turns closed so far */
    uint64_t          next_close_ms;
    /* The turn each seat's player left on, by resigning, being
     * removed or dropping, or TAK_TURN_SEAT_STAYED. A reclaim clears it. */
    uint32_t          seat_left_turn[TAK_NET_SEATS];

    /* What the clients were last told about pacing. */
    uint8_t           pace_reason;
    uint8_t           pace_seat;
    uint8_t           pace_paused;
    uint8_t           pace_speed;
    uint16_t          pace_period;
    uint8_t           governor_q4;    /* period multiple in quarters, 4 is 1x */
    uint8_t           governor_seat;

    TAK_TurnSim       sim[TAK_TURN_SIMS_MAX];
    TAK_TurnPending   pending[TAK_NET_SEATS];
    TAK_TurnPending   system;

    TAK_TurnHashRow   row[TAK_TURN_HASH_ROWS];
    TAK_TurnConsensus history[TAK_TURN_HASH_HISTORY];
    TAK_TurnDesync    desync;

    uint8_t           frame[TAK_NET_FRAME_MAX];
} TAK_TurnClock;

void TAK_TurnClock_Init(TAK_TurnClock *c, const TAK_TurnClockCfg *cfg,
                        TAK_TurnLog *log, TAK_TurnSend send, void *user);

/* A simulation that joins the match. Returns its index, or -1. */
int TAK_TurnClock_AddSim(TAK_TurnClock *c, uint32_t client_id, uint8_t seat,
                         uint64_t now_ms);
int TAK_TurnClock_SimOf(const TAK_TurnClock *c, uint32_t client_id);

/* GO: turn zero opens now and everyone is told. */
void TAK_TurnClock_Start(TAK_TurnClock *c, uint64_t now_ms);

/* Any message from a simulation. Brings a lost player straight back. */
void TAK_TurnClock_Heard(TAK_TurnClock *c, int sim, uint64_t now_ms);

/* A player's commands for the open turn. The seat is stamped from the
 * sender, never read from the message, which is the rule that stops order
 * forging. Returns 0, or a reject reason. A repeated sequence number is
 * dropped and returns 0, so a duplicated frame changes nothing. */
int TAK_TurnClock_Command(TAK_TurnClock *c, int sim, const TAK_MsgCmd *cmd);

/* A system command for the open turn, on TAK_NET_SEAT_SERVER. */
int TAK_TurnClock_System(TAK_TurnClock *c, const uint8_t *blob, uint16_t len);

/* An acknowledgement, with a state hash when it carries one. Returns 0,
 * or -1 for an ACK claiming a turn that has not been closed yet. */
int TAK_TurnClock_Ack(TAK_TurnClock *c, int sim, const TAK_MsgAck *ack,
                      uint64_t now_ms);

/* Pacing, N-004. `sim` is who asked, checked against the room's rule. */
int TAK_TurnClock_SetPaused(TAK_TurnClock *c, int sim, int paused,
                            uint8_t host_seat, uint64_t now_ms);
int TAK_TurnClock_SetSpeed(TAK_TurnClock *c, int sim, uint8_t speed,
                           uint8_t host_seat, uint64_t now_ms);

/* Replay the log to this simulation at the pace it acknowledges, rather
 * than all at once. Set before its catch up begins. */
void TAK_TurnClock_SetPaced(TAK_TurnClock *c, int sim, int paced);

/* Whether a catch up to the head could be sent now: the log still holds
 * every turn of the match. */
int TAK_TurnClock_CanReplay(const TAK_TurnClock *c);

/* The transport closed. The player counts as lost from now. */
void TAK_TurnClock_Disconnect(TAK_TurnClock *c, int sim, uint64_t now_ms);

/* A player back on a fresh connection, replaying from `from_turn`, which
 * is 0 after a restart. Sends GO for that turn and the log after it, all
 * of it or, paced, a window at a time. GO for turn zero tells a client to
 * reset its world first. A watcher arriving leaves the turn cadence
 * alone. Returns 0, or -1 when the log no longer holds the turns it
 * would need. */
int TAK_TurnClock_Reconnect(TAK_TurnClock *c, int sim, uint32_t from_turn,
                            uint64_t now_ms);

/* Who holds a seat in the match: nobody, a player still in it, or only
 * players who have gone and left it to the computer. */
enum { TAK_SEAT_HELD_NONE = 0, TAK_SEAT_HELD_LIVE, TAK_SEAT_HELD_GONE };
int TAK_TurnClock_SeatHeld(const TAK_TurnClock *c, uint8_t seat);

/* A player dropping in to a match under way, to take `seat` from the
 * computer. The seat must be held by nobody live. Returns the new
 * simulation, which TAK_TurnClock_Reconnect then replays from turn 0.
 * Once it has caught up, TAK_SYS_SEAT_TAKEOVER goes into the open turn
 * and the seat is theirs. Until then it pauses nothing and orders
 * nothing. Returns -1 when the seat or the match cannot take one. */
int TAK_TurnClock_DropIn(TAK_TurnClock *c, uint32_t client_id, uint8_t seat,
                         uint64_t now_ms);

/* Forget a simulation outright, as if it never joined. For a drop in
 * that leaves before it takes its seat. */
void TAK_TurnClock_Forget(TAK_TurnClock *c, int sim, uint64_t now_ms);

/* The host rejects a lost player before the countdown ends. */
int TAK_TurnClock_Reject(TAK_TurnClock *c, uint8_t seat, uint64_t now_ms);

/* A player resigns. Their army goes the way resigning takes it. */
int TAK_TurnClock_Resign(TAK_TurnClock *c, int sim);

/* Move time on. Closes every turn that is due, runs the governor and the
 * lost player countdowns, and sends whatever that produced. Returns the
 * number of turns closed. */
int TAK_TurnClock_Advance(TAK_TurnClock *c, uint64_t now_ms);

/* The period a turn is currently produced at, in milliseconds. */
uint16_t TAK_TurnClock_Period(const TAK_TurnClock *c);

#endif /* TAK_NET_TURN_H */
