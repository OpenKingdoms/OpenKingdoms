/*
 * sim_probe.c: the cross-platform simulation gate.
 *
 * test_trig proves the engine's own trigonometry returns the same bits
 * everywhere. This proves the rest: that a real battle, driven through
 * the real mover and the real combat path, reaches the same simulation
 * hash on Windows, macOS, Linux and in the browser. Between them they
 * are what lets the four platforms share one room.
 *
 * It runs on a synthetic world with synthetic unit definitions, so it
 * needs no game files and CI can run it on every platform. The number
 * it prints is TAK_SimHash over a fixed workload, folded over the run,
 * and it is pinned below. A platform that disagrees fails here rather
 * than desyncing a player's match ten minutes in.
 *
 * When a deliberate change to the simulation moves the number, take the
 * new one from a run and say in the commit what moved it.
 */

#include "tak_battle_config.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_sim_hash.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pinned answer. Every platform in CI has to reach this. */
#define SIM_PROBE_HASH 0xac347984u

#define PB_TILES   192      /* 16 px tiles per side, so a 3072 px map */
#define PB_GROUND  64       /* flat height, clear of the water line */
#define PB_TICKS   1800
#define PB_SAMPLE  60

enum { PB_DEF_WALKER = 0, PB_DEF_ARCHER, PB_DEF_COUNT };

static void pb_fill_def(UnitDef *d, const char *name, const char *mclass,
                        float velocity, int health) {
    memset(d, 0, sizeof(*d));
    strncpy(d->unitname, name, sizeof(d->unitname) - 1);
    strncpy(d->display_name, name, sizeof(d->display_name) - 1);
    strncpy(d->category, "TEST WALKER", sizeof(d->category) - 1);
    strncpy(d->movement_class, mclass, sizeof(d->movement_class) - 1);
    d->max_health = health;
    d->sight_distance = 320;
    d->max_velocity = velocity;
    d->acceleration = velocity / 4.0f;
    d->brake_rate = velocity / 2.0f;
    d->turn_rate = 4000.0f;
    d->max_slope = 30;
    d->bmcode = 1;          /* mobile: no yardmap, no structure imprint */
    d->cap_flags = UNIT_CAP_MOVE | UNIT_CAP_STOP | UNIT_CAP_ATTACK;
    d->footprint_x = 1;
    d->footprint_z = 1;
}

/* An arced shot with a spray angle, so the launch runs through every
 * angle the migration touched: the aim, the spread and the flight. */
static void pb_fill_weapon(UnitDef *d) {
    UnitWeapon *w = &d->weapons[0];
    d->num_weapons = 1;
    memset(w, 0, sizeof(*w));
    strncpy(w->name, "TESTBOW", sizeof(w->name) - 1);
    strncpy(w->type, "Ballistic", sizeof(w->type) - 1);
    w->is_gravity = 1;
    w->gravity_adjust = 1.0f;
    w->range = 480;
    w->damage = 9;
    w->reload_ticks = 45;
    w->velocity_pps = 620;
    w->spray_angle = 1200;
    w->area_of_effect = 24;
    w->edge_effectiveness = 0.25f;
    w->explosion_idx = -1;
}

/* A flat world with an occupancy layer, two move classes and two
 * synthetic defs. Returns NULL if anything could not be built. */
static GameWorld *pb_world(void) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.line_of_sight = 0;
    cfg.seed = 0x5eed1234u;
    cfg.players[0].kind = TAK_SLOT_HUMAN;
    cfg.players[1].kind = TAK_SLOT_HUMAN;
    if (World_BeginLoad(NULL, &cfg, "synthetic", "aramon") != 0) return NULL;
    GameWorld *w = World_Get();
    if (!w) return NULL;
    w->map_pixels_w = PB_TILES * 16;
    w->map_pixels_h = PB_TILES * 16;
    w->viewport_w = 640;
    w->viewport_h = 480;
    w->water_height = 0;
    w->tnt.width_tiles = PB_TILES;
    w->tnt.height_tiles = PB_TILES;
    w->tnt.height_w = PB_TILES + 1;
    w->tnt.height_h = PB_TILES + 1;
    size_t hn = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)tak_malloc(hn);
    if (!w->tnt.heightmap) return NULL;
    memset(w->tnt.heightmap, PB_GROUND, hn);

    /* Two footprints, as the shipped GROUND2 and GROUND3 have. A wall
     * of one tile units is thinner than a path cell. */
    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 2;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 2;
    w->moveinfo.classes[0].footprint_z = 2;
    w->moveinfo.classes[0].max_slope = 30;
    strncpy(w->moveinfo.classes[1].name, "TESTBIG", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[1].footprint_x = 3;
    w->moveinfo.classes[1].footprint_z = 3;
    w->moveinfo.classes[1].max_slope = 30;

    if (!Occ_Ensure(w)) return NULL;
    TAK_PathCacheReset();
    World_MarkLoaded();

    UnitDef defs[PB_DEF_COUNT];
    pb_fill_def(&defs[PB_DEF_WALKER], "TESTSWORD", "TESTSMALL", 1.4f, 200);
    pb_fill_def(&defs[PB_DEF_ARCHER], "TESTARCHR", "TESTBIG", 2.2f, 400);
    pb_fill_weapon(&defs[PB_DEF_ARCHER]);
    if (Units_DebugSetDefs(defs, PB_DEF_COUNT) != PB_DEF_COUNT) return NULL;
    return w;
}

/* Two hostile lines that walk into each other and shoot. The walk puts
 * every heading, direction vector and subpixel step through the
 * engine's own trigonometry, and the archers put the aim angle, the
 * spray and the arc through it. */
static int pb_battle(uint32_t *out_hash, int *out_shots) {
    if (!pb_world()) return 0;
    const int32_t mid_x = 1600, mid_y = 1600;
    for (int i = 0; i < 8; i++) {
        int a = Units_Spawn(i & 1 ? PB_DEF_ARCHER : PB_DEF_WALKER, 1, 0,
                            mid_x - 560, mid_y - 112 + i * 32);
        int b = Units_Spawn(i & 1 ? PB_DEF_ARCHER : PB_DEF_WALKER, 2, 1,
                            mid_x + 560, mid_y - 112 + i * 32);
        if (a < 0 || b < 0) return 0;
        Units_CommandMoveUnit(a, mid_x + 240, mid_y);
        Units_CommandMoveUnit(b, mid_x - 240, mid_y);
    }

    uint32_t h = TAK_SIM_HASH_SEED;
    int shots = 0;
    for (int t = 0; t < PB_TICKS; t++) {
        Units_TickEngines();
        if ((t + 1) % PB_SAMPLE == 0) h = TAK_HashU32(h, TAK_SimHash());
        int live = 0;
        const Projectile *p = Units_GetProjectiles(&live);
        for (int i = 0; p && i < live; i++) if (p[i].alive) { shots++; break; }
    }
    *out_hash = h;
    *out_shots = shots;

    Units_ClearInstances();
    World_End(NULL);
    TAK_PathCacheReset();
    return 1;
}

int main(int argc, char **argv) {
    /* --print takes the number without asserting it, which is how a
     * deliberate change to the simulation gets its new pin. */
    int print_only = (argc > 1 && strcmp(argv[1], "--print") == 0);

    uint32_t h = 0;
    int shots = 0;
    if (!pb_battle(&h, &shots)) {
        fprintf(stderr, "sim probe: the synthetic battle would not start\n");
        return 1;
    }
    printf("sim probe: hash %08x, %d ticks with a shot in the air\n",
           (unsigned)h, shots);
    if (print_only) return 0;

    /* A battle where nothing ever flew would pin a number that proves
     * nothing about the paths this gate exists for. */
    if (shots <= 0) {
        fprintf(stderr, "sim probe: no shot ever flew, the gate is blind\n");
        return 1;
    }
    if (h != SIM_PROBE_HASH) {
        fprintf(stderr, "sim probe: hash %08x, expected %08x. This platform "
                        "does not agree with the others.\n",
                (unsigned)h, (unsigned)SIM_PROBE_HASH);
        return 1;
    }
    printf("sim probe: agrees with the pinned run\n");
    return 0;
}
