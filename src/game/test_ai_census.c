/*
 * test_ai_census.c: the computer player's per-think census of a seat's
 * units, against the scan it replaced.
 *
 * Every count, build score, limit test and pending-frame test in
 * src/game/ai.c used to walk every unit, once per builder and type. The
 * census counts the seat's units once a think. This runs whole thinks
 * over seeded random populations of eight seats, with the census off
 * (every answer by the old scan) and on, and asserts the same orders,
 * the same builds, the same AI state hash and the same units after
 * every tick. With the census on, a check mode also recounts every
 * answer the census gives by the old scan and counts the differences.
 *
 * The populations hold dead, dying and carried slots, frames under
 * construction, units of no seat, and builds that reuse a dead slot or
 * take a new one mid-think. No game data.
 *
 * `test_ai_census --bench` times one think of eight seats over 8000
 * units with 200 builders, census off and on.
 */

#include "tak_ai.h"
#include "tak_ai_influence.h"
#include "tak_economy.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "tak_features.h"
#include "tak_hpi.h"
#include "tak_pathing.h"
#include "tak_sim_hash.h"
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built against an ai.c from before the census, for the benchmark's
 * before. Only --bench means anything then. */
#ifdef AI_CENSUS_BASELINE
#define TAK_AI_DebugSetCensus(on) ((void)(on))
#define TAK_AI_DebugCensusCheck(on) ((void)(on))
#define TAK_AI_DebugCensusMismatches(checks) (*(checks) = 1, 0)
#endif

#define MOCK_DEFS  12
#define MOCK_UNITS TAK_MAX_UNITS
#define MAP_PX     8192

enum {
    D_MONARCH = 0, D_LODE, D_CASTLE, D_TOWER, D_KEEP, D_TROOP, D_ARCHER,
    D_WORKER, D_HANDLER, D_MANA, D_DRAGON, D_WALL
};

static UnitDef g_defs[MOCK_DEFS];
static Unit g_units[MOCK_UNITS];
static int g_unit_count;
static int g_buildable_counts[MOCK_DEFS];
static int g_buildables[MOCK_DEFS][8];
static GameWorld g_world_store;
static const GameWorld *g_world;
static int32_t g_mana[TAK_MAX_PLAYERS + 1];
static int32_t g_income[TAK_MAX_PLAYERS + 1];
static int32_t g_spend[TAK_MAX_PLAYERS + 1];
static float g_share[TAK_MAX_PLAYERS + 1];

/* Every order and build the AI gives, folded in the order given. */
static uint32_t g_log;
static int g_builds;
static int g_reused;
static int g_appended;

static void log_i(int32_t v) {
    uint32_t x = (uint32_t)v;
    for (int k = 0; k < 4; k++) {
        g_log ^= (x >> (k * 8)) & 0xFFu;
        g_log *= 16777619u;
    }
}

/* ── Mocks ─────────────────────────────────────────────────────────── */

int TAK_PathPlanQuery(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query, TAK_Path *out_path) {
    (void)world; (void)start_x; (void)start_y; (void)query;
    if (!out_path) return 0;
    memset(out_path, 0, sizeof(*out_path));
    /* A band of the map no route reaches, so some sites fail. */
    if (goal_x >= 3000 && goal_x < 3200) return 0;
    out_path->count = 1;
    out_path->x[0] = goal_x;
    out_path->y[0] = goal_y;
    return 1;
}

int TAK_PathGroundConnected(const struct GameWorld *world,
                            const struct MoveClassDef *move_class,
                            int fallback_max_slope,
                            int32_t ax, int32_t ay,
                            int32_t bx, int32_t by) {
    (void)world; (void)move_class; (void)fallback_max_slope;
    (void)ax; (void)ay; (void)bx; (void)by;
    return 1;
}

const MoveClassDef *TAK_MoveInfo_Find(const MoveInfoTable *table,
                                      const char *name) {
    (void)table; (void)name;
    return NULL;
}

static int seat_ok(int p) { return p >= 0 && p <= TAK_MAX_PLAYERS; }
float Economy_GetShare(const EconomyState *eco, int p) {
    (void)eco; return seat_ok(p) ? g_share[p] : 1.0f;
}
int32_t Economy_GetMana(const EconomyState *eco, int p) {
    (void)eco; return seat_ok(p) ? g_mana[p] : 0;
}
int32_t Economy_GetMaxMana(const EconomyState *eco, int p) {
    (void)eco; (void)p; return 2000;
}
int32_t Economy_GetIncome(const EconomyState *eco, int p) {
    (void)eco; return seat_ok(p) ? g_income[p] : 0;
}
int32_t Economy_GetSpend(const EconomyState *eco, int p) {
    (void)eco; return seat_ok(p) ? g_spend[p] : 0;
}

int VFS_ReadFile(const char *path, void **out_data, uint32_t *out_size) {
    (void)path; (void)out_data; (void)out_size;
    return -1;
}
const FeatureDef *Features_GetByIndex(int idx) { (void)idx; return NULL; }
int Units_FindDefByName(const char *unitname) { (void)unitname; return -1; }
void *tak_malloc(size_t n) { return malloc(n); }
void tak_free(void *p) { free(p); }

const UnitDef *Units_GetDef(int idx) {
    if (idx < 0 || idx >= MOCK_DEFS) return NULL;
    return &g_defs[idx];
}

GameWorld *World_Get(void) { return (GameWorld *)g_world; }

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_unit_count;
    return g_units;
}

/* A bucket grid, as the engine's, rebuilt when a unit comes or goes
 * or a tick passes. Answers in slot order within each bucket. */
#define GRID_SHIFT 8
#define GRID_W     (MAP_PX >> GRID_SHIFT)
static int g_grid_head[GRID_W * GRID_W];
static int g_grid_next[MOCK_UNITS];
static int g_grid_stale = 1;

static int grid_cell(int32_t v) {
    int c = v >> GRID_SHIFT;
    return c < 0 ? 0 : c >= GRID_W ? GRID_W - 1 : c;
}

static void grid_build(void) {
    for (int i = 0; i < GRID_W * GRID_W; i++) g_grid_head[i] = -1;
    for (int i = g_unit_count - 1; i >= 0; i--) {
        if (g_units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        int c = grid_cell(g_units[i].world_y) * GRID_W + grid_cell(g_units[i].world_x);
        g_grid_next[i] = g_grid_head[c];
        g_grid_head[c] = i;
    }
    g_grid_stale = 0;
}

int Units_Candidates(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int *out, int cap) {
    if (g_grid_stale) grid_build();
    int n = 0;
    for (int cy = grid_cell(y0); cy <= grid_cell(y1); cy++) {
        for (int cx = grid_cell(x0); cx <= grid_cell(x1); cx++) {
            for (int i = g_grid_head[cy * GRID_W + cx]; i >= 0; i = g_grid_next[i]) {
                const Unit *u = &g_units[i];
                if (u->world_x < x0 || u->world_x > x1 ||
                    u->world_y < y0 || u->world_y > y1) continue;
                if (n >= cap) return -1;
                out[n++] = i;
            }
        }
    }
    return n;
}
int Units_DebugGridQueries(void) { return 1; }

int Units_PlayerTeamId(int player_id) {
    if (!g_world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return player_id;
    const PlayerSlot *slot = &g_world->cfg.players[player_id - 1];
    if (slot->kind == TAK_SLOT_CLOSED) return 0;
    return slot->team > 0 ? slot->team : TAK_MAX_PLAYERS + player_id;
}

int Units_PlayersAreEnemies(int a, int b) {
    int ta = Units_PlayerTeamId(a);
    int tb = Units_PlayerTeamId(b);
    if (ta <= 0 || tb <= 0) return a != b;
    return ta != tb;
}

int Units_CanAttackTarget(int handle, int target_handle) {
    return handle >= 0 && target_handle >= 0 && handle != target_handle;
}

void Units_CommandMoveUnit(int handle, int32_t world_x, int32_t world_y) {
    log_i(1); log_i(handle); log_i(world_x); log_i(world_y);
    if (handle < 0 || handle >= g_unit_count) return;
    g_units[handle].cmd_kind = UNIT_CMD_MOVE;
    g_units[handle].cmd_x = world_x;
    g_units[handle].cmd_y = world_y;
    g_units[handle].target = -1;
}

/* The census runs with the remastered rules off, so neither is asked. */
int Units_OrderReclaimFeature(int handle, int32_t world_x, int32_t world_y,
                              int target_handle) {
    (void)handle; (void)world_x; (void)world_y; (void)target_handle;
    return 0;
}
int Features_InstanceCentre(const struct GameWorld *world, int idx,
                            int32_t *out_x, int32_t *out_y) {
    (void)world; (void)idx; (void)out_x; (void)out_y;
    return -1;
}

int Units_OrderStop(int handle) {
    log_i(2); log_i(handle);
    if (handle < 0 || handle >= g_unit_count) return 0;
    g_units[handle].cmd_kind = UNIT_CMD_NONE;
    g_units[handle].target = -1;
    return 1;
}

void Units_CommandAttackUnit(int handle, int target_handle) {
    log_i(3); log_i(handle); log_i(target_handle);
    if (handle < 0 || handle >= g_unit_count) return;
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    if (!Units_PlayersAreEnemies(g_units[handle].player_id,
                                 g_units[target_handle].player_id)) return;
    g_units[handle].cmd_kind = UNIT_CMD_ATTACK;
    g_units[handle].target = (int16_t)target_handle;
}

int Units_OrderSetAggro(int handle, int aggro_mode) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    g_units[handle].aggro_mode = (uint8_t)aggro_mode;
    return 1;
}

int Units_CanAnswer(int victim, int shooter) {
    (void)victim; (void)shooter;
    return 1;
}

void Units_StopUnit(int handle) {
    log_i(4); log_i(handle);
    if (handle < 0 || handle >= g_unit_count) return;
    g_units[handle].cmd_kind = UNIT_CMD_NONE;
    g_units[handle].target = -1;
    g_units[handle].build_target = -1;
}

int Units_GetBuildables(int builder_def_idx, int *out_def_idxs, int max_out) {
    if (builder_def_idx < 0 || builder_def_idx >= MOCK_DEFS || !out_def_idxs || max_out <= 0)
        return 0;
    int n = g_buildable_counts[builder_def_idx];
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; i++) out_def_idxs[i] = g_buildables[builder_def_idx][i];
    return n;
}

void Terrain_BlockingBegin(const struct GameWorld *world,
                           int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    (void)world; (void)x0; (void)y0; (void)x1; (void)y1;
}
void Terrain_BlockingEnd(void) {}

/* A checkerboard of 64 px squares where nothing may stand. */
int Units_IsBuildSiteClear(int def_idx, int32_t world_x, int32_t world_y) {
    (void)def_idx;
    if (world_x < 0 || world_y < 0 || world_x >= MAP_PX || world_y >= MAP_PX)
        return 0;
    return ((world_x >> 6) + (world_y >> 6)) % 3 != 0;
}

/* As units.c: the lowest dead slot, else a new one, under construction
 * and owned by the builder's seat. */
int Units_BeginBuildingForUnit(int builder_handle, int building_def_idx,
                               int32_t world_x, int32_t world_y) {
    log_i(5); log_i(builder_handle); log_i(building_def_idx);
    log_i(world_x); log_i(world_y);
    if (builder_handle < 0 || builder_handle >= g_unit_count) return -1;
    if (building_def_idx < 0 || building_def_idx >= MOCK_DEFS) return -1;
    int slot = -1;
    for (int i = 0; i < g_unit_count && slot < 0; i++)
        if (g_units[i].alive == UNIT_ALIVE_DEAD) slot = i;
    if (slot < 0) {
        if (g_unit_count >= MOCK_UNITS) return -1;
        slot = g_unit_count++;
        g_appended++;
    } else {
        g_reused++;
    }
    TAK_AI_ForgetUnit(slot);
    Unit *b = &g_units[builder_handle];
    Unit *u = &g_units[slot];
    memset(u, 0, sizeof(*u));
    u->alive = UNIT_ALIVE_ACTIVE;
    u->player_id = b->player_id;
    u->def_idx = (uint16_t)building_def_idx;
    u->under_construction = 1;
    u->world_x = world_x;
    u->world_y = world_y;
    u->health = 1;
    u->max_health = 100;
    u->target = -1;
    u->build_target = -1;
    b->cmd_kind = UNIT_CMD_BUILD;
    b->build_target = (int16_t)slot;
    b->cmd_x = world_x;
    b->cmd_y = world_y;
    g_grid_stale = 1;
    g_builds++;
    log_i(slot);
    return slot;
}

int Fog_IsVisibleForPlayer(const struct GameWorld *world, int player_id,
                           int32_t world_x, int32_t world_y) {
    (void)world;
    return ((world_x >> 9) + (world_y >> 9) + player_id) % 4 != 0;
}

/* ── The populations ──────────────────────────────────────────────── */

static uint32_t g_rng;
static uint32_t rnd(uint32_t n) {
    g_rng = g_rng * 1103515245u + 12345u;
    return n ? (g_rng >> 8) % n : 0;
}

static void def(int i, const char *name, const char *cat, int builder,
                float vel, int weapons) {
    UnitDef *d = &g_defs[i];
    memset(d, 0, sizeof(*d));
    strcpy(d->unitname, name);
    strcpy(d->category, cat);
    if (builder) d->cap_flags = UNIT_CAP_BUILDER;
    d->max_velocity = vel;
    d->num_weapons = weapons;
    d->worker_time = 10.0f;
    d->buildtime = 600.0f;
    d->build_cost = 50 + 25 * i;
    d->sight_distance = 200;
    d->max_health = 100;
    d->footprint_x = 2;
    d->footprint_z = 2;
    if (weapons) d->weapons[0].range = 60;
}

static void builds(int who, int n, const int *what) {
    g_buildable_counts[who] = n;
    for (int k = 0; k < n; k++) g_buildables[who][k] = what[k];
}

static void setup_defs(void) {
    memset(g_buildable_counts, 0, sizeof(g_buildable_counts));
    def(D_MONARCH, "TARKING", "TAR Monarch", 1, 1.5f, 1);
    g_defs[D_MONARCH].commander = 1;
    def(D_LODE, "TARLODE", "TAR", 0, 0.0f, 0);
    g_defs[D_LODE].mogrium_storage = 1000;
    g_defs[D_LODE].mogrium_income_per_sec = 10.0f;
    def(D_CASTLE, "TARCASTL", "TAR FACTORY", 1, 0.0f, 0);
    def(D_TOWER, "TARTOWER", "TAR DEFENSE", 0, 0.0f, 1);
    def(D_KEEP, "TARKEEP", "TAR FACTORY", 1, 0.0f, 0);
    def(D_TROOP, "TARTROOP", "TAR MELEE ATTACK", 0, 1.0f, 1);
    def(D_ARCHER, "TARARCH", "TAR RANGED ATTACK", 0, 1.2f, 1);
    def(D_WORKER, "TARWORK", "TAR BUILDER", 1, 1.0f, 0);
    def(D_HANDLER, "ZONHAND", "ZON", 1, 1.0f, 0);
    def(D_MANA, "TARMANA", "TAR", 0, 0.0f, 0);
    g_defs[D_MANA].max_mana = 500;
    def(D_DRAGON, "ZONDRAG", "ZON ATTACK", 0, 2.0f, 1);
    def(D_WALL, "TARWALL", "TAR", 0, 0.0f, 0);
    static const int monarch[] = { D_LODE, D_MANA, D_CASTLE, D_KEEP, D_TOWER, D_HANDLER, D_WALL };
    static const int castle[] = { D_TROOP, D_ARCHER, D_WORKER };
    static const int keep[] = { D_ARCHER, D_TROOP };
    static const int worker[] = { D_LODE, D_CASTLE, D_TOWER, D_KEEP, D_MANA };
    static const int handler[] = { D_DRAGON, D_WORKER };
    builds(D_MONARCH, 7, monarch);
    builds(D_CASTLE, 3, castle);
    builds(D_KEEP, 2, keep);
    builds(D_WORKER, 5, worker);
    builds(D_HANDLER, 2, handler);
}

static void setup_world(void) {
    memset(&g_world_store, 0, sizeof(g_world_store));
    GameWorld *w = &g_world_store;
    g_world = w;
    w->loaded = 1;
    w->skirmish_elapsed_ticks = 0;
    w->cfg.line_of_sight = 1;
    w->map_pixels_w = MAP_PX;
    w->map_pixels_h = MAP_PX;
    for (int p = 0; p < TAK_MAX_PLAYERS; p++) {
        w->cfg.players[p].kind = TAK_SLOT_AI;
        w->cfg.players[p].team = 0;
    }
}

static void place(Unit *u, int p, int d, uint32_t spread) {
    int32_t hx = 800 + ((p - 1) % 4) * 2000, hy = 1200 + ((p - 1) / 4) * 5000;
    u->player_id = (uint8_t)p;
    u->def_idx = (uint16_t)d;
    u->world_x = hx + (int32_t)rnd(spread) - (int32_t)spread / 2;
    u->world_y = hy + (int32_t)rnd(spread) - (int32_t)spread / 2;
    if (u->world_x < 0) u->world_x = 0;
    if (u->world_y < 0) u->world_y = 0;
    if (u->world_x >= MAP_PX) u->world_x = MAP_PX - 1;
    if (u->world_y >= MAP_PX) u->world_y = MAP_PX - 1;
}

/* A random population: each slot is live, dead, dying or carried; a
 * live one belongs to a seat or to none, stands built or as a frame,
 * and is idle or busy. Builders are spread over the seats. */
static void setup_units(uint32_t seed, int count, int builders_per_seat,
                        int only_those) {
    g_rng = seed * 2654435761u + 1u;
    memset(g_units, 0, sizeof(g_units));
    g_unit_count = count;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        g_mana[p] = (int32_t)rnd(1500);
        g_income[p] = (int32_t)rnd(40);
        g_spend[p] = (int32_t)rnd(40);
        g_share[p] = rnd(5) == 0 ? 0.5f : 1.0f;
    }
    static const int kinds[] = { D_LODE, D_LODE, D_MANA, D_TOWER,
                                 D_TROOP, D_TROOP, D_TROOP, D_ARCHER, D_ARCHER,
                                 D_DRAGON, D_WALL,
                                 D_CASTLE, D_KEEP, D_WORKER, D_HANDLER };
    /* The last four kinds are builders. */
    const int nk = (int)(sizeof(kinds) / sizeof(kinds[0])) - (only_those ? 4 : 0);
    for (int i = 0; i < count; i++) {
        Unit *u = &g_units[i];
        u->target = -1;
        u->build_target = -1;
        u->max_health = 100;
        u->health = 20 + (int)rnd(81);
        uint32_t life = rnd(20);
        u->alive = life < 16 ? UNIT_ALIVE_ACTIVE
                 : life < 18 ? UNIT_ALIVE_DEAD
                 : life < 19 ? UNIT_ALIVE_DYING : UNIT_ALIVE_TRANSPORTED;
        int p = rnd(30) == 0 ? 0 : 1 + (int)rnd(TAK_MAX_PLAYERS);
        place(u, p, kinds[rnd((uint32_t)nk)], 2400);
        if (rnd(8) == 0) u->under_construction = 1;
        if (rnd(6) == 0) {
            u->cmd_kind = UNIT_CMD_MOVE;
            u->cmd_x = u->world_x + 200;
            u->cmd_y = u->world_y;
        }
    }
    /* A monarch and the builders at the front, so a seat has them. */
    int next = 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS && next < count; p++) {
        for (int b = 0; b < builders_per_seat && next < count; b++, next++) {
            Unit *u = &g_units[next];
            int d = b == 0 ? D_MONARCH
                  : b % 3 == 1 ? D_WORKER
                  : b % 3 == 2 ? (b % 2 ? D_CASTLE : D_KEEP) : D_HANDLER;
            place(u, p, d, 600);
            u->alive = UNIT_ALIVE_ACTIVE;
            u->under_construction = 0;
            u->cmd_kind = UNIT_CMD_NONE;
        }
    }
    g_grid_stale = 1;
}

static void setup_profile(uint32_t seed) {
    g_rng = seed * 40503u + 7u;
    for (int d = 0; d < MOCK_DEFS; d++) {
        uint32_t r = rnd(6);
        TAK_AI_DebugSetLimit(d, r == 0 ? -1 : r == 1 ? 1 : (int)rnd(40));
        uint32_t wr = rnd(8);
        TAK_AI_DebugSetWeight(d, wr == 0 ? 0.0f : (float)(10 + rnd(91)));
    }
}

/* Between thinks: frames finish, units die and dead slots clear, busy
 * units go idle. Seeded, so the same orders give the same world. */
static void sim_step(void) {
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive == UNIT_ALIVE_DYING) { if (rnd(3) == 0) u->alive = UNIT_ALIVE_DEAD; continue; }
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        if (u->under_construction && rnd(40) == 0) u->under_construction = 0;
        if (rnd(900) == 0) { u->alive = UNIT_ALIVE_DYING; continue; }
        if (u->cmd_kind != UNIT_CMD_NONE && rnd(30) == 0) {
            u->cmd_kind = UNIT_CMD_NONE;
            u->build_target = -1;
            u->target = -1;
        }
    }
    g_grid_stale = 1;
}

static uint32_t hash_units(uint32_t h) {
    h = TAK_HashI32(h, g_unit_count);
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *u = &g_units[i];
        h = TAK_HashI32(h, u->alive);
        h = TAK_HashI32(h, u->player_id);
        h = TAK_HashI32(h, u->def_idx);
        h = TAK_HashI32(h, u->under_construction);
        h = TAK_HashI32(h, u->cmd_kind);
        h = TAK_HashI32(h, u->cmd_x);
        h = TAK_HashI32(h, u->cmd_y);
        h = TAK_HashI32(h, u->target);
        h = TAK_HashI32(h, u->build_target);
    }
    return h;
}

typedef struct {
    uint32_t hash;
    int builds, reused, appended;
} RunResult;

/* One match of `ticks` ticks from a fresh population. */
static RunResult run_match(uint32_t seed, int count, int builders, int ticks,
                           int census, int stagger) {
    setup_world();
    setup_defs();
    TAK_AI_ResetProfile();
    TAK_AI_BeginMatch(seed);
    setup_profile(seed);
    setup_units(seed, count, builders, 0);
    TAK_AI_DebugSetCensus(census);
    TAK_AI_DebugSetStagger(stagger);
    g_log = 2166136261u;
    g_builds = g_reused = g_appended = 0;
    g_rng = seed ^ 0x5bd1e995u;
    uint32_t h = TAK_SIM_HASH_SEED;
    for (int t = 0; t < ticks; t++) {
        g_world_store.skirmish_elapsed_ticks = t;
        TAK_AI_TickSkirmish(&g_world_store);
        h = TAK_HashU32(h, g_log);
        h = TAK_HashU32(h, TAK_AI_DebugStateHash());
        h = hash_units(h);
        for (int p = 1; p <= TAK_MAX_PLAYERS; p++)
            for (int k = 0; k < TAK_AI_COUNT_KINDS; k++)
                h = TAK_HashI32(h, TAK_AI_DebugCount(p, k));
        sim_step();
    }
    RunResult r = { h, g_builds, g_reused, g_appended };
    return r;
}

static int g_total_builds, g_total_reused, g_total_appended;

static int check_seed(uint32_t seed, int count, int builders, int ticks, int stagger) {
    RunResult off = run_match(seed, count, builders, ticks, 0, stagger);
    TAK_AI_DebugCensusCheck(1);
    RunResult chk = run_match(seed, count, builders, ticks, 1, stagger);
    int checks = 0;
    int bad = TAK_AI_DebugCensusMismatches(&checks);
    TAK_AI_DebugCensusCheck(0);
    RunResult on = run_match(seed, count, builders, ticks, 1, stagger);
    printf("seed %u: %d units, %d builders a seat, stagger %d: %d builds "
           "(%d reused, %d new slots), %d answers checked, %d differ, "
           "hash %08x %08x %08x\n", seed, count, builders, stagger,
           on.builds, on.reused, on.appended, checks, bad,
           off.hash, chk.hash, on.hash);
    if (bad != 0 || checks == 0) {
        fprintf(stderr, "FAIL: census answers differ from the scan\n");
        return 1;
    }
    if (off.hash != on.hash || chk.hash != on.hash ||
        off.builds != on.builds || off.reused != on.reused ||
        off.appended != on.appended) {
        fprintf(stderr, "FAIL: the census changed what the AI did\n");
        return 1;
    }
    g_total_builds += on.builds;
    g_total_reused += on.reused;
    g_total_appended += on.appended;
    return 0;
}

/* ── The benchmark ────────────────────────────────────────────────── */

extern double g_ai_prof_ms[7];
extern int    g_ai_prof_on;

static double now_ms(void) {
    return (double)SDL_GetPerformanceCounter() * 1000.0 /
           (double)SDL_GetPerformanceFrequency();
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* One think of every seat from the same population, `reps` times. */
static void bench(int census, int reps) {
    static Unit snapshot[MOCK_UNITS];
    int snap_count;
    setup_world();
    setup_defs();
    TAK_AI_ResetProfile();
    setup_profile(1);
    /* Weights and limits that let every draw go ahead. */
    for (int d = 0; d < MOCK_DEFS; d++) {
        TAK_AI_DebugSetLimit(d, -1);
        TAK_AI_DebugSetWeight(d, 50.0f);
    }
    setup_units(1, 8000, 25, 1);
    memcpy(snapshot, g_units, sizeof(Unit) * (size_t)g_unit_count);
    snap_count = g_unit_count;
    TAK_AI_DebugSetCensus(census);
    TAK_AI_DebugSetStagger(0);
    int builders = 0;
    for (int i = 0; i < g_unit_count; i++) {
        const UnitDef *d = Units_GetDef(g_units[i].def_idx);
        if (g_units[i].alive == UNIT_ALIVE_ACTIVE && g_units[i].player_id >= 1 &&
            d && (d->cap_flags & UNIT_CAP_BUILDER)) builders++;
    }
    double *ms = (double *)malloc(sizeof(double) * (size_t)reps);
    double part[7] = { 0 };
    int started = 0;
    for (int r = 0; r < reps; r++) {
        memcpy(g_units, snapshot, sizeof(Unit) * (size_t)snap_count);
        g_unit_count = snap_count;
        g_grid_stale = 1;
        TAK_AI_BeginMatch(1);
        g_world_store.skirmish_elapsed_ticks = 0;
        g_builds = 0;
        memset(g_ai_prof_ms, 0, sizeof(double) * 7);
        g_ai_prof_on = 1;
        double t0 = now_ms();
        TAK_AI_TickSkirmish(&g_world_store);
        ms[r] = now_ms() - t0;
        g_ai_prof_on = 0;
        for (int k = 0; k < 7; k++) part[k] += g_ai_prof_ms[k];
        started = g_builds;
    }
    qsort(ms, (size_t)reps, sizeof(double), cmp_double);
    printf("census %s: 8 seats, %d units, %d builders, %d builds started: "
           "one think of every seat median %.2f ms (min %.2f, max %.2f)\n"
           "  maps %.2f, threat %.2f, plan read %.2f, wave %.2f, break-offs %.2f, "
           "builders %.2f, fighters %.2f ms\n",
           census ? "on " : "off", snap_count, builders, started,
           ms[reps / 2], ms[0], ms[reps - 1], part[0] / reps, part[1] / reps,
           part[2] / reps, part[3] / reps, part[4] / reps, part[5] / reps,
           part[6] / reps);
    free(ms);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        int reps = argc > 2 ? atoi(argv[2]) : 15;
        if (reps < 1) reps = 1;
        bench(0, reps);
        bench(1, reps);
        return 0;
    }
    int fails = 0;
    /* Small and large populations, every seat on one tick and spread. */
    fails += check_seed(1, 400, 4, 240, 0);
    fails += check_seed(2, 900, 6, 240, 1);
    fails += check_seed(9, 1500, 8, 180, 0);
    fails += check_seed(10, 2500, 12, 120, 1);
    fails += check_seed(5, 600, 25, 300, 0);
    fails += check_seed(6, 300, 3, 600, 1);
    if (fails) return 1;
    /* Builds into dead slots and into new ones, or the stale census
     * and the counted tail went untested. */
    if (g_total_reused == 0 || g_total_appended == 0) {
        fprintf(stderr, "FAIL: %d builds, %d into dead slots, %d into new ones\n",
                g_total_builds, g_total_reused, g_total_appended);
        return 1;
    }
    printf("test_ai_census: ok\n");
    return 0;
}
