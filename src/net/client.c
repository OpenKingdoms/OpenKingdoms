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
    c->status.seat = TAK_NET_SEAT_NONE;
    c->pace.seat = TAK_NET_SEAT_NONE;
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
    /* The queue holds several frames back to back, so the length is
     * read from the header rather than by splitting: TAK_Net_Split
     * refuses a buffer that is longer than the frame in it, which is
     * right for one message and wrong for a queue. */
    size_t whole = TAK_Net_PeekLen(c->out, c->out_len);
    if (whole == 0) {
        /* Only our own encoders write here, so a frame that does not
         * parse is a bug above rather than a peer. */
        c->out_len = 0;
        c->out_overflow = 1;
        return 0;
    }
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

static void turns_reset(TAK_NetClient *c);
static int  hold_turn_run(TAK_NetClient *c, const TAK_MsgTurn *m);

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
        /* Once a match is under way the seat comes from START_GAME and
         * the snapshot must not move it. The server keeps sending room
         * states through the load, for the rows and the ping column. */
        if (c->state != TAK_NC_LOADING && c->state != TAK_NC_PLAYING) {
            c->seat = seat_of(&rs, c->session_id);
            c->state = TAK_NC_ROOM;
        }
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

    case TAK_MSG_START_GAME: {
        TAK_MsgStartGame m;
        if (TAK_Msg_StartGameDecode(&m, f.payload, f.payload_len) != 0) return -1;
        c->start = m;
        /* The server names the seat here too, and this one is the one
         * the simulation uses, so it wins over anything the room
         * snapshot said. */
        c->seat = m.your_seat;
        turns_reset(c);
        c->next_turn = 0;
        c->last_turn_held = 0;
        c->cmd_seq = 0;
        c->state = TAK_NC_LOADING;
        raise_event(c, TAK_NC_EV_START_GAME);
        return 0;
    }

    case TAK_MSG_LOAD_STATE: {
        TAK_MsgLoadState m;
        if (TAK_Msg_LoadStateDecode(&m, f.payload, f.payload_len) != 0) return -1;
        c->load_state = m;
        raise_event(c, TAK_NC_EV_LOAD_STATE);
        return 0;
    }

    case TAK_MSG_GO: {
        TAK_MsgGo m;
        if (TAK_Msg_GoDecode(&m, f.payload, f.payload_len) != 0) return -1;
        c->next_turn = m.first_turn;
        c->state = TAK_NC_PLAYING;
        raise_event(c, TAK_NC_EV_GO);
        return 0;
    }

    case TAK_MSG_TURN: {
        TAK_MsgTurn m;
        if (TAK_Msg_TurnDecode(&m, f.payload, f.payload_len) != 0) return -1;
        /* A turn before GO is one the server sent as the match opened.
         * Holding it is right: GO says which turn to start at and the
         * ring is already in order. */
        if (c->state != TAK_NC_PLAYING && c->state != TAK_NC_LOADING) return 0;
        if (hold_turn_run(c, &m) != 0) return -1;
        raise_event(c, TAK_NC_EV_TURN);
        return 0;
    }

    case TAK_MSG_PACE: {
        TAK_MsgPace m;
        if (TAK_Msg_PaceDecode(&m, f.payload, f.payload_len) != 0) return -1;
        c->pace = m;
        raise_event(c, TAK_NC_EV_PACE);
        return 0;
    }

    case TAK_MSG_PLAYER_STATUS: {
        TAK_MsgPlayerStatus m;
        if (TAK_Msg_PlayerStatusDecode(&m, f.payload, f.payload_len) != 0) {
            return -1;
        }
        c->status = m;
        raise_event(c, TAK_NC_EV_PLAYER_STATUS);
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

/* The fields that change your own row rather than the room. The server
 * refuses one of these unless it names the seat the sender sits in, so
 * the client fills that in and a screen never has to know. The other
 * fields are the host changing the room, where the seat names whoever
 * is being kicked or blocked and is the caller to give. */
static int own_row_field(uint8_t field) {
    switch (field) {
    case TAK_EDIT_SIDE:
    case TAK_EDIT_COLOUR:
    case TAK_EDIT_TEAM:
    case TAK_EDIT_NAME:
    case TAK_EDIT_WATCH:
    case TAK_EDIT_READY:
    case TAK_EDIT_HAVE_MAP:
        return 1;
    default:
        return 0;
    }
}

int TAK_NetClient_EditRoom(TAK_NetClient *c, const TAK_MsgRoomEdit *m) {
    if (c->state != TAK_NC_ROOM) return -1;
    TAK_MsgRoomEdit e = *m;
    if (own_row_field(e.field)) {
        if (c->seat == TAK_NET_SEAT_NONE) return -1;
        e.seat = c->seat;
    }
    QUEUE_ENCODED(c, TAK_Msg_RoomEditEncode(&e, _f, sizeof _f));
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

/* ── The match ─────────────────────────────────────────────────────── */

/* The turn ring. Commands are copied into one arena and the entries
 * point into it, so a turn is freed by taking it and the whole thing
 * is one allocation that never grows.
 *
 * Turns are taken strictly in order, so the arena empties from the
 * front. When it is empty it resets to the start, which is what keeps
 * a match that runs for an hour inside a fixed buffer. */

static void turns_reset(TAK_NetClient *c) {
    c->held_head = 0;
    c->held_count = 0;
    c->cmd_count = 0;
    c->arena_len = 0;
    c->turns_lost = 0;
}

static int hold_turn(TAK_NetClient *c, const TAK_MsgTurn *m) {
    if (c->held_count >= TAK_NC_TURNS_MAX) { c->turns_lost = 1; return -1; }

    /* Room for the commands first, so a turn is either whole or not
     * held at all. A half held turn is a hole in the stream, and a
     * simulation cannot run over a hole. */
    uint32_t need_cmds = 0, need_bytes = 0;
    for (int e = 0; e < m->entry_count; e++) {
        need_cmds += m->entry[e].count;
        for (int k = 0; k < m->entry[e].count; k++) {
            need_bytes += m->entry[e].cmd[k].len;
        }
    }
    if (c->cmd_count + need_cmds > (uint32_t)(TAK_NC_TURNS_MAX * 8) ||
        c->arena_len + need_bytes > TAK_NC_TURN_ARENA) {
        /* Nothing has been taken for a while and the buffer is full.
         * Reclaiming the front would only help if turns were being
         * taken, so this is the honest end of the road. */
        c->turns_lost = 1;
        return -1;
    }

    uint32_t slot = (c->held_head + c->held_count) % TAK_NC_TURNS_MAX;
    c->held[slot].turn = m->turn;
    c->held[slot].entry_count = m->entry_count;
    c->held[slot].first_cmd = c->cmd_count;
    for (int e = 0; e < m->entry_count; e++) {
        c->held[slot].seat[e] = m->entry[e].seat;
        c->held[slot].count[e] = m->entry[e].count;
        for (int k = 0; k < m->entry[e].count; k++) {
            uint16_t len = m->entry[e].cmd[k].len;
            c->cmd_len[c->cmd_count] = len;
            c->cmd_off[c->cmd_count] = c->arena_len;
            if (len > 0 && m->entry[e].cmd[k].data) {
                memcpy(c->arena + c->arena_len, m->entry[e].cmd[k].data, len);
            }
            c->arena_len += len;
            c->cmd_count++;
        }
    }
    c->held_count++;
    c->last_turn_held = m->turn;
    return 0;
}

/* An empty run says this turn and the next run-1 carry nothing, which
 * is how a quiet match costs a few bytes a second instead of a frame
 * each. They are expanded here so the simulation sees every turn. */
static int hold_turn_run(TAK_NetClient *c, const TAK_MsgTurn *m) {
    if (hold_turn(c, m) != 0) return -1;
    uint16_t run = m->empty_run;
    if (run <= 1) return 0;
    TAK_MsgTurn empty;
    memset(&empty, 0, sizeof empty);
    for (uint16_t i = 1; i < run; i++) {
        empty.turn = m->turn + i;
        empty.entry_count = 0;
        if (hold_turn(c, &empty) != 0) return -1;
    }
    return 0;
}

uint32_t TAK_NetClient_TurnsHeld(const TAK_NetClient *c) {
    return c->held_count;
}

int TAK_NetClient_TakeTurn(TAK_NetClient *c, TAK_NetTurn *out) {
    if (c->held_count == 0) return 0;
    uint32_t slot = c->held_head;
    out->turn = c->held[slot].turn;
    out->entry_count = c->held[slot].entry_count;
    uint32_t ci = c->held[slot].first_cmd;
    for (int e = 0; e < out->entry_count; e++) {
        out->entry[e].seat = c->held[slot].seat[e];
        out->entry[e].count = c->held[slot].count[e];
        for (int k = 0; k < out->entry[e].count; k++) {
            out->entry[e].len[k] = c->cmd_len[ci];
            out->entry[e].data[k] = c->arena + c->cmd_off[ci];
            ci++;
        }
    }
    c->held_head = (c->held_head + 1) % TAK_NC_TURNS_MAX;
    c->held_count--;
    c->next_turn = out->turn + 1;
    /* Everything held has been taken, so the arena starts again. The
     * caller reads the bytes before its next call, which is the same
     * contract the connection pump makes. */
    if (c->held_count == 0) {
        c->cmd_count = 0;
        c->arena_len = 0;
    }
    return 1;
}

int TAK_NetClient_ReportLoadProgress(TAK_NetClient *c, uint8_t percent) {
    if (c->state != TAK_NC_LOADING) return -1;
    TAK_MsgLoadProgress m;
    m.percent = percent;
    QUEUE_ENCODED(c, TAK_Msg_LoadProgressEncode(&m, _f, sizeof _f));
}

int TAK_NetClient_ReportLoaded(TAK_NetClient *c, uint64_t world_hash) {
    if (c->state != TAK_NC_LOADING) return -1;
    TAK_MsgLoaded m;
    m.world_hash = world_hash;
    QUEUE_ENCODED(c, TAK_Msg_LoadedEncode(&m, _f, sizeof _f));
}

int TAK_NetClient_SendCommands(TAK_NetClient *c, const TAK_CmdBlob *cmd,
                               int count) {
    if (c->state != TAK_NC_PLAYING) return -1;
    if (count < 0 || count > TAK_NET_CMDS_PER_MSG) return -1;
    TAK_MsgCmd m;
    memset(&m, 0, sizeof m);
    /* The sequence number is ours and rises. It is not what orders the
     * commands, the turn is, but it lets the server see a gap. */
    m.client_seq = ++c->cmd_seq;
    m.count = (uint8_t)count;
    for (int i = 0; i < count; i++) m.cmd[i] = cmd[i];
    QUEUE_ENCODED(c, TAK_Msg_CmdEncode(&m, _f, sizeof _f));
}

int TAK_NetClient_Ack(TAK_NetClient *c, uint32_t last_turn,
                      uint32_t hash_tick, uint64_t state_hash) {
    if (c->state != TAK_NC_PLAYING) return -1;
    TAK_MsgAck m;
    m.last_turn = last_turn;
    m.hash_tick = hash_tick;
    m.state_hash = state_hash;
    QUEUE_ENCODED(c, TAK_Msg_AckEncode(&m, _f, sizeof _f));
}
