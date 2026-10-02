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
#include "tak_net_ledger.h"
#include "tak_net_http.h"
#include "tak_ws_conn.h"
#include "tak_mod_proxy.h"
#include "tak_mod_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define MAX_CONNS   TAK_RELAY_CLIENTS_MAX
/* One more for the listener, which sits at index 0 of the wait set. */
#define WAIT_SLOTS  (MAX_CONNS + 1)

/* Each room keeps a turn log for reconnect and replay, and a rejoin
 * replays it from the first turn, so it has to hold a whole game: about
 * an hour of an eight seat match at 20 turns a second. */
#define LOG_ARENA_PER_ROOM   (4u << 20)
#define LOG_ENTRIES_PER_ROOM 131072u

typedef struct {
    TakSocket   sock;
    TAK_ConnId  id;
    TAK_WsConn  ws;
    int         in_use;
    int         proxy;      /* streaming a mod download, reads nothing more */
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

/* Every finished match, and the answer to the last HTTP request. */
static TAK_Ledger g_ledger;
static char       g_http_out[TAK_HTTP_RESPONSE_MAX + 1024];

static volatile sig_atomic_t g_stop;

/* The mod registry, built in from web/mods/registry.json unless
 * --mod-registry names another, and the downloads it allows. */
extern const char *const tak_mod_registry_json;
extern const size_t tak_mod_registry_json_len;
static TAK_ModRegistry g_mods;
static TAK_ModProxy    g_proxy;
static char           *g_mods_file;

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
    /* A mod download never says hello, so the relay's hello timeout
     * would end it part way. The proxy ends it instead. */
    if (c && !c->proxy) TAK_WsConn_Close(&c->ws, 1000);
}

static void drop(Conn *c, uint64_t now, const char *why) {
    if (!c->in_use) return;
    if (why) {
        fprintf(stderr, "conn %u closed: %s\n", (unsigned)c->id, why);
    }
    if (c->proxy) TAK_ModFetch_Stop(TAK_ModProxy_Cancel(&g_proxy, c->id));
    c->proxy = 0;
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
        c->proxy = 0;
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
        if (c->proxy) continue;
        /* A plain request is the leaderboard page asking. One answer,
         * then the connection drains and goes. A mod download streams
         * for as long as it takes and then goes. */
        const uint8_t *req = NULL;
        size_t req_len = 0;
        if (TAK_WsConn_PlainRequest(&c->ws, &req, &req_len) &&
            TAK_ModProxy_IsDownload(req, req_len)) {
            size_t an = 0;
            int slot = TAK_ModProxy_Begin(&g_proxy, c->id, req, req_len,
                                          g_http_out, sizeof g_http_out, &an);
            if (slot >= 0) {
                c->proxy = 1;
                if (TAK_ModFetch_Start(&g_proxy, slot) != 0)
                    TAK_ModProxy_Done(&g_proxy, slot, 0, "the server could not start the download");
            } else if (an == 0 || TAK_WsConn_Answer(&c->ws, g_http_out, an) != 0) {
                drop(c, now, "an answer that did not fit");
            }
            return;
        }
        if (req) {
            static TAK_HttpLive live;
            TAK_Relay_Live(&g_server.relay, &live);
            size_t rn = TAK_Http_AnswerLive(&g_ledger, &live, req, req_len,
                                            g_http_out, sizeof g_http_out);
            if (rn == 0 || TAK_WsConn_Answer(&c->ws, g_http_out, rn) != 0)
                drop(c, now, "an answer that did not fit");
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

/* Where a mod download's bytes go: the connection's out buffer, as much
 * as fits, and a close behind the last. */
static size_t proxy_write(void *ctx, uint32_t conn, const void *bytes, size_t len, int last) {
    (void)ctx;
    Conn *c = conn_by_id(conn);
    if (!c || !c->proxy) return len;
    size_t n = len ? TAK_WsConn_Stream(&c->ws, bytes, len) : 0;
    if (last) (void)TAK_WsConn_Answer(&c->ws, "", 0);
    return n;
}

/* The registry, from a file when --mod-registry names one. */
static void load_mods(const char *path) {
    const char *text = tak_mod_registry_json;
    size_t len = tak_mod_registry_json_len;
    if (path) {
        FILE *fp = fopen(path, "rb");
        long n = -1;
        if (fp && fseek(fp, 0, SEEK_END) == 0) n = ftell(fp);
        if (fp && n > 0 && n < (long)TAK_HTTP_RESPONSE_MAX && fseek(fp, 0, SEEK_SET) == 0 &&
            (g_mods_file = (char *)malloc((size_t)n + 1)) != NULL &&
            fread(g_mods_file, 1, (size_t)n, fp) == (size_t)n) {
            g_mods_file[n] = '\0';
            text = g_mods_file;
            len = (size_t)n;
        } else {
            fprintf(stderr, "mod registry: could not read %s, using the built in one\n", path);
        }
        if (fp) fclose(fp);
    }
    if (TAK_ModRegistry_Parse(text, len, &g_mods) < 0) {
        fprintf(stderr, "mod registry: not a registry, no mods are offered\n");
        memset(&g_mods, 0, sizeof g_mods);
        text = NULL;
        len = 0;
    } else if (g_mods.errors) {
        fprintf(stderr, "mod registry: %d entries left out, first: %s\n",
                g_mods.errors, g_mods.first_error);
    }
    TAK_Http_SetModRegistry(text, len);
    TAK_ModProxy_Init(&g_proxy, &g_mods, TAK_ModFetch_Available());
    printf("mod registry: %d mods, downloads %s\n", g_mods.count,
           g_proxy.can_fetch ? "streamed" : "not available in this build");
}

static void usage(const char *argv0) {
    printf("usage: %s [--port N] [--name TEXT] [--motd TEXT] [--key TEXT]"
           " [--store PATH] [--mod-registry PATH]\n", argv0);
    printf("  --port  which port to listen on, default 8443\n");
    printf("  --name  the server name clients see\n");
    printf("  --motd  the message of the day\n");
    printf("  --key   an access key clients must present, default none\n");
    printf("  --store the file finished matches are kept in, for the\n"
           "          leaderboard. Without it results last until restart.\n");
    printf("  --mod-registry  a registry file to offer in place of the one\n"
           "          this build carries\n");
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
    const char *store = NULL;
    const char *mods_path = NULL;

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
        } else if (strcmp(a, "--store") == 0 && has_next) {
            store = argv[++i];
        } else if (strcmp(a, "--mod-registry") == 0 && has_next) {
            mods_path = argv[++i];
        } else {
            printf("unknown option \"%s\"\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (store && TAK_Ledger_Open(&g_ledger, store) == 0) {
        printf("store %s holds %u finished matches\n", store, (unsigned)g_ledger.count);
        if (g_ledger.bad_records || g_ledger.bad_bytes)
            fprintf(stderr, "store: skipped %u records and %u stray bytes\n",
                    (unsigned)g_ledger.bad_records, (unsigned)g_ledger.bad_bytes);
    } else if (store) {
        /* A store that will not open must not stop the games. The file
         * is left as it is for someone to look at. */
        TAK_Ledger_Init(&g_ledger);
        fprintf(stderr, "STORE UNUSABLE: could not open %s, it is untouched and\n"
                        "results are kept in memory only until restart\n", store);
    } else {
        TAK_Ledger_Init(&g_ledger);
        printf("no --store, results are kept until restart\n");
    }
    /* The host clock counts from boot. The ledger wants the wall clock,
     * so the difference is measured once and added on. */
    cfg.wall_offset_ms = (uint64_t)time(NULL) * 1000u - TakNet_NowMs();

    load_mods(mods_path);

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
    TAK_Relay_SetLedger(&g_server.relay, &g_ledger);

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

        TAK_ModFetch_Pump(&g_proxy);
        TAK_ModProxy_Drain(&g_proxy, proxy_write, NULL, now);

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
    TAK_Ledger_Close(&g_ledger);
    return 0;
}
