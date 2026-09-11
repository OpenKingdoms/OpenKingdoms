#ifndef TAK_NET_TRANSPORT_H
#define TAK_NET_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/*
 * What the relay core needs from a network, and nothing more.
 *
 * The core never opens a socket. A host program owns the connections and
 * calls into the relay when one opens, closes or delivers a frame. The
 * relay answers through this interface. That is what lets the eight client
 * loopback test run the real room and turn logic in one process over the
 * fake network below, and what lets a WebSocket server, or a native client
 * hosting on a local network, drive the same core later.
 */

typedef uint32_t TAK_ConnId;           /* 0 is never a connection */

typedef struct TAK_NetTransport {
    void *ctx;
    /* Queue one whole frame for a connection. Returns 0 on success. */
    int  (*send)(void *ctx, TAK_ConnId conn, const uint8_t *frame, size_t len);
    /* Close a connection from the server's side. */
    void (*close)(void *ctx, TAK_ConnId conn);
} TAK_NetTransport;

/* ── The fake network ─────────────────────────────────────────────────────
 * In process, driven by virtual time. Frames stay in order on their
 * connection, the way a WebSocket over TCP delivers them, and jitter only
 * varies how late they arrive. Loss, duplication and reordering are
 * injected on the way to the server, which is the direction the relay has
 * to survive. A test can also mute a client, cut its connection, or push
 * any bytes it likes at the relay. Test code, so it allocates. */

#define TAK_FAKE_CONNS_MAX 64

typedef struct TAK_FakeNetCfg {
    uint32_t latency_ms;        /* one way */
    uint32_t jitter_ms;         /* extra delay, uniform in 0..jitter */
    uint32_t loss_permille;     /* client to server */
    uint32_t dup_permille;      /* client to server */
    uint32_t reorder_permille;  /* client to server, held back by 3x latency */
    uint32_t seed;
} TAK_FakeNetCfg;

typedef struct TAK_FakePacket TAK_FakePacket;

typedef struct TAK_FakeConn {
    uint8_t  open;              /* both ends still there */
    uint8_t  muted;             /* the client stops talking */
    uint8_t  closed_by_server;
    uint8_t  client_gone;       /* the client end closed */
    uint64_t last_to_client;    /* keeps each direction in order */
    uint64_t last_to_server;
} TAK_FakeConn;

typedef struct TAK_FakeNet {
    TAK_FakeNetCfg  cfg;
    uint64_t        now;
    uint64_t        order;
    uint32_t        rng;
    uint32_t        next_id;
    TAK_FakeConn    conn[TAK_FAKE_CONNS_MAX + 1];   /* index is the id */
    TAK_FakePacket *head;       /* in flight, sorted by due time */
    uint32_t        in_flight;
    uint32_t        sent_to_server;
    uint32_t        lost;
    uint32_t        duplicated;
    uint32_t        reordered;
} TAK_FakeNet;

/* Where Pump delivers. `to_server` frames go to the relay, the rest to a
 * client's inbox. A NULL frame with len 0 means the connection closed. */
typedef void (*TAK_FakeDeliver)(void *user, TAK_ConnId conn, int to_server,
                                const uint8_t *frame, size_t len);

void TAK_FakeNet_Init(TAK_FakeNet *n, const TAK_FakeNetCfg *cfg);
void TAK_FakeNet_Free(TAK_FakeNet *n);

/* The server's side, to hand to the relay. */
TAK_NetTransport TAK_FakeNet_Server(TAK_FakeNet *n);

/* A client opens a connection. Returns its id, or 0 when out of ids. */
TAK_ConnId TAK_FakeNet_Connect(TAK_FakeNet *n);
/* A client sends a frame at `now`. Faults are applied here. */
void TAK_FakeNet_ClientSend(TAK_FakeNet *n, TAK_ConnId c,
                            const uint8_t *frame, size_t len, uint64_t now);
/* A client's connection drops. The server hears after the latency. */
void TAK_FakeNet_ClientClose(TAK_FakeNet *n, TAK_ConnId c, uint64_t now);
void TAK_FakeNet_SetMuted(TAK_FakeNet *n, TAK_ConnId c, int muted);
int  TAK_FakeNet_IsOpen(const TAK_FakeNet *n, TAK_ConnId c);

/* Deliver everything due by `now`, in due order. */
void TAK_FakeNet_Pump(TAK_FakeNet *n, uint64_t now,
                      TAK_FakeDeliver deliver, void *user);

#endif /* TAK_NET_TRANSPORT_H */
