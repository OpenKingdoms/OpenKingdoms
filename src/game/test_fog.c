#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static Unit g_test_units[4];
static int g_test_unit_count = 0;
static UnitDef g_test_defs[4];

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_test_unit_count;
    return g_test_units;
}

const UnitDef *Units_GetDef(int idx) {
    if (idx < 0 || idx >= 4) return NULL;
    return &g_test_defs[idx];
}

/* The reveal anchor. It is simulation state on the unit rather than a
 * cache, so fog.c asks the unit layer for it and this fixture owns the
 * unit array. Same rule as the real one: the anchor follows the unit
 * once it has moved 16 px from where the cells were worked out. */
void Units_FogAnchor(int handle, int sight, int32_t *out_x, int32_t *out_y) {
    if (handle < 0 || handle >= g_test_unit_count) return;
    Unit *u = &g_test_units[handle];
    if (!u->fog_lit || u->fog_sight != (int16_t)sight ||
        labs((long)(u->world_x - u->fog_x)) >= 16 ||
        labs((long)(u->world_y - u->fog_y)) >= 16) {
        u->fog_x = u->world_x;
        u->fog_y = u->world_y;
        u->fog_sight = (int16_t)sight;
        u->fog_lit = 1;
    }
    if (out_x) *out_x = u->fog_x;
    if (out_y) *out_y = u->fog_y;
}

int Terrain_SampleHeight(const GameWorld *world,
                         int32_t world_x, int32_t world_y) {
    (void)world;
    (void)world_x;
    (void)world_y;
    return 0;
}

#define ASSERT_EQ_INT(exp, got) do { \
    int _e = (exp); \
    int _g = (got); \
    if (_e != _g) { \
        fprintf(stderr, "ASSERT_EQ_INT failed at %s:%d: expected %d got %d\n", \
                __FILE__, __LINE__, _e, _g); \
        return 1; \
    } \
} while (0)

static int test_map_revealed_initializes_explored(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    w.map_pixels_w = 256;
    w.map_pixels_h = 192;
    w.cfg.line_of_sight = 1;
    w.cfg.map_revealed = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    int total = w.fog_w * w.fog_h;
    for (int i = 0; i < total; i++) {
        ASSERT_EQ_INT(TAK_FOG_EXPLORED, w.fog_state[i]);
    }
    Fog_Update(&w, 1);
    for (int i = 0; i < total; i++) {
        ASSERT_EQ_INT(TAK_FOG_EXPLORED, w.fog_state[i]);
    }
    Fog_Free(&w);
    return 0;
}

static int test_los_disabled_is_visible(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    w.map_pixels_w = 128;
    w.map_pixels_h = 128;
    w.cfg.line_of_sight = 0;
    w.cfg.map_revealed = 0;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    Fog_Update(&w, 1);
    int total = w.fog_w * w.fog_h;
    for (int i = 0; i < total; i++) {
        ASSERT_EQ_INT(TAK_FOG_VISIBLE, w.fog_state[i]);
    }
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAt(&w, 32, 32));
    Fog_Free(&w);
    return 0;
}

static int test_player_layers_are_independent(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    w.cfg.map_revealed = 0;
    ASSERT_EQ_INT(0, Fog_Init(&w));

    g_test_unit_count = 2;
    g_test_defs[0].sight_distance = 96;
    g_test_defs[1].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 64;
    g_test_units[0].world_y = 64;
    g_test_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[1].player_id = 2;
    g_test_units[1].def_idx = 1;
    g_test_units[1].world_x = 384;
    g_test_units[1].world_y = 384;

    Fog_Update(&w, 1);
    Fog_Update(&w, 2);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 64, 64));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 2, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 2, 64, 64));

    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* Two human seats on one team see through each other's units
 * (legacy:167261-167267). */
static int test_allies_share_sight(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    for (int p = 0; p < 3; p++) {
        w.cfg.players[p].kind = TAK_SLOT_HUMAN;
        w.cfg.players[p].team = (p < 2) ? 1 : 2;
    }
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 2;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 384;
    g_test_units[0].world_y = 384;

    ASSERT_EQ_INT(1, Fog_SharesSight(&w, 1, 2));
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 3));
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 384, 384));
    Fog_Update(&w, 3);
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 3, 384, 384));

    /* The ally leaves and the ground stays explored, not forgotten. */
    g_test_units[0].world_x = 64;
    g_test_units[0].world_y = 64;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 64, 64));

    /* On different teams nothing is shared. */
    w.cfg.players[1].team = 3;
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 2));
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* A computer seat shares with nobody, in either direction
 * (legacy:206336-206348). Teamless seats share with nobody either. */
static int test_an_ai_teammate_grants_no_sight(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    w.cfg.players[1].kind = TAK_SLOT_AI;
    w.cfg.players[1].team = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 2;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 384;
    g_test_units[0].world_y = 384;

    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 2));
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 2, 1));
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));
    /* The AI still sees through its own units. */
    Fog_Update(&w, 2);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 2, 384, 384));

    /* Two humans with no team are two sides. */
    w.cfg.players[1].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 0;
    w.cfg.players[1].team = 0;
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 2));
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

int main(void) {
    if (test_map_revealed_initializes_explored() != 0) return 1;
    if (test_los_disabled_is_visible() != 0) return 1;
    if (test_player_layers_are_independent() != 0) return 1;
    if (test_allies_share_sight() != 0) return 1;
    if (test_an_ai_teammate_grants_no_sight() != 0) return 1;
    puts("test_fog: ok");
    return 0;
}
