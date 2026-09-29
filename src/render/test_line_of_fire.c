/*
 * test_line_of_fire.c -- shots meet what they fly into, and a side only
 * takes on what it can see. No game data.
 *
 * The harness is the movement tests' flat world with synthetic defs: an
 * archer, a bolt caster, a lightning caster, a catapult, walls and a
 * target with no weapon. A ridge, a rock and a sea are drawn into the
 * heightmap and the feature list where a case needs them.
 */

#include "test_framework.h"
#include "tak_battle_config.h"
#include "tak_features.h"
#include "tak_fog.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_shot_path.h"
#include "tak_sim_hash.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define LF_TILES  192
#define LF_GROUND 64

enum { LF_ARCHER = 0, LF_BOLT, LF_FIREBALL, LF_LIGHTNING, LF_FLAME, LF_SIEGE,
       LF_TARGET, LF_WALL, LF_SPELL, LF_THROUGH, LF_SEABOLT, LF_LONGBOW,
       LF_VICTIM, LF_SWORD, LF_SPOTTER, LF_KEEP, LF_TOWER, LF_KING, LF_QUICK,
       LF_SPLASH, LF_POST, LF_DEF_COUNT };

/* The shooter stands west of the target on one row of cells. */
#define LF_SX   800
#define LF_TX   1200
#define LF_ROW  1608

static void lf_fill(UnitDef *d, const char *name, float velocity, int health) {
    memset(d, 0, sizeof(*d));
    strncpy(d->unitname, name, sizeof(d->unitname) - 1);
    strncpy(d->display_name, name, sizeof(d->display_name) - 1);
    strncpy(d->movement_class, velocity > 0.0f ? "TESTSMALL" : "",
            sizeof(d->movement_class) - 1);
    d->max_health = health;
    d->sight_distance = 250;
    d->max_velocity = velocity;
    d->acceleration = velocity / 4.0f;
    d->brake_rate = velocity / 2.0f;
    d->turn_rate = 4000.0f;
    d->max_slope = 30;
    d->bmcode = velocity > 0.0f ? 1 : 0;
    d->cap_flags = velocity > 0.0f ? (UNIT_CAP_MOVE | UNIT_CAP_STOP) : 0;
    d->footprint_x = 1;
    d->footprint_z = 1;
    /* A model 30 px tall, what a bake would have found. */
    d->body_span_set = 1;
    d->body_bottom_px = 0;
    d->body_top_px = 30;
}

static UnitWeapon *lf_weapon(UnitDef *d, const char *name, const char *type,
                             int velocity, int range, int damage) {
    UnitWeapon *w = &d->weapons[d->num_weapons++];
    memset(w, 0, sizeof(*w));
    strncpy(w->name, name, sizeof(w->name) - 1);
    strncpy(w->type, type, sizeof(w->type) - 1);
    w->velocity_pps = velocity;
    w->range = range;
    w->damage = damage;
    w->reload_ticks = 600;
    w->burst_rate_ticks = 1;
    w->gravity_adjust = 1.0f;
    w->explosion_idx = -1;
    w->shadow_sprite = -1;
    w->rain_sprite = -1;
    for (int k = 0; k < 3; k++) w->radius_sprite[k] = -1;
    w->emit_ticks = 20;
    return w;
}

static void lf_heights(GameWorld *w, int x0, int x1, int y0, int y1, int h) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            w->tnt.heightmap[y * w->tnt.height_w + x] = (uint8_t)h;
}

static GameWorld *lf_world(int line_of_sight, int fog) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.line_of_sight = line_of_sight;
    cfg.players[0].kind = TAK_SLOT_HUMAN;
    cfg.players[1].kind = TAK_SLOT_HUMAN;
    cfg.players[2].kind = TAK_SLOT_HUMAN;
    /* Seats one and three are allies. */
    cfg.players[0].team = 1;
    cfg.players[1].team = 2;
    cfg.players[2].team = 1;
    if (World_BeginLoad(NULL, &cfg, "synthetic", "aramon") != 0) return NULL;
    GameWorld *w = World_Get();
    if (!w) return NULL;
    w->map_pixels_w = LF_TILES * 16;
    w->map_pixels_h = LF_TILES * 16;
    w->viewport_w = 640;
    w->viewport_h = 480;
    w->water_height = 0;
    w->tnt.width_tiles = LF_TILES;
    w->tnt.height_tiles = LF_TILES;
    w->tnt.height_w = LF_TILES + 1;
    w->tnt.height_h = LF_TILES + 1;
    size_t hn = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)tak_malloc(hn);
    if (!w->tnt.heightmap) return NULL;
    memset(w->tnt.heightmap, LF_GROUND, hn);
    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 1;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 2;
    w->moveinfo.classes[0].footprint_z = 2;
    w->moveinfo.classes[0].max_slope = 255;
    if (!Occ_Ensure(w)) return NULL;
    if (fog && Fog_Init(w) != 0) return NULL;
    TAK_PathCacheReset();
    World_MarkLoaded();

    UnitDef defs[LF_DEF_COUNT];
    /* An archer's low arc, a bowman's numbers. */
    lf_fill(&defs[LF_ARCHER], "TESTBOW", 1.2f, 300);
    UnitWeapon *wp = lf_weapon(&defs[LF_ARCHER], "TESTARROW", "Ballistic", 450, 550, 40);
    wp->is_gravity = 1;
    /* A plain Line of Sight bolt flies straight. */
    lf_fill(&defs[LF_BOLT], "TESTBOLT", 1.2f, 300);
    lf_weapon(&defs[LF_BOLT], "TESTBOLTW", "Line of Sight", 500, 550, 40);
    /* A mage's fireball: the same class of shot, paid for in mana. */
    lf_fill(&defs[LF_FIREBALL], "TESTMAGE", 1.2f, 300);
    wp = lf_weapon(&defs[LF_FIREBALL], "TESTFIREB", "Line of Sight", 400, 550, 60);
    strncpy(wp->weapon_art, "fireball", sizeof(wp->weapon_art) - 1);
    lf_fill(&defs[LF_LIGHTNING], "TESTZAP", 1.2f, 300);
    wp = lf_weapon(&defs[LF_LIGHTNING], "TESTZAPW", "Line of Sight", 2000, 550, 50);
    strncpy(wp->subtype, "lightning", sizeof(wp->subtype) - 1);
    wp->los_kind = 1;
    wp->is_los = 1;
    lf_fill(&defs[LF_FLAME], "TESTFLAME", 1.2f, 300);
    wp = lf_weapon(&defs[LF_FLAME], "TESTFLAMW", "Line of Sight", 300, 550, 50);
    strncpy(wp->subtype, "fire", sizeof(wp->subtype) - 1);
    wp->los_kind = 2;
    wp->is_los = 1;
    /* A catapult throwing high over what stands in front. */
    lf_fill(&defs[LF_SIEGE], "TESTPULT", 0.0f, 600);
    defs[LF_SIEGE].bmcode = 1;
    wp = lf_weapon(&defs[LF_SIEGE], "TESTROCK", "Ballistic", 330, 800, 80);
    wp->is_gravity = 1;
    wp->lob_preferred = 1;
    lf_fill(&defs[LF_TARGET], "TESTDUMMY", 1.2f, 1000);
    /* A wall, two cells square and 40 px tall. */
    lf_fill(&defs[LF_WALL], "TESTWALL", 0.0f, 500);
    defs[LF_WALL].footprint_x = 2;
    defs[LF_WALL].footprint_z = 2;
    defs[LF_WALL].is_feature = 1;
    defs[LF_WALL].body_top_px = 40;
    lf_fill(&defs[LF_SPELL], "TESTWAVE", 1.2f, 300);
    wp = lf_weapon(&defs[LF_SPELL], "TESTWAVEW", "Remote Effect", 400, 550, 40);
    wp->path_free = 1;
    lf_fill(&defs[LF_THROUGH], "TESTMIND", 1.2f, 300);
    wp = lf_weapon(&defs[LF_THROUGH], "TESTTHRU", "Line of Sight", 500, 550, 40);
    wp->units_only = 1;
    lf_fill(&defs[LF_SEABOLT], "TESTHARP", 1.2f, 300);
    wp = lf_weapon(&defs[LF_SEABOLT], "TESTHARPW", "Line of Sight", 500, 550, 40);
    wp->water_weapon = 1;
    lf_fill(&defs[LF_LONGBOW], "TESTLONG", 1.2f, 300);
    lf_weapon(&defs[LF_LONGBOW], "TESTLONGW", "Line of Sight", 500, 750, 40);
    /* The archer the report is about: it outreaches its own sight. */
    lf_fill(&defs[LF_VICTIM], "TESTVICT", 1.2f, 1000);
    defs[LF_VICTIM].sight_distance = 0;
    lf_weapon(&defs[LF_VICTIM], "TESTVICW", "Line of Sight", 500, 550, 40);
    lf_fill(&defs[LF_SWORD], "TESTBLADE", 1.2f, 1000);
    defs[LF_SWORD].sight_distance = 0;
    defs[LF_SWORD].leash_length = 500;
    lf_weapon(&defs[LF_SWORD], "TESTSWORD", "Melee", 0, 40, 40);
    lf_fill(&defs[LF_SPOTTER], "TESTSCOUT", 1.2f, 100);
    /* A keep seven cells square. */
    lf_fill(&defs[LF_KEEP], "TESTKEEP", 0.0f, 2000);
    defs[LF_KEEP].footprint_x = 7;
    defs[LF_KEEP].footprint_z = 7;
    defs[LF_KEEP].body_top_px = 80;
    /* A watch tower: reach 500 and nothing inside 180. */
    lf_fill(&defs[LF_TOWER], "TESTTOWER", 0.0f, 2000);
    defs[LF_TOWER].footprint_x = 2;
    defs[LF_TOWER].footprint_z = 2;
    defs[LF_TOWER].body_top_px = 60;
    wp = lf_weapon(&defs[LF_TOWER], "TESTTOWW", "Line of Sight", 500, 500, 40);
    wp->min_range = 180;
    /* A monarch that builds, sees the default 256 and reaches 500. */
    lf_fill(&defs[LF_KING], "TESTKING", 1.2f, 3000);
    defs[LF_KING].sight_distance = 0;
    defs[LF_KING].commander = 1;
    defs[LF_KING].cap_flags |= UNIT_CAP_BUILDER;
    defs[LF_KING].worker_time = 10.0f;
    lf_weapon(&defs[LF_KING], "TESTKINGW", "Line of Sight", 500, 500, 40);
    /* A bolt that reloads in half a second. */
    lf_fill(&defs[LF_QUICK], "TESTQUICK", 1.2f, 300);
    wp = lf_weapon(&defs[LF_QUICK], "TESTQUICKW", "Line of Sight", 500, 550, 40);
    wp->reload_ticks = 30;
    /* A bolt that splashes. */
    lf_fill(&defs[LF_SPLASH], "TESTSPLASH", 1.2f, 300);
    wp = lf_weapon(&defs[LF_SPLASH], "TESTSPLASW", "Line of Sight", 400, 550, 40);
    wp->area_of_effect = 48;
    wp->edge_effectiveness = 1.0f;

    /* A post one cell square with no weapon. */
    lf_fill(&defs[LF_POST], "TESTPOST", 0.0f, 1000);

    FeatureDef rock;
    memset(&rock, 0, sizeof rock);
    strncpy(rock.name, "TESTROCK", sizeof(rock.name) - 1);
    rock.footprint_x = 2;
    rock.footprint_z = 2;
    rock.height = 60;
    rock.blocking = 1;
    if (Features_DebugSetDefs(&rock, 1) != 1) return NULL;
    if (Units_DebugSetDefs(defs, LF_DEF_COUNT) != LF_DEF_COUNT) return NULL;
    if (Units_DebugSetYardmap(LF_WALL, "oooo") != 0) return NULL;
    if (Units_DebugSetYardmap(LF_KEEP,
            "ooooooooooooooooooooooooooooooooooooooooooooooooo") != 0) return NULL;
    if (Units_DebugSetYardmap(LF_TOWER, "oooo") != 0) return NULL;
    if (Units_DebugSetYardmap(LF_POST, "o") != 0) return NULL;
    Units_SetLocalPlayer(1);
    Fog_SetViewer(1);
    return w;
}

static void lf_end(void) {
    Units_ClearInstances();
    Features_FreeAll();
    World_End(NULL);
    TAK_PathCacheReset();
}

static const Unit *lf_unit(int handle) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    return (handle >= 0 && handle < count) ? &units[handle] : NULL;
}

static int lf_spawn(int def, int player, int32_t x, int32_t y) {
    int h = Units_Spawn(def, player, player - 1, x, y);
    if (h >= 0) Units_DebugSetAggro(h, UNIT_AGGRO_PASSIVE);
    return h;
}

/* A ridge across the row, two cells wide at x 992..1024, `rise` px
 * above the plain. */
static void lf_ridge(GameWorld *w, int rise) {
    lf_heights(w, 62, 64, 0, LF_TILES, LF_GROUND + rise);
}

/* Where a shot ended. */
typedef struct LfShot {
    int     fired;
    int     alive_ticks;
    int32_t x, y;
    float   height;
} LfShot;

/* Fire weapon 0 of `shooter` at `target` and follow the shot until it
 * is gone or `ticks` pass. */
static LfShot lf_fire(int shooter, int target, int ticks) {
    LfShot r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    int before = 0;
    Units_GetProjectiles(&before);
    if (!Units_DebugFireAt(shooter, 0, target)) return r;
    int count = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    int slot = -1;
    for (int i = 0; i < count; i++)
        if (ps[i].alive && ps[i].shooter == shooter) slot = i;
    if (slot < 0) return r;
    r.fired = 1;
    for (int t = 0; t < ticks && ps[slot].alive; t++) {
        Units_TickEngines();
        ps = Units_GetProjectiles(&count);
        r.alive_ticks++;
    }
    r.x = ps[slot].world_x;
    r.y = ps[slot].world_y;
    r.height = ps[slot].height;
    return r;
}

/* ── the cell test ─────────────────────────────────────────────────── */

static int g_lf_ground_unit = -1;
static int lf_ground_cb(void *user, int handle, int owner, int h) {
    (void)user; (void)owner;
    return handle == g_lf_ground_unit && h < 100;
}
static int lf_air_cb(void *user, int32_t x, int32_t y, int h, int owner) {
    (void)user; (void)x; (void)y; (void)h; (void)owner;
    return -1;
}

TEST(the_cell_test_meets_a_unit_then_a_feature_then_the_ground) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    /* A rock 60 tall on cells (70..71, 100..101), a unit on (70, 100). */
    ASSERT(Features_AddInstance(w, 0, 70, 100, 70 * 16 + 16, 100 * 16 + 16, 0, -1) >= 0);
    w->occ[100 * w->occ_w + 70].unit_plus1 = 8;
    w->occ[100 * w->occ_w + 70].owner = 2;
    g_lf_ground_unit = 7;
    TAK_ShotBodies bodies = { lf_ground_cb, lf_air_cb, NULL };
    int u = -1;
    int32_t x = 70 * 16 + 4, y = 100 * 16 + 4;
    ASSERT_EQ_INT(TAK_SHOT_UNIT, ShotPath_Test(w, x, y, 90, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(7, u);
    /* Above the unit's body, under the rock's top. */
    ASSERT_EQ_INT(TAK_SHOT_FEATURE, ShotPath_Test(w, x, y, 110, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(-1, u);
    ASSERT_EQ_INT(TAK_SHOT_FLY, ShotPath_Test(w, x, y, LF_GROUND + 61, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(TAK_SHOT_FEATURE, ShotPath_Test(w, x, y, LF_GROUND + 60, 1, 0, &bodies, &u));
    /* unitsonly passes the rock but not the unit. */
    ASSERT_EQ_INT(TAK_SHOT_FLY,
                  ShotPath_Test(w, x, y, 110, 1, TAK_SHOT_UNITS_ONLY, &bodies, &u));
    ASSERT_EQ_INT(TAK_SHOT_UNIT,
                  ShotPath_Test(w, x, y, 90, 1, TAK_SHOT_UNITS_ONLY, &bodies, &u));
    /* Open ground: flies above its cell's floor, bursts at it. */
    int32_t ox = 40 * 16 + 8, oy = 40 * 16 + 8;
    ASSERT_EQ_INT(TAK_SHOT_FLY, ShotPath_Test(w, ox, oy, LF_GROUND + 1, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(TAK_SHOT_GROUND, ShotPath_Test(w, ox, oy, LF_GROUND, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(TAK_SHOT_BOUNCE,
                  ShotPath_Test(w, ox, oy, LF_GROUND, 1, TAK_SHOT_GROUND_BOUNCE, &bodies, &u));
    /* The floor is the cell's lowest corner. */
    lf_heights(w, 41, 41, 41, 41, LF_GROUND + 20);
    ASSERT_EQ_INT(TAK_SHOT_FLY, ShotPath_Test(w, ox, oy, LF_GROUND + 1, 1, 0, &bodies, &u));
    /* The sea stops a shot above the ground, a water weapon flies on. */
    w->water_height = LF_GROUND + 10;
    ASSERT_EQ_INT(TAK_SHOT_WATER, ShotPath_Test(w, ox, oy, LF_GROUND + 5, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(TAK_SHOT_FLY,
                  ShotPath_Test(w, ox, oy, LF_GROUND + 5, 1, TAK_SHOT_WATER_WEAPON, &bodies, &u));
    w->no_sea_level_trigger = 1;
    ASSERT_EQ_INT(TAK_SHOT_FLY, ShotPath_Test(w, ox, oy, LF_GROUND + 5, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(TAK_SHOT_OFFMAP, ShotPath_Test(w, -1, oy, 200, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(TAK_SHOT_OFFMAP,
                  ShotPath_Test(w, LF_TILES * 16 + 4, oy, 200, 1, 0, &bodies, &u));
    ASSERT_EQ_INT(1, ShotPath_Substeps(0, 0));
    ASSERT_EQ_INT(1, ShotPath_Substeps(16, -9));
    ASSERT_EQ_INT(2, ShotPath_Substeps(-17, 3));
    w->occ[100 * w->occ_w + 70].unit_plus1 = 0;
    lf_end();
}

TEST(a_feature_leaves_its_cells_when_it_goes) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int f = Features_AddInstance(w, 0, 70, 100, 70 * 16 + 16, 100 * 16 + 16, 0, -1);
    ASSERT(f >= 0);
    ASSERT_EQ_INT(61, Features_TopAt(w, 71, 101));
    ASSERT_EQ_INT(0, Features_TopAt(w, 72, 101));
    ASSERT_EQ_INT(0, Features_RemoveInstance(w, f));
    ASSERT_EQ_INT(0, Features_TopAt(w, 71, 101));
    lf_end();
}

/* A feature that goes leaves the cells of its neighbour and of one far
 * off as they were. */
TEST(a_feature_that_goes_leaves_the_others_their_cells) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int a = Features_AddInstance(w, 0, 68, 100, 68 * 16 + 16, 100 * 16 + 16, 0, -1);
    int far = Features_AddInstance(w, 0, 20, 20, 20 * 16 + 16, 20 * 16 + 16, 0, -1);
    int b = Features_AddInstance(w, 0, 70, 101, 70 * 16 + 16, 101 * 16 + 16, 0, -1);
    ASSERT(a >= 0 && far >= 0 && b >= 0);
    ASSERT_EQ_INT(61, Features_TopAt(w, 71, 102));
    ASSERT_EQ_INT(0, Features_RemoveInstance(w, b));
    ASSERT_EQ_INT(61, Features_TopAt(w, 69, 101));
    ASSERT_EQ_INT(0, Features_TopAt(w, 70, 101));
    ASSERT_EQ_INT(0, Features_TopAt(w, 71, 102));
    ASSERT_EQ_INT(61, Features_TopAt(w, 21, 21));
    lf_end();
}

/* ── shots across a hill ───────────────────────────────────────────── */

TEST(an_arrow_over_open_ground_hits) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(t)->health < 1000);
    lf_end();
}

TEST(an_arrow_bursts_on_a_ridge_taller_than_its_arc) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 40);
    int s = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    printf("(burst at x %d) ", (int)r.x);
    ASSERT(r.x >= 976 && r.x <= 1024);
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    lf_end();
}

TEST(a_straight_bolt_bursts_on_a_ridge_and_hits_over_flat_ground) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_BOLT, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(t)->health < 1000);
    int hurt = lf_unit(t)->health;
    lf_ridge(w, 20);
    r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    printf("(burst at x %d h %d) ", (int)r.x, (int)r.height);
    ASSERT(r.x >= 976 && r.x <= 1024);
    ASSERT_EQ_INT(hurt, lf_unit(t)->health);
    lf_end();
}

TEST(a_fireball_bursts_on_a_ridge) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 20);
    int s = lf_spawn(LF_FIREBALL, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(r.x >= 976 && r.x <= 1024);
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    lf_end();
}

/* A unit right behind a one cell crest is not struck through it: the
 * cell a shot would land in has its say first. Lightning walks in 16 px
 * steps, and every phase of those steps against the crest is tried. */
TEST(a_unit_right_behind_a_one_cell_crest_is_not_struck_through_it) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    /* Cell 62, x 992 to 1008, stands 40 px up, and the post holds
     * cell 63 behind it, 8 to 24 px from every point of the crest. */
    lf_heights(w, 62, 63, 0, LF_TILES, LF_GROUND + 40);
    int t = lf_spawn(LF_POST, 2, 1016, LF_ROW);
    int zap = lf_spawn(LF_LIGHTNING, 1, LF_SX, LF_ROW);
    int bolt = lf_spawn(LF_BOLT, 1, LF_SX, LF_ROW + 32);
    ASSERT(t >= 0 && zap >= 0 && bolt >= 0);
    ASSERT_EQ_INT(1016, lf_unit(t)->world_x);
    LfShot r = lf_fire(zap, t, 30);
    ASSERT(r.fired);
    printf("(lightning ends at x %d, ", (int)r.x);
    ASSERT(r.x >= 992 && r.x < 1008);
    r = lf_fire(bolt, t, 300);
    ASSERT(r.fired);
    printf("the bolt at x %d) ", (int)r.x);
    ASSERT(r.x >= 976 && r.x < 1008);
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    lf_end();
}

/* A rock thrown high at a unit that walks in under its arc passes over
 * its head: the shot has to reach the target in height too. */
TEST(a_lobbed_rock_passes_over_a_unit_that_walked_under_it) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_SIEGE, 1, LF_SX - 150, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    ASSERT(Units_OrderMove(t, LF_SX, LF_ROW));
    LfShot r = lf_fire(s, t, 400);
    ASSERT(r.fired);
    printf("(landed at x %d, target at x %d) ", (int)r.x,
           (int)lf_unit(t)->world_x);
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    lf_end();
}

TEST(lightning_ends_at_a_rock_in_its_way) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    ASSERT(Features_AddInstance(w, 0, 62, 99, 1008, 1600, 0, -1) >= 0);
    int s = lf_spawn(LF_LIGHTNING, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireAt(s, 0, t));
    int count = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    int beam = -1;
    for (int i = 0; i < count; i++) if (ps[i].alive && ps[i].is_beam) beam = i;
    ASSERT(beam >= 0);
    printf("(ray ends at x %d h %d) ", (int)ps[beam].world_x, (int)ps[beam].height);
    ASSERT(ps[beam].world_x >= 976 && ps[beam].world_x <= 1024);
    /* It stops at the height it reached, not on the ground. */
    ASSERT(ps[beam].height > (float)LF_GROUND);
    ASSERT(ps[beam].height <= (float)(LF_GROUND + 60 + 1));
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    lf_end();
}

TEST(a_flame_stops_at_a_ridge_and_reaches_over_open_ground) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_FLAME, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireAt(s, 0, t));
    ASSERT(lf_unit(t)->health < 1000);
    int hurt = lf_unit(t)->health;
    for (int i = 0; i < 60; i++) Units_TickEngines();
    lf_ridge(w, 20);
    ASSERT(Units_DebugFireAt(s, 0, t));
    ASSERT_EQ_INT(hurt, lf_unit(t)->health);
    lf_end();
}

/* ── shots through a wall ──────────────────────────────────────────── */

TEST(an_enemy_wall_takes_the_arrow) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int wl = lf_spawn(LF_WALL, 2, 1008, LF_ROW);
    ASSERT(s >= 0 && t >= 0 && wl >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(r.x >= 976 && r.x <= 1024);
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    ASSERT(lf_unit(wl)->health < 500);
    lf_end();
}

TEST(the_shooters_own_wall_lets_its_arrow_through) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int wl = lf_spawn(LF_WALL, 1, 1008, LF_ROW);
    ASSERT(s >= 0 && t >= 0 && wl >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(t)->health < 1000);
    ASSERT_EQ_INT(500, lf_unit(wl)->health);
    lf_end();
}

TEST(an_allied_wall_stops_the_arrow_and_takes_nothing) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int wl = lf_spawn(LF_WALL, 3, 1008, LF_ROW);
    ASSERT(s >= 0 && t >= 0 && wl >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(r.x >= 976 && r.x <= 1024);
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    ASSERT_EQ_INT(500, lf_unit(wl)->health);
    lf_end();
}

TEST(a_bolt_and_a_fireball_through_an_enemy_wall_hit_the_wall) {
    static const int shooters[2] = { LF_BOLT, LF_FIREBALL };
    for (int k = 0; k < 2; k++) {
        ASSERT_NOT_NULL(lf_world(0, 0));
        int s = lf_spawn(shooters[k], 1, LF_SX, LF_ROW);
        int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
        int wl = lf_spawn(LF_WALL, 2, 1008, LF_ROW);
        ASSERT(s >= 0 && t >= 0 && wl >= 0);
        LfShot r = lf_fire(s, t, 300);
        ASSERT(r.fired);
        ASSERT(r.x >= 976 && r.x <= 1024);
        ASSERT_EQ_INT(1000, lf_unit(t)->health);
        ASSERT(lf_unit(wl)->health < 500);
        lf_end();
    }
}

TEST(lightning_through_an_enemy_wall_strikes_the_wall) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_LIGHTNING, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int wl = lf_spawn(LF_WALL, 2, 1008, LF_ROW);
    ASSERT(s >= 0 && t >= 0 && wl >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireAt(s, 0, t));
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    ASSERT(lf_unit(wl)->health < 500);
    lf_end();
}

TEST(a_catapult_throws_over_a_wall) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_SIEGE, 1, LF_SX - 200, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int wl = lf_spawn(LF_WALL, 2, LF_TX - 48, LF_ROW);
    ASSERT(s >= 0 && t >= 0 && wl >= 0);
    LfShot r = lf_fire(s, t, 600);
    ASSERT(r.fired);
    printf("(landed at x %d) ", (int)r.x);
    ASSERT(lf_unit(t)->health < 1000);
    ASSERT_EQ_INT(500, lf_unit(wl)->health);
    lf_end();
}

/* ── what passes ───────────────────────────────────────────────────── */

TEST(a_units_only_shot_passes_a_ridge) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 20);
    int s = lf_spawn(LF_THROUGH, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(t)->health < 1000);
    lf_end();
}

TEST(a_water_weapon_passes_under_the_sea_and_a_bolt_does_not) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    /* The target wades a channel, sea at 60 over a bed at 40. */
    lf_heights(w, 58, 90, 0, LF_TILES, 40);
    w->water_height = 60;
    int b = lf_spawn(LF_BOLT, 1, LF_SX, LF_ROW);
    int h = lf_spawn(LF_SEABOLT, 1, LF_SX, LF_ROW + 64);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int t2 = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW + 64);
    ASSERT(b >= 0 && h >= 0 && t >= 0 && t2 >= 0);
    LfShot r = lf_fire(b, t, 300);
    ASSERT(r.fired);
    printf("(bolt into the sea at x %d) ", (int)r.x);
    ASSERT(r.x < LF_TX - 24);
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    r = lf_fire(h, t2, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(t2)->health < 1000);
    lf_end();
}

TEST(a_shot_whose_target_dies_flies_on_and_lands) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireAt(s, 0, t));
    int count = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    int slot = -1;
    for (int i = 0; i < count; i++) if (ps[i].alive) slot = i;
    ASSERT(slot >= 0);
    for (int i = 0; i < 10; i++) Units_TickEngines();
    ASSERT_EQ_INT(0, Units_DebugRemove(t));
    Units_TickEngines();
    ps = Units_GetProjectiles(&count);
    ASSERT(ps[slot].alive);
    int ticks = 0;
    while (ps[slot].alive && ticks < 300) {
        Units_TickEngines();
        ps = Units_GetProjectiles(&count);
        ticks++;
    }
    printf("(landed at x %d after %d more ticks) ", (int)ps[slot].world_x, ticks);
    /* Past where its target stood, on the ground. */
    ASSERT(ticks > 10);
    ASSERT(ps[slot].world_x > LF_TX - 24 && ps[slot].world_x < LF_TX + 200);
    ASSERT(ps[slot].height < (float)(LF_GROUND + 1));
    lf_end();
}

/* Only a shot fired at the ground has a fuse at its aim point. A bolt
 * whose target dies flies on past where it was aimed, and its splash
 * does not go off over a bystander standing there. */
TEST(a_bolt_whose_target_dies_flies_past_its_aim) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_SPLASH, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int by = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW + 40);
    ASSERT(s >= 0 && t >= 0 && by >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireAt(s, 0, t));
    int count = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    int slot = -1;
    for (int i = 0; i < count; i++) if (ps[i].alive && ps[i].shooter == s) slot = i;
    ASSERT(slot >= 0);
    for (int i = 0; i < 10; i++) Units_TickEngines();
    ASSERT_EQ_INT(0, Units_DebugRemove(t));
    int passed = 0;
    for (int i = 0; i < 300; i++) {
        Units_TickEngines();
        ps = Units_GetProjectiles(&count);
        if (!ps[slot].alive) break;
        if (ps[slot].world_x > LF_TX + 48) { passed = 1; break; }
    }
    ASSERT(passed);
    ASSERT_EQ_INT(1000, lf_unit(by)->health);
    lf_end();
}

/* A beam that finds the shot pool full draws nothing, and its ray still
 * has to reach the target: a clear line hits, a rock in the way stops it. */
TEST(lightning_with_the_pool_full_still_meets_the_rock) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    ASSERT(Features_AddInstance(w, 0, 62, 99, 1008, 1600, 0, -1) >= 0);
    int s = lf_spawn(LF_LIGHTNING, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int clear = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW + 200);
    int filler = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW + 600);
    ASSERT(s >= 0 && t >= 0 && clear >= 0 && filler >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    for (int i = 0; i < 5000; i++)
        Units_DebugFireGround(filler, 0, LF_SX + 300, LF_ROW + 600);
    int count = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    int alive = 0;
    for (int i = 0; i < count; i++) alive += ps[i].alive ? 1 : 0;
    printf("(%d shots in the air) ", alive);
    ASSERT(count >= 4096);
    ASSERT_EQ_INT(count, alive);
    ASSERT(Units_DebugFireAt(s, 0, clear));
    ASSERT(lf_unit(clear)->health < 1000);
    ASSERT(Units_DebugFireAt(s, 0, t));
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    lf_end();
}

/* With the pool full a beam still stops on the first thing in its way:
 * an enemy wall there takes the hit, and a rock there counts toward
 * letting the target go (D-025). */
TEST(a_beam_with_the_pool_full_strikes_what_is_in_its_way) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.players[0].kind = TAK_SLOT_AI;
    ASSERT(Features_AddInstance(w, 0, 62, 99, 1008, 1600, 0, -1) >= 0);
    int s = lf_spawn(LF_LIGHTNING, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int s2 = lf_spawn(LF_LIGHTNING, 1, LF_SX, LF_ROW + 200);
    int wall = lf_spawn(LF_WALL, 2, 1104, LF_ROW + 200);
    int t2 = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW + 200);
    int filler = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW + 600);
    ASSERT(s >= 0 && t >= 0 && s2 >= 0 && wall >= 0 && t2 >= 0 && filler >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    for (int i = 0; i < 5000; i++)
        Units_DebugFireGround(filler, 0, LF_SX + 300, LF_ROW + 600);
    int count = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    int alive = 0;
    for (int i = 0; i < count; i++) alive += ps[i].alive ? 1 : 0;
    ASSERT_EQ_INT(count, alive);
    ASSERT(Units_DebugFireAt(s2, 0, t2));
    ASSERT(lf_unit(wall)->health < 500);
    ASSERT_EQ_INT(1000, lf_unit(t2)->health);
    Units_CommandAttackUnit(s, t);
    ASSERT_EQ_INT(t, lf_unit(s)->target);
    for (int i = 0; i < 3; i++) ASSERT(Units_DebugFireAt(s, 0, t));
    ASSERT_EQ_INT(1000, lf_unit(t)->health);
    ASSERT_EQ_INT(-1, lf_unit(s)->target);
    ASSERT_EQ_INT(0, Units_CanAttackTarget(s, t));
    lf_end();
}

/* A shot never strikes the unit that fired it: a splash at its feet
 * passes it by (legacy:245150) and so does a burst on the ground there. */
TEST(a_shot_at_the_shooters_feet_leaves_the_shooter_whole) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_SPLASH, 1, LF_SX, LF_ROW);
    int own = lf_spawn(LF_TARGET, 1, LF_SX + 40, LF_ROW);
    int b = lf_spawn(LF_BOLT, 1, LF_SX, LF_ROW + 200);
    ASSERT(s >= 0 && own >= 0 && b >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireGround(s, 0, LF_SX + 30, LF_ROW));
    ASSERT(Units_DebugFireGround(b, 0, LF_SX + 8, LF_ROW + 200));
    for (int t = 0; t < 60; t++) Units_TickEngines();
    ASSERT_EQ_INT(300, lf_unit(s)->health);
    ASSERT(lf_unit(own)->health < 1000);
    ASSERT_EQ_INT(300, lf_unit(b)->health);
    lf_end();
}

TEST(a_remote_spell_behind_a_ridge_still_lands) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 60);
    int s = lf_spawn(LF_SPELL, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    LfShot r = lf_fire(s, t, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(t)->health < 1000);
    lf_end();
}

/* ── determinism ───────────────────────────────────────────────────── */

static uint32_t lf_volley_hash(int *out_hurt) {
    GameWorld *w = lf_world(0, 0);
    if (!w) return 0;
    lf_ridge(w, 20);
    Features_AddInstance(w, 0, 62, 110, 1008, 1776, 0, -1);
    int shooters[5] = { LF_ARCHER, LF_BOLT, LF_FIREBALL, LF_LIGHTNING, LF_SIEGE };
    int targets[5];
    for (int i = 0; i < 5; i++) {
        int32_t y = 1400 + i * 96;
        int s = lf_spawn(shooters[i], 1, LF_SX - (i == 4 ? 150 : 0), y);
        targets[i] = lf_spawn(LF_TARGET, 2, LF_TX + (i & 1) * 40, y);
        if (!(i & 1)) lf_spawn(LF_WALL, 2, 1104, y);
        Units_DebugSetAggro(s, UNIT_AGGRO_OFFENSIVE);
        Units_OrderAttack(s, targets[i]);
    }
    for (int i = 0; i < 1200; i++) Units_TickEngines();
    int hurt = 0;
    for (int i = 0; i < 5; i++) if (lf_unit(targets[i])->health < 1000) hurt++;
    *out_hurt = hurt;
    uint32_t h = TAK_SimHash();
    lf_end();
    return h;
}

TEST(a_blocked_volley_hashes_the_same) {
    int hurt_a = 0, hurt_b = 0;
    uint32_t a = lf_volley_hash(&hurt_a);
    uint32_t b = lf_volley_hash(&hurt_b);
    printf("(%08x, %d targets hurt) ", a, hurt_a);
    ASSERT(a != 0);
    ASSERT(hurt_a >= 1);
    ASSERT_EQ_INT((int)a, (int)b);
    ASSERT_EQ_INT(hurt_a, hurt_b);
}

/* ── what a side can see ───────────────────────────────────────────── */

static void lf_fog(GameWorld *w) {
    for (int p = 1; p <= 3; p++) Fog_Update(w, p);
}

/* The volley again with Line of Sight on, a computer's shooters and
 * armed targets that see only 256: return fire answers what it cannot
 * see, the shooters let go of targets behind the ridge and go looking,
 * and the fog is state. Pinned, so every platform in CI has to
 * reach the same answer. */
#define LF_FOG_VOLLEY_HASH 0xaf7efc87u

static uint32_t lf_fog_volley_hash(int *out_hurt) {
    GameWorld *w = lf_world(1, 1);
    if (!w) return 0;
    w->cfg.players[0].kind = TAK_SLOT_AI;
    lf_ridge(w, 20);
    Features_AddInstance(w, 0, 62, 110, 1008, 1776, 0, -1);
    int shooters[5] = { LF_ARCHER, LF_BOLT, LF_FIREBALL, LF_LIGHTNING, LF_SIEGE };
    int targets[5];
    for (int i = 0; i < 5; i++) {
        int32_t y = 1400 + i * 96;
        int s = lf_spawn(shooters[i], 1, LF_SX - (i == 4 ? 150 : 0), y);
        targets[i] = lf_spawn(LF_VICTIM, 2, LF_TX + (i & 1) * 40, y);
        if (!(i & 1)) lf_spawn(LF_WALL, 2, 1104, y);
        Units_DebugSetAggro(s, UNIT_AGGRO_OFFENSIVE);
        Units_DebugSetAggro(targets[i], UNIT_AGGRO_OFFENSIVE);
        Units_CommandAttackUnit(s, targets[i]);
    }
    for (int i = 0; i < 2400; i++) {
        if (i % 30 == 0) lf_fog(w);
        Units_TickEngines();
    }
    int hurt = 0;
    for (int i = 0; i < 5; i++) if (lf_unit(targets[i])->health < 1000) hurt++;
    *out_hurt = hurt;
    uint32_t h = TAK_SimHash();
    lf_end();
    return h;
}

TEST(a_volley_in_the_fog_hashes_to_its_pin) {
    int hurt_a = 0, hurt_b = 0;
    uint32_t a = lf_fog_volley_hash(&hurt_a);
    uint32_t b = lf_fog_volley_hash(&hurt_b);
    printf("(%08x, %d targets hurt) ", a, hurt_a);
    ASSERT(hurt_a >= 1);
    ASSERT_EQ_INT((int)a, (int)b);
    ASSERT_EQ_INT((int)LF_FOG_VOLLEY_HASH, (int)a);
}

TEST(a_ranged_unit_waits_to_see_before_it_acquires) {
    GameWorld *w = lf_world(1, 1);
    ASSERT_NOT_NULL(w);
    /* Range 550 against sight 250. */
    int a = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int e = lf_spawn(LF_TARGET, 2, LF_SX + 420, LF_ROW);
    ASSERT(a >= 0 && e >= 0);
    Units_DebugSetAggro(a, UNIT_AGGRO_OFFENSIVE);
    lf_fog(w);
    for (int i = 0; i < 30; i++) Units_TickEngines();
    ASSERT_EQ_INT(-1, lf_unit(a)->target);
    ASSERT_EQ_INT(0, Units_IsVisibleToLocalPlayer(lf_unit(e)));
    int sp = lf_spawn(LF_SPOTTER, 1, LF_SX + 300, LF_ROW + 64);
    ASSERT(sp >= 0);
    lf_fog(w);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT_EQ_INT(1, Units_IsVisibleToLocalPlayer(lf_unit(e)));
    ASSERT_EQ_INT(e, lf_unit(a)->target);
    lf_end();
}

/* The original's return fire has no sight test and shows the side
 * nothing: the victim answers a shooter in its reach that its player
 * cannot see, and the shooter stays unseen. */
TEST(a_hit_is_answered_and_leaves_its_shooter_unseen) {
    GameWorld *w = lf_world(1, 1);
    ASSERT_NOT_NULL(w);
    int v = lf_spawn(LF_VICTIM, 1, LF_SX, LF_ROW);
    int s = lf_spawn(LF_BOLT, 2, LF_SX + 450, LF_ROW);
    ASSERT(v >= 0 && s >= 0);
    Units_DebugSetAggro(v, UNIT_AGGRO_OFFENSIVE);
    lf_fog(w);
    ASSERT_EQ_INT(0, Units_SideSees(1, s));
    LfShot r = lf_fire(s, v, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(v)->health < 1000);
    ASSERT_EQ_INT(s, lf_unit(v)->target);
    ASSERT_EQ_INT(0, Units_SideSees(1, s));
    ASSERT_EQ_INT(0, Units_SideSees(3, s));
    ASSERT_EQ_INT(0, Units_IsVisibleToLocalPlayer(lf_unit(s)));
    lf_end();
}

TEST(return_fire_needs_the_shooter_in_reach) {
    GameWorld *w = lf_world(1, 1);
    ASSERT_NOT_NULL(w);
    /* Reach 550 and no leash against a shooter 700 away. */
    int v = lf_spawn(LF_VICTIM, 1, LF_SX, LF_ROW);
    int s = lf_spawn(LF_LONGBOW, 2, LF_SX + 700, LF_ROW);
    int sp = lf_spawn(LF_SPOTTER, 1, LF_SX + 680, LF_ROW + 64);
    ASSERT(v >= 0 && s >= 0 && sp >= 0);
    Units_DebugSetAggro(v, UNIT_AGGRO_OFFENSIVE);
    lf_fog(w);
    ASSERT_EQ_INT(1, Units_SideSees(1, s));
    LfShot r = lf_fire(s, v, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(v)->health < 1000);
    ASSERT_EQ_INT(-1, lf_unit(v)->target);
    ASSERT_EQ_INT(UNIT_CMD_NONE, lf_unit(v)->cmd_kind);
    lf_end();
}

TEST(a_blade_answers_a_seen_shooter_inside_its_leash) {
    GameWorld *w = lf_world(1, 1);
    ASSERT_NOT_NULL(w);
    /* Nine tenths of a 500 leash is 450. */
    int v = lf_spawn(LF_SWORD, 1, LF_SX, LF_ROW);
    int near = lf_spawn(LF_BOLT, 2, LF_SX + 400, LF_ROW);
    int sp = lf_spawn(LF_SPOTTER, 1, LF_SX + 400, LF_ROW + 64);
    ASSERT(v >= 0 && near >= 0 && sp >= 0);
    Units_DebugSetAggro(v, UNIT_AGGRO_OFFENSIVE);
    lf_fog(w);
    LfShot r = lf_fire(near, v, 300);
    ASSERT(r.fired);
    ASSERT_EQ_INT(near, lf_unit(v)->target);
    lf_end();

    w = lf_world(1, 1);
    ASSERT_NOT_NULL(w);
    v = lf_spawn(LF_SWORD, 1, LF_SX, LF_ROW);
    int far = lf_spawn(LF_BOLT, 2, LF_SX + 500, LF_ROW);
    sp = lf_spawn(LF_SPOTTER, 1, LF_SX + 500, LF_ROW + 64);
    ASSERT(v >= 0 && far >= 0 && sp >= 0);
    Units_DebugSetAggro(v, UNIT_AGGRO_OFFENSIVE);
    lf_fog(w);
    r = lf_fire(far, v, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(v)->health < 1000);
    ASSERT_EQ_INT(-1, lf_unit(v)->target);
    lf_end();
}

/* A building has no leash, so it answers only a shooter its weapon
 * reaches, past its minrange, and goes on firing at what it can hit.
 * The shooter on the diagonal stands 540 away, which the leash's
 * measure, the longer side plus a quarter of the shorter, reads as 477. */
TEST(a_tower_answers_only_what_it_can_reach) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int tw = Units_Spawn(LF_TOWER, 1, 0, 1600, 1600);
    int far = lf_spawn(LF_LONGBOW, 2, 1600 + 382, 1600 + 382);
    int near = lf_spawn(LF_BOLT, 2, 1600 + 150, 1600);
    ASSERT(tw >= 0 && far >= 0 && near >= 0);
    Units_DebugSetAggro(tw, UNIT_AGGRO_OFFENSIVE);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT_EQ_INT(-1, lf_unit(tw)->target);
    LfShot r = lf_fire(far, tw, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(tw)->health < 2000);
    ASSERT_EQ_INT(-1, lf_unit(tw)->target);
    int hurt = lf_unit(tw)->health;
    r = lf_fire(near, tw, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(tw)->health < hurt);
    ASSERT_EQ_INT(-1, lf_unit(tw)->target);
    int knight = lf_spawn(LF_TARGET, 2, 1600, 1600 + 300);
    ASSERT(knight >= 0);
    for (int i = 0; i < 120 && lf_unit(knight)->health == 1000; i++)
        Units_TickEngines();
    ASSERT_EQ_INT(knight, lf_unit(tw)->target);
    ASSERT(lf_unit(knight)->health < 1000);
    lf_end();
}

/* A computer's monarch hit at its work drops the build only to answer
 * the shooter, which its side need not see. One it cannot answer,
 * beyond its reach with no leash, it builds on through. */
TEST(an_ai_monarch_at_work_answers_a_shooter_or_builds_on) {
    for (int k = 0; k < 2; k++) {
        GameWorld *w = lf_world(1, 1);
        ASSERT_NOT_NULL(w);
        w->cfg.players[1].kind = TAK_SLOT_AI;
        int king = lf_spawn(LF_KING, 2, LF_SX, LF_ROW);
        int s = k == 0 ? lf_spawn(LF_BOLT, 1, LF_SX - 450, LF_ROW)
                       : lf_spawn(LF_LONGBOW, 1, LF_SX - 700, LF_ROW);
        ASSERT(king >= 0 && s >= 0);
        Units_DebugSetAggro(king, UNIT_AGGRO_OFFENSIVE);
        ASSERT(Units_BeginBuildingForUnit(king, LF_WALL, LF_SX + 64,
                                          LF_ROW + 64) >= 0);
        lf_fog(w);
        ASSERT_EQ_INT(0, Units_SideSees(2, s));
        LfShot r = lf_fire(s, king, 300);
        ASSERT(r.fired);
        ASSERT(lf_unit(king)->health < 3000);
        for (int i = 0; i < 10; i++) Units_TickEngines();
        const Unit *m = lf_unit(king);
        printf("(%s: order %d, target %d) ", k == 0 ? "in reach" : "beyond it",
               (int)m->cmd_kind, (int)m->target);
        if (k == 0) ASSERT_EQ_INT(s, m->target);
        else        ASSERT_EQ_INT(UNIT_CMD_BUILD, m->cmd_kind);
        lf_end();
    }
}

/* D-025: a unit lets go of a target it picked for itself once three
 * shots in a row stop short of it, and passes it over for a while, so
 * it takes one it can hit. A player's own attack order keeps firing,
 * and a computer's is let go like a target it picked. */
TEST(a_unit_lets_go_of_a_target_its_shots_cannot_reach) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 40);
    int a = lf_spawn(LF_QUICK, 1, LF_SX, LF_ROW);
    int hid = lf_spawn(LF_TARGET, 2, LF_SX + 400, LF_ROW);
    ASSERT(a >= 0 && hid >= 0);
    Units_DebugSetAggro(a, UNIT_AGGRO_OFFENSIVE);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT_EQ_INT(hid, lf_unit(a)->target);
    int open = lf_spawn(LF_TARGET, 2, LF_SX - 450, LF_ROW);
    ASSERT(open >= 0);
    int ticks = 0;
    while (lf_unit(a)->target == hid && ticks < 600) {
        Units_TickEngines();
        ticks++;
    }
    printf("(let go after %d ticks) ", ticks);
    ASSERT(lf_unit(a)->target != hid);
    ASSERT_EQ_INT(1000, lf_unit(hid)->health);
    ASSERT_EQ_INT(0, Units_CanAttackTarget(a, hid));
    for (int i = 0; i < 240; i++) Units_TickEngines();
    ASSERT_EQ_INT(open, lf_unit(a)->target);
    ASSERT(lf_unit(open)->health < 1000);
    lf_end();

    w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 40);
    a = lf_spawn(LF_QUICK, 1, LF_SX, LF_ROW);
    hid = lf_spawn(LF_TARGET, 2, LF_SX + 400, LF_ROW);
    ASSERT(a >= 0 && hid >= 0);
    ASSERT(Units_OrderAttack(a, hid));
    for (int i = 0; i < 600; i++) Units_TickEngines();
    ASSERT_EQ_INT(hid, lf_unit(a)->target);
    ASSERT_EQ_INT(1000, lf_unit(hid)->health);
    lf_end();

    w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.players[0].kind = TAK_SLOT_AI;
    lf_ridge(w, 40);
    a = lf_spawn(LF_QUICK, 1, LF_SX, LF_ROW);
    hid = lf_spawn(LF_TARGET, 2, LF_SX + 400, LF_ROW);
    ASSERT(a >= 0 && hid >= 0);
    Units_CommandAttackUnit(a, hid);
    ASSERT_EQ_INT(hid, lf_unit(a)->target);
    ticks = 0;
    while (lf_unit(a)->target == hid && ticks < 600) {
        Units_TickEngines();
        ticks++;
    }
    ASSERT(lf_unit(a)->target != hid);
    ASSERT_EQ_INT(0, Units_CanAttackTarget(a, hid));
    lf_end();
}

/* D-025 never keeps a unit from answering what it passed over: a hit
 * from the skipped unit ends the skip, and so does either one moving
 * more than 32 px from where it stood. */
TEST(a_skip_ends_when_the_skipped_unit_strikes_or_either_moves) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 40);
    int a = lf_spawn(LF_QUICK, 1, LF_SX, LF_ROW);
    int mage = lf_spawn(LF_SPELL, 2, LF_SX + 400, LF_ROW);
    ASSERT(a >= 0 && mage >= 0);
    Units_DebugSetAggro(a, UNIT_AGGRO_OFFENSIVE);
    for (int i = 0; i < 600 && lf_unit(a)->skip_id == 0; i++)
        Units_TickEngines();
    ASSERT_EQ_INT(0, Units_CanAttackTarget(a, mage));
    /* The spell passes the ridge. */
    LfShot r = lf_fire(mage, a, 300);
    ASSERT(r.fired);
    ASSERT(lf_unit(a)->health < 300);
    ASSERT_EQ_INT(mage, lf_unit(a)->target);
    ASSERT_EQ_INT(1, Units_CanAttackTarget(a, mage));
    lf_end();

    w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_ridge(w, 40);
    a = lf_spawn(LF_QUICK, 1, LF_SX, LF_ROW);
    int hid = lf_spawn(LF_TARGET, 2, LF_SX + 400, LF_ROW);
    ASSERT(a >= 0 && hid >= 0);
    Units_DebugSetAggro(a, UNIT_AGGRO_OFFENSIVE);
    for (int i = 0; i < 600 && lf_unit(a)->skip_id == 0; i++)
        Units_TickEngines();
    ASSERT_EQ_INT(0, Units_CanAttackTarget(a, hid));
    ASSERT(Units_OrderMove(hid, LF_SX + 400, LF_ROW + 200));
    int ticks = 0;
    while (lf_unit(hid)->world_y < LF_ROW + 20 && ticks < 300) {
        Units_TickEngines();
        ticks++;
    }
    ASSERT_EQ_INT(0, Units_CanAttackTarget(a, hid));
    while (lf_unit(hid)->world_y < LF_ROW + 40 && ticks < 300) {
        Units_TickEngines();
        ticks++;
    }
    printf("(moved 40 px in %d ticks) ", ticks);
    ASSERT_EQ_INT(1, Units_CanAttackTarget(a, hid));
    lf_end();
}

/* A mission script's attack order holds like a player's whoever owns
 * the unit, so a computer's army sent at a unit keeps firing. */
TEST(a_mission_scripts_attack_order_is_never_let_go) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.players[0].kind = TAK_SLOT_AI;
    lf_ridge(w, 40);
    int a = lf_spawn(LF_QUICK, 1, LF_SX, LF_ROW);
    int hid = lf_spawn(LF_TARGET, 2, LF_SX + 400, LF_ROW);
    ASSERT(a >= 0 && hid >= 0);
    ASSERT(Units_OrderAttackHeld(a, hid));
    for (int i = 0; i < 600; i++) Units_TickEngines();
    ASSERT_EQ_INT(hid, lf_unit(a)->target);
    ASSERT_EQ_INT(0, (int)lf_unit(a)->skip_id);
    ASSERT_EQ_INT(1000, lf_unit(hid)->health);
    lf_end();
}

TEST(a_building_is_drawn_when_its_side_can_take_it) {
    GameWorld *w = lf_world(1, 1);
    ASSERT_NOT_NULL(w);
    /* The spotter lights the keep's west corners, 56 px from its
     * centre, and not the centre itself. */
    int k = lf_spawn(LF_KEEP, 2, 1560, LF_ROW);
    int sp = lf_spawn(LF_SPOTTER, 1, 1290, LF_ROW);
    ASSERT(k >= 0 && sp >= 0);
    lf_fog(w);
    ASSERT_EQ_INT(0, Fog_ShowsAt(w, 1560, LF_ROW));
    ASSERT_EQ_INT(1, Units_SideSees(1, k));
    ASSERT_EQ_INT(1, Units_IsVisibleToLocalPlayer(lf_unit(k)));
    Units_DebugRemove(sp);
    sp = lf_spawn(LF_SPOTTER, 1, 1250, LF_ROW);
    ASSERT(sp >= 0);
    lf_fog(w);
    ASSERT_EQ_INT(0, Units_SideSees(1, k));
    ASSERT_EQ_INT(0, Units_IsVisibleToLocalPlayer(lf_unit(k)));
    lf_end();
}

/* An archer shut in a ring too steep to climb, its target far out of
 * range: the route search gives up, and the attack order is kept and
 * tried again, as the original's attack keeps closing
 * (legacy:246390-246428). */
TEST(an_attack_on_a_target_out_of_reach_is_kept) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->moveinfo.classes[0].max_slope = 30;
    lf_heights(w, 30, 50, 90, 91, 250);
    lf_heights(w, 30, 50, 109, 110, 250);
    lf_heights(w, 30, 31, 90, 110, 250);
    lf_heights(w, 49, 50, 90, 110, 250);
    TAK_PathCacheReset();
    int target = lf_spawn(LF_TARGET, 2, 150 * 16, 100 * 16);
    int archer = lf_spawn(LF_ARCHER, 1, 40 * 16, 100 * 16);
    ASSERT(target >= 0 && archer >= 0);
    ASSERT_EQ_INT(1, Units_OrderAttack(archer, target));
    int dropped = -1;
    for (int t = 0; t < 2400; t++) {
        Units_TickEngines();
        if (dropped < 0 && lf_unit(archer)->cmd_kind != UNIT_CMD_ATTACK) dropped = t;
    }
    printf("(dropped at %d) ", dropped);
    int kind = lf_unit(archer)->cmd_kind, held = lf_unit(archer)->target;
    int ax = lf_unit(archer)->world_x / 16, esc = lf_unit(archer)->stall_esc;
    lf_end();
    printf("(order %d, target %d, at cell %d, rung %d) ", kind, held, ax, esc);
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, kind);
    ASSERT_EQ_INT(target, held);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("An attack out of reach");
    RUN(an_attack_on_a_target_out_of_reach_is_kept);
    TEST_SUITE("The cell test");
    RUN(the_cell_test_meets_a_unit_then_a_feature_then_the_ground);
    RUN(a_feature_leaves_its_cells_when_it_goes);
    RUN(a_feature_that_goes_leaves_the_others_their_cells);
    TEST_SUITE("Shots across a hill");
    RUN(an_arrow_over_open_ground_hits);
    RUN(an_arrow_bursts_on_a_ridge_taller_than_its_arc);
    RUN(a_straight_bolt_bursts_on_a_ridge_and_hits_over_flat_ground);
    RUN(a_fireball_bursts_on_a_ridge);
    RUN(a_unit_right_behind_a_one_cell_crest_is_not_struck_through_it);
    RUN(a_lobbed_rock_passes_over_a_unit_that_walked_under_it);
    RUN(lightning_ends_at_a_rock_in_its_way);
    RUN(a_flame_stops_at_a_ridge_and_reaches_over_open_ground);
    TEST_SUITE("Shots through a wall");
    RUN(an_enemy_wall_takes_the_arrow);
    RUN(the_shooters_own_wall_lets_its_arrow_through);
    RUN(an_allied_wall_stops_the_arrow_and_takes_nothing);
    RUN(a_bolt_and_a_fireball_through_an_enemy_wall_hit_the_wall);
    RUN(lightning_through_an_enemy_wall_strikes_the_wall);
    RUN(a_catapult_throws_over_a_wall);
    TEST_SUITE("What passes");
    RUN(a_units_only_shot_passes_a_ridge);
    RUN(a_water_weapon_passes_under_the_sea_and_a_bolt_does_not);
    RUN(a_shot_whose_target_dies_flies_on_and_lands);
    RUN(a_bolt_whose_target_dies_flies_past_its_aim);
    RUN(lightning_with_the_pool_full_still_meets_the_rock);
    RUN(a_beam_with_the_pool_full_strikes_what_is_in_its_way);
    RUN(a_remote_spell_behind_a_ridge_still_lands);
    RUN(a_shot_at_the_shooters_feet_leaves_the_shooter_whole);
    TEST_SUITE("State hash");
    RUN(a_blocked_volley_hashes_the_same);
    TEST_SUITE("What a side sees");
    RUN(a_ranged_unit_waits_to_see_before_it_acquires);
    RUN(a_hit_is_answered_and_leaves_its_shooter_unseen);
    RUN(return_fire_needs_the_shooter_in_reach);
    RUN(a_blade_answers_a_seen_shooter_inside_its_leash);
    RUN(a_tower_answers_only_what_it_can_reach);
    RUN(an_ai_monarch_at_work_answers_a_shooter_or_builds_on);
    RUN(a_unit_lets_go_of_a_target_its_shots_cannot_reach);
    RUN(a_skip_ends_when_the_skipped_unit_strikes_or_either_moves);
    RUN(a_mission_scripts_attack_order_is_never_let_go);
    RUN(a_building_is_drawn_when_its_side_can_take_it);
    RUN(a_volley_in_the_fog_hashes_to_its_pin);
    TEST_REPORT();
}
