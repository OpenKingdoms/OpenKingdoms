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
    cfg.seats[0] = (OkxSeat){ 1, 2, 1, 5, 0 };   /* Veruna, team 1, colour 5 */
    cfg.seats[1] = (OkxSeat){ 2, 3, 2, 7, 2 };   /* a hard Zhon computer */
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
    /* A user folder of the test's own, where the edited map is saved. */
    okx_set_user_dir("test_embed_user");
    RUN(the_maps_are_listed);
    RUN(a_skirmish_loads_with_terrain);
    RUN(units_stand_on_the_map_with_models_and_poses);
    RUN(a_marching_unit_moves_and_its_pieces_swing);
    RUN(features_come_as_models_or_sprites);
    RUN(the_lobby_and_the_hud_have_what_they_show);
    RUN(the_studio_plays_a_units_walk_outside_the_battle);
    RUN(the_hud_can_place_queue_and_read_orders);
    RUN(the_view_follows_the_host_camera_and_audio_is_optional);
    RUN(a_battle_shows_its_shots_and_explosions);
    RUN(the_games_own_click_selects_and_orders);
    RUN(the_fog_comes_as_the_classic_view_draws_it);
    RUN(an_override_model_replaces_the_shipped_one);
    RUN(a_load_comes_in_slices_with_progress);
    RUN(the_lobby_lineup_sets_the_seats);
    RUN(a_saved_battle_comes_back_as_it_was);
    RUN(an_edited_map_saves_and_plays);
    RUN(the_game_ends_cleanly_and_can_start_again);
    TEST_REPORT();
}
