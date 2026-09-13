#ifndef TAK_WS_H
#define TAK_WS_H

#include <stddef.h>
#include <stdint.h>

/*
 * WebSocket framing, RFC 6455, the part of it this project needs.
 *
 * One module for both ends. The relay's host program uses the server
 * half, a native client uses the client half, and the browser uses
 * neither because the page already has a WebSocket.
 *
 * No sockets and no allocation. The caller owns every buffer, which is
 * what lets the whole thing be tested against the RFC's own vectors
 * with no network in the room.
 *
 * What it does not do, on purpose: no extensions, no permessage
 * deflate, no continuation frames. The protocol above sends one whole
 * message per frame with a 64 KB cap, so a fragmented frame is a
 * client we do not talk to rather than a case to carry.
 */

#define TAK_WS_KEY_CHARS     24    /* base64 of 16 bytes, no terminator */
#define TAK_WS_ACCEPT_CHARS  28    /* base64 of a 20 byte digest       */
#define TAK_WS_PAYLOAD_MAX   65536 /* the cap the protocol enforces    */
/* Two byte header, eight byte length, four byte mask. */
#define TAK_WS_HEADER_MAX    14

enum {
    TAK_WS_OP_CONT   = 0x0,
    TAK_WS_OP_TEXT   = 0x1,
    TAK_WS_OP_BINARY = 0x2,
    TAK_WS_OP_CLOSE  = 0x8,
    TAK_WS_OP_PING   = 0x9,
    TAK_WS_OP_PONG   = 0xA
};

/* ── The opening handshake ─────────────────────────────────────────── */

/* Read a client's upgrade request and write the 101 response.
 * Returns the response length, or 0 when the request is not a
 * WebSocket upgrade this module will answer. `req` need not be
 * terminated. A request still arriving in pieces, meaning one with no
 * blank line in it yet, returns 0 with *need_more set to 1, and the
 * caller reads more and asks again. */
size_t TAK_Ws_ServerHandshake(const char *req, size_t req_len,
                              char *out, size_t cap, int *need_more);

/* Write a client upgrade request for host and path. `key_out` takes
 * the Sec-WebSocket-Key so the response can be checked, and it is not
 * terminated. `nonce` is 16 bytes the caller supplies, because this
 * module has no randomness of its own. Returns the request length. */
size_t TAK_Ws_ClientRequest(const char *host, const char *path,
                            const uint8_t nonce[16],
                            char key_out[TAK_WS_KEY_CHARS],
                            char *out, size_t cap);

/* Does this 101 response accept the key we sent? Returns 1 when it
 * does. A response still arriving returns 0 with *need_more set. */
int TAK_Ws_CheckServerResponse(const char *resp, size_t resp_len,
                               const char key[TAK_WS_KEY_CHARS],
                               int *need_more);

/* The accept value for a key, which is what the handshake is. Exposed
 * because it is the one part with a published test vector. */
void TAK_Ws_Accept(const char key[TAK_WS_KEY_CHARS],
                   char out[TAK_WS_ACCEPT_CHARS]);

/* ── Frames ────────────────────────────────────────────────────────── */

/* Write one whole frame. `mask` is the four byte masking key a client
 * must use, or NULL for a server, which never masks. Returns the byte
 * count, or 0 when it does not fit or the payload is over the cap. */
size_t TAK_Ws_Encode(uint8_t opcode, const void *payload, size_t len,
                     const uint8_t mask[4], void *out, size_t cap);

typedef struct TAK_WsFrame {
    uint8_t  opcode;
    uint8_t  fin;
    uint8_t *payload;      /* points into the caller's buffer, unmasked
                            * in place when the frame was masked */
    size_t   payload_len;
    size_t   frame_len;    /* header and payload, to advance the buffer */
} TAK_WsFrame;

enum {
    TAK_WS_PARSE_OK      =  1,  /* one whole frame in `out`          */
    TAK_WS_PARSE_PARTIAL =  0,  /* nothing yet, read more            */
    TAK_WS_PARSE_BAD     = -1   /* close the connection              */
};

/* Take the first whole frame out of `buf`. A masked frame is unmasked
 * in place, so `buf` is not const and the payload points into it.
 * `expect_mask` is 1 on the server, where an unmasked frame from a
 * client is a protocol error, and 0 on a client, where a masked frame
 * from a server is. */
int TAK_Ws_Parse(void *buf, size_t len, int expect_mask, TAK_WsFrame *out);

#endif /* TAK_WS_H */
