/*
 * server_main.c -- okrelay, the dedicated relay server.
 *
 * It owns a listening socket and a set of connections. Everything it
 * knows about a game it asks the relay core, which holds no
 * simulation, no game state and no game data, and never reads any.
 *
 * The loop is the whole program: accept what is waiting, read what
 * arrived, hand whole messages to the core, write what the core
 * queued, and tick the clock. That is deliberately dull, because the
 * parts worth getting right are tested with no network in the room.
 * The room rules, the turn clock and the reconnect log are covered by
 * test_relay_loopback over a fake network with latency, loss,
 * duplication and reordering. The wire format is covered by test_ws
 * against the RFC's own vectors, and the chunking by test_ws_conn.
 *
 * Nothing about a particular deployment belongs here. No domain, no
 * key and no path: they are arguments, and the run book that holds
 * their values is not in this repository.
 */

#include "net_socket.h"

#include "tak_net_relay.h"
#include "tak_ws_conn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define MAX_CONNS   TAK_RELAY_CLIENTS_MAX
/* One more for the listener, which sits at index 0 of the wait set. */
#define WAIT_SLOTS  (MAX_CONNS + 1)

/* Each room keeps a turn log for reconnect and replay. Half a megabyte
 * a room holds several minutes of an eight seat match, which is longer
 * than any reconnect window the room options allow. */
#define LOG_ARENA_PER_ROOM   (512u << 10)
#define LOG_ENTRIES_PER_ROOM 32768u

typedef struct {
    TakSocket   sock;
    TAK_ConnId  id;
    TAK_WsConn  ws;
    int         in_use;
} Conn;

typedef struct {
    Conn        conn[MAX_CONNS];
    TAK_Relay   relay;
    TakSocket   listener;
} Server;

/* One server, because it holds about forty megabytes of connection
 * buffers and turn logs and there is exactly one of it. */
static Server g_server;

static uint8_t          g_log_arena[TAK_RELAY_ROOMS_MAX * LOG_ARENA_PER_ROOM];
static TAK_TurnLogEntry g_log_entries[TAK_RELAY_ROOMS_MAX * LOG_ENTRIES_PER_ROOM];

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static Conn *conn_by_id(TAK_ConnId id) {
    if (id == 0) return NULL;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_server.conn[i].in_use && g_server.conn[i].id == id) {
            return &g_server.conn[i];
        }
    }
    return NULL;
}

/* The transport the relay answers through. It queues into the
 * connection's out buffer and the loop does the writing, so the core
 * never blocks on a slow client. */
static int tx_send(void *ctx, TAK_ConnId id, const uint8_t *frame, size_t len) {
    (void)ctx;
    Conn *c = conn_by_id(id);
    if (!c) return -1;
    if (TAK_WsConn_Send(&c->ws, frame, len) != 0) {
        /* The peer has stopped reading and the queue is full. Dropping
         * it is the honest answer: growing a queue for someone who is
         * not there is how a relay runs out of memory. */
        TAK_WsConn_Close(&c->ws, 1011);
        return -1;
    }
    return 0;
}

static void tx_close(void *ctx, TAK_ConnId id) {
    (void)ctx;
    Conn *c = conn_by_id(id);
    if (c) TAK_WsConn_Close(&c->ws, 1000);
}

static void drop(Conn *c, uint64_t now, const char *why) {
    if (!c->in_use) return;
    if (why) {
        fprintf(stderr, "conn %u closed: %s\n", (unsigned)c->id, why);
    }
    TAK_Relay_OnClose(&g_server.relay, c->id, now);
    TakNet_Close(c->sock);
    c->sock = TAK_INVALID_SOCKET;
    c->in_use = 0;
}

static void accept_waiting(uint64_t now) {
    for (;;) {
        TakSocket s = TakNet_Accept(g_server.listener);
        if (s == TAK_INVALID_SOCKET) return;
        int slot = -1;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (!g_server.conn[i].in_use) { slot = i; break; }
        }
        if (slot < 0) {
            /* Full. Closing at once is kinder than a queue that never
             * moves, and the client sees a refused connection rather
             * than a hang. */
            TakNet_Close(s);
            continue;
        }
        Conn *c = &g_server.conn[slot];
        c->sock = s;
        /* Ids are the slot plus one, so zero is never a connection,
         * and a slot reused later gets a fresh id from the relay's own
         * session counter rather than from this. */
        c->id = (TAK_ConnId)(slot + 1);
        c->in_use = 1;
        TAK_WsConn_InitServer(&c->ws);
        TAK_Relay_OnConnect(&g_server.relay, c->id, now);
    }
}

/* Read what arrived and hand whole messages to the core. */
static void read_conn(Conn *c, uint64_t now) {
    uint8_t buf[16384];
    for (;;) {
        int n = TakNet_Recv(c->sock, buf, sizeof buf);
        if (n == -2) break;                       /* nothing more */
        if (n == 0)  { drop(c, now, "peer closed"); return; }
        if (n < 0)   { drop(c, now, "read failed"); return; }
        if (TAK_WsConn_Feed(&c->ws, buf, (size_t)n) != 0) {
            drop(c, now, c->ws.why ? c->ws.why : "fed more than it can hold");
            return;
        }
        for (;;) {
            TAK_WsConnStep s = TAK_WsConn_Step(&c->ws);
            if (s == TAK_WSCONN_NEED_MORE) break;
            if (s == TAK_WSCONN_ERROR) {
                drop(c, now, c->ws.why ? c->ws.why : "protocol error");
                return;
            }
            if (s == TAK_WSCONN_MESSAGE) {
                TAK_Relay_OnFrame(&g_server.relay, c->id,
                                  c->ws.msg, c->ws.msg_len, now);
                /* The relay may have closed it while answering. */
                if (!c->in_use) return;
            }
            /* A close leaves the connection draining. The write side
             * finishes what is queued and the loop retires it. */
        }
        if ((size_t)n < sizeof buf) break;
    }
}

/* Write what is queued. A short write is ordinary and the rest stays. */
static void write_conn(Conn *c, uint64_t now) {
    for (;;) {
        size_t len = 0;
        const uint8_t *p = TAK_WsConn_Pending(&c->ws, &len);
        if (len == 0) break;
        int n = TakNet_Send(c->sock, p, len);
        if (n == -2) break;
        if (n < 0) { drop(c, now, "write failed"); return; }
        TAK_WsConn_Wrote(&c->ws, (size_t)n);
        if ((size_t)n < len) break;
    }
    /* A closing connection with nothing left to say can go. */
    size_t left = 0;
    (void)TAK_WsConn_Pending(&c->ws, &left);
    if (left == 0 && (c->ws.state == TAK_WSCONN_CLOSING ||
                      c->ws.state == TAK_WSCONN_DEAD)) {
        drop(c, now, NULL);
    }
}

static void usage(const char *argv0) {
    printf("usage: %s [--port N] [--name TEXT] [--motd TEXT] [--key TEXT]\n",
           argv0);
    printf("  --port  which port to listen on, default 8443\n");
    printf("  --name  the server name clients see\n");
    printf("  --motd  the message of the day\n");
    printf("  --key   an access key clients must present, default none\n");
    printf("\nTLS is not terminated here. Put a reverse proxy in front\n"
           "for wss, which is what a browser on a secure page needs.\n");
}

static void copy_arg(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int main(int argc, char **argv) {
    unsigned short port = 8443;
    TAK_RelayCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    copy_arg(cfg.server_name, sizeof cfg.server_name, "OpenKingdoms relay");
    copy_arg(cfg.motd, sizeof cfg.motd, "");
    cfg.seed = 1u;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int has_next = (i + 1 < argc);
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--port") == 0 && has_next) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 1 || v > 65535) { usage(argv[0]); return 2; }
            port = (unsigned short)v;
        } else if (strcmp(a, "--name") == 0 && has_next) {
            copy_arg(cfg.server_name, sizeof cfg.server_name, argv[++i]);
        } else if (strcmp(a, "--motd") == 0 && has_next) {
            copy_arg(cfg.motd, sizeof cfg.motd, argv[++i]);
        } else if (strcmp(a, "--key") == 0 && has_next) {
            copy_arg(cfg.access_key, sizeof cfg.access_key, argv[++i]);
        } else if (strcmp(a, "--seed") == 0 && has_next) {
            cfg.seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            printf("unknown option \"%s\"\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (TakNet_Start() != 0) {
        fprintf(stderr, "could not start the network layer\n");
        return 1;
    }
    g_server.listener = TakNet_Listen(port);
    if (g_server.listener == TAK_INVALID_SOCKET) {
        fprintf(stderr, "could not listen on port %u\n", (unsigned)port);
        TakNet_Stop();
        return 1;
    }
    for (int i = 0; i < MAX_CONNS; i++) g_server.conn[i].sock = TAK_INVALID_SOCKET;

    TAK_NetTransport tx;
    tx.ctx = NULL;
    tx.send = tx_send;
    tx.close = tx_close;
    TAK_Relay_Init(&g_server.relay, &cfg, tx,
                   g_log_arena, sizeof g_log_arena,
                   g_log_entries,
                   (uint32_t)(sizeof g_log_entries / sizeof g_log_entries[0]));

    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif

    printf("%s listening on port %u, up to %d clients and %d rooms\n",
           cfg.server_name, (unsigned)port, MAX_CONNS, TAK_RELAY_ROOMS_MAX);
    fflush(stdout);

    TakSocket      socks[WAIT_SLOTS];
    unsigned char  want_write[WAIT_SLOTS];
    unsigned char  readable[WAIT_SLOTS];
    unsigned char  writable[WAIT_SLOTS];

    while (!g_stop) {
        socks[0] = g_server.listener;
        want_write[0] = 0;
        for (int i = 0; i < MAX_CONNS; i++) {
            Conn *c = &g_server.conn[i];
            socks[i + 1] = c->in_use ? c->sock : TAK_INVALID_SOCKET;
            size_t len = 0;
            if (c->in_use) (void)TAK_WsConn_Pending(&c->ws, &len);
            want_write[i + 1] = (unsigned char)(len > 0);
        }

        /* The turn clock closes a turn every 3 ticks, 50 ms at 60 Hz,
         * so the loop must come round well inside that even when
         * nothing is arriving. */
        int ready = TakNet_Wait(socks, want_write, readable, writable,
                                WAIT_SLOTS, 10);
        uint64_t now = TakNet_NowMs();
        if (ready < 0) {
            /* An interrupted wait is ordinary. Anything else and the
             * next pass will find out what went wrong. */
            continue;
        }

        if (readable[0]) accept_waiting(now);

        for (int i = 0; i < MAX_CONNS; i++) {
            Conn *c = &g_server.conn[i];
            if (!c->in_use) continue;
            if (readable[i + 1]) read_conn(c, now);
            if (c->in_use && writable[i + 1]) write_conn(c, now);
        }

        TAK_Relay_Tick(&g_server.relay, now);

        /* The tick may have queued frames for connections that were
         * not writable a moment ago. Trying them now costs one failed
         * send and saves a whole turn of latency. */
        for (int i = 0; i < MAX_CONNS; i++) {
            Conn *c = &g_server.conn[i];
            if (c->in_use) write_conn(c, now);
        }
    }

    printf("stopping\n");
    uint64_t now = TakNet_NowMs();
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_server.conn[i].in_use) drop(&g_server.conn[i], now, NULL);
    }
    TakNet_Close(g_server.listener);
    TakNet_Stop();
    return 0;
}
