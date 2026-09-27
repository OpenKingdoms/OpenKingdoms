/*
 * embed_net_stress.c -- two okengine processes play one match through a
 * real relay under varied orders, stop on one tick and write the state
 * hash in parts, so a runner can compare them and name the first part
 * that differs.
 *
 *   embed_net_stress <url> host|guest <map> <stop tick> <seed> <dump file>
 *
 * Both sides take the same seed. Each draws its own orders from it
 * (moves, turned buildings, spells, stances) at fixed ticks, and paces
 * its frames from it too, so runs differ in timing as well as orders.
 */
#include "ok_embed.h"
#include "tak_net_protocol.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define ROOM_NAME "embed stress"

static const char *g_role = "host";
static uint32_t g_rng;

static uint32_t draw(uint32_t n) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return n ? (g_rng >> 8) % n : 0;
}

static void say(const char *what) {
    printf("%-5s %s\n", g_role, what);
    fflush(stdout);
}

static int wait_state(int want, int ms) {
    uint64_t until = SDL_GetTicks64() + (uint64_t)ms;
    for (;;) {
        int s = okx_net_pump();
        if (s == want) return 1;
        if (s == OKX_NET_GONE || s == OKX_NET_REFUSED) {
            printf("%-5s session ended: %s\n", g_role, okx_net_why());
            return 0;
        }
        if (SDL_GetTicks64() > until) return 0;
        SDL_Delay(5);
    }
}

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

static OkxUnit g_units[1024];
static int g_n;

static void read_units(void) {
    g_n = 0;
    for (int h = 0; h < 1024; h++)
        if (okx_unit(h, &g_units[g_n]) == 0) g_n++;
}

static int nearest_enemy(const OkxUnit *u, int me) {
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < g_n; i++) {
        if (g_units[i].player == me || g_units[i].state != OKX_UNIT_ACTIVE) continue;
        float dx = g_units[i].x - u->x, dz = g_units[i].z - u->z, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* One round of orders for our own units, drawn from the seed. */
static void give_orders(int me, int *built) {
    read_units();
    for (int i = 0; i < g_n; i++) {
        const OkxUnit *u = &g_units[i];
        if (u->player != me || u->state != OKX_UNIT_ACTIVE || u->building) continue;
        int pick = (int)draw(6);
        if (pick == 0) {
            okx_command(1, u->handle, (int32_t)u->x + (int32_t)draw(600) - 300,
                        (int32_t)u->z + (int32_t)draw(600) - 300, -1, -1, 0);
        } else if (pick == 1 && *built < 3) {
            static int32_t opts[256];
            int k = okx_def_buildables(u->def, opts, 256);
            if (k <= 0) continue;
            int def = opts[draw((uint32_t)k)];
            OkxDefInfo d;
            if (okx_def_info(def, &d) != 0 || !d.is_building) continue;
            int facing = (int)draw(4);
            int32_t sx = 0, sy = 0;
            for (int r = 96; r <= 480; r += 48) {
                int32_t x = (int32_t)u->x + (int32_t)draw(2 * r) - r;
                int32_t y = (int32_t)u->z + (int32_t)draw(2 * r) - r;
                if (okx_build_site_facing(def, facing, x, y, &sx, &sy)) {
                    okx_command(3, u->handle, sx, sy, -1, def, facing);
                    (*built)++;
                    break;
                }
            }
        } else if (pick == 2) {
            /* A spell: choose a weapon slot and attack the nearest enemy. */
            okx_select(&u->handle, 1, 0);
            static OkxHudCommand cmds[32];
            int nc = okx_hud_commands(cmds, 32);
            for (int c = 0; c < nc; c++)
                if (cmds[c].weapon_slot >= 0 && cmds[c].enabled && draw(2)) {
                    okx_hud_do(cmds[c].id);
                    break;
                }
            int e = nearest_enemy(u, me);
            if (e >= 0) okx_command(2, u->handle, 0, 0, g_units[e].handle, -1, 0);
            okx_select(NULL, 0, 0);
        } else if (pick == 3) {
            okx_select(&u->handle, 1, 0);
            okx_hud_do(101 + (int)draw(3));
            okx_select(NULL, 0, 0);
        } else if (pick == 4) {
            int e = nearest_enemy(u, me);
            if (e >= 0) okx_command(1, u->handle, (int32_t)g_units[e].x, (int32_t)g_units[e].z, -1, -1, 0);
        }
    }
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "ws://127.0.0.1:8799/play";
    g_role = argc > 2 ? argv[2] : "host";
    const char *map = argc > 3 ? argv[3] : "two castles";
    uint32_t stop = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 1200;
    uint32_t seed = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 1;
    const char *dump = argc > 6 ? argv[6] : "stress.txt";
    int host = strcmp(g_role, "host") == 0;
    g_rng = seed * 2654435761u + (host ? 17u : 91u);
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        printf("no game data: %s\n", okx_last_error());
        return 1;
    }
    if (okx_net_connect(url, host ? "Host" : "Guest") != 0 || !wait_state(OKX_NET_LOBBY, 5000)) {
        say("no lobby");
        return 2;
    }
    if (host) {
        if (okx_net_create_room(ROOM_NAME, map, TAK_ROOMOPT_MAP_REVEALED) != 0 ||
            !wait_state(OKX_NET_ROOM, 5000) || !wait_room(two_humans, 30000)) {
            say("no room");
            return 2;
        }
    } else if (!find_and_join(30000) || !wait_state(OKX_NET_ROOM, 5000)) {
        say("no room to join");
        return 2;
    }
    okx_net_edit(TAK_EDIT_READY, -1, 1, NULL);
    if (!wait_room(we_are_ready, 5000)) { say("not ready"); return 2; }
    if (host) {
        if (!wait_room(all_ready, 30000)) { say("not all ready"); return 2; }
        okx_net_start();
    }
    if (!wait_state(OKX_NET_LOADING, 30000)) { say("no load"); return 2; }
    okx_net_load_begin();
    int rc = 0;
    uint64_t until = SDL_GetTicks64() + 90000;
    while ((rc = okx_load_step(20, NULL, NULL, 0)) == 0 && SDL_GetTicks64() < until) okx_net_pump();
    if (rc != 1) { printf("%-5s load failed: %s\n", g_role, okx_net_why()); return 2; }

    int me = okx_local_player(), built = 0;
    uint32_t next_orders = 0;
    until = SDL_GetTicks64() + 180000;
    while (okx_tick_count() < stop && SDL_GetTicks64() < until) {
        int s = okx_net_pump();
        if (s == OKX_NET_GONE) { printf("%-5s gone: %s\n", g_role, okx_net_why()); break; }
        if (okx_tick_count() >= next_orders) {
            give_orders(me, &built);
            next_orders = okx_tick_count() + 90 + draw(120);
        }
        uint32_t left = stop - okx_tick_count();
        okx_tick(left < 4 ? (int32_t)left : 4);
        /* A host reads the drawn units every frame, which loads their
         * models, and fog makes the two sides load different ones. */
        static OkxUnit drawn[1024];
        okx_units(drawn, 1024);
        SDL_Delay(4 + draw(24));
    }
    uint32_t t = okx_tick_count();
    printf("%-5s stopped at tick %u, hash %08x\n", g_role, t, okx_sim_hash());
    FILE *f = fopen(dump, "w");
    if (f) {
        static uint32_t parts[8192];
        int np = okx_sim_hash_parts(parts, 8192);
        fprintf(f, "tick %u hash %08x\n", t, okx_sim_hash());
        for (int i = 0; i < np && i < 8192; i++) fprintf(f, "part %d %08x\n", i, parts[i]);
        read_units();
        for (int i = 0; i < g_n; i++) {
            const OkxUnit *u = &g_units[i];
            float m = 0, mx = 0;
            okx_unit_mana(u->handle, &m, &mx);
            OkxOrder o;
            okx_unit_order(u->handle, &o);
            fprintf(f, "unit %d def %d pl %d st %d xz %.0f %.0f hd %.4f hp %d/%d b %d f %d mana %.2f ord %d tgt %d\n",
                    u->handle, u->def, u->player, u->state, u->x, u->z, u->heading,
                    u->health, u->max_health, u->building, u->facing, m, o.kind, o.target);
        }
        fclose(f);
    }
    okx_net_disconnect();
    okx_shutdown();
    return t == stop ? 0 : 3;
}
