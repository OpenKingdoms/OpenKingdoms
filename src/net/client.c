/*
 * client.c -- the client half of a session, with no transport in it.
 *
 * See tak_net_client.h for why. The short version is that the browser
 * already has a WebSocket in the page and a native build has ws_conn
 * under it, and neither of them should change this file.
 */

#include "tak_net_client.h"

#include <string.h>

static void raise_event(TAK_NetClient *c, TAK_NetClientEventKind k) {
    if (c->event_count >= TAK_NC_EVENTS_MAX) {
        /* A screen that does not poll loses the oldest rather than the
         * newest, because the newest is the one it has not seen. */
        memmove(&c->event[0], &c->event[1],
                sizeof(c->event[0]) * (TAK_NC_EVENTS_MAX - 1));
        c->event_count = TAK_NC_EVENTS_MAX - 1;
    }
    c->event[c->event_count].kind = k;
    c->event_count++;
}

/* Queue one whole frame. 0 on success. */
static int queue(TAK_NetClient *c, const uint8_t *frame, size_t len) {
    if (len == 0) { c->out_overflow = 1; return -1; }
    if (c->out_len + len > sizeof c->out) {
        /* The host is not sending, or the screen is asking faster than
         * the wire can carry. Either way, say so rather than drop a
         * message quietly: a lost JOIN_ROOM looks like a hung lobby. */
        c->out_overflow = 1;
        return -1;
    }
    memcpy(c->out + c->out_len, frame, len);
    c->out_len += len;
    return 0;
}

/* Encode straight into the out buffer, so nothing is copied twice. */
#define QUEUE_ENCODED(c, call)                                          \
    do {                                                                \
        uint8_t _f[TAK_NET_FRAME_MAX];                                  \
        size_t _n = (call);                                             \
        if (_n == 0) { (c)->out_overflow = 1; return -1; }               \
        return queue((c), _f, _n);                                      \
    } while (0)

void TAK_NetClient_Init(TAK_NetClient *c, const TAK_MsgHello *hello) {
    memset(c, 0, sizeof(*c));
    c->seat = TAK_NET_SEAT_NONE;
    c->room.room_id = 0;
    uint8_t frame[TAK_NET_FRAME_MAX];
    size_t n = TAK_Msg_HelloEncode(hello, frame, sizeof frame);
    if (n == 0) { c->state = TAK_NC_GONE; c->out_overflow = 1; return; }
    (void)queue(c, frame, n);
    c->state = TAK_NC_GREETING;
}

void TAK_NetClient_OnClose(TAK_NetClient *c) {
    if (c->state == TAK_NC_GONE) return;
    c->state = TAK_NC_GONE;
    raise_event(c, TAK_NC_EV_GONE);
}

const uint8_t *TAK_NetClient_Pending(const TAK_NetClient *c, size_t *len) {
    *len = c->out_len;
    return c->out;
}

void TAK_NetClient_Sent(TAK_NetClient *c, size_t len) {
    if (len >= c->out_len) { c->out_len = 0; return; }
    memmove(c->out, c->out + len, c->out_len - len);
    c->out_len -= len;
}

size_t TAK_NetClient_TakeMessage(TAK_NetClient *c, void *out, size_t cap) {
    if (c->out_len == 0) return 0;
    TAK_NetFrame f;
    if (TAK_Net_Split(c->out, c->out_len, &f) != 0) {
        /* Only our own encoders write here, so this cannot happen
         * without a bug above. Dropping the queue is better than
         * handing a host something it will send. */
        c->out_len = 0;
        c->out_overflow = 1;
        return 0;
    }
    size_t whole = TAK_Net_FrameSize(f.payload_len);
    if (whole > cap) return 0;
    memcpy(out, c->out, whole);
    TAK_NetClient_Sent(c, whole);
    return whole;
}

int TAK_NetClient_PollEvent(TAK_NetClient *c, TAK_NetClientEvent *out) {
    if (c->event_count == 0) return 0;
    *out = c->event[0];
    memmove(&c->event[0], &c->event[1],
            sizeof(c->event[0]) * (size_t)(c->event_count - 1));
    c->event_count--;
    return 1;
}

/* Which seat in this room state is ours, or SEAT_NONE. */
static uint8_t seat_of(const TAK_MsgRoomState *rs, uint32_t session_id) {
    if (session_id == 0) return TAK_NET_SEAT_NONE;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        if (rs->slot[i].client_id == session_id) return (uint8_t)i;
    }
    return TAK_NET_SEAT_NONE;
}

int TAK_NetClient_OnMessage(TAK_NetClient *c, const void *msg, size_t len,
                            uint64_t now_ms) {
    if (c->state == TAK_NC_GONE) return -1;
    TAK_NetFrame f;
    if (TAK_Net_Split(msg, len, &f) != 0) return -1;

    switch (f.type) {
    case TAK_MSG_WELCOME: {
        TAK_MsgWelcome w;
        if (TAK_Msg_WelcomeDecode(&w, f.payload, f.payload_len) != 0) return -1;
        c->welcome = w;
        c->session_id = w.session_id;
        c->state = TAK_NC_LOBBY;
        raise_event(c, TAK_NC_EV_WELCOMED);
        return 0;
    }

    case TAK_MSG_REJECT: {
        TAK_MsgReject r;
        if (TAK_Msg_RejectDecode(&r, f.payload, f.payload_len) != 0) return -1;
        c->reject = r;
        /* A refusal before the welcome ends the session. One after it
         * is an answer to something we asked, a room that filled up or
         * a password that did not match, and the lobby stays up. */
        if (c->state == TAK_NC_GREETING) c->state = TAK_NC_REFUSED;
        raise_event(c, TAK_NC_EV_REFUSED);
        return 0;
    }

    case TAK_MSG_PING: {
        TAK_MsgPing p;
        if (TAK_Msg_PingDecode(&p, f.payload, f.payload_len) != 0) return -1;
        /* The timestamp goes back unchanged. It is the server's clock
         * and only the server can read it, which is what makes the
         * round trip a measurement rather than two guesses. */
        uint8_t frame[TAK_NET_FRAME_MAX];
        size_t n = TAK_Msg_PingEncode(TAK_MSG_PONG, &p, frame, sizeof frame);
        return queue(c, frame, n);
    }

    case TAK_MSG_PONG: {
        TAK_MsgPong p;
        if (TAK_Msg_PingDecode(&p, f.payload, f.payload_len) != 0) return -1;
        if (now_ms > p.sent_ms) c->ping_ms = (uint32_t)(now_ms - p.sent_ms);
        return 0;
    }

    case TAK_MSG_ROOM_LIST: {
        TAK_MsgRoomList rl;
        if (TAK_Msg_RoomListDecode(&rl, f.payload, f.payload_len) != 0) return -1;
        c->rooms = rl;
        raise_event(c, TAK_NC_EV_ROOM_LIST);
        return 0;
    }

    case TAK_MSG_ROOM_STATE: {
        TAK_MsgRoomState rs;
        if (TAK_Msg_RoomStateDecode(&rs, f.payload, f.payload_len) != 0) return -1;
        /* A snapshot older than the one in hand is a reordering the
         * transport should not produce, but the revision is there so
         * the screen is never walked backwards if it does. */
        if (c->room.room_id == rs.room_id && rs.revision < c->room.revision) {
            return 0;
        }
        c->room = rs;
        c->seat = seat_of(&rs, c->session_id);
        c->state = TAK_NC_ROOM;
        raise_event(c, TAK_NC_EV_ROOM_STATE);
        return 0;
    }

    case TAK_MSG_CHAT: {
        TAK_MsgChat m;
        if (TAK_Msg_ChatDecode(&m, f.payload, f.payload_len) != 0) return -1;
        c->chat = m;
        raise_event(c, TAK_NC_EV_CHAT);
        return 0;
    }

    default:
        /* A type this build does not know is skipped, not refused.
         * Every message carries its length for exactly this, so an
         * older client can sit in a room on a newer server. */
        return 0;
    }
}

/* ── What a screen asks for ────────────────────────────────────────── */

/* Everything below refuses before the welcome, because the server
 * refuses it too and a screen finding out here is quicker. */
static int ready(const TAK_NetClient *c) {
    return c->state == TAK_NC_LOBBY || c->state == TAK_NC_ROOM;
}

int TAK_NetClient_ListRooms(TAK_NetClient *c) {
    if (!ready(c)) return -1;
    TAK_MsgListRooms m;
    memset(&m, 0, sizeof m);
    QUEUE_ENCODED(c, TAK_Msg_ListRoomsEncode(&m, _f, sizeof _f));
}

int TAK_NetClient_CreateRoom(TAK_NetClient *c, const TAK_MsgCreateRoom *m) {
    if (!ready(c)) return -1;
    QUEUE_ENCODED(c, TAK_Msg_CreateRoomEncode(m, _f, sizeof _f));
}

int TAK_NetClient_JoinRoom(TAK_NetClient *c, const TAK_MsgJoinRoom *m) {
    if (!ready(c)) return -1;
    QUEUE_ENCODED(c, TAK_Msg_JoinRoomEncode(m, _f, sizeof _f));
}

int TAK_NetClient_LeaveRoom(TAK_NetClient *c) {
    if (c->state != TAK_NC_ROOM) return -1;
    uint8_t frame[TAK_NET_FRAME_MAX];
    size_t n = TAK_Msg_EmptyEncode(TAK_MSG_LEAVE_ROOM, frame, sizeof frame);
    if (n == 0) { c->out_overflow = 1; return -1; }
    if (queue(c, frame, n) != 0) return -1;
    /* The server answers with a room list rather than a room state, so
     * the screen is told here instead of waiting to be told twice. */
    memset(&c->room, 0, sizeof c->room);
    c->seat = TAK_NET_SEAT_NONE;
    c->state = TAK_NC_LOBBY;
    raise_event(c, TAK_NC_EV_LEFT_ROOM);
    return 0;
}

int TAK_NetClient_EditRoom(TAK_NetClient *c, const TAK_MsgRoomEdit *m) {
    if (c->state != TAK_NC_ROOM) return -1;
    QUEUE_ENCODED(c, TAK_Msg_RoomEditEncode(m, _f, sizeof _f));
}

int TAK_NetClient_Chat(TAK_NetClient *c, const TAK_MsgChat *m) {
    if (!ready(c)) return -1;
    QUEUE_ENCODED(c, TAK_Msg_ChatEncode(m, _f, sizeof _f));
}

int TAK_NetClient_Start(TAK_NetClient *c) {
    if (c->state != TAK_NC_ROOM) return -1;
    uint8_t frame[TAK_NET_FRAME_MAX];
    size_t n = TAK_Msg_EmptyEncode(TAK_MSG_START, frame, sizeof frame);
    if (n == 0) { c->out_overflow = 1; return -1; }
    return queue(c, frame, n);
}
