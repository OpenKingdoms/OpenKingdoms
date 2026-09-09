#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_world.h"

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

int main(void) {
    if (test_map_revealed_initializes_explored() != 0) return 1;
    if (test_los_disabled_is_visible() != 0) return 1;
    if (test_player_layers_are_independent() != 0) return 1;
    puts("test_fog: ok");
    return 0;
}
