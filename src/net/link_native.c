/*
 * link_native.c -- a link over a real socket, for the desktop builds.
 *
 * A TCP socket with ws_conn over it. Everything hard about the framing
 * and the handshake is in ws_conn, which is tested against the RFC's
 * own vectors with no network in the room, so this is the dull part:
 * connect, read, write, and hand whole messages up.
 */

#ifndef __EMSCRIPTEN__

#include "tak_net_link.h"
#include "tak_ws_conn.h"
#include "net_socket.h"

#include <stdio.h>
#include <string.h>

/* One link, because a client is in one session at a time. */
static struct {
    TakSocket     sock;
    TAK_WsConn    ws;
    TAK_LinkState state;
    const char   *why;
} g_link;

static void fail(const char *why) {
    if (g_link.sock != TAK_INVALID_SOCKET) TakNet_Close(g_link.sock);
    g_link.sock = TAK_INVALID_SOCKET;
    g_link.state = TAK_LINK_FAILED;
    if (!g_link.why) g_link.why = why;
}

/* ws://host[:port][/path]. Returns 0 on success. */
static int parse_url(const char *url, char *host, size_t host_cap,
                     unsigned short *port, char *path, size_t path_cap) {
    if (!url) return -1;
    const char *p = url;
    *port = 80;
    if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        /* No TLS here on purpose. A desktop client reaches a secure
         * server the way a browser does, through the proxy in front. */
        return -2;
    }
    size_t hi = 0;
    while (*p && *p != ':' && *p != '/') {
        if (hi + 1 >= host_cap) return -1;
        host[hi++] = *p++;
    }
    host[hi] = '\0';
    if (hi == 0) return -1;
    if (*p == ':') {
        p++;
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p++ - '0'); }
        if (v == 0 || v > 65535) return -1;
        *port = (unsigned short)v;
    }
    if (*p == '/') {
        size_t pi = 0;
        while (*p) {
            if (pi + 1 >= path_cap) return -1;
            path[pi++] = *p++;
        }
        path[pi] = '\0';
    } else {
        if (path_cap < 2) return -1;
        path[0] = '/';
        path[1] = '\0';
    }
    return 0;
}

int TAK_NetLink_Open(const char *url) {
    memset(&g_link, 0, sizeof g_link);
    g_link.sock = TAK_INVALID_SOCKET;
    g_link.why = NULL;

    char host[128], path[128];
    unsigned short port = 0;
    int rc = parse_url(url, host, sizeof host, &port, path, sizeof path);
    if (rc == -2) { g_link.state = TAK_LINK_FAILED;
                    g_link.why = "wss needs a proxy in front, not this build";
                    return -1; }
    if (rc != 0) { g_link.state = TAK_LINK_FAILED;
                   g_link.why = "that is not a WebSocket address";
                   return -1; }

    if (TakNet_Start() != 0) {
        g_link.state = TAK_LINK_FAILED;
        g_link.why = "the network layer would not start";
        return -1;
    }

    g_link.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_link.sock == TAK_INVALID_SOCKET) {
        fail("no socket");
        return -1;
    }
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        fail("that name did not resolve");
        return -1;
    }
    int connected = connect(g_link.sock, res->ai_addr,
                            (int)res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!connected) {
        fail("nothing answered there");
        return -1;
    }
    /* Connected blocking, because there is nothing to do until it is
     * up, and non blocking from here because the pump runs inside a
     * frame and must never wait in one. */
    if (TakNet_SetNonBlocking(g_link.sock) != 0) {
        fail("the socket would not stop blocking");
        return -1;
    }
    /* Turns are small and latency is the point, so no Nagle. */
    int on = 1;
    setsockopt(g_link.sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&on,
               sizeof on);

    /* The masking key is a proxy cache defence rather than a secret, so
     * a counter seeded from the clock is what this needs and no more. */
    uint8_t nonce[16];
    unsigned long long t = TakNet_NowMs();
    for (int i = 0; i < 16; i++) {
        nonce[i] = (uint8_t)((t >> ((i & 7) * 8)) ^ (unsigned)(i * 37 + 11));
    }
    if (TAK_WsConn_InitClient(&g_link.ws, host, path, nonce) != 0) {
        fail("the upgrade request would not fit");
        return -1;
    }
    g_link.state = TAK_LINK_OPENING;
    return 0;
}

TAK_LinkState TAK_NetLink_State(void) { return g_link.state; }

const char *TAK_NetLink_Why(void) {
    return g_link.why ? g_link.why : "";
}

void TAK_NetLink_Close(void) {
    if (g_link.state == TAK_LINK_OPEN) TAK_WsConn_Close(&g_link.ws, 1000);
    if (g_link.sock != TAK_INVALID_SOCKET) TakNet_Close(g_link.sock);
    g_link.sock = TAK_INVALID_SOCKET;
    g_link.state = TAK_LINK_CLOSED;
}

void TAK_NetLink_Pump(TAK_NetClient *c, uint64_t now_ms) {
    if (g_link.state != TAK_LINK_OPENING && g_link.state != TAK_LINK_OPEN) {
        return;
    }

    /* Read, then step, then send, then write. In that order an answer
     * to something that arrived this frame leaves in the same frame,
     * and the handshake completing does not cost the queued HELLO a
     * frame of its own. */
    for (;;) {
        uint8_t buf[16384];
        int n = TakNet_Recv(g_link.sock, buf, sizeof buf);
        if (n == -2) break;
        if (n == 0) { fail("the server closed the connection");
                      TAK_NetClient_OnClose(c); return; }
        if (n < 0)  { fail("the connection went while reading");
                      TAK_NetClient_OnClose(c); return; }
        if (TAK_WsConn_Feed(&g_link.ws, buf, (size_t)n) != 0) {
            fail(g_link.ws.why ? g_link.ws.why : "more than a frame can hold");
            TAK_NetClient_OnClose(c);
            return;
        }
        if ((size_t)n < sizeof buf) break;
    }

    for (;;) {
        TAK_WsConnStep st = TAK_WsConn_Step(&g_link.ws);
        if (st == TAK_WSCONN_NEED_MORE) break;
        if (st == TAK_WSCONN_ERROR || st == TAK_WSCONN_CLOSED) {
            fail(g_link.ws.why ? g_link.ws.why : "the connection ended");
            TAK_NetClient_OnClose(c);
            return;
        }
        if (st == TAK_WSCONN_MESSAGE) {
            if (TAK_NetClient_OnMessage(c, g_link.ws.msg, g_link.ws.msg_len,
                                        now_ms) != 0) {
                fail("a message this build could not read");
                TAK_NetClient_OnClose(c);
                return;
            }
        }
    }
    if (g_link.state == TAK_LINK_OPENING &&
        g_link.ws.state == TAK_WSCONN_OPEN) {
        g_link.state = TAK_LINK_OPEN;
    }

    if (g_link.state == TAK_LINK_OPEN) {
        uint8_t msg[TAK_NET_FRAME_MAX];
        size_t n;
        while ((n = TAK_NetClient_TakeMessage(c, msg, sizeof msg)) > 0) {
            if (TAK_WsConn_Send(&g_link.ws, msg, n) != 0) {
                /* The peer has stopped reading and the queue is full.
                 * Growing it for someone who is not there is how a
                 * client runs out of memory. */
                fail("the send queue filled, so the peer is not reading");
                TAK_NetClient_OnClose(c);
                return;
            }
        }
    }

    for (;;) {
        size_t len = 0;
        const uint8_t *p = TAK_WsConn_Pending(&g_link.ws, &len);
        if (len == 0) break;
        int n = TakNet_Send(g_link.sock, p, len);
        if (n == -2) break;
        if (n < 0) { fail("the connection went while writing");
                     TAK_NetClient_OnClose(c); return; }
        TAK_WsConn_Wrote(&g_link.ws, (size_t)n);
        if ((size_t)n < len) break;
    }
}

#endif /* not __EMSCRIPTEN__ */

/* Both halves are compiled everywhere and one of them is empty, which
 * ISO C does not allow a translation unit to be. */
typedef int tak_link_translation_unit_is_not_empty;

