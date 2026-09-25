/*
 * relay_smoke.c -- does a real client reach a real relay over a real
 * socket, and get the answers the protocol says it should?
 *
 * Everything below this is already tested without a network: the wire
 * format against the RFC's own vectors, the chunking against split
 * reads, and the room and turn rules against a fake network with loss
 * and reordering. What none of those can say is whether the socket
 * loop, the connection pump and the relay core are wired to each
 * other. That is what this is for, and it is the only test here that
 * opens a port.
 *
 * It is a program rather than a ctest case on purpose. It needs a
 * server to talk to, and a test that starts one has to pick a port,
 * which is a thing that fails on a shared machine for reasons that
 * have nothing to do with the code.
 *
 *   okrelay --port 8799 &
 *   relay_smoke 127.0.0.1 8799
 *
 * With --hold N it keeps its room open for N seconds afterwards,
 * answering the heartbeat, so a page can be pointed at a live game.
 */

#include "net_socket.h"

#include "tak_net_protocol.h"
#include "tak_ws_conn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void ok(int cond, const char *what) {
    printf("%-52s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) g_fail++;
}

static TAK_WsConn g_conn;

/* Connect, blocking, since there is nothing else to do until it is up.
 * By name as well as by address, so a deployed relay can be checked. */
static TakSocket dial(const char *host, unsigned short port) {
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        return TAK_INVALID_SOCKET;
    }
    TakSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == TAK_INVALID_SOCKET) {
        freeaddrinfo(res);
        return TAK_INVALID_SOCKET;
    }
    int connected = connect(s, res->ai_addr, (int)res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!connected) {
        TakNet_Close(s);
        return TAK_INVALID_SOCKET;
    }
    return s;
}

/* Write everything queued. 0 on success. */
static int pump_out(TakSocket s) {
    for (;;) {
        size_t len = 0;
        const uint8_t *p = TAK_WsConn_Pending(&g_conn, &len);
        if (len == 0) return 0;
        int n = TakNet_Send(s, p, len);
        if (n == -2) return 0;
        if (n <= 0) return -1;
        TAK_WsConn_Wrote(&g_conn, (size_t)n);
    }
}

/* Read once, with a bounded wait. 0 when nothing arrived in time. */
static int pump_in(TakSocket s, int ms) {
    TakSocket one[1] = { s };
    unsigned char w[1] = { 0 }, r[1] = { 0 }, ww[1] = { 0 };
    if (TakNet_Wait(one, w, r, ww, 1, ms) <= 0) return 0;
    if (!r[0]) return 0;
    uint8_t buf[8192];
    int n = TakNet_Recv(s, buf, sizeof buf);
    if (n == -2) return 0;
    if (n <= 0) return -1;
    return TAK_WsConn_Feed(&g_conn, buf, (size_t)n) == 0 ? n : -1;
}

/* Until the upgrade is answered. The server says nothing of its own
 * accord before that, so this waits on the handshake and not on a
 * message. Waiting on a message here is how the first draft of this
 * sat silent until the relay's ten second HELLO timeout closed it. */
static int wait_open(TakSocket s, int ms) {
    if (pump_out(s) != 0) return -1;
    unsigned long long deadline = TakNet_NowMs() + (unsigned long long)ms;
    while (g_conn.state == TAK_WSCONN_HANDSHAKE) {
        for (;;) {
            TAK_WsConnStep st = TAK_WsConn_Step(&g_conn);
            if (st == TAK_WSCONN_NEED_MORE) break;
            if (st == TAK_WSCONN_ERROR || st == TAK_WSCONN_CLOSED) return -1;
            if (g_conn.state == TAK_WSCONN_OPEN) return 0;
        }
        if (TakNet_NowMs() > deadline) return -1;
        if (pump_in(s, 20) < 0) return -1;
    }
    return g_conn.state == TAK_WSCONN_OPEN ? 0 : -1;
}

/* Push everything queued, then wait up to `ms` for one message.
 * Returns the message type, or 0 when nothing came. */
static int exchange(TakSocket s, uint8_t *out_payload, size_t *out_len,
                    int ms) {
    if (pump_out(s) != 0) return -1;
    unsigned long long deadline = TakNet_NowMs() + (unsigned long long)ms;
    for (;;) {
        for (;;) {
            TAK_WsConnStep st = TAK_WsConn_Step(&g_conn);
            if (st == TAK_WSCONN_NEED_MORE) break;
            if (st == TAK_WSCONN_ERROR || st == TAK_WSCONN_CLOSED) return -1;
            if (st == TAK_WSCONN_MESSAGE) {
                TAK_NetFrame f;
                if (TAK_Net_Split(g_conn.msg, g_conn.msg_len, &f) != 0) return -1;
                if (out_payload && out_len) {
                    memcpy(out_payload, f.payload, f.payload_len);
                    *out_len = f.payload_len;
                }
                return f.type;
            }
        }
        if (TakNet_NowMs() > deadline) return 0;
        if (pump_in(s, 20) < 0) return -1;
    }
}

/* One plain request on its own connection, the way the front page asks,
 * read until the server closes. Returns the bytes read. */
static size_t api_get(const char *host, unsigned short port, const char *path,
                      char *out, size_t cap) {
    TakSocket s = dial(host, port);
    if (s == TAK_INVALID_SOCKET) return 0;
    char req[256];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n", path, host);
    size_t got = 0;
    if (TakNet_Send(s, req, (size_t)n) == n) {
        unsigned long long deadline = TakNet_NowMs() + 3000;
        while (got + 1 < cap && TakNet_NowMs() < deadline) {
            TakSocket one[1] = { s };
            unsigned char w[1] = { 0 }, r[1] = { 0 }, ww[1] = { 0 };
            if (TakNet_Wait(one, w, r, ww, 1, 50) <= 0 || !r[0]) continue;
            int k = TakNet_Recv(s, out + got, cap - 1 - got);
            if (k <= 0) break;
            got += (size_t)k;
        }
    }
    out[got] = '\0';
    TakNet_Close(s);
    return got;
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned short port = (unsigned short)(argc > 2 ? atoi(argv[2]) : 8799);
    int hold_secs = (argc > 4 && strcmp(argv[3], "--hold") == 0) ? atoi(argv[4]) : 0;

    if (TakNet_Start() != 0) { printf("no network layer\n"); return 1; }
    TakSocket s = dial(host, port);
    if (s == TAK_INVALID_SOCKET) {
        printf("could not reach %s:%u. Is okrelay running?\n", host,
               (unsigned)port);
        TakNet_Stop();
        return 1;
    }

    uint8_t nonce[16];
    for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(i * 37 + 11);
    ok(TAK_WsConn_InitClient(&g_conn, host, "/play", nonce) == 0,
       "the client writes an upgrade request");

    uint8_t payload[TAK_NET_PAYLOAD_MAX];
    size_t plen = 0;

    /* HELLO first. Everything in it is ours to choose here except the
     * protocol version, which the server checks. */
    TAK_MsgHello h;
    memset(&h, 0, sizeof h);
    h.protocol_version = TAK_NET_PROTOCOL_VERSION;
    h.engine_build_id = 1;
    h.determinism_class = 1;
    h.client_kind = 0;
    for (int i = 0; i < 16; i++) h.device_token[i] = (uint8_t)(i + 1);
    snprintf(h.name, sizeof h.name, "smoke");

    uint8_t frame[TAK_NET_FRAME_MAX];
    size_t n = TAK_Msg_HelloEncode(&h, frame, sizeof frame);
    ok(n > 0, "HELLO encodes");

    /* The handshake has to finish before a message can be queued. */
    int type = wait_open(s, 3000);
    if (g_conn.state != TAK_WSCONN_OPEN) {
        printf("  (exchange returned %d, state %d, why %s, in %u out %u)\n",
               type, (int)g_conn.state, g_conn.why ? g_conn.why : "-",
               (unsigned)g_conn.in_len, (unsigned)g_conn.out_len);
    }
    ok(g_conn.state == TAK_WSCONN_OPEN, "the server accepted the upgrade");

    ok(TAK_WsConn_Send(&g_conn, frame, n) == 0, "HELLO goes out");
    type = exchange(s, payload, &plen, 2000);
    ok(type == TAK_MSG_WELCOME, "the server answers WELCOME");

    if (type == TAK_MSG_WELCOME) {
        TAK_MsgWelcome w;
        ok(TAK_Msg_WelcomeDecode(&w, payload, plen) == 0, "WELCOME decodes");
        printf("  server name: %s\n", w.server_name);
        printf("  session id:  %u\n", (unsigned)w.session_id);
        ok(w.session_id != 0, "the session has an id");
        ok(w.server_name[0] != '\0', "the server named itself");
    }

    /* A room, which is the next thing a player does. */
    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    snprintf(cr.name, sizeof cr.name, "smoke room");
    snprintf(cr.map_name, sizeof cr.map_name, "Two Castles");
    cr.flags = TAK_ROOMF_LISTED;
    cr.max_players = 4;
    n = TAK_Msg_CreateRoomEncode(&cr, frame, sizeof frame);
    ok(n > 0, "CREATE_ROOM encodes");
    ok(TAK_WsConn_Send(&g_conn, frame, n) == 0, "CREATE_ROOM goes out");
    type = exchange(s, payload, &plen, 2000);
    ok(type == TAK_MSG_ROOM_STATE, "the server answers with the room state");

    if (type == TAK_MSG_ROOM_STATE) {
        TAK_MsgRoomState rs;
        ok(TAK_Msg_RoomStateDecode(&rs, payload, plen) == 0,
           "ROOM_STATE decodes");
        printf("  room id:     %u\n", (unsigned)rs.room_id);
        printf("  room name:   %s\n", rs.name);
        ok(rs.room_id != 0, "the room has an id");
    }

    /* The front page's question, on a connection of its own. */
    static char api[16384];
    size_t alen = api_get(host, port, "/api/rooms", api, sizeof api);
    ok(alen > 0 && strstr(api, "HTTP/1.1 200") != NULL, "/api/rooms answers");
    ok(strstr(api, "\"online\":1") != NULL, "it counts this client online");
    ok(strstr(api, "\"name\":\"smoke room\"") != NULL &&
       strstr(api, "\"map\":\"Two Castles\"") != NULL, "it lists the room just made");

    /* Keep the room up, answering the heartbeat, until the time is out. */
    if (hold_secs > 0) printf("  holding the room for %d s\n", hold_secs);
    unsigned long long until = TakNet_NowMs() + (unsigned long long)hold_secs * 1000u;
    while (hold_secs > 0 && TakNet_NowMs() < until) {
        type = exchange(s, payload, &plen, 500);
        if (type < 0) { ok(0, "the relay kept the connection while holding"); break; }
        if (type == TAK_MSG_PING) {
            TAK_MsgPing pm;
            if (TAK_Msg_PingDecode(&pm, payload, plen) == 0) {
                n = TAK_Msg_PingEncode(TAK_MSG_PONG, &pm, frame, sizeof frame);
                if (n > 0) TAK_WsConn_Send(&g_conn, frame, n);
            }
        }
    }

    TAK_WsConn_Close(&g_conn, 1000);
    (void)exchange(s, NULL, NULL, 200);
    TakNet_Close(s);
    TakNet_Stop();

    printf("\n%s\n", g_fail == 0 ? "all good" : "something is wrong");
    return g_fail == 0 ? 0 : 1;
}
