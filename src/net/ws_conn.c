/*
 * ws_conn.c -- one WebSocket connection as a byte pump.
 *
 * See tak_ws_conn.h for why this has no sockets in it.
 */

#include "tak_ws_conn.h"

#include <string.h>

/* How much of an upgrade request we will read before giving up on it.
 * A real one is a few hundred bytes. A peer that sends more than this
 * without a blank line is not going to finish. */
#define HANDSHAKE_MAX 8192

/* Length of the header block at `p`, including its blank line, or 0
 * when there is not one. The handshake parser has already said there
 * is one, so this only has to find where it ends. */
static size_t header_len(const uint8_t *p, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (i + 3 < len && p[i] == '\r' && p[i + 1] == '\n' &&
            p[i + 2] == '\r' && p[i + 3] == '\n') return i + 4;
        if (p[i] == '\n' && p[i + 1] == '\n') return i + 2;
    }
    return 0;
}

static void die(TAK_WsConn *c, const char *why) {
    c->state = TAK_WSCONN_DEAD;
    if (!c->why) c->why = why;
}

void TAK_WsConn_InitServer(TAK_WsConn *c) {
    memset(c, 0, sizeof(*c));
    c->state = TAK_WSCONN_HANDSHAKE;
    c->is_server = 1;
}

int TAK_WsConn_InitClient(TAK_WsConn *c, const char *host, const char *path,
                          const uint8_t nonce[16]) {
    memset(c, 0, sizeof(*c));
    c->state = TAK_WSCONN_HANDSHAKE;
    c->is_server = 0;
    size_t n = TAK_Ws_ClientRequest(host, path, nonce, c->key,
                                    (char *)c->out, sizeof c->out);
    if (n == 0) { die(c, "upgrade request did not fit"); return -1; }
    c->out_len = n;
    return 0;
}

/* Drop what has been parsed. This is the only thing that moves the
 * buffer, and it is why a message handed back is good until the next
 * Step or Feed and no longer. */
static void in_compact(TAK_WsConn *c) {
    if (c->in_pos == 0) return;
    if (c->in_pos >= c->in_len) { c->in_len = 0; }
    else {
        memmove(c->in, c->in + c->in_pos, c->in_len - c->in_pos);
        c->in_len -= c->in_pos;
    }
    c->in_pos = 0;
}

int TAK_WsConn_Feed(TAK_WsConn *c, const void *data, size_t len) {
    if (c->state == TAK_WSCONN_DEAD) return -1;
    if (len == 0) return 0;
    in_compact(c);
    if (c->in_len + len > sizeof c->in) {
        /* Over the cap before a frame has ended. Either the peer is
         * lying about a length or it is not speaking this protocol. */
        die(c, "more bytes than a whole frame can hold");
        return -1;
    }
    memcpy(c->in + c->in_len, data, len);
    c->in_len += len;
    return 0;
}

/* Unparsed bytes and where they start. */
static uint8_t *in_head(TAK_WsConn *c) { return c->in + c->in_pos; }
static size_t   in_avail(const TAK_WsConn *c) { return c->in_len - c->in_pos; }
static void     in_consume(TAK_WsConn *c, size_t n) { c->in_pos += n; }

static int out_room(const TAK_WsConn *c, size_t n) {
    return c->out_len + n <= sizeof c->out;
}

/* A client masks every frame. The mask is not a secret, it is there so
 * a proxy cannot be talked into caching a frame as a response, so a
 * counter is enough and keeps this module free of randomness. */
static void next_mask(TAK_WsConn *c, uint8_t out[4]) {
    c->mask_counter = c->mask_counter * 1664525u + 1013904223u;
    out[0] = (uint8_t)(c->mask_counter >> 24);
    out[1] = (uint8_t)(c->mask_counter >> 16);
    out[2] = (uint8_t)(c->mask_counter >> 8);
    out[3] = (uint8_t)(c->mask_counter);
}

static int queue_frame(TAK_WsConn *c, uint8_t opcode,
                       const void *payload, size_t len) {
    uint8_t mask[4];
    const uint8_t *m = NULL;
    if (!c->is_server) { next_mask(c, mask); m = mask; }
    size_t room = sizeof c->out - c->out_len;
    size_t n = TAK_Ws_Encode(opcode, payload, len, m,
                             c->out + c->out_len, room);
    if (n == 0) return -1;
    c->out_len += n;
    return 0;
}

int TAK_WsConn_Send(TAK_WsConn *c, const void *payload, size_t len) {
    if (c->state != TAK_WSCONN_OPEN) return -1;
    if (queue_frame(c, TAK_WS_OP_BINARY, payload, len) != 0) return -1;
    return 0;
}

void TAK_WsConn_Close(TAK_WsConn *c, uint16_t code) {
    if (c->sent_close || c->state == TAK_WSCONN_DEAD) return;
    uint8_t body[2];
    body[0] = (uint8_t)(code >> 8);
    body[1] = (uint8_t)code;
    /* A close that does not fit is a peer that stopped reading, and
     * the host drops the socket either way. */
    (void)queue_frame(c, TAK_WS_OP_CLOSE, body, sizeof body);
    c->sent_close = 1;
    c->state = TAK_WSCONN_CLOSING;
}

int TAK_WsConn_PlainRequest(const TAK_WsConn *c, const uint8_t **req,
                            size_t *len) {
    if (!c->is_server || c->state != TAK_WSCONN_HANDSHAKE) return 0;
    const uint8_t *p = c->in + c->in_pos;
    size_t n = c->in_len - c->in_pos;
    if (header_len(p, n) == 0) return 0;
    /* An upgrade this end would answer is the socket's, whatever else
     * the request says. */
    char resp[256];
    int need_more = 0;
    if (TAK_Ws_ServerHandshake((const char *)p, n, resp, sizeof resp,
                               &need_more) != 0) return 0;
    if (n < 4 || (memcmp(p, "GET ", 4) != 0 && memcmp(p, "HEAD", 4) != 0 &&
                  memcmp(p, "OPTI", 4) != 0)) return 0;
    *req = p;
    *len = header_len(p, n);
    return 1;
}

int TAK_WsConn_Answer(TAK_WsConn *c, const void *bytes, size_t len) {
    if (c->state != TAK_WSCONN_HANDSHAKE || !out_room(c, len)) return -1;
    memcpy(c->out + c->out_len, bytes, len);
    c->out_len += len;
    /* Nothing more is read and no close frame follows: this was never
     * a WebSocket. The host writes what is queued and drops it. */
    c->sent_close = 1;
    c->state = TAK_WSCONN_CLOSING;
    return 0;
}

const uint8_t *TAK_WsConn_Pending(const TAK_WsConn *c, size_t *len) {
    *len = c->out_len;
    return c->out;
}

void TAK_WsConn_Wrote(TAK_WsConn *c, size_t len) {
    if (len >= c->out_len) { c->out_len = 0; return; }
    memmove(c->out, c->out + len, c->out_len - len);
    c->out_len -= len;
}

/* The handshake, from whichever side we are on. */
static TAK_WsConnStep step_handshake(TAK_WsConn *c) {
    int need_more = 0;
    if (c->is_server) {
        char resp[256];
        size_t n = TAK_Ws_ServerHandshake((const char *)in_head(c),
                                          in_avail(c),
                                          resp, sizeof resp, &need_more);
        if (n == 0) {
            if (need_more && in_avail(c) < HANDSHAKE_MAX) {
                return TAK_WSCONN_NEED_MORE;
            }
            die(c, "not a WebSocket upgrade");
            return TAK_WSCONN_ERROR;
        }
        if (!out_room(c, n)) { die(c, "no room for the answer"); return TAK_WSCONN_ERROR; }
        memcpy(c->out + c->out_len, resp, n);
        c->out_len += n;
        /* Everything after the blank line is already frames. */
        in_consume(c, header_len(in_head(c), in_avail(c)));
        c->state = TAK_WSCONN_OPEN;
        return TAK_WSCONN_OK;
    }

    int ok = TAK_Ws_CheckServerResponse((const char *)in_head(c), in_avail(c),
                                        c->key, &need_more);
    if (!ok) {
        if (need_more && in_avail(c) < HANDSHAKE_MAX) return TAK_WSCONN_NEED_MORE;
        die(c, "the server did not accept the upgrade");
        return TAK_WSCONN_ERROR;
    }
    in_consume(c, header_len(in_head(c), in_avail(c)));
    c->state = TAK_WSCONN_OPEN;
    return TAK_WSCONN_OK;
}

TAK_WsConnStep TAK_WsConn_Step(TAK_WsConn *c) {
    c->msg = NULL;
    c->msg_len = 0;
    if (c->state == TAK_WSCONN_DEAD) return TAK_WSCONN_ERROR;
    if (c->state == TAK_WSCONN_HANDSHAKE) return step_handshake(c);

    TAK_WsFrame f;
    int rc = TAK_Ws_Parse(in_head(c), in_avail(c), c->is_server ? 1 : 0, &f);
    if (rc == TAK_WS_PARSE_PARTIAL) return TAK_WSCONN_NEED_MORE;
    if (rc == TAK_WS_PARSE_BAD) {
        die(c, "a frame this end will not accept");
        return TAK_WSCONN_ERROR;
    }

    switch (f.opcode) {
    case TAK_WS_OP_BINARY:
        /* A close went out, so nothing after it is acted on. Drain it
         * rather than treat it as an error: the peer was mid sentence
         * when we stopped listening. */
        if (c->state == TAK_WSCONN_CLOSING) {
            in_consume(c, f.frame_len);
            return TAK_WSCONN_OK;
        }
        c->msg = f.payload;
        c->msg_len = f.payload_len;
        /* The payload points into `in`. Consuming only moves an
         * offset, so those bytes stay put until the next Feed. */
        in_consume(c, f.frame_len);
        return TAK_WSCONN_MESSAGE;

    case TAK_WS_OP_TEXT:
        /* The protocol above is binary. A text frame is a browser
         * console or a probe, not a client. */
        in_consume(c, f.frame_len);
        die(c, "a text frame, and this protocol is binary");
        return TAK_WSCONN_ERROR;

    case TAK_WS_OP_PING: {
        uint8_t body[125];
        size_t n = f.payload_len;
        if (n > sizeof body) n = sizeof body;
        memcpy(body, f.payload, n);
        in_consume(c, f.frame_len);
        if (c->state == TAK_WSCONN_OPEN) {
            /* A pong that does not fit means the peer is not reading,
             * which the host sees as a full out buffer soon enough. */
            (void)queue_frame(c, TAK_WS_OP_PONG, body, n);
        }
        return TAK_WSCONN_OK;
    }

    case TAK_WS_OP_PONG:
        in_consume(c, f.frame_len);
        return TAK_WSCONN_OK;

    case TAK_WS_OP_CLOSE:
        in_consume(c, f.frame_len);
        c->got_close = 1;
        if (!c->sent_close) TAK_WsConn_Close(c, 1000);
        c->state = TAK_WSCONN_CLOSING;
        return TAK_WSCONN_CLOSED;

    default:
        die(c, "an opcode nobody defined");
        return TAK_WSCONN_ERROR;
    }
}
