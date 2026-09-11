/*
 * protocol.c -- the codec between a client and the relay.
 *
 * Every decode runs off a bounded reader, allocates nothing, and refuses a
 * message that leaves trailing bytes, so a sender and this parser cannot
 * silently disagree. The bulk carriers, CMD and TURN, point into the
 * caller's frame buffer and copy no command bytes at all.
 *
 * The shape of each message is in include/tak_net_protocol.h.
 */

#include "tak_net_protocol.h"
#include "tak_bytes.h"

#include <string.h>

/* ── Framing ────────────────────────────────────────────────────────── */

size_t TAK_Net_FrameSize(size_t payload_len) {
    return TAK_NET_FRAME_HEADER + payload_len;
}

int TAK_Net_Split(const void *data, size_t len, TAK_NetFrame *out) {
    if (!data || !out || len < TAK_NET_FRAME_HEADER) return -1;
    if (len > TAK_NET_FRAME_MAX) return -1;
    const uint8_t *p = (const uint8_t *)data;
    uint8_t type = p[0];
    uint16_t plen = tak_get_u16(p + 1);
    if (type == TAK_MSG_NONE) return -1;
    if ((size_t)plen + TAK_NET_FRAME_HEADER != len) return -1;
    out->type = type;
    out->payload = plen ? p + TAK_NET_FRAME_HEADER : NULL;
    out->payload_len = plen;
    return 0;
}

static void begin(TAK_ByteWriter *w, void *out, size_t cap, uint8_t type) {
    if (cap > TAK_NET_FRAME_MAX) cap = TAK_NET_FRAME_MAX;
    TAK_BW_Init(w, out, cap);
    TAK_BW_U8(w, type);
    TAK_BW_U16(w, 0);
}

static size_t finish(TAK_ByteWriter *w) {
    if (!TAK_BW_Ok(w) || w->len < TAK_NET_FRAME_HEADER) return 0;
    size_t payload = w->len - TAK_NET_FRAME_HEADER;
    if (payload > TAK_NET_PAYLOAD_MAX) return 0;
    tak_put_u16(w->data + 1, (uint16_t)payload);
    return w->len;
}

static int done(const TAK_ByteReader *r) { return TAK_BR_Done(r) ? 0 : -1; }

size_t TAK_Msg_EmptyEncode(uint8_t type, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, type);
    return finish(&w);
}

/* ── Handshake ──────────────────────────────────────────────────────── */

size_t TAK_Msg_HelloEncode(const TAK_MsgHello *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_HELLO);
    TAK_BW_U16(&w, m->protocol_version);
    TAK_BW_U32(&w, m->engine_build_id);
    TAK_BW_U8(&w, m->determinism_class);
    TAK_BW_U8(&w, m->client_kind);
    TAK_BW_U8(&w, m->flags);
    TAK_BW_U64(&w, m->schema_hash);
    TAK_BW_U64(&w, m->content_hash);
    for (int i = 0; i < TAK_NET_GROUP_HASHES; i++)
        TAK_BW_U64(&w, m->group_hash[i]);
    TAK_BW_Bytes(&w, m->device_token, TAK_NET_TOKEN_BYTES);
    TAK_BW_Str(&w, m->name, TAK_NET_NAME_MAX);
    TAK_BW_Str(&w, m->access_key, TAK_NET_KEY_MAX);
    return finish(&w);
}

int TAK_Msg_HelloDecode(TAK_MsgHello *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->protocol_version = TAK_BR_U16(&r);
    m->engine_build_id = TAK_BR_U32(&r);
    m->determinism_class = TAK_BR_U8(&r);
    m->client_kind = TAK_BR_U8(&r);
    m->flags = TAK_BR_U8(&r);
    m->schema_hash = TAK_BR_U64(&r);
    m->content_hash = TAK_BR_U64(&r);
    for (int i = 0; i < TAK_NET_GROUP_HASHES; i++)
        m->group_hash[i] = TAK_BR_U64(&r);
    TAK_BR_Bytes(&r, m->device_token, TAK_NET_TOKEN_BYTES);
    TAK_BR_Str(&r, m->name, TAK_NET_NAME_MAX);
    TAK_BR_Str(&r, m->access_key, TAK_NET_KEY_MAX);
    return done(&r);
}

size_t TAK_Msg_WelcomeEncode(const TAK_MsgWelcome *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_WELCOME);
    TAK_BW_U32(&w, m->session_id);
    TAK_BW_U32(&w, m->flags);
    TAK_BW_U16(&w, m->protocol_min);
    TAK_BW_U16(&w, m->protocol_max);
    TAK_BW_U32(&w, m->newest_build);
    TAK_BW_Str(&w, m->server_name, TAK_NET_SERVER_NAME_MAX);
    TAK_BW_Str(&w, m->motd, TAK_NET_MOTD_MAX);
    return finish(&w);
}

int TAK_Msg_WelcomeDecode(TAK_MsgWelcome *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->session_id = TAK_BR_U32(&r);
    m->flags = TAK_BR_U32(&r);
    m->protocol_min = TAK_BR_U16(&r);
    m->protocol_max = TAK_BR_U16(&r);
    m->newest_build = TAK_BR_U32(&r);
    TAK_BR_Str(&r, m->server_name, TAK_NET_SERVER_NAME_MAX);
    TAK_BR_Str(&r, m->motd, TAK_NET_MOTD_MAX);
    return done(&r);
}

size_t TAK_Msg_RejectEncode(const TAK_MsgReject *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_REJECT);
    TAK_BW_U8(&w, m->reason);
    TAK_BW_U8(&w, m->detail);
    TAK_BW_Str(&w, m->text, TAK_NET_TEXT_MAX);
    return finish(&w);
}

int TAK_Msg_RejectDecode(TAK_MsgReject *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->reason = TAK_BR_U8(&r);
    m->detail = TAK_BR_U8(&r);
    TAK_BR_Str(&r, m->text, TAK_NET_TEXT_MAX);
    return done(&r);
}

size_t TAK_Msg_PingEncode(uint8_t type, const TAK_MsgPing *m,
                          void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, type);
    TAK_BW_U32(&w, m->seq);
    TAK_BW_U64(&w, m->sent_ms);
    return finish(&w);
}

int TAK_Msg_PingDecode(TAK_MsgPing *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->seq = TAK_BR_U32(&r);
    m->sent_ms = TAK_BR_U64(&r);
    return done(&r);
}

/* ── Lobby ──────────────────────────────────────────────────────────── */

size_t TAK_Msg_ListRoomsEncode(const TAK_MsgListRooms *m,
                               void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_LIST_ROOMS);
    TAK_BW_U32(&w, m->filter_flags);
    return finish(&w);
}

int TAK_Msg_ListRoomsDecode(TAK_MsgListRooms *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->filter_flags = TAK_BR_U32(&r);
    return done(&r);
}

static void summary_write(TAK_ByteWriter *w, const TAK_RoomSummary *s) {
    TAK_BW_U32(w, s->room_id);
    TAK_BW_Str(w, s->code, TAK_NET_CODE_MAX);
    TAK_BW_Str(w, s->name, TAK_NET_ROOM_NAME_MAX);
    TAK_BW_Str(w, s->host_name, TAK_NET_NAME_MAX);
    TAK_BW_Str(w, s->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BW_Bytes(w, s->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    TAK_BW_U8(w, s->players);
    TAK_BW_U8(w, s->max_players);
    TAK_BW_U8(w, s->watchers);
    TAK_BW_U8(w, s->status);
    TAK_BW_U32(w, s->flags);
    TAK_BW_U32(w, s->engine_build_id);
    TAK_BW_U8(w, s->determinism_class);
    TAK_BW_U8(w, s->compat);
}

static void summary_read(TAK_ByteReader *r, TAK_RoomSummary *s) {
    s->room_id = TAK_BR_U32(r);
    TAK_BR_Str(r, s->code, TAK_NET_CODE_MAX);
    TAK_BR_Str(r, s->name, TAK_NET_ROOM_NAME_MAX);
    TAK_BR_Str(r, s->host_name, TAK_NET_NAME_MAX);
    TAK_BR_Str(r, s->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BR_Bytes(r, s->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    s->players = TAK_BR_U8(r);
    s->max_players = TAK_BR_U8(r);
    s->watchers = TAK_BR_U8(r);
    s->status = TAK_BR_U8(r);
    s->flags = TAK_BR_U32(r);
    s->engine_build_id = TAK_BR_U32(r);
    s->determinism_class = TAK_BR_U8(r);
    s->compat = TAK_BR_U8(r);
}

size_t TAK_Msg_RoomListEncode(const TAK_MsgRoomList *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    if (m->count > TAK_NET_ROOMS_PER_LIST) return 0;
    begin(&w, out, cap, TAK_MSG_ROOM_LIST);
    TAK_BW_U8(&w, m->flags);
    TAK_BW_U8(&w, m->count);
    for (uint8_t i = 0; i < m->count; i++) summary_write(&w, &m->room[i]);
    return finish(&w);
}

int TAK_Msg_RoomListDecode(TAK_MsgRoomList *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->flags = TAK_BR_U8(&r);
    m->count = TAK_BR_U8(&r);
    if (m->count > TAK_NET_ROOMS_PER_LIST) return -1;
    for (uint8_t i = 0; i < m->count; i++) summary_read(&r, &m->room[i]);
    return done(&r);
}

size_t TAK_Msg_CreateRoomEncode(const TAK_MsgCreateRoom *m,
                                void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_CREATE_ROOM);
    TAK_BW_Str(&w, m->name, TAK_NET_ROOM_NAME_MAX);
    TAK_BW_Str(&w, m->password, TAK_NET_PASSWORD_MAX);
    TAK_BW_Str(&w, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BW_Bytes(&w, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    TAK_BW_U32(&w, m->flags);
    TAK_BW_U32(&w, m->options);
    TAK_BW_U8(&w, m->max_players);
    TAK_BW_U16(&w, m->unit_cap);
    TAK_BW_U16(&w, m->timeout_secs);
    return finish(&w);
}

int TAK_Msg_CreateRoomDecode(TAK_MsgCreateRoom *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    TAK_BR_Str(&r, m->name, TAK_NET_ROOM_NAME_MAX);
    TAK_BR_Str(&r, m->password, TAK_NET_PASSWORD_MAX);
    TAK_BR_Str(&r, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BR_Bytes(&r, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    m->flags = TAK_BR_U32(&r);
    m->options = TAK_BR_U32(&r);
    m->max_players = TAK_BR_U8(&r);
    m->unit_cap = TAK_BR_U16(&r);
    m->timeout_secs = TAK_BR_U16(&r);
    return done(&r);
}

size_t TAK_Msg_JoinRoomEncode(const TAK_MsgJoinRoom *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_JOIN_ROOM);
    TAK_BW_U32(&w, m->room_id);
    TAK_BW_Str(&w, m->code, TAK_NET_CODE_MAX);
    TAK_BW_Str(&w, m->password, TAK_NET_PASSWORD_MAX);
    TAK_BW_U8(&w, m->as_watcher);
    return finish(&w);
}

int TAK_Msg_JoinRoomDecode(TAK_MsgJoinRoom *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->room_id = TAK_BR_U32(&r);
    TAK_BR_Str(&r, m->code, TAK_NET_CODE_MAX);
    TAK_BR_Str(&r, m->password, TAK_NET_PASSWORD_MAX);
    m->as_watcher = TAK_BR_U8(&r);
    return done(&r);
}

size_t TAK_Msg_RoomEditEncode(const TAK_MsgRoomEdit *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_ROOM_EDIT);
    TAK_BW_U8(&w, m->field);
    TAK_BW_U8(&w, m->seat);
    TAK_BW_U32(&w, m->value);
    TAK_BW_Str(&w, m->text, TAK_NET_TEXT_MAX);
    TAK_BW_Bytes(&w, m->fingerprint, TAK_NET_FINGERPRINT_BYTES);
    return finish(&w);
}

int TAK_Msg_RoomEditDecode(TAK_MsgRoomEdit *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->field = TAK_BR_U8(&r);
    m->seat = TAK_BR_U8(&r);
    m->value = TAK_BR_U32(&r);
    TAK_BR_Str(&r, m->text, TAK_NET_TEXT_MAX);
    TAK_BR_Bytes(&r, m->fingerprint, TAK_NET_FINGERPRINT_BYTES);
    return done(&r);
}

size_t TAK_Msg_RoomStateEncode(const TAK_MsgRoomState *m,
                               void *out, size_t cap) {
    TAK_ByteWriter w;
    if (m->seat_count > TAK_NET_SEATS) return 0;
    begin(&w, out, cap, TAK_MSG_ROOM_STATE);
    TAK_BW_U32(&w, m->room_id);
    TAK_BW_U32(&w, m->revision);
    TAK_BW_Str(&w, m->code, TAK_NET_CODE_MAX);
    TAK_BW_Str(&w, m->name, TAK_NET_ROOM_NAME_MAX);
    TAK_BW_Str(&w, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BW_Bytes(&w, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    TAK_BW_U32(&w, m->host_client_id);
    TAK_BW_U32(&w, m->flags);
    TAK_BW_U32(&w, m->options);
    TAK_BW_U8(&w, m->status);
    TAK_BW_U8(&w, m->watchers);
    TAK_BW_U16(&w, m->unit_cap);
    TAK_BW_U16(&w, m->timeout_secs);
    TAK_BW_U8(&w, m->seat_count);
    for (uint8_t i = 0; i < m->seat_count; i++) {
        const TAK_NetSlot *s = &m->slot[i];
        TAK_BW_U8(&w, s->kind);
        TAK_BW_U8(&w, s->side);
        TAK_BW_U8(&w, s->colour);
        TAK_BW_U8(&w, s->team);
        TAK_BW_U8(&w, s->ready);
        TAK_BW_U8(&w, s->connected);
        TAK_BW_U8(&w, s->load_percent);
        TAK_BW_U8(&w, s->flags);
        TAK_BW_U16(&w, s->ping_ms);
        TAK_BW_U32(&w, s->client_id);
        TAK_BW_Str(&w, s->name, TAK_NET_NAME_MAX);
    }
    return finish(&w);
}

int TAK_Msg_RoomStateDecode(TAK_MsgRoomState *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->room_id = TAK_BR_U32(&r);
    m->revision = TAK_BR_U32(&r);
    TAK_BR_Str(&r, m->code, TAK_NET_CODE_MAX);
    TAK_BR_Str(&r, m->name, TAK_NET_ROOM_NAME_MAX);
    TAK_BR_Str(&r, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BR_Bytes(&r, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    m->host_client_id = TAK_BR_U32(&r);
    m->flags = TAK_BR_U32(&r);
    m->options = TAK_BR_U32(&r);
    m->status = TAK_BR_U8(&r);
    m->watchers = TAK_BR_U8(&r);
    m->unit_cap = TAK_BR_U16(&r);
    m->timeout_secs = TAK_BR_U16(&r);
    m->seat_count = TAK_BR_U8(&r);
    if (m->seat_count > TAK_NET_SEATS) return -1;
    for (uint8_t i = 0; i < m->seat_count; i++) {
        TAK_NetSlot *s = &m->slot[i];
        s->kind = TAK_BR_U8(&r);
        s->side = TAK_BR_U8(&r);
        s->colour = TAK_BR_U8(&r);
        s->team = TAK_BR_U8(&r);
        s->ready = TAK_BR_U8(&r);
        s->connected = TAK_BR_U8(&r);
        s->load_percent = TAK_BR_U8(&r);
        s->flags = TAK_BR_U8(&r);
        s->ping_ms = TAK_BR_U16(&r);
        s->client_id = TAK_BR_U32(&r);
        TAK_BR_Str(&r, s->name, TAK_NET_NAME_MAX);
    }
    return done(&r);
}

size_t TAK_Msg_ChatEncode(const TAK_MsgChat *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_CHAT);
    TAK_BW_U8(&w, m->scope);
    TAK_BW_U8(&w, m->from_seat);
    TAK_BW_U8(&w, m->to_seat);
    TAK_BW_U32(&w, m->turn);
    TAK_BW_Str(&w, m->name, TAK_NET_NAME_MAX);
    TAK_BW_Str(&w, m->text, TAK_NET_CHAT_MAX);
    return finish(&w);
}

int TAK_Msg_ChatDecode(TAK_MsgChat *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->scope = TAK_BR_U8(&r);
    m->from_seat = TAK_BR_U8(&r);
    m->to_seat = TAK_BR_U8(&r);
    m->turn = TAK_BR_U32(&r);
    TAK_BR_Str(&r, m->name, TAK_NET_NAME_MAX);
    TAK_BR_Str(&r, m->text, TAK_NET_CHAT_MAX);
    return done(&r);
}

/* ── Match start ────────────────────────────────────────────────────── */

size_t TAK_Msg_StartGameEncode(const TAK_MsgStartGame *m,
                               void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_START_GAME);
    TAK_BW_U32(&w, m->match_id);
    TAK_BW_U32(&w, m->seed);
    TAK_BW_U8(&w, m->your_seat);
    TAK_BW_U8(&w, m->turn_ticks);
    TAK_BW_U8(&w, m->flags);
    TAK_BW_U64(&w, m->schema_hash);
    TAK_BW_U64(&w, m->content_hash);
    TAK_BW_Str(&w, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BW_Bytes(&w, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    TAK_BW_U32(&w, m->options);
    TAK_BW_U16(&w, m->unit_cap);
    TAK_BW_U16(&w, m->timeout_secs);
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        TAK_BW_U8(&w, m->slot[i].kind);
        TAK_BW_U8(&w, m->slot[i].side);
        TAK_BW_U8(&w, m->slot[i].colour);
        TAK_BW_U8(&w, m->slot[i].team);
        TAK_BW_Str(&w, m->slot[i].name, TAK_NET_NAME_MAX);
    }
    return finish(&w);
}

int TAK_Msg_StartGameDecode(TAK_MsgStartGame *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->match_id = TAK_BR_U32(&r);
    m->seed = TAK_BR_U32(&r);
    m->your_seat = TAK_BR_U8(&r);
    m->turn_ticks = TAK_BR_U8(&r);
    m->flags = TAK_BR_U8(&r);
    m->schema_hash = TAK_BR_U64(&r);
    m->content_hash = TAK_BR_U64(&r);
    TAK_BR_Str(&r, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BR_Bytes(&r, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    m->options = TAK_BR_U32(&r);
    m->unit_cap = TAK_BR_U16(&r);
    m->timeout_secs = TAK_BR_U16(&r);
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        m->slot[i].kind = TAK_BR_U8(&r);
        m->slot[i].side = TAK_BR_U8(&r);
        m->slot[i].colour = TAK_BR_U8(&r);
        m->slot[i].team = TAK_BR_U8(&r);
        TAK_BR_Str(&r, m->slot[i].name, TAK_NET_NAME_MAX);
    }
    return done(&r);
}

size_t TAK_Msg_LoadProgressEncode(const TAK_MsgLoadProgress *m,
                                  void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_LOAD_PROGRESS);
    TAK_BW_U8(&w, m->percent);
    return finish(&w);
}

int TAK_Msg_LoadProgressDecode(TAK_MsgLoadProgress *m,
                               const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->percent = TAK_BR_U8(&r);
    return done(&r);
}

size_t TAK_Msg_LoadStateEncode(const TAK_MsgLoadState *m,
                               void *out, size_t cap) {
    TAK_ByteWriter w;
    if (m->count > TAK_NET_SEATS) return 0;
    begin(&w, out, cap, TAK_MSG_LOAD_STATE);
    TAK_BW_U8(&w, m->count);
    for (uint8_t i = 0; i < m->count; i++) {
        TAK_BW_U8(&w, m->entry[i].seat);
        TAK_BW_U8(&w, m->entry[i].percent);
        TAK_BW_U8(&w, m->entry[i].loaded);
    }
    return finish(&w);
}

int TAK_Msg_LoadStateDecode(TAK_MsgLoadState *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->count = TAK_BR_U8(&r);
    if (m->count > TAK_NET_SEATS) return -1;
    for (uint8_t i = 0; i < m->count; i++) {
        m->entry[i].seat = TAK_BR_U8(&r);
        m->entry[i].percent = TAK_BR_U8(&r);
        m->entry[i].loaded = TAK_BR_U8(&r);
        if (m->entry[i].seat >= TAK_NET_SEATS) return -1;
    }
    return done(&r);
}

size_t TAK_Msg_LoadedEncode(const TAK_MsgLoaded *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_LOADED);
    TAK_BW_U64(&w, m->world_hash);
    return finish(&w);
}

int TAK_Msg_LoadedDecode(TAK_MsgLoaded *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->world_hash = TAK_BR_U64(&r);
    return done(&r);
}

size_t TAK_Msg_GoEncode(const TAK_MsgGo *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_GO);
    TAK_BW_U32(&w, m->first_turn);
    return finish(&w);
}

int TAK_Msg_GoDecode(TAK_MsgGo *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->first_turn = TAK_BR_U32(&r);
    return done(&r);
}

/* ── In game ────────────────────────────────────────────────────────── */

size_t TAK_Msg_CmdEncode(const TAK_MsgCmd *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    size_t total = 0;
    if (m->count > TAK_NET_CMDS_PER_MSG) return 0;
    for (uint8_t i = 0; i < m->count; i++) {
        if (m->cmd[i].len == 0 || !m->cmd[i].data) return 0;
        total += m->cmd[i].len;
    }
    if (total > TAK_NET_CMD_BYTES_MAX) return 0;
    begin(&w, out, cap, TAK_MSG_CMD);
    TAK_BW_U32(&w, m->client_seq);
    TAK_BW_U8(&w, m->count);
    for (uint8_t i = 0; i < m->count; i++) {
        TAK_BW_U16(&w, m->cmd[i].len);
        TAK_BW_Bytes(&w, m->cmd[i].data, m->cmd[i].len);
    }
    return finish(&w);
}

int TAK_Msg_CmdDecode(TAK_MsgCmd *m, const void *p, size_t len) {
    TAK_ByteReader r;
    size_t total = 0;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->client_seq = TAK_BR_U32(&r);
    m->count = TAK_BR_U8(&r);
    if (!TAK_BR_Ok(&r) || m->count > TAK_NET_CMDS_PER_MSG) return -1;
    for (uint8_t i = 0; i < m->count; i++) {
        uint16_t n = TAK_BR_U16(&r);
        if (n == 0 || n > TAK_NET_CMD_BYTES_MAX) return -1;
        total += n;
        if (total > TAK_NET_CMD_BYTES_MAX) return -1;
        const uint8_t *at = TAK_BR_Take(&r, n);
        if (!at) return -1;
        m->cmd[i].data = at;
        m->cmd[i].len = n;
    }
    return done(&r);
}

size_t TAK_Msg_TurnEncode(const TAK_MsgTurn *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    size_t total = 0;
    int prev = -1;
    if (m->entry_count > TAK_NET_SEATS + 1) return 0;
    if (m->empty_run == 0) return 0;
    if (m->entry_count > 0 && m->empty_run != 1) return 0;
    for (uint8_t e = 0; e < m->entry_count; e++) {
        const TAK_TurnEntry *t = &m->entry[e];
        int seat = (t->seat == TAK_NET_SEAT_SERVER) ? TAK_NET_SEATS : t->seat;
        if (t->seat != TAK_NET_SEAT_SERVER && t->seat >= TAK_NET_SEATS) return 0;
        if (seat <= prev) return 0;         /* strictly increasing */
        prev = seat;
        if (t->count == 0 || t->count > TAK_NET_CMDS_PER_MSG) return 0;
        for (uint8_t i = 0; i < t->count; i++) {
            if (t->cmd[i].len == 0 || !t->cmd[i].data) return 0;
            total += t->cmd[i].len;
        }
    }
    if (total > TAK_NET_TURN_BYTES_MAX) return 0;
    begin(&w, out, cap, TAK_MSG_TURN);
    TAK_BW_U32(&w, m->turn);
    TAK_BW_U16(&w, m->empty_run);
    TAK_BW_U8(&w, m->entry_count);
    for (uint8_t e = 0; e < m->entry_count; e++) {
        const TAK_TurnEntry *t = &m->entry[e];
        TAK_BW_U8(&w, t->seat);
        TAK_BW_U8(&w, t->count);
        for (uint8_t i = 0; i < t->count; i++) {
            TAK_BW_U16(&w, t->cmd[i].len);
            TAK_BW_Bytes(&w, t->cmd[i].data, t->cmd[i].len);
        }
    }
    return finish(&w);
}

int TAK_Msg_TurnDecode(TAK_MsgTurn *m, const void *p, size_t len) {
    TAK_ByteReader r;
    size_t total = 0;
    int prev = -1;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->turn = TAK_BR_U32(&r);
    m->empty_run = TAK_BR_U16(&r);
    m->entry_count = TAK_BR_U8(&r);
    if (!TAK_BR_Ok(&r)) return -1;
    if (m->empty_run == 0) return -1;
    if (m->entry_count > TAK_NET_SEATS + 1) return -1;
    if (m->entry_count > 0 && m->empty_run != 1) return -1;
    for (uint8_t e = 0; e < m->entry_count; e++) {
        TAK_TurnEntry *t = &m->entry[e];
        t->seat = TAK_BR_U8(&r);
        t->count = TAK_BR_U8(&r);
        if (!TAK_BR_Ok(&r)) return -1;
        int seat = (t->seat == TAK_NET_SEAT_SERVER) ? TAK_NET_SEATS : t->seat;
        if (t->seat != TAK_NET_SEAT_SERVER && t->seat >= TAK_NET_SEATS) return -1;
        if (seat <= prev) return -1;
        prev = seat;
        if (t->count == 0 || t->count > TAK_NET_CMDS_PER_MSG) return -1;
        for (uint8_t i = 0; i < t->count; i++) {
            uint16_t n = TAK_BR_U16(&r);
            if (n == 0 || n > TAK_NET_CMD_BYTES_MAX) return -1;
            total += n;
            if (total > TAK_NET_TURN_BYTES_MAX) return -1;
            const uint8_t *at = TAK_BR_Take(&r, n);
            if (!at) return -1;
            t->cmd[i].data = at;
            t->cmd[i].len = n;
        }
    }
    return done(&r);
}

size_t TAK_Msg_AckEncode(const TAK_MsgAck *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_ACK);
    TAK_BW_U32(&w, m->last_turn);
    TAK_BW_U32(&w, m->hash_tick);
    TAK_BW_U64(&w, m->state_hash);
    return finish(&w);
}

int TAK_Msg_AckDecode(TAK_MsgAck *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->last_turn = TAK_BR_U32(&r);
    m->hash_tick = TAK_BR_U32(&r);
    m->state_hash = TAK_BR_U64(&r);
    return done(&r);
}

size_t TAK_Msg_PaceEncode(const TAK_MsgPace *m, void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_PACE);
    TAK_BW_U8(&w, m->speed_level);
    TAK_BW_U8(&w, m->paused);
    TAK_BW_U8(&w, m->reason);
    TAK_BW_U8(&w, m->seat);
    TAK_BW_U16(&w, m->turn_period_ms);
    return finish(&w);
}

int TAK_Msg_PaceDecode(TAK_MsgPace *m, const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->speed_level = TAK_BR_U8(&r);
    m->paused = TAK_BR_U8(&r);
    m->reason = TAK_BR_U8(&r);
    m->seat = TAK_BR_U8(&r);
    m->turn_period_ms = TAK_BR_U16(&r);
    return done(&r);
}

size_t TAK_Msg_PlayerStatusEncode(const TAK_MsgPlayerStatus *m,
                                  void *out, size_t cap) {
    TAK_ByteWriter w;
    begin(&w, out, cap, TAK_MSG_PLAYER_STATUS);
    TAK_BW_U8(&w, m->seat);
    TAK_BW_U8(&w, m->status);
    TAK_BW_U16(&w, m->countdown_secs);
    return finish(&w);
}

int TAK_Msg_PlayerStatusDecode(TAK_MsgPlayerStatus *m,
                               const void *p, size_t len) {
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->seat = TAK_BR_U8(&r);
    m->status = TAK_BR_U8(&r);
    m->countdown_secs = TAK_BR_U16(&r);
    return done(&r);
}

/* ── System commands ────────────────────────────────────────────────── */

size_t TAK_Sys_PlayerLeft(uint8_t seat, uint8_t left_as,
                          void *out, size_t cap) {
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, cap);
    TAK_BW_U8(&w, TAK_SYS_PLAYER_LEFT);
    TAK_BW_U8(&w, seat);
    TAK_BW_U8(&w, left_as);
    return TAK_BW_Ok(&w) ? TAK_BW_Len(&w) : 0;
}

size_t TAK_Sys_SeatReclaim(uint8_t seat, uint32_t client_id,
                           void *out, size_t cap) {
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, cap);
    TAK_BW_U8(&w, TAK_SYS_SEAT_RECLAIM);
    TAK_BW_U8(&w, seat);
    TAK_BW_U32(&w, client_id);
    return TAK_BW_Ok(&w) ? TAK_BW_Len(&w) : 0;
}

size_t TAK_Sys_MatchEnd(uint8_t reason, void *out, size_t cap) {
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, cap);
    TAK_BW_U8(&w, TAK_SYS_MATCH_END);
    TAK_BW_U8(&w, reason);
    return TAK_BW_Ok(&w) ? TAK_BW_Len(&w) : 0;
}

/* ── Reject text ────────────────────────────────────────────────────────
 * Plain fallback wording. A client that has the player's own string table
 * loaded shows that instead, the way every other screen resolves text. */

const char *TAK_Net_RejectText(uint8_t reason) {
    switch (reason) {
    case TAK_REJECT_GAME_CLOSED:       return "The game is closed.";
    case TAK_REJECT_WRONG_PASSWORD:    return "The password is not correct.";
    case TAK_REJECT_GAME_FULL:         return "The game is full.";
    case TAK_REJECT_LOST_CONNECTION:   return "The connection was lost.";
    case TAK_REJECT_MISSING_UNIT:      return "A unit is missing from your data.";
    case TAK_REJECT_NEEDS_NEWER:       return "A newer version is needed.";
    case TAK_REJECT_NO_WATCHING:       return "Watching is not allowed here.";
    case TAK_REJECT_CREATOR_LEFT:      return "The player who made the game has left.";
    case TAK_REJECT_DATA_MISMATCH:     return "Your game data does not match.";
    case TAK_REJECT_ENGINE_VERSION:    return "Your engine version does not match.";
    case TAK_REJECT_SERVER_PASSWORD:   return "The server key is not correct.";
    case TAK_REJECT_BANNED:            return "You are banned from this server.";
    case TAK_REJECT_RATE_LIMITED:      return "Too many requests. Try again shortly.";
    case TAK_REJECT_PROTOCOL_VERSION:  return "This server does not speak your version.";
    case TAK_REJECT_NO_SUCH_ROOM:      return "There is no game with that code.";
    case TAK_REJECT_NAME_REQUIRED:     return "You must enter your name.";
    case TAK_REJECT_DETERMINISM_CLASS: return "That game is for a different client build.";
    case TAK_REJECT_NOT_ALLOWED:       return "You are not allowed to do that.";
    case TAK_REJECT_NOT_READY:         return "Every player must be ready.";
    case TAK_REJECT_NEEDS_HUMAN:       return "Another player must join first.";
    case TAK_REJECT_ONE_TEAM:          return "Not everyone can be on one team.";
    case TAK_REJECT_MAP_MISSING:       return "A player does not have this map.";
    case TAK_REJECT_REMOVED_BY_HOST:   return "The host removed you from the game.";
    default:                           return "Unknown reason.";
    }
}

int TAK_Net_SecretEqual(const char *a, const char *b, size_t field) {
    size_t na = a ? strlen(a) : 0, nb = b ? strlen(b) : 0;
    unsigned diff = (unsigned)(na > field) | (unsigned)(nb > field);
    for (size_t i = 0; i < field; i++) {
        unsigned char ca = (i < na) ? (unsigned char)a[i] : 0u;
        unsigned char cb = (i < nb) ? (unsigned char)b[i] : 0u;
        diff |= (unsigned)(ca ^ cb);
    }
    return diff == 0;
}

/* ── One pass over an arbitrary frame ───────────────────────────────── */

int TAK_Net_Validate(const void *data, size_t len) {
    TAK_NetFrame f;
    if (TAK_Net_Split(data, len, &f) != 0) return -1;
    union {
        TAK_MsgHello hello;
        TAK_MsgWelcome welcome;
        TAK_MsgReject reject;
        TAK_MsgPing ping;
        TAK_MsgListRooms list;
        TAK_MsgRoomList rooms;
        TAK_MsgCreateRoom create;
        TAK_MsgJoinRoom join;
        TAK_MsgRoomEdit edit;
        TAK_MsgRoomState state;
        TAK_MsgChat chat;
        TAK_MsgStartGame start;
        TAK_MsgLoadProgress progress;
        TAK_MsgLoadState loadstate;
        TAK_MsgLoaded loaded;
        TAK_MsgGo go;
        TAK_MsgCmd cmd;
        TAK_MsgTurn turn;
        TAK_MsgAck ack;
        TAK_MsgPace pace;
        TAK_MsgPlayerStatus status;
    } m;
    const void *p = f.payload;
    size_t n = f.payload_len;
    switch (f.type) {
    case TAK_MSG_HELLO:         return TAK_Msg_HelloDecode(&m.hello, p, n);
    case TAK_MSG_WELCOME:       return TAK_Msg_WelcomeDecode(&m.welcome, p, n);
    case TAK_MSG_REJECT:        return TAK_Msg_RejectDecode(&m.reject, p, n);
    case TAK_MSG_PING:
    case TAK_MSG_PONG:          return TAK_Msg_PingDecode(&m.ping, p, n);
    case TAK_MSG_LIST_ROOMS:    return TAK_Msg_ListRoomsDecode(&m.list, p, n);
    case TAK_MSG_ROOM_LIST:     return TAK_Msg_RoomListDecode(&m.rooms, p, n);
    case TAK_MSG_CREATE_ROOM:   return TAK_Msg_CreateRoomDecode(&m.create, p, n);
    case TAK_MSG_JOIN_ROOM:     return TAK_Msg_JoinRoomDecode(&m.join, p, n);
    case TAK_MSG_LEAVE_ROOM:
    case TAK_MSG_START:         return n == 0 ? 0 : -1;
    case TAK_MSG_ROOM_EDIT:     return TAK_Msg_RoomEditDecode(&m.edit, p, n);
    case TAK_MSG_ROOM_STATE:    return TAK_Msg_RoomStateDecode(&m.state, p, n);
    case TAK_MSG_CHAT:          return TAK_Msg_ChatDecode(&m.chat, p, n);
    case TAK_MSG_START_GAME:    return TAK_Msg_StartGameDecode(&m.start, p, n);
    case TAK_MSG_LOAD_PROGRESS: return TAK_Msg_LoadProgressDecode(&m.progress, p, n);
    case TAK_MSG_LOAD_STATE:    return TAK_Msg_LoadStateDecode(&m.loadstate, p, n);
    case TAK_MSG_LOADED:        return TAK_Msg_LoadedDecode(&m.loaded, p, n);
    case TAK_MSG_GO:            return TAK_Msg_GoDecode(&m.go, p, n);
    case TAK_MSG_CMD:           return TAK_Msg_CmdDecode(&m.cmd, p, n);
    case TAK_MSG_TURN:          return TAK_Msg_TurnDecode(&m.turn, p, n);
    case TAK_MSG_ACK:           return TAK_Msg_AckDecode(&m.ack, p, n);
    case TAK_MSG_PACE:          return TAK_Msg_PaceDecode(&m.pace, p, n);
    case TAK_MSG_PLAYER_STATUS: return TAK_Msg_PlayerStatusDecode(&m.status, p, n);
    default:                    return -1;   /* a type we do not know */
    }
}
