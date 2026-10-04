/*
 * embed_net_smoke.c -- a match through okengine against a real relay,
 * with two processes as the two players, because the engine is one per
 * process. The host opens a room on a real map, the guest finds it and
 * joins, both get ready, the host starts, both load through the relay's
 * handshake and play on its turns, and each raises a building turned
 * with an order that travels through the relay and comes back. The
 * guest claims the first start, so each monarch stands where the claim
 * puts it. Both stop on one tick and print the state hash, to compare.
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

static int we_hold_the_first_start(const OkxNetRoomInfo *r) {
    return r->your_seat >= 0 && r->your_seat < r->seat_count &&
           r->seats[r->your_seat].start == 0;
}

static int we_are_ready(const OkxNetRoomInfo *r) {
    return r->your_seat >= 0 && r->your_seat < r->seat_count &&
           r->seats[r->your_seat].ready && r->seats[r->your_seat].has_map;
}

/* A building the monarch can raise turned, with sides of two lengths so
 * the turn shows, and a clear site for it near the monarch at that
 * facing. Returns the def, or -1. */
static int turned_site(const OkxUnit *king, int facing, int32_t *sx, int32_t *sy) {
    static int32_t opts[256];
    int k = okx_def_buildables(king->def, opts, 256);
    for (int j = 0; j < k; j++) {
        OkxDefInfo d;
        if (okx_def_info(opts[j], &d) != 0 || !d.is_building || !okx_def_can_turn(opts[j]))
            continue;
        if (d.footprint_x == d.footprint_z) continue;
        for (int r = 96; r <= 640; r += 32)
            for (int a = 0; a < 8; a++) {
                int32_t x = (int32_t)king->x + (a % 3 - 1) * r;
                int32_t y = (int32_t)king->z + (a / 3 - 1) * r;
                if (okx_build_site_facing(opts[j], facing, x, y, sx, sy)) return opts[j];
            }
    }
    return -1;
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
    if (!host) {
        ok(okx_net_edit(TAK_EDIT_START, -1, 0, NULL) == 0, "we claim the first start");
        ok(wait_room(we_hold_the_first_start, 5000), "the relay gives it to us");
    }
    ok(okx_net_edit(TAK_EDIT_READY, -1, 1, NULL) == 0, "we say ready");
    ok(wait_room(we_are_ready, 5000), "the relay has us ready, with the map");

    if (host) {
        ok(wait_room(all_ready, 30000), "everyone is ready");
        OkxNetRoomInfo r;
        ok(okx_net_room(&r) == 0 && r.seats[0].start == -1 && r.seats[1].start == 0,
           "the guest's claim reaches us");
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
    {
        /* The guest's claim is kept and the host takes the start left. */
        int map = -1;
        char name[96];
        int32_t xz[16];
        for (int i = 0; i < okx_map_count() && map < 0; i++)
            if (okx_map_name(i, name, sizeof name) > 0 && strcmp(name, "two castles") == 0) map = i;
        int k = host ? 1 : 0, near = 0;
        if (okx_map_starts(map, xz, 8) > 1 && mine >= 0) {
            float dx = units[mine].x - (float)(xz[2 * k] * 16);
            float dz = units[mine].z - (float)(xz[2 * k + 1] * 16);
            near = dx * dx + dz * dz < 64.0f * 64.0f;
        }
        ok(near, host ? "we stand on the start left over" : "we stand on the start we claimed");
    }
    int handle = mine >= 0 ? units[mine].handle : -1;
    /* Each side raises a building turned through the relay, the host a
     * quarter and the guest three quarters. */
    int facing = host ? 1 : 3;
    int32_t sx = 0, sy = 0;
    int product = mine >= 0 ? turned_site(&units[mine], facing, &sx, &sy) : -1;
    ok(product >= 0, "a turned site near the monarch is clear");
    printf("%-5s   builds def %d at %d,%d facing %d from tick %u\n", g_role, product, sx, sy,
           facing, okx_tick_count());
    if (product >= 0)
        ok(okx_command(3, handle, sx, sy, -1, product, facing) == 0,
           "the turned build order goes out");
    /* Both stop on the same tick, so the two hashes can be compared. */
    const uint32_t stop = 480;
    uint32_t t0 = okx_tick_count();
    until = SDL_GetTicks64() + 30000;
    while (okx_tick_count() < stop && SDL_GetTicks64() < until) {
        okx_net_pump();
        uint32_t left = stop - okx_tick_count();
        okx_tick(left < 4 ? (int32_t)left : 4);
        SDL_Delay(16);
    }
    printf("%-5s   %u ticks on the relay's turns\n", g_role, okx_tick_count() - t0);
    ok(okx_tick_count() == stop, "the battle runs on the relay's turns");
    printf("%-5s   hash at tick %u: %08x\n", g_role, stop, okx_sim_hash());
    /* Every unit by handle, the other side's in the fog included. */
    int turned_mine = 0, turned_theirs = 0;
    for (int h = 0; h < 512; h++) {
        OkxUnit u;
        if (okx_unit(h, &u) != 0) continue;
        if (u.facing == facing && u.player == me) turned_mine = 1;
        if (u.facing == 4 - facing && u.player != me) turned_theirs = 1;
        if (u.building) printf("%-5s   frame %d def %d player %d facing %d at %.0f,%.0f\n",
                               g_role, h, u.def, u.player, u.facing, u.x, u.z);
    }
    ok(turned_mine, "our building stands turned");
    ok(turned_theirs, "the other side's stands turned as they placed it");
    ok(okx_net_last_chat(NULL, 0, NULL, 0) > 0, "chat from the room arrived");

    okx_net_disconnect();
    okx_shutdown();
    printf("%-5s %s\n", g_role, g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
