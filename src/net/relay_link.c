/*
 * relay_link.c -- a real client reaching a real relay through the same
 * link API the browser uses.
 *
 * relay_smoke proved the socket loop and the handshake by driving
 * ws_conn by hand. This proves the layer above it: TAK_NetLink_Open,
 * TAK_NetLink_Pump and the session client, which is exactly the code
 * the screens will call and exactly the code the browser build calls
 * with its own half of the link underneath.
 *
 * A program rather than a ctest case, for the same reason relay_smoke
 * is one: it needs a server to talk to and a port to talk on.
 *
 *   okrelay --port 8799 &
 *   relay_link ws://127.0.0.1:8799/play
 */

#include "net_socket.h"
#include "tak_net_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void ok(int cond, const char *what) {
    printf("%-52s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) g_fail++;
}

static TAK_NetClient g_c;

/* Pump until `want` happens or the clock runs out. Returns 1 when it
 * happened. */
static int wait_for(TAK_NetClientEventKind want, int ms) {
    unsigned long long deadline = TakNet_NowMs() + (unsigned long long)ms;
    for (;;) {
        TAK_NetLink_Pump(&g_c, TakNet_NowMs());
        TAK_NetClientEvent e;
        while (TAK_NetClient_PollEvent(&g_c, &e)) {
            if (e.kind == want) return 1;
            if (e.kind == TAK_NC_EV_GONE) return 0;
        }
        if (TAK_NetLink_State() == TAK_LINK_FAILED) return 0;
        if (TakNet_NowMs() > deadline) return 0;
    }
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "ws://127.0.0.1:8799/play";

    if (TAK_NetLink_Open(url) != 0) {
        printf("could not open %s: %s\n", url, TAK_NetLink_Why());
        return 1;
    }

    TAK_MsgHello h;
    memset(&h, 0, sizeof h);
    h.protocol_version = TAK_NET_PROTOCOL_VERSION;
    h.engine_build_id = 1;
    h.determinism_class = 1;
    for (int i = 0; i < TAK_NET_TOKEN_BYTES; i++) {
        h.device_token[i] = (uint8_t)(i + 3);
    }
    snprintf(h.name, sizeof h.name, "link smoke");
    TAK_NetClient_Init(&g_c, &h);

    /* The HELLO is already queued, and the link sends it the moment the
     * upgrade is answered. Nothing here has to know when that was. */
    ok(wait_for(TAK_NC_EV_WELCOMED, 3000), "the session is welcomed");
    if (g_c.state == TAK_NC_LOBBY) {
        printf("  server name: %s\n", g_c.welcome.server_name);
        printf("  session id:  %u\n", (unsigned)g_c.session_id);
    }
    ok(g_c.state == TAK_NC_LOBBY, "the client is in the lobby");

    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    snprintf(cr.name, sizeof cr.name, "link room");
    snprintf(cr.map_name, sizeof cr.map_name, "two castles");
    memset(cr.map_fingerprint, 0x5A, sizeof cr.map_fingerprint);
    cr.flags = TAK_ROOMF_LISTED;
    cr.max_players = 4;
    ok(TAK_NetClient_CreateRoom(&g_c, &cr) == 0, "a room is asked for");
    ok(wait_for(TAK_NC_EV_ROOM_STATE, 3000), "the room comes back");
    if (g_c.state == TAK_NC_ROOM) {
        printf("  room id:     %u\n", (unsigned)g_c.room.room_id);
        printf("  room name:   %s\n", g_c.room.name);
        printf("  our seat:    %u\n", (unsigned)g_c.seat);
    }
    ok(g_c.room.room_id != 0, "the room has an id");
    ok(g_c.seat != TAK_NET_SEAT_NONE, "the server gave us a seat");

    /* An own row edit, which the client stamps with our seat. Without
     * that stamping the server refuses it, so a green line here is the
     * stamping working over a real wire. */
    TAK_MsgRoomEdit ed;
    memset(&ed, 0, sizeof ed);
    ed.field = TAK_EDIT_READY;
    ok(TAK_NetClient_EditRoom(&g_c, &ed) == 0, "ready is sent");
    ok(wait_for(TAK_NC_EV_ROOM_STATE, 3000), "the room state comes back");
    ok(g_c.room.slot[g_c.seat].ready == 1, "the server marked us ready");

    /* Optionally sit in the room rather than leaving, so something
     * else can be pointed at the server and find a game listed. A room
     * the last human leaves is retired, which is right and is exactly
     * why this has to stay. */
    int hold_ms = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[i + 1]) * 1000;
        }
    }
    if (hold_ms > 0) {
        printf("\nholding the room for %d seconds\n", hold_ms / 1000);
        fflush(stdout);
        unsigned long long until = TakNet_NowMs() + (unsigned long long)hold_ms;
        while (TakNet_NowMs() < until &&
               TAK_NetLink_State() != TAK_LINK_FAILED) {
            TAK_NetLink_Pump(&g_c, TakNet_NowMs());
            TAK_NetClientEvent e;
            while (TAK_NetClient_PollEvent(&g_c, &e)) { }
        }
    }

    TAK_NetClient_LeaveRoom(&g_c);
    TAK_NetLink_Pump(&g_c, TakNet_NowMs());
    TAK_NetLink_Close();

    printf("\n%s\n", g_fail == 0 ? "all good" : "something is wrong");
    return g_fail == 0 ? 0 : 1;
}
