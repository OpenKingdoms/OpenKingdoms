/*
 * test_embed.c -- the engine through ok_embed.h, the way a host uses it:
 * boot, load a real map, tick, and read back terrain, models, units,
 * poses and features. Needs the game data.
 */
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

/* 1 when there is no game data to run against. */
static int boot(void) {
    if (g_booted) return 0;
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
    /* A frame is not drawn until it is half raised, so read it by the
     * builder's order. */
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

int main(void) {
    TEST_SUITE("ok_embed");
    /* A user folder of the test's own, where the edited map is saved. */
    okx_set_user_dir("test_embed_user");
    RUN(the_maps_are_listed);
    /* First, while no read has loaded any model or art yet. */
    RUN(what_a_host_reads_never_changes_the_battle);
    RUN(the_menus_play_the_interface_music);
    RUN(seeing_all_shows_the_whole_field);
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
    RUN(a_saved_battle_comes_back_as_it_was);
    RUN(an_edited_map_saves_and_plays);
    RUN(a_factory_queue_takes_counts_repeats_and_a_rally);
    RUN(an_unfinished_factory_takes_a_queue_while_the_host_allows_it);
    RUN(shift_queues_orders_and_the_host_reads_them_back);
    RUN(the_interface_art_comes_by_sheet_and_entry);
    RUN(a_picture_comes_by_name_for_painting_a_model);
    RUN(a_shot_and_its_blast_carry_the_weapons_lightmap);
    RUN(a_nimbus_rides_its_caster);
    RUN(a_beam_leaves_from_its_firing_piece);
    RUN(a_defs_effect_strips_are_its_weapons_and_blasts);
    RUN(a_beam_from_a_save_leaves_12_px_over_the_ground);
    RUN(a_flyers_nimbus_rides_at_its_height);
    RUN(a_new_caster_never_cuts_a_live_nimbus_short);
    RUN(a_nimbus_ends_with_its_caster);
    RUN(a_nimbus_in_the_fog_is_not_shown);
    RUN(the_cursor_over_a_flyer_in_the_air_is_the_flyers);
    RUN(the_game_ends_cleanly_and_can_start_again);
    TEST_REPORT();
}
