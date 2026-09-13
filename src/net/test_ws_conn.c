/*
 * test_ws_conn.c -- one WebSocket connection as a byte pump.
 *
 * The states worth testing here are the ones a socket produces by luck
 * and a test can produce exactly: a handshake split across two reads,
 * a frame split across three, frames arriving in the same read as the
 * end of the handshake, a peer that never finishes its request, and a
 * peer that stops reading while we keep writing.
 *
 * The two ends are driven against each other rather than against
 * recorded bytes, so anything the server will not accept from a client
 * is something our own client cannot send.
 */

#include "test_framework.h"
#include "tak_ws_conn.h"

#include <string.h>

/* 200 KB apiece, which is more than a default stack wants to hold. */
static TAK_WsConn g_server;
static TAK_WsConn g_client;

static const uint8_t NONCE[16] = {
    1, 8, 15, 22, 29, 36, 43, 50, 57, 64, 71, 78, 85, 92, 99, 106
};

/* Move everything one end has queued into the other, as a socket would
 * on a good day. Returns the byte count. */
static size_t pump(TAK_WsConn *from, TAK_WsConn *to) {
    size_t n = 0;
    const uint8_t *p = TAK_WsConn_Pending(from, &n);
    if (n == 0) return 0;
    if (TAK_WsConn_Feed(to, p, n) != 0) return 0;
    TAK_WsConn_Wrote(from, n);
    return n;
}

/* Step until nothing more comes out, collecting messages. */
static int drain(TAK_WsConn *c, char out[8][64], int *count) {
    for (;;) {
        TAK_WsConnStep s = TAK_WsConn_Step(c);
        if (s == TAK_WSCONN_NEED_MORE) return 0;
        if (s == TAK_WSCONN_ERROR) return -1;
        if (s == TAK_WSCONN_CLOSED) return 1;
        if (s == TAK_WSCONN_MESSAGE && out && count && *count < 8) {
            size_t n = c->msg_len < 63 ? c->msg_len : 63;
            memcpy(out[*count], c->msg, n);
            out[*count][n] = '\0';
            (*count)++;
        }
    }
}

/* Bring both ends up. 0 on success. */
static int handshake(void) {
    TAK_WsConn_InitServer(&g_server);
    if (TAK_WsConn_InitClient(&g_client, "openkingdoms.example", "/play",
                              NONCE) != 0) return -1;
    if (pump(&g_client, &g_server) == 0) return -1;
    if (drain(&g_server, NULL, NULL) != 0) return -1;
    if (g_server.state != TAK_WSCONN_OPEN) return -1;
    if (pump(&g_server, &g_client) == 0) return -1;
    if (drain(&g_client, NULL, NULL) != 0) return -1;
    if (g_client.state != TAK_WSCONN_OPEN) return -1;
    return 0;
}

TEST(the_two_ends_shake_hands_and_talk) {
    ASSERT_EQ_INT(0, handshake());

    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, "from the client", 15));
    ASSERT(pump(&g_client, &g_server) > 0);
    char got[8][64];
    int n = 0;
    ASSERT_EQ_INT(0, drain(&g_server, got, &n));
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_STR("from the client", got[0]);

    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_server, "from the server", 15));
    ASSERT(pump(&g_server, &g_client) > 0);
    n = 0;
    ASSERT_EQ_INT(0, drain(&g_client, got, &n));
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_STR("from the server", got[0]);
}

TEST(a_handshake_split_across_reads_still_completes) {
    TAK_WsConn_InitServer(&g_server);
    ASSERT_EQ_INT(0, TAK_WsConn_InitClient(&g_client, "h", "/play", NONCE));

    size_t n = 0;
    const uint8_t *req = TAK_WsConn_Pending(&g_client, &n);
    ASSERT(n > 8);
    /* One byte at a time, which is the worst a socket can do. */
    for (size_t i = 0; i < n; i++) {
        ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_server, req + i, 1));
        TAK_WsConnStep s = TAK_WsConn_Step(&g_server);
        if (i + 1 < n) {
            ASSERT_EQ_INT(TAK_WSCONN_NEED_MORE, s);
            ASSERT_EQ_INT(TAK_WSCONN_HANDSHAKE, g_server.state);
        } else {
            ASSERT_EQ_INT(TAK_WSCONN_OK, s);
            ASSERT_EQ_INT(TAK_WSCONN_OPEN, g_server.state);
        }
    }
}

TEST(frames_riding_in_with_the_handshake_are_not_lost) {
    /* A client that writes its request and its first message in one
     * go, which a real one does whenever it can. */
    TAK_WsConn_InitServer(&g_server);
    ASSERT_EQ_INT(0, TAK_WsConn_InitClient(&g_client, "h", "/play", NONCE));
    /* The client cannot Send before its own handshake is answered, so
     * queue the frame by hand behind the request. */
    size_t reqlen = 0;
    (void)TAK_WsConn_Pending(&g_client, &reqlen);
    g_client.state = TAK_WSCONN_OPEN;
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, "early", 5));
    g_client.state = TAK_WSCONN_HANDSHAKE;

    ASSERT(pump(&g_client, &g_server) > reqlen);
    char got[8][64];
    int n = 0;
    ASSERT_EQ_INT(0, drain(&g_server, got, &n));
    ASSERT_EQ_INT(TAK_WSCONN_OPEN, g_server.state);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_STR("early", got[0]);
}

TEST(a_frame_split_across_reads_arrives_whole) {
    ASSERT_EQ_INT(0, handshake());

    static uint8_t body[4096];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 13 + 5);
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, body, sizeof body));

    size_t n = 0;
    const uint8_t *frame = TAK_WsConn_Pending(&g_client, &n);
    static uint8_t copy[8192];
    ASSERT(n <= sizeof copy);
    memcpy(copy, frame, n);
    TAK_WsConn_Wrote(&g_client, n);

    /* Three pieces, the first cutting the header and the second
     * cutting the payload. */
    const size_t cut1 = 3, cut2 = n / 2;
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_server, copy, cut1));
    ASSERT_EQ_INT(TAK_WSCONN_NEED_MORE, TAK_WsConn_Step(&g_server));
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_server, copy + cut1, cut2 - cut1));
    ASSERT_EQ_INT(TAK_WSCONN_NEED_MORE, TAK_WsConn_Step(&g_server));
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_server, copy + cut2, n - cut2));
    ASSERT_EQ_INT(TAK_WSCONN_MESSAGE, TAK_WsConn_Step(&g_server));
    ASSERT_EQ_INT((int)sizeof body, (int)g_server.msg_len);
    ASSERT_EQ_INT(0, memcmp(body, g_server.msg, sizeof body));
}

TEST(several_frames_in_one_read_all_come_out) {
    ASSERT_EQ_INT(0, handshake());
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, "one", 3));
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, "two", 3));
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, "three", 5));
    ASSERT(pump(&g_client, &g_server) > 0);

    char got[8][64];
    int n = 0;
    ASSERT_EQ_INT(0, drain(&g_server, got, &n));
    ASSERT_EQ_INT(3, n);
    ASSERT_EQ_STR("one", got[0]);
    ASSERT_EQ_STR("two", got[1]);
    ASSERT_EQ_STR("three", got[2]);
}

TEST(a_message_survives_until_the_next_step) {
    ASSERT_EQ_INT(0, handshake());
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, "first", 5));
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_client, "second", 6));
    ASSERT(pump(&g_client, &g_server) > 0);

    ASSERT_EQ_INT(TAK_WSCONN_MESSAGE, TAK_WsConn_Step(&g_server));
    const uint8_t *first = g_server.msg;
    ASSERT_EQ_INT(5, (int)g_server.msg_len);
    /* Reading it after the step that produced it is the contract, and
     * the bytes must not have moved under a second frame waiting. */
    ASSERT_EQ_INT(0, memcmp("first", first, 5));
    ASSERT_EQ_INT(TAK_WSCONN_MESSAGE, TAK_WsConn_Step(&g_server));
    ASSERT_EQ_INT(0, memcmp("second", g_server.msg, 6));
}

TEST(a_ping_is_answered_with_a_pong) {
    ASSERT_EQ_INT(0, handshake());
    /* The server pings, which is what the relay's 2 second heartbeat
     * would do if it used a control frame. */
    uint8_t frame[32];
    size_t n = TAK_Ws_Encode(TAK_WS_OP_PING, "beat", 4, NULL,
                             frame, sizeof frame);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_client, frame, n));
    ASSERT_EQ_INT(TAK_WSCONN_OK, TAK_WsConn_Step(&g_client));
    ASSERT_EQ_INT(TAK_WSCONN_NEED_MORE, TAK_WsConn_Step(&g_client));

    /* And the pong comes back masked, carrying the same body. */
    size_t out = 0;
    const uint8_t *p = TAK_WsConn_Pending(&g_client, &out);
    ASSERT(out > 0);
    static uint8_t copy[64];
    memcpy(copy, p, out);
    TAK_WsFrame f;
    ASSERT_EQ_INT(TAK_WS_PARSE_OK, TAK_Ws_Parse(copy, out, 1, &f));
    ASSERT_EQ_INT(TAK_WS_OP_PONG, f.opcode);
    ASSERT_EQ_INT(4, (int)f.payload_len);
    ASSERT_EQ_INT(0, memcmp("beat", f.payload, 4));
}

TEST(a_close_from_the_peer_ends_it_both_ways) {
    ASSERT_EQ_INT(0, handshake());
    TAK_WsConn_Close(&g_client, 1000);
    ASSERT_EQ_INT(TAK_WSCONN_CLOSING, g_client.state);
    ASSERT(pump(&g_client, &g_server) > 0);
    ASSERT_EQ_INT(1, drain(&g_server, NULL, NULL));
    ASSERT_EQ_INT(1, (int)g_server.got_close);
    ASSERT_EQ_INT(1, (int)g_server.sent_close);
    ASSERT_EQ_INT(TAK_WSCONN_CLOSING, g_server.state);
    /* And it will not take new work after that. */
    ASSERT_EQ_INT(-1, TAK_WsConn_Send(&g_server, "too late", 8));
}

TEST(a_client_that_does_not_mask_is_refused) {
    ASSERT_EQ_INT(0, handshake());
    uint8_t frame[32];
    size_t n = TAK_Ws_Encode(TAK_WS_OP_BINARY, "plain", 5, NULL,
                             frame, sizeof frame);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_server, frame, n));
    ASSERT_EQ_INT(TAK_WSCONN_ERROR, TAK_WsConn_Step(&g_server));
    ASSERT_EQ_INT(TAK_WSCONN_DEAD, g_server.state);
    ASSERT_NOT_NULL(g_server.why);
}

TEST(a_text_frame_is_refused_because_this_protocol_is_binary) {
    ASSERT_EQ_INT(0, handshake());
    static const uint8_t mask[4] = { 3, 1, 4, 1 };
    uint8_t frame[32];
    size_t n = TAK_Ws_Encode(TAK_WS_OP_TEXT, "hello", 5, mask,
                             frame, sizeof frame);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_server, frame, n));
    ASSERT_EQ_INT(TAK_WSCONN_ERROR, TAK_WsConn_Step(&g_server));
    ASSERT_EQ_INT(TAK_WSCONN_DEAD, g_server.state);
}

TEST(a_request_that_never_ends_is_given_up_on) {
    TAK_WsConn_InitServer(&g_server);
    static uint8_t junk[1024];
    memset(junk, 'x', sizeof junk);
    /* No blank line, ever. It asks for more until the cap, then goes. */
    int gave_up = 0;
    for (int i = 0; i < 64; i++) {
        if (TAK_WsConn_Feed(&g_server, junk, sizeof junk) != 0) {
            gave_up = 1;
            break;
        }
        if (TAK_WsConn_Step(&g_server) == TAK_WSCONN_ERROR) {
            gave_up = 1;
            break;
        }
    }
    ASSERT_EQ_INT(1, gave_up);
    ASSERT_EQ_INT(TAK_WSCONN_DEAD, g_server.state);
}

TEST(a_peer_that_stops_reading_is_reported_not_grown) {
    ASSERT_EQ_INT(0, handshake());
    static uint8_t body[16384];
    memset(body, 7, sizeof body);
    /* Nothing is ever drained, so the out buffer fills and says so
     * rather than the host growing it for a peer that is not there. */
    int refused = 0;
    for (int i = 0; i < 64; i++) {
        if (TAK_WsConn_Send(&g_server, body, sizeof body) != 0) {
            refused = 1;
            break;
        }
    }
    ASSERT_EQ_INT(1, refused);
    size_t n = 0;
    (void)TAK_WsConn_Pending(&g_server, &n);
    ASSERT(n <= TAK_WSCONN_OUT_CAP);
}

TEST(a_partial_write_leaves_the_rest_queued) {
    ASSERT_EQ_INT(0, handshake());
    ASSERT_EQ_INT(0, TAK_WsConn_Send(&g_server, "a message worth splitting", 25));
    size_t n = 0;
    const uint8_t *p = TAK_WsConn_Pending(&g_server, &n);
    ASSERT(n > 4);
    static uint8_t copy[128];
    memcpy(copy, p, n);

    /* The socket took four bytes and no more, which is ordinary. */
    TAK_WsConn_Wrote(&g_server, 4);
    size_t left = 0;
    const uint8_t *rest = TAK_WsConn_Pending(&g_server, &left);
    ASSERT_EQ_INT((int)(n - 4), (int)left);
    ASSERT_EQ_INT(0, memcmp(copy + 4, rest, left));

    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_client, copy, 4));
    ASSERT_EQ_INT(TAK_WSCONN_NEED_MORE, TAK_WsConn_Step(&g_client));
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_client, rest, left));
    ASSERT_EQ_INT(TAK_WSCONN_MESSAGE, TAK_WsConn_Step(&g_client));
    ASSERT_EQ_INT(25, (int)g_client.msg_len);
}

TEST(a_server_that_is_not_a_websocket_server_is_not_believed) {
    TAK_WsConn_InitClient(&g_client, "h", "/play", NONCE);
    const char page[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>";
    ASSERT_EQ_INT(0, TAK_WsConn_Feed(&g_client, page, sizeof page - 1));
    ASSERT_EQ_INT(TAK_WSCONN_ERROR, TAK_WsConn_Step(&g_client));
    ASSERT_EQ_INT(TAK_WSCONN_DEAD, g_client.state);
}

int main(void) {
    TEST_SUITE("WebSocket connection");
    RUN(the_two_ends_shake_hands_and_talk);
    RUN(a_handshake_split_across_reads_still_completes);
    RUN(frames_riding_in_with_the_handshake_are_not_lost);
    RUN(a_frame_split_across_reads_arrives_whole);
    RUN(several_frames_in_one_read_all_come_out);
    RUN(a_message_survives_until_the_next_step);
    RUN(a_ping_is_answered_with_a_pong);
    RUN(a_close_from_the_peer_ends_it_both_ways);
    RUN(a_client_that_does_not_mask_is_refused);
    RUN(a_text_frame_is_refused_because_this_protocol_is_binary);
    RUN(a_request_that_never_ends_is_given_up_on);
    RUN(a_peer_that_stops_reading_is_reported_not_grown);
    RUN(a_partial_write_leaves_the_rest_queued);
    RUN(a_server_that_is_not_a_websocket_server_is_not_believed);
    TEST_REPORT();
}
