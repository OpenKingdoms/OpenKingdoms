#ifndef TAK_NET_CLIENT_H
#define TAK_NET_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_protocol.h"

/*
 * The client half of a session, with no transport in it.
 *
 * The relay core takes whole messages and answers through an interface
 * a host program owns. This is the same shape from the other side:
 * whole messages in, whole messages out, and the screens ask it what
 * happened. Nothing here opens a socket or knows what a WebSocket is.
 *
 * That is not tidiness. The browser build has a WebSocket already, in
 * the page, and it hands over whole messages. A native build has
 * ws_conn under it doing the framing. Both drive this, and neither
 * changes it.
 *
 * The screens never read the wire. They poll events and read the room
 * snapshot, which is what makes the lobby testable with no server.
 */

/* Where the session is. A screen shows a different thing at each. */
typedef enum {
    TAK_NC_IDLE = 0,     /* nothing said yet                         */
    TAK_NC_GREETING,     /* HELLO sent, waiting to be let in         */
    TAK_NC_LOBBY,        /* welcomed, in the room list               */
    TAK_NC_ROOM,         /* sitting in a room                        */
    TAK_NC_LOADING,      /* START_GAME arrived, building the world    */
    TAK_NC_PLAYING,      /* GO arrived, turns are flowing             */
    TAK_NC_REFUSED,      /* the server said no, reason in `reject`   */
    TAK_NC_GONE          /* the connection went                      */
} TAK_NetClientState;

typedef enum {
    TAK_NC_EV_NONE = 0,
    TAK_NC_EV_WELCOMED,      /* the session is up, `welcome` is filled  */
    TAK_NC_EV_REFUSED,       /* `reject` says why                       */
    TAK_NC_EV_ROOM_LIST,     /* `rooms` changed                         */
    TAK_NC_EV_ROOM_STATE,    /* `room` changed                          */
    TAK_NC_EV_CHAT,          /* `chat` holds the line                   */
    TAK_NC_EV_LEFT_ROOM,     /* back in the lobby                       */
    TAK_NC_EV_START_GAME,    /* `start` says what to build              */
    TAK_NC_EV_LOAD_STATE,    /* `load_state` changed, for the seven rows */
    TAK_NC_EV_GO,            /* turn zero, the battle runs              */
    TAK_NC_EV_TURN,          /* a turn is held, take it when ready      */
    TAK_NC_EV_PACE,          /* `pace` changed, speed or a lagging seat */
    TAK_NC_EV_PLAYER_STATUS, /* `status` changed for one seat           */
    TAK_NC_EV_GONE           /* the connection went                     */
} TAK_NetClientEventKind;

typedef struct {
    TAK_NetClientEventKind kind;
} TAK_NetClientEvent;

/* Events are polled, not called back, so a screen reads them where it
 * already runs. Sixteen is more than a frame ever produces. */
#define TAK_NC_EVENTS_MAX 16

/* Turns held but not yet simulated. The clock closes one every 3 ticks
 * and a client runs one to three behind, so this is a wide margin. A
 * client that falls further behind than this is one the server's
 * governor has already slowed the room for. */
#define TAK_NC_TURNS_MAX  256
/* The commands inside them. A turn is usually a few bytes and a busy
 * one is a few hundred, so this holds several seconds of a real fight
 * even when every seat is issuing orders. */
#define TAK_NC_TURN_ARENA (256u << 10)

/* One turn, ready to hand to the simulation. */
typedef struct TAK_NetTurn {
    uint32_t turn;
    uint8_t  entry_count;
    struct {
        uint8_t  seat;         /* TAK_NET_SEAT_SERVER for the relay's own */
        uint8_t  count;
        uint16_t len[TAK_NET_CMDS_PER_MSG];
        /* Into the client's arena, good until the turn is taken. */
        const uint8_t *data[TAK_NET_CMDS_PER_MSG];
    } entry[TAK_NET_SEATS + 1];
} TAK_NetTurn;

typedef struct TAK_NetClient {
    TAK_NetClientState state;

    /* What the last exchange left. A screen reads these. */
    TAK_MsgWelcome   welcome;
    TAK_MsgReject    reject;
    TAK_MsgRoomList  rooms;
    TAK_MsgRoomState room;
    TAK_MsgChat      chat;
    /* Which seat the server gave us, once we are in a room, and
     * TAK_NET_SEAT_NONE before that. The server stamps every command
     * with it, so nothing here has to be told twice. */
    uint8_t          seat;
    uint32_t         session_id;
    /* The round trip the last PING measured, for the Ping column. */
    uint32_t         ping_ms;

    /* The match, once one starts. */
    TAK_MsgStartGame    start;
    TAK_MsgLoadState    load_state;
    TAK_MsgPace         pace;
    TAK_MsgPlayerStatus status;
    /* The first turn GO named, and the next one not yet taken. A
     * simulation never runs past what is held, which is the whole of
     * lockstep in one sentence. */
    uint32_t next_turn;
    uint32_t last_turn_held;
    /* Ours and rising. It is not what orders the commands, the turn
     * is, but it lets the server see a gap. */
    uint32_t cmd_seq;

    /* Turns held, oldest first. The commands live in the arena and the
     * entries point into it, so taking a turn frees its bytes. */
    struct {
        uint32_t turn;
        uint8_t  entry_count;
        uint8_t  seat[TAK_NET_SEATS + 1];
        uint8_t  count[TAK_NET_SEATS + 1];
        uint32_t first_cmd;     /* index into cmd_len / cmd_off      */
    } held[TAK_NC_TURNS_MAX];
    uint32_t held_head, held_count;
    uint16_t cmd_len[TAK_NC_TURNS_MAX * 8];
    uint32_t cmd_off[TAK_NC_TURNS_MAX * 8];
    uint32_t cmd_count;
    uint8_t  arena[TAK_NC_TURN_ARENA];
    uint32_t arena_len;
    /* A turn that did not fit. The match cannot go on from a gap, so
     * this is fatal and says so rather than simulating a hole. */
    uint8_t  turns_lost;

    TAK_NetClientEvent event[TAK_NC_EVENTS_MAX];
    int                event_count;

    /* Messages waiting to go out. One frame each, in order. */
    uint8_t  out[TAK_NET_FRAME_MAX * 4];
    size_t   out_len;
    /* Refused to queue something because the buffer was full, which a
     * host turns into a dropped connection rather than a silent loss. */
    uint8_t  out_overflow;
} TAK_NetClient;

/* Start a session. Writes HELLO into the out queue, so the host has
 * something to send the moment the transport is up. */
void TAK_NetClient_Init(TAK_NetClient *c, const TAK_MsgHello *hello);

/* One whole protocol message arrived. Returns 0 when it was understood,
 * -1 when it was not, which a host treats as a connection to drop. */
int  TAK_NetClient_OnMessage(TAK_NetClient *c, const void *msg, size_t len,
                             uint64_t now_ms);

/* The transport went. Marks the session gone and raises the event. */
void TAK_NetClient_OnClose(TAK_NetClient *c);

/* Bytes the host should send, and how many of them it managed. Every
 * message is whole, so a host that frames them can take one at a time
 * with TAK_Net_Split. */
const uint8_t *TAK_NetClient_Pending(const TAK_NetClient *c, size_t *len);
void TAK_NetClient_Sent(TAK_NetClient *c, size_t len);

/* Take the next whole message rather than a byte count, for a host
 * that sends one frame at a time. Returns its length, or 0. */
size_t TAK_NetClient_TakeMessage(TAK_NetClient *c, void *out, size_t cap);

/* Read the events this exchange produced, then forget them. */
int  TAK_NetClient_PollEvent(TAK_NetClient *c, TAK_NetClientEvent *out);

/* ── What a screen asks for ────────────────────────────────────────── */

int TAK_NetClient_ListRooms(TAK_NetClient *c);
int TAK_NetClient_CreateRoom(TAK_NetClient *c, const TAK_MsgCreateRoom *m);
int TAK_NetClient_JoinRoom(TAK_NetClient *c, const TAK_MsgJoinRoom *m);
int TAK_NetClient_LeaveRoom(TAK_NetClient *c);
/* A field that changes your own row is stamped with your own seat
 * before it goes, because the server refuses one that names any other
 * and a screen should not have to know that. A host only field, a kick
 * or a block, keeps the seat the caller put in it. */
int TAK_NetClient_EditRoom(TAK_NetClient *c, const TAK_MsgRoomEdit *m);
int TAK_NetClient_Chat(TAK_NetClient *c, const TAK_MsgChat *m);
int TAK_NetClient_Start(TAK_NetClient *c);

/* ── The match ─────────────────────────────────────────────────────── */

/* How far the loading screen has got, which fills the other seats'
 * rows on everyone else's screen. */
int TAK_NetClient_ReportLoadProgress(TAK_NetClient *c, uint8_t percent);

/* The world is built. The hash is over the freshly built world, and
 * the server compares them before the first tick, which catches a data
 * mismatch while it is still a message rather than a desync ten
 * minutes in. */
int TAK_NetClient_ReportLoaded(TAK_NetClient *c, uint64_t world_hash);

/* Send this frame's commands. The client does not say which seat it
 * is: the server stamps that, and that is the rule that stops one
 * player forging another's orders. */
int TAK_NetClient_SendCommands(TAK_NetClient *c, const TAK_CmdBlob *cmd,
                               int count);

/* Take the next turn to simulate. Returns 1 and fills `out`, or 0 when
 * nothing is held, which means the simulation waits. The command bytes
 * are good until the next call. */
int TAK_NetClient_TakeTurn(TAK_NetClient *c, TAK_NetTurn *out);

/* How many turns are held and not yet taken, for the adaptive buffer
 * and for the "waiting for" overlay. */
uint32_t TAK_NetClient_TurnsHeld(const TAK_NetClient *c);

/* Acknowledge simulation up to `last_turn`. Every 60 ticks the caller
 * passes the tick and the simulation hash, else TAK_NET_NO_HASH. */
int TAK_NetClient_Ack(TAK_NetClient *c, uint32_t last_turn,
                      uint32_t hash_tick, uint64_t state_hash);

#endif /* TAK_NET_CLIENT_H */
