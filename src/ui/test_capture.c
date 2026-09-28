/*
 * test_capture.c -- the fixed fog capture's flags, its fog dump against
 * the classic overlay, and the scout walk, with no game data.
 */

#include "test_framework.h"
#include "tak_battle_config.h"
#include "tak_capture.h"
#include "tak_command_queue.h"
#include "tak_fog.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_net_client.h"
#include "tak_net_match.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define CT_TILES 96

static GameWorld *ct_world(int line_of_sight) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.line_of_sight = line_of_sight;
    cfg.players[0].kind = TAK_SLOT_HUMAN;
    cfg.players[1].kind = TAK_SLOT_HUMAN;
    if (World_BeginLoad(NULL, &cfg, "synthetic", "aramon") != 0) return NULL;
    GameWorld *w = World_Get();
    if (!w) return NULL;
    w->map_pixels_w = CT_TILES * 16;
    w->map_pixels_h = CT_TILES * 16;
    w->viewport_w = 640;
    w->viewport_h = 480;
    w->tnt.width_tiles = CT_TILES;
    w->tnt.height_tiles = CT_TILES;
    w->tnt.height_w = CT_TILES + 1;
    w->tnt.height_h = CT_TILES + 1;
    size_t hn = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)tak_malloc(hn);
    if (!w->tnt.heightmap) return NULL;
    memset(w->tnt.heightmap, 64, hn);
    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 1;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 1;
    w->moveinfo.classes[0].footprint_z = 1;
    w->moveinfo.classes[0].max_slope = 30;
    if (!Occ_Ensure(w)) return NULL;
    TAK_PathCacheReset();
    World_MarkLoaded();
    if (Fog_Init(w) != 0) return NULL;

    UnitDef king;
    memset(&king, 0, sizeof king);
    strncpy(king.unitname, "TESTKING", sizeof(king.unitname) - 1);
    strncpy(king.movement_class, "TESTSMALL", sizeof(king.movement_class) - 1);
    king.max_health = 500;
    king.sight_distance = 320;
    king.max_velocity = 1.4f;
    king.acceleration = 0.35f;
    king.brake_rate = 0.7f;
    king.turn_rate = 4000.0f;
    king.max_slope = 30;
    king.bmcode = 1;
    king.cap_flags = UNIT_CAP_MOVE | UNIT_CAP_STOP;
    king.footprint_x = 1;
    king.footprint_z = 1;
    king.commander = 1;
    if (Units_DebugSetDefs(&king, 1) != 1) return NULL;
    Units_SetLocalPlayer(1);
    TAK_CmdQueue_Reset(0);
    Capture_ScoutReset();
    return w;
}

static void ct_end(void) {
    Capture_ScoutReset();
    Units_ClearInstances();
    World_End(NULL);
    TAK_PathCacheReset();
    TAK_CmdQueue_Reset(0);
}

/* ── the flags ─────────────────────────────────────────────────────── */

static int ct_take(TAK_CaptureArgs *a, int argc, char **argv, char *why) {
    for (int i = 1; i < argc; i++) {
        int rc = Capture_TakeArg(a, argc, argv, &i, why, 160);
        if (rc < 0) return -1;
        if (rc == 0) return -2;
    }
    return 0;
}

TEST(the_capture_flags_take_good_values_and_refuse_bad_ones) {
    TAK_CaptureArgs a;
    char why[160] = "";
    char *good[] = { "x", "--map", "two castles", "--seed", "4242", "--los", "OFF",
                     "--scout", "--fog-dump", "fog.bin" };
    Capture_ArgsInit(&a);
    ASSERT_EQ_INT(0, ct_take(&a, 10, good, why));
    ASSERT_EQ_STR("two castles", a.map);
    ASSERT_EQ_INT(1, a.has_seed);
    ASSERT_EQ_INT(4242, (int)a.seed);
    ASSERT_EQ_INT(0, a.los);
    ASSERT_EQ_INT(1, a.scout);
    ASSERT_EQ_STR("fog.bin", a.fog_dump);
    ASSERT_EQ_INT(0, Capture_Check(&a, 1, why, sizeof why));
    /* Without --skirmish they mean nothing, and say so. */
    ASSERT_EQ_INT(-1, Capture_Check(&a, 0, why, sizeof why));

    static char *bad[][3] = {
        { "x", "--seed", "abc" }, { "x", "--seed", "-3" }, { "x", "--seed", "99999999999" },
        { "x", "--los", "maybe" }, { "x", "--los", "1" }, { "x", "--map", "--scout" },
    };
    for (int k = 0; k < 6; k++) {
        Capture_ArgsInit(&a);
        why[0] = 0;
        ASSERT_EQ_INT(-1, ct_take(&a, 3, bad[k], why));
        ASSERT(why[0] != 0);
    }
    char *missing[] = { "x", "--fog-dump" };
    Capture_ArgsInit(&a);
    ASSERT_EQ_INT(-1, ct_take(&a, 2, missing, why));
    char *other[] = { "x", "--view3d" };
    Capture_ArgsInit(&a);
    ASSERT_EQ_INT(-2, ct_take(&a, 2, other, why));
}

/* ── the fog dump ──────────────────────────────────────────────────── */

/* The dump is a byte a 16 pixel cell, row 0 north, and each byte says
 * what the classic overlay lays on that cell: 2 nothing, 1 the dimming,
 * 0 black. With line of sight off, ground seen before is laid bare. */
static int ct_fog_matches_overlay(int line_of_sight) {
    GameWorld *w = ct_world(line_of_sight);
    if (!w) return 0;
    uint8_t *layer = w->fog_layers[Fog_Viewer()];
    int n = w->fog_w * w->fog_h;
    for (int i = 0; i < n; i++) layer[i] = (uint8_t)(i % 3);
    const char *path = "test_capture_fog.bin";
    remove(path);
    int ok = Capture_WriteFog(w, path) == 0;
    int cw = w->map_pixels_w / 16, ch = w->map_pixels_h / 16;
    FILE *f = fopen(path, "rb");
    static uint8_t bytes[CT_TILES * CT_TILES + 1];
    size_t got = f ? fread(bytes, 1, sizeof bytes, f) : 0;
    if (f) fclose(f);
    remove(path);
    ok = ok && got == (size_t)(cw * ch);
    int seen[3] = { 0, 0, 0 };
    for (int cy = 0; ok && cy < ch; cy++)
        for (int cx = 0; ok && cx < cw; cx++) {
            int b = bytes[cy * cw + cx];
            uint8_t alpha = Fog_OverlayAlphaAt(w, cx * 16 + 8, cy * 16 + 8);
            int want = alpha == 0 ? 2 : alpha == 0xFF ? 0 : 1;
            if (b != want || b > 2) ok = 0;
            else seen[b]++;
        }
    if (line_of_sight) ok = ok && seen[0] && seen[1] && seen[2];
    else ok = ok && seen[0] && !seen[1] && seen[2];
    ct_end();
    return ok;
}

TEST(the_fog_dump_is_what_the_classic_overlay_draws) {
    ASSERT(ct_fog_matches_overlay(1));
    ASSERT(ct_fog_matches_overlay(0));
}

/* ── the scout ─────────────────────────────────────────────────────── */

TEST(the_scout_walks_in_a_skirmish_and_never_in_a_match) {
    GameWorld *w = ct_world(1);
    ASSERT_NOT_NULL(w);
    /* In the far corner, so the view has to stop at the map's edge. */
    int king = Units_Spawn(0, 1, 0, w->map_pixels_w - 40, w->map_pixels_h - 40);
    ASSERT(king >= 0);

    static TAK_NetClient client;
    memset(&client, 0, sizeof client);
    TAK_Match_Begin(&client, 0, 4);
    ASSERT_EQ_INT(1, TAK_Match_IsLive());
    ASSERT_EQ_INT(0, Capture_ScoutTick(w, 1, 1000));
    TAK_Match_End();

    ASSERT_EQ_INT(1, Capture_ScoutTick(w, 2, 1000));
    ASSERT_EQ_INT(0, Capture_ScoutTick(w, 3, 1000));
    int32_t max_x = w->map_pixels_w - w->viewport_w, max_y = w->map_pixels_h - w->viewport_h;
    ASSERT(w->cam_x >= 0 && w->cam_x <= max_x);
    ASSERT(w->cam_y >= 0 && w->cam_y <= max_y);
    /* Half the capture in, the walk turns back. */
    ASSERT_EQ_INT(1, Capture_ScoutTick(w, 1000, 1000));
    /* A new battle starts a new walk. */
    Capture_ScoutReset();
    ASSERT_EQ_INT(1, Capture_ScoutTick(w, 1, 1000));
    ct_end();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("The fixed fog capture");
    RUN(the_capture_flags_take_good_values_and_refuse_bad_ones);
    RUN(the_fog_dump_is_what_the_classic_overlay_draws);
    RUN(the_scout_walks_in_a_skirmish_and_never_in_a_match);
    TEST_REPORT();
}
