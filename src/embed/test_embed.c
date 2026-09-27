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

static int g_booted;

static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* 1 when there is no game data to run against. */
static int boot(void) {
    if (g_booted) return 0;
    if (okx_init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        SKIP_MARK("no game data: %s", okx_last_error());
        return 1;
    }
    OkxSkirmish cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.map, sizeof(cfg.map), "%s", MAP_NAME);
    snprintf(cfg.kingdom, sizeof(cfg.kingdom), "aramon");
    cfg.ai_players = 1;
    cfg.map_revealed = 1;
    cfg.seed = 12345;
    if (okx_start_skirmish(&cfg) != 0) {
        printf("start failed: %s ", okx_last_error());
        return -1;
    }
    g_booted = 1;
    return 0;
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
    int w = 0, h = 0;
    int need = okx_map_preview(idx, NULL, 0, &w, &h);
    ASSERT(need > 0 && need == w * h * 4);
    uint8_t *px = (uint8_t *)malloc((size_t)need);
    ASSERT_EQ_INT(need, okx_map_preview(idx, px, need, &w, &h));
    int opaque = 0;
    for (int i = 0; i < need; i += 4) opaque += px[i + 3] != 0 && (px[i] | px[i + 1] | px[i + 2]) != 0;
    free(px);
    ASSERT(opaque > need / 8);

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

TEST(the_game_ends_cleanly_and_can_start_again) {
    int rc = boot();
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    okx_end_game();
    g_booted = 0;
    ASSERT_EQ_INT(0, okx_units(NULL, 0));
    ASSERT_EQ_INT(0, boot());
    ASSERT(okx_units(NULL, 0) >= 2);
    okx_shutdown();
    g_booted = 0;
}

int main(void) {
    TEST_SUITE("ok_embed");
    RUN(the_maps_are_listed);
    RUN(a_skirmish_loads_with_terrain);
    RUN(units_stand_on_the_map_with_models_and_poses);
    RUN(a_marching_unit_moves_and_its_pieces_swing);
    RUN(features_come_as_models_or_sprites);
    RUN(the_lobby_and_the_hud_have_what_they_show);
    RUN(the_studio_plays_a_units_walk_outside_the_battle);
    RUN(the_hud_can_place_queue_and_read_orders);
    RUN(an_override_model_replaces_the_shipped_one);
    RUN(the_game_ends_cleanly_and_can_start_again);
    TEST_REPORT();
}
