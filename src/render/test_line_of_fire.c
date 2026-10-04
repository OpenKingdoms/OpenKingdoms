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
#include "test_hpi_builder.h"
#include "tak_battle_config.h"
#include "tak_features.h"
#include "tak_fog.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_savegame.h"
#include "tak_shot_path.h"
#include "tak_terrain.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
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
       LF_SPLASH, LF_POST, LF_BONE, LF_STAFF, LF_BOWBLADE, LF_VETERAN,
       LF_HOLDER, LF_ROVER, LF_PICKER, LF_SEEKER, LF_SEEKFAR, LF_ROAMER,
       LF_FLYPICK, LF_BLADEPICK, LF_BROAD, LF_SWIRL, LF_CANNON, LF_TORCH,
       LF_SWEEPER, LF_BOMBER, LF_STURDY,
       LF_DEF_COUNT };

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

/* The feature defs: index 0 is the rock every older case places. */
enum { FD_ROCK = 0, FD_TREE, FD_TREEDEAD, FD_SMUDGE, FD_TREEBURNT, FD_WALL,
       FD_WALLA, FD_WALLB, FD_LONGTREE, FD_COUNT };

static void lf_feature(FeatureDef *f, const char *name, int fp, int height,
                       int blocking, int damage, const char *dead) {
    memset(f, 0, sizeof(*f));
    strncpy(f->name, name, sizeof(f->name) - 1);
    strncpy(f->filename, "testscenery", sizeof(f->filename) - 1);
    strncpy(f->seqname, name, sizeof(f->seqname) - 1);
    strncpy(f->feature_dead, dead, sizeof(f->feature_dead) - 1);
    f->footprint_x = fp;
    f->footprint_z = fp;
    f->height = height;
    f->blocking = blocking;
    f->damage = damage;
}

static void lf_release_script(void);

static GameWorld *lf_world(int line_of_sight, int fog) {
    lf_release_script();
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

    /* A thrown bone that is a melee weapon by its type alone, reach 50
     * like the goblin's. */
    lf_fill(&defs[LF_BONE], "TESTGOBLIN", 1.2f, 1000);
    lf_weapon(&defs[LF_BONE], "TESTBONE", "Melee", 0, 50, 40);
    /* A staff with no speed and a long reach that is not a melee type. */
    lf_fill(&defs[LF_STAFF], "TESTSTAFF", 1.2f, 1000);
    lf_weapon(&defs[LF_STAFF], "TESTSTAFFW", "", 0, 100, 40);
    /* A shot whose name reads like a blade. */
    lf_fill(&defs[LF_BOWBLADE], "TESTBOWBLD", 1.2f, 1000);
    wp = lf_weapon(&defs[LF_BOWBLADE], "TESTSWORDBOW", "Ballistic", 450, 300, 40);
    wp->is_gravity = 1;
    /* A blade that ranks: one kill of its kind is a level. */
    lf_fill(&defs[LF_VETERAN], "TESTVET", 1.2f, 100000);
    defs[LF_VETERAN].kill_xp_value = 10;
    lf_weapon(&defs[LF_VETERAN], "TESTVETW", "Melee", 0, 40, 100);
    /* An archer that holds position, and one that maneuvers. */
    lf_fill(&defs[LF_HOLDER], "TESTHOLD", 1.2f, 1000);
    defs[LF_HOLDER].has_standing_order = 1;
    defs[LF_HOLDER].standing_order = UNIT_AGGRO_DEFENSIVE;
    defs[LF_HOLDER].leash_length = 500;
    lf_weapon(&defs[LF_HOLDER], "TESTHOLDW", "Line of Sight", 500, 200, 1);
    lf_fill(&defs[LF_ROVER], "TESTROVE", 1.2f, 1000);
    defs[LF_ROVER].has_standing_order = 1;
    defs[LF_ROVER].standing_order = UNIT_AGGRO_OFFENSIVE;
    defs[LF_ROVER].leash_length = 500;
    lf_weapon(&defs[LF_ROVER], "TESTROVEW", "Line of Sight", 500, 200, 1);
    /* An archer that draws its targets at random and barely scratches. */
    lf_fill(&defs[LF_PICKER], "TESTPICK", 1.2f, 1000);
    defs[LF_PICKER].fire_at_will_random = 1;
    wp = lf_weapon(&defs[LF_PICKER], "TESTPICKW", "Line of Sight", 500, 400, 1);
    wp->reload_ticks = 30;
    /* The random picker with canfly, and a melee one that sees 250. */
    defs[LF_FLYPICK] = defs[LF_PICKER];
    strncpy(defs[LF_FLYPICK].unitname, "TESTFLYPICK", sizeof(defs[LF_FLYPICK].unitname) - 1);
    defs[LF_FLYPICK].can_fly = 1;
    lf_fill(&defs[LF_BLADEPICK], "TESTBLADEPICK", 1.2f, 1000);
    defs[LF_BLADEPICK].fire_at_will_random = 1;
    wp = lf_weapon(&defs[LF_BLADEPICK], "TESTBLADEPW", "Melee", 0, 40, 1);
    wp->reload_ticks = 30;
    /* Maneuvering archers that take the nearest, one on a short leash,
     * one on a long one, and one that roams. They creep, so the dummy
     * behind stays the nearer for the whole run. */
    lf_fill(&defs[LF_SEEKER], "TESTSEEK", 0.02f, 1000);
    defs[LF_SEEKER].sight_distance = 700;
    wp = lf_weapon(&defs[LF_SEEKER], "TESTSEEKW", "Line of Sight", 500, 200, 1);
    wp->reload_ticks = 30;
    defs[LF_SEEKFAR] = defs[LF_SEEKER];
    strncpy(defs[LF_SEEKFAR].unitname, "TESTSEEKFAR", sizeof(defs[LF_SEEKFAR].unitname) - 1);
    defs[LF_SEEKFAR].leash_length = 1000;
    defs[LF_ROAMER] = defs[LF_SEEKER];
    strncpy(defs[LF_ROAMER].unitname, "TESTROAM", sizeof(defs[LF_ROAMER].unitname) - 1);
    defs[LF_ROAMER].roams = 1;

    /* A dummy whose model reaches 24 px either side of its centre. */
    lf_fill(&defs[LF_BROAD], "TESTBROAD", 1.2f, 1000);
    defs[LF_BROAD].body_min_x_px = -24;
    defs[LF_BROAD].body_max_x_px = 24;
    /* A bolt whose areaofeffect is under 17, the Fire Swirl's 10. */
    lf_fill(&defs[LF_SWIRL], "TESTSWIRL", 1.2f, 300);
    wp = lf_weapon(&defs[LF_SWIRL], "TESTSWIRLW", "Line of Sight", 400, 550, 40);
    wp->area_of_effect = 10;
    wp->edge_effectiveness = 1.0f;
    /* A cannoneer's ball, a fire mage's flame and a sweeping wave. */
    lf_fill(&defs[LF_CANNON], "TESTCANNON", 1.2f, 300);
    wp = lf_weapon(&defs[LF_CANNON], "TESTCANNONW", "Ballistic", 300, 550, 2000);
    wp->area_of_effect = 90;
    lf_fill(&defs[LF_TORCH], "TESTTORCH", 1.2f, 300);
    wp = lf_weapon(&defs[LF_TORCH], "TESTTORCHW", "Line of Sight", 400, 550, 50);
    wp->area_of_effect = 32;
    wp->fire_starter = 1;
    lf_fill(&defs[LF_SWEEPER], "TESTSWEEP", 1.2f, 300);
    wp = lf_weapon(&defs[LF_SWEEPER], "TESTSWEEPW", "Remote Effect", 400, 550, 2000);
    wp->area_of_effect = 90;
    wp->units_only = 1;

    /* A unit that bursts as it dies, a cannon ball's 2000 over 96 px of
     * area with no falloff, and a sandbag that soaks blasts. */
    lf_fill(&defs[LF_BOMBER], "TESTBOMBER", 1.2f, 30);
    wp = lf_weapon(&defs[LF_BOMBER], "TESTDIE", "Ballistic", 300, 0, 2000);
    wp->area_of_effect = 96;
    wp->edge_effectiveness = 1.0f;
    defs[LF_BOMBER].death_weapons[0] = *wp;
    defs[LF_BOMBER].death_weapon_set = 1;
    defs[LF_BOMBER].num_weapons = 0;
    lf_fill(&defs[LF_STURDY], "TESTSTURDY", 1.2f, 100000);

    FeatureDef fdefs[FD_COUNT];
    memset(fdefs, 0, sizeof fdefs);
    lf_feature(&fdefs[FD_ROCK], "TESTROCK", 2, 60, 1, 3000, "");
    fdefs[FD_ROCK].indestructible = 1;
    strncpy(fdefs[FD_ROCK].category, "rocks", sizeof(fdefs[FD_ROCK].category) - 1);
    lf_feature(&fdefs[FD_TREE], "TESTTREE", 1, 100, 1, 1000, "TESTTREEDEAD");
    strncpy(fdefs[FD_TREE].feature_burnt, "TESTTREEBURNT", 39);
    fdefs[FD_TREE].flamable = 1;
    fdefs[FD_TREE].spread_chance = 100;
    fdefs[FD_TREE].spark_time = 150;
    lf_feature(&fdefs[FD_TREEDEAD], "TESTTREEDEAD", 1, 60, 1, 500, "TESTSMUDGE");
    lf_feature(&fdefs[FD_SMUDGE], "TESTSMUDGE", 1, 0, 0, 0, "");
    fdefs[FD_SMUDGE].indestructible = 1;
    lf_feature(&fdefs[FD_TREEBURNT], "TESTTREEBURNT", 1, 60, 1, 200, "TESTSMUDGE");
    lf_feature(&fdefs[FD_WALL], "TESTWALL", 2, 60, 1, 12000, "TESTWALLA");
    lf_feature(&fdefs[FD_WALLA], "TESTWALLA", 2, 30, 1, 9000, "TESTWALLB");
    lf_feature(&fdefs[FD_WALLB], "TESTWALLB", 2, 0, 0, 2000, "");
    fdefs[FD_WALLB].indestructible = 1;
    for (int k = FD_WALL; k <= FD_WALLB; k++)
        strncpy(fdefs[k].category, "walls", sizeof(fdefs[k].category) - 1);
    /* A tree whose burn outlasts its spark, as a mod could author. */
    fdefs[FD_LONGTREE] = fdefs[FD_TREE];
    strncpy(fdefs[FD_LONGTREE].name, "TESTLONGTREE", 39);
    if (Features_DebugSetDefs(fdefs, FD_COUNT) != FD_COUNT) return NULL;
    /* The original's tree: a death of 10 pictures and a burn of 37, two
     * frames each, its flames 30 and 31 (AraTree01, mediflame, megaflame). */
    Features_DebugSetSequence(FD_TREE, 0, 10, 2);
    Features_DebugSetSequence(FD_TREE, 1, 37, 2);
    Features_DebugSetSequence(FD_TREE, 2, 30, 2);
    Features_DebugSetSequence(FD_TREE, 3, 31, 2);
    Features_DebugSetSequence(FD_TREEDEAD, 0, 10, 2);
    Features_DebugSetSequence(FD_WALL, 0, 10, 2);
    Features_DebugSetSequence(FD_WALLA, 0, 10, 2);
    Features_DebugSetSequence(FD_LONGTREE, 1, 200, 2);
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

/* ── blasts ────────────────────────────────────────────────────────── */

/* The original's falloff: half the areaofeffect, all of it at the
 * centre, edge + (1 - edge) * (d / r - 1)^2 out to the radius
 * (legacy:245089, 245213-245217). A catapult's ball and a cannon's. */
TEST(a_blast_falls_off_like_the_originals) {
    ASSERT_EQ_INT(1250, Units_ComputeSplashDamage(1250, 100, 0.1f, 0));
    ASSERT_EQ_INT(845, Units_ComputeSplashDamage(1250, 100, 0.1f, 10));
    ASSERT_EQ_INT(406, Units_ComputeSplashDamage(1250, 100, 0.1f, 25));
    ASSERT_EQ_INT(125, Units_ComputeSplashDamage(1250, 100, 0.1f, 49));
    ASSERT_EQ_INT(0, Units_ComputeSplashDamage(1250, 100, 0.1f, 50));
    ASSERT_EQ_INT(1209, Units_ComputeSplashDamage(2000, 90, 0.0f, 10));
    ASSERT_EQ_INT(0, Units_ComputeSplashDamage(2000, 90, 0.0f, 45));
}

/* A blast with an areaofeffect of 48 reaches 24 px, measured to the
 * side of each model (legacy:245089, 245164-245209). */
TEST(a_blast_reaches_half_its_area_to_the_side_of_a_unit) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_SPLASH, 1, LF_SX, LF_ROW);
    int near = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW + 16);
    int far = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW - 32);
    int broad = lf_spawn(LF_BROAD, 2, LF_TX + 44, LF_ROW);
    ASSERT(s >= 0 && near >= 0 && far >= 0 && broad >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugBlastAt(s, 0, LF_TX, LF_ROW));
    ASSERT_EQ_INT(960, lf_unit(near)->health);
    ASSERT_EQ_INT(1000, lf_unit(far)->health);
    ASSERT_EQ_INT(960, lf_unit(broad)->health);
    lf_end();
}

/* A shot whose areaofeffect is under 17 that strikes a unit hits that
 * unit alone (legacy:245029). */
TEST(a_small_blast_on_a_unit_hits_it_alone) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_SWIRL, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    int by = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW + 6);
    ASSERT(s >= 0 && t >= 0 && by >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireAt(s, 0, t));
    for (int i = 0; i < 150; i++) Units_TickEngines();
    ASSERT_EQ_INT(960, lf_unit(t)->health);
    ASSERT_EQ_INT(1000, lf_unit(by)->health);
    lf_end();
}

/* ── scenery ───────────────────────────────────────────────────────── */

static int lf_place(GameWorld *w, int def, int cx, int cz) {
    const FeatureDef *fd = Features_GetByIndex(def);
    int fp = fd && fd->footprint_x > 0 ? fd->footprint_x : 1;
    return Features_AddInstance(w, def, cx, cz, cx * 16 + fp * 8, cz * 16 + fp * 8, 0, -1);
}

static void lf_ticks(int n) {
    for (int i = 0; i < n; i++) Units_TickEngines();
}

/* A cannoneer's 2000 fells a tree of 1000. Its death plays ten pictures
 * of two of the original's frames, then the dead tree that still blocks
 * takes its cell, and a second ball leaves the smudge that does not
 * (legacy:128781-128789, 127838-127955). */
TEST(a_cannon_fells_a_tree_to_its_stump) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW);
    int t = lf_place(w, FD_TREE, 90, 90);
    ASSERT(s >= 0 && t >= 0);
    int32_t x = 90 * 16 + 8, y = 90 * 16 + 8;
    ASSERT_EQ_INT(0, Terrain_IsWalkable(w, x, y, 255));
    lf_ticks(4);
    ASSERT(Units_DebugBlastAt(s, 0, x, y));
    ASSERT_EQ_INT(FEATURE_FX_DYING, w->features[t].fx);
    lf_ticks(38);
    ASSERT_EQ_INT(FD_TREE, w->features[t].global_idx);
    lf_ticks(2);
    ASSERT_EQ_INT(FD_TREEDEAD, w->features[t].global_idx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[t].fx);
    ASSERT_EQ_INT(0, Terrain_IsWalkable(w, x, y, 255));
    ASSERT(Units_DebugBlastAt(s, 0, x, y));
    lf_ticks(40);
    ASSERT_EQ_INT(FD_SMUDGE, w->features[t].global_idx);
    ASSERT_EQ_INT(1, Terrain_IsWalkable(w, x, y, 255));
    lf_end();
}

/* A wall of 12000 takes six balls to drop to its lower stage, which
 * still blocks, and five more to fall to rubble that does not. The
 * route planner's cached layers are patched to match each stage. */
TEST(a_wall_drops_a_stage_then_to_rubble_that_stops_blocking) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW);
    int wl = lf_place(w, FD_WALL, 90, 90);
    ASSERT(s >= 0 && wl >= 0);
    TAK_PathCacheWarm(w, &w->moveinfo.classes[0], 255, 0, 0);
    int32_t x = 90 * 16 + 16, y = 90 * 16 + 16;
    lf_ticks(4);
    for (int i = 0; i < 5; i++) ASSERT(Units_DebugBlastAt(s, 0, x, y));
    ASSERT_EQ_INT(10000, w->features[wl].damage_taken);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[wl].fx);
    ASSERT(Units_DebugBlastAt(s, 0, x, y));
    ASSERT_EQ_INT(FEATURE_FX_DYING, w->features[wl].fx);
    lf_ticks(40);
    ASSERT_EQ_INT(FD_WALLA, w->features[wl].global_idx);
    ASSERT_EQ_INT(0, w->features[wl].damage_taken);
    ASSERT_EQ_INT(0, Terrain_IsWalkable(w, x, y, 255));
    ASSERT_EQ_INT(31, Features_TopAt(w, 90, 90));
    for (int i = 0; i < 5; i++) ASSERT(Units_DebugBlastAt(s, 0, x, y));
    lf_ticks(40);
    ASSERT_EQ_INT(FD_WALLB, w->features[wl].global_idx);
    ASSERT_EQ_INT(1, Terrain_IsWalkable(w, x, y, 255));
    ASSERT_EQ_INT(0, TAK_PathDebugCheckCache(w));
    lf_end();
}

/* A forest of trees whose burn outlasts its spark, the wind east at
 * three cells a step. The flame lights the middle tree, whose one spark
 * comes 75 to 149 frames later and walks downwind, so the trees six and
 * nine cells east catch and the ones west never do
 * (legacy:127801, 128021-128088). */
static int32_t lf_forest(GameWorld *w, int *mid, int *east6, int *east9,
                         int *west6, int *west9) {
    *mid = lf_place(w, FD_LONGTREE, 100, 100);
    *east6 = lf_place(w, FD_LONGTREE, 106, 100);
    *east9 = lf_place(w, FD_LONGTREE, 109, 100);
    *west6 = lf_place(w, FD_LONGTREE, 94, 100);
    *west9 = lf_place(w, FD_LONGTREE, 91, 100);
    Features_DebugSetWind(w, 98304, 0x4000);
    w->wind_next_frame = 0xFFFFFF00u;
    return w->wind_x;
}

TEST(a_firestarter_lights_a_forest_that_spreads_downwind_not_upwind) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
    ASSERT(s >= 0);
    int mid, e6, e9, w6, w9;
    ASSERT_EQ_INT(98304, lf_forest(w, &mid, &e6, &e9, &w6, &w9));
    ASSERT_EQ_INT(0, w->wind_z);
    lf_ticks(4);
    ASSERT(Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8));
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[mid].fx);
    ASSERT_EQ_INT(0, w->features[mid].damage_taken);
    int spark = w->features[mid].spark;
    ASSERT(spark >= 75 && spark <= 149);
    lf_ticks(2 * (spark - 1));
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[e6].fx);
    lf_ticks(2);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[e6].fx);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[e9].fx);
    lf_ticks(1200);
    ASSERT_EQ_INT(FD_LONGTREE, w->features[w6].global_idx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[w6].fx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[w9].fx);
    ASSERT_EQ_INT(FD_TREEBURNT, w->features[mid].global_idx);
    ASSERT_EQ_INT(FD_TREEBURNT, w->features[e6].global_idx);
    lf_end();
}

/* The shipped trees burn for as long as their flames, 62 frames, and
 * their spark is 75 frames away at the soonest, so a fire stops at the
 * trees the flame itself reached, as the original's does. */
TEST(a_shipped_tree_burns_out_before_its_spark) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
    int t = lf_place(w, FD_TREE, 100, 100);
    int n = lf_place(w, FD_TREE, 102, 100);
    ASSERT(s >= 0 && t >= 0 && n >= 0);
    ASSERT_EQ_INT(74, Features_SequenceFrames(FD_TREE, 1));
    lf_ticks(4);
    ASSERT(Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8));
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[t].fx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[n].fx);
    lf_ticks(122);
    ASSERT_EQ_INT(FD_TREE, w->features[t].global_idx);
    lf_ticks(2);
    ASSERT_EQ_INT(FD_TREEBURNT, w->features[t].global_idx);
    lf_ticks(400);
    ASSERT_EQ_INT(FD_TREE, w->features[n].global_idx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[n].fx);
    lf_end();
}

/* Under the remastered rules the shipped tree's fire burns on past its
 * 62 frames until its spark, 93 to 186 frames in, and the spark lights
 * the tree beside it, which the original's never reaches (D-036). */
TEST(a_remastered_fire_burns_until_its_spark_and_spreads) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.remastered = 1;
    int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
    int t = lf_place(w, FD_TREE, 100, 100);
    int n = lf_place(w, FD_TREE, 102, 100);
    ASSERT(s >= 0 && t >= 0 && n >= 0);
    lf_ticks(4);
    ASSERT(Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8));
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[t].fx);
    int spark = w->features[t].spark;
    ASSERT(spark >= 93 && spark <= 186);
    lf_ticks(126);
    ASSERT_EQ_INT(FD_TREE, w->features[t].global_idx);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[t].fx);
    ASSERT(w->features[t].front_on || w->features[t].back_on);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[n].fx);
    lf_ticks(2 * spark + 2 - 126);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[n].fx);
    lf_ticks(2000);
    ASSERT_EQ_INT(FD_TREEBURNT, w->features[t].global_idx);
    ASSERT_EQ_INT(FD_TREEBURNT, w->features[n].global_idx);
    lf_end();
}

/* A remastered forest fire saved while it spreads runs on the same
 * after the load. */
TEST(a_remastered_forest_fire_saved_and_loaded_runs_on_the_same) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.remastered = 1;
    int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
    ASSERT(s >= 0);
    int lit = -1;
    for (int z = 0; z < 6; z++)
        for (int x = 0; x < 12; x++) {
            int f = lf_place(w, FD_TREE, 100 + 2 * x, 100 + 2 * z);
            ASSERT(f >= 0);
            if (x == 0 && z == 0) lit = f;
        }
    lf_ticks(4);
    ASSERT(Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8));
    lf_ticks(400);
    /* The fire has spread past the tree the flame lit. */
    int spread = 0;
    for (int i = 0; i < w->feature_count; i++)
        spread += i != lit && w->features[i].fx == FEATURE_FX_BURNING;
    ASSERT(spread >= 1);
    char err[256] = { 0 };
    const char *path = "lf_remaster_fire.oksave";
    remove(path);
    ASSERT_EQ_INT(0, Save_Write(path, err, sizeof err));
    uint32_t at_save = TAK_SimHash();
    lf_ticks(600);
    uint32_t want = TAK_SimHash();
    TAK_SaveGame *sg = Save_Read(path, err, sizeof err);
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof err));
    Save_ReadClose(sg);
    ASSERT_EQ_INT((int)at_save, (int)TAK_SimHash());
    lf_ticks(600);
    ASSERT_EQ_INT((int)want, (int)TAK_SimHash());
    remove(path);
    lf_end();
}

/* ── remastered fire spread (D-036) ───────────────────────────────── */

static void lf_frames(GameWorld *w, int n) {
    for (int i = 0; i < n; i++) Features_TickFrame(w);
}

static int lf_alight(const GameWorld *w, int i) {
    return w->features[i].fx == FEATURE_FX_BURNING || w->features[i].global_idx == FD_TREEBURNT;
}

/* With nothing within three cells a remastered spark reaches a tree six
 * cells off on its first spark, and never one seven off. */
TEST(a_remastered_spark_reaches_a_tree_six_cells_off) {
    for (int rules = 0; rules < 2; rules++) {
        GameWorld *w = lf_world(0, 0);
        ASSERT_NOT_NULL(w);
        w->cfg.remastered = rules;
        int a = lf_place(w, FD_TREE, 100, 100);
        int six = lf_place(w, FD_TREE, 106, 103);
        int seven = lf_place(w, FD_TREE, 93, 100);
        ASSERT(a >= 0 && six >= 0 && seven >= 0);
        Features_DebugHit(w, a, 50, 1);
        int spark = w->features[a].spark;
        ASSERT(rules ? spark >= 93 && spark <= 186 : spark >= 75 && spark <= 149);
        lf_frames(w, spark - 1);
        ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[six].fx);
        lf_frames(w, 1);
        ASSERT_EQ_INT(rules ? FEATURE_FX_BURNING : FEATURE_FX_NONE, w->features[six].fx);
        lf_frames(w, 1500);
        ASSERT_EQ_INT(rules ? FD_TREEBURNT : FD_TREE, w->features[six].global_idx);
        ASSERT_EQ_INT(FD_TREE, w->features[seven].global_idx);
        ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[seven].fx);
        lf_end();
    }
}

/* A remastered fire throws four sparks, each 93 to 186 frames after the
 * last, and burns until the last. The first lights the tree two cells
 * off, and the second, with nothing left within three, the one five off. */
TEST(a_remastered_fire_throws_four_sparks_and_burns_until_the_last) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.remastered = 1;
    int a = lf_place(w, FD_TREE, 100, 100);
    int near = lf_place(w, FD_TREE, 102, 100);
    int far = lf_place(w, FD_TREE, 95, 100);
    ASSERT(a >= 0 && near >= 0 && far >= 0);
    Features_DebugHit(w, a, 50, 1);
    int first = w->features[a].spark;
    ASSERT(first >= 93 && first <= 186);
    lf_frames(w, first);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[near].fx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[far].fx);
    int second = w->features[a].spark;
    ASSERT(second >= 93 && second <= 186);
    lf_frames(w, second - 1);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[far].fx);
    lf_frames(w, 1);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[far].fx);
    for (int k = 0; k < 2; k++) {
        int next = w->features[a].spark;
        ASSERT(next >= 93 && next <= 186);
        lf_frames(w, next);
        ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[a].fx);
    }
    ASSERT_EQ_INT(0, w->features[a].spark);
    lf_frames(w, 80);
    ASSERT_EQ_INT(FD_TREEBURNT, w->features[a].global_idx);
    lf_end();
}

/* Under the remastered rules a wind of 2000 carries a spark two cells,
 * so the fire reaches trees seven and eight cells downwind. Upwind or
 * calm it reaches neither. */
TEST(a_remastered_wind_carries_the_spark_downwind) {
    static const uint16_t heading[3] = { 0, 0x4000, 0xC000 };
    for (int k = 0; k < 3; k++) {
        GameWorld *w = lf_world(0, 0);
        ASSERT_NOT_NULL(w);
        w->cfg.remastered = 1;
        int a = lf_place(w, FD_TREE, 100, 100);
        int east8 = lf_place(w, FD_TREE, 108, 100);
        int west7 = lf_place(w, FD_TREE, 93, 100);
        int east7 = lf_place(w, FD_TREE, 107, 110);
        int west8 = lf_place(w, FD_TREE, 92, 110);
        int b = lf_place(w, FD_TREE, 100, 110);
        ASSERT(a >= 0 && east8 >= 0 && west7 >= 0 && east7 >= 0 && west8 >= 0 && b >= 0);
        if (k) {
            Features_DebugSetWind(w, 2000, heading[k]);
            w->wind_next_frame = 0xFFFFFF00u;
            ASSERT_EQ_INT(k == 1 ? 2000 : -2000, w->wind_x);
            ASSERT_EQ_INT(0, w->wind_z);
        }
        Features_DebugHit(w, a, 50, 1);
        Features_DebugHit(w, b, 50, 1);
        lf_frames(w, 2000);
        ASSERT_EQ_INT(k == 1, lf_alight(w, east7));
        ASSERT_EQ_INT(k == 1, lf_alight(w, east8));
        ASSERT_EQ_INT(k == 2, lf_alight(w, west7));
        ASSERT_EQ_INT(k == 2, lf_alight(w, west8));
        lf_end();
    }
}

/* A remastered spark lights a tree upwind at a quarter of the chance,
 * a half for the test tree, which catches at twice 100. Sixteen fires,
 * each with a tree five cells east and five west: under a wind east
 * every first spark lights the east tree and about half light the west
 * one, and calm every one lights both. */
TEST(a_remastered_wind_favours_the_trees_downwind) {
    for (int windy = 0; windy < 2; windy++) {
        GameWorld *w = lf_world(0, 0);
        ASSERT_NOT_NULL(w);
        w->cfg.remastered = 1;
        if (windy) {
            Features_DebugSetWind(w, 2000, 0x4000);
            w->wind_next_frame = 0xFFFFFF00u;
        }
        int src[16], east[16], west[16], done[16] = { 0 };
        for (int k = 0; k < 16; k++) {
            int cx = 30 + 20 * (k % 4), cz = 30 + 20 * (k / 4);
            src[k] = lf_place(w, FD_TREE, cx, cz);
            east[k] = lf_place(w, FD_TREE, cx + 5, cz);
            west[k] = lf_place(w, FD_TREE, cx - 5, cz);
            ASSERT(src[k] >= 0 && east[k] >= 0 && west[k] >= 0);
        }
        int lit_east = 0, lit_west = 0, seen = 0;
        for (int f = 0; f < 600 && seen < 16; f++) {
            /* Lit eight frames apart, so few first sparks share a frame. */
            if (f % 8 == 0 && f / 8 < 16) Features_DebugHit(w, src[f / 8], 50, 1);
            lf_frames(w, 1);
            for (int k = 0; k < 16; k++) {
                const struct MapFeature *mf = &w->features[src[k]];
                if (done[k] || mf->fx != FEATURE_FX_BURNING || mf->sparks == FEATURE_SPARKS - 1)
                    continue;
                done[k] = 1;
                seen++;
                lit_east += w->features[east[k]].fx == FEATURE_FX_BURNING;
                lit_west += w->features[west[k]].fx == FEATURE_FX_BURNING;
            }
        }
        lf_end();
        printf("[%s: %d east and %d west of 16] ", windy ? "wind east" : "calm", lit_east,
               lit_west);
        ASSERT_EQ_INT(16, seen);
        ASSERT_EQ_INT(16, lit_east);
        if (windy) ASSERT(lit_west >= 3 && lit_west <= 12);
        else ASSERT_EQ_INT(16, lit_west);
    }
}

/* A calm remastered fire in a wood with a tree on every cell spreads
 * alike every way. Each spark starts its cells from another corner, so
 * the frame's cap favours no side. Frames to 15 cells east, west, north
 * and south, summed over four fires. */
TEST(a_remastered_calm_fire_spreads_alike_every_way) {
    int sum[4] = { 0, 0, 0, 0 };
    for (int s = 0; s < 4; s++) {
        GameWorld *w = lf_world(0, 0);
        ASSERT_NOT_NULL(w);
        w->cfg.remastered = 1;
        World_SeedRand(500u + 31u * (uint32_t)s);
        int mid = -1;
        for (int z = -20; z <= 20; z++)
            for (int x = -20; x <= 20; x++) {
                int f = lf_place(w, FD_TREE, 96 + x, 96 + z);
                if (x == 0 && z == 0) mid = f;
            }
        ASSERT(mid >= 0);
        Features_DebugHit(w, mid, 50, 1);
        int got[4] = { 0, 0, 0, 0 }, left = 4;
        for (int f = 1; f <= 3000 && left; f++) {
            lf_frames(w, 1);
            for (int i = 0; i < w->feature_count; i++) {
                if (w->features[i].fx != FEATURE_FX_BURNING) continue;
                int dx = w->features[i].tile_x - 96, dz = w->features[i].tile_z - 96;
                int far[4] = { dx, -dx, -dz, dz };
                for (int k = 0; k < 4; k++)
                    if (!got[k] && far[k] >= 15) { got[k] = f; left--; }
            }
        }
        lf_end();
        for (int k = 0; k < 4; k++) {
            ASSERT(got[k] > 0);
            sum[k] += got[k];
        }
    }
    int lo = sum[0], hi = sum[0];
    for (int k = 1; k < 4; k++) {
        if (sum[k] < lo) lo = sum[k];
        if (sum[k] > hi) hi = sum[k];
    }
    printf("[15 cells east %d, west %d, north %d, south %d frames] ", sum[0], sum[1], sum[2],
           sum[3]);
    ASSERT(hi * 100 <= lo * 135);
}

/* No more than four features catch from remastered sparks in one frame.
 * Ten sparks due together light four, four the next frame and two the
 * frame after, the ones held waiting their turn. */
TEST(remastered_sparks_light_no_more_than_four_a_frame) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.remastered = 1;
    int src[10], dst[10];
    for (int k = 0; k < 10; k++) {
        src[k] = lf_place(w, FD_TREE, 40 + 10 * k, 100);
        dst[k] = lf_place(w, FD_TREE, 40 + 10 * k, 102);
        ASSERT(src[k] >= 0 && dst[k] >= 0);
    }
    for (int k = 0; k < 10; k++) {
        Features_DebugHit(w, src[k], 50, 1);
        w->features[src[k]].spark = 1;
    }
    int want[3] = { 4, 8, 10 };
    for (int f = 0; f < 3; f++) {
        lf_frames(w, 1);
        int lit = 0;
        for (int k = 0; k < 10; k++) lit += w->features[dst[k]].fx == FEATURE_FX_BURNING;
        ASSERT_EQ_INT(want[f], lit);
    }
    lf_end();
}

/* A remastered fire crosses a wood with a tree every five cells under a
 * changing wind, which the original's never can. Two runs hash the
 * same, and one saved and loaded part way runs on to the same. */
static GameWorld *lf_sparse_wood(void) {
    GameWorld *w = lf_world(0, 0);
    if (!w) return NULL;
    w->cfg.remastered = 1;
    Features_WindBegin(w, 100, 2000);
    int first = -1;
    for (int z = 0; z < 8; z++)
        for (int x = 0; x < 12; x++) {
            int f = lf_place(w, FD_TREE, 60 + 5 * x, 60 + 5 * z);
            if (first < 0) first = f;
        }
    lf_ticks(4);
    if (first >= 0) Features_DebugHit(w, first, 50, 1);
    return w;
}

static int lf_sparse_wood_lit(const GameWorld *w) {
    int lit = 0;
    for (int i = 0; i < w->feature_count; i++) lit += lf_alight(w, i);
    return lit;
}

TEST(a_remastered_sparse_wood_fire_saved_and_loaded_runs_on_the_same) {
    GameWorld *w = lf_sparse_wood();
    ASSERT_NOT_NULL(w);
    lf_ticks(1200);
    uint32_t first = TAK_SimHash();
    int lit_first = lf_sparse_wood_lit(w);
    lf_end();
    w = lf_sparse_wood();
    ASSERT_NOT_NULL(w);
    lf_ticks(1200);
    ASSERT_EQ_INT((int)first, (int)TAK_SimHash());
    ASSERT(lit_first >= 4);
    char err[256] = { 0 };
    const char *path = "lf_remaster_sparse.oksave";
    remove(path);
    ASSERT_EQ_INT(0, Save_Write(path, err, sizeof err));
    uint32_t at_save = TAK_SimHash();
    lf_ticks(2400);
    uint32_t want = TAK_SimHash();
    int lit = lf_sparse_wood_lit(w);
    TAK_SaveGame *sg = Save_Read(path, err, sizeof err);
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof err));
    Save_ReadClose(sg);
    ASSERT_EQ_INT((int)at_save, (int)TAK_SimHash());
    lf_ticks(2400);
    ASSERT_EQ_INT((int)want, (int)TAK_SimHash());
    ASSERT_EQ_INT(lit, lf_sparse_wood_lit(w));
    ASSERT(lit >= 40);
    remove(path);
    lf_end();
}

/* With the rules off a spark still reaches three cells and no further,
 * and a wind of 2000 carries it nowhere. */
TEST(the_classic_spark_still_reaches_three_cells) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int a = lf_place(w, FD_LONGTREE, 100, 100);
    int three = lf_place(w, FD_LONGTREE, 103, 100);
    int four = lf_place(w, FD_LONGTREE, 96, 100);
    int east5 = lf_place(w, FD_LONGTREE, 108, 100);
    ASSERT(a >= 0 && three >= 0 && four >= 0 && east5 >= 0);
    Features_DebugSetWind(w, 2000, 0x4000);
    w->wind_next_frame = 0xFFFFFF00u;
    Features_DebugHit(w, a, 50, 1);
    lf_frames(w, w->features[a].spark);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[three].fx);
    lf_frames(w, 2000);
    ASSERT_EQ_INT(FD_TREEBURNT, w->features[three].global_idx);
    ASSERT_EQ_INT(FD_LONGTREE, w->features[four].global_idx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[four].fx);
    ASSERT_EQ_INT(FD_LONGTREE, w->features[east5].global_idx);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[east5].fx);
    lf_end();
}

/* A unitsonly weapon leaves scenery alone and still hurts units
 * (legacy:245240), and a bolt with no areaofeffect that lands on the
 * ground hits the feature on that cell (legacy:245281-245297). */
TEST(a_units_only_blast_leaves_scenery_alone) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int sw = lf_spawn(LF_SWEEPER, 1, LF_SX, LF_ROW);
    int ar = lf_spawn(LF_BOLT, 1, LF_SX, LF_ROW + 40);
    int near = lf_spawn(LF_TARGET, 2, 90 * 16 + 8, 90 * 16 + 40);
    int t = lf_place(w, FD_TREE, 90, 90);
    int u = lf_place(w, FD_TREE, 80, 90);
    ASSERT(sw >= 0 && ar >= 0 && near >= 0 && t >= 0 && u >= 0);
    lf_ticks(4);
    ASSERT(Units_DebugBlastAt(sw, 0, 90 * 16 + 8, 90 * 16 + 8));
    ASSERT_EQ_INT(0, w->features[t].damage_taken);
    ASSERT_EQ_INT(FEATURE_FX_NONE, w->features[t].fx);
    ASSERT(lf_unit(near)->health < 1000);
    ASSERT(Units_DebugBlastAt(ar, 0, 80 * 16 + 8, 90 * 16 + 8));
    ASSERT_EQ_INT(40, w->features[u].damage_taken);
    lf_end();
}

/* Saved part way through the fire and loaded back, the battle runs on
 * to the same state as if it had never stopped. */
TEST(a_fire_saved_and_loaded_runs_on_the_same) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
    ASSERT(s >= 0);
    int mid, e6, e9, w6, w9;
    lf_forest(w, &mid, &e6, &e9, &w6, &w9);
    lf_ticks(4);
    ASSERT(Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8));
    lf_ticks(2 * w->features[mid].spark + 40);
    ASSERT_EQ_INT(FEATURE_FX_BURNING, w->features[e6].fx);
    char err[256] = { 0 };
    const char *path = "lf_fire_save.oksave";
    remove(path);
    ASSERT_EQ_INT(0, Save_Write(path, err, sizeof err));
    uint32_t at_save = TAK_SimHash();
    lf_ticks(300);
    uint32_t want = TAK_SimHash();
    TAK_SaveGame *sg = Save_Read(path, err, sizeof err);
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof err));
    Save_ReadClose(sg);
    ASSERT_EQ_INT((int)at_save, (int)TAK_SimHash());
    lf_ticks(300);
    ASSERT_EQ_INT((int)want, (int)TAK_SimHash());
    remove(path);
    lf_end();
}

/* Two runs of the same fire and the same wall hash the same. */
static uint32_t lf_fire_hash(int ticks) {
    GameWorld *w = lf_world(0, 0);
    if (!w) return 0;
    int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
    int c = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW + 40);
    int mid, e6, e9, w6, w9;
    lf_forest(w, &mid, &e6, &e9, &w6, &w9);
    lf_place(w, FD_WALL, 120, 120);
    lf_ticks(4);
    Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8);
    for (int i = 0; i < 7; i++) Units_DebugBlastAt(c, 0, 120 * 16 + 16, 120 * 16 + 16);
    lf_ticks(ticks);
    uint32_t h = TAK_SimHash();
    lf_end();
    return h;
}

TEST(two_fires_hash_the_same) {
    uint32_t a = lf_fire_hash(500);
    uint32_t b = lf_fire_hash(500);
    ASSERT(a != 0);
    ASSERT_EQ_INT((int)a, (int)b);
    ASSERT(lf_fire_hash(502) != a);
}

/* ── death blasts ─────────────────────────────────────────────────── */

#define LF_BLASTS 64
static UnitsBlast g_lf_blast[LF_BLASTS];
static int g_lf_blasts;

static void lf_note_blast(const UnitsBlast *b) {
    if (g_lf_blasts < LF_BLASTS) g_lf_blast[g_lf_blasts] = *b;
    g_lf_blasts++;
}

static void lf_listen(void) {
    g_lf_blasts = 0;
    Units_SetBlastHook(lf_note_blast);
}

/* A unit struck down bursts its death weapon where it fell, a blast on
 * the ground that spares nobody: its own side, an ally and the foe all
 * take the 2000, the tree beside it falls, and what it kills counts for
 * its side (legacy:227346-227349, 245709-245739). */
TEST(a_death_weapon_bursts_on_friend_and_foe_where_its_unit_falls) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    /* All but the foe behind it stand off the bolt's line. */
    int bomber = lf_spawn(LF_BOMBER, 1, LF_TX, LF_ROW);
    int foe = lf_spawn(LF_STURDY, 2, LF_TX + 32, LF_ROW);
    int own = lf_spawn(LF_STURDY, 1, LF_TX - 16, LF_ROW - 32);
    int ally = lf_spawn(LF_STURDY, 3, LF_TX, LF_ROW + 32);
    int weak = lf_spawn(LF_SPOTTER, 2, LF_TX - 24, LF_ROW + 24);
    int far = lf_spawn(LF_STURDY, 2, LF_TX + 200, LF_ROW);
    int shooter = lf_spawn(LF_BOLT, 2, LF_SX, LF_ROW);
    int t = lf_place(w, FD_TREE, LF_TX / 16 + 1, LF_ROW / 16 - 2);
    ASSERT(bomber >= 0 && foe >= 0 && own >= 0 && ally >= 0 && weak >= 0);
    ASSERT(far >= 0 && shooter >= 0 && t >= 0);
    int32_t bx = lf_unit(bomber)->world_x, by = lf_unit(bomber)->world_y;
    lf_listen();
    LfShot r = lf_fire(shooter, bomber, 300);
    Units_SetBlastHook(NULL);
    ASSERT(r.fired);
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, lf_unit(bomber)->alive);
    ASSERT_EQ_INT(98000, lf_unit(foe)->health);
    ASSERT_EQ_INT(98000, lf_unit(own)->health);
    ASSERT_EQ_INT(98000, lf_unit(ally)->health);
    ASSERT_EQ_INT(100000, lf_unit(far)->health);
    ASSERT(lf_unit(weak)->alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(1, (int)w->stats[1].kills);
    ASSERT_EQ_INT(1, (int)w->stats[2].kills);
    ASSERT_EQ_INT(FEATURE_FX_DYING, w->features[t].fx);
    /* The bolt, then the death: the owner's, from no weapon slot. */
    ASSERT_EQ_INT(2, g_lf_blasts);
    const UnitsBlast *b = &g_lf_blast[1];
    ASSERT_EQ_INT(UNITS_BLAST_SLOT_DEATH, b->slot);
    ASSERT_EQ_INT(LF_BOMBER, b->def);
    ASSERT_EQ_INT(bomber, b->shooter);
    ASSERT_EQ_INT(-1, b->struck);
    ASSERT_EQ_INT(1, b->player);
    ASSERT_EQ_INT(96, b->area_of_effect);
    ASSERT_EQ_INT(2000, b->damage);
    ASSERT_EQ_INT(bx, b->x);
    ASSERT_EQ_INT(by, b->y);
    lf_end();
}

/* A frame still being built and a side taken off the map go without a
 * blast: the original bursts only a finished unit that was struck down
 * (legacy:227346, 227119-227144). */
TEST(a_frame_or_a_side_taken_off_never_bursts) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int frame = lf_spawn(LF_BOMBER, 1, LF_TX, LF_ROW);
    int near_frame = lf_spawn(LF_STURDY, 2, LF_TX + 32, LF_ROW);
    int gone = lf_spawn(LF_BOMBER, 3, LF_TX, LF_ROW + 400);
    int near_gone = lf_spawn(LF_STURDY, 2, LF_TX + 32, LF_ROW + 400);
    int shooter = lf_spawn(LF_BOLT, 2, LF_SX, LF_ROW);
    ASSERT(frame >= 0 && near_frame >= 0 && gone >= 0 && near_gone >= 0 && shooter >= 0);
    ((Unit *)lf_unit(frame))->under_construction = 1;
    lf_listen();
    LfShot r = lf_fire(shooter, frame, 300);
    Units_KillAllOf(3);
    lf_ticks(4);
    Units_SetBlastHook(NULL);
    ASSERT(r.fired);
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, lf_unit(frame)->alive);
    ASSERT(lf_unit(gone)->alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(100000, lf_unit(near_frame)->health);
    ASSERT_EQ_INT(100000, lf_unit(near_gone)->health);
    ASSERT_EQ_INT(1, g_lf_blasts);
    lf_end();
}

/* A Killed that sleeps half a second and returns, so a unit dies under
 * its script and owes its blast for that long. */
static uint32_t  g_lf_die_code[] = { 0x10021001u, 500u, 0x10013000u, 0x10065000u };
static uint32_t  g_lf_die_offsets[1];
static char      g_lf_killed[] = "Killed";
static char      g_lf_base[] = "base";
static char     *g_lf_die_names[1] = { g_lf_killed };
static char     *g_lf_die_pieces[1] = { g_lf_base };
static CobScript g_lf_die_script;
/* A one piece model, which a script needs to be bound again on a load. */
static float    g_lf_pos[9] = { 0, 0, 0, 8, 0, 0, 0, 0, 8 };
static float    g_lf_uv[6];
static uint32_t g_lf_col[3] = { 0xff808080u, 0xff808080u, 0xff808080u };
static uint16_t g_lf_idx[3] = { 0, 1, 2 };
static uint16_t g_lf_node[3];
static uint32_t g_lf_seq[1];
static UnitMesh g_lf_mesh;

static void lf_dying_script(int def, int on) {
    memset(&g_lf_mesh, 0, sizeof g_lf_mesh);
    g_lf_mesh.positions = g_lf_pos;
    g_lf_mesh.uvs = g_lf_uv;
    g_lf_mesh.colors = g_lf_col;
    g_lf_mesh.indices = g_lf_idx;
    g_lf_mesh.vert_node_idx = g_lf_node;
    g_lf_mesh.tri_seq = g_lf_seq;
    g_lf_mesh.vert_count = 3;
    g_lf_mesh.tri_count = 1;
    g_lf_mesh.node_count = 1;
    g_lf_mesh.nodes[0].parent = -1;
    strncpy(g_lf_mesh.nodes[0].name, "base", sizeof g_lf_mesh.nodes[0].name - 1);
    memset(&g_lf_die_script, 0, sizeof g_lf_die_script);
    g_lf_die_script.version = 6;
    g_lf_die_script.code = g_lf_die_code;
    g_lf_die_script.num_code_words = 4;
    g_lf_die_script.script_names = g_lf_die_names;
    g_lf_die_script.script_offsets = g_lf_die_offsets;
    g_lf_die_script.num_scripts = 1;
    g_lf_die_script.num_pieces = 1;
    g_lf_die_script.piece_names = g_lf_die_pieces;
    UnitDef *d = (UnitDef *)Units_GetDef(def);
    d->cob_script = on ? &g_lf_die_script : NULL;
    for (int c = 0; c < 12; c++) d->mesh_per_color[c] = on ? &g_lf_mesh : NULL;
}

/* The defs own what they point at, so a case that failed with the
 * script still lent must not leave it for the next one to free. */
static void lf_release_script(void) {
    for (int k = 0; k < LF_DEF_COUNT; k++) {
        UnitDef *d = (UnitDef *)Units_GetDef(k);
        if (!d || d->cob_script != &g_lf_die_script) continue;
        d->cob_script = NULL;
        for (int c = 0; c < 12; c++) d->mesh_per_color[c] = NULL;
    }
}

/* A unit dying under its script bursts as its death ends, and one saved
 * while it dies still owes the blast after the load: both runs hurt the
 * foe the same and hash the same. */
TEST(a_death_blast_owed_comes_back_with_a_save) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_dying_script(LF_BOMBER, 1);
    int bomber = lf_spawn(LF_BOMBER, 1, LF_TX, LF_ROW);
    int foe = lf_spawn(LF_STURDY, 2, LF_TX + 32, LF_ROW);
    ASSERT(bomber >= 0 && foe >= 0);
    lf_ticks(4);
    ASSERT_EQ_INT(bomber, Units_DebugKillHandle(bomber));
    lf_ticks(2);
    ASSERT_EQ_INT(UNIT_ALIVE_DYING, lf_unit(bomber)->alive);
    ASSERT_EQ_INT(100000, lf_unit(foe)->health);
    char err[256] = { 0 };
    const char *path = "lf_death_save.oksave";
    remove(path);
    ASSERT_EQ_INT(0, Save_Write(path, err, sizeof err));
    uint32_t at_save = TAK_SimHash();
    lf_ticks(60);
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, lf_unit(bomber)->alive);
    ASSERT_EQ_INT(98000, lf_unit(foe)->health);
    uint32_t want = TAK_SimHash();
    TAK_SaveGame *sg = Save_Read(path, err, sizeof err);
    ASSERT_NOT_NULL(sg);
    int applied = Save_Apply(sg, err, sizeof err);
    Save_ReadClose(sg);
    if (applied != 0) printf("(%s) ", err);
    ASSERT_EQ_INT(0, applied);
    ASSERT_EQ_INT((int)at_save, (int)TAK_SimHash());
    ASSERT_EQ_INT(UNIT_ALIVE_DYING, lf_unit(bomber)->alive);
    lf_ticks(60);
    ASSERT_EQ_INT(98000, lf_unit(foe)->health);
    ASSERT_EQ_INT((int)want, (int)TAK_SimHash());
    remove(path);
    lf_dying_script(LF_BOMBER, 0);
    lf_end();
}

/* A unit still dying when its monarch falls and its side is taken off
 * the map bursts then, as the original's removal of a dying unit does
 * (legacy:227574). */
TEST(a_unit_dying_as_its_side_is_taken_off_still_bursts) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_dying_script(LF_BOMBER, 1);
    lf_dying_script(LF_KING, 1);
    int bomber = lf_spawn(LF_BOMBER, 1, LF_TX, LF_ROW);
    int foe = lf_spawn(LF_STURDY, 2, LF_TX + 32, LF_ROW);
    int king = lf_spawn(LF_KING, 1, LF_SX, LF_ROW + 300);
    int rest = lf_spawn(LF_STURDY, 1, LF_SX, LF_ROW - 300);
    ASSERT(bomber >= 0 && foe >= 0 && king >= 0 && rest >= 0);
    lf_ticks(4);
    ASSERT_EQ_INT(bomber, Units_DebugKillHandle(bomber));
    ASSERT_EQ_INT(UNIT_ALIVE_DYING, lf_unit(bomber)->alive);
    ASSERT_EQ_INT(100000, lf_unit(foe)->health);
    ASSERT_EQ_INT(king, Units_DebugKillHandle(king));
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, lf_unit(rest)->alive);
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, lf_unit(bomber)->alive);
    ASSERT_EQ_INT(98000, lf_unit(foe)->health);
    lf_ticks(60);
    ASSERT_EQ_INT(98000, lf_unit(foe)->health);
    lf_dying_script(LF_BOMBER, 0);
    lf_dying_script(LF_KING, 0);
    lf_end();
}

/* ── remastered battlefield rules (D-036) ─────────────────────────── */

/* A rock is indestructible in the original (legacy:128756). Under the
 * remastered rules it takes its file's 3000 and is gone. */
TEST(a_remastered_battle_breaks_a_rock_the_original_cannot) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW);
    ASSERT(s >= 0);
    int r = lf_place(w, FD_ROCK, 90, 90);
    ASSERT(r >= 0);
    int32_t x = 90 * 16 + 16, y = 90 * 16 + 16;
    lf_ticks(4);
    for (int i = 0; i < 3; i++) ASSERT(Units_DebugBlastAt(s, 0, x, y));
    ASSERT_EQ_INT(1, w->feature_count);
    ASSERT_EQ_INT(0, w->features[r].damage_taken);
    w->cfg.remastered = 1;
    ASSERT(Units_DebugBlastAt(s, 0, x, y));
    ASSERT_EQ_INT(2000, w->features[r].damage_taken);
    ASSERT(Units_DebugBlastAt(s, 0, x, y));
    ASSERT_EQ_INT(0, w->feature_count);
    ASSERT_EQ_INT(1, Terrain_IsWalkable(w, x, y, 255));
    lf_end();
}

/* A unitsonly spell leaves scenery alone in the original (legacy:245240)
 * and fells it under the remastered rules. */
TEST(a_sweeping_spell_reaches_scenery_under_the_remastered_rules) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int sw = lf_spawn(LF_SWEEPER, 1, LF_SX, LF_ROW);
    int t = lf_place(w, FD_TREE, 90, 90);
    ASSERT(sw >= 0 && t >= 0);
    lf_ticks(4);
    w->cfg.remastered = 1;
    ASSERT(Units_DebugBlastAt(sw, 0, 90 * 16 + 8, 90 * 16 + 8));
    ASSERT_EQ_INT(FEATURE_FX_DYING, w->features[t].fx);
    lf_end();
}

/* Burning scenery hurts nobody in the original: no feature has the
 * BurnWeapon its loader reads (legacy:127389-127401). Under the
 * remastered rules a unit beside a burning tree takes 25 every 15
 * frames, from no one. */
TEST(fire_hurts_under_the_remastered_rules) {
    int hurt[2] = { 0, 0 };
    for (int rules = 0; rules < 2; rules++) {
        GameWorld *w = lf_world(0, 0);
        ASSERT_NOT_NULL(w);
        w->cfg.remastered = rules;
        int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
        int by = lf_spawn(LF_TARGET, 2, 100 * 16 + 8, 100 * 16 + 30);
        int t = lf_place(w, FD_TREE, 100, 100);
        ASSERT(s >= 0 && by >= 0 && t >= 0);
        lf_ticks(4);
        ASSERT(Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8));
        int before = lf_unit(by)->health;
        lf_ticks(130);
        hurt[rules] = before - lf_unit(by)->health;
        lf_end();
    }
    ASSERT_EQ_INT(0, hurt[0]);
    ASSERT(hurt[1] >= 4 * FEATURE_BURN_DAMAGE);
    ASSERT_EQ_INT(0, hurt[1] % FEATURE_BURN_DAMAGE);
}

/* A wall's rubble lets units through in the original. Under the
 * remastered rules it blocks until it is swept, and the route planner's
 * layers follow it both ways. */
TEST(rubble_blocks_until_swept_under_the_remastered_rules) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.remastered = 1;
    int s = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW);
    int wl = lf_place(w, FD_WALL, 90, 90);
    ASSERT(s >= 0 && wl >= 0);
    TAK_PathCacheWarm(w, &w->moveinfo.classes[0], 255, 0, 0);
    int32_t x = 90 * 16 + 16, y = 90 * 16 + 16;
    lf_ticks(4);
    for (int i = 0; i < 6; i++) ASSERT(Units_DebugBlastAt(s, 0, x, y));
    lf_ticks(40);
    for (int i = 0; i < 5; i++) ASSERT(Units_DebugBlastAt(s, 0, x, y));
    lf_ticks(40);
    ASSERT_EQ_INT(FD_WALLB, w->features[wl].global_idx);
    ASSERT_EQ_INT(1, w->features[wl].rubble);
    ASSERT_EQ_INT(0, Terrain_IsWalkable(w, x, y, 255));
    ASSERT_EQ_INT(0, TAK_PathDebugCheckCache(w));
    ASSERT_EQ_INT(0, Features_RemoveInstance(w, wl));
    ASSERT_EQ_INT(1, Terrain_IsWalkable(w, x, y, 255));
    ASSERT_EQ_INT(0, TAK_PathDebugCheckCache(w));
    lf_end();
}

/* The rules ride in the save with the battle they were set up for. */
TEST(the_remastered_rules_come_back_with_a_save) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    w->cfg.remastered = 1;
    int s = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW);
    ASSERT(s >= 0);
    lf_ticks(4);
    uint32_t on = TAK_SimHash();
    char err[256] = { 0 };
    const char *path = "lf_rules_save.oksave";
    remove(path);
    ASSERT_EQ_INT(0, Save_Write(path, err, sizeof err));
    w->cfg.remastered = 0;
    ASSERT(TAK_SimHash() != on);
    TAK_SaveGame *sg = Save_Read(path, err, sizeof err);
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(1, Save_Info(sg)->cfg.remastered);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof err));
    Save_ReadClose(sg);
    ASSERT_EQ_INT(1, w->cfg.remastered);
    ASSERT_EQ_INT((int)on, (int)TAK_SimHash());
    remove(path);
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
#define LF_FOG_VOLLEY_HASH 0x5c9fd4d9u

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

/* ── combat rules ──────────────────────────────────────────────────── */

/* One blow from `striker` on `victim`, in hit points. */
static int lf_blow(int striker, int victim) {
    int before = lf_unit(victim)->health;
    if (!Units_DebugFireAt(striker, 0, victim)) return -1;
    return before - lf_unit(victim)->health;
}

/* Attack and armour each go up a tenth a level, to level 10
 * (legacy:232971-232984, legacy:235826-235862). */
TEST(a_veteran_hits_harder_and_takes_less) {
    ASSERT_EQ_INT(150, Units_ScaleDamageVeteran(100, 100, 5, 0, 100));
    ASSERT_EQ_INT(66, Units_ScaleDamageVeteran(100, 100, 0, 5, 100));
    ASSERT_EQ_INT(200, Units_ScaleDamageVeteran(100, 100, 12, 0, 100));
    ASSERT_EQ_INT(300, Units_ScaleDamageVeteran(200, 100, 5, 0, 100));
    ASSERT_EQ_INT(1, Units_ScaleDamageVeteran(100, 100, 0, 10, 1));

    ASSERT_NOT_NULL(lf_world(0, 0));
    int a = lf_spawn(LF_VETERAN, 1, LF_SX, LF_ROW);
    int b = lf_spawn(LF_VETERAN, 2, LF_SX + 24, LF_ROW);
    ASSERT(a >= 0 && b >= 0);
    int plain = lf_blow(a, b);
    Units_DebugSetVeteranLevel(a, 5);
    int veteran = lf_blow(a, b);
    Units_DebugSetVeteranLevel(b, 5);
    int even = lf_blow(a, b);
    Units_DebugSetVeteranLevel(a, 0);
    int armoured = lf_blow(a, b);
    /* Past 10 counts as 10. */
    Units_DebugSetVeteranLevel(a, 15);
    Units_DebugSetVeteranLevel(b, 0);
    int capped = lf_blow(a, b);
    lf_end();
    printf("(%d %d %d %d %d) ", plain, veteran, even, armoured, capped);
    ASSERT_EQ_INT(100, plain);
    ASSERT_EQ_INT(150, veteran);
    ASSERT_EQ_INT(100, even);
    ASSERT_EQ_INT(66, armoured);
    ASSERT_EQ_INT(200, capped);
}

/* A weapon is melee by type = Melee (legacy:249726), not by its name or
 * its reach. */
TEST(a_weapon_is_melee_by_its_type) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    ASSERT_EQ_INT(1, Units_WeaponIsMelee(LF_BONE, 0));
    ASSERT_EQ_INT(1, Units_WeaponIsMelee(LF_SWORD, 0));
    ASSERT_EQ_INT(0, Units_WeaponIsMelee(LF_STAFF, 0));
    ASSERT_EQ_INT(0, Units_WeaponIsMelee(LF_BOWBLADE, 0));
    ASSERT_EQ_INT(0, Units_WeaponIsMelee(LF_ARCHER, 0));
    /* The bone strikes at once and throws nothing. */
    int g = lf_spawn(LF_BONE, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_SX + 30, LF_ROW);
    ASSERT(g >= 0 && t >= 0);
    int dealt = lf_blow(g, t);
    int count = 0, flying = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    for (int i = 0; i < count; i++) if (ps[i].alive) flying++;
    lf_end();
    ASSERT_EQ_INT(40, dealt);
    ASSERT_EQ_INT(0, flying);
}

/* The file's standingunitorder is the stance a unit is born with, and a
 * unit whose file says nothing starts offensive (legacy:162926-162947). */
TEST(a_unit_is_born_with_its_standing_order) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int h = Units_Spawn(LF_HOLDER, 1, 0, LF_SX, LF_ROW);
    int r = Units_Spawn(LF_ROVER, 1, 0, LF_SX, LF_ROW + 64);
    int n = Units_Spawn(LF_ARCHER, 1, 0, LF_SX, LF_ROW + 128);
    ASSERT(h >= 0 && r >= 0 && n >= 0);
    int hs = lf_unit(h)->aggro_mode, rs = lf_unit(r)->aggro_mode;
    int ns = lf_unit(n)->aggro_mode;
    lf_end();
    ASSERT_EQ_INT(UNIT_AGGRO_DEFENSIVE, hs);
    ASSERT_EQ_INT(UNIT_AGGRO_OFFENSIVE, rs);
    ASSERT_EQ_INT(UNIT_AGGRO_OFFENSIVE, ns);
}

#ifdef _WIN32
#include <direct.h>
#define lf_mkdir(p) _mkdir(p)
#define lf_rmdir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define lf_mkdir(p) mkdir(p, 0755)
#define lf_rmdir(p) rmdir(p)
#endif

#define LF_FBI_DIR "lf_fbi_tmp"

static const char LF_FBI_HOLD[] =
    "[UNITINFO]\n{\n\tUnitName=FBIHOLD;\n\tstandingunitorder=1;\n}\n";
static const char LF_FBI_ROVE[] =
    "[UNITINFO]\n{\n\tUnitName=FBIROVE;\n\tstandingunitorder=2;\n}\n";
static const char LF_FBI_NONE[] =
    "[UNITINFO]\n{\n\tUnitName=FBINONE;\n}\n";
static const char LF_FBI_STEADY[] =
    "[UNITINFO]\n{\n\tUnitName=FBISTEADY;\n\tstandingmoveorder=1;\n}\n";

TEST(the_unit_file_gives_the_standing_order) {
    VFS_Shutdown();
    lf_mkdir(LF_FBI_DIR);
    TestHPIEntry e[] = {
        { "units/fbihold.fbi", LF_FBI_HOLD, 100, 0 },
        { "units/fbirove.fbi", LF_FBI_ROVE, 100, 0 },
        { "units/fbinone.fbi", LF_FBI_NONE, 100, 0 },
        { "units/fbisteady.fbi", LF_FBI_STEADY, 100, 0 },
    };
    ASSERT_EQ_INT(0, test_write_hpi(LF_FBI_DIR "/a.hpi", e, 4));
    int loaded = (VFS_Init(LF_FBI_DIR, NULL) == 0) ? Units_LoadDefs() : -1;
    const UnitDef *hold = Units_GetDef(Units_FindDefByName("FBIHOLD"));
    const UnitDef *rove = Units_GetDef(Units_FindDefByName("FBIROVE"));
    const UnitDef *none = Units_GetDef(Units_FindDefByName("FBINONE"));
    int hold_has = hold ? hold->has_standing_order : -1;
    int hold_order = hold ? hold->standing_order : -1;
    int rove_order = rove ? rove->standing_order : -1;
    int none_has = none ? none->has_standing_order : -1;
    const UnitDef *steady = Units_GetDef(Units_FindDefByName("FBISTEADY"));
    /* Without a standing order the movement is standingmoveorder,
     * roam when the file does not say (legacy:162930-162934). */
    int hold_roams = hold ? hold->roams : -1;
    int none_roams = none ? none->roams : -1;
    int steady_roams = steady ? steady->roams : -1;
    VFS_Shutdown();
    Units_FreeDefs();
    remove(LF_FBI_DIR "/a.hpi");
    lf_rmdir(LF_FBI_DIR);
    ASSERT_EQ_INT(4, loaded);
    ASSERT_EQ_INT(1, hold_has);
    ASSERT_EQ_INT(UNIT_AGGRO_DEFENSIVE, hold_order);
    ASSERT_EQ_INT(UNIT_AGGRO_OFFENSIVE, rove_order);
    ASSERT_EQ_INT(0, none_has);
    ASSERT_EQ_INT(0, hold_roams);
    ASSERT_EQ_INT(1, none_roams);
    ASSERT_EQ_INT(0, steady_roams);
}

/* A unit holding position shoots what comes into reach and lets it go
 * when it walks off. One that maneuvers goes after it. */
static int32_t lf_after_walk_off(int def, int *out_target) {
    if (!lf_world(0, 0)) return -1;
    int a = Units_Spawn(def, 1, 0, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_SX + 150, LF_ROW);
    if (a < 0 || t < 0) return -1;
    for (int i = 0; i < 10; i++) Units_TickEngines();
    if (lf_unit(a)->target != t) return -2;
    Units_CommandMoveUnit(t, LF_SX + 900, LF_ROW);
    for (int i = 0; i < 900; i++) Units_TickEngines();
    int32_t x = lf_unit(a)->world_x;
    *out_target = lf_unit(a)->target;
    lf_end();
    return x;
}

/* Hit from 400 px, past its reach of 200 and inside its leash. */
static int lf_answer_from_afar(int def, int32_t *out_x, int *out_can) {
    if (!lf_world(0, 0)) return -2;
    int a = Units_Spawn(def, 1, 0, LF_SX, LF_ROW);
    int s = lf_spawn(LF_BOLT, 2, LF_SX + 400, LF_ROW);
    if (a < 0 || s < 0) return -2;
    *out_can = Units_CanAnswer(a, s);
    LfShot r = lf_fire(s, a, 300);
    for (int i = 0; i < 120; i++) Units_TickEngines();
    int held = r.fired ? lf_unit(a)->target : -3;
    *out_x = lf_unit(a)->world_x;
    lf_end();
    return held;
}

TEST(a_unit_holding_position_does_not_chase) {
    int held = 0, chased = 0;
    int32_t hx = lf_after_walk_off(LF_HOLDER, &held);
    int32_t rx = lf_after_walk_off(LF_ROVER, &chased);
    printf("(held at %d target %d, rover at %d target %d) ", hx, held, rx, chased);
    ASSERT_EQ_INT(LF_SX, hx);
    ASSERT_EQ_INT(-1, held);
    ASSERT(rx > LF_SX + 300);
    /* It answers no shooter out of its reach, where a maneuvering unit
     * answers one inside its leash. */
    int32_t ax = 0, bx = 0;
    int can_held = -1, can_rover = -1;
    int answered_held = lf_answer_from_afar(LF_HOLDER, &ax, &can_held);
    int answered_rover = lf_answer_from_afar(LF_ROVER, &bx, &can_rover);
    printf("(holder takes %d at %d, rover takes %d at %d) ",
           answered_held, ax, answered_rover, bx);
    ASSERT_EQ_INT(0, can_held);
    ASSERT_EQ_INT(-1, answered_held);
    ASSERT_EQ_INT(LF_SX, ax);
    ASSERT_EQ_INT(1, can_rover);
    ASSERT(answered_rover >= 0);
}

/* A fight a unit took on for itself may run the target search again at
 * the end of each wait of the attack handler (legacy:11485-11520). Three
 * dummies 200 px from a `def`, with `stance` and, if `ordered`, an
 * attack order on the middle one, for `ticks`. Counts the switches and
 * the ends of waits, and fails with -2 when a switch lands off the end
 * of a wait or a wait is not 4 to 12 of the original's frames. */
static int lf_picker_run(int def, int stance, int ordered, int ticks,
                         int *out_first, uint32_t *out_trace, int *out_checks) {
    if (!lf_world(0, 0)) return -1;
    int a = Units_Spawn(def, 1, 0, LF_SX, LF_ROW);
    int t[3];
    for (int k = 0; k < 3; k++) {
        t[k] = lf_spawn(LF_TARGET, 2, LF_SX + 200, LF_ROW - 64 + k * 64);
        if (t[k] < 0) return -1;
    }
    if (a < 0) return -1;
    Units_DebugSetAggro(a, stance);
    if (ordered && !Units_OrderAttack(a, t[1])) return -1;
    for (int i = 0; i < 4; i++) Units_TickEngines();
    int held = lf_unit(a)->target;
    *out_first = held;
    int switches = 0, checks = 0, bad = 0;
    uint32_t trace = 0;
    int wait = lf_unit(a)->research_wait;
    for (int i = 0; i < ticks; i++) {
        Units_TickEngines();
        int now = lf_unit(a)->target;
        int next = lf_unit(a)->research_wait;
        if (wait == 1) {
            checks++;
            if (next < 8 || next > 24 || (next & 1)) bad = 1;
        } else if (next != 0 && wait != 0 && next != wait - 1) {
            bad = 1;
        }
        if (now != held) {
            if (wait != 1) bad = 1;
            switches++;
            trace = trace * 31u + (uint32_t)(i * 8 + now);
            held = now;
        }
        wait = next;
    }
    *out_trace = trace;
    *out_checks = checks;
    lf_end();
    return bad ? -2 : switches;
}

/* A draw of 2 before the search and one of 10 after it
 * (legacy:11517, legacy:11519): about 1 check in 20 takes what the
 * search finds, and a random picker among three finds another two
 * times in three, so about 1 in 30 switches. */
TEST(a_fight_of_its_own_looks_again_once_a_wait) {
    int first = -1, first2 = -1, checks = 0, checks2 = 0;
    uint32_t trace = 0, trace2 = 0;
    int switches = lf_picker_run(LF_PICKER, UNIT_AGGRO_OFFENSIVE, 0, 36000,
                                 &first, &trace, &checks);
    int again = lf_picker_run(LF_PICKER, UNIT_AGGRO_OFFENSIVE, 0, 36000,
                              &first2, &trace2, &checks2);
    printf("(%d switches in %d checks, first %d) ", switches, checks, first);
    ASSERT(first >= 0);
    /* 36000 ticks of waits of 8 to 24 ticks. */
    ASSERT(checks >= 36000 / 24 && checks <= 36000 / 8 + 1);
    ASSERT(switches > 0);
    ASSERT(switches * 60 >= checks && switches * 15 <= checks);
    ASSERT_EQ_INT(switches, again);
    ASSERT_EQ_INT(checks, checks2);
    ASSERT_EQ_INT(first, first2);
    ASSERT_EQ_INT((int)trace, (int)trace2);
}

/* Holding position, not on fire at will, or on an attack order, it
 * stays on its target. */
TEST(a_unit_holding_position_or_ordered_never_looks_again) {
    int first = -1, checks = 0;
    uint32_t trace = 0;
    int held = lf_picker_run(LF_PICKER, UNIT_AGGRO_DEFENSIVE, 0, 1200,
                             &first, &trace, &checks);
    ASSERT(first >= 0);
    ASSERT_EQ_INT(0, held);
    ASSERT_EQ_INT(0, checks);
    int ordered = lf_picker_run(LF_PICKER, UNIT_AGGRO_OFFENSIVE, 1, 1200,
                                &first, &trace, &checks);
    ASSERT(first >= 0);
    ASSERT_EQ_INT(0, ordered);
    ASSERT_EQ_INT(0, checks);
}

/* The handler returns at once for a unit with no mover or with canfly
 * (legacy:11351-11355), so a tower and a flyer on offensive never look
 * again this way. A melee chase has its own search (legacy:11189-11195),
 * so neither does a melee unit here. */
static int lf_never_looks_again(int def) {
    int first = -1, checks = -1;
    uint32_t trace = 0;
    int switches = lf_picker_run(def, UNIT_AGGRO_OFFENSIVE, 0, 6000,
                                 &first, &trace, &checks);
    printf("(first %d, %d switches in %d checks) ", first, switches, checks);
    return first >= 0 && switches == 0 && checks == 0;
}

TEST(a_tower_never_looks_again_this_way) {
    ASSERT(lf_never_looks_again(LF_TOWER));
}

TEST(a_flyer_never_looks_again_this_way) {
    ASSERT(lf_never_looks_again(LF_FLYPICK));
}

TEST(a_melee_unit_never_looks_again_this_way) {
    ASSERT(lf_never_looks_again(LF_BLADEPICK));
}

/* A maneuvering unit that takes the nearest looks again too, not only a
 * random picker, and takes what it finds only inside its leash
 * (legacy:11520). One that roams has no leash (legacy:13733-13737).
 * It chases a dummy 500 px east, then one turns up 300 px west, nearer
 * but past a leash of nothing plus its reach of 200. Returns 1 when it
 * switches, at the end of a wait, and 0 when it keeps the first. */
static int lf_leash_run(int def, int *out_checks) {
    if (!lf_world(0, 0)) return -1;
    int a = Units_Spawn(def, 1, 0, LF_SX, LF_ROW);
    int far = lf_spawn(LF_TARGET, 2, LF_SX + 500, LF_ROW);
    if (a < 0 || far < 0) return -1;
    for (int i = 0; i < 4; i++) Units_TickEngines();
    if (lf_unit(a)->target != far) return -1;
    int near = lf_spawn(LF_TARGET, 2, LF_SX - 300, LF_ROW);
    if (near < 0) return -1;
    int checks = 0, result = 0;
    int wait = lf_unit(a)->research_wait;
    for (int i = 0; i < 3000 && result == 0; i++) {
        Units_TickEngines();
        int now = lf_unit(a)->target;
        if (wait == 1) checks++;
        if (now == near) result = wait == 1 ? 1 : -2;
        else if (now != far) result = -1;
        wait = lf_unit(a)->research_wait;
    }
    *out_checks = checks;
    lf_end();
    return result;
}

TEST(a_unit_looks_again_only_inside_its_leash) {
    int cs = 0, cl = 0, cr = 0;
    int short_leash = lf_leash_run(LF_SEEKER, &cs);
    int long_leash = lf_leash_run(LF_SEEKFAR, &cl);
    int roams = lf_leash_run(LF_ROAMER, &cr);
    printf("(short %d after %d, long %d after %d, roam %d after %d) ",
           short_leash, cs, long_leash, cl, roams, cr);
    /* Over 3000 ticks, well past the 20 or so checks a switch takes. */
    ASSERT_EQ_INT(0, short_leash);
    ASSERT(cs >= 3000 / 24);
    ASSERT_EQ_INT(1, long_leash);
    ASSERT_EQ_INT(1, roams);
}

/* ── what the blast hook hears ─────────────────────────────────────── */

TEST(a_rock_on_the_ground_tells_the_hook_where_and_whose) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_SIEGE, 1, LF_SX, LF_ROW);
    ASSERT(s >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    lf_listen();
    ASSERT(Units_DebugFireGround(s, 0, LF_TX, LF_ROW));
    for (int t = 0; t < 400 && g_lf_blasts == 0; t++) Units_TickEngines();
    Units_SetBlastHook(NULL);
    ASSERT_EQ_INT(1, g_lf_blasts);
    const UnitsBlast *b = &g_lf_blast[0];
    ASSERT_EQ_INT(LF_SIEGE, b->def);
    ASSERT_EQ_INT(0, b->slot);
    ASSERT_EQ_INT(s, b->shooter);
    ASSERT_EQ_INT(-1, b->struck);
    ASSERT_EQ_INT(1, b->player);
    ASSERT_EQ_INT(80, b->damage);
    ASSERT_EQ_INT(0, b->in_water);
    ASSERT(b->x > LF_TX - 24 && b->x < LF_TX + 24 && b->y > LF_ROW - 24 && b->y < LF_ROW + 24);
    printf("(dir %.2f %.2f %.2f) ", b->dir_x, b->dir_y, b->dir_up);
    /* Lobbed east, and coming down steeply. */
    ASSERT(b->dir_x > 0.05f);
    ASSERT(b->dir_y > -0.05f && b->dir_y < 0.05f);
    ASSERT(b->dir_up < -0.5f);
    lf_end();
}

TEST(an_arrow_tells_the_hook_the_unit_it_struck) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    lf_listen();
    LfShot r = lf_fire(s, t, 300);
    Units_SetBlastHook(NULL);
    ASSERT(r.fired);
    ASSERT_EQ_INT(1, g_lf_blasts);
    ASSERT_EQ_INT(t, g_lf_blast[0].struck);
    ASSERT_EQ_INT(LF_ARCHER, g_lf_blast[0].def);
    ASSERT_EQ_INT(0, g_lf_blast[0].area_of_effect);
    ASSERT(lf_unit(t)->health < 1000);
    lf_end();
}

TEST(lightning_tells_the_hook_as_it_strikes) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int s = lf_spawn(LF_LIGHTNING, 1, LF_SX, LF_ROW);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW);
    ASSERT(s >= 0 && t >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    lf_listen();
    ASSERT(Units_DebugFireAt(s, 0, t));
    Units_SetBlastHook(NULL);
    ASSERT_EQ_INT(1, g_lf_blasts);
    ASSERT_EQ_INT(t, g_lf_blast[0].struck);
    ASSERT_EQ_INT(LF_LIGHTNING, g_lf_blast[0].def);
    /* The bolt runs east from the muzzle to the target. */
    ASSERT(g_lf_blast[0].dir_x > 0.9f);
    lf_end();
}

/* A shot read back from a save keeps no record of whose it was, and must
 * not take the record of the last shot that held its slot. */
TEST(a_shot_read_back_from_a_save_names_no_weapon) {
    ASSERT_NOT_NULL(lf_world(0, 0));
    int pult = lf_spawn(LF_SIEGE, 1, LF_SX, LF_ROW);
    int bow = lf_spawn(LF_ARCHER, 1, LF_SX, LF_ROW + 200);
    int t = lf_spawn(LF_TARGET, 2, LF_TX, LF_ROW + 200);
    ASSERT(pult >= 0 && bow >= 0 && t >= 0);
    for (int i = 0; i < 4; i++) Units_TickEngines();
    ASSERT(Units_DebugFireGround(pult, 0, LF_TX, LF_ROW));
    Units_TickEngines();
    int count = 0;
    const Projectile *ps = Units_GetProjectiles(&count);
    ASSERT(count >= 1 && count <= 4);
    Projectile saved[4];
    memcpy(saved, ps, sizeof(Projectile) * (size_t)count);
    int n = count;
    for (int i = 0; i < 400; i++) Units_TickEngines();
    /* The bow's arrow takes the rock's slot and lands. */
    LfShot r = lf_fire(bow, t, 300);
    ASSERT(r.fired);
    Projectile *pool = Units_LoadProjectiles(n);
    ASSERT_NOT_NULL(pool);
    memcpy(pool, saved, sizeof(Projectile) * (size_t)n);
    lf_listen();
    for (int i = 0; i < 400 && g_lf_blasts == 0; i++) Units_TickEngines();
    Units_SetBlastHook(NULL);
    ASSERT_EQ_INT(1, g_lf_blasts);
    ASSERT_EQ_INT(-1, g_lf_blast[0].def);
    ASSERT_EQ_INT(-1, g_lf_blast[0].slot);
    lf_end();
}

TEST(the_blast_hook_leaves_the_volley_as_it_was) {
    int hurt_a = 0, hurt_b = 0;
    uint32_t a = lf_volley_hash(&hurt_a);
    lf_listen();
    uint32_t b = lf_volley_hash(&hurt_b);
    Units_SetBlastHook(NULL);
    printf("(%d blasts) ", g_lf_blasts);
    ASSERT(g_lf_blasts >= 3);
    ASSERT_EQ_INT((int)a, (int)b);
    ASSERT_EQ_INT(hurt_a, hurt_b);
}

/* ── what the feature hook hears ───────────────────────────────────── */

#define LF_EVENTS 64
static FeatureEvent g_lf_event[LF_EVENTS];
static int g_lf_events;

static void lf_note_event(const GameWorld *w, const FeatureEvent *e) {
    (void)w;
    if (g_lf_events < LF_EVENTS) g_lf_event[g_lf_events] = *e;
    g_lf_events++;
}

static void lf_hear(void) {
    g_lf_events = 0;
    Features_SetEventHook(lf_note_event);
}

/* The nth event of a kind for an instance, or NULL. */
static const FeatureEvent *lf_heard(int kind, int idx, int nth) {
    for (int i = 0; i < g_lf_events && i < LF_EVENTS; i++)
        if (g_lf_event[i].kind == kind && g_lf_event[i].idx == idx && nth-- == 0) return &g_lf_event[i];
    return NULL;
}

TEST(a_felled_tree_tells_the_hook_its_hit_death_and_stage) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW);
    int t = lf_place(w, FD_TREE, 90, 90);
    ASSERT(s >= 0 && t >= 0);
    int32_t x = 90 * 16 + 8, y = 90 * 16 + 8;
    lf_ticks(4);
    lf_hear();
    ASSERT(Units_DebugBlastAt(s, 0, x, y));
    lf_ticks(44);
    Features_SetEventHook(NULL);
    ASSERT_EQ_INT(3, g_lf_events);
    const FeatureEvent *hit = &g_lf_event[0], *dying = &g_lf_event[1], *dead = &g_lf_event[2];
    ASSERT_EQ_INT(FEATURE_EVENT_HIT, hit->kind);
    ASSERT_EQ_INT(t, hit->idx);
    ASSERT_EQ_INT(FD_TREE, hit->def);
    ASSERT_EQ_INT(2000, hit->damage);
    ASSERT_EQ_INT(0, hit->left);
    ASSERT_EQ_INT(1, hit->blast);
    ASSERT(hit->bx == x && hit->by == y);
    ASSERT_EQ_INT(FEATURE_EVENT_DYING, dying->kind);
    /* Ten pictures of two of the original's frames. */
    ASSERT_EQ_INT(20, dying->frames);
    ASSERT_EQ_INT(1, dying->blast);
    ASSERT_EQ_INT(FEATURE_EVENT_DEAD, dead->kind);
    ASSERT_EQ_INT(t, dead->idx);
    ASSERT_EQ_INT(FD_TREE, dead->def);
    ASSERT_EQ_INT(FD_TREEDEAD, dead->new_def);
    ASSERT_EQ_INT(0, dead->blast);
    lf_end();
}

TEST(a_wall_tells_the_hook_each_hit_and_what_it_has_left) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_CANNON, 1, LF_SX, LF_ROW);
    int wl = lf_place(w, FD_WALL, 90, 90);
    int r = lf_place(w, FD_ROCK, 94, 90);
    ASSERT(s >= 0 && wl >= 0 && r >= 0);
    lf_ticks(4);
    lf_hear();
    ASSERT(Units_DebugBlastAt(s, 0, 90 * 16 + 16, 90 * 16 + 16));
    ASSERT(Units_DebugBlastAt(s, 0, 94 * 16 + 16, 90 * 16 + 16));
    Features_SetEventHook(NULL);
    const FeatureEvent *hit = lf_heard(FEATURE_EVENT_HIT, wl, 0);
    ASSERT_NOT_NULL(hit);
    ASSERT_EQ_INT(2000, hit->damage);
    ASSERT_EQ_INT(10000, hit->left);
    /* A rock the blast reached, which ignores it. */
    const FeatureEvent *rock = lf_heard(FEATURE_EVENT_HIT, r, 0);
    ASSERT_NOT_NULL(rock);
    ASSERT_EQ_INT(0, rock->damage);
    ASSERT_EQ_INT(FD_ROCK, rock->def);
    lf_end();
}

TEST(a_fire_tells_the_hook_where_it_caught_and_burnt_out) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    int s = lf_spawn(LF_TORCH, 1, LF_SX, LF_ROW);
    ASSERT(s >= 0);
    int mid, e6, e9, w6, w9;
    lf_forest(w, &mid, &e6, &e9, &w6, &w9);
    lf_ticks(4);
    lf_hear();
    ASSERT(Units_DebugBlastAt(s, 0, 100 * 16 + 8, 100 * 16 + 8));
    lf_ticks(1200);
    Features_SetEventHook(NULL);
    const FeatureEvent *lit = lf_heard(FEATURE_EVENT_BURNING, mid, 0);
    ASSERT_NOT_NULL(lit);
    ASSERT_EQ_INT(1, lit->blast);
    /* A burn of 200 pictures of two frames, with no flames to end it sooner. */
    ASSERT_EQ_INT(400, lit->frames);
    const FeatureEvent *caught = lf_heard(FEATURE_EVENT_BURNING, e6, 0);
    ASSERT_NOT_NULL(caught);
    ASSERT_EQ_INT(0, caught->blast);
    ASSERT_NULL(lf_heard(FEATURE_EVENT_BURNING, w6, 0));
    const FeatureEvent *burnt = lf_heard(FEATURE_EVENT_BURNT, mid, 0);
    ASSERT_NOT_NULL(burnt);
    ASSERT_EQ_INT(FD_LONGTREE, burnt->def);
    ASSERT_EQ_INT(FD_TREEBURNT, burnt->new_def);
    lf_end();
}

TEST(placing_sweeping_and_removing_tell_the_hook) {
    GameWorld *w = lf_world(0, 0);
    ASSERT_NOT_NULL(w);
    lf_hear();
    int a = lf_place(w, FD_TREE, 80, 80);
    int b = lf_place(w, FD_TREE, 84, 80);
    ASSERT(a >= 0 && b >= 0);
    ASSERT_EQ_INT(0, Features_SweepInstance(w, a));
    ASSERT_EQ_INT(0, Features_RemoveInstance(w, a));
    Features_SetEventHook(NULL);
    ASSERT_EQ_INT(4, g_lf_events);
    ASSERT_EQ_INT(FEATURE_EVENT_PLACED, g_lf_event[0].kind);
    ASSERT_EQ_INT(a, g_lf_event[0].idx);
    ASSERT_EQ_INT(FD_TREE, g_lf_event[0].def);
    ASSERT_EQ_INT(80 * 16 + 8, g_lf_event[0].x);
    ASSERT_EQ_INT(FEATURE_EVENT_PLACED, g_lf_event[1].kind);
    ASSERT_EQ_INT(FEATURE_EVENT_SWEPT, g_lf_event[2].kind);
    ASSERT_EQ_INT(a, g_lf_event[2].idx);
    /* The second tree moved down into the first one's place. */
    ASSERT_EQ_INT(FEATURE_EVENT_REMOVED, g_lf_event[3].kind);
    ASSERT_EQ_INT(84 * 16 + 8, g_lf_event[3].x);
    lf_end();
}

TEST(the_feature_hook_leaves_a_fire_as_it_was) {
    uint32_t a = lf_fire_hash(500);
    lf_hear();
    uint32_t b = lf_fire_hash(500);
    Features_SetEventHook(NULL);
    printf("(%d events) ", g_lf_events);
    ASSERT(g_lf_events > 10);
    ASSERT_EQ_INT((int)a, (int)b);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("What the feature hook hears");
    RUN(a_felled_tree_tells_the_hook_its_hit_death_and_stage);
    RUN(a_wall_tells_the_hook_each_hit_and_what_it_has_left);
    RUN(a_fire_tells_the_hook_where_it_caught_and_burnt_out);
    RUN(placing_sweeping_and_removing_tell_the_hook);
    RUN(the_feature_hook_leaves_a_fire_as_it_was);
    TEST_SUITE("What the blast hook hears");
    RUN(a_rock_on_the_ground_tells_the_hook_where_and_whose);
    RUN(an_arrow_tells_the_hook_the_unit_it_struck);
    RUN(lightning_tells_the_hook_as_it_strikes);
    RUN(a_shot_read_back_from_a_save_names_no_weapon);
    RUN(the_blast_hook_leaves_the_volley_as_it_was);
    TEST_SUITE("Combat rules");
    RUN(a_veteran_hits_harder_and_takes_less);
    RUN(a_weapon_is_melee_by_its_type);
    RUN(a_unit_is_born_with_its_standing_order);
    RUN(the_unit_file_gives_the_standing_order);
    RUN(a_unit_holding_position_does_not_chase);
    RUN(a_fight_of_its_own_looks_again_once_a_wait);
    RUN(a_unit_holding_position_or_ordered_never_looks_again);
    RUN(a_tower_never_looks_again_this_way);
    RUN(a_flyer_never_looks_again_this_way);
    RUN(a_melee_unit_never_looks_again_this_way);
    RUN(a_unit_looks_again_only_inside_its_leash);
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
    TEST_SUITE("Blasts");
    RUN(a_blast_falls_off_like_the_originals);
    RUN(a_blast_reaches_half_its_area_to_the_side_of_a_unit);
    RUN(a_small_blast_on_a_unit_hits_it_alone);
    TEST_SUITE("Scenery");
    RUN(a_cannon_fells_a_tree_to_its_stump);
    RUN(a_wall_drops_a_stage_then_to_rubble_that_stops_blocking);
    RUN(a_firestarter_lights_a_forest_that_spreads_downwind_not_upwind);
    RUN(a_shipped_tree_burns_out_before_its_spark);
    RUN(a_units_only_blast_leaves_scenery_alone);
    RUN(a_fire_saved_and_loaded_runs_on_the_same);
    RUN(two_fires_hash_the_same);
    TEST_SUITE("Death blasts");
    RUN(a_death_weapon_bursts_on_friend_and_foe_where_its_unit_falls);
    RUN(a_frame_or_a_side_taken_off_never_bursts);
    RUN(a_death_blast_owed_comes_back_with_a_save);
    RUN(a_unit_dying_as_its_side_is_taken_off_still_bursts);
    TEST_SUITE("Remastered battlefield");
    RUN(a_remastered_battle_breaks_a_rock_the_original_cannot);
    RUN(a_sweeping_spell_reaches_scenery_under_the_remastered_rules);
    RUN(fire_hurts_under_the_remastered_rules);
    RUN(a_remastered_fire_burns_until_its_spark_and_spreads);
    RUN(a_remastered_forest_fire_saved_and_loaded_runs_on_the_same);
    RUN(a_remastered_spark_reaches_a_tree_six_cells_off);
    RUN(a_remastered_fire_throws_four_sparks_and_burns_until_the_last);
    RUN(a_remastered_wind_carries_the_spark_downwind);
    RUN(a_remastered_wind_favours_the_trees_downwind);
    RUN(a_remastered_calm_fire_spreads_alike_every_way);
    RUN(remastered_sparks_light_no_more_than_four_a_frame);
    RUN(a_remastered_sparse_wood_fire_saved_and_loaded_runs_on_the_same);
    RUN(the_classic_spark_still_reaches_three_cells);
    RUN(rubble_blocks_until_swept_under_the_remastered_rules);
    RUN(the_remastered_rules_come_back_with_a_save);
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
