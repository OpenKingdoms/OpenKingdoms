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
    TAK_NC_EV_GONE           /* the connection went                     */
} TAK_NetClientEventKind;

typedef struct {
    TAK_NetClientEventKind kind;
} TAK_NetClientEvent;

/* Events are polled, not called back, so a screen reads them where it
 * already runs. Sixteen is more than a frame ever produces. */
#define TAK_NC_EVENTS_MAX 16

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
int TAK_NetClient_EditRoom(TAK_NetClient *c, const TAK_MsgRoomEdit *m);
int TAK_NetClient_Chat(TAK_NetClient *c, const TAK_MsgChat *m);
int TAK_NetClient_Start(TAK_NetClient *c);

#endif /* TAK_NET_CLIENT_H */
