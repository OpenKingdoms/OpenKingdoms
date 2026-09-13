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
            TAK_NetFrame f;
            if (TAK_Net_Split(g_to_client + off, g_to_client_len - off,
                              &f) != 0) break;
            size_t whole = TAK_Net_FrameSize(f.payload_len);
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
    TEST_REPORT();
}
