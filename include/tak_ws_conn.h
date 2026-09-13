#ifndef TAK_WS_CONN_H
#define TAK_WS_CONN_H

#include <stddef.h>
#include <stdint.h>

#include "tak_ws.h"

/*
 * One WebSocket connection as a byte pump, with no sockets in it.
 *
 * Bytes arrive from somewhere and go somewhere. This turns the first
 * into whole protocol messages and the second into whole frames, and
 * it owns the opening handshake, the close, and the ping the other end
 * has to hear back. A host program does the reading and writing.
 *
 * That split is the same one the relay core already makes, and for the
 * same reason: the interesting states, a handshake split across two
 * reads, a frame split across three, a peer that never says hello, are
 * states a test can produce exactly and a socket produces by luck.
 *
 * Both ends use it. A server answers a handshake and expects masked
 * frames, a client sends one and expects unmasked frames.
 */

/* One protocol message is capped at 64 KB, and a frame carries at most
 * one message plus its header. The read buffer holds a whole frame,
 * because the parser needs one before it can hand anything over. */
#define TAK_WSCONN_IN_CAP   (TAK_WS_PAYLOAD_MAX + TAK_WS_HEADER_MAX)
/* A turn bundle for eight seats, several deep, plus room for a lobby
 * snapshot going out behind it. */
#define TAK_WSCONN_OUT_CAP  (128 * 1024)

typedef enum {
    TAK_WSCONN_HANDSHAKE = 0,  /* still reading or writing the upgrade */
    TAK_WSCONN_OPEN,           /* frames flow                          */
    TAK_WSCONN_CLOSING,        /* a close went out, drain and go       */
    TAK_WSCONN_DEAD            /* the host should drop the socket      */
} TAK_WsConnState;

typedef enum {
    TAK_WSCONN_OK = 0,
    TAK_WSCONN_NEED_MORE,      /* nothing whole yet, read again        */
    TAK_WSCONN_MESSAGE,        /* one message in msg/msg_len           */
    TAK_WSCONN_CLOSED,         /* the peer closed, or we refused it    */
    TAK_WSCONN_ERROR           /* protocol error, the socket goes      */
} TAK_WsConnStep;

typedef struct TAK_WsConn {
    TAK_WsConnState state;
    uint8_t  is_server;        /* answers a handshake, expects masks   */
    uint8_t  sent_close;
    uint8_t  got_close;
    /* The client's key, so a client can check the answer it gets. */
    char     key[TAK_WS_KEY_CHARS];
    /* Masking keys a client needs. Counted, not random: the mask is a
     * proxy cache defence in the RFC, not a secret, and a predictable
     * one is still a different four bytes per frame. */
    uint32_t mask_counter;

    uint8_t  in[TAK_WSCONN_IN_CAP];
    size_t   in_len;
    /* Where the unparsed bytes start. Parsing advances this rather
     * than moving the buffer down, so a message handed back still
     * points at bytes nobody has touched. */
    size_t   in_pos;
    uint8_t  out[TAK_WSCONN_OUT_CAP];
    size_t   out_len;

    /* Set when a step returns TAK_WSCONN_MESSAGE. It points into `in`,
     * so it is good until the next Step or Feed on this connection and
     * no longer. Copy it if you need it after that. */
    const uint8_t *msg;
    size_t         msg_len;

    /* Why the connection went, for a log line. Never shown to a
     * player: the protocol's own reject codes are what a player sees. */
    const char *why;
} TAK_WsConn;

/* A server connection, waiting for an upgrade request. */
void TAK_WsConn_InitServer(TAK_WsConn *c);

/* A client connection. Writes its upgrade request into the out buffer,
 * so the host has something to send at once. `nonce` is 16 bytes the
 * caller supplies, since this module has no randomness of its own. */
int  TAK_WsConn_InitClient(TAK_WsConn *c, const char *host, const char *path,
                           const uint8_t nonce[16]);

/* Hand over bytes that arrived. Returns 0 on success, -1 when they do
 * not fit, which is itself a peer we drop. This may move the buffer
 * down, so a message from an earlier Step does not survive it. */
int  TAK_WsConn_Feed(TAK_WsConn *c, const void *data, size_t len);

/* Take the next thing out of what has been fed. Call until it answers
 * NEED_MORE. A MESSAGE leaves the payload in c->msg. */
TAK_WsConnStep TAK_WsConn_Step(TAK_WsConn *c);

/* Queue one protocol message. Returns 0 on success, -1 when the out
 * buffer is full, which means the peer is not reading and the host
 * should drop it rather than grow. */
int  TAK_WsConn_Send(TAK_WsConn *c, const void *payload, size_t len);

/* Queue a close. The host keeps writing until out_len is 0, then
 * drops the socket. */
void TAK_WsConn_Close(TAK_WsConn *c, uint16_t code);

/* Bytes the host should write, and how many of them it managed. */
const uint8_t *TAK_WsConn_Pending(const TAK_WsConn *c, size_t *len);
void TAK_WsConn_Wrote(TAK_WsConn *c, size_t len);

#endif /* TAK_WS_CONN_H */
