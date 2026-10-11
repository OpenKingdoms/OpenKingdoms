/*
 * test_embed.c -- the engine through ok_embed.h, the way a host uses it:
 * boot, load a real map, tick, and read back terrain, models, units,
 * poses and features. Needs the game data.
 */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <direct.h>
/* windows.h's old pointer words, which the cases use as names. */
#  undef near
#  undef far
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#include "test_framework.h"
#include "ok_embed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define MAP_NAME "two castles"

/* TAK_CMD_* values, as a host passes them. */
#define TAK_CMD_MOVE_ORDER      1
#define TAK_CMD_ATTACK_ORDER    2
#define TAK_CMD_BUILD_ORDER     3
#define TAK_CMD_STOP_ORDER      4
#define TAK_CMD_PATROL_ORDER    5
#define TAK_CMD_SET_AGGRO_ORDER 13
#define TAK_CMD_RALLY_ORDER     18
#define TAK_CMD_ATTACK_GROUND   20
#define TAK_CMD_GIVE_UNITS      24

static int g_booted;

static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* The test battle against one computer, the map shown or fogged. */
static int start_battle(int revealed) {
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "%s", MAP_NAME);
    snprintf(cfg.kingdom, sizeof(cfg.kingdom), "aramon");
    cfg.ai_players = 1;
    cfg.map_revealed = revealed;
    /* Hidden means out of sight now, not only never explored. */
    cfg.line_of_sight = !revealed;
    cfg.seed = 12345;
    if (okx_start_skirmish(&cfg) != 0) {
        printf("start failed: %s ", okx_last_error());
        return -1;
    }
    g_booted = 1;
    return 0;
}

/* 1 when there is no game data to run against. A battle an earlier case
 * decided is ended, so every case starts on one still being fought. */
static int boot(void) {
    if (g_booted && okx_outcome() == 0) return 0;
    if (g_booted) {
        okx_end_game();
        g_booted = 0;
        return start_battle(1);
    }
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        SKIP_MARK("no game data: %s", okx_last_error());
        return 1;
    }
    return start_battle(1);
}

TEST(the_maps_are_listed) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    ASSERT_EQ_INT(OKX_API_VERSION, okx_api_version());
    int n = okx_map_count();
    ASSERT(n > 5);
    char name[96];
    int found = 0;
    for (int i = 0; i < n; i++) {
        ASSERT(okx_map_name(i, name, sizeof(name)) > 0);
        if (strcmp(name, MAP_NAME) == 0) found = 1;
    }
    ASSERT(found);
}

TEST(a_skirmish_loads_with_terrain) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    OkxTerrainInfo t;
    ASSERT_EQ_INT(0, okx_terrain_info(&t));
    ASSERT(t.map_w > 0 && t.map_h > 0);
    ASSERT(t.heights_w > 1 && t.heights_h > 1);
    ASSERT(t.chunk_count > 0);
    int n = t.heights_w * t.heights_h;
    float *hs = (float *)malloc(sizeof(float) * (size_t)n);
    ASSERT_EQ_INT(n, okx_terrain_heights(hs, n));
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; i++) { if (hs[i] < lo) lo = hs[i]; if (hs[i] > hi) hi = hs[i]; }
    free(hs);
    ASSERT(hi > lo);
    int nb = t.blocks_w * t.blocks_h;
    int32_t *blocks = (int32_t *)malloc(sizeof(int32_t) * 3 * (size_t)nb);
    ASSERT_EQ_INT(nb, okx_terrain_blocks(blocks, nb * 3));
    for (int i = 0; i < nb; i++) ASSERT(blocks[3 * i] >= 0 && blocks[3 * i] < t.chunk_count);
    free(blocks);
    int w = 0, h = 0;
    int need = okx_terrain_chunk(0, NULL, 0, &w, &h);
    ASSERT(need > 0 && w > 0 && h > 0);
    ASSERT_EQ_INT(w * h * 4, need);
}

TEST(units_stand_on_the_map_with_models_and_poses) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    ASSERT(okx_def_count() > 20);
    static OkxUnit units[512];
    int n = okx_units(units, 512);
    ASSERT(n >= 2);
    const OkxUnit *u = &units[0];
    ASSERT(u->model >= 0);
    OkxDefInfo d;
    ASSERT_EQ_INT(0, okx_def_info(u->def, &d));
    ASSERT(d.object[0]);

    OkxModelInfo mi;
    ASSERT_EQ_INT(0, okx_model_info(u->model, &mi));
    ASSERT(mi.vert_count > 0 && mi.index_count > 0 && mi.node_count > 1);
    float *pos = (float *)malloc(sizeof(float) * 3 * (size_t)mi.vert_count);
    float *nrm = (float *)malloc(sizeof(float) * 3 * (size_t)mi.vert_count);
    int32_t *nodes = (int32_t *)malloc(sizeof(int32_t) * (size_t)mi.vert_count);
    int32_t *idx = (int32_t *)malloc(sizeof(int32_t) * (size_t)mi.index_count);
    ASSERT_EQ_INT(0, okx_model_geometry(u->model, pos, nrm, NULL, NULL, nodes, idx));
    for (int v = 0; v < mi.vert_count; v++) {
        ASSERT(nodes[v] >= 0 && nodes[v] < mi.node_count);
        float l = nrm[3 * v] * nrm[3 * v] + nrm[3 * v + 1] * nrm[3 * v + 1] + nrm[3 * v + 2] * nrm[3 * v + 2];
        ASSERT(l > 0.9f && l < 1.1f);
    }
    for (int i = 0; i < mi.index_count; i++) ASSERT(idx[i] >= 0 && idx[i] < mi.vert_count);
    free(pos); free(nrm); free(nodes); free(idx);

    static OkxNode nd[128];
    ASSERT_EQ_INT(mi.node_count, okx_model_nodes(u->model, nd, 128));
    ASSERT_EQ_INT(-1, nd[0].parent);
    static OkxBatch bt[32];
    int nbt = okx_model_batches(u->model, bt, 32);
    ASSERT(nbt > 0);
    int textured = 0;
    for (int b = 0; b < nbt; b++) {
        if (bt[b].texture < 0) continue;
        int w = 0, h = 0;
        int need = okx_texture(bt[b].texture, NULL, 0, &w, &h);
        ASSERT(need > 0 && need == w * h * 4);
        uint8_t *px = (uint8_t *)malloc((size_t)need);
        ASSERT(okx_texture(bt[b].texture, px, need, &w, &h) == need);
        int lit = 0;
        for (int i = 0; i < need; i += 4) lit += px[i + 3] != 0;
        free(px);
        ASSERT(lit > 0);
        textured++;
    }
    ASSERT(textured > 0);

    /* The root of the pose stands at the unit, near the ground. */
    static float mats[128 * 12];
    static uint8_t hidden[128];
    ASSERT_EQ_INT(mi.node_count, okx_unit_pose(u->handle, mats, hidden, 128));
    ASSERT(fabsf(mats[3] - u->x) < 64.0f);
    ASSERT(fabsf(mats[11] - u->z) < 64.0f);
    ASSERT(fabsf(mats[7] - u->y) < 64.0f);
}

TEST(a_marching_unit_moves_and_its_pieces_swing) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512);
    int me = okx_local_player();
    int pick = -1;
    for (int i = 0; i < n && pick < 0; i++)
        if (units[i].player == me && units[i].state == OKX_UNIT_ACTIVE) pick = i;
    ASSERT(pick >= 0);
    OkxUnit u = units[pick];
    /* 1 is TAK_CMD_MOVE. */
    ASSERT_EQ_INT(0, okx_command(1, u.handle, (int32_t)u.x + 400, (int32_t)u.z, -1, -1, 0));
    uint32_t t0 = okx_tick_count();
    ASSERT_EQ_INT(90, okx_tick(90));
    ASSERT(okx_tick_count() >= t0 + 90);

    static float a[128 * 12], b[128 * 12];
    int nodes = okx_unit_pose(u.handle, a, NULL, 128);
    ASSERT(nodes > 1);
    okx_tick(7);
    ASSERT_EQ_INT(nodes, okx_unit_pose(u.handle, b, NULL, 128));
    int moved = 0;
    for (int i = 0; i < nodes; i++) {
        for (int k = 0; k < 12; k++) {
            if (a[i * 12 + k] != b[i * 12 + k]) { moved++; break; }
        }
    }
    ASSERT(moved >= 2);
    /* Walking, as its script sees it, with a walk running. */
    char running[512];
    ASSERT_EQ_INT(OKX_ANIM_MOVING, okx_unit_anim(u.handle, running, sizeof running));
    ASSERT(strstr(running, "walk") != NULL);

    n = okx_units(units, 512);
    float x = -1.0f;
    for (int i = 0; i < n; i++) if (units[i].handle == u.handle) x = units[i].x;
    ASSERT(x > u.x + 50.0f);
    ASSERT_EQ_INT(0, okx_outcome());
}

TEST(features_come_as_models_or_sprites) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int n = okx_features(NULL, 0);
    ASSERT(n > 0);
    OkxFeature *f = (OkxFeature *)malloc(sizeof(OkxFeature) * (size_t)n);
    ASSERT_EQ_INT(n, okx_features(f, n));
    int models = 0, sprites = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].model >= 0) {
            static float m[128 * 12];
            if (okx_feature_pose(f[i].index, m, 128) > 0) models++;
        } else if (f[i].sprite >= 0) {
            ASSERT(f[i].w > 0 && f[i].top != f[i].bottom);
            if (!sprites) {
                int w = 0, h = 0;
                int need = okx_sprite(f[i].sprite, NULL, 0, &w, &h);
                ASSERT(need > 0 && need == w * h * 4);
            }
            sprites++;
        }
        OkxFeatureDefInfo d;
        ASSERT_EQ_INT(0, okx_feature_def_info(f[i].def, &d));
    }
    free(f);
    printf("(%d models, %d sprites) ", models, sprites);
    ASSERT(models + sprites == n);
    ASSERT(okx_feature_def_count() > 100);
}

TEST(the_lobby_and_the_hud_have_what_they_show) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int idx = -1;
    char name[96];
    for (int i = 0; i < okx_map_count() && idx < 0; i++)
        if (okx_map_name(i, name, sizeof(name)) > 0 && strcmp(name, MAP_NAME) == 0) idx = i;
    ASSERT(idx >= 0);
    OkxMapInfo mi;
    ASSERT_EQ_INT(0, okx_map_info(idx, &mi));
    ASSERT(mi.max_players >= 2);
    ASSERT(mi.kingdom[0]);
    /* Sizes in cells, 32 to a size unit, and the starts inside them. */
    ASSERT(mi.size_x >= 64 && mi.size_x % 32 == 0 && mi.size_y % 32 == 0);
    int32_t xz[16];
    int starts = okx_map_starts(idx, xz, 8);
    ASSERT(starts >= 2 && starts <= 8);
    for (int i = 0; i < starts; i++)
        ASSERT(xz[2 * i] >= 0 && xz[2 * i] < mi.size_x && xz[2 * i + 1] >= 0 && xz[2 * i + 1] < mi.size_y);
    ASSERT_EQ_INT(-1, okx_map_starts(-1, xz, 8));
    int w = 0, h = 0;
    int need = okx_map_preview(idx, NULL, 0, &w, &h);
    ASSERT(need > 0 && need == w * h * 4);
    uint8_t *px = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_map_preview(idx, px, need, &w, &h));
    int opaque = 0;
    for (int i = 0; i < need; i += 4) opaque += px[i + 3] != 0 && (px[i] | px[i + 1] | px[i + 2]) != 0;
    free(px);
    ASSERT(opaque > need / 8);

    /* A map wider than it is deep gets a picture of the same shape, with
     * no padding rows below the map. */
    int wide = -1;
    for (int i = 0; i < okx_map_count() && wide < 0; i++)
        if (okx_map_name(i, name, sizeof(name)) > 0 && strcmp(name, "abnar's terrace") == 0) wide = i;
    if (wide >= 0) {
        OkxMapInfo wi;
        ASSERT_EQ_INT(0, okx_map_info(wide, &wi));
        need = okx_map_preview(wide, NULL, 0, &w, &h);
        ASSERT(need == w * h * 4);
        ASSERT(w > h);
        float want = (float)wi.size_x / (float)wi.size_y, got = (float)w / (float)h;
        ASSERT(got > want * 0.9f && got < want * 1.1f);
    }

    static OkxPlayer players[8];
    int np = okx_players(players, 8);
    ASSERT(np >= 2);
    int me = okx_local_player();
    int found = 0;
    for (int i = 0; i < np; i++) if (players[i].index == me) { found = 1; ASSERT_EQ_INT(1, players[i].kind); }
    ASSERT(found);
    OkxEconomy e;
    ASSERT_EQ_INT(0, okx_economy(me, &e));
    ASSERT(e.max_mana > 0 && e.mana > 0.0f);

    /* The monarch builds, and what it builds costs mana. */
    static OkxUnit units[512];
    int n = okx_units(units, 512);
    int builds = 0;
    for (int i = 0; i < n && !builds; i++) {
        if (units[i].player != me) continue;
        static int32_t opts[256];
        int k = okx_def_buildables(units[i].def, opts, 256);
        if (k <= 0) continue;
        OkxDefInfo d;
        ASSERT_EQ_INT(0, okx_def_info(opts[0], &d));
        ASSERT(d.build_cost > 0);
        ASSERT(d.display_name[0]);
        int pw = 0, ph = 0;
        int pneed = okx_unit_picture(opts[0], NULL, 0, &pw, &ph);
        ASSERT(pneed > 0 && pneed == pw * ph * 4);
        builds = k;
    }
    ASSERT(builds > 0);
    ASSERT(okx_projectiles(NULL, 0) >= 0);
}

TEST(the_studio_plays_a_units_walk_outside_the_battle) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int def = -1;
    for (int i = 0; i < okx_def_count() && def < 0; i++) {
        OkxDefInfo d;
        if (okx_def_info(i, &d) == 0 && same_name(d.object, "araking")) def = i;
    }
    ASSERT(def >= 0);
    static char names[4096];
    ASSERT(okx_def_scripts(def, names, sizeof(names)) > 0);
    ASSERT(strstr(names, "Create") != NULL);
    uint32_t t0 = okx_tick_count();

    static float a[128 * 12], b[128 * 12];
    int n = okx_studio_pose(def, 0, "walk", 60, a, NULL, 128);
    ASSERT(n > 1);
    ASSERT_EQ_INT(n, okx_studio_pose(def, 0, "walk", 67, b, NULL, 128));
    int moved = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 12; k++) if (a[i * 12 + k] != b[i * 12 + k]) { moved++; break; }
    ASSERT(moved >= 2);
    /* Standing at the origin, and the battle did not move a tick. */
    ASSERT(fabsf(a[3]) < 64.0f && fabsf(a[11]) < 64.0f);
    ASSERT_EQ_INT((int)t0, (int)okx_tick_count());
    /* No script: Create alone, which a model always has. */
    ASSERT_EQ_INT(n, okx_studio_pose(def, 0, NULL, 30, a, NULL, 128));
}

/* A flyer's fly moves nothing until BeginFlight has run, so the studio
 * begins the flight first and the wings beat. */
TEST(the_studio_plays_a_flyers_fly) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int def = -1;
    for (int i = 0; i < okx_def_count() && def < 0; i++) {
        OkxDefInfo d;
        if (okx_def_info(i, &d) == 0 && same_name(d.object, "zonharp")) def = i;
    }
    if (def < 0) return;
    static float a[128 * 12], b[128 * 12];
    int n = okx_studio_pose(def, 0, "fly", 60, a, NULL, 128);
    ASSERT(n > 1);
    int moved = 0;
    for (int t = 61; t < 120 && !moved; t++) {
        ASSERT_EQ_INT(n, okx_studio_pose(def, 0, "fly", t, b, NULL, 128));
        for (int i = 0; i < n && !moved; i++)
            for (int k = 0; k < 12; k++) if (fabsf(a[i * 12 + k] - b[i * 12 + k]) > 1e-3f) { moved = 1; break; }
    }
    ASSERT(moved);
}

TEST(the_hud_can_place_queue_and_read_orders) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512);
    int me = okx_local_player();
    int builder = -1, product = -1;
    for (int i = 0; i < n && builder < 0; i++) {
        if (units[i].player != me) continue;
        static int32_t opts[256];
        int k = okx_def_buildables(units[i].def, opts, 256);
        for (int j = 0; j < k && product < 0; j++) {
            OkxDefInfo d;
            if (okx_def_info(opts[j], &d) == 0 && d.is_building) product = opts[j];
        }
        if (product >= 0) builder = i;
    }
    ASSERT(builder >= 0);
    const OkxUnit *b = &units[builder];
    /* Find clear ground near the builder, then order the building. */
    int32_t sx = 0, sy = 0, found = 0;
    for (int r = 96; r <= 800 && !found; r += 32)
        for (int a = 0; a < 8 && !found; a++) {
            int32_t x = (int32_t)b->x + (a % 3 - 1) * r, y = (int32_t)b->z + (a / 3 - 1) * r;
            if (okx_build_site(product, x, y, &sx, &sy)) found = 1;
        }
    ASSERT(found);
    ASSERT_EQ_INT(0, okx_command(3, b->handle, sx, sy, -1, product, 0));
    okx_tick(5);
    OkxOrder o;
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &o));
    ASSERT(o.kind == OKX_ORDER_BUILD || o.kind == OKX_ORDER_MOVE);
    ASSERT(okx_factory_queue(b->handle, -1) >= 0);

    int w = 0, h = 0;
    int need = okx_fog(NULL, 0, &w, &h);
    ASSERT(need > 0 && need == w * h);
    uint8_t *fog = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_fog(fog, need, &w, &h));
    int seen = 0;
    for (int i = 0; i < need; i++) seen += fog[i] == 2;
    free(fog);
    ASSERT(seen > 0);
}

TEST(the_view_follows_the_host_camera_and_audio_is_optional) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_set_view(1000, 2000, 800, 600);
    okx_tick(1);
    /* Audio needs a device, which a build machine may not have. Asking
     * must not break anything either way, and stopping always works. */
    int a = okx_audio(1, 0, 0);
    ASSERT(a == 0 || a == -1);
    okx_tick(2);
    ASSERT_EQ_INT(0, okx_audio(0, 0, 0));
    ASSERT_EQ_INT(0, okx_outcome());
}

TEST(a_battle_shows_its_shots_and_explosions) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    /* March the local monarch at the enemy's and let them fight. */
    static OkxUnit units[512];
    int n = okx_units(units, 512);
    int me = okx_local_player(), mine = -1, theirs = -1;
    for (int i = 0; i < n; i++) {
        if (units[i].player == me && mine < 0) mine = i;
        if (units[i].player != me && theirs < 0) theirs = i;
    }
    ASSERT(mine >= 0 && theirs >= 0);
    ASSERT_EQ_INT(0, okx_command(2, units[mine].handle, 0, 0, units[theirs].handle, -1, 0));
    int seen = 0, strips = 0;
    static OkxEffect fx[256];
    for (int t = 0; t < 60 * 180 && !seen; t += 10) {
        okx_tick(10);
        int k = okx_effects(fx, 256);
        for (int i = 0; i < k && i < 256; i++) {
            ASSERT(fx[i].w > 0 && fx[i].top > fx[i].bottom);
            ASSERT(fx[i].u1 > fx[i].u0 && fx[i].v1 > 0.0f && fx[i].v1 <= 1.0f);
            int w = 0, h = 0;
            int need = okx_effect_strip(fx[i].sprite, NULL, 0, &w, &h);
            ASSERT(need > 0 && need == w * h * 4);
            /* The pace and the frame on show agree with the geometry. */
            ASSERT(fx[i].ticks_per_frame > 0 && fx[i].frame_count > fx[i].frame);
            int32_t geo[4 * 64];
            ASSERT_EQ_INT(fx[i].frame_count, okx_effect_frames(fx[i].sprite, geo, 64));
            if (fx[i].frame < 64)
                ASSERT(fabsf((float)geo[4 * fx[i].frame] - fx[i].w) < 0.5f);
            strips++;
        }
        if (k > 0) seen = 1;
    }
    printf("(%d frames) ", strips);
    ASSERT(seen);
}

TEST(the_games_own_click_selects_and_orders) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[256];
    int n = okx_units(units, 256), me = okx_local_player(), mine = -1, theirs = -1;
    for (int i = 0; i < n; i++) {
        if (units[i].state != OKX_UNIT_ACTIVE) continue;
        if (units[i].player == me && mine < 0) mine = i;
        if (units[i].player != me && theirs < 0) theirs = i;
    }
    ASSERT(mine >= 0 && theirs >= 0);
    const OkxUnit *u = &units[mine];

    /* A click on a friend selects it, the right click clears. */
    okx_cancel();
    ASSERT_EQ_INT(0, okx_selection(NULL, 0));
    okx_click(u->x, u->z, u->handle, 0);
    int32_t sel[8];
    ASSERT_EQ_INT(1, okx_selection(sel, 8));
    ASSERT_EQ_INT(u->handle, sel[0]);

    /* A click on open ground moves the selection there. */
    okx_click(u->x + 300.0f, u->z, -1, 0);
    okx_tick(3);
    OkxOrder o;
    ASSERT_EQ_INT(0, okx_unit_order(u->handle, &o));
    ASSERT_EQ_INT(OKX_ORDER_MOVE, o.kind);
    ASSERT(o.x > (int32_t)u->x + 200);

    /* A click on an enemy attacks it. */
    okx_click(units[theirs].x, units[theirs].z, units[theirs].handle, 0);
    okx_tick(3);
    ASSERT_EQ_INT(0, okx_unit_order(u->handle, &o));
    ASSERT_EQ_INT(OKX_ORDER_ATTACK, o.kind);
    ASSERT_EQ_INT(units[theirs].handle, o.target);

    /* Stop for the whole selection, and an armed patrol taken by a click. */
    ASSERT_EQ_INT(0, okx_order_selection(4, 0));
    okx_tick(3);
    ASSERT_EQ_INT(0, okx_unit_order(u->handle, &o));
    ASSERT_EQ_INT(OKX_ORDER_NONE, o.kind);
    okx_arm(OKX_ARM_PATROL, -1);
    ASSERT_EQ_INT(OKX_ARM_PATROL, okx_armed(NULL));
    okx_click(u->x, u->z + 300.0f, -1, 0);
    okx_tick(3);
    ASSERT_EQ_INT(0, okx_unit_order(u->handle, &o));
    ASSERT_EQ_INT(OKX_ORDER_PATROL, o.kind);
    ASSERT_EQ_INT(OKX_ARM_NONE, okx_armed(NULL));

    /* Control groups. */
    okx_group_assign(3);
    okx_cancel();
    ASSERT_EQ_INT(0, okx_selection(NULL, 0));
    ASSERT_EQ_INT(1, okx_group_recall(3));
    okx_cancel();

    /* Ctrl+Z takes every finished unit of the player's of the selected
     * one's type, Ctrl+Shift and a number add a group, and Ctrl+A takes
     * every unit of the player's. */
    int same = 0, all = 0;
    for (int i = 0; i < n; i++) {
        if (units[i].player != me || units[i].building) continue;
        if (units[i].state != OKX_UNIT_ACTIVE) continue;
        all++;
        if (units[i].def == u->def) same++;
    }
    ASSERT_EQ_INT(1, okx_select(&u->handle, 1, 0));
    ASSERT_EQ_INT(same, okx_select_kind(OKX_SELECT_SAME_TYPE, NULL, 0));
    okx_cancel();
    ASSERT_EQ_INT(1, okx_group_add(3));
    ASSERT_EQ_INT(all, okx_select_kind(OKX_SELECT_ALL, NULL, 0));
    okx_cancel();
}

/* Studio Mode sets a unit down by the local start, a boat on the sea
 * rides it, and a match would refuse both. */
TEST(the_studio_places_a_unit_by_the_start_and_a_boat_floats) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int me = okx_local_player();
    int walker = -1, boat = -1;
    for (int i = 0; i < okx_def_count(); i++) {
        OkxDefInfo d;
        if (okx_def_info(i, &d) != 0 || d.is_building || d.can_fly) continue;
        if (d.floater && boat < 0) boat = i;
        if (!d.floater && walker < 0) walker = i;
    }
    ASSERT(walker >= 0);
    int before = okx_units(NULL, 0);
    int h = okx_place_unit(walker, me);
    ASSERT(h >= 0);
    ASSERT_EQ_INT(before + 1, okx_units(NULL, 0));
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    ASSERT_EQ_INT(me, u.player);
    ASSERT_EQ_INT(walker, u.def);
    /* By the start: the monarch spawned there too. */
    static OkxUnit units[256];
    int n = okx_units(units, 256);
    int near = 0;
    for (int i = 0; i < n; i++) {
        if (units[i].player != me || units[i].handle == h) continue;
        float dx = units[i].x - u.x, dz = units[i].z - u.z;
        if (dx * dx + dz * dz < 1024.0f * 1024.0f) near = 1;
    }
    ASSERT(near);
    ASSERT_EQ_INT(-1, okx_place_unit(walker, 99));
    if (boat < 0) return;
    OkxTerrainInfo t;
    ASSERT_EQ_INT(0, okx_terrain_info(&t));
    int hb = okx_place_unit(boat, me);
    if (hb < 0 || t.water_height <= 0) return;   /* no sea by the start */
    ASSERT_EQ_INT(0, okx_unit(hb, &u));
    ASSERT(u.y >= (float)t.water_height - 0.5f);
    printf("(boat at y %.0f, sea %d) ", (double)u.y, (int)t.water_height);
}

static float embed_turn_gap(float a, float b) {
    float d = a - b;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return d < 0.0f ? -d : d;
}

/* A formation walks the unit to its own point, a queued one waits its
 * turn and ends facing the heading given, and an enemy named in the
 * call is left alone. */
TEST(a_formation_walks_turns_and_queues) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_cancel();
    static OkxUnit units[256];
    int n = okx_units(units, 256), me = okx_local_player(), mine = -1, theirs = -1;
    for (int i = 0; i < n; i++) {
        if (units[i].state != OKX_UNIT_ACTIVE) continue;
        if (units[i].player == me && mine < 0) mine = i;
        if (units[i].player != me && theirs < 0) theirs = i;
    }
    ASSERT(mine >= 0 && theirs >= 0);
    OkxUnit u = units[mine], foe = units[theirs];
    int32_t handles[2] = { u.handle, foe.handle };
    int32_t xy[4] = { (int32_t)u.x + 160, (int32_t)u.z, (int32_t)foe.x + 300, (int32_t)foe.z };
    ASSERT_EQ_INT(0, okx_move_formation(handles, xy, 2, 0, 0.0f, 1, 0));
    /* Queued behind it, a second point and a heading to end on. */
    const float face = 1.5f;
    int32_t xy2[2] = { (int32_t)u.x + 160, (int32_t)u.z + 160 };
    ASSERT_EQ_INT(0, okx_move_formation(handles, xy2, 1, 1, face, 0, 1));
    okx_tick(3);
    OkxOrder o;
    ASSERT_EQ_INT(0, okx_unit_order(u.handle, &o));
    ASSERT_EQ_INT(OKX_ORDER_MOVE, o.kind);
    ASSERT_EQ_INT(xy[0], o.x);
    ASSERT_EQ_INT(xy[1], o.y);
    ASSERT_EQ_INT(0, okx_unit_order(foe.handle, &o));
    ASSERT(!(o.kind == OKX_ORDER_MOVE && o.x == xy[2]));

    int saw_next = 0, faced = 0;
    for (int t = 0; t < 2400 && !faced; t += 5) {
        okx_tick(5);
        OkxUnit now;
        ASSERT_EQ_INT(0, okx_unit(u.handle, &now));
        ASSERT_EQ_INT(0, okx_unit_order(u.handle, &o));
        if (o.kind == OKX_ORDER_MOVE && o.y == xy2[1]) saw_next = 1;
        if (saw_next && o.kind == OKX_ORDER_NONE &&
            embed_turn_gap(now.heading, face) < 0.02f) faced = 1;
    }
    ASSERT_EQ_INT(1, saw_next);
    ASSERT_EQ_INT(1, faced);
    /* Nothing of ours named, nothing sent. */
    ASSERT_EQ_INT(-1, okx_move_formation(handles + 1, xy + 2, 1, 0, 0.0f, 0, 0));
}

/* The pointer is the one the classic view shows: the select hand over
 * a friend, the sword over an enemy once something is selected, an
 * armed command's own cursor, and a placement's ghost with the plain
 * pointer. Every cursor has the game's art, and the revive one moves. */
TEST(the_cursor_is_the_one_the_classic_view_shows) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player(), mine = -1, theirs = -1;
    int product = -1;
    for (int i = 0; i < n; i++) {
        if (units[i].state != OKX_UNIT_ACTIVE) continue;
        if (units[i].player == me && mine < 0) mine = i;
        if (units[i].player != me && theirs < 0) theirs = i;
        static int32_t opts[256];
        int k = units[i].player == me ? okx_def_buildables(units[i].def, opts, 256) : 0;
        for (int j = 0; j < k && product < 0; j++) {
            OkxDefInfo d;
            if (okx_def_info(opts[j], &d) == 0 && d.is_building) product = opts[j];
        }
    }
    ASSERT(mine >= 0 && theirs >= 0 && product >= 0);
    const OkxUnit *u = &units[mine], *e = &units[theirs];
    float gx = u->x + 400.0f, gz = u->z;

    okx_cancel();
    okx_cancel();
    ASSERT_EQ_INT(OKX_CURSOR_NORMAL, okx_cursor_at(gx, gz, -1, NULL));
    ASSERT_EQ_INT(OKX_CURSOR_SELECT, okx_cursor_at(u->x, u->z, u->handle, NULL));
    ASSERT_EQ_INT(OKX_CURSOR_SELECT, okx_cursor_at(e->x, e->z, e->handle, NULL));
    okx_select(&u->handle, 1, 0);
    ASSERT_EQ_INT(OKX_CURSOR_ATTACK, okx_cursor_at(e->x, e->z, e->handle, NULL));
    ASSERT_EQ_INT(OKX_CURSOR_NORMAL, okx_cursor_at(gx, gz, -1, NULL));
    okx_arm(OKX_ARM_PATROL, -1);
    ASSERT_EQ_INT(OKX_CURSOR_PATROL, okx_cursor_at(gx, gz, -1, NULL));
    /* An armed attack over a friend, or a guard over an enemy, cannot
     * be carried out, and the remaster says so in red. */
    okx_arm(OKX_ARM_ATTACK, -1);
    ASSERT_EQ_INT(OKX_CURSOR_RED, okx_cursor_at(u->x, u->z, u->handle, NULL));
    ASSERT_EQ_INT(OKX_CURSOR_ATTACK, okx_cursor_at(e->x, e->z, e->handle, NULL));
    okx_arm(OKX_ARM_GUARD, -1);
    ASSERT_EQ_INT(OKX_CURSOR_RED, okx_cursor_at(e->x, e->z, e->handle, NULL));
    okx_arm(OKX_ARM_GUARD, -1);
    ASSERT_EQ_INT(OKX_CURSOR_GUARD, okx_cursor_at(gx, gz, -1, NULL));
    okx_arm(OKX_ARM_MOVE, -1);
    ASSERT_EQ_INT(OKX_CURSOR_MOVE, okx_cursor_at(gx, gz, -1, NULL));
    okx_arm(OKX_ARM_BUILD, product);
    int32_t clear = -1;
    ASSERT_EQ_INT(OKX_CURSOR_PLACE, okx_cursor_at(e->x, e->z, -1, &clear));
    ASSERT_EQ_INT(0, clear);
    okx_cancel();
    okx_cancel();

    for (int c = 0; c < OKX_CURSOR_COUNT; c++) {
        int32_t w = 0, h = 0, hx = -1, hy = -1, ms = 0;
        int frames = okx_cursor_frame(c, 0, NULL, 0, &w, &h, &hx, &hy, &ms);
        ASSERT(frames >= 1);
        ASSERT(w > 0 && h > 0 && w <= 128 && h <= 128);
        ASSERT(hx >= 0 && hx < w && hy >= 0 && hy < h);
        ASSERT(ms > 0);
        uint8_t *px = (uint8_t *)malloc((size_t)(w * h * 4));
        ASSERT_EQ_INT(frames, okx_cursor_frame(c, 0, px, w * h * 4, &w, &h, &hx, &hy, &ms));
        int opaque = 0, clear_px = 0;
        for (int i = 0; i < w * h; i++) {
            if (px[i * 4 + 3] == 255) opaque++;
            if (px[i * 4 + 3] == 0) clear_px++;
        }
        free(px);
        ASSERT(opaque > 0 && clear_px > 0);
    }
    ASSERT(okx_cursor_frame(OKX_CURSOR_REVIVE, 0, NULL, 0, NULL, NULL, NULL, NULL, NULL) > 1);
    ASSERT_EQ_INT(-1, okx_cursor_frame(OKX_CURSOR_COUNT, 0, NULL, 0, NULL, NULL, NULL, NULL, NULL));
}

/* A building placed turned stands turned: the order carries the facing,
 * the unit reads it back, the heading follows it, and the site test
 * swaps the footprint. A lodestone never turns. */
TEST(a_building_placed_turned_stands_turned) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player();
    int builder = -1, product = -1, lode = -1;
    for (int i = 0; i < n && product < 0; i++) {
        if (units[i].player != me || units[i].state != OKX_UNIT_ACTIVE) continue;
        static int32_t opts[256];
        int k = okx_def_buildables(units[i].def, opts, 256);
        for (int j = 0; j < k; j++) {
            OkxDefInfo d;
            if (okx_def_info(opts[j], &d) != 0 || !d.is_building) continue;
            if (!okx_def_can_turn(opts[j])) { if (lode < 0) lode = opts[j]; continue; }
            if (product < 0 && d.footprint_x != d.footprint_z) { product = opts[j]; builder = i; }
        }
    }
    ASSERT(builder >= 0 && product >= 0);
    ASSERT(lode >= 0);
    const OkxUnit *b = &units[builder];

    /* An odd facing snaps on the swapped sides. */
    OkxDefInfo d;
    ASSERT_EQ_INT(0, okx_def_info(product, &d));
    int32_t ax = 0, ay = 0, bx = 0, by = 0;
    okx_build_site_facing(product, 0, (int32_t)b->x, (int32_t)b->z, &ax, &ay);
    okx_build_site_facing(product, 1, (int32_t)b->x, (int32_t)b->z, &bx, &by);
    int odd_x = d.footprint_x & 1, odd_z = d.footprint_z & 1;
    ASSERT_EQ_INT(odd_x ? 8 : 0, ((ax % 16) + 16) % 16);
    ASSERT_EQ_INT(odd_z ? 8 : 0, ((bx % 16) + 16) % 16);

    int32_t sx = 0, sy = 0, found = 0;
    for (int r = 96; r <= 800 && !found; r += 32)
        for (int a = 0; a < 8 && !found; a++) {
            int32_t x = (int32_t)b->x + (a % 3 - 1) * r, y = (int32_t)b->z + (a / 3 - 1) * r;
            if (okx_build_site_facing(product, 3, x, y, &sx, &sy)) found = 1;
        }
    ASSERT(found);
    ASSERT_EQ_INT(0, okx_command(3, b->handle, sx, sy, -1, product, 3));
    okx_tick(5);
    /* The frame, by the builder's order. */
    OkxOrder bo;
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &bo));
    OkxUnit fr;
    ASSERT_EQ_INT(0, okx_unit(bo.building, &fr));
    ASSERT_EQ_INT(product, fr.def);
    ASSERT_EQ_INT(3, fr.facing);
    ASSERT(fr.heading > 1.5f && fr.heading < 1.6f);
    /* The site it took is no longer clear, at any facing. */
    ASSERT_EQ_INT(0, okx_build_site_facing(product, 3, sx, sy, NULL, NULL));
    ASSERT_EQ_INT(0, okx_build_site_facing(product, 0, sx, sy, NULL, NULL));

    /* The classic click places turned too: armed at facing 1 through the
     * host, the building the click raises stands at 1. The classic 2D
     * client places unturned, and a host with its own 3D camera turns. */
    okx_select(&b->handle, 1, 0);
    int32_t cx = 0, cy = 0, spot = 0;
    for (int r = 96; r <= 800 && !spot; r += 32)
        for (int a = 0; a < 8 && !spot; a++) {
            int32_t x = (int32_t)b->x + (a % 3 - 1) * r, y = (int32_t)b->z + (a / 3 - 1) * r;
            if (okx_build_site_facing(product, 1, x, y, &cx, &cy)) spot = 1;
        }
    ASSERT(spot);
    okx_arm(OKX_ARM_BUILD, product);
    okx_set_build_facing(1);
    int32_t def = -1;
    ASSERT_EQ_INT(OKX_ARM_BUILD, okx_armed(&def));
    ASSERT_EQ_INT(product, def);
    okx_click((float)cx, (float)cy, -1, 0);
    okx_tick(5);
    OkxOrder co;
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &co));
    OkxUnit placed;
    ASSERT_EQ_INT(0, okx_unit(co.building, &placed));
    ASSERT_EQ_INT(product, placed.def);
    ASSERT_EQ_INT(1, placed.facing);
    okx_arm(OKX_ARM_BUILD, lode);
    okx_set_build_facing(1);
    okx_cancel();
    okx_cancel();
}

/* The sidebar's orders come from the classic HUD's own table: a caster
 * lists its spells with their cost and art, the stance buttons change
 * the unit's stance through the sidebar's order, and a spell chosen and
 * cast at an enemy spends the caster's mana. */
static int find_cmd(const OkxHudCommand *c, int n, int id) {
    for (int i = 0; i < n; i++) if (c[i].id == id) return i;
    return -1;
}

TEST(the_sidebar_orders_list_cast_and_toggle) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    static OkxHudCommand cmds[32];
    int n = okx_units(units, 512), me = okx_local_player();
    int caster = -1, spell = -1, nc = 0;
    for (int i = 0; i < n && caster < 0; i++) {
        if (units[i].player != me || units[i].state != OKX_UNIT_ACTIVE) continue;
        okx_select(&units[i].handle, 1, 0);
        nc = okx_hud_commands(cmds, 32);
        for (int k = 0; k < nc; k++)
            if (cmds[k].weapon_slot >= 0 && cmds[k].mana_cost > 0) { caster = i; spell = k; break; }
    }
    ASSERT(caster >= 0);
    const OkxUnit *u = &units[caster];
    printf("(%s casts %s for %d) ", cmds[find_cmd(cmds, nc, OKX_ARM_MOVE)].name,
           cmds[spell].weapon, cmds[spell].mana_cost);
    ASSERT(cmds[spell].weapon[0] != 0);
    ASSERT_EQ_INT(OKX_CMDGROUP_WEAPON, cmds[spell].group);
    /* Every button the selection has, with the original's picture. */
    ASSERT(find_cmd(cmds, nc, OKX_ARM_ATTACK) >= 0);
    ASSERT(find_cmd(cmds, nc, OKX_HUD_STOP) >= 0);
    ASSERT_EQ_INT('A', cmds[find_cmd(cmds, nc, OKX_ARM_ATTACK)].hotkey);
    for (int k = 0; k < nc; k++) {
        if (cmds[k].why == OKX_WHY_UNSUPPORTED) continue;
        int32_t w = 0, h = 0;
        int need = okx_hud_command_art(cmds[k].id, 2, NULL, 0, &w, &h);
        if (need <= 0) printf("(no art for %s) ", cmds[k].name);
        ASSERT(need > 0 && need == w * h * 4);
    }

    /* The stance buttons are one choice of three, and pressing one sets
     * the unit's stance on the next tick. */
    int pas = find_cmd(cmds, nc, OKX_HUD_PASSIVE);
    ASSERT(pas >= 0);
    ASSERT_EQ_INT(OKX_CMDGROUP_STANCE, cmds[pas].group);
    ASSERT_EQ_INT(1, okx_hud_do(OKX_HUD_PASSIVE));
    okx_tick(3);
    nc = okx_hud_commands(cmds, 32);
    ASSERT_EQ_INT(1, cmds[find_cmd(cmds, nc, OKX_HUD_PASSIVE)].active);
    ASSERT_EQ_INT(0, cmds[find_cmd(cmds, nc, OKX_HUD_OFFENSIVE)].active);
    ASSERT_EQ_INT(1, okx_hud_do(OKX_HUD_OFFENSIVE));
    okx_tick(3);
    nc = okx_hud_commands(cmds, 32);
    ASSERT_EQ_INT(1, cmds[find_cmd(cmds, nc, OKX_HUD_OFFENSIVE)].active);

    /* Attack arms and a second press disarms, as the sidebar does. */
    ASSERT_EQ_INT(1, okx_hud_do(OKX_ARM_ATTACK));
    ASSERT_EQ_INT(OKX_ARM_ATTACK, okx_armed(NULL));
    ASSERT_EQ_INT(1, okx_hud_do(OKX_ARM_ATTACK));
    ASSERT_EQ_INT(OKX_ARM_NONE, okx_armed(NULL));

    /* A caster starts with no mana, so its spell button is off until the
     * reserve covers a cast. */
    int spell_id = cmds[spell].id;
    int ready = 0;
    for (int t = 0; t < 60 * 120 && !ready; t += 30) {
        nc = okx_hud_commands(cmds, 32);
        int k = find_cmd(cmds, nc, spell_id);
        ready = k >= 0 && cmds[k].enabled;
        if (!ready) {
            ASSERT_EQ_INT(OKX_WHY_MANA, cmds[k].why);
            ASSERT_EQ_INT(0, okx_hud_do(spell_id));
            okx_tick(30);
        }
    }
    ASSERT(ready);
    /* Choose the spell, then send the caster at the nearest enemy. */
    ASSERT_EQ_INT(1, okx_hud_do(spell_id));
    okx_tick(3);
    nc = okx_hud_commands(cmds, 32);
    ASSERT_EQ_INT(1, cmds[find_cmd(cmds, nc, spell_id)].active);
    float mana0 = 0, max0 = 0;
    ASSERT_EQ_INT(0, okx_unit_mana(u->handle, &mana0, &max0));
    ASSERT(max0 > 0);
    int target = -1;
    float best = 1e30f;
    n = okx_units(units, 512);
    for (int i = 0; i < n; i++) {
        if (units[i].player == me || units[i].state != OKX_UNIT_ACTIVE) continue;
        float dx = units[i].x - u->x, dz = units[i].z - u->z, d2 = dx * dx + dz * dz;
        if (d2 < best) { best = d2; target = i; }
    }
    ASSERT(target >= 0);
    okx_arm(OKX_ARM_ATTACK, -1);
    okx_click(units[target].x, units[target].z, units[target].handle, 0);
    /* The nearest enemy can be across the map, near two minutes' walk. */
    int spent = 0;
    for (int t = 0; t < 60 * 240 && !spent; t += 10) {
        okx_tick(10);
        float m = 0, mx = 0;
        okx_unit_mana(u->handle, &m, &mx);
        if (m < mana0 - 1.0f) spent = t + 10;
        mana0 = m > mana0 ? m : mana0;   /* it recharges on the way */
    }
    printf("(mana spent at tick %d) ", spent);
    ASSERT(spent > 0);
    okx_order_selection(4, 0);
    okx_cancel();
}

/* The fog comes as the classic view draws it: with line of sight on,
 * ground seen before is dimmed. With it off, the original keeps showing
 * whatever was seen, so that ground is drawn clear. */
static void walk_and_count_fog(int line_of_sight, int counts[3]) {
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "%s", MAP_NAME);
    cfg.ai_players = 1;
    cfg.line_of_sight = line_of_sight;
    cfg.seed = 5;
    counts[0] = counts[1] = counts[2] = -1;
    if (okx_start_skirmish(&cfg) != 0) return;
    static OkxUnit units[64];
    int n = okx_units(units, 64), me = okx_local_player();
    for (int i = 0; i < n; i++)
        if (units[i].player == me)
            okx_command(1, units[i].handle, (int32_t)units[i].x + 1200, (int32_t)units[i].z - 1200, -1, -1, 0);
    okx_tick(60 * 30);
    int w = 0, h = 0;
    int need = okx_fog(NULL, 0, &w, &h);
    uint8_t *fog = (uint8_t *)malloc((size_t)need);
    okx_fog(fog, need, &w, &h);
    counts[0] = counts[1] = counts[2] = 0;
    for (int i = 0; i < need; i++) counts[fog[i] > 2 ? 2 : fog[i]]++;
    free(fog);
}

TEST(the_fog_comes_as_the_classic_view_draws_it) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    int on[3], off[3];
    walk_and_count_fog(1, on);
    walk_and_count_fog(0, off);
    g_booted = 0;
    ASSERT(on[0] > 0 && on[1] > 0 && on[2] > 0);
    /* With line of sight off nothing is dimmed: seen is seen. */
    ASSERT(off[0] > 0 && off[2] > 0);
    ASSERT_EQ_INT(0, off[1]);
}

TEST(an_override_model_replaces_the_shipped_one) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    /* No folder: the shipped model. A folder without the file: still. */
    okx_set_override_dir("no_such_folder_here");
    int m = okx_model_load("araking", 3);
    ASSERT(m >= 0);
    OkxModelInfo mi;
    ASSERT_EQ_INT(0, okx_model_info(m, &mi));
    ASSERT_EQ_INT(0, mi.from_override);
    ASSERT_EQ_INT(-1, okx_model_load("../escape", 0));
    okx_set_override_dir(NULL);
}

TEST(a_load_comes_in_slices_with_progress) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "%s", MAP_NAME);
    cfg.ai_players = 1;
    cfg.seed = 3;
    ASSERT_EQ_INT(0, okx_load_begin(&cfg));
    g_booted = 0;
    float progress = 0.0f, last = 0.0f;
    char status[128];
    int steps = 0, rc = 0;
    while ((rc = okx_load_step(5, &progress, status, sizeof(status))) == 0 && steps < 100000) {
        ASSERT(progress >= last - 0.001f);
        last = progress;
        steps++;
    }
    printf("(%d slices) ", steps);
    ASSERT_EQ_INT(1, rc);
    ASSERT(progress == 1.0f);
    ASSERT(steps > 1);
    /* The map is not revealed, so the enemy is under the fog. */
    ASSERT(okx_units(NULL, 0) >= 1);
    g_booted = 1;
}

TEST(the_lobby_lineup_sets_the_seats) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "%s", MAP_NAME);
    cfg.seed = 11;
    cfg.map_revealed = 1;
    cfg.seat_count = 2;
    cfg.seats[0] = (OkxSeat){ 1, 2, 1, 5, 0, 1 };  /* Veruna, team 1, colour 5, the second start */
    cfg.seats[1] = (OkxSeat){ 2, 3, 2, 7, 2, -1 }; /* a hard Zhon computer, any start */
    ASSERT_EQ_INT(0, okx_start_skirmish(&cfg));
    g_booted = 1;
    static OkxPlayer p[8];
    ASSERT_EQ_INT(2, okx_players(p, 8));
    ASSERT_EQ_INT(1, p[0].kind);
    ASSERT_EQ_INT(2, p[0].side);
    ASSERT_EQ_INT(5, p[0].color);
    ASSERT_EQ_INT(2, p[1].kind);
    ASSERT_EQ_INT(3, p[1].side);
    ASSERT_EQ_INT(7, p[1].color);
    /* The monarchs are the lineup's own. */
    static OkxUnit units[64];
    int n = okx_units(units, 64), found = 0;
    for (int i = 0; i < n; i++) {
        OkxDefInfo d;
        if (units[i].player != p[0].index || okx_def_info(units[i].def, &d) != 0) continue;
        if (same_name(d.side, "VER")) found = 1;
    }
    ASSERT(found);
    /* The player stands on the start it claimed, the computer on the
     * one left. */
    int map = -1;
    char name[96];
    for (int i = 0; i < okx_map_count() && map < 0; i++)
        if (okx_map_name(i, name, sizeof name) > 0 && strcmp(name, MAP_NAME) == 0) map = i;
    int32_t xz[16];
    ASSERT(okx_map_starts(map, xz, 8) >= 2);
    int near[2] = { 0, 0 };
    for (int i = 0; i < n; i++) {
        OkxDefInfo d;
        if (okx_def_info(units[i].def, &d) != 0 ||
            (!strstr(d.category, "MONARCH") && !strstr(d.category, "Monarch"))) continue;
        for (int s = 0; s < 2; s++) {
            int k = s == 0 ? 1 : 0;
            float dx = units[i].x - (float)(xz[2 * k] * 16), dz = units[i].z - (float)(xz[2 * k + 1] * 16);
            if (units[i].player == p[s].index && dx * dx + dz * dz < 64.0f * 64.0f) near[s] = 1;
        }
    }
    ASSERT(near[0]);
    ASSERT(near[1]);
}

/* A host on the remastered battlefield rules starts every skirmish on
 * them: the same seed builds another battle, and turning them off
 * builds the first one again (D-036). */
TEST(a_host_on_the_remastered_rules_starts_its_skirmishes_on_them) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "%s", MAP_NAME);
    cfg.seed = 11;
    cfg.ai_players = 1;
    ASSERT_EQ_INT(0, okx_remastered());
    ASSERT_EQ_INT(0, okx_start_skirmish(&cfg));
    g_booted = 1;
    uint32_t original = okx_sim_hash();
    okx_set_remastered(1);
    ASSERT_EQ_INT(1, okx_remastered());
    ASSERT_EQ_INT(0, okx_start_skirmish(&cfg));
    uint32_t remastered = okx_sim_hash();
    okx_set_remastered(0);
    ASSERT_EQ_INT(0, okx_start_skirmish(&cfg));
    ASSERT(remastered != original);
    ASSERT_EQ_INT((int)original, (int)okx_sim_hash());
}

TEST(a_saved_battle_comes_back_as_it_was) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_tick(120);
    static OkxUnit before[256], after[256];
    int n = okx_units(before, 256);
    ASSERT(n >= 1);
    uint32_t tick = okx_tick_count();
    const char *path = "test_embed_save.tsv";
    ASSERT_EQ_INT(0, okx_save(path));
    OkxSaveInfo si;
    ASSERT_EQ_INT(0, okx_save_info(path, &si));
    ASSERT(strcmp(si.map, MAP_NAME) == 0);
    ASSERT_EQ_INT(2, si.players);

    /* Move on, then load: the battle is back where it was saved. */
    okx_tick(300);
    ASSERT_EQ_INT(0, okx_load_save_begin(path));
    float progress = 0.0f;
    int steps = 0;
    while ((rc = okx_load_step(50, &progress, NULL, 0)) == 0 && steps < 10000) steps++;
    ASSERT_EQ_INT(1, rc);
    ASSERT_EQ_INT((int)tick, (int)okx_tick_count());
    int m = okx_units(after, 256);
    ASSERT_EQ_INT(n, m);
    for (int i = 0; i < n; i++) {
        ASSERT_EQ_INT((int)before[i].stable_id, (int)after[i].stable_id);
        ASSERT(before[i].x == after[i].x && before[i].z == after[i].z);
        ASSERT_EQ_INT(before[i].health, after[i].health);
    }
    remove(path);
    /* A save that is not there changes nothing. */
    ASSERT_EQ_INT(-1, okx_load_save_begin("no_such_save_here.tsv"));
    ASSERT_EQ_INT((int)tick, (int)okx_tick_count());
    g_booted = 1;
}

TEST(an_edited_map_saves_and_plays) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int w = 0, h = 0;
    int need = okx_map_cells(NULL, 0, &w, &h);
    ASSERT(need == w * h && w > 16 && h > 16);
    uint8_t *cells = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_map_cells(cells, need, &w, &h));

    /* Raise a 6 by 4 plateau in the middle. The ground follows at once. */
    uint8_t hill[24];
    for (int i = 0; i < 24; i++) hill[i] = 200;
    int x0 = w / 2, z0 = h / 2;
    float before = okx_ground_height((float)(x0 * 16 + 40), (float)(z0 * 16 + 24));
    ASSERT_EQ_INT(0, okx_edit_cells(x0, z0, 6, 4, hill));
    float after = okx_ground_height((float)(x0 * 16 + 40), (float)(z0 * 16 + 24));
    ASSERT(after > before + 20.0f);
    ASSERT_EQ_INT(-1, okx_edit_cells(w - 2, 0, 6, 4, hill));

    /* Paint a block with another picture from the library. */
    static uint32_t lib[4096];
    int nlib = okx_chunk_library(lib, 4096);
    ASSERT(nlib > 50);
    uint32_t current = okx_terrain_chunk_id(0), other = lib[0] == current ? lib[1] : lib[0];
    int pw = 0, ph = 0;
    int pneed = okx_chunk_picture(other, NULL, 0, &pw, &ph);
    ASSERT(pneed > 0 && pneed == pw * ph * 4);
    uint8_t zero = 0, one = 1;
    ASSERT_EQ_INT(0, okx_edit_blocks(2, 3, 1, 1, &other, &one, &zero));

    /* Take one feature away and put a known one down on open ground. */
    int nf = okx_features(NULL, 0);
    OkxFeature *fs = (OkxFeature *)malloc(sizeof(OkxFeature) * (size_t)(nf + 8));
    okx_features(fs, nf);
    int keep_def = fs[0].def;
    ASSERT_EQ_INT(0, okx_feature_remove(fs[nf - 1].index));
    int placed = okx_feature_place(keep_def, x0 - 8, z0 - 8);
    ASSERT(placed >= 0);
    ASSERT_EQ_INT(nf, okx_features(NULL, 0));

    /* Saved as a new map, it is listed and plays with the plateau. */
    ASSERT_EQ_INT(0, okx_map_save("okx test plateau"));
    ASSERT_EQ_INT(-1, okx_map_save("../escape"));
    /* A shipped map's name is refused, whatever the case. */
    ASSERT_EQ_INT(-1, okx_map_save("Two Castles"));
    /* The player's own map saves again under its name. */
    ASSERT_EQ_INT(0, okx_map_save("okx test plateau"));
    /* And comes as a map pack too, to share. */
    FILE *pack = fopen("test_embed_user/maps/okx test plateau.kmp", "rb");
    ASSERT_NOT_NULL(pack);
    char magic[4] = { 0 };
    ASSERT_EQ_INT(4, (int)fread(magic, 1, 4, pack));
    fclose(pack);
    ASSERT(memcmp(magic, "HAPI", 4) == 0);
    int listed = 0;
    char name[96];
    for (int i = 0; i < okx_map_count(); i++)
        if (okx_map_name(i, name, sizeof(name)) > 0 && strcmp(name, "okx test plateau") == 0) listed = 1;
    ASSERT(listed);
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "okx test plateau");
    cfg.ai_players = 1;
    cfg.map_revealed = 1;
    ASSERT_EQ_INT(0, okx_start_skirmish(&cfg));
    uint8_t *again = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_map_cells(again, need, &w, &h));
    for (int z = 0; z < h; z++)
        for (int x = 0; x < w; x++) {
            int in = x >= x0 && x < x0 + 6 && z >= z0 && z < z0 + 4;
            ASSERT_EQ_INT(in ? 200 : cells[z * w + x], again[z * w + x]);
        }
    /* The painted block and the features came back too. */
    int32_t info_blocks = 0;
    OkxTerrainInfo ti;
    ASSERT_EQ_INT(0, okx_terrain_info(&ti));
    info_blocks = ti.blocks_w * ti.blocks_h;
    int32_t *blocks = (int32_t *)malloc(sizeof(int32_t) * 3 * (size_t)info_blocks);
    okx_terrain_blocks(blocks, info_blocks * 3);
    int b = 3 * ti.blocks_w + 2;
    ASSERT(okx_terrain_chunk_id(blocks[3 * b]) == other);
    ASSERT_EQ_INT(1, blocks[3 * b + 1]);
    ASSERT_EQ_INT(nf, okx_features(NULL, 0));
    int found_placed = 0;
    okx_features(fs, nf);
    for (int i = 0; i < nf; i++) {
        OkxFeatureDefInfo d;
        okx_feature_def_info(fs[i].def, &d);
        if (fs[i].def == keep_def && (int)(fs[i].x / 16) >= x0 - 8 && (int)(fs[i].x / 16) < x0 - 4 &&
            (int)(fs[i].z / 16) >= z0 - 8 && (int)(fs[i].z / 16) < z0 - 4) found_placed = 1;
    }
    ASSERT(found_placed);
    free(blocks);
    free(fs);
    free(cells);
    free(again);
    g_booted = 1;
}

/* A host reads the battle every frame: units with their models, poses
 * and scripts, features, shots, effects, fog, the pointer and the
 * sidebar. Two players' hosts read different things, since each sees
 * through its own fog and points at its own ground, so none of it may
 * touch the simulation. The same battle is played twice, once read
 * every tick the way a host reads it and once never read, and the two
 * must hash the same, part by part. */
static int read_everything(int step) {
    static OkxUnit units[1024];
    static OkxFeature feats[4096];
    static OkxProjectile shots[512];
    static OkxEffect fx[512];
    static float mats[128 * 12];
    static uint8_t hidden[128], fog[512 * 512];
    static char names[1024];
    static OkxHudCommand cmds[32];
    static OkxPlayer players[16];
    static uint8_t pixels[256 * 256 * 4];
    int reads = 0;
    int n = okx_units(units, 1024);
    for (int i = 0; i < n && i < 1024; i++) {
        okx_unit_pose(units[i].handle, mats, hidden, 128);
        okx_unit_anim(units[i].handle, names, sizeof names);
        float m, mx;
        okx_unit_mana(units[i].handle, &m, &mx);
        OkxOrder o;
        okx_unit_order(units[i].handle, &o);
        int32_t strips[32];
        okx_def_effect_strips(units[i].def, strips, 32);
        reads += 5;
    }
    int nf = okx_features(feats, 4096);
    for (int i = 0; i < nf && i < 4096; i += 7) { okx_feature_pose(feats[i].index, mats, 128); reads++; }
    int np = okx_projectiles(shots, 512);
    for (int i = 0; i < np && i < 512; i++) { okx_projectile_pose(shots[i].id, mats, 128); reads++; }
    int ne = okx_effects(fx, 512);
    for (int i = 0; i < ne && i < 512; i++) {
        int32_t w = 0, h = 0;
        if (okx_effect_strip(fx[i].sprite, NULL, 0, &w, &h) > 0) reads++;
    }
    int32_t fw = 0, fh = 0;
    okx_fog(fog, sizeof fog, &fw, &fh);
    okx_players(players, 16);
    OkxEconomy eco;
    okx_economy(okx_local_player(), &eco);
    /* What an end screen reads. */
    for (int p = 1; p <= 2; p++) {
        static int32_t series[1024], defs[64], counts[64];
        static OkxBattleEvent moments[64];
        OkxBattleStats st;
        int32_t every = 0;
        okx_battle_stats(p, &st);
        for (int s = 0; s < OKX_SERIES_COUNT; s++) okx_battle_series(p, s, series, 1024, &every);
        okx_battle_built(p, defs, counts, 64);
        okx_battle_events(moments, 64);
        reads += 4;
    }
    if (n > 0) {
        int32_t k, xp, rank;
        okx_unit_record(units[step % n].handle, &k, &xp, &rank);
    }
    /* What a host drawing the field reads. */
    static OkxPieceEvent pieces[OKX_RING];
    okx_piece_events(0, pieces, OKX_RING);
    static OkxFeatureEvent fevents[OKX_RING];
    okx_feature_events(0, fevents, OKX_RING);
    float ws, wm, wx, wz;
    okx_wind(&ws, &wm, &wx, &wz);
    static OkxBlast blasts[OKX_RING];
    int nb = okx_blasts(0, blasts, OKX_RING);
    for (int i = 0; i < nb && i < OKX_RING; i++) {
        OkxWeaponInfo wi;
        okx_weapon_info(blasts[i].def, blasts[i].slot, &wi);
        reads++;
    }
    /* The pointer wanders, and the sidebar is read for a selection. */
    if (n > 0) {
        const OkxUnit *u = &units[step % n];
        int32_t clear;
        okx_cursor_at(u->x, u->z, u->handle, &clear);
        okx_cursor_at(u->x + 200.0f, u->z - 120.0f, -1, &clear);
        okx_ground_height(u->x, u->z);
        if (u->player == okx_local_player()) {
            okx_select(&u->handle, 1, 0);
            int nc = okx_hud_commands(cmds, 32);
            for (int k = 0; k < nc && k < 32; k++) {
                int32_t w = 0, h = 0;
                okx_hud_command_art(cmds[k].id, 2, pixels, sizeof pixels, &w, &h);
            }
            okx_select(NULL, 0, 0);
        }
        okx_set_view((int32_t)u->x, (int32_t)u->z, 640, 480);
    }
    return reads + n + nf + np + ne;
}

static int play_battle(int read, uint32_t *parts, int cap, int *read_count) {
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "%s", MAP_NAME);
    snprintf(cfg.kingdom, sizeof(cfg.kingdom), "aramon");
    cfg.ai_players = 1;
    cfg.seed = 4242;
    if (okx_start_skirmish(&cfg) != 0) return -1;
    *read_count = 0;
    for (int t = 0; t < 60 * 40; t += 4) {
        okx_tick(4);
        if (read) *read_count += read_everything(t);
    }
    int np = okx_sim_hash_parts(parts, cap);
    okx_end_game();
    return np;
}

/* Between battles the interface's own music plays, and moves on with
 * okx_tick, and a battle plays its side's list in its place. */
TEST(the_menus_play_the_interface_music) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    okx_end_game();
    g_booted = 0;
    if (okx_audio(1, 0, 1) != 0) { SKIP("no audio device"); }
    ASSERT_EQ_INT(0, okx_tick(1));
    int menu = okx_music_track();
    ASSERT(menu > 0);
    ASSERT_EQ_INT(0, start_battle(1));
    okx_tick(1);
    int battle = okx_music_track();
    ASSERT(battle > 0 && battle != menu);
    okx_end_game();
    g_booted = 0;
    okx_tick(0);
    ASSERT_EQ_INT(menu, okx_music_track());
    okx_audio(0, 0, 0);
    ASSERT_EQ_INT(0, okx_music_track());
}

/* Seeing all shows the whole map and the enemy's units, changes nothing
 * the battle holds, and a new battle starts in the player's own sight. */
TEST(seeing_all_shows_the_whole_field) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    okx_end_game();
    g_booted = 0;
    ASSERT_EQ_INT(0, start_battle(0));
    okx_tick(30);
    int w = 0, h = 0;
    int need = okx_fog(NULL, 0, &w, &h);
    ASSERT(need > 0);
    uint8_t *fog = (uint8_t *)malloc((size_t)need);
    okx_fog(fog, need, &w, &h);
    int clear = 0;
    for (int i = 0; i < need; i++) clear += fog[i] == 2;
    ASSERT(clear < need);
    int mine = okx_units(NULL, 0);
    uint32_t hash = okx_sim_hash();

    okx_see_all(1);
    okx_fog(fog, need, &w, &h);
    for (int i = 0; i < need; i++) ASSERT_EQ_INT(2, fog[i]);
    ASSERT(okx_units(NULL, 0) > mine);
    ASSERT_EQ_INT((int)hash, (int)okx_sim_hash());

    okx_end_game();
    ASSERT_EQ_INT(0, start_battle(0));
    okx_tick(30);
    okx_fog(fog, need, &w, &h);
    clear = 0;
    for (int i = 0; i < need; i++) clear += fog[i] == 2;
    ASSERT(clear < need);
    free(fog);
    okx_end_game();
    g_booted = 0;
}

/* A def by its UnitName, or -1. */
static int def_named(const char *name) {
    int n = okx_def_count();
    for (int i = 0; i < n; i++) {
        OkxDefInfo d;
        if (okx_def_info(i, &d) == 0 && same_name(d.name, name)) return i;
    }
    return -1;
}

/* What an end screen reads, through the host: the first blood and its
 * damage, the champion and its rank, a building raised by its kind, a
 * sample every 5 s with the value now last, and the defeat as a kingdom
 * fallen, all as the original's tallies stand beside them. */
TEST(a_decided_battle_hands_out_its_record) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    okx_end_game();
    g_booted = 0;
    ASSERT_EQ_INT(0, start_battle(1));
    int me = okx_local_player();
    static OkxUnit units[512];
    int n = okx_units(units, 512), king = -1, king_def = -1;
    for (int i = 0; i < n && king < 0; i++)
        if (units[i].player == me) { king = units[i].handle; king_def = units[i].def; }
    ASSERT(king >= 0);
    OkxBattleStats st;
    ASSERT_EQ_INT(-1, okx_battle_stats(0, &st));
    ASSERT_EQ_INT(-1, okx_battle_stats(9, &st));
    ASSERT_EQ_INT(0, okx_battle_stats(me, &st));
    ASSERT_EQ_INT(-1, st.best_def);
    ASSERT_EQ_INT(-1, st.best_handle);
    ASSERT_EQ_INT(0, okx_battle_events(NULL, 0));

    /* A sword of ours handed to the computer, and the monarch fells it. */
    int sword_def = def_named("ARASWORD");
    ASSERT(sword_def >= 0);
    int sword = okx_place_unit(sword_def, me);
    ASSERT(sword >= 0);
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_GIVE_UNITS, sword, 0, 0, -1, -1, 2));
    okx_tick(2);
    OkxUnit su;
    ASSERT_EQ_INT(0, okx_unit(sword, &su));
    ASSERT_EQ_INT(2, su.player);
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_ATTACK_ORDER, king, 0, 0, sword, -1, 0));
    for (int t = 0; t < 60 * 60 && st.kills == 0; t += 30) {
        okx_tick(30);
        okx_battle_stats(me, &st);
    }
    ASSERT_EQ_INT(1, st.kills);
    ASSERT(st.score > 0);
    ASSERT(st.damage_dealt >= su.max_health);
    OkxBattleStats foe;
    ASSERT_EQ_INT(0, okx_battle_stats(2, &foe));
    ASSERT_EQ_INT(1, foe.losses);
    ASSERT(foe.damage_taken >= st.damage_dealt);
    ASSERT_EQ_INT(king_def, st.best_def);
    ASSERT_EQ_INT(1, st.best_kills);
    ASSERT_EQ_INT(st.score, st.best_xp);
    ASSERT_EQ_INT(1, st.best_standing);
    ASSERT_EQ_INT(king, st.best_handle);
    int32_t kills = 0, xp = 0, rank = -1;
    ASSERT_EQ_INT(0, okx_unit_record(king, &kills, &xp, &rank));
    ASSERT_EQ_INT(1, kills);
    ASSERT_EQ_INT(st.best_xp, xp);
    ASSERT_EQ_INT(st.best_rank, rank);
    ASSERT_EQ_INT(-1, okx_unit_record(-1, &kills, &xp, &rank));
    static OkxBattleEvent moments[64];
    ASSERT_EQ_INT(1, okx_battle_events(moments, 64));
    ASSERT_EQ_INT(OKX_EVENT_FIRST_BLOOD, moments[0].kind);
    ASSERT_EQ_INT(me, moments[0].player);
    ASSERT_EQ_INT(2, moments[0].other);
    ASSERT_EQ_INT(king_def, moments[0].def);
    ASSERT_EQ_INT(sword_def, moments[0].other_def);

    /* The cheapest building the monarch can raise near itself, raised.
     * Walls and gates are never counted built, as the original never
     * counts them, and a lodestone wants a sacred site. */
    static int32_t opts[256];
    int k = okx_def_buildables(king_def, opts, 256), product = -1, cost = 0;
    int32_t sx = 0, sy = 0;
    ASSERT_EQ_INT(0, okx_unit(king, &su));
    for (int j = 0; j < k; j++) {
        OkxDefInfo d;
        if (okx_def_info(opts[j], &d) != 0 || !d.is_building) continue;
        if (strstr(d.name, "WALL") || strstr(d.name, "GATE")) continue;
        if (product >= 0 && d.build_cost >= cost) continue;
        int32_t x0 = 0, y0 = 0, found = 0;
        for (int r = 96; r <= 800 && !found; r += 32)
            for (int a = 0; a < 8 && !found; a++) {
                int32_t x = (int32_t)su.x + (a % 3 - 1) * r, y = (int32_t)su.z + (a / 3 - 1) * r;
                if (okx_build_site(opts[j], x, y, &x0, &y0)) found = 1;
            }
        if (!found) continue;
        product = opts[j];
        cost = d.build_cost;
        sx = x0;
        sy = y0;
    }
    ASSERT(product >= 0);
    ASSERT_EQ_INT(0, okx_command(3, king, sx, sy, -1, product, 0));
    for (int t = 0; t < 60 * 180 && st.buildings_raised == 0; t += 60) {
        okx_tick(60);
        okx_battle_stats(me, &st);
    }
    ASSERT_EQ_INT(1, st.buildings_raised);
    ASSERT_EQ_INT(0, st.units_trained);
    int32_t defs[8], counts[8];
    ASSERT_EQ_INT(1, okx_battle_built(me, defs, counts, 8));
    ASSERT_EQ_INT(product, defs[0]);
    ASSERT_EQ_INT(1, counts[0]);
    ASSERT(st.mana_spent >= (float)cost * 0.99f);
    ASSERT(st.mana_gathered > 0.0f);

    /* A sample every 5 s, the kill among them, and the value now last. */
    static int32_t series[1024];
    int32_t every = 0;
    int m = okx_battle_series(me, OKX_SERIES_KILLS, series, 1024, &every);
    ASSERT_EQ_INT(300, every);
    ASSERT(m >= 3);
    ASSERT((uint32_t)(m - 2) * (uint32_t)every <= okx_tick_count());
    ASSERT_EQ_INT(0, series[0]);
    for (int i = 1; i < m; i++) ASSERT(series[i] >= series[i - 1]);
    ASSERT_EQ_INT(1, series[m - 1]);
    ASSERT_EQ_INT(m, okx_battle_series(me, OKX_SERIES_ARMY, series, 1024, &every));
    ASSERT(series[0] >= 1);
    ASSERT_EQ_INT(m, okx_battle_series(me, OKX_SERIES_GATHERED, series, 1024, &every));
    ASSERT_EQ_INT((int)st.mana_gathered, series[m - 1]);
    ASSERT_EQ_INT(m, okx_battle_series(me, OKX_SERIES_KILLS, NULL, 0, &every));
    ASSERT_EQ_INT(-1, okx_battle_series(me, OKX_SERIES_COUNT, series, 1024, &every));

    /* A save keeps it all, and a load brings it back. */
    const char *path = "test_embed_record.tsv";
    ASSERT_EQ_INT(0, okx_save(path));
    okx_tick(600);
    ASSERT_EQ_INT(0, okx_load_save_begin(path));
    float progress = 0.0f;
    int rc = 0, steps = 0;
    while ((rc = okx_load_step(50, &progress, NULL, 0)) == 0 && steps < 10000) steps++;
    ASSERT_EQ_INT(1, rc);
    remove(path);
    OkxBattleStats back;
    ASSERT_EQ_INT(0, okx_battle_stats(me, &back));
    ASSERT_EQ_INT(st.kills, back.kills);
    ASSERT_EQ_INT(st.damage_dealt, back.damage_dealt);
    ASSERT_EQ_INT(st.buildings_raised, back.buildings_raised);
    ASSERT_EQ_INT(st.best_def, back.best_def);
    ASSERT_EQ_INT(st.best_kills, back.best_kills);
    ASSERT_EQ_INT(1, back.best_standing);
    ASSERT(back.mana_gathered == st.mana_gathered);
    ASSERT_EQ_INT(1, okx_battle_built(me, defs, counts, 8));
    ASSERT_EQ_INT(product, defs[0]);
    ASSERT_EQ_INT(1, okx_battle_events(moments, 64));
    ASSERT_EQ_INT(m, okx_battle_series(me, OKX_SERIES_KILLS, series, 1024, &every));
    ASSERT_EQ_INT(1, series[m - 2]);

    /* The player hands the rest over and has nothing left: a defeat, and
     * the kingdom fallen at the tick it went. */
    n = okx_units(units, 512);
    for (int i = 0; i < n; i++)
        if (units[i].player == me) okx_command(TAK_CMD_GIVE_UNITS, units[i].handle, 0, 0, -1, -1, 2);
    for (int t = 0; t < 600 && okx_outcome() == 0; t += 10) okx_tick(10);
    ASSERT_EQ_INT(-1, okx_outcome());
    int e = okx_battle_events(moments, 64), fell = -1;
    for (int i = 0; i < e; i++)
        if (moments[i].kind == OKX_EVENT_FELL && moments[i].player == me) fell = i;
    ASSERT(fell > 0);
    ASSERT_EQ_INT(0, okx_battle_stats(me, &st));
    ASSERT_EQ_INT(moments[fell].tick, st.fell_tick);
    ASSERT(st.fell_tick > 0 && (uint32_t)st.fell_tick <= okx_tick_count());
    ASSERT_EQ_INT(0, st.best_standing);
    ASSERT_EQ_INT(-1, st.best_handle);
    ASSERT_EQ_INT(king_def, st.best_def);
    okx_end_game();
    g_booted = 0;
}

/* Beaten with two computers at war, the player watches them fight on: the
 * ticks run, they keep building, and the defeat stands. */
TEST(a_lost_battle_plays_on_between_the_computers) {
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) { SKIP("no game data"); }
    okx_end_game();
    g_booted = 0;
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "Angvir's Maze");
    cfg.map_revealed = 1;
    cfg.seed = 777;
    cfg.seat_count = 3;
    for (int i = 0; i < 3; i++) {
        cfg.seats[i].kind = i == 0 ? 1 : 2;
        cfg.seats[i].side = i;
        cfg.seats[i].team = i + 1;
        cfg.seats[i].color = i;
        cfg.seats[i].start = -1;
    }
    ASSERT_EQ_INT(0, okx_start_skirmish(&cfg));
    okx_tick(2);
    ASSERT_EQ_INT(0, okx_play_on());
    ASSERT_EQ_INT(0, okx_playing_on());

    /* The player hands the army to a computer and has nothing left. */
    static OkxUnit units[256];
    int n = okx_units(units, 256), me = okx_local_player(), given = 0;
    for (int i = 0; i < n; i++)
        if (units[i].player == me && okx_command(TAK_CMD_GIVE_UNITS, units[i].handle, 0, 0, -1, -1, 2) == 0)
            given++;
    ASSERT(given > 0);
    for (int t = 0; t < 600 && okx_outcome() == 0; t += 10) okx_tick(10);
    ASSERT_EQ_INT(-1, okx_outcome());
    uint32_t ended = okx_tick_count();
    okx_tick(600);
    ASSERT(okx_tick_count() - ended < 600);

    int had2 = okx_unit_count(2), had3 = okx_unit_count(3);
    ASSERT_EQ_INT(1, okx_play_on());
    ASSERT_EQ_INT(1, okx_playing_on());
    uint32_t from = okx_tick_count();
    for (int t = 0; t < 60 * 120; t += 60) {
        okx_tick(60);
        ASSERT_EQ_INT(-1, okx_outcome());
    }
    ASSERT_EQ_INT((int)(from + 60 * 120), (int)okx_tick_count());
    ASSERT(okx_unit_count(2) > had2);
    ASSERT(okx_unit_count(3) > had3);
    ASSERT_EQ_INT(0, okx_unit_count(me));
    ASSERT_EQ_INT(1, okx_playing_on());

    /* A new battle starts without it. */
    okx_end_game();
    ASSERT_EQ_INT(0, start_battle(1));
    ASSERT_EQ_INT(0, okx_playing_on());
    okx_end_game();
    g_booted = 0;
}

TEST(what_a_host_reads_never_changes_the_battle) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_end_game();
    g_booted = 0;
    static uint32_t read_parts[8192], plain_parts[8192];
    int reads = 0, none = 0;
    /* Unread first, so the models and art the reads load are not warm
     * yet when the unread battle plays. */
    int b = play_battle(0, plain_parts, 8192, &none);
    int a = play_battle(1, read_parts, 8192, &reads);
    printf("(%d reads) ", reads);
    ASSERT(a > 9 && a == b);
    ASSERT(reads > 1000);
    int first = -1;
    for (int i = 0; i < a && i < 8192 && first < 0; i++)
        if (read_parts[i] != plain_parts[i]) first = i;
    if (first >= 0) printf("(part %d differs: %08x read, %08x not) ", first,
                           read_parts[first], plain_parts[first]);
    ASSERT_EQ_INT(-1, first);
}

/* A def of the local side that trains units, and one it trains. */
static int find_factory(int *product) {
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player();
    for (int i = 0; i < n; i++) {
        if (units[i].player != me) continue;
        static int32_t opts[256];
        int k = okx_def_buildables(units[i].def, opts, 256);
        for (int j = 0; j < k; j++) {
            OkxDefInfo d;
            if (okx_def_info(opts[j], &d) != 0 || !d.is_building) continue;
            static int32_t made[256];
            int m = okx_def_buildables(opts[j], made, 256);
            for (int q = 0; q < m; q++) {
                OkxDefInfo pd;
                if (okx_def_info(made[q], &pd) == 0 && !pd.is_building) {
                    *product = made[q];
                    return opts[j];
                }
            }
        }
    }
    return -1;
}

/* Open ground for def near the unit, away from (ax, ay). */
static int open_spot(int handle, int def, int32_t ax, int32_t ay,
                     int32_t *cx, int32_t *cy) {
    OkxUnit u;
    if (okx_unit(handle, &u) != 0) return 0;
    for (int r = 96; r <= 800; r += 32)
        for (int a = 0; a < 8; a++) {
            int32_t x = (int32_t)u.x + (a % 3 - 1) * r, y = (int32_t)u.z + (a / 3 - 1) * r;
            if (!okx_build_site_facing(def, 0, x, y, cx, cy)) continue;
            int32_t dx = *cx - ax, dy = *cy - ay;
            if (dx > 64 || dx < -64 || dy > 64 || dy < -64) return 1;
        }
    return 0;
}

/* Ctrl on a Zhon builder's card through the host: the click places the
 * goblin once, Shift or not, and the builder summons it there without
 * end until the right click's set_repeat 0. OKX_ENDLESS on a build order
 * does the same (legacy:150077-150084). */
TEST(a_walking_builder_summons_without_end) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int hand = def_named("ZONHAND"), gob = def_named("ZONGOB");
    ASSERT(hand >= 0 && gob >= 0);
    int me = okx_local_player();
    int b = okx_place_unit(hand, me);
    ASSERT(b >= 0);
    int32_t cx = 0, cy = 0;
    ASSERT(open_spot(b, gob, -100000, -100000, &cx, &cy));
    okx_select(&b, 1, 0);
    okx_arm_build(gob, 1);
    int32_t def = -1;
    ASSERT_EQ_INT(OKX_ARM_BUILD, okx_armed(&def));
    ASSERT_EQ_INT(gob, def);
    okx_click((float)cx, (float)cy, -1, 1);
    ASSERT_EQ_INT(OKX_ARM_NONE, okx_armed(NULL));
    okx_tick(2);
    ASSERT_EQ_INT(gob, okx_factory_repeat_of(b));
    ASSERT_EQ_INT(0, okx_factory_set_repeat(b, gob, 0));
    okx_tick(1);
    ASSERT_EQ_INT(-1, okx_factory_repeat_of(b));
    /* The frame stays on the first spot, so the order takes another. */
    int32_t dx = 0, dy = 0;
    ASSERT(open_spot(b, gob, cx, cy, &dx, &dy));
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_BUILD_ORDER, b, dx, dy, -1, gob, OKX_ENDLESS));
    okx_tick(2);
    ASSERT_EQ_INT(gob, okx_factory_repeat_of(b));
    ASSERT_EQ_INT(0, okx_factory_set_repeat(b, gob, 0));
    okx_tick(1);
    okx_select(NULL, 0, 0);
}

/* The build buttons as the host sends them: five with Shift, some taken
 * back, Ctrl's run without end and its cancel, and a rally with a
 * standing patrol behind it read back as the factory's orders. */
TEST(a_factory_queue_takes_counts_repeats_and_a_rally) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int product = -1;
    int fdef = find_factory(&product);
    ASSERT(fdef >= 0 && product >= 0);
    int me = okx_local_player();
    int f = okx_place_unit(fdef, me);
    ASSERT(f >= 0);
    ASSERT_EQ_INT(0, okx_factory_add(f, product, 5));
    okx_tick(1);
    ASSERT_EQ_INT(5, okx_factory_queue(f, product));
    ASSERT_EQ_INT(0, okx_factory_add(f, product, -2));
    okx_tick(1);
    ASSERT_EQ_INT(3, okx_factory_queue(f, product));
    ASSERT_EQ_INT(-1, okx_factory_repeat_of(f));
    ASSERT_EQ_INT(0, okx_factory_set_repeat(f, product, 1));
    okx_tick(1);
    ASSERT_EQ_INT(product, okx_factory_repeat_of(f));
    ASSERT_EQ_INT(0, okx_factory_set_repeat(f, product, 0));
    okx_tick(1);
    ASSERT_EQ_INT(-1, okx_factory_repeat_of(f));
    ASSERT_EQ_INT(0, okx_factory_queue(f, product));

    OkxUnit fu;
    ASSERT_EQ_INT(0, okx_unit(f, &fu));
    int32_t rx = (int32_t)fu.x + 200, ry = (int32_t)fu.z + 100;
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_MOVE_ORDER, f, rx, ry, -1, -1, 0));
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_PATROL_ORDER, f, rx + 100, ry, -1, -1, OKX_QUEUE));
    okx_tick(1);
    OkxOrderLeg legs[4];
    ASSERT_EQ_INT(2, okx_unit_orders(f, legs, 4));
    ASSERT_EQ_INT(TAK_CMD_RALLY_ORDER, legs[0].kind);
    ASSERT_EQ_INT(rx, legs[0].x);
    ASSERT_EQ_INT(ry, legs[0].y);
    ASSERT_EQ_INT(TAK_CMD_PATROL_ORDER, legs[1].kind);
    ASSERT(legs[1].flags & OKX_LEG_QUEUED);
    ASSERT_EQ_INT(0, okx_command(4, f, 0, 0, -1, -1, 0));
    okx_tick(1);
    ASSERT_EQ_INT(0, okx_unit_orders(f, legs, 4));
}

/* A factory still going up takes a queue while the host allows it, and
 * nothing else: not a rally, not a stop. */
TEST(an_unfinished_factory_takes_a_queue_while_the_host_allows_it) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int product = -1;
    int fdef = find_factory(&product);
    ASSERT(fdef >= 0);
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player(), builder = -1;
    for (int i = 0; i < n && builder < 0; i++) {
        if (units[i].player != me || units[i].state != OKX_UNIT_ACTIVE) continue;
        static int32_t opts[256];
        int k = okx_def_buildables(units[i].def, opts, 256);
        for (int j = 0; j < k; j++) if (opts[j] == fdef) builder = i;
    }
    ASSERT(builder >= 0);
    const OkxUnit *b = &units[builder];
    int32_t sx = 0, sy = 0, found = 0;
    for (int r = 160; r <= 900 && !found; r += 32)
        for (int a = 0; a < 8 && !found; a++) {
            int32_t x = (int32_t)b->x + (a % 3 - 1) * r, y = (int32_t)b->z + (a / 3 - 1) * r;
            if (okx_build_site(fdef, x, y, &sx, &sy)) found = 1;
        }
    ASSERT(found);
    ASSERT_EQ_INT(0, okx_command(3, b->handle, sx, sy, -1, fdef, 0));
    okx_tick(2);
    OkxOrder o;
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &o));
    int frame = o.building;
    ASSERT(frame >= 0);
    OkxUnit fr;
    ASSERT_EQ_INT(0, okx_unit(frame, &fr));
    ASSERT_EQ_INT(1, fr.building);

    ASSERT_EQ_INT(OKX_ALLOW_QUEUE_UNFINISHED, okx_allowed() & OKX_ALLOW_QUEUE_UNFINISHED);
    okx_allow(0);
    okx_factory_add(frame, product, 2);
    okx_tick(1);
    ASSERT_EQ_INT(0, okx_factory_queue(frame, -1));
    okx_allow(OKX_ALLOW_QUEUE_UNFINISHED);
    ASSERT_EQ_INT(0, okx_factory_add(frame, product, 2));
    okx_tick(1);
    ASSERT_EQ_INT(2, okx_factory_queue(frame, -1));
    okx_command(TAK_CMD_MOVE_ORDER, frame, sx + 200, sy, -1, -1, 0);
    okx_tick(1);
    ASSERT_EQ_INT(0, okx_unit_orders(frame, NULL, 0));
}

/* Shift through okx_command and through the game's own click: the
 * orders queue, the host reads them back in turn, and a click the host
 * calls open ground is never taken for the unit beside it. */
TEST(shift_queues_orders_and_the_host_reads_them_back) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player(), mine = -1;
    for (int i = 0; i < n && mine < 0; i++) {
        OkxDefInfo d;
        if (units[i].player == me && units[i].state == OKX_UNIT_ACTIVE &&
            okx_def_info(units[i].def, &d) == 0 && !d.is_building) mine = i;
    }
    ASSERT(mine >= 0);
    const OkxUnit *u = &units[mine];
    int32_t x = (int32_t)u->x, y = (int32_t)u->z;
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_MOVE_ORDER, u->handle, x + 120, y, -1, -1, 0));
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_MOVE_ORDER, u->handle, x + 120, y + 120, -1, -1,
                                 OKX_QUEUE));
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_PATROL_ORDER, u->handle, x, y + 120, -1, -1,
                                 OKX_QUEUE));
    okx_tick(1);
    OkxOrderLeg legs[8];
    ASSERT_EQ_INT(3, okx_unit_orders(u->handle, legs, 8));
    ASSERT_EQ_INT(TAK_CMD_MOVE_ORDER, legs[0].kind);
    ASSERT_EQ_INT(0, legs[0].flags & OKX_LEG_QUEUED);
    ASSERT_EQ_INT(y + 120, legs[1].y);
    ASSERT(legs[1].flags & OKX_LEG_QUEUED);
    ASSERT_EQ_INT(TAK_CMD_PATROL_ORDER, legs[2].kind);
    ASSERT_EQ_INT(-1, okx_unit_orders(-5, legs, 8));

    /* The game's click with Shift appends a move, without replaces. */
    okx_select(&u->handle, 1, 0);
    okx_click((float)(x - 150), (float)y, -1, 1);
    okx_tick(1);
    ASSERT_EQ_INT(4, okx_unit_orders(u->handle, NULL, 0));
    okx_click((float)(x - 150), (float)y, -1, 0);
    okx_tick(1);
    ASSERT_EQ_INT(1, okx_unit_orders(u->handle, NULL, 0));

    /* Open ground right beside another unit of ours is still ground. */
    int other = okx_place_unit(u->def, me);
    ASSERT(other >= 0);
    OkxUnit ou;
    ASSERT_EQ_INT(0, okx_unit(other, &ou));
    okx_select(&u->handle, 1, 0);
    okx_click(ou.x + 24.0f, ou.z, -1, 0);
    int32_t sel[4];
    ASSERT_EQ_INT(1, okx_selection(sel, 4));
    ASSERT_EQ_INT(u->handle, sel[0]);

    /* A queue bit on an order that takes none is dropped. */
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_SET_AGGRO_ORDER, u->handle, 0, 0, -1, -1,
                                 1 | OKX_QUEUE));
    okx_tick(1);
    /* Every gate turns like any other building. */
    int gates = 0;
    for (int d = 0; d < okx_def_count(); d++) {
        OkxDefInfo gi;
        if (okx_def_info(d, &gi) != 0 || !strstr(gi.name, "GATE")) continue;
        gates++;
        ASSERT_EQ_INT(1, okx_def_can_turn(d));
    }
    ASSERT(gates >= 3);
    /* Ships have hulls from their models and nothing else has one. */
    int ships = 0;
    for (int d = 0; d < okx_def_count(); d++) {
        OkxDefInfo si;
        if (okx_def_info(d, &si) != 0) continue;
        int32_t fore = -1, aft = -1, half = -1;
        int32_t ship = okx_def_hull(d, &fore, &aft, &half);
        if (strcmp(si.name, "ARAWAR") == 0) {
            ASSERT_EQ_INT(1, ship);
            ASSERT(fore == 75 && aft == 75 && half == 25);
        }
        if (si.is_building) ASSERT_EQ_INT(0, ship);
        if (ship == 1) ships++;
        else ASSERT(fore == 0 && aft == 0 && half == 0);
    }
    ASSERT(ships >= 10);
    /* With audio off no sound plays, and one that is not there never does. */
    okx_audio(0, 100, 0);
    ASSERT_EQ_INT(-1, okx_play_ui_sound("menubutton.wav", 85));
    ASSERT_EQ_INT(-1, okx_play_ui_sound("no_such_sound_anywhere.wav", 85));
    ASSERT_EQ_INT(-1, okx_play_ui_sound("", 85));
    okx_cancel();
}


/* The interface art a front end draws its menus with, by sheet, entry
 * and frame, with the frame's origin and the entry's frame count. */
TEST(the_interface_art_comes_by_sheet_and_entry) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int w = 0, h = 0, ox = -99, oy = -99, frames = 0;
    int need = okx_gui_art("mainscreen", "ExitButton", 1, NULL, 0, &w, &h, &ox, &oy, &frames);
    ASSERT_EQ_INT(3, frames);
    ASSERT(need > 0 && need == w * h * 4);
    uint8_t *px = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_gui_art("mainscreen.gaf", "exitbutton", 1, px, need,
                                    &w, &h, &ox, &oy, &frames));
    int opaque = 0;
    for (int i = 0; i < w * h; i++) if (px[i * 4 + 3] == 255) opaque++;
    free(px);
    ASSERT(opaque > 0);
    ASSERT_EQ_INT(-1, okx_gui_art("mainscreen", "ExitButton", 3, NULL, 0, &w, &h, &ox, &oy, &frames));
    ASSERT_EQ_INT(-1, okx_gui_art("mainscreen", "NoSuchEntry", 0, NULL, 0, &w, &h, &ox, &oy, &frames));
    ASSERT(okx_gui_art("scrollbars", "CheckBox", 4, NULL, 0, &w, &h, &ox, &oy, &frames) > 0);
    ASSERT_EQ_INT(6, frames);
}

/* A painted model asks for its picture by name. A feature's comes in
 * the palette of the world named, the same pixels the battle draws
 * wherever the battle draws them opaque, and a 3DO texture by its name. */
TEST(a_picture_comes_by_name_for_painting_a_model) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int n = okx_features(NULL, 0);
    OkxFeature *f = (OkxFeature *)malloc(sizeof(OkxFeature) * (size_t)(n > 0 ? n : 1));
    ASSERT_EQ_INT(n, okx_features(f, n));
    int def = -1;
    for (int i = 0; i < n && def < 0; i++) if (f[i].sprite >= 0) def = f[i].sprite;
    free(f);
    ASSERT(def >= 0);
    OkxFeatureDefInfo d;
    ASSERT_EQ_INT(0, okx_feature_def_info(def, &d));
    OkxMapInfo mi;
    int map = -1;
    char name[96];
    for (int i = 0; i < okx_map_count() && map < 0; i++)
        if (okx_map_name(i, name, sizeof name) > 0 && strcmp(name, MAP_NAME) == 0) map = i;
    ASSERT_EQ_INT(0, okx_map_info(map, &mi));

    int w = 0, h = 0, bw = 0, bh = 0;
    int need = okx_sprite(def, NULL, 0, &w, &h);
    ASSERT(need > 0);
    uint8_t *a = (uint8_t *)malloc((size_t)need), *b = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_sprite(def, a, need, &w, &h));
    ASSERT_EQ_INT(need, okx_sprite_by_name(d.name, mi.kingdom, b, need, &bw, &bh));
    ASSERT_EQ_INT(w, bw);
    ASSERT_EQ_INT(h, bh);
    int same = 0, differ = 0, opaque = 0;
    for (int i = 0; i < w * h; i++) {
        if (a[i * 4 + 3] != 255) continue;
        opaque++;
        if (memcmp(a + i * 4, b + i * 4, 4) == 0) same++; else differ++;
    }
    ASSERT_EQ_INT(-1, okx_sprite_by_name("NoSuchFeature", NULL, NULL, 0, &w, &h));
    /* By its sequence name too. */
    ASSERT_EQ_INT(need, okx_sprite_by_name(d.seqname, mi.kingdom, NULL, 0, &bw, &bh));
    free(a);
    free(b);
    printf("(%s in %s: %d of %d opaque texels agree) ", d.name, mi.kingdom, same, opaque);
    ASSERT(opaque > 0);
    ASSERT_EQ_INT(0, differ);

    need = okx_texture_by_name("basiliskstone", "aramon", NULL, 0, &w, &h);
    ASSERT(need > 0 && need == w * h * 4);
    uint8_t *t = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_texture_by_name("BasiliskStone", "ara", t, need, &w, &h));
    ASSERT_EQ_INT(0, t[3]);                     /* its corner colour is clear */
    free(t);
    ASSERT_EQ_INT(-1, okx_texture_by_name("nosuchtexture", NULL, NULL, 0, &w, &h));
}

/* A weapon with a lightmap marks its shots and their blasts with it,
 * so a host lights the ground under those alone. */
TEST(a_shot_and_its_blast_carry_the_weapons_lightmap) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int def = -1;
    for (int i = 0; i < okx_def_count() && def < 0; i++) {
        OkxDefInfo di;
        if (okx_def_info(i, &di) == 0 && same_name(di.name, "TARPRIES")) def = i;
    }
    ASSERT(def >= 0);
    int h = okx_place_unit(def, okx_local_player());
    ASSERT(h >= 0);
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    ASSERT_EQ_INT(0, okx_command(20, h, (int)u.x + 160, (int)u.z, -1, -1, 0));
    static OkxProjectile ps[256];
    static OkxEffect fx[256];
    int shot = 0, blast = 0, unlit = 0;
    for (int t = 0; t < 900 && !(shot && blast); t += 2) {
        okx_tick(2);
        int k = okx_projectiles(ps, 256);
        for (int i = 0; i < k && i < 256; i++) {
            if (ps[i].lightmap == OKX_LIGHTMAP_SMALL) shot = 1;
            else if (ps[i].lightmap != OKX_LIGHTMAP_NONE) unlit++;
        }
        k = okx_effects(fx, 256);
        for (int i = 0; i < k && i < 256; i++)
            if (fx[i].kind == OKX_EFFECT_IMPACT && fx[i].lightmap == OKX_LIGHTMAP_SMALL) blast = 1;
    }
    printf("(shot %d, blast %d) ", shot, blast);
    ASSERT(shot);
    ASSERT(blast);
    ASSERT_EQ_INT(0, unlit);
}

static int place_named(const char *name, int *def_out) {
    int def = -1;
    for (int i = 0; i < okx_def_count() && def < 0; i++) {
        OkxDefInfo di;
        if (okx_def_info(i, &di) == 0 && same_name(di.name, name)) def = i;
    }
    if (def_out) *def_out = def;
    return def >= 0 ? okx_place_unit(def, okx_local_player()) : -1;
}

TEST(a_nimbus_rides_its_caster) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int h = place_named("VERMAGE", NULL);
    ASSERT(h >= 0);
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    ASSERT_EQ_INT(0, okx_command(20, h, (int)u.x + 200, (int)u.z, -1, -1, 0));
    static OkxEffect fx[256];
    int lit = 0, sprite = -1;
    for (int t = 0; t < 900 && !lit; t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 256);
        for (int i = 0; i < k && i < 256; i++) {
            if (fx[i].kind != OKX_EFFECT_NIMBUS) { ASSERT_EQ_INT(-1, fx[i].follow); continue; }
            if (fx[i].follow != h) continue;
            lit = 1;
            sprite = fx[i].sprite;
            /* nimbus_veruna: 11 pictures, each three 30 Hz frames. */
            ASSERT_EQ_INT(11, fx[i].frame_count);
            ASSERT_EQ_INT(6, fx[i].ticks_per_frame);
            ASSERT_EQ_INT(0, fx[i].loops);
        }
    }
    ASSERT(lit);
    ASSERT_EQ_INT(11, okx_effect_frames(sprite, NULL, 0));
    /* Walk the caster away: the glow goes where it goes until it ends. */
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    float x0 = u.x, z0 = u.z;
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_MOVE_ORDER, h, (int)u.x - 400, (int)u.z, -1, -1, 0));
    int rode = 0, moved = 0, ended = 0;
    for (int t = 0; t < 400 && !ended; t += 2) {
        okx_tick(2);
        ASSERT_EQ_INT(0, okx_unit(h, &u));
        int k = okx_effects(fx, 256), on = 0;
        for (int i = 0; i < k && i < 256; i++) {
            if (fx[i].kind != OKX_EFFECT_NIMBUS || fx[i].follow != h) continue;
            on = 1;
            ASSERT(fabsf(fx[i].x - u.x) < 0.5f && fabsf(fx[i].z - u.z) < 0.5f);
            ASSERT(fabsf(fx[i].y - u.y) < 0.5f);
            ASSERT(fx[i].age < fx[i].frame_count * fx[i].ticks_per_frame);
            if (fabsf(u.x - x0) + fabsf(u.z - z0) > 8.0f) moved = 1;
            rode++;
        }
        if (!on) ended = 1;
    }
    printf("(%d reads, moved %d) ", rode, moved);
    ASSERT(moved);
    ASSERT(ended);
}

TEST(a_beam_leaves_from_its_firing_piece) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int h = place_named("ZONSHAM", NULL);
    ASSERT(h >= 0);
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    ASSERT_EQ_INT(0, okx_command(20, h, (int)u.x + 200, (int)u.z, -1, -1, 0));
    static OkxProjectile ps[256];
    static float m[12 * 64];
    static OkxNode nodes[64];
    int found = 0;
    for (int t = 0; t < 900 && !found; t += 2) {
        okx_tick(2);
        int k = okx_projectiles(ps, 256);
        for (int i = 0; i < k && i < 256 && !found; i++) {
            if (ps[i].kind != OKX_PROJ_BEAM) continue;
            ASSERT_EQ_INT(1, ps[i].from_piece);
            ASSERT_EQ_INT(0, okx_unit(h, &u));
            /* The source is a piece of the shaman's pose, well clear of
             * the ground the old source sat on. */
            int nn = okx_unit_pose(h, m, NULL, 64);
            ASSERT(nn > 0 && okx_model_nodes(u.model, nodes, 64) == nn);
            float best = 1e9f;
            int at = -1;
            for (int q = 0; q < nn && q < 64; q++) {
                float dx = m[12 * q + 3] - ps[i].from_x, dy = m[12 * q + 7] - ps[i].from_y;
                float dz = m[12 * q + 11] - ps[i].from_z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) { best = d2; at = q; }
            }
            printf("(%s, %.1f px up) ", at >= 0 ? nodes[at].name : "?",
                   ps[i].from_y - okx_ground_height(ps[i].from_x, ps[i].from_z));
            ASSERT(best < 4.0f);
            ASSERT(ps[i].from_y - okx_ground_height(ps[i].from_x, ps[i].from_z) > 20.0f);
            found = 1;
        }
    }
    ASSERT(found);
}

TEST(a_defs_effect_strips_are_its_weapons_and_blasts) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int def = -1;
    int h = place_named("VERMAGE", &def);
    ASSERT(def >= 0 && h >= 0);
    /* waterball, watersplash, waterballexplode, tsunamiexplode (three
     * radius arts, one strip) and nimbus_veruna. */
    int32_t strips[32];
    int n = okx_def_effect_strips(def, strips, 32);
    ASSERT_EQ_INT(5, n);
    ASSERT_EQ_INT(5, okx_def_effect_strips(def, NULL, 0));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) ASSERT(strips[i] != strips[j]);
        int w = 0, hh = 0;
        ASSERT(okx_effect_strip(strips[i], NULL, 0, &w, &hh) > 0);
    }
    ASSERT_EQ_INT(-1, okx_def_effect_strips(okx_def_count(), strips, 32));
    ASSERT_EQ_INT(-1, okx_def_effect_strips(-1, strips, 32));
    ASSERT_EQ_INT(-1, okx_def_effect_strips(def, strips, -1));
    /* A short buffer takes what fits and the count still says all. */
    int32_t few[4] = { -7, -7, -7, -7 };
    ASSERT_EQ_INT(5, okx_def_effect_strips(def, few, 2));
    ASSERT_EQ_INT(strips[0], few[0]);
    ASSERT_EQ_INT(strips[1], few[1]);
    ASSERT_EQ_INT(-7, few[2]);
    ASSERT_EQ_INT(-7, few[3]);
    /* What the mage shows in battle is on the list: its nimbus and the
     * blasts where its shots land. */
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    ASSERT_EQ_INT(0, okx_command(20, h, (int)u.x + 200, (int)u.z, -1, -1, 0));
    static OkxEffect fx[256];
    int nimbus = 0, blast = 0;
    for (int t = 0; t < 900 && !(nimbus && blast); t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 256);
        for (int i = 0; i < k && i < 256; i++) {
            int listed = 0;
            for (int j = 0; j < n; j++) listed |= strips[j] == fx[i].sprite;
            if (fx[i].kind == OKX_EFFECT_NIMBUS && fx[i].follow == h) { ASSERT(listed); nimbus = 1; }
            if (fx[i].kind == OKX_EFFECT_IMPACT && listed) blast = 1;
        }
    }
    ASSERT(nimbus);
    ASSERT(blast);
}

/* ── nimbuses and beams at the edges ───────────────────────────────── */

static void centre_of(const int *h, int n, float *cx, float *cz) {
    float x = 0.0f, z = 0.0f;
    int k = 0;
    for (int i = 0; i < n; i++) {
        OkxUnit u;
        if (okx_unit(h[i], &u) != 0) continue;
        x += u.x;
        z += u.z;
        k++;
    }
    *cx = k ? x / (float)k : 0.0f;
    *cz = k ? z / (float)k : 0.0f;
}

/* Unit h fires at ground `reach` px out from (cx, cz) through itself, so
 * a crowd shoots outward, not at itself. Kept on the map. */
static void cast_away(int h, float cx, float cz, float reach) {
    OkxUnit u;
    OkxTerrainInfo t;
    if (okx_unit(h, &u) != 0 || okx_terrain_info(&t) != 0) return;
    float dx = u.x - cx, dz = u.z - cz, l = sqrtf(dx * dx + dz * dz);
    if (l < 1.0f) { dx = 1.0f; dz = 0.0f; l = 1.0f; }
    float x = u.x + dx / l * reach, z = u.z + dz / l * reach;
    if (x < 48.0f) x = 48.0f;
    if (z < 48.0f) z = 48.0f;
    if (x > (float)t.map_w - 48.0f) x = (float)t.map_w - 48.0f;
    if (z > (float)t.map_h - 48.0f) z = (float)t.map_h - 48.0f;
    okx_command(TAK_CMD_ATTACK_GROUND, h, (int)x, (int)z, -1, -1, 0);
}

static void stop_all(const int *h, int n) {
    for (int i = 0; i < n; i++) okx_command(TAK_CMD_STOP_ORDER, h[i], 0, 0, -1, -1, 0);
}

static const OkxUnit *find_listed(const OkxUnit *us, int nu, int h) {
    for (int i = 0; i < nu; i++) if (us[i].handle == h) return &us[i];
    return NULL;
}

static const OkxEffect *glow_on(const OkxEffect *fx, int k, int h) {
    for (int i = 0; i < k; i++)
        if (fx[i].kind == OKX_EFFECT_NIMBUS && fx[i].follow == h) return &fx[i];
    return NULL;
}

static int enemy_seat(void) {
    OkxPlayer p[8];
    int n = okx_players(p, 8);
    for (int i = 0; i < n && i < 8; i++)
        if (p[i].index != okx_local_player()) return p[i].index;
    return -1;
}

/* 1 when every nimbus this frame rides a unit the frame lists, where
 * that unit stands: none on a dead caster, a hidden one or a stranger. */
static int nimbuses_ride_listed_units(void) {
    static OkxUnit us[1024];
    static OkxEffect fx[512];
    int nu = okx_units(us, 1024), k = okx_effects(fx, 512);
    if (nu > 1024) nu = 1024;
    if (k > 512) k = 512;
    for (int i = 0; i < k; i++) {
        if (fx[i].kind != OKX_EFFECT_NIMBUS) continue;
        const OkxUnit *u = find_listed(us, nu, fx[i].follow);
        if (!u || fabsf(fx[i].x - u->x) > 0.5f || fabsf(fx[i].z - u->z) > 0.5f ||
            fabsf(fx[i].y - u->y) > 0.5f) {
            printf("(nimbus %d on unit %d) ", fx[i].id, fx[i].follow);
            return 0;
        }
    }
    return 1;
}

TEST(a_beam_from_a_save_leaves_12_px_over_the_ground) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int h = place_named("ZONSHAM", NULL);
    ASSERT(h >= 0);
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_ATTACK_GROUND, h, (int)u.x + 200, (int)u.z, -1, -1, 0));
    static OkxProjectile ps[256];
    const char *path = "test_embed_beam.tsv";
    int saved = 0;
    for (int t = 0; t < 900 && !saved; t++) {
        okx_tick(1);
        int k = okx_projectiles(ps, 256);
        for (int i = 0; i < k && i < 256 && !saved; i++) {
            if (ps[i].kind != OKX_PROJ_BEAM) continue;
            ASSERT_EQ_INT(1, ps[i].from_piece);
            ASSERT_EQ_INT(0, okx_save(path));
            saved = 1;
        }
    }
    ASSERT(saved);
    /* The save keeps the beam but not the piece it left from. */
    ASSERT_EQ_INT(0, okx_load_save_begin(path));
    float progress = 0.0f;
    int steps = 0;
    while ((rc = okx_load_step(50, &progress, NULL, 0)) == 0 && steps < 10000) steps++;
    ASSERT_EQ_INT(1, rc);
    remove(path);
    int k = okx_projectiles(ps, 256), beams = 0;
    for (int i = 0; i < k && i < 256; i++) {
        if (ps[i].kind != OKX_PROJ_BEAM) continue;
        beams++;
        ASSERT_EQ_INT(0, ps[i].from_piece);
        float ground = okx_ground_height(ps[i].from_x, ps[i].from_z);
        ASSERT(fabsf(ps[i].from_y - (ground + 12.0f)) < 0.5f);
    }
    ASSERT(beams > 0);
    stop_all(&h, 1);
}

/* A builder over its own frame shows the hammer, and the game's click on
 * the frame sets it to work there, as the manual's left click does. Only
 * the frame's own player can help it (legacy:233556-233574). */
TEST(the_hammer_over_a_frame_sends_the_builder_to_help) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player();
    int builder = -1, theirs = -1, product = -1;
    for (int i = 0; i < n; i++) {
        if (units[i].state != OKX_UNIT_ACTIVE) continue;
        if (units[i].player != me) {
            if (theirs < 0) theirs = i;
            continue;
        }
        static int32_t opts[256];
        int k = product < 0 ? okx_def_buildables(units[i].def, opts, 256) : 0;
        for (int j = 0; j < k && product < 0; j++) {
            OkxDefInfo d;
            if (okx_def_info(opts[j], &d) == 0 && d.is_building) {
                product = opts[j];
                builder = i;
            }
        }
    }
    ASSERT(builder >= 0 && theirs >= 0 && product >= 0);
    const OkxUnit *b = &units[builder];
    int32_t sx = 0, sy = 0, found = 0;
    for (int r = 96; r <= 800 && !found; r += 32)
        for (int a = 0; a < 8 && !found; a++) {
            int32_t x = (int32_t)b->x + (a % 3 - 1) * r, y = (int32_t)b->z + (a / 3 - 1) * r;
            if (okx_build_site_facing(product, 0, x, y, &sx, &sy)) found = 1;
        }
    ASSERT(found);
    ASSERT_EQ_INT(0, okx_command(3, b->handle, sx, sy, -1, product, 0));
    okx_tick(5);
    OkxOrder o;
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &o));
    int32_t frame = o.building;
    ASSERT(frame >= 0);
    /* Stopped, the builder leaves its frame standing. */
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_STOP_ORDER, b->handle, 0, 0, -1, -1, 0));
    okx_tick(2);
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &o));
    ASSERT(o.building != frame);
    OkxUnit fr;
    ASSERT_EQ_INT(0, okx_unit(frame, &fr));
    ASSERT_EQ_INT(1, okx_can_help(b->handle, frame));
    ASSERT_EQ_INT(0, okx_can_help(units[theirs].handle, frame));

    okx_cancel();
    okx_cancel();
    ASSERT(okx_cursor_at(fr.x, fr.z, frame, NULL) != OKX_CURSOR_REPAIR);
    okx_select(&b->handle, 1, 0);
    ASSERT_EQ_INT(OKX_CURSOR_REPAIR, okx_cursor_at(fr.x, fr.z, frame, NULL));
    okx_click(fr.x, fr.z, frame, 0);
    okx_tick(2);
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &o));
    ASSERT_EQ_INT(OKX_ORDER_BUILD, o.kind);
    ASSERT_EQ_INT(frame, o.building);
    okx_cancel();
    okx_cancel();
}

/* A host raises a frame from the ground as it is built, so the list
 * carries it from the start, where the classic view draws nothing of it
 * until it is half built. */
TEST(a_frame_is_listed_from_the_start_of_its_build) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player();
    int builder = -1, product = -1;
    for (int i = 0; i < n && product < 0; i++) {
        if (units[i].state != OKX_UNIT_ACTIVE || units[i].player != me) continue;
        static int32_t opts[256];
        int k = okx_def_buildables(units[i].def, opts, 256);
        for (int j = 0; j < k && product < 0; j++) {
            OkxDefInfo d;
            if (okx_def_info(opts[j], &d) == 0 && d.is_building) {
                product = opts[j];
                builder = i;
            }
        }
    }
    ASSERT(builder >= 0 && product >= 0);
    const OkxUnit *b = &units[builder];
    int32_t sx = 0, sy = 0, found = 0;
    for (int r = 96; r <= 800 && !found; r += 32)
        for (int a = 0; a < 8 && !found; a++) {
            int32_t x = (int32_t)b->x + (a % 3 - 1) * r, y = (int32_t)b->z + (a / 3 - 1) * r;
            if (okx_build_site_facing(product, 0, x, y, &sx, &sy)) found = 1;
        }
    ASSERT(found);
    ASSERT_EQ_INT(0, okx_command(3, b->handle, sx, sy, -1, product, 0));
    okx_tick(5);
    OkxOrder o;
    ASSERT_EQ_INT(0, okx_unit_order(b->handle, &o));
    int32_t frame = o.building;
    ASSERT(frame >= 0);
    OkxUnit fr;
    ASSERT_EQ_INT(0, okx_unit(frame, &fr));
    ASSERT(fr.health * 2 < fr.max_health);
    n = okx_units(units, 512);
    int listed = 0;
    for (int i = 0; i < n; i++)
        if (units[i].handle == frame) {
            listed = 1;
            ASSERT_EQ_INT(1, units[i].building);
        }
    ASSERT_EQ_INT(1, listed);
}

/* Every flyer up at its height, as the host picks it where it draws it:
 * the cursor over each is the flyer's, the select hand while it is
 * yours, and once it is the computer's the attack cursor with your
 * monarch selected, whose click attacks it. The game used to look again
 * at the ground under the flyer and find nothing there
 * (legacy:237815-237922 lifts the pick by the unit's own height). */
TEST(the_cursor_over_a_flyer_in_the_air_is_the_flyers) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    static OkxUnit units[512];
    int n = okx_units(units, 512), me = okx_local_player(), mine = -1, them = -1;
    for (int i = 0; i < n; i++) {
        if (units[i].state != OKX_UNIT_ACTIVE) continue;
        OkxDefInfo di;
        if (units[i].player == me && mine < 0 && okx_def_info(units[i].def, &di) == 0 &&
            !di.is_building && !di.can_fly) mine = units[i].handle;
        if (units[i].player != me && them < 0) them = units[i].player;
    }
    ASSERT(mine >= 0 && them > 0);
    enum { MAXF = 32 };
    int fl[MAXF], nf = 0;
    for (int d = 0; d < okx_def_count() && nf < MAXF; d++) {
        OkxDefInfo di;
        if (okx_def_info(d, &di) != 0 || !di.can_fly) continue;
        int h = okx_place_unit(d, me);
        if (h >= 0) fl[nf++] = h;
    }
    ASSERT(nf > 0);
    float cx, cz;
    centre_of(fl, nf, &cx, &cz);
    for (int i = 0; i < nf; i++) cast_away(fl[i], cx, cz, 1500.0f);
    okx_tick(150);
    okx_cancel();
    okx_cancel();
    int up[MAXF], nu = 0;
    for (int i = 0; i < nf; i++) {
        OkxUnit u;
        if (okx_unit(fl[i], &u) != 0 || u.y - okx_ground_height(u.x, u.z) < 40.0f) continue;
        up[nu++] = fl[i];
        ASSERT_EQ_INT(OKX_CURSOR_SELECT, okx_cursor_at(u.x, u.z, fl[i], NULL));
    }
    printf("(%d flyers, %d up) ", nf, nu);
    ASSERT(nu > 0);

    for (int i = 0; i < nu; i++)
        ASSERT_EQ_INT(0, okx_command(TAK_CMD_GIVE_UNITS, up[i], 0, 0, -1, -1, them));
    okx_tick(1);
    okx_select(&mine, 1, 0);
    for (int i = 0; i < nu; i++) {
        OkxUnit u;
        ASSERT_EQ_INT(0, okx_unit(up[i], &u));
        ASSERT_EQ_INT(them, u.player);
        ASSERT_EQ_INT(OKX_CURSOR_ATTACK, okx_cursor_at(u.x, u.z, up[i], NULL));
    }
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(up[0], &u));
    okx_click(u.x, u.z, up[0], 0);
    okx_tick(1);
    OkxOrder o;
    ASSERT_EQ_INT(0, okx_unit_order(mine, &o));
    ASSERT_EQ_INT(OKX_ORDER_ATTACK, o.kind);
    ASSERT_EQ_INT(up[0], o.target);
    okx_command(TAK_CMD_STOP_ORDER, mine, 0, 0, -1, -1, 0);
    okx_cancel();
    okx_cancel();
}

/* Sent over the open water's middle on Two Castles an Aramon dragon
 * circles on, and okx_unit holds it its cruise height over the sea,
 * never over the sea floor, and never down in the water
 * (legacy:220103-220109, legacy:190499-190507). */
TEST(a_flyer_over_open_sea_stays_up_over_the_water) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    OkxTerrainInfo t;
    ASSERT_EQ_INT(0, okx_terrain_info(&t));
    ASSERT(t.water_height > 0);
    int drag = -1;
    for (int d = 0; d < okx_def_count() && drag < 0; d++) {
        OkxDefInfo di;
        if (okx_def_info(d, &di) == 0 && strcmp(di.name, "ARADRAG") == 0) drag = d;
    }
    ASSERT(drag >= 0);
    /* The open water's middle: the height sample farthest from dry
     * ground and the map's edge, by steps in any of eight directions. */
    int cw = t.heights_w, ch = t.heights_h, px = t.tile_px;
    int *dist = (int *)malloc(sizeof(int) * (size_t)cw * (size_t)ch);
    ASSERT(dist != NULL);
    for (int z = 0; z < ch; z++)
        for (int x = 0; x < cw; x++) {
            int e = x < z ? x : z;
            if (cw - 1 - x < e) e = cw - 1 - x;
            if (ch - 1 - z < e) e = ch - 1 - z;
            dist[z * cw + x] = okx_ground_height((float)(x * px), (float)(z * px)) >=
                               (float)t.water_height ? 0 : e;
        }
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < cw * ch; k++) {
            int i = pass ? cw * ch - 1 - k : k, x = i % cw, z = i / cw;
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= cw || nz >= ch) continue;
                    if (dist[nz * cw + nx] + 1 < dist[i]) dist[i] = dist[nz * cw + nx] + 1;
                }
        }
    int best = 0, at = 0;
    for (int i = 0; i < cw * ch; i++)
        if (dist[i] > best) { best = dist[i]; at = i; }
    free(dist);
    int mx = (at % cw) * px, mz = (at / cw) * px;
    ASSERT(best * px >= 288);
    int h = okx_place_unit(drag, okx_local_player());
    ASSERT(h >= 0);
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    /* A move ends 80 to 144 px short of its point (legacy:25066-25110). */
    float vx = (float)mx - u.x, vz = (float)mz - u.z;
    float vl = sqrtf(vx * vx + vz * vz);
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_MOVE_ORDER, h, mx + (int)(vx / vl * 112.0f),
                                 mz + (int)(vz / vl * 112.0f), -1, -1, 0));
    int over = 0;
    for (int i = 0; i < 6000 && !over; i += 10) {
        okx_tick(10);
        ASSERT_EQ_INT(0, okx_unit(h, &u));
        float dx = u.x - (float)mx, dz = u.z - (float)mz;
        over = dx * dx + dz * dz < 200.0f * 200.0f;
    }
    ASSERT(over);
    float lowest = 1e9f, lowest_over_sea = 1e9f;
    for (int i = 0; i < 1200; i++) {
        okx_tick(1);
        ASSERT_EQ_INT(0, okx_unit(h, &u));
        if (u.y < lowest) lowest = u.y;
        if (okx_ground_height(u.x, u.z) < (float)t.water_height && u.y < lowest_over_sea)
            lowest_over_sea = u.y;
    }
    printf("(sea at %d,%d, level %d, dry ground %d px off, lowest %.0f, over the "
           "sea %.0f) ", mx, mz, t.water_height, best * px, (double)lowest,
           (double)lowest_over_sea);
    ASSERT(lowest >= (float)t.water_height);
    ASSERT(lowest_over_sea >= (float)(t.water_height + 150));
    okx_command(TAK_CMD_STOP_ORDER, h, 0, 0, -1, -1, 0);
}

TEST(a_flyers_nimbus_rides_at_its_height) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    enum { MAXF = 32 };
    int fl[MAXF], nf = 0;
    for (int d = 0; d < okx_def_count() && nf < MAXF; d++) {
        OkxDefInfo di;
        if (okx_def_info(d, &di) != 0 || !di.can_fly) continue;
        int h = okx_place_unit(d, okx_local_player());
        if (h >= 0) fl[nf++] = h;
    }
    ASSERT(nf > 0);
    float cx, cz;
    centre_of(fl, nf, &cx, &cz);
    for (int i = 0; i < nf; i++) cast_away(fl[i], cx, cz, 300.0f);
    static OkxEffect fx[512];
    int reads = 0, aloft = 0;
    for (int t = 0; t < 1200; t += 2) {
        okx_tick(2);
        ASSERT(nimbuses_ride_listed_units());
        int k = okx_effects(fx, 512);
        for (int j = 0; j < nf; j++) {
            const OkxEffect *e = glow_on(fx, k < 512 ? k : 512, fl[j]);
            OkxUnit u;
            if (!e || okx_unit(fl[j], &u) != 0) continue;
            ASSERT(fabsf(e->y - u.y) < 0.5f);
            reads++;
            if (u.y - okx_ground_height(u.x, u.z) > 8.0f) aloft++;
        }
    }
    printf("(%d flyers, %d reads, %d aloft) ", nf, reads, aloft);
    stop_all(fl, nf);
    /* Flyers do cast nimbus weapons, so a run that read none checked nothing. */
    ASSERT(reads > 0);
    ASSERT(aloft > 0);
}

TEST(a_new_caster_never_cuts_a_live_nimbus_short) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    enum { OLD = 64, NEW = 16, ALL = OLD + NEW };
    static int h[ALL];
    for (int i = 0; i < ALL; i++) {
        h[i] = place_named("VERMAGE", NULL);
        ASSERT(h[i] >= 0);
    }
    float cx, cz;
    centre_of(h, ALL, &cx, &cz);
    /* 64 mages fill the nimbus table, then every glow runs out. Each
     * aims inside its 400 px range, so none has to push through the crowd. */
    for (int i = 0; i < OLD; i++) cast_away(h[i], cx, cz, 300.0f);
    static OkxEffect fx[512];
    static int seen[ALL];
    int owner[NEW];
    for (int s = 0; s < NEW; s++) owner[s] = -1;
    int lit = 0;
    for (int t = 0; t < 1800 && lit < OLD; t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 512);
        for (int i = 0; i < k && i < 512; i++) {
            if (fx[i].kind != OKX_EFFECT_NIMBUS) continue;
            if (fx[i].id >= 0 && fx[i].id < NEW) owner[fx[i].id] = fx[i].follow;
            for (int j = 0; j < OLD; j++)
                if (h[j] == fx[i].follow && !seen[j]) { seen[j] = 1; lit++; }
        }
    }
    ASSERT_EQ_INT(OLD, lit);
    stop_all(h, OLD);
    int on = 1;
    for (int t = 0; t < 900 && on; t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 512);
        on = 0;
        for (int i = 0; i < k && i < 512; i++) on |= fx[i].kind == OKX_EFFECT_NIMBUS;
    }
    ASSERT(!on);
    /* The casters holding the first sixteen entries cast again, then
     * sixteen mages new to the table cast among them. With 63 dead
     * entries to take, no glow may end before its pictures run out. */
    for (int s = 0; s < NEW; s++) if (owner[s] >= 0) cast_away(owner[s], cx, cz, 300.0f);
    okx_tick(60);
    for (int i = OLD; i < ALL; i++) cast_away(h[i], cx, cz, 300.0f);
    int prev_age[ALL], prev_end[ALL], age[ALL], end[ALL];
    for (int j = 0; j < ALL; j++) { prev_age[j] = -1; prev_end[j] = 0; }
    memset(seen, 0, sizeof(seen));
    int cut = 0, fresh = 0, most = 0;
    for (int t = 0; t < 1200; t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 512), live = 0;
        for (int j = 0; j < ALL; j++) { age[j] = -1; end[j] = 0; }
        for (int i = 0; i < k && i < 512; i++) {
            if (fx[i].kind != OKX_EFFECT_NIMBUS) continue;
            live++;
            for (int j = 0; j < ALL; j++) {
                if (h[j] != fx[i].follow) continue;
                age[j] = fx[i].age;
                end[j] = fx[i].frame_count * fx[i].ticks_per_frame;
            }
        }
        if (live > most) most = live;
        for (int j = 0; j < ALL; j++) {
            OkxUnit u;
            if (prev_age[j] >= 0 && prev_age[j] + 2 < prev_end[j] && age[j] < 0 &&
                okx_unit(h[j], &u) == 0)
                cut++;
            if (j >= OLD && age[j] >= 0 && !seen[j]) { seen[j] = 1; fresh++; }
            prev_age[j] = age[j];
            prev_end[j] = end[j];
        }
    }
    printf("(%d new lit, %d at once at most, %d cut short) ", fresh, most, cut);
    stop_all(h, ALL);
    ASSERT(fresh > 0);
    ASSERT(most < 64);
    ASSERT_EQ_INT(0, cut);
}

TEST(a_nimbus_ends_with_its_caster) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int enemy = enemy_seat();
    ASSERT(enemy > 0);
    enum { N = 12 };
    int all[N + 1];
    for (int i = 0; i <= N; i++) {
        all[i] = place_named("VERMAGE", NULL);
        ASSERT(all[i] >= 0);
    }
    int c = all[N];
    float cx, cz;
    centre_of(all, N + 1, &cx, &cz);
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(c, &u));
    uint32_t sid = u.stable_id;
    cast_away(c, cx, cz, 300.0f);
    static OkxEffect fx[512];
    int lit = 0;
    for (int t = 0; t < 900 && !lit; t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 512);
        lit = glow_on(fx, k < 512 ? k : 512, c) != NULL;
    }
    ASSERT(lit);
    /* Handed to the other seat, the caster is fair game for the rest. */
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_GIVE_UNITS, c, 0, 0, -1, -1, enemy));
    okx_tick(2);
    ASSERT_EQ_INT(0, okx_unit(c, &u));
    ASSERT_EQ_INT(enemy, u.player);
    for (int i = 0; i < N; i++)
        okx_command(TAK_CMD_ATTACK_ORDER, all[i], (int)u.x, (int)u.z, c, -1, 0);
    int gone = 0, glowing_at_death = 0, was_glowing = 0;
    for (int t = 0; t < 3600 && gone < 120; t += 2) {
        okx_tick(2);
        ASSERT(nimbuses_ride_listed_units());
        int k = okx_effects(fx, 512);
        int now = glow_on(fx, k < 512 ? k : 512, c) != NULL;
        int alive = okx_unit(c, &u) == 0 && u.stable_id == sid;
        if (!alive) {
            if (!gone && was_glowing) glowing_at_death = 1;
            gone += 2;
        }
        was_glowing = now;
    }
    printf("(glowing as it went: %d) ", glowing_at_death);
    ASSERT(gone > 0);
    /* A unit set down now may take the dead caster's slot. The dead
     * one's glow is not its own. */
    int fresh = place_named("VERMAGE", NULL);
    ASSERT(fresh >= 0);
    for (int t = 0; t < 120; t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 512);
        ASSERT(glow_on(fx, k < 512 ? k : 512, fresh) == NULL);
    }
    stop_all(all, N);
}

TEST(a_nimbus_in_the_fog_is_not_shown) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_end_game();
    g_booted = 0;
    ASSERT_EQ_INT(0, start_battle(0));
    int enemy = enemy_seat();
    int c = place_named("VERMAGE", NULL);
    ASSERT(enemy > 0 && c >= 0);
    /* Out to the middle of the map, beyond what the rest of the army
     * sees, until it gets there or stops. */
    OkxTerrainInfo ti;
    ASSERT_EQ_INT(0, okx_terrain_info(&ti));
    int mx = ti.map_w / 2, mz = ti.map_h / 2;
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_MOVE_ORDER, c, mx, mz, -1, -1, 0));
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(c, &u));
    float lx = u.x, lz = u.z;
    int still = 0;
    for (int t = 0; t < 6000 && still < 120; t += 10) {
        okx_tick(10);
        ASSERT_EQ_INT(0, okx_unit(c, &u));
        still = (fabsf(u.x - lx) + fabsf(u.z - lz) < 1.0f) ? still + 10 : 0;
        lx = u.x;
        lz = u.z;
    }
    cast_away(c, u.x - 1.0f, u.z, 200.0f);
    static OkxEffect fx[512];
    const OkxEffect *e = NULL;
    for (int t = 0; t < 900 && !e; t += 2) {
        okx_tick(2);
        int k = okx_effects(fx, 512);
        e = glow_on(fx, k < 512 ? k : 512, c);
    }
    ASSERT(e != NULL);
    int age = e->age, end = e->frame_count * e->ticks_per_frame;
    /* The other seat's now, and out of sight: its glow goes with it. */
    ASSERT_EQ_INT(0, okx_command(TAK_CMD_GIVE_UNITS, c, 0, 0, -1, -1, enemy));
    static OkxUnit us[1024];
    int hidden_lit = 0;
    for (int t = 0; t < 240; t += 2) {
        okx_tick(2);
        age += 2;
        ASSERT(nimbuses_ride_listed_units());
        int nu = okx_units(us, 1024);
        if (!find_listed(us, nu < 1024 ? nu : 1024, c) && age < end) hidden_lit = 1;
    }
    ASSERT_EQ_INT(0, okx_unit(c, &u));
    printf("(at %.0f,%.0f) ", u.x, u.z);
    ASSERT_EQ_INT(enemy, u.player);
    ASSERT(hidden_lit);
    okx_end_game();
    g_booted = 0;
}

TEST(the_game_ends_cleanly_and_can_start_again) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_end_game();
    g_booted = 0;
    int32_t strips[8];
    ASSERT_EQ_INT(-1, okx_def_effect_strips(0, strips, 8));
    ASSERT_EQ_INT(0, okx_units(NULL, 0));
    ASSERT_EQ_INT(0, boot());
    ASSERT(okx_units(NULL, 0) >= 2);
    okx_shutdown();
    g_booted = 0;
}

/* The last blast id so far, 0 for none. */
static int32_t last_blast(void) {
    static OkxBlast bl[OKX_RING];
    int n = okx_blasts(0, bl, OKX_RING);
    if (n > OKX_RING) n = OKX_RING;
    return n > 0 ? bl[n - 1].id : 0;
}

/* A catapult's rock thrown at the ground comes back as a blast with its
 * weapon, its place and its way, and the ring reads from an id and
 * starts again with the next battle. */
TEST(a_blast_names_its_weapon_and_where_it_burst) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int def = -1;
    for (int i = 0; i < okx_def_count() && def < 0; i++) {
        OkxDefInfo di;
        if (okx_def_info(i, &di) == 0 && same_name(di.name, "ARAPULT")) def = i;
    }
    ASSERT(def >= 0);
    OkxWeaponInfo wi;
    ASSERT_EQ_INT(0, okx_weapon_info(def, 0, &wi));
    printf("(%s: %s, %s, area %d, damage %d) ", wi.name, wi.type, wi.explosion_class,
           wi.area_of_effect, wi.damage);
    ASSERT(wi.area_of_effect > 0 && wi.damage > 0 && wi.type[0] && wi.explosion_class[0]);
    ASSERT_EQ_INT(-1, okx_weapon_info(def, 3, &wi));
    ASSERT_EQ_INT(-1, okx_weapon_info(-1, 0, &wi));
    ASSERT_EQ_INT(0, okx_weapon_info(def, 0, &wi));
    int h = okx_place_unit(def, okx_local_player());
    ASSERT(h >= 0);
    OkxUnit u;
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    int32_t since = last_blast();
    ASSERT_EQ_INT(0, okx_blasts(since, NULL, 0));
    int32_t ax = (int32_t)u.x + 320, az = (int32_t)u.z;
    ASSERT_EQ_INT(0, okx_command(20, h, ax, az, -1, -1, 0));
    static OkxBlast bl[OKX_RING];
    OkxBlast rock;
    memset(&rock, 0, sizeof rock);
    for (int t = 0; t < 1200 && !rock.id; t += 2) {
        okx_tick(2);
        int n = okx_blasts(since, bl, OKX_RING);
        for (int i = 0; i < n && i < OKX_RING && !rock.id; i++)
            if (bl[i].def == def && bl[i].shooter == h) rock = bl[i];
    }
    ASSERT(rock.id > since);
    printf("(at %.0f %.0f, way %.2f %.2f %.2f, radius %.0f) ", rock.x, rock.z, rock.dx, rock.dy, rock.dz, rock.radius);
    ASSERT_EQ_INT(OKX_BLAST_WEAPON, rock.cause);
    ASSERT_EQ_INT(0, rock.slot);
    ASSERT_EQ_INT(okx_local_player(), rock.player);
    ASSERT_EQ_INT(wi.damage, rock.damage);
    ASSERT(fabsf(rock.radius - wi.area_of_effect * 0.5f) < 0.01f);
    ASSERT(fabsf(rock.x - (float)ax) < 96.0f && fabsf(rock.z - (float)az) < 96.0f);
    ASSERT(rock.dx > 0.05f && rock.dy < 0.0f);
    ASSERT_EQ_INT(-1, rock.unit);
    ASSERT_EQ_INT(0, rock.flags & (OKX_BLAST_DIRECT_HIT | OKX_BLAST_UNSEEN));
    ASSERT(rock.tick > 0 && rock.tick <= okx_tick_count());
    /* From an id: what came after it, oldest first. */
    int n = okx_blasts(since, bl, OKX_RING);
    ASSERT(n >= 1);
    ASSERT_EQ_INT(since + 1, bl[0].id);
    for (int i = 1; i < n && i < OKX_RING; i++) ASSERT_EQ_INT(bl[i - 1].id + 1, bl[i].id);
    int32_t last = bl[(n < OKX_RING ? n : OKX_RING) - 1].id;
    ASSERT_EQ_INT(0, okx_blasts(last, bl, OKX_RING));
    /* A new battle starts the ring empty, and its ids carry on. */
    okx_end_game();
    g_booted = 0;
    ASSERT_EQ_INT(0, okx_blasts(0, NULL, 0));
    ASSERT_EQ_INT(0, start_battle(1));
    ASSERT_EQ_INT(0, okx_blasts(0, NULL, 0));
    h = okx_place_unit(def, okx_local_player());
    ASSERT(h >= 0);
    ASSERT_EQ_INT(0, okx_unit(h, &u));
    ASSERT_EQ_INT(0, okx_command(20, h, (int)u.x + 320, (int)u.z, -1, -1, 0));
    for (int t = 0; t < 1200 && okx_blasts(0, NULL, 0) == 0; t += 2) okx_tick(2);
    ASSERT(okx_blasts(0, bl, OKX_RING) > 0);
    ASSERT(bl[0].id > last);
}

static int def_by_name(const char *name) {
    for (int i = 0; i < okx_def_count(); i++) {
        OkxDefInfo di;
        if (okx_def_info(i, &di) == 0 && same_name(di.name, name)) return i;
    }
    return -1;
}

/* A unit with a death weapon that is struck down bursts it as its death
 * ends, and the host hears that blast as a death, of the unit's def and
 * from the death slot, which names the unit's [EXPLODEAS]. */
TEST(a_death_blast_says_it_is_a_death_and_names_its_weapon) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int rat = def_by_name("TARKAM"), pult = def_by_name("ARAPULT");
    ASSERT(rat >= 0 && pult >= 0);
    OkxWeaponInfo wi;
    ASSERT_EQ_INT(0, okx_weapon_info(rat, OKX_SLOT_DEATH, &wi));
    printf("(%s: area %d, damage %d, %s) ", wi.name, wi.area_of_effect, wi.damage,
           wi.explosion_class);
    ASSERT_EQ_INT(203, wi.area_of_effect);
    ASSERT_EQ_INT(8000, wi.damage);
    ASSERT_EQ_INT(-1, okx_weapon_info(pult, OKX_SLOT_DEATH, &wi));
    int me = okx_local_player();
    int r = okx_place_unit(rat, me), c = okx_place_unit(pult, me);
    ASSERT(r >= 0 && c >= 0);
    OkxUnit ru, cu;
    ASSERT_EQ_INT(0, okx_unit(c, &cu));
    /* The rat walks off, and the catapult's rock comes down on it. */
    ASSERT_EQ_INT(0, okx_command(1, r, (int)cu.x + 300, (int)cu.z, -1, -1, 0));
    for (int t = 0; t < 900; t += 4) okx_tick(4);
    ASSERT_EQ_INT(0, okx_unit(r, &ru));
    int32_t since = last_blast();
    ASSERT_EQ_INT(0, okx_command(20, c, (int)ru.x, (int)ru.z, -1, -1, 0));
    static OkxBlast bl[OKX_RING];
    OkxBlast death;
    memset(&death, 0, sizeof death);
    for (int t = 0; t < 2400 && !death.id; t += 2) {
        okx_tick(2);
        int n = okx_blasts(since, bl, OKX_RING);
        for (int i = 0; i < n && i < OKX_RING && !death.id; i++)
            if (bl[i].cause == OKX_BLAST_DEATH) death = bl[i];
    }
    printf("(burst at %.0f %.0f, radius %.0f) ", death.x, death.z, death.radius);
    ASSERT(death.id > since);
    ASSERT_EQ_INT(rat, death.def);
    ASSERT_EQ_INT(OKX_SLOT_DEATH, death.slot);
    ASSERT_EQ_INT(me, death.player);
    ASSERT_EQ_INT(8000, death.damage);
    ASSERT(fabsf(death.radius - 101.5f) < 0.01f);
    ASSERT_EQ_INT(-1, death.unit);
    ASSERT_EQ_INT(0, okx_weapon_info(death.def, death.slot, &wi));
    ASSERT_EQ_INT(203, wi.area_of_effect);
    /* The burst cleared the scenery by the start, so the next case
     * starts on a fresh battle. */
    okx_end_game();
    g_booted = 0;
}

/* The wind blows within the map's range, a fire starter says so, and a
 * feature def names the stages it leaves. */
TEST(the_field_tells_its_wind_fire_and_stages) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_tick(4);
    float speed = -1.0f, top = -1.0f, dx = 0.0f, dz = 0.0f;
    ASSERT_EQ_INT(0, okx_wind(&speed, &top, &dx, &dz));
    printf("(wind %.0f of %.0f toward %.2f %.2f) ", speed, top, dx, dz);
    ASSERT(top > 0.0f && speed >= 0.0f && speed <= top);
    ASSERT(fabsf(dx * dx + dz * dz - 1.0f) < 0.001f);
    OkxWeaponInfo wi;
    int necro = def_by_name("TARNECRO"), pult = def_by_name("ARAPULT");
    ASSERT(necro >= 0 && pult >= 0);
    ASSERT_EQ_INT(0, okx_weapon_info(necro, 0, &wi));
    ASSERT(wi.flags & OKX_WEAPON_FIRE_STARTER);
    ASSERT_EQ_INT(0, okx_weapon_info(pult, 0, &wi));
    ASSERT_EQ_INT(0, wi.flags & OKX_WEAPON_FIRE_STARTER);
    ASSERT(same_name(wi.water_explosion_class, "medium water explosion"));
    int n = okx_feature_def_count(), dies = 0, burns = 0, stands = 0;
    for (int i = 0; i < n; i++) {
        OkxFeatureFate f;
        ASSERT_EQ_INT(0, okx_feature_def_fate(i, &f));
        ASSERT(f.dead_def >= -1 && f.dead_def < n && f.burnt_def >= -1 && f.burnt_def < n);
        if (f.indestructible) stands++;
        else if (f.damage > 0 && f.dead_def >= 0) dies++;
        if (f.flammable && f.burnt_def >= 0) burns++;
    }
    printf("(%d die into a stage, %d burn into one, %d stand) ", dies, burns, stands);
    ASSERT(dies > 50 && burns > 50 && stands > 50);
    OkxFeatureFate none;
    ASSERT_EQ_INT(-1, okx_feature_def_fate(-1, &none));
    ASSERT_EQ_INT(-1, okx_feature_def_fate(n, &none));
}

/* An archer of our own under a catapult's ground shot dies, and its
 * script's EXPLODE throws pieces, each with its pose as it went. */
TEST(a_dying_archer_throws_its_pieces) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int pult_def = def_by_name("ARAPULT"), arch_def = def_by_name("ARAARCH");
    ASSERT(pult_def >= 0 && arch_def >= 0);
    int me = okx_local_player();
    int pult = okx_place_unit(pult_def, me), arch = okx_place_unit(arch_def, me);
    ASSERT(pult >= 0 && arch >= 0);
    OkxUnit p, a;
    ASSERT_EQ_INT(0, okx_unit(pult, &p));
    /* Out of the catapult's way, and of its least reach. */
    ASSERT_EQ_INT(0, okx_command(1, arch, (int)p.x + 320, (int)p.z, -1, -1, 0));
    okx_tick(60 * 8);
    ASSERT_EQ_INT(0, okx_unit(arch, &a));
    static OkxPieceEvent pe[OKX_RING];
    int32_t since = 0;
    int k = okx_piece_events(0, pe, OKX_RING);
    if (k > 0) since = pe[(k < OKX_RING ? k : OKX_RING) - 1].id;
    int thrown = 0;
    OkxPieceEvent first;
    memset(&first, 0, sizeof first);
    for (int shot = 0; shot < 6 && !thrown; shot++) {
        if (okx_unit(arch, &a) != 0) break;
        ASSERT_EQ_INT(0, okx_command(20, pult, (int)a.x, (int)a.z, -1, -1, 0));
        for (int t = 0; t < 60 * 10 && !thrown; t += 2) {
            okx_tick(2);
            int m = okx_piece_events(since, pe, OKX_RING);
            for (int i = 0; i < m && i < OKX_RING; i++)
                if (pe[i].unit == arch) { if (!thrown) first = pe[i]; thrown++; }
        }
    }
    /* Rows of four, x, y and z, the place last in each. */
    printf("(%d pieces, first node %d type %d at %.0f %.0f %.0f) ", thrown, first.piece, first.how,
           first.m[3], first.m[7], first.m[11]);
    ASSERT(thrown > 0);
    ASSERT_EQ_INT(arch_def, first.def);
    ASSERT_EQ_INT(me, first.player);
    ASSERT(first.model >= 0 && first.how != 0 && first.piece >= 0);
    ASSERT_EQ_INT(0, first.unseen);
    /* The piece stands where the archer fell. */
    ASSERT(fabsf(first.m[3] - a.x) < 64.0f && fabsf(first.m[11] - a.z) < 64.0f);
    ASSERT(first.tick > 0 && first.tick <= okx_tick_count());
}

/* A catapult's rock on the nearest scenery one rock destroys: the hit,
 * its death and the stage that takes its cell come out in order, each
 * naming the rock's blast. */
TEST(scenery_a_rock_destroys_tells_each_step) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int pult_def = def_by_name("ARAPULT");
    ASSERT(pult_def >= 0);
    OkxWeaponInfo wi;
    ASSERT_EQ_INT(0, okx_weapon_info(pult_def, 0, &wi));
    int pult = okx_place_unit(pult_def, okx_local_player());
    ASSERT(pult >= 0);
    OkxUnit p;
    ASSERT_EQ_INT(0, okx_unit(pult, &p));
    static OkxFeature fs[8192];
    int nf = okx_features(fs, 8192);
    int pick = -1;
    float best = 1e9f;
    for (int i = 0; i < nf && i < 8192; i++) {
        OkxFeatureFate f;
        if (okx_feature_def_fate(fs[i].def, &f) != 0) continue;
        if (f.indestructible || f.damage <= 0 || f.damage > wi.damage || f.dead_def < 0) continue;
        float d = sqrtf((fs[i].x - p.x) * (fs[i].x - p.x) + (fs[i].z - p.z) * (fs[i].z - p.z));
        if (d > 160.0f && d < best) { best = d; pick = i; }
    }
    ASSERT(pick >= 0);
    OkxFeature target = fs[pick];
    OkxFeatureFate fate;
    ASSERT_EQ_INT(0, okx_feature_def_fate(target.def, &fate));
    static OkxFeatureEvent fe[OKX_RING];
    int32_t since = 0;
    int k = okx_feature_events(0, fe, OKX_RING);
    if (k > 0) since = fe[(k < OKX_RING ? k : OKX_RING) - 1].id;
    ASSERT_EQ_INT(0, okx_command(20, pult, (int)target.x, (int)target.z, -1, -1, 0));
    OkxFeatureEvent hit, end;
    memset(&hit, 0, sizeof hit);
    memset(&end, 0, sizeof end);
    for (int t = 0; t < 60 * 30 && !end.id; t += 2) {
        okx_tick(2);
        int m = okx_feature_events(since, fe, OKX_RING);
        for (int i = 0; i < m && i < OKX_RING; i++) {
            since = fe[i].id;
            if (fe[i].feature != target.index) continue;
            if (fe[i].kind == OKX_FEATURE_HIT && !hit.id) hit = fe[i];
            if (fe[i].kind == OKX_FEATURE_DEAD) end = fe[i];
        }
    }
    printf("(%.0f px away, hit %d left %d, blast %d, dead into %d after %u ticks) ", best, hit.damage,
           hit.health, hit.blast, end.new_def, end.tick - hit.tick);
    ASSERT(hit.id > 0);
    ASSERT_EQ_INT(wi.damage, hit.damage);
    ASSERT_EQ_INT(0, hit.health);
    ASSERT(hit.blast > 0);
    static OkxBlast bl[OKX_RING];
    int nb = okx_blasts(hit.blast - 1, bl, 1);
    ASSERT(nb >= 1);
    ASSERT_EQ_INT(pult_def, bl[0].def);
    ASSERT(fabsf(hit.from_x - bl[0].x) < 0.5f && fabsf(hit.from_z - bl[0].z) < 0.5f);
    ASSERT(end.id > hit.id);
    ASSERT_EQ_INT(target.def, end.def);
    ASSERT_EQ_INT(fate.dead_def, end.new_def);
    ASSERT_EQ_INT(hit.blast, end.blast);
}

/* ── Where the engine keeps its options ─────────────────────────────── */

/* The desktop game keeps its options.cfg in TAK_CONFIG_DIR when that is
 * set, so a folder named there stands in for the player's own. */
#define GAME_PREFS   "test_embed_game_prefs"
#define USER_OPTIONS "test_embed_user/options.cfg"

static void set_config_env(const char *value) {
#ifdef _WIN32
    _putenv_s("TAK_CONFIG_DIR", value ? value : "");
#else
    if (value) setenv("TAK_CONFIG_DIR", value, 1);
    else unsetenv("TAK_CONFIG_DIR");
#endif
}

static void make_dir(const char *path) {
#ifdef _WIN32
    (void)_mkdir(path);
#else
    (void)mkdir(path, 0755);
#endif
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* The value a key=value file gives `key`, "" when it gives none. */
static const char *file_value(const char *path, const char *key, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return out;
    char line[256];
    size_t kn = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kn) != 0 || line[kn] != '=') continue;
        snprintf(out, cap, "%s", line + kn + 1);
        out[strcspn(out, "\r\n")] = 0;
    }
    fclose(f);
    return out;
}

#ifdef _WIN32
typedef SOCKET lsock_t;
#  define LSOCK_NONE INVALID_SOCKET
#  define lsock_close closesocket
#else
typedef int lsock_t;
#  define LSOCK_NONE (-1)
#  define lsock_close close
#endif

/* A socket on this machine that takes a connection and says nothing,
 * which is all a join needs to make its hello. The port, 0 for none. */
static int listen_here(lsock_t *out) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif
    lsock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == LSOCK_NONE) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &len) != 0) {
        lsock_close(s);
        return 0;
    }
    *out = s;
    return ntohs(a.sin_port);
}

static void listen_end(lsock_t s) {
    lsock_close(s);
#ifdef _WIN32
    WSACleanup();
#endif
}

/* Joins a server here and leaves, which makes the device token on the
 * first join and saves it. 0 when the join went out. */
static int join_and_leave(void) {
    lsock_t s;
    int port = listen_here(&s);
    if (!port) return -1;
    char url[64];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/play", port);
    int rc = okx_net_connect(url, "Tester");
    okx_net_disconnect();
    listen_end(s);
    return rc;
}

/* The engine saves a joining player's device token in the host's user
 * folder, keeping what that file held, reads it back the next time, and
 * never writes the desktop game's options.cfg. */
TEST(the_engine_keeps_its_options_in_the_hosts_folder) {
    okx_shutdown();
    g_booted = 0;
    make_dir("test_embed_user");
    FILE *f = fopen(USER_OPTIONS, "w");
    ASSERT(f != NULL);
    fputs("HostKept=7\n", f);
    fclose(f);
    make_dir(GAME_PREFS);
    remove(GAME_PREFS "/options.cfg");
    char was[1024];
    const char *env = getenv("TAK_CONFIG_DIR");
    snprintf(was, sizeof was, "%s", env ? env : "");
    set_config_env(GAME_PREFS);

    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        set_config_env(was);
        SKIP("no game data");
    }
    char token[64], again[64], kept[16];
    int first = join_and_leave();
    file_value(USER_OPTIONS, "DeviceToken", token, sizeof token);
    okx_shutdown();
    int second = okx_init(TAK_GAME_DIR, TAK_DATA_DIR) == 0 ? join_and_leave() : -2;
    okx_shutdown();
    set_config_env(was);

    ASSERT_EQ_INT(0, first);
    ASSERT_EQ_INT(0, second);
    ASSERT(!file_exists(GAME_PREFS "/options.cfg"));
    ASSERT_EQ_INT(32, (int)strlen(token));
    ASSERT_EQ_STR("7", file_value(USER_OPTIONS, "HostKept", kept, sizeof kept));
    /* Read back, not made again. */
    ASSERT_EQ_STR(token, file_value(USER_OPTIONS, "DeviceToken", again, sizeof again));
}

int main(void) {
    TEST_SUITE("ok_embed");
    /* Nothing in the run may reach the player's own options: ctest names
     * a folder for them, else this does. */
    if (!getenv("TAK_CONFIG_DIR") || !getenv("TAK_CONFIG_DIR")[0])
        set_config_env("test_embed_prefs");
    /* A user folder of the test's own, where the edited map is saved. */
    okx_set_user_dir("test_embed_user");
    RUN(the_maps_are_listed);
    /* First, while no read has loaded any model or art yet. */
    RUN(what_a_host_reads_never_changes_the_battle);
    RUN(the_menus_play_the_interface_music);
    RUN(seeing_all_shows_the_whole_field);
    RUN(a_lost_battle_plays_on_between_the_computers);
    RUN(a_skirmish_loads_with_terrain);
    RUN(units_stand_on_the_map_with_models_and_poses);
    RUN(a_marching_unit_moves_and_its_pieces_swing);
    RUN(features_come_as_models_or_sprites);
    RUN(the_lobby_and_the_hud_have_what_they_show);
    RUN(the_studio_plays_a_units_walk_outside_the_battle);
    RUN(the_studio_plays_a_flyers_fly);
    RUN(the_hud_can_place_queue_and_read_orders);
    RUN(the_view_follows_the_host_camera_and_audio_is_optional);
    RUN(a_battle_shows_its_shots_and_explosions);
    RUN(the_games_own_click_selects_and_orders);
    RUN(a_formation_walks_turns_and_queues);
    RUN(the_studio_places_a_unit_by_the_start_and_a_boat_floats);
    RUN(the_cursor_is_the_one_the_classic_view_shows);
    RUN(a_building_placed_turned_stands_turned);
    RUN(the_sidebar_orders_list_cast_and_toggle);
    RUN(the_fog_comes_as_the_classic_view_draws_it);
    RUN(an_override_model_replaces_the_shipped_one);
    RUN(a_load_comes_in_slices_with_progress);
    RUN(the_lobby_lineup_sets_the_seats);
    RUN(a_host_on_the_remastered_rules_starts_its_skirmishes_on_them);
    RUN(a_saved_battle_comes_back_as_it_was);
    RUN(an_edited_map_saves_and_plays);
    RUN(a_factory_queue_takes_counts_repeats_and_a_rally);
    RUN(a_walking_builder_summons_without_end);
    RUN(an_unfinished_factory_takes_a_queue_while_the_host_allows_it);
    RUN(shift_queues_orders_and_the_host_reads_them_back);
    RUN(the_interface_art_comes_by_sheet_and_entry);
    RUN(a_picture_comes_by_name_for_painting_a_model);
    RUN(a_shot_and_its_blast_carry_the_weapons_lightmap);
    RUN(a_blast_names_its_weapon_and_where_it_burst);
    RUN(a_death_blast_says_it_is_a_death_and_names_its_weapon);
    RUN(the_field_tells_its_wind_fire_and_stages);
    RUN(a_dying_archer_throws_its_pieces);
    RUN(scenery_a_rock_destroys_tells_each_step);
    RUN(a_nimbus_rides_its_caster);
    RUN(a_beam_leaves_from_its_firing_piece);
    RUN(a_defs_effect_strips_are_its_weapons_and_blasts);
    RUN(a_beam_from_a_save_leaves_12_px_over_the_ground);
    RUN(a_flyers_nimbus_rides_at_its_height);
    RUN(a_flyer_over_open_sea_stays_up_over_the_water);
    RUN(a_new_caster_never_cuts_a_live_nimbus_short);
    RUN(a_nimbus_ends_with_its_caster);
    RUN(a_nimbus_in_the_fog_is_not_shown);
    RUN(the_cursor_over_a_flyer_in_the_air_is_the_flyers);
    RUN(the_hammer_over_a_frame_sends_the_builder_to_help);
    RUN(a_frame_is_listed_from_the_start_of_its_build);
    RUN(a_decided_battle_hands_out_its_record);
    RUN(the_game_ends_cleanly_and_can_start_again);
    RUN(the_engine_keeps_its_options_in_the_hosts_folder);
    TEST_REPORT();
}
