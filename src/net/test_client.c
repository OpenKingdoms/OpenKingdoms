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
#include "tak_net_ledger.h"
#include "tak_net_player.h"
#include "tak_net_match.h"
#include "tak_command_queue.h"

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

/* A client measures its own round trip: it pings on its own clock,
 * the relay sends that straight back, and the answer sets ping_ms. Once
 * a heartbeat, and nothing before the welcome (#295). */
TEST(a_client_measures_its_own_ping_to_the_relay) {
    relay_up();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    TAK_NetClient_Heartbeat(&g_c, 1000);        /* still greeting */
    uint8_t msg[TAK_NET_FRAME_MAX];
    size_t n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_HELLO, f.type);
    ASSERT_EQ_INT(0, (int)TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg));
    TAK_Relay_OnFrame(&g_relay, 1, msg, n, 1000);
    settle(1000);
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
    ASSERT_EQ_INT(0, (int)g_c.ping_ms);

    /* Out at 2000, through the relay at 2020, back at 2045. */
    TAK_NetClient_Heartbeat(&g_c, 2000);
    n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg);
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_PING, f.type);
    g_to_client_len = 0;
    TAK_Relay_OnFrame(&g_relay, 1, msg, n, 2020);
    ASSERT(g_to_client_len > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(g_to_client, g_to_client_len, &f));
    ASSERT_EQ_INT(TAK_MSG_PONG, f.type);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, g_to_client, g_to_client_len, 2045));
    g_to_client_len = 0;
    ASSERT_EQ_INT(45, (int)g_c.ping_ms);

    /* Not again until the next heartbeat. */
    TAK_NetClient_Heartbeat(&g_c, 2000 + TAK_NC_PING_MS - 1);
    ASSERT_EQ_INT(0, (int)TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg));
    TAK_NetClient_Heartbeat(&g_c, 2000 + TAK_NC_PING_MS);
    n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg);
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_PING, f.type);
    /* An answer in the same millisecond still reads as measured. */
    TAK_Relay_OnFrame(&g_relay, 1, msg, n, 4000);
    settle(4000);
    ASSERT_EQ_INT(1, (int)g_c.ping_ms);
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

/* The room list carries the host's ping, measured by the relay's own
 * heartbeat, and a client in the lobby is sent the list again with each
 * heartbeat so the number stays current (#295). */
TEST(the_room_list_carries_the_hosts_ping) {
    relay_up2();
    TAK_MsgHello h;
    fill_hello(&h);
    TAK_NetClient_Init(&g_c, &h);
    memcpy(h.name, "second", 7);
    h.device_token[0] = 0xEE;
    TAK_NetClient_Init(&g_c2, &h);
    settle2(1000);
    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    memcpy(cr.name, "the match", 10);
    memcpy(cr.map_name, "two castles", 12);
    memcpy(cr.map_fingerprint, MAPFP, sizeof MAPFP);
    cr.flags = TAK_ROOMF_LISTED;
    cr.max_players = 4;
    ASSERT_EQ_INT(0, TAK_NetClient_CreateRoom(&g_c, &cr));
    settle2(1100);
    ASSERT_EQ_INT(1, g_c2.rooms.count);
    ASSERT_EQ_INT(0, g_c2.rooms.room[0].host_ping_ms);   /* not measured yet */

    /* The relay's heartbeat pings the host, whose answer takes 70 ms. */
    TAK_Relay_Tick(&g_relay, 3000);
    feed_one(&g_c, g_to_client, &g_to_client_len, 3000);
    feed_one(&g_c2, g_to2, &g_to2_len, 3000);
    uint8_t msg[TAK_NET_FRAME_MAX];
    size_t n;
    while ((n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg)) > 0)
        TAK_Relay_OnFrame(&g_relay, 1, msg, n, 3070);
    settle2(3070);

    /* The next heartbeat brings the lobby the list with it. */
    TAK_Relay_Tick(&g_relay, 5000);
    settle2(5000);
    ASSERT_EQ_INT(1, g_c2.rooms.count);
    ASSERT_EQ_INT(70, g_c2.rooms.room[0].host_ping_ms);
    /* The same number the battle room's Ping column shows. */
    ASSERT_EQ_INT(70, (int)g_c.room.slot[g_c.seat].ping_ms);
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

/* ── The simulation on the server's turns ──────────────────────────── */

/* The queue applies a command by calling into the engine, and these
 * cases are about when a command runs rather than what it does to a
 * unit. This stands in for that one call so the file stays data free
 * and runs in CI. Anything about what an order does to units is
 * test_command_pipeline's, which links the real engine. */
static int g_applied;
/* The commands applied, in order, for the cases that look at them. */
#define APPLIED_KEEP 8192
static struct { uint8_t type, seat; uint16_t arg; uint32_t tick; } g_seen[APPLIED_KEEP];
int TAK_CommandExec_Apply(const TAK_GameCommand *cmd) {
    if (g_applied < APPLIED_KEEP) {
        g_seen[g_applied].type = cmd->type;
        g_seen[g_applied].seat = cmd->seat;
        g_seen[g_applied].arg = cmd->arg;
        g_seen[g_applied].tick = cmd->tick;
    }
    g_applied++;
    return 0;
}

/* The queue and the match module together, with the real relay behind
 * them. What matters here is not that a command arrived, which the
 * client tests already say, but that it arrived on a tick and that the
 * simulation could not run past the turns it held. */

static TAK_GameCommand mlt_order(uint32_t unit_id, int32_t x, int32_t y) {
    TAK_GameCommand c;
    memset(&c, 0, sizeof c);
    c.type = TAK_CMD_MOVE;
    c.target_x = x;
    c.target_y = y;
    c.unit_count = 1;
    c.unit_ids[0] = unit_id;
    return c;
}

TEST(the_simulation_cannot_run_past_the_turns_it_holds) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);

    /* No turns taken yet, so no tick may run. A client that ran here
     * would be simulating a future nobody has agreed. */
    ASSERT_EQ_INT(0, (int)TAK_Match_TickLimit());
    ASSERT_EQ_INT(0, TAK_Match_CanAdvance());

    TAK_Relay_Tick(&g_relay, 1650);
    settle2(1650);
    int taken = TAK_Match_Pump();
    ASSERT(taken > 0);

    /* Now exactly as far as the turns reach, and not one tick more. */
    uint32_t limit = TAK_Match_TickLimit();
    ASSERT_EQ_INT((int)(taken * g_c.start.turn_ticks), (int)limit);
    for (uint32_t t = 0; t < limit; t++) {
        ASSERT_EQ_INT(1, TAK_Match_CanAdvance());
        (void)TAK_CmdQueue_Run();
    }
    ASSERT_EQ_INT((int)limit, (int)TAK_CmdQueue_Tick());
    ASSERT_EQ_INT(0, TAK_Match_CanAdvance());

    TAK_Match_End();
}

TEST(an_order_lands_on_the_tick_its_turn_owns) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);

    TAK_GameCommand order = mlt_order(4242, 1234, 5678);
    /* The wire form has to survive a round trip before anything else
     * here can mean much. */
    uint8_t probe[512];
    size_t plen = 0, pused = 0;
    ASSERT_EQ_INT(0, TAK_CommandSerialize(&order, probe, sizeof probe, &plen));
    ASSERT(plen > 0);
    TAK_GameCommand back;
    ASSERT_EQ_INT(0, TAK_CommandDeserialize(&back, probe, plen, &pused));
    ASSERT_EQ_INT((int)plen, (int)pused);
    ASSERT_EQ_INT((int)order.unit_ids[0], (int)back.unit_ids[0]);

    ASSERT_EQ_INT(0, TAK_Match_SubmitLocal(&order));
    /* Nothing is in the local queue: it went to the server and comes
     * back in a turn like everyone else's. */
    ASSERT_EQ_INT(0, TAK_CmdQueue_Pending());

    int found_tick = -1;
    for (uint64_t t = 1600; t <= 2600 && found_tick < 0; t += 50) {
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        TAK_Match_Pump();
        /* The queue holds it against a tick. Which tick is the turn's
         * to decide, and every client is told the same one. */
        uint32_t limit = TAK_Match_TickLimit();
        while (TAK_CmdQueue_Tick() < limit && found_tick < 0) {
            uint32_t tick = TAK_CmdQueue_Tick();
            int ran = TAK_CmdQueue_Run();
            if (ran > 0) found_tick = (int)tick;
        }
    }
    ASSERT(found_tick >= 0);
    /* A turn covers turn_ticks ticks, so the tick an order runs on is
     * the first of its turn and never a tick in the middle. */
    ASSERT_EQ_INT(0, found_tick % g_c.start.turn_ticks);

    TAK_Match_End();
}

TEST(both_clients_are_given_the_same_command_on_the_same_tick) {
    ASSERT_EQ_INT(0, both_playing());

    /* One order from the first client, and both sides asked where it
     * landed. This is the whole of lockstep as a single assertion. */
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);
    TAK_GameCommand order = mlt_order(777, 64, 96);
    ASSERT_EQ_INT(0, TAK_Match_SubmitLocal(&order));

    uint32_t mine = 0xffffffffu;
    for (uint64_t t = 1600; t <= 2600 && mine == 0xffffffffu; t += 50) {
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        TAK_Match_Pump();
        while (TAK_CmdQueue_Tick() < TAK_Match_TickLimit()) {
            uint32_t tick = TAK_CmdQueue_Tick();
            if (TAK_CmdQueue_Run() > 0) { mine = tick; break; }
        }
    }
    ASSERT(mine != 0xffffffffu);
    TAK_Match_End();

    /* The second client, from its own turn stream, with its own queue. */
    TAK_Match_Begin(&g_c2, g_c2.seat, g_c2.start.turn_ticks);
    uint32_t theirs = 0xffffffffu;
    for (uint64_t t = 1600; t <= 2600 && theirs == 0xffffffffu; t += 50) {
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        TAK_Match_Pump();
        while (TAK_CmdQueue_Tick() < TAK_Match_TickLimit()) {
            uint32_t tick = TAK_CmdQueue_Tick();
            if (TAK_CmdQueue_Run() > 0) { theirs = tick; break; }
        }
    }
    ASSERT(theirs != 0xffffffffu);
    ASSERT_EQ_INT((int)mine, (int)theirs);
    TAK_Match_End();
}

/* What a client's queue ran, in order. */
static TAK_GameCommand g_ran[8];
static int g_ran_n;

static void mlt_seen(const TAK_GameCommand *cmd, void *user) {
    (void)user;
    if (g_ran_n < 8) g_ran[g_ran_n++] = *cmd;
}

/* Run the live client's turns until its queue has run `want` commands. */
static void mlt_run_until(int want) {
    g_ran_n = 0;
    TAK_CmdQueue_SetObserver(mlt_seen, NULL);
    for (uint64_t t = 1600; t <= 2600 && g_ran_n < want; t += 50) {
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        TAK_Match_Pump();
        while (TAK_CmdQueue_Tick() < TAK_Match_TickLimit()) TAK_CmdQueue_Run();
    }
    TAK_CmdQueue_SetObserver(NULL, NULL);
}

/* An order for a big army crosses whole, and a formation of the most
 * units a command holds brings every unit's point with it. */
TEST(an_order_for_a_whole_army_crosses_the_relay_whole) {
    ASSERT_EQ_INT(0, both_playing());
    static TAK_GameCommand army, form;
    army = mlt_order(1, 700, 900);
    army.unit_count = 200;
    for (int i = 0; i < 200; i++) army.unit_ids[i] = 5000u + (unsigned)i;
    memset(&form, 0, sizeof form);
    form.type = TAK_CMD_MOVE_FORMATION;
    form.target_x = 1200;
    form.target_y = 1300;
    form.build_type_id = 16384;
    form.arg = TAK_FORMATION_FACE | TAK_FORMATION_GROUP_PACE;
    form.unit_count = TAK_COMMAND_MAX_UNITS;
    for (int i = 0; i < TAK_COMMAND_MAX_UNITS; i++) {
        form.unit_ids[i] = 9000u + (unsigned)i;
        form.unit_dx[i] = (int16_t)((i % 16) * 40 - 300);
        form.unit_dy[i] = (int16_t)((i / 16) * -40);
    }

    for (int side = 0; side < 2; side++) {
        TAK_NetClient *c = side ? &g_c2 : &g_c;
        TAK_Match_Begin(c, c->seat, c->start.turn_ticks);
        if (side == 0) {
            ASSERT_EQ_INT(0, TAK_Match_SubmitLocal(&army));
            ASSERT_EQ_INT(0, TAK_Match_SubmitLocal(&form));
        }
        mlt_run_until(2);
        ASSERT_EQ_INT(2, g_ran_n);
        ASSERT_EQ_INT(TAK_CMD_MOVE, (int)g_ran[0].type);
        ASSERT_EQ_INT(200, (int)g_ran[0].unit_count);
        ASSERT_EQ_INT(5199, (int)g_ran[0].unit_ids[199]);
        const TAK_GameCommand *f = &g_ran[1];
        ASSERT_EQ_INT(TAK_CMD_MOVE_FORMATION, (int)f->type);
        ASSERT_EQ_INT(16384, (int)f->build_type_id);
        ASSERT_EQ_INT((int)form.arg, (int)f->arg);
        ASSERT_EQ_INT(TAK_COMMAND_MAX_UNITS, (int)f->unit_count);
        for (int i = 0; i < TAK_COMMAND_MAX_UNITS; i++) {
            ASSERT_EQ_INT((int)form.unit_ids[i], (int)f->unit_ids[i]);
            ASSERT_EQ_INT(form.unit_dx[i], f->unit_dx[i]);
            ASSERT_EQ_INT(form.unit_dy[i], f->unit_dy[i]);
        }
        TAK_Match_End();
    }
}

/* A move for a thousand units is eight commands of 128, more than the
 * relay takes from a seat in a turn. They wait on the sender and go a
 * share a turn, so every one reaches both machines, in order, and none
 * is refused on the way. */
TEST(a_move_bigger_than_a_turn_goes_over_several_and_loses_nothing) {
    ASSERT_EQ_INT(0, both_playing());
    static TAK_GameCommand part;
    for (int side = 0; side < 2; side++) {
        TAK_NetClient *c = side ? &g_c2 : &g_c;
        TAK_Match_Begin(c, c->seat, c->start.turn_ticks);
        if (side == 0) {
            for (int k = 0; k < 8; k++) {
                memset(&part, 0, sizeof part);
                part.type = TAK_CMD_MOVE_FORMATION;
                part.target_x = 1000 + k;
                part.target_y = 2000;
                part.target_unit_id = 77;
                part.unit_count = TAK_FORMATION_CHUNK;
                for (int i = 0; i < TAK_FORMATION_CHUNK; i++) {
                    part.unit_ids[i] = 20000u + (unsigned)(k * TAK_FORMATION_CHUNK + i);
                    part.unit_dx[i] = (int16_t)(i * 3);
                    part.unit_dy[i] = (int16_t)k;
                }
                ASSERT_EQ_INT(0, TAK_Match_SubmitLocal(&part));
            }
            ASSERT_EQ_INT(8, TAK_Match_Unsent());
        }
        g_ran_n = 0;
        TAK_CmdQueue_SetObserver(mlt_seen, NULL);
        for (uint64_t t = 1600; t <= 4600 && g_ran_n < 8; t += 50) {
            TAK_Relay_Tick(&g_relay, t);
            settle2(t);
            TAK_Match_Pump();
            while (TAK_CmdQueue_Tick() < TAK_Match_TickLimit()) TAK_CmdQueue_Run();
        }
        TAK_CmdQueue_SetObserver(NULL, NULL);
        ASSERT_EQ_INT(8, g_ran_n);
        ASSERT_EQ_INT(0, TAK_Match_Unsent());
        for (int k = 0; k < 8; k++) {
            ASSERT_EQ_INT(1000 + k, g_ran[k].target_x);
            ASSERT_EQ_INT(TAK_FORMATION_CHUNK, (int)g_ran[k].unit_count);
            ASSERT_EQ_INT((int)(20000u + (unsigned)(k * TAK_FORMATION_CHUNK + 127)),
                          (int)g_ran[k].unit_ids[127]);
            ASSERT_EQ_INT(127 * 3, g_ran[k].unit_dx[127]);
        }
        TAK_Match_End();
    }
}

/* A battle says whom its turns are waiting on: the seat the server's
 * pace names, and one it has lost with the seconds it still gives it.
 * The server sent both and nothing showed either (#295). */
TEST(a_battle_says_whom_it_is_waiting_for) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);
    char line[96];
    ASSERT_EQ_INT(TAK_MATCH_FLOWING, TAK_Match_Waiting(line, sizeof line));
    uint8_t other = g_c2.seat;
    ASSERT(other < TAK_NET_SEATS && other != g_c.seat);
    snprintf(g_c.start.slot[other].name, sizeof g_c.start.slot[other].name, "Zach");

    /* The clock stopped for a seat. */
    g_c.pace.reason = TAK_PACE_WAITING_FOR_PLAYER;
    g_c.pace.paused = 1;
    g_c.pace.seat = other;
    ASSERT_EQ_INT(TAK_MATCH_STALLED, TAK_Match_Waiting(line, sizeof line));
    ASSERT(strcmp(line, "Waiting for Zach") == 0);

    g_c.status.seat = other;
    g_c.status.status = TAK_PSTATUS_LOST;
    g_c.status.countdown_secs = 25;
    ASSERT_EQ_INT(TAK_MATCH_STALLED, TAK_Match_Waiting(line, sizeof line));
    ASSERT(strcmp(line, "Waiting for Zach, 25 s") == 0);

    /* Back, and nobody is waited for. */
    g_c.status.status = TAK_PSTATUS_CONNECTED;
    g_c.pace.reason = TAK_PACE_NORMAL;
    g_c.pace.paused = 0;
    g_c.pace.seat = TAK_NET_SEAT_NONE;
    ASSERT_EQ_INT(TAK_MATCH_FLOWING, TAK_Match_Waiting(line, sizeof line));
    TAK_Match_End();
}

/* The turns still come but slower, because the server's governor holds
 * them back for players who lag: the seat the pace names first, then
 * every other one marked lagging. Our own seat is never named (#295). */
TEST(a_battle_says_whom_it_is_slowing_down_for) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);
    char line[96];
    uint8_t other = g_c2.seat;
    uint8_t a = 0, b = 0;
    while (a == g_c.seat || a == other) a++;
    b = (uint8_t)(a + 1);
    while (b == g_c.seat || b == other) b++;
    snprintf(g_c.start.slot[other].name, sizeof g_c.start.slot[other].name, "Zach");
    snprintf(g_c.start.slot[a].name, sizeof g_c.start.slot[a].name, "Ann");
    snprintf(g_c.start.slot[b].name, sizeof g_c.start.slot[b].name, "Bo");

    g_c.pace.reason = TAK_PACE_WAITING_FOR_PLAYER;
    g_c.pace.paused = 0;
    g_c.pace.seat = other;
    ASSERT_EQ_INT(TAK_MATCH_SLOWED, TAK_Match_Waiting(line, sizeof line));
    ASSERT_EQ_STR("Slowing down to wait for Zach", line);

    g_c.seat_status[other] = TAK_PSTATUS_LAGGING;   /* named once */
    g_c.seat_status[a] = TAK_PSTATUS_LAGGING;
    ASSERT_EQ_INT(TAK_MATCH_SLOWED, TAK_Match_Waiting(line, sizeof line));
    ASSERT_EQ_STR("Slowing down to wait for Zach and Ann", line);
    g_c.seat_status[b] = TAK_PSTATUS_LAGGING;
    g_c.seat_status[g_c.seat] = TAK_PSTATUS_LAGGING;
    ASSERT_EQ_INT(TAK_MATCH_SLOWED, TAK_Match_Waiting(line, sizeof line));
    if (a < b) ASSERT_EQ_STR("Slowing down to wait for Zach, Ann and Bo", line);

    /* A stall outranks a lag. */
    g_c.pace.paused = 1;
    ASSERT_EQ_INT(TAK_MATCH_STALLED, TAK_Match_Waiting(line, sizeof line));
    ASSERT_EQ_STR("Waiting for Zach", line);

    /* The pace naming us is not a lag anyone else caused. */
    g_c.pace.paused = 0;
    g_c.pace.seat = g_c.seat;
    memset(g_c.seat_status, TAK_PSTATUS_CONNECTED, sizeof g_c.seat_status);
    g_c.seat_status[g_c.seat] = TAK_PSTATUS_LAGGING;
    ASSERT_EQ_INT(TAK_MATCH_FLOWING, TAK_Match_Waiting(line, sizeof line));
    ASSERT_EQ_STR("", line);

    /* And a small buffer is cut, never overrun. */
    g_c.pace.seat = other;
    char small[12];
    ASSERT_EQ_INT(TAK_MATCH_SLOWED, TAK_Match_Waiting(small, sizeof small));
    ASSERT_EQ_INT(11, (int)strlen(small));
    TAK_Match_End();
}

TEST(a_finished_tick_is_acknowledged_and_hashed_on_the_sixtieth) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);
    TAK_Relay_Tick(&g_relay, 1650);
    settle2(1650);
    TAK_Match_Pump();

    uint8_t msg[TAK_NET_FRAME_MAX];
    while (TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg) > 0) { }

    /* An ordinary tick acknowledges the turn and carries no hash. */
    TAK_Match_TickDone(7, 0xAAAA);
    size_t n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg);
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_ACK, f.type);
    TAK_MsgAck a;
    ASSERT_EQ_INT(0, TAK_Msg_AckDecode(&a, f.payload, f.payload_len));
    ASSERT_EQ_INT((int)TAK_NET_NO_HASH, (int)a.hash_tick);

    /* The sixtieth carries one, which is what the server compares
     * between clients to catch a desync. */
    TAK_Match_TickDone(60, 0x1234BEEF);
    n = TAK_NetClient_TakeMessage(&g_c, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(msg, n, &f));
    ASSERT_EQ_INT(TAK_MSG_ACK, f.type);
    ASSERT_EQ_INT(0, TAK_Msg_AckDecode(&a, f.payload, f.payload_len));
    ASSERT_EQ_INT(60, (int)a.hash_tick);
    ASSERT(a.state_hash == 0x1234BEEFull);

    TAK_Match_End();
}

/* ── The verdict, for the leaderboard ──────────────────────────────── */

static TAK_Ledger g_ledger;

/* Twenty turns, sixty ticks, delivered. */
#define PLAYED_MS  2600
#define PLAYED_TICKS 60

static void fill_result(TAK_MsgMatchResult *m, uint8_t winner, uint8_t loser) {
    memset(m, 0, sizeof *m);
    m->end_tick = 30;
    m->count = 2;
    m->entry[0].seat = winner;
    m->entry[0].standing = 1;
    m->entry[0].units_built = 40;
    m->entry[0].kills = 12;
    m->entry[0].losses = 3;
    m->entry[0].score = 7992;
    m->entry[0].last_alive_tick = 30;
    m->entry[1].seat = loser;
    m->entry[1].standing = 0;
    m->entry[1].eliminated = 1;
    m->entry[1].units_built = 25;
    m->entry[1].kills = 3;
    m->entry[1].losses = 12;
    m->entry[1].score = 1998;
    m->entry[1].last_alive_tick = 25;
}

/* Ack every turn held by one client, as a simulation would. */
static void ack_turns(TAK_NetClient *c) {
    static TAK_NetTurn turn;
    uint32_t last = 0;
    int got = 0;
    while (TAK_NetClient_TakeTurn(c, &turn)) { last = turn.turn; got = 1; }
    if (got) (void)TAK_NetClient_Ack(c, last, TAK_NET_NO_HASH, 0);
}

/* Play on until `now`: the clock closes a turn every 50 ms and both
 * clients keep up, so a verdict has ticks to fall on. */
static void play_until(uint64_t now) {
    for (uint64_t t = g_relay.now + 50; t <= now; t += 50) {
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        ack_turns(&g_c);
        ack_turns(&g_c2);
        settle2(t);
    }
}

/* The same through the real relay: a player that stops simulating is
 * marked lagging for everyone else, the battle says so, and the line
 * goes once they catch up. */
TEST(a_player_who_falls_behind_is_named_until_they_catch_up) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);
    char line[96];
    uint64_t t = g_relay.now;
    for (int i = 0; i < 80; i++) {          /* four seconds, c2 idle */
        t += 50;
        TAK_Relay_Tick(&g_relay, t);
        settle2(t);
        ack_turns(&g_c);
        settle2(t);
    }
    ASSERT_EQ_INT(TAK_PSTATUS_LAGGING, g_c.seat_status[g_c2.seat]);
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, g_c.seat_status[g_c.seat]);
    ASSERT_EQ_INT(TAK_MATCH_SLOWED, TAK_Match_Waiting(line, sizeof line));
    ASSERT_EQ_STR("Slowing down to wait for second", line);

    play_until(t + 3000);
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, g_c.seat_status[g_c2.seat]);
    ASSERT_EQ_INT(TAK_MATCH_FLOWING, TAK_Match_Waiting(line, sizeof line));
    TAK_Match_End();
}

static int take_type(TAK_NetClient *c, uint8_t want, TAK_MsgMatchResult *out) {
    uint8_t msg[TAK_NET_FRAME_MAX];
    size_t n;
    int found = 0;
    while ((n = TAK_NetClient_TakeMessage(c, msg, sizeof msg)) > 0) {
        TAK_NetFrame f;
        if (TAK_Net_Split(msg, n, &f) != 0 || f.type != want) continue;
        if (out && TAK_Msg_MatchResultDecode(out, f.payload, f.payload_len) != 0) continue;
        found++;
    }
    return found;
}

TEST(a_verdict_is_reported_once_and_only_while_playing) {
    TAK_MsgMatchResult m;
    fill_result(&m, 0, 1);
    /* Outside a match there is nobody to tell. */
    TAK_Match_End();
    ASSERT_EQ_INT(-1, TAK_Match_ReportResult(&m));
    ASSERT_EQ_INT(0, TAK_Match_Reported());

    ASSERT_EQ_INT(0, both_playing());
    TAK_Match_Begin(&g_c, g_c.seat, g_c.start.turn_ticks);
    (void)take_type(&g_c, TAK_MSG_MATCH_RESULT, NULL);
    ASSERT_EQ_INT(0, TAK_Match_ReportResult(&m));
    ASSERT_EQ_INT(1, TAK_Match_Reported());
    TAK_MsgMatchResult sent;
    ASSERT_EQ_INT(1, take_type(&g_c, TAK_MSG_MATCH_RESULT, &sent));
    /* Stamped with the match the server named, and the tally set. */
    ASSERT_EQ_INT((int)g_c.start.match_id, (int)sent.match_id);
    ASSERT_EQ_INT(TAK_NET_STATS_VERSION, sent.stats_version);
    ASSERT_EQ_INT(2, sent.count);
    ASSERT_EQ_INT(7992, sent.entry[0].score);
    /* The rules fire once, and so does this, whoever asks again. */
    ASSERT_EQ_INT(-1, TAK_Match_ReportResult(&m));
    ASSERT_EQ_INT(0, take_type(&g_c, TAK_MSG_MATCH_RESULT, NULL));
    TAK_Match_End();
    /* A client that is not playing refuses at its own level too. */
    g_c.state = TAK_NC_ROOM;
    ASSERT_EQ_INT(-1, TAK_NetClient_ReportMatchResult(&g_c, &m));
}

TEST(a_reported_verdict_is_recorded_and_the_other_seat_confirms_it) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Ledger_Init(&g_ledger);
    TAK_Relay_SetLedger(&g_relay, &g_ledger);
    play_until(PLAYED_MS);
    TAK_MsgMatchResult m;
    fill_result(&m, g_c.seat, g_c2.seat);
    m.match_id = g_c.start.match_id;
    m.stats_version = TAK_NET_STATS_VERSION;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(2700);

    ASSERT_EQ_INT(1, (int)g_ledger.count);
    ASSERT_EQ_INT(1, (int)g_relay.results_recorded);
    const TAK_LedgerMatch *rec = TAK_Ledger_Find(&g_ledger, 1);
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ_INT((int)g_c.start.match_id, (int)rec->relay_match_id);
    ASSERT_EQ_STR("two castles", rec->map_name);
    ASSERT_EQ_INT(0, memcmp(rec->map_fingerprint, MAPFP, sizeof MAPFP));
    ASSERT_EQ_INT(30, (int)rec->end_tick);
    ASSERT_EQ_INT(2, rec->seat_count);
    ASSERT_EQ_INT(1, rec->reports);
    /* The room's own view of who sat where, joined to the tallies. */
    const TAK_LedgerSeat *w = NULL, *l = NULL;
    for (int i = 0; i < rec->seat_count; i++) {
        if (rec->seat[i].seat == g_c.seat) w = &rec->seat[i];
        if (rec->seat[i].seat == g_c2.seat) l = &rec->seat[i];
    }
    ASSERT_NOT_NULL(w);
    ASSERT_NOT_NULL(l);
    ASSERT_EQ_STR("player", w->name);
    ASSERT_EQ_STR("second", l->name);
    /* Each seat is the device that sat in it, not the name it typed. */
    TAK_MsgHello h1, h2;
    fill_hello(&h1);
    fill_hello(&h2);
    h2.device_token[0] = 0xEE;
    ASSERT_EQ_INT(TAK_LEDGER_IDENT_DEVICE, w->ident);
    ASSERT(w->player_id == TAK_Player_FromToken(h1.device_token));
    ASSERT(l->player_id == TAK_Player_FromToken(h2.device_token));
    ASSERT(w->player_id != TAK_Ledger_PlayerId("player"));
    ASSERT_EQ_INT(TAK_NSLOT_HUMAN, w->kind);
    ASSERT_EQ_INT(1, w->place);
    ASSERT_EQ_INT(TAK_LEDGER_WON, w->result);
    ASSERT_EQ_INT(2, l->place);
    ASSERT_EQ_INT(TAK_LEDGER_LOST, l->result);
    ASSERT_EQ_INT(1, l->eliminated);
    ASSERT_EQ_INT(7992, w->score);
    ASSERT_EQ_INT(12, l->losses);
    ASSERT_EQ_INT(25, l->last_alive_tick);
    /* Time is virtual here, so the stamps are the relay's own clock. */
    ASSERT(rec->started_ms >= 1000 && rec->started_ms <= 1700);
    ASSERT_EQ_INT(2700, (int)rec->ended_ms);

    /* The other seat says the same and is counted, not recorded twice. */
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c2, &m));
    settle2(2800);
    ASSERT_EQ_INT(1, (int)g_ledger.count);
    ASSERT_EQ_INT(2, rec->reports);
    ASSERT_EQ_INT(0, rec->disputed);
    /* A seat that reports twice is refused the second time. */
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c2, &m));
    settle2(2900);
    ASSERT_EQ_INT(2, rec->reports);
    ASSERT_EQ_INT(1, (int)g_relay.results_refused);
    TAK_Relay_SetLedger(&g_relay, NULL);
}

TEST(a_report_that_disagrees_marks_the_game_disputed) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Ledger_Init(&g_ledger);
    TAK_Relay_SetLedger(&g_relay, &g_ledger);
    play_until(PLAYED_MS);
    TAK_MsgMatchResult m;
    fill_result(&m, g_c.seat, g_c2.seat);
    m.match_id = g_c.start.match_id;
    m.stats_version = TAK_NET_STATS_VERSION;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(2700);
    m.entry[0].kills += 1;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c2, &m));
    settle2(2800);
    const TAK_LedgerMatch *rec = TAK_Ledger_Find(&g_ledger, 1);
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ_INT(1, rec->reports);
    ASSERT_EQ_INT(1, rec->disputed);
    /* The first report stands. */
    ASSERT_EQ_INT(12, rec->seat[rec->seat[0].seat == g_c.seat ? 0 : 1].kills);
    TAK_Relay_SetLedger(&g_relay, NULL);
}

TEST(a_report_for_the_wrong_match_or_tally_set_is_refused) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Ledger_Init(&g_ledger);
    TAK_Relay_SetLedger(&g_relay, &g_ledger);
    play_until(PLAYED_MS);
    TAK_MsgMatchResult m;
    fill_result(&m, g_c.seat, g_c2.seat);
    m.match_id = g_c.start.match_id + 1;
    m.stats_version = TAK_NET_STATS_VERSION;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(2700);
    ASSERT_EQ_INT(0, (int)g_ledger.count);
    ASSERT_EQ_INT(1, (int)g_relay.results_refused);
    /* A tally set this relay does not know is refused too. */
    m.match_id = g_c.start.match_id;
    m.stats_version = TAK_NET_STATS_VERSION + 1;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(2750);
    ASSERT_EQ_INT(0, (int)g_ledger.count);
    ASSERT_EQ_INT(2, (int)g_relay.results_refused);
    /* And the right one afterwards still counts: a refusal is not a
     * strike against the seat. */
    m.stats_version = TAK_NET_STATS_VERSION;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(2800);
    ASSERT_EQ_INT(1, (int)g_ledger.count);
    TAK_Relay_SetLedger(&g_relay, NULL);
}

/* The name is the player's identity on the board, so the relay trims
 * it and refuses one that is nothing but blanks. */
TEST(a_blank_name_is_refused_and_a_padded_one_is_trimmed) {
    relay_up();
    TAK_MsgHello h;
    fill_hello(&h);
    memcpy(h.name, "   ", 4);
    TAK_NetClient_Init(&g_c, &h);
    settle(1000);
    ASSERT_EQ_INT(TAK_NC_REFUSED, g_c.state);
    ASSERT_EQ_INT(TAK_REJECT_NAME_REQUIRED, g_c.reject.reason);

    relay_up();
    fill_hello(&h);
    memcpy(h.name, "  Zach \t", 9);
    TAK_NetClient_Init(&g_c, &h);
    settle(1000);
    ASSERT_EQ_INT(TAK_NC_LOBBY, g_c.state);
    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    memcpy(cr.name, "the match", 10);
    cr.max_players = 2;
    ASSERT_EQ_INT(0, TAK_NetClient_CreateRoom(&g_c, &cr));
    settle(1100);
    ASSERT_EQ_INT(TAK_NC_ROOM, g_c.state);
    ASSERT_EQ_STR("Zach", g_c.room.slot[0].name);
}

/* A verdict cannot fall on a tick nobody has been given. A client that
 * claims one has forged it, and the relay knows: it closed the turns. */
TEST(a_verdict_beyond_the_turns_delivered_is_refused) {
    ASSERT_EQ_INT(0, both_playing());
    TAK_Ledger_Init(&g_ledger);
    TAK_Relay_SetLedger(&g_relay, &g_ledger);
    TAK_MsgMatchResult m;
    fill_result(&m, g_c.seat, g_c2.seat);
    m.match_id = g_c.start.match_id;
    m.stats_version = TAK_NET_STATS_VERSION;
    /* No turn has closed yet, so tick 30 has not happened anywhere. */
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(1700);
    ASSERT_EQ_INT(0, (int)g_ledger.count);
    ASSERT_EQ_INT(1, (int)g_relay.results_refused);
    /* Twenty turns later it has, and one past them still has not. */
    play_until(PLAYED_MS);
    m.end_tick = PLAYED_TICKS + 1;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(2700);
    ASSERT_EQ_INT(0, (int)g_ledger.count);
    ASSERT_EQ_INT(2, (int)g_relay.results_refused);
    m.end_tick = PLAYED_TICKS;
    ASSERT_EQ_INT(0, TAK_NetClient_ReportMatchResult(&g_c, &m));
    settle2(2800);
    ASSERT_EQ_INT(1, (int)g_ledger.count);
    ASSERT_EQ_INT(PLAYED_TICKS, (int)TAK_Ledger_Find(&g_ledger, 1)->end_tick);
    TAK_Relay_SetLedger(&g_relay, NULL);
}

/* Outside a match nothing changes: the queue runs at zero delay and
 * the simulation is never held back, which is how one body of code
 * serves a skirmish and a match. */
TEST(outside_a_match_the_simulation_is_never_held_back) {
    TAK_Match_End();
    ASSERT_EQ_INT(0, TAK_Match_IsLive());
    ASSERT_EQ_INT(1, TAK_Match_CanAdvance());
    ASSERT_EQ_INT(TAK_NET_SEAT_NONE, (int)TAK_Match_Seat());
    /* And a local order is refused rather than sent nowhere. */
    TAK_GameCommand order = mlt_order(1, 2, 3);
    ASSERT_EQ_INT(-1, TAK_Match_SubmitLocal(&order));
}


/* ── Drop in and rejoin: the whole log at once (#292) ──────────────── */

/* A client welcomed, told to build a world and told to go. */
static int playing_client(void) {
    if (welcome_client(7) != 0) return -1;
    uint8_t msg[TAK_NET_FRAME_MAX];
    TAK_MsgStartGame sg;
    memset(&sg, 0, sizeof sg);
    sg.match_id = 1;
    sg.your_seat = 0;
    sg.turn_ticks = 3;
    size_t n = TAK_Msg_StartGameEncode(&sg, msg, sizeof msg);
    if (!n || TAK_NetClient_OnMessage(&g_c, msg, n, 1000) != 0) return -1;
    TAK_MsgGo go;
    go.first_turn = 0;
    n = TAK_Msg_GoEncode(&go, msg, sizeof msg);
    if (!n || TAK_NetClient_OnMessage(&g_c, msg, n, 1000) != 0) return -1;
    return g_c.state == TAK_NC_PLAYING ? 0 : -1;
}

/* One turn into the client: `cmds` commands from `seat`, each four bytes
 * naming its turn, or an empty run of `run` when cmds is 0. */
static int feed_turn(uint32_t turn, uint8_t seat, int cmds, uint16_t run) {
    static uint8_t msg[TAK_NET_FRAME_MAX];
    static uint8_t body[TAK_NET_CMDS_PER_MSG][4];
    static TAK_MsgTurn t;
    memset(&t, 0, sizeof t);
    t.turn = turn;
    t.empty_run = cmds ? 1 : run;
    if (cmds) {
        t.entry_count = 1;
        t.entry[0].seat = seat;
        t.entry[0].count = (uint8_t)cmds;
        for (int k = 0; k < cmds; k++) {
            body[k][0] = (uint8_t)turn;
            body[k][1] = (uint8_t)(turn >> 8);
            body[k][2] = (uint8_t)(turn >> 16);
            body[k][3] = (uint8_t)k;
            t.entry[0].cmd[k].data = body[k];
            t.entry[0].cmd[k].len = 4;
        }
    }
    size_t n = TAK_Msg_TurnEncode(&t, msg, sizeof msg);
    return n ? TAK_NetClient_OnMessage(&g_c, msg, n, 1100) : -1;
}

/* Take one turn and check it is the one expected, commands and all. */
static int take_checked(uint32_t want, int cmds) {
    TAK_NetTurn out;
    if (!TAK_NetClient_TakeTurn(&g_c, &out) || out.turn != want) return -1;
    if (!cmds) return out.entry_count == 0 ? 0 : -1;
    if (out.entry_count != 1 || out.entry[0].count != cmds) return -1;
    for (int k = 0; k < cmds; k++) {
        const uint8_t *b = out.entry[0].data[k];
        if (out.entry[0].len[k] != 4 || b[0] != (uint8_t)want ||
            b[1] != (uint8_t)(want >> 8) || b[2] != (uint8_t)(want >> 16) ||
            b[3] != (uint8_t)k) return -1;
    }
    return 0;
}

/* A player who rejoins or drops in is sent the whole log while the world
 * is still loading and nothing is taken. Twenty minutes of it, a busy
 * turn every five and an empty run between, is held whole and handed
 * out turn by turn. It used to hold 256 turns, thirteen seconds. */
TEST(twenty_minutes_of_turn_log_is_held_while_the_world_loads) {
    ASSERT_EQ_INT(0, playing_client());
    const uint32_t turns = 24000;
    for (uint32_t t = 0; t < turns; t += 5) {
        ASSERT_EQ_INT(0, feed_turn(t, 1, 3, 1));
        ASSERT_EQ_INT(0, feed_turn(t + 1, 0, 0, 4));
    }
    ASSERT_EQ_INT(0, g_c.turns_lost);
    ASSERT_EQ_INT((int)turns, (int)TAK_NetClient_TurnsHeld(&g_c));
    for (uint32_t t = 0; t < turns; t++)
        if (take_checked(t, t % 5 == 0 ? 3 : 0) != 0) {
            printf("\n    turn %u came back wrong\n", (unsigned)t);
            ASSERT(0);
        }
    TAK_NetTurn out;
    ASSERT_EQ_INT(0, TAK_NetClient_TakeTurn(&g_c, &out));
    ASSERT_EQ_INT(0, (int)TAK_NetClient_TurnsHeld(&g_c));
}

/* Catching up while new turns keep arriving, the ring never empties.
 * The space the taken turns held is reused rather than run out of. */
TEST(a_ring_that_never_empties_reuses_the_space_of_taken_turns) {
    ASSERT_EQ_INT(0, playing_client());
    uint32_t next_in = 0, next_out = 0;
    /* Always three hundred turns ahead, each carrying the most commands
     * a turn may, so the command arrays fill and are reused many times
     * over. */
    for (; next_in < 300; next_in++)
        ASSERT_EQ_INT(0, feed_turn(next_in, 2, TAK_NET_CMDS_PER_MSG, 1));
    for (int round = 0; round < 4000; round++) {
        for (int k = 0; k < 3; k++, next_in++)
            ASSERT_EQ_INT(0, feed_turn(next_in, 2, TAK_NET_CMDS_PER_MSG, 1));
        for (int k = 0; k < 3; k++, next_out++)
            if (take_checked(next_out, TAK_NET_CMDS_PER_MSG) != 0) {
                printf("\n    turn %u came back wrong\n", (unsigned)next_out);
                ASSERT(0);
            }
        ASSERT(g_c.held_count > 0);
    }
    while (next_out < next_in) {
        ASSERT_EQ_INT(0, take_checked(next_out, TAK_NET_CMDS_PER_MSG));
        next_out++;
    }
    ASSERT_EQ_INT(0, g_c.turns_lost);
    ASSERT_EQ_INT((int)next_in, (int)next_out);
}

/* The match puts the log into the command queue a couple of seconds at
 * a time, so a catch up never overflows it, and nothing is lost. */
TEST(a_catch_up_feeds_the_queue_without_overflowing_it) {
    ASSERT_EQ_INT(0, playing_client());
    g_applied = 0;
    TAK_Match_Begin(&g_c, 0, 3);
    TAK_GameCommand cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = TAK_CMD_STOP;
    uint8_t blob[64];
    size_t used = 0;
    ASSERT_EQ_INT(0, TAK_CommandSerialize(&cmd, blob, sizeof blob, &used));
    static uint8_t msg[TAK_NET_FRAME_MAX];
    static TAK_MsgTurn t;
    const uint32_t turns = 3000;
    for (uint32_t n = 0; n < turns; n++) {
        memset(&t, 0, sizeof t);
        t.turn = n;
        t.empty_run = 1;
        t.entry_count = 1;
        t.entry[0].seat = 1;
        t.entry[0].count = 4;
        for (int k = 0; k < 4; k++) {
            t.entry[0].cmd[k].data = blob;
            t.entry[0].cmd[k].len = (uint16_t)used;
        }
        size_t len = TAK_Msg_TurnEncode(&t, msg, sizeof msg);
        ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, len, 1100));
    }
    ASSERT((int)TAK_Match_TicksBehind() == (int)(turns * 3));
    int most = 0;
    for (uint32_t tick = 0; tick < turns * 3; tick++) {
        (void)TAK_Match_Pump();
        if (TAK_CmdQueue_Pending() > most) most = TAK_CmdQueue_Pending();
        ASSERT(TAK_Match_CanAdvance());
        TAK_CmdQueue_Run();
    }
    ASSERT_EQ_INT((int)(turns * 4), g_applied);
    ASSERT(most < TAK_CMD_QUEUE_MAX);
    ASSERT_EQ_INT(0, (int)TAK_Match_TicksBehind());
    TAK_Match_End();
}

/* The relay's own entries become the command that hands a seat over, on
 * their turn's tick and after that turn's orders, and a player who sends
 * one of those itself is ignored the same way everywhere. */
TEST(the_relays_seat_entries_become_seat_commands_on_their_tick) {
    ASSERT_EQ_INT(0, playing_client());
    g_applied = 0;
    TAK_Match_Begin(&g_c, 0, 3);
    TAK_GameCommand forged, move;
    memset(&forged, 0, sizeof forged);
    forged.type = TAK_CMD_SEAT_CONTROL;
    forged.arg = TAK_SEAT_TO_HUMAN;
    memset(&move, 0, sizeof move);
    move.type = TAK_CMD_MOVE;
    move.unit_count = 1;
    move.unit_ids[0] = 9;
    uint8_t fb[64], mb[64], left[8], take[8], back[8];
    size_t fl = 0, ml = 0;
    ASSERT_EQ_INT(0, TAK_CommandSerialize(&forged, fb, sizeof fb, &fl));
    ASSERT_EQ_INT(0, TAK_CommandSerialize(&move, mb, sizeof mb, &ml));
    size_t ll = TAK_Sys_PlayerLeft(2, TAK_LEFT_COMPUTER_TAKES_OVER, left, sizeof left);
    size_t tl = TAK_Sys_SeatTakeover(3, 0x77, take, sizeof take);
    size_t rl = TAK_Sys_PlayerLeft(4, TAK_LEFT_ARMY_REMOVED, back, sizeof back);

    static uint8_t msg[TAK_NET_FRAME_MAX];
    static TAK_MsgTurn t;
    memset(&t, 0, sizeof t);
    t.turn = 0;
    t.empty_run = 1;
    ASSERT(TAK_Msg_TurnEncode(&t, msg, sizeof msg) > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, TAK_Msg_TurnEncode(&t, msg, sizeof msg), 1100));
    t.turn = 1;
    t.entry_count = 2;
    t.entry[0].seat = 1;
    t.entry[0].count = 2;
    t.entry[0].cmd[0].data = fb; t.entry[0].cmd[0].len = (uint16_t)fl;
    t.entry[0].cmd[1].data = mb; t.entry[0].cmd[1].len = (uint16_t)ml;
    t.entry[1].seat = TAK_NET_SEAT_SERVER;
    t.entry[1].count = 3;
    t.entry[1].cmd[0].data = left; t.entry[1].cmd[0].len = (uint16_t)ll;
    t.entry[1].cmd[1].data = take; t.entry[1].cmd[1].len = (uint16_t)tl;
    t.entry[1].cmd[2].data = back; t.entry[1].cmd[2].len = (uint16_t)rl;
    size_t n = TAK_Msg_TurnEncode(&t, msg, sizeof msg);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_NetClient_OnMessage(&g_c, msg, n, 1100));
    ASSERT_EQ_INT(2, TAK_Match_Pump());
    while (TAK_Match_CanAdvance()) TAK_CmdQueue_Run();

    /* The move, then the three hand overs by seat. The forgery is gone. */
    ASSERT_EQ_INT(4, g_applied);
    int seat_cmds = 0;
    for (int i = 0; i < g_applied; i++) {
        ASSERT_EQ_INT(3, (int)g_seen[i].tick);
        if (g_seen[i].type == TAK_CMD_MOVE) { ASSERT_EQ_INT(2, g_seen[i].seat); continue; }
        ASSERT_EQ_INT(TAK_CMD_SEAT_CONTROL, g_seen[i].type);
        seat_cmds++;
        if (g_seen[i].seat == 3) ASSERT_EQ_INT(TAK_SEAT_TO_COMPUTER, g_seen[i].arg);
        else if (g_seen[i].seat == 4) ASSERT_EQ_INT(TAK_SEAT_TO_HUMAN, g_seen[i].arg);
        else if (g_seen[i].seat == 5) ASSERT_EQ_INT(TAK_SEAT_ARMY_REMOVED, g_seen[i].arg);
        else ASSERT(0);
    }
    ASSERT_EQ_INT(3, seat_cmds);
    TAK_Match_End();
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
    RUN(a_client_measures_its_own_ping_to_the_relay);
    RUN(the_client_and_the_relay_agree_about_a_room);
    RUN(the_client_and_the_relay_agree_about_leaving);
    RUN(a_room_someone_is_in_is_offered_to_everyone_else);
    RUN(a_room_nobody_is_in_is_not_offered);

    TEST_SUITE("A match between two of them");
    RUN(two_clients_reach_a_room_together);
    RUN(the_room_list_carries_the_hosts_ping);
    RUN(a_match_starts_and_both_worlds_are_asked_for);
    RUN(loading_progress_reaches_the_other_seat);
    RUN(the_battle_starts_when_both_worlds_are_built);
    RUN(an_order_one_player_gives_reaches_both_simulations);
    RUN(both_clients_walk_the_same_turns_in_the_same_order);
    RUN(nothing_is_sent_into_a_match_that_has_not_started);
    RUN(an_empty_run_becomes_the_turns_it_stands_for);

    TEST_SUITE("The simulation on the server's turns");
    RUN(the_simulation_cannot_run_past_the_turns_it_holds);
    RUN(an_order_lands_on_the_tick_its_turn_owns);
    RUN(both_clients_are_given_the_same_command_on_the_same_tick);
    RUN(an_order_for_a_whole_army_crosses_the_relay_whole);
    RUN(a_move_bigger_than_a_turn_goes_over_several_and_loses_nothing);
    RUN(a_finished_tick_is_acknowledged_and_hashed_on_the_sixtieth);
    RUN(outside_a_match_the_simulation_is_never_held_back);
    RUN(twenty_minutes_of_turn_log_is_held_while_the_world_loads);
    RUN(a_ring_that_never_empties_reuses_the_space_of_taken_turns);
    RUN(a_catch_up_feeds_the_queue_without_overflowing_it);
    RUN(the_relays_seat_entries_become_seat_commands_on_their_tick);

    TEST_SUITE("The verdict, for the leaderboard");
    RUN(a_battle_says_whom_it_is_waiting_for);
    RUN(a_battle_says_whom_it_is_slowing_down_for);
    RUN(a_player_who_falls_behind_is_named_until_they_catch_up);
    RUN(a_verdict_is_reported_once_and_only_while_playing);
    RUN(a_reported_verdict_is_recorded_and_the_other_seat_confirms_it);
    RUN(a_report_that_disagrees_marks_the_game_disputed);
    RUN(a_report_for_the_wrong_match_or_tally_set_is_refused);
    RUN(a_verdict_beyond_the_turns_delivered_is_refused);
    RUN(a_blank_name_is_refused_and_a_padded_one_is_trimmed);
    TEST_REPORT();
}
