/*
 * embed_net_smoke.c -- a match through okengine against a real relay,
 * with two processes as the two players, because the engine is one per
 * process. The host opens a room on a real map, the guest finds it and
 * joins, both get ready, the host starts, both load through the relay's
 * handshake and play on its turns, and each sends its monarch east with
 * an order that travels through the relay and comes back.
 *
 *   okrelay --port 8799 &
 *   embed_net_smoke ws://127.0.0.1:8799/play host &
 *   embed_net_smoke ws://127.0.0.1:8799/play guest
 */
#include "ok_embed.h"
#include "tak_net_protocol.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define ROOM_NAME "embed smoke"

static int g_fail;
static const char *g_role = "host";

static void ok(int cond, const char *what) {
    printf("%-5s %-52s %s\n", g_role, what, cond ? "ok" : "FAILED");
    fflush(stdout);
    if (!cond) g_fail++;
}

/* Pump until the session reaches `want`, or give up after ms. */
static int wait_state(int want, int ms) {
    uint64_t until = SDL_GetTicks64() + (uint64_t)ms;
    for (;;) {
        int s = okx_net_pump();
        if (s == want) return 1;
        if (s == OKX_NET_GONE || s == OKX_NET_REFUSED) {
            printf("%-5s   session ended: %s\n", g_role, okx_net_why());
            return 0;
        }
        if (SDL_GetTicks64() > until) return 0;
        SDL_Delay(5);
    }
}

/* Pump until the room says what cond looks for, or give up. */
static int wait_room(int (*cond)(const OkxNetRoomInfo *), int ms) {
    uint64_t until = SDL_GetTicks64() + (uint64_t)ms;
    OkxNetRoomInfo r;
    for (;;) {
        okx_net_pump();
        if (okx_net_room(&r) == 0 && cond(&r)) return 1;
        if (SDL_GetTicks64() > until) return 0;
        SDL_Delay(5);
    }
}

static int two_humans(const OkxNetRoomInfo *r) {
    int n = 0;
    for (int i = 0; i < r->seat_count; i++) n += r->seats[i].kind == 1 && r->seats[i].connected;
    return n >= 2;
}

static int all_ready(const OkxNetRoomInfo *r) {
    int humans = 0;
    for (int i = 0; i < r->seat_count; i++) {
        if (r->seats[i].kind != 1) continue;
        humans++;
        if (!r->seats[i].ready || !r->seats[i].has_map) return 0;
    }
    return humans >= 2;
}

static int we_are_ready(const OkxNetRoomInfo *r) {
    return r->your_seat >= 0 && r->your_seat < r->seat_count &&
           r->seats[r->your_seat].ready && r->seats[r->your_seat].has_map;
}

/* The guest looks for the host's room in the list and joins it. */
static int find_and_join(int ms) {
    uint64_t until = SDL_GetTicks64() + (uint64_t)ms;
    static OkxNetRoom rooms[32];
    while (SDL_GetTicks64() < until) {
        okx_net_list_rooms();
        uint64_t wait = SDL_GetTicks64() + 300;
        while (SDL_GetTicks64() < wait) { okx_net_pump(); SDL_Delay(5); }
        int n = okx_net_rooms(rooms, 32);
        for (int i = 0; i < n && i < 32; i++)
            if (strcmp(rooms[i].name, ROOM_NAME) == 0 && rooms[i].joinable)
                return okx_net_join_room(rooms[i].id, NULL) == 0;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "ws://127.0.0.1:8799/play";
    g_role = argc > 2 ? argv[2] : "host";
    int host = strcmp(g_role, "host") == 0;
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        printf("no game data: %s\n", okx_last_error());
        return 1;
    }
    ok(okx_net_connect(url, host ? "Host" : "Guest") == 0, "the session opens");
    ok(wait_state(OKX_NET_LOBBY, 5000), "the relay lets us into the lobby");

    if (host) {
        ok(okx_net_create_room(ROOM_NAME, "two castles", TAK_ROOMOPT_MAP_REVEALED) == 0,
           "a room is asked for");
        ok(wait_state(OKX_NET_ROOM, 5000), "we sit in it");
        OkxNetRoomInfo r;
        ok(okx_net_room(&r) == 0 && r.you_host && strcmp(r.map, "two castles") == 0,
           "we host it on the map we chose");
        ok(wait_room(two_humans, 30000), "the guest joins");
    } else {
        ok(find_and_join(30000), "the host's room is listed and we join it");
        ok(wait_state(OKX_NET_ROOM, 5000), "we sit in it");
    }

    /* Chat goes while we sit in the room: once the host starts, the
     * room is gone and a line has nowhere to go. */
    ok(okx_net_chat(host ? "hello from the host" : "hello from the guest") == 0, "a chat line goes out");
    ok(okx_net_edit(TAK_EDIT_READY, -1, 1, NULL) == 0, "we say ready");
    ok(wait_room(we_are_ready, 5000), "the relay has us ready, with the map");

    if (host) {
        ok(wait_room(all_ready, 30000), "everyone is ready");
        ok(okx_net_start() == 0, "we start the match");
    }
    int loading = wait_state(OKX_NET_LOADING, 30000);
    if (!loading) printf("%-5s   the relay said: %s\n", g_role, okx_net_why());
    ok(loading, "the relay says build it");
    ok(okx_net_load_begin() == 0, "the battle begins loading");
    int rc = 0;
    uint64_t until = SDL_GetTicks64() + 60000;
    while ((rc = okx_load_step(20, NULL, NULL, 0)) == 0 && SDL_GetTicks64() < until) okx_net_pump();
    if (rc != 1) printf("%-5s   the relay said: %s\n", g_role, okx_net_why());
    ok(rc == 1, "it loads and the relay says go");

    /* Play on the relay's turns, and send the monarch east through it. */
    static OkxUnit units[64];
    int n = okx_units(units, 64), me = okx_local_player(), mine = -1;
    for (int i = 0; i < n; i++) if (units[i].player == me && mine < 0) mine = i;
    ok(mine >= 0, "our monarch stands on the map");
    float x0 = mine >= 0 ? units[mine].x : 0.0f;
    int handle = mine >= 0 ? units[mine].handle : -1;
    if (handle >= 0) okx_command(1, handle, (int32_t)x0 + 400, (int32_t)units[mine].z, -1, -1, 0);
    uint32_t t0 = okx_tick_count();
    int ran = 0;
    until = SDL_GetTicks64() + 8000;
    while (SDL_GetTicks64() < until) {
        okx_net_pump();
        ran += okx_tick(4);
        SDL_Delay(16);
    }
    printf("%-5s   %d ticks in 8 s on the relay's turns\n", g_role, ran);
    ok(okx_tick_count() > t0 + 120, "the battle runs on the relay's turns");
    n = okx_units(units, 64);
    float x1 = x0;
    for (int i = 0; i < n; i++) if (units[i].handle == handle) x1 = units[i].x;
    ok(x1 > x0 + 50.0f, "the order went through the relay and the monarch walked");
    ok(okx_net_last_chat(NULL, 0, NULL, 0) > 0, "chat from the room arrived");

    okx_net_disconnect();
    okx_shutdown();
    printf("%-5s %s\n", g_role, g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
