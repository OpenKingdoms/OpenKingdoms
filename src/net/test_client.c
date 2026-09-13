/*
 * test_client.c -- the client half of a session.
 *
 * Two halves to this. The first drives the client with messages built
 * by hand, which is how the awkward cases are reached: a refusal
 * before the welcome against one after it, a room snapshot that
 * arrives out of order, a message type this build has never heard of.
 *
 * The second wires the real client to the real relay core, with
 * nothing between them but a function call. Both are transport free,
 * so they can be put in one process and asked to agree. A protocol
 * where each side is only ever tested against its author's idea of the
 * other side is a protocol with two implementations of one bug.
 */

#include "test_framework.h"

#include "tak_net_client.h"
#include "tak_net_relay.h"

#include <string.h>

static TAK_NetClient g_c;

static void fill_hello(TAK_MsgHello *h) {
    memset(h, 0, sizeof *h);
    h->protocol_version = TAK_NET_PROTOCOL_VERSION;
    h->engine_build_id = 7;
    h->determinism_class = 1;
    for (int i = 0; i < TAK_NET_TOKEN_BYTES; i++) {
        h->device_token[i] = (uint8_t)(i + 1);
    }
    memcpy(h->name, "player", 7);
}

/* Take the one message the client has queued. 0 when there is none. */
static size_t take(uint8_t *out, size_t cap) {
    return TAK_NetClient_TakeMessage(&g_c, out, cap);
}

static int next_event(TAK_NetClientEventKind *k) {
    TAK_NetClientEvent e;
    if (!TAK_NetClient_PollEvent(&g_c, &e)) return 0;
    *k = e.kind;
    return 1;
}

/* Was this event raised, in what is queued now? Drains the queue. */
static int saw_event(TAK_NetClientEventKind want) {
    TAK_NetClientEventKind k;
    int found = 0;
    while (next_event(&k)) if (k == want) found = 1;
    return found;
}

TEST(a_new_client_says_hello_first) {
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    ASSERT_EQ_INT(TAK_NC_GREETING, g_c.state);

    uint8_t msg[TAK_NET_FRAME_MAX];
    size_t n = take(msg, sizeof msg);
    ASSERT(n > 0);

    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_HELLO, f.type);
    TAK_MsgHello got;
    ASSERT_EQ_INT(0, TAK_Msg_HelloDecode(&got, f.payload, f.payload_len));
    ASSERT_EQ_INT(TAK_NET_PROTOCOL_VERSION, got.protocol_version);
    ASSERT_EQ_STR("player", got.name);
    /* And nothing else is waiting behind it. */
    ASSERT_EQ_INT(0, (int)take(msg, sizeof msg));
}

/* Bring the client to the lobby with a WELCOME built by hand. */
static int welcome_client(uint32_t session_id) {
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    uint8_t msg[TAK_NET_FRAME_MAX];
    (void)take(msg, sizeof msg);

    TAK_MsgWelcome w;
    memset(&w, 0, sizeof w);
    w.session_id = session_id;
    w.protocol_min = TAK_NET_PROTOCOL_VERSION;
    w.protocol_max = TAK_NET_PROTOCOL_VERSION;
    memcpy(w.server_name, "test relay", 11);
    size_t n = TAK_Msg_WelcomeEncode(&w, msg, sizeof msg);
    if (n == 0) return -1;
    return TAK_NetClient_OnMessage(&g_c, msg, n, 1000);
}

TEST(a_welcome_opens_the_lobby) {
    ASSERT_EQ_INT(0, welcome_client(42));
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
    ASSERT_EQ_INT(42, (int)g_c.session_id);
    ASSERT_EQ_STR("test relay", g_c.welcome.server_name);
    ASSERT(saw_event(TAK_NC_EV_WELCOMED));
}

TEST(a_refusal_before_the_welcome_ends_the_session) {
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    uint8_t msg[TAK_NET_FRAME_MAX];
    (void)take(msg, sizeof msg);

    TAK_MsgReject r;
    memset(&r, 0, sizeof r);
    r.reason = 1;
    memcpy(r.text, "no room here", 13);
    size_t n = TAK_Msg_RejectEncode(&r, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 1000));
    ASSERT_EQ_INT(TAK_NC_REFUSED, g_c.state);
    ASSERT_EQ_STR("no room here", g_c.reject.text);
    ASSERT(saw_event(TAK_NC_EV_REFUSED));
}

TEST(a_refusal_after_the_welcome_leaves_the_lobby_standing) {
    ASSERT_EQ_INT(0, welcome_client(7));
    (void)saw_event(TAK_NC_EV_WELCOMED);

    /* A room that filled up between the listing and the click. The
     * player stays where they are and reads why. */
    uint8_t msg[TAK_NET_FRAME_MAX];
    TAK_MsgReject r;
    memset(&r, 0, sizeof r);
    r.reason = 2;
    memcpy(r.text, "that room is full", 18);
    size_t n = TAK_Msg_RejectEncode(&r, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 1100));
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
    ASSERT(saw_event(TAK_NC_EV_REFUSED));
}

TEST(a_ping_comes_back_with_the_servers_own_clock) {
    ASSERT_EQ_INT(0, welcome_client(7));
    uint8_t msg[TAK_NET_FRAME_MAX];

    TAK_MsgPing p;
    p.seq = 5;
    p.sent_ms = 1234567ull;
    size_t n = TAK_Msg_PingEncode(TAK_MSG_PING, &p, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 9999));

    n = take(msg, sizeof msg);
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_PONG, f.type);
    TAK_MsgPong got;
    ASSERT_EQ_INT(0, TAK_Msg_PingDecode(&got, f.payload, f.payload_len));
    ASSERT_EQ_INT(5, (int)got.seq);
    /* Unchanged, because only the server can read its own clock. */
    ASSERT(got.sent_ms == 1234567ull);
}

TEST(a_pong_measures_the_round_trip) {
    ASSERT_EQ_INT(0, welcome_client(7));
    uint8_t msg[TAK_NET_FRAME_MAX];
    TAK_MsgPong p;
    p.seq = 1;
    p.sent_ms = 1000ull;
    size_t n = TAK_Msg_PingEncode(TAK_MSG_PONG, &p, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 1085));
    ASSERT_EQ_INT(85, (int)g_c.ping_ms);
}

TEST(a_room_snapshot_names_our_own_seat) {
    ASSERT_EQ_INT(0, welcome_client(99));
    uint8_t msg[TAK_NET_FRAME_MAX];

    TAK_MsgRoomState rs;
    memset(&rs, 0, sizeof rs);
    rs.room_id = 3;
    rs.revision = 10;
    rs.seat_count = 4;
    memcpy(rs.name, "a room", 7);
    rs.slot[0].client_id = 55;
    rs.slot[2].client_id = 99;      /* ours */
    size_t n = TAK_Msg_RoomStateEncode(&rs, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 2000));

    ASSERT_EQ_INT(TAK_NC_ROOM, g_c.state);
    ASSERT_EQ_INT(2, (int)g_c.seat);
    ASSERT_EQ_STR("a room", g_c.room.name);
    ASSERT(saw_event(TAK_NC_EV_ROOM_STATE));
}

TEST(an_older_snapshot_never_walks_the_screen_backwards) {
    ASSERT_EQ_INT(0, welcome_client(99));
    uint8_t msg[TAK_NET_FRAME_MAX];

    TAK_MsgRoomState rs;
    memset(&rs, 0, sizeof rs);
    rs.room_id = 3;
    rs.revision = 10;
    memcpy(rs.name, "current", 8);
    size_t n = TAK_Msg_RoomStateEncode(&rs, msg, sizeof msg);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 2000));

    memset(&rs, 0, sizeof rs);
    rs.room_id = 3;
    rs.revision = 9;                /* older */
    memcpy(rs.name, "stale", 6);
    n = TAK_Msg_RoomStateEncode(&rs, msg, sizeof msg);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 2100));
    ASSERT_EQ_STR("current", g_c.room.name);
    ASSERT_EQ_INT(10, (int)g_c.room.revision);
}

TEST(leaving_a_room_puts_the_player_back_at_once) {
    ASSERT_EQ_INT(0, welcome_client(99));
    uint8_t msg[TAK_NET_FRAME_MAX];
    TAK_MsgRoomState rs;
    memset(&rs, 0, sizeof rs);
    rs.room_id = 3;
    rs.revision = 1;
    rs.slot[0].client_id = 99;
    size_t n = TAK_Msg_RoomStateEncode(&rs, msg, sizeof msg);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 2000));
    ASSERT_EQ_INT(TAK_NC_ROOM, g_c.state);
    while (take(msg, sizeof msg) > 0) { }

    ASSERT_EQ_INT(0, TAK_NetClient_LeaveRoom(&g_c));
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
    ASSERT_EQ_INT(TAK_NET_SEAT_NONE, (int)g_c.seat);
    ASSERT_EQ_INT(0, (int)g_c.room.room_id);
    ASSERT(saw_event(TAK_NC_EV_LEFT_ROOM));

    n = take(msg, sizeof msg);
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_LEAVE_ROOM, f.type);
}

TEST(nothing_is_asked_for_before_the_welcome) {
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    uint8_t msg[TAK_NET_FRAME_MAX];
    (void)take(msg, sizeof msg);

    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    TAK_MsgJoinRoom jr;
    memset(&jr, 0, sizeof jr);
    TAK_MsgChat ch;
    memset(&ch, 0, sizeof ch);

    ASSERT_EQ_INT(-1, TAK_NetClient_ListRooms(&g_c));
    ASSERT_EQ_INT(-1, TAK_NetClient_CreateRoom(&g_c, &cr));
    ASSERT_EQ_INT(-1, TAK_NetClient_JoinRoom(&g_c, &jr));
    ASSERT_EQ_INT(-1, TAK_NetClient_Chat(&g_c, &ch));
    ASSERT_EQ_INT(-1, TAK_NetClient_LeaveRoom(&g_c));
    ASSERT_EQ_INT(-1, TAK_NetClient_Start(&g_c));
    /* And none of them left anything on the wire. */
    ASSERT_EQ_INT(0, (int)take(msg, sizeof msg));
}

TEST(a_message_this_build_never_heard_of_is_skipped) {
    ASSERT_EQ_INT(0, welcome_client(7));
    /* Type 200, which nothing defines. An older client has to be able
     * to sit in a room on a newer server, which is what the length on
     * every message is for. */
    uint8_t msg[64];
    memset(msg, 0, sizeof msg);
    size_t n = TAK_Msg_EmptyEncode(200, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 3000));
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
}

TEST(a_queue_that_fills_says_so_rather_than_losing_a_message) {
    ASSERT_EQ_INT(0, welcome_client(7));
    TAK_MsgChat ch;
    memset(&ch, 0, sizeof ch);
    memset(ch.text, 'x', sizeof ch.text - 1);
    int refused = 0;
    for (int i = 0; i < 4096; i++) {
        if (TAK_NetClient_Chat(&g_c, &ch) != 0) { refused = 1; break; }
    }
    ASSERT_EQ_INT(1, refused);
    ASSERT_EQ_INT(1, (int)g_c.out_overflow);
}

TEST(a_closed_transport_is_reported_once) {
    ASSERT_EQ_INT(0, welcome_client(7));
    (void)saw_event(TAK_NC_EV_WELCOMED);
    TAK_NetClient_OnClose(&g_c);
    ASSERT_EQ_INT(TAK_NC_GONE, g_c.state);
    ASSERT(saw_event(TAK_NC_EV_GONE));
    /* And again is not a second event. */
    TAK_NetClient_OnClose(&g_c);
    ASSERT_EQ_INT(0, (int)g_c.event_count);
}

/* ── The client against the real relay ─────────────────────────────── */

/* Both halves are transport free, so they go in one process with a
 * function call between them. Anything they disagree about shows up
 * here rather than on a wire at three in the morning. */

static TAK_Relay          g_relay;
static uint8_t            g_arena[TAK_RELAY_ROOMS_MAX * (64u << 10)];
static TAK_TurnLogEntry   g_entries[TAK_RELAY_ROOMS_MAX * 512u];
static uint8_t            g_to_client[TAK_NET_FRAME_MAX * 8];
static size_t             g_to_client_len;
static int                g_relay_closed;

static int relay_send(void *ctx, TAK_ConnId conn, const uint8_t *frame,
                      size_t len) {
    (void)ctx; (void)conn;
    if (g_to_client_len + len > sizeof g_to_client) return -1;
    memcpy(g_to_client + g_to_client_len, frame, len);
    g_to_client_len += len;
    return 0;
}

static void relay_close(void *ctx, TAK_ConnId conn) {
    (void)ctx; (void)conn;
    g_relay_closed = 1;
}

/* Everything the client queued goes to the relay, then everything the
 * relay queued comes back, until neither has anything to say. */
static void settle(uint64_t now) {
    for (int round = 0; round < 8; round++) {
        int moved = 0;
        uint8_t msg[TAK_NET_FRAME_MAX];
        size_t n;
        while ((n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg)) > 0) {
            TAK_Relay_OnFrame(&g_relay, 1, msg, n, now);
            moved = 1;
        }
        size_t off = 0;
        while (off < g_to_client_len) {
            size_t whole = TAK_Net_PeekLen(g_to_client + off,
                                           g_to_client_len - off);
            if (whole == 0) break;
            TAK_NetClient_OnMessage(&g_c, g_to_client + off, whole, now);
            off += whole;
            moved = 1;
        }
        g_to_client_len = 0;
        if (!moved) return;
    }
}

static void relay_up(void) {
    g_to_client_len = 0;
    g_relay_closed = 0;
    TAK_RelayCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.server_name, "loopback", 9);
    cfg.seed = 5;
    TAK_NetTransport tx;
    tx.ctx = NULL;
    tx.send = relay_send;
    tx.close = relay_close;
    TAK_Relay_Init(&g_relay, &cfg, tx, g_arena, sizeof g_arena,
                   g_entries, (uint32_t)(sizeof g_entries / sizeof g_entries[0]));
    TAK_Relay_OnConnect(&g_relay, 1, 1000);
}

TEST(the_client_and_the_relay_agree_about_a_welcome) {
    relay_up();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    settle(1000);

    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
    ASSERT(g_c.session_id != 0);
    ASSERT_EQ_STR("loopback", g_c.welcome.server_name);
    ASSERT_EQ_INT(0, g_relay_closed);
}

TEST(the_client_and_the_relay_agree_about_a_room) {
    relay_up();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    settle(1000);
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);

    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    memcpy(cr.name, "the room", 9);
    cr.max_players = 4;
    ASSERT_EQ_INT(0, TAK_NetClient_CreateRoom(&g_c, &cr));
    settle(1100);

    ASSERT_EQ_INT(TAK_NC_ROOM, g_c.state);
    ASSERT(g_c.room.room_id != 0);
    ASSERT_EQ_STR("the room", g_c.room.name);
    /* The maker of a room sits in it, and the server said which seat
     * rather than the client assuming the first one. */
    ASSERT(g_c.seat != TAK_NET_SEAT_NONE);
    ASSERT_EQ_INT((int)g_c.session_id,
                  (int)g_c.room.slot[g_c.seat].client_id);
}

TEST(the_client_and_the_relay_agree_about_leaving) {
    relay_up();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    settle(1000);
    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    memcpy(cr.name, "the room", 9);
    cr.max_players = 4;
    ASSERT_EQ_INT(0, TAK_NetClient_CreateRoom(&g_c, &cr));
    settle(1100);
    ASSERT_EQ_INT(TAK_NC_ROOM, g_c.state);

    ASSERT_EQ_INT(0, TAK_NetClient_LeaveRoom(&g_c));
    settle(1200);
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
    ASSERT_EQ_INT(0, g_relay_closed);
}

TEST(a_room_someone_is_in_is_offered_to_everyone_else) {
    relay_up();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    settle(1000);

    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    memcpy(cr.name, "listed", 7);
    /* A room only appears in the listing if it asks to. Without this
     * it is private and reachable by its six character code alone,
     * which is what the flag is for and what the first draft of this
     * case forgot. */
    cr.flags = TAK_ROOMF_LISTED;
    cr.max_players = 4;
    ASSERT_EQ_INT(0, TAK_NetClient_CreateRoom(&g_c, &cr));
    settle(1100);
    ASSERT_EQ_INT(TAK_NC_ROOM, g_c.state);

    ASSERT_EQ_INT(0, TAK_NetClient_ListRooms(&g_c));
    settle(1200);
    ASSERT(g_c.rooms.count >= 1);
    ASSERT_EQ_STR("listed", g_c.rooms.room[0].name);
}

/* The relay retires a room when the last human leaves it, so an empty
 * room is never offered to anyone. The test that sat here asserted the
 * opposite, listed a room after leaving it and expected to find it,
 * which is the client author guessing at the server rather than
 * reading it. This is what the relay does. */
TEST(a_room_nobody_is_in_is_not_offered) {
    relay_up();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    settle(1000);

    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    memcpy(cr.name, "briefly", 8);
    cr.flags = TAK_ROOMF_LISTED;    /* so its absence means retired */
    cr.max_players = 4;
    ASSERT_EQ_INT(0, TAK_NetClient_CreateRoom(&g_c, &cr));
    settle(1100);
    ASSERT_EQ_INT(0, TAK_NetClient_LeaveRoom(&g_c));
    settle(1200);

    ASSERT_EQ_INT(0, TAK_NetClient_ListRooms(&g_c));
    settle(1300);
    ASSERT_EQ_INT(0, (int)g_c.rooms.count);
}

/* ── The match ─────────────────────────────────────────────────────── */

/* Two clients, one relay, and the question lockstep exists to answer:
 * do both simulations get the same commands on the same turns? They
 * are all transport free, so the whole match runs in one process with
 * function calls between them.
 *
 * A second connection, so the room has the two humans the original
 * insists on before a game starts. */
static TAK_NetClient g_c2;
static uint8_t       g_to2[TAK_NET_FRAME_MAX * 8];
static size_t        g_to2_len;

static int relay_send2(void *ctx, TAK_ConnId conn, const uint8_t *frame,
                       size_t len) {
    (void)ctx;
    uint8_t *buf = (conn == 1) ? g_to_client : g_to2;
    size_t  *n   = (conn == 1) ? &g_to_client_len : &g_to2_len;
    size_t   cap = (conn == 1) ? sizeof g_to_client : sizeof g_to2;
    if (*n + len > cap) return -1;
    memcpy(buf + *n, frame, len);
    *n += len;
    return 0;
}

static void feed_one(TAK_NetClient *c, uint8_t *buf, size_t *len,
                     uint64_t now) {
    size_t off = 0;
    while (off < *len) {
        size_t whole = TAK_Net_PeekLen(buf + off, *len - off);
        if (whole == 0) break;
        TAK_NetClient_OnMessage(c, buf + off, whole, now);
        off += whole;
    }
    *len = 0;
}

static void settle2(uint64_t now) {
    for (int round = 0; round < 12; round++) {
        int moved = 0;
        uint8_t msg[TAK_NET_FRAME_MAX];
        size_t n;
        while ((n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg)) > 0) {
            TAK_Relay_OnFrame(&g_relay, 1, msg, n, now);
            moved = 1;
        }
        while ((n = TAK_NetClient_TakeMessage(&g_c2, msg, sizeof msg)) > 0) {
            TAK_Relay_OnFrame(&g_relay, 2, msg, n, now);
            moved = 1;
        }
        if (g_to_client_len) {
            feed_one(&g_c, g_to_client, &g_to_client_len, now);
            moved = 1;
        }
        if (g_to2_len) {
            feed_one(&g_c2, g_to2, &g_to2_len, now);
            moved = 1;
        }
        if (!moved) return;
    }
}

static void relay_up2(void) {
    g_to_client_len = 0;
    g_to2_len = 0;
    g_relay_closed = 0;
    TAK_RelayCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.server_name, "loopback", 9);
    cfg.seed = 5;
    TAK_NetTransport tx;
    tx.ctx = NULL;
    tx.send = relay_send2;
    tx.close = relay_close;
    TAK_Relay_Init(&g_relay, &cfg, tx, g_arena, sizeof g_arena,
                   g_entries,
                   (uint32_t)(sizeof g_entries / sizeof g_entries[0]));
    TAK_Relay_OnConnect(&g_relay, 1, 1000);
    TAK_Relay_OnConnect(&g_relay, 2, 1000);
}

static const uint8_t MAPFP[TAK_NET_FINGERPRINT_BYTES] = {
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
    0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01
};

/* Both clients welcomed, in one room, with a map both of them have.
 * 0 on success. */
static int two_in_a_room(void) {
    relay_up2();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    memcpy(h.name, "second", 7);
    h.device_token[0] = 0xEE;
    TAK_NetClient_Init(&g_c2, &h);
    settle2(1000);
    if (g_c.state != TAK_NC_LOBBY || g_c2.state != TAK_NC_LOBBY) return -1;

    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    memcpy(cr.name, "the match", 10);
    memcpy(cr.map_name, "two castles", 12);
    memcpy(cr.map_fingerprint, MAPFP, sizeof MAPFP);
    cr.flags = TAK_ROOMF_LISTED;
    cr.max_players = 4;
    if (TAK_NetClient_CreateRoom(&g_c, &cr) != 0) return -1;
    settle2(1100);
    if (g_c.state != TAK_NC_ROOM) return -1;

    TAK_MsgJoinRoom jr;
    memset(&jr, 0, sizeof jr);
    jr.room_id = g_c.room.room_id;
    if (TAK_NetClient_JoinRoom(&g_c2, &jr) != 0) return -1;
    settle2(1200);
    if (g_c2.state != TAK_NC_ROOM) return -1;

    /* The joiner says it has this exact map, by fingerprint. The host
     * computed the room's own and already counts as having it. */
    TAK_MsgRoomEdit ed;
    memset(&ed, 0, sizeof ed);
    ed.field = TAK_EDIT_HAVE_MAP;
    ed.seat = TAK_NET_SEAT_NONE;
    memcpy(ed.fingerprint, MAPFP, sizeof MAPFP);
    if (TAK_NetClient_EditRoom(&g_c2, &ed) != 0) return -1;
    settle2(1300);

    /* And both say they are ready, which the original wants before a
     * host can start. It toggles rather than latches, so it is sent
     * once each. */
    memset(&ed, 0, sizeof ed);
    ed.field = TAK_EDIT_READY;
    ed.seat = TAK_NET_SEAT_NONE;
    ed.value = 1;
    if (TAK_NetClient_EditRoom(&g_c, &ed) != 0) return -1;
    if (TAK_NetClient_EditRoom(&g_c2, &ed) != 0) return -1;
    settle2(1350);
    return 0;
}

TEST(two_clients_reach_a_room_together) {
    ASSERT_EQ_INT(0, two_in_a_room());
    ASSERT(g_c.room.room_id != 0);
    ASSERT_EQ_INT((int)g_c.room.room_id, (int)g_c2.room.room_id);
    ASSERT(g_c.seat != g_c2.seat);
    ASSERT_EQ_STR("two castles", g_c.room.map_name);
}

TEST(a_match_starts_and_both_worlds_are_asked_for) {
    ASSERT_EQ_INT(0, two_in_a_room());
    ASSERT_EQ_INT(0, TAK_NetClient_Start(&g_c));
    settle2(1400);

    ASSERT_EQ_INT(TAK_NC_LOADING, g_c.state);
    ASSERT_EQ_INT(TAK_NC_LOADING, g_c2.state);
    /* Both were told the same match, the same seed and the same map,
     * which is everything a world is built from. */
    ASSERT_EQ_INT((int)g_c.start.match_id, (int)g_c2.start.match_id);
    ASSERT_EQ_INT((int)g_c.start.seed, (int)g_c2.start.seed);
    ASSERT_EQ_INT(0, memcmp(g_c.start.map_fingerprint,
                            g_c2.start.map_fingerprint,
                            TAK_NET_FINGERPRINT_BYTES));
    /* And each was told its own seat, which is not the other one. */
    ASSERT(g_c.start.your_seat != g_c2.start.your_seat);
    ASSERT_EQ_INT((int)g_c.start.your_seat, (int)g_c.seat);
}

TEST(loading_progress_reaches_the_other_seat) {
    ASSERT_EQ_INT(0, two_in_a_room());
    ASSERT_EQ_INT(0, TAK_NetClient_Start(&g_c));
    settle2(1400);

    ASSERT_EQ_INT(0, TAK_NetClient_ReportLoadProgress(&g_c, 40));
    settle2(1450);
    /* The other client sees a row move, which is what the seven rows
     * on the loading screen are. */
    int found = 0;
    for (int i = 0; i < g_c2.load_state.count; i++) {
        if (g_c2.load_state.entry[i].seat == g_c.seat &&
            g_c2.load_state.entry[i].percent == 40) found = 1;
    }
    ASSERT_EQ_INT(1, found);
}

TEST(the_battle_starts_when_both_worlds_are_built) {
    ASSERT_EQ_INT(0, two_in_a_room());
    ASSERT_EQ_INT(0, TAK_NetClient_Start(&g_c));
    settle2(1400);

    ASSERT_EQ_INT(0, TAK_NetClient_ReportLoaded(&g_c, 0x1234ull));
    settle2(1500);
    /* One world is not enough. Nobody runs until everyone can. */
    ASSERT_EQ_INT(TAK_NC_LOADING, g_c.state);

    ASSERT_EQ_INT(0, TAK_NetClient_ReportLoaded(&g_c2, 0x1234ull));
    settle2(1600);
    ASSERT_EQ_INT(TAK_NC_PLAYING, g_c.state);
    ASSERT_EQ_INT(TAK_NC_PLAYING, g_c2.state);
}

/* Bring both to the first turn. 0 on success. */
static int both_playing(void) {
    if (two_in_a_room() != 0) return -1;
    if (TAK_NetClient_Start(&g_c) != 0) return -1;
    settle2(1400);
    if (TAK_NetClient_ReportLoaded(&g_c, 0x99ull) != 0) return -1;
    if (TAK_NetClient_ReportLoaded(&g_c2, 0x99ull) != 0) return -1;
    settle2(1600);
    return (g_c.state == TAK_NC_PLAYING && g_c2.state == TAK_NC_PLAYING)
           ? 0 : -1;
}

/* Does this turn carry these exact bytes, stamped with this seat? */
static int turn_carries(const TAK_NetTurn *t, const uint8_t *want, size_t len,
                        uint8_t seat) {
    for (int e = 0; e < t->entry_count; e++) {
        if (t->entry[e].seat != seat) continue;
        for (int k = 0; k < t->entry[e].count; k++) {
            if (t->entry[e].len[k] == (uint16_t)len &&
                memcmp(t->entry[e].data[k], want, len) == 0) return 1;
        }
    }
    return 0;
}

TEST(an_order_one_player_gives_reaches_both_simulations) {
    ASSERT_EQ_INT(0, both_playing());

    static const uint8_t ORDER[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    TAK_CmdBlob blob;
    blob.data = ORDER;
    blob.len = (uint16_t)sizeof ORDER;
    ASSERT_EQ_INT(0, TAK_NetClient_SendCommands(&g_c, &blob, 1));

    /* Turns close on the clock, so the relay is ticked until the turn
     * carrying that order has been sent. The seat is the one the
     * server stamped: the client never said which it was, and that is
     * the rule that stops one player forging another one's orders. */
    int seen_here = 0, seen_there = 0;
    for (uint64_t t = 1600; t <= 2600; t += 50) {
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        TAK_NetTurn turn;
        while (TAK_NetClient_TakeTurn(&g_c, &turn)) {
            if (turn_carries(&turn, ORDER, sizeof ORDER, g_c.seat)) seen_here = 1;
        }
        while (TAK_NetClient_TakeTurn(&g_c2, &turn)) {
            if (turn_carries(&turn, ORDER, sizeof ORDER, g_c.seat)) seen_there = 1;
        }
        if (seen_here && seen_there) break;
    }
    ASSERT_EQ_INT(1, seen_here);
    ASSERT_EQ_INT(1, seen_there);
}

TEST(both_clients_walk_the_same_turns_in_the_same_order) {
    ASSERT_EQ_INT(0, both_playing());
    uint32_t mine[128], theirs[128];
    int nm = 0, nt = 0;
    for (uint64_t t = 1600; t <= 2600; t += 50) {
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        TAK_NetTurn turn;
        while (nm < 128 && TAK_NetClient_TakeTurn(&g_c, &turn)) {
            mine[nm++] = turn.turn;
        }
        while (nt < 128 && TAK_NetClient_TakeTurn(&g_c2, &turn)) {
            theirs[nt++] = turn.turn;
        }
    }
    ASSERT(nm > 2);
    ASSERT_EQ_INT(nm, nt);
    for (int i = 0; i < nm; i++) {
        ASSERT_EQ_INT((int)mine[i], (int)theirs[i]);
        /* No gaps. A simulation cannot run over a hole. */
        if (i > 0) ASSERT_EQ_INT((int)mine[i - 1] + 1, (int)mine[i]);
    }
    ASSERT_EQ_INT(0, (int)g_c.turns_lost);
    ASSERT_EQ_INT(0, (int)g_c2.turns_lost);
}

TEST(nothing_is_sent_into_a_match_that_has_not_started) {
    ASSERT_EQ_INT(0, two_in_a_room());
    static const uint8_t ORDER[] = { 1, 2, 3 };
    TAK_CmdBlob blob;
    blob.data = ORDER;
    blob.len = 3;
    ASSERT_EQ_INT(-1, TAK_NetClient_SendCommands(&g_c, &blob, 1));
    ASSERT_EQ_INT(-1, TAK_NetClient_Ack(&g_c, 0, TAK_NET_NO_HASH, 0));
    ASSERT_EQ_INT(-1, TAK_NetClient_ReportLoaded(&g_c, 1));
}

TEST(an_empty_run_becomes_the_turns_it_stands_for) {
    /* Consecutive empty turns collapse into a range on the wire, so a
     * quiet match costs a few bytes a second rather than a frame each.
     * The simulation still has to see every turn. */
    ASSERT_EQ_INT(0, welcome_client(7));
    uint8_t msg[TAK_NET_FRAME_MAX];

    TAK_MsgStartGame sg;
    memset(&sg, 0, sizeof sg);
    sg.match_id = 1;
    sg.your_seat = 0;
    sg.turn_ticks = 3;
    size_t n = TAK_Msg_StartGameEncode(&sg, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 1000));
    TAK_MsgGo go;
    go.first_turn = 0;
    n = TAK_Msg_GoEncode(&go, msg, sizeof msg);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 1000));
    ASSERT_EQ_INT(TAK_NC_PLAYING, g_c.state);

    TAK_MsgTurn t;
    memset(&t, 0, sizeof t);
    t.turn = 10;
    t.empty_run = 5;        /* 10, 11, 12, 13 and 14 */
    t.entry_count = 0;
    n = TAK_Msg_TurnEncode(&t, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 1100));
    ASSERT_EQ_INT(5, (int)TAK_NetClient_TurnsHeld(&g_c));

    TAK_NetTurn out;
    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_EQ_INT(1, TAK_NetClient_TakeTurn(&g_c, &out));
        ASSERT_EQ_INT((int)(10 + i), (int)out.turn);
        ASSERT_EQ_INT(0, (int)out.entry_count);
    }
    ASSERT_EQ_INT(0, TAK_NetClient_TakeTurn(&g_c, &out));
}

int main(void) {
    TEST_SUITE("The client on its own");
    RUN(a_new_client_says_hello_first);
    RUN(a_welcome_opens_the_lobby);
    RUN(a_refusal_before_the_welcome_ends_the_session);
    RUN(a_refusal_after_the_welcome_leaves_the_lobby_standing);
    RUN(a_ping_comes_back_with_the_servers_own_clock);
    RUN(a_pong_measures_the_round_trip);
    RUN(a_room_snapshot_names_our_own_seat);
    RUN(an_older_snapshot_never_walks_the_screen_backwards);
    RUN(leaving_a_room_puts_the_player_back_at_once);
    RUN(nothing_is_asked_for_before_the_welcome);
    RUN(a_message_this_build_never_heard_of_is_skipped);
    RUN(a_queue_that_fills_says_so_rather_than_losing_a_message);
    RUN(a_closed_transport_is_reported_once);

    TEST_SUITE("The client against the real relay");
    RUN(the_client_and_the_relay_agree_about_a_welcome);
    RUN(the_client_and_the_relay_agree_about_a_room);
    RUN(the_client_and_the_relay_agree_about_leaving);
    RUN(a_room_someone_is_in_is_offered_to_everyone_else);
    RUN(a_room_nobody_is_in_is_not_offered);

    TEST_SUITE("A match between two of them");
    RUN(two_clients_reach_a_room_together);
    RUN(a_match_starts_and_both_worlds_are_asked_for);
    RUN(loading_progress_reaches_the_other_seat);
    RUN(the_battle_starts_when_both_worlds_are_built);
    RUN(an_order_one_player_gives_reaches_both_simulations);
    RUN(both_clients_walk_the_same_turns_in_the_same_order);
    RUN(nothing_is_sent_into_a_match_that_has_not_started);
    RUN(an_empty_run_becomes_the_turns_it_stands_for);
    TEST_REPORT();
}
