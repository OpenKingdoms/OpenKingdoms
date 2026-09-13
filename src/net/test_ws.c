/*
 * test_ws.c -- WebSocket framing against RFC 6455's own vectors.
 *
 * The handshake example is section 1.3 and the frame examples are
 * section 5.7. Using the published values rather than our own output
 * is the point: a codec that only agrees with itself agrees with no
 * browser.
 */

#include "test_framework.h"
#include "tak_ws.h"

#include <string.h>

/* Section 1.3: this key must produce this accept value. */
static const char RFC_KEY[]    = "dGhlIHNhbXBsZSBub25jZQ==";
static const char RFC_ACCEPT[] = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

TEST(the_accept_value_is_the_one_the_rfc_publishes) {
    char got[TAK_WS_ACCEPT_CHARS + 1];
    ASSERT_EQ_INT(TAK_WS_KEY_CHARS, (int)strlen(RFC_KEY));
    TAK_Ws_Accept(RFC_KEY, got);
    got[TAK_WS_ACCEPT_CHARS] = '\0';
    ASSERT_EQ_STR(RFC_ACCEPT, got);
}

TEST(a_browsers_upgrade_request_is_answered) {
    const char req[] =
        "GET /ws HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: http://example.com\r\n\r\n";
    char out[256];
    int need_more = -1;
    size_t n = TAK_Ws_ServerHandshake(req, strlen(req), out, sizeof out,
                                      &need_more);
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, need_more);
    out[n] = '\0';
    ASSERT(strstr(out, "HTTP/1.1 101 Switching Protocols") == out);
    ASSERT_NOT_NULL(strstr(out, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    /* The response has to end the header block itself. */
    ASSERT_NOT_NULL(strstr(out, "\r\n\r\n"));
}

TEST(half_an_upgrade_request_asks_for_more_rather_than_refusing) {
    const char part[] =
        "GET /ws HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: webso";
    char out[256];
    int need_more = 0;
    ASSERT_EQ_INT(0, (int)TAK_Ws_ServerHandshake(part, strlen(part), out,
                                                 sizeof out, &need_more));
    ASSERT_EQ_INT(1, need_more);
}

TEST(a_request_that_is_not_an_upgrade_is_refused) {
    struct { const char *name; const char *req; } bad[] = {
        { "no upgrade header",
          "GET /ws HTTP/1.1\r\nHost: h\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n" },
        { "no connection header",
          "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n" },
        { "the wrong version",
          "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
          "Connection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 8\r\n\r\n" },
        { "a key of the wrong length",
          "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
          "Connection: Upgrade\r\nSec-WebSocket-Key: short\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n" },
        { "a POST",
          "POST /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
          "Connection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n" },
    };
    char out[256];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int need_more = -1;
        size_t n = TAK_Ws_ServerHandshake(bad[i].req, strlen(bad[i].req),
                                          out, sizeof out, &need_more);
        if (n != 0 || need_more != 0) {
            printf("(%s was accepted) ", bad[i].name);
            ASSERT_EQ_INT(0, (int)n);
            ASSERT_EQ_INT(0, need_more);
        }
    }
}

TEST(a_client_request_is_answered_by_its_own_server) {
    uint8_t nonce[16];
    for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(i * 7 + 1);
    char key[TAK_WS_KEY_CHARS];
    char req[512], resp[256];
    size_t rn = TAK_Ws_ClientRequest("openkingdoms.example", "/play",
                                     nonce, key, req, sizeof req);
    ASSERT(rn > 0);
    ASSERT_NOT_NULL(strstr(req, "Host: openkingdoms.example"));
    ASSERT(strstr(req, "GET /play HTTP/1.1") == req);

    int need_more = -1;
    size_t sn = TAK_Ws_ServerHandshake(req, rn, resp, sizeof resp, &need_more);
    ASSERT(sn > 0);
    ASSERT_EQ_INT(1, TAK_Ws_CheckServerResponse(resp, sn, key, &need_more));
    ASSERT_EQ_INT(0, need_more);

    /* A response that accepts a different key is not ours. */
    char other[TAK_WS_KEY_CHARS];
    memcpy(other, key, sizeof other);
    other[0] = (char)(other[0] == 'A' ? 'B' : 'A');
    ASSERT_EQ_INT(0, TAK_Ws_CheckServerResponse(resp, sn, other, &need_more));
}

/* Section 5.7, a single unmasked frame carrying "Hello". */
TEST(an_unmasked_frame_is_the_bytes_the_rfc_prints) {
    static const uint8_t want[] = { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
    uint8_t out[32];
    size_t n = TAK_Ws_Encode(TAK_WS_OP_TEXT, "Hello", 5, NULL, out, sizeof out);
    ASSERT_EQ_INT((int)sizeof want, (int)n);
    ASSERT_EQ_INT(0, memcmp(want, out, sizeof want));
}

/* Section 5.7, the same payload masked with 0x37fa213d. */
TEST(a_masked_frame_is_the_bytes_the_rfc_prints) {
    static const uint8_t want[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                    0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    static const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
    uint8_t out[32];
    size_t n = TAK_Ws_Encode(TAK_WS_OP_TEXT, "Hello", 5, mask, out, sizeof out);
    ASSERT_EQ_INT((int)sizeof want, (int)n);
    ASSERT_EQ_INT(0, memcmp(want, out, sizeof want));

    /* And reading it back gives the plain payload. */
    TAK_WsFrame f;
    ASSERT_EQ_INT(TAK_WS_PARSE_OK, TAK_Ws_Parse(out, n, 1, &f));
    ASSERT_EQ_INT(TAK_WS_OP_TEXT, f.opcode);
    ASSERT_EQ_INT(5, (int)f.payload_len);
    ASSERT_EQ_INT(0, memcmp("Hello", f.payload, 5));
    ASSERT_EQ_INT((int)n, (int)f.frame_len);
}

/* Section 5.7 gives 256 and 65536 byte binary frames with these two
 * headers, which is where a length field is easy to get wrong. */
TEST(the_two_long_length_forms_match_the_rfc) {
    static uint8_t body[TAK_WS_PAYLOAD_MAX];
    static uint8_t out[TAK_WS_PAYLOAD_MAX + 32];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 31 + 7);

    size_t n = TAK_Ws_Encode(TAK_WS_OP_BINARY, body, 256, NULL, out, sizeof out);
    ASSERT_EQ_INT(4 + 256, (int)n);
    ASSERT_EQ_INT(0x82, out[0]);
    ASSERT_EQ_INT(0x7E, out[1]);
    ASSERT_EQ_INT(0x01, out[2]);
    ASSERT_EQ_INT(0x00, out[3]);

    n = TAK_Ws_Encode(TAK_WS_OP_BINARY, body, 65536, NULL, out, sizeof out);
    ASSERT_EQ_INT(10 + 65536, (int)n);
    ASSERT_EQ_INT(0x82, out[0]);
    ASSERT_EQ_INT(0x7F, out[1]);
    static const uint8_t len64[] = { 0, 0, 0, 0, 0, 1, 0, 0 };
    ASSERT_EQ_INT(0, memcmp(len64, out + 2, sizeof len64));

    TAK_WsFrame f;
    ASSERT_EQ_INT(TAK_WS_PARSE_OK, TAK_Ws_Parse(out, n, 0, &f));
    ASSERT_EQ_INT(65536, (int)f.payload_len);
    ASSERT_EQ_INT(0, memcmp(body, f.payload, 65536));
}

TEST(a_frame_that_has_not_all_arrived_is_partial_not_bad) {
    uint8_t src[600], out[700], copy[700];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)i;
    static const uint8_t mask[4] = { 1, 2, 3, 4 };
    size_t n = TAK_Ws_Encode(TAK_WS_OP_BINARY, src, sizeof src, mask,
                             out, sizeof out);
    ASSERT(n > 0);
    TAK_WsFrame f;
    /* Every prefix short of the whole frame, since a header can split
     * inside the length field or inside the masking key. */
    for (size_t cut = 1; cut < n; cut++) {
        memcpy(copy, out, n);
        ASSERT_EQ_INT(TAK_WS_PARSE_PARTIAL, TAK_Ws_Parse(copy, cut, 1, &f));
    }
    memcpy(copy, out, n);
    ASSERT_EQ_INT(TAK_WS_PARSE_OK, TAK_Ws_Parse(copy, n, 1, &f));
    ASSERT_EQ_INT((int)sizeof src, (int)f.payload_len);
    ASSERT_EQ_INT(0, memcmp(src, f.payload, sizeof src));
}

TEST(two_frames_in_one_read_come_out_one_at_a_time) {
    uint8_t buf[64];
    static const uint8_t mask[4] = { 9, 8, 7, 6 };
    size_t a = TAK_Ws_Encode(TAK_WS_OP_BINARY, "one", 3, mask, buf, sizeof buf);
    size_t b = TAK_Ws_Encode(TAK_WS_OP_BINARY, "two", 3, mask, buf + a,
                             sizeof buf - a);
    ASSERT(a > 0 && b > 0);
    TAK_WsFrame f;
    ASSERT_EQ_INT(TAK_WS_PARSE_OK, TAK_Ws_Parse(buf, a + b, 1, &f));
    ASSERT_EQ_INT(0, memcmp("one", f.payload, 3));
    ASSERT_EQ_INT((int)a, (int)f.frame_len);
    ASSERT_EQ_INT(TAK_WS_PARSE_OK,
                  TAK_Ws_Parse(buf + f.frame_len, b, 1, &f));
    ASSERT_EQ_INT(0, memcmp("two", f.payload, 3));
}

TEST(a_frame_the_server_must_not_accept_is_refused) {
    uint8_t buf[64];
    TAK_WsFrame f;

    /* A client that does not mask. RFC 6455 says the server closes. */
    size_t n = TAK_Ws_Encode(TAK_WS_OP_BINARY, "hi", 2, NULL, buf, sizeof buf);
    ASSERT_EQ_INT(TAK_WS_PARSE_BAD, TAK_Ws_Parse(buf, n, 1, &f));

    /* A reserved bit, which means an extension we did not negotiate. */
    static const uint8_t mask[4] = { 1, 1, 1, 1 };
    n = TAK_Ws_Encode(TAK_WS_OP_BINARY, "hi", 2, mask, buf, sizeof buf);
    buf[0] = (uint8_t)(buf[0] | 0x40);
    ASSERT_EQ_INT(TAK_WS_PARSE_BAD, TAK_Ws_Parse(buf, n, 1, &f));

    /* A fragment, which the message layer above has no use for. */
    n = TAK_Ws_Encode(TAK_WS_OP_BINARY, "hi", 2, mask, buf, sizeof buf);
    buf[0] = (uint8_t)(buf[0] & 0x7F);
    ASSERT_EQ_INT(TAK_WS_PARSE_BAD, TAK_Ws_Parse(buf, n, 1, &f));

    /* An opcode nobody defined. */
    n = TAK_Ws_Encode(TAK_WS_OP_BINARY, "hi", 2, mask, buf, sizeof buf);
    buf[0] = 0x83;
    ASSERT_EQ_INT(TAK_WS_PARSE_BAD, TAK_Ws_Parse(buf, n, 1, &f));

    /* A ping carrying more than 125 bytes. */
    uint8_t big[200];
    memset(big, 0, sizeof big);
    n = TAK_Ws_Encode(TAK_WS_OP_PING, big, sizeof big, mask, buf, sizeof buf);
    ASSERT_EQ_INT(0, (int)n);       /* it does not even fit here */
    static uint8_t wide[512];
    n = TAK_Ws_Encode(TAK_WS_OP_PING, big, sizeof big, mask, wide, sizeof wide);
    ASSERT(n > 0);
    ASSERT_EQ_INT(TAK_WS_PARSE_BAD, TAK_Ws_Parse(wide, n, 1, &f));
}

TEST(a_length_longer_than_the_cap_is_refused_before_it_is_read) {
    /* A header claiming four gigabytes, with nothing behind it. A
     * parser that trusted the field would wait for it forever or, on a
     * 32 bit build, truncate it into something small and read on. */
    uint8_t buf[14];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x82;
    buf[1] = (uint8_t)(0x80 | 127);
    buf[2] = 0; buf[3] = 0; buf[4] = 0; buf[5] = 1;   /* 2^32 bytes */
    buf[6] = 0; buf[7] = 0; buf[8] = 0; buf[9] = 0;
    TAK_WsFrame f;
    ASSERT_EQ_INT(TAK_WS_PARSE_BAD, TAK_Ws_Parse(buf, sizeof buf, 1, &f));

    /* And a length written in a longer form than it needs, which is
     * how two encoders of the same message disagree. */
    uint8_t two[16];
    memset(two, 0, sizeof two);
    two[0] = 0x82;
    two[1] = (uint8_t)(0x80 | 126);
    two[2] = 0; two[3] = 5;                    /* 5 fits in seven bits */
    ASSERT_EQ_INT(TAK_WS_PARSE_BAD, TAK_Ws_Parse(two, sizeof two, 1, &f));
}

TEST(a_payload_over_the_cap_is_never_encoded) {
    static uint8_t body[TAK_WS_PAYLOAD_MAX + 1];
    static uint8_t out[TAK_WS_PAYLOAD_MAX + 32];
    memset(body, 0, sizeof body);
    ASSERT_EQ_INT(0, (int)TAK_Ws_Encode(TAK_WS_OP_BINARY, body,
                                        TAK_WS_PAYLOAD_MAX + 1, NULL,
                                        out, sizeof out));
    /* And a buffer one byte short takes nothing rather than some. */
    ASSERT_EQ_INT(0, (int)TAK_Ws_Encode(TAK_WS_OP_BINARY, "hello", 5, NULL,
                                        out, 6));
}

int main(void) {
    TEST_SUITE("WebSocket handshake");
    RUN(the_accept_value_is_the_one_the_rfc_publishes);
    RUN(a_browsers_upgrade_request_is_answered);
    RUN(half_an_upgrade_request_asks_for_more_rather_than_refusing);
    RUN(a_request_that_is_not_an_upgrade_is_refused);
    RUN(a_client_request_is_answered_by_its_own_server);

    TEST_SUITE("WebSocket frames");
    RUN(an_unmasked_frame_is_the_bytes_the_rfc_prints);
    RUN(a_masked_frame_is_the_bytes_the_rfc_prints);
    RUN(the_two_long_length_forms_match_the_rfc);
    RUN(a_frame_that_has_not_all_arrived_is_partial_not_bad);
    RUN(two_frames_in_one_read_come_out_one_at_a_time);
    RUN(a_frame_the_server_must_not_accept_is_refused);
    RUN(a_length_longer_than_the_cap_is_refused_before_it_is_read);
    RUN(a_payload_over_the_cap_is_never_encoded);
    TEST_REPORT();
}
