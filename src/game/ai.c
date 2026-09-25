#include "tak_ai.h"
#include "tak_terrain.h"
#include "tak_ai_influence.h"
#include "tak_ai_squad.h"
#include "tak_ai_htn.h"
#include "tak_ai_plan.h"
#include "tak_economy.h"
#include "tak_battle_config.h"
#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "tak_hpi.h"
#include "tak_features.h"
#include "tak_memory.h"
#include "tak_bytes.h"
#include "tak_pathing.h"
#include "tak_sim_hash.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AI profile: data/ai/*.txt console-cmd lines `weight <unit> <0-100>`
 * / `limit <unit> <n|-1>` (digs:ai). Unlisted types default 50 / -1. */
#define AI_MAX_DEFS 512
static float   g_ai_weight[AI_MAX_DEFS];
static int32_t g_ai_limit[AI_MAX_DEFS];
static int     g_ai_profile_loaded = 0;
static uint32_t g_ai_rng = 0x2A5F19C7u;

static uint32_t ai_rand(uint32_t n) {
    g_ai_rng = g_ai_rng * 1664525u + 1013904223u;   /* deterministic LCG */
    return n ? (g_ai_rng >> 8) % n : 0;
}

static int ai_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void TAK_AI_ResetProfile(void) { g_ai_profile_loaded = 0; }

static void ai_profile_load(void) {
    if (g_ai_profile_loaded) return;
    g_ai_profile_loaded = 1;
    for (int i = 0; i < AI_MAX_DEFS; i++) {
        g_ai_weight[i] = 50.0f;
        g_ai_limit[i]  = -1;
    }
    void *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile("data/ai/default.txt", &data, &size) != 0 || !data) {
        if (VFS_ReadFile("ai/default.txt", &data, &size) != 0 || !data)
            return;
    }
    char *txt = (char *)tak_malloc(size + 1);
    if (!txt) { tak_free(data); return; }
    memcpy(txt, data, size);
    txt[size] = '\0';
    tak_free(data);
    int n_weights = 0, n_limits = 0;
    for (char *line = strtok(txt, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (line[0] == '/' || line[0] == '\0') continue;
        char cmd[16], name[40];
        float val;
        if (sscanf(line, "%15s %39s %f", cmd, name, &val) != 3) continue;
        int def = Units_FindDefByName(name);
        if (def < 0 || def >= AI_MAX_DEFS) continue;
        if (ai_stricmp(cmd, "weight") == 0) {
            if (val < 0.0f) val = 0.0f;
            if (val > 100.0f) val = 100.0f;
            g_ai_weight[def] = val;
            n_weights++;
        } else if (ai_stricmp(cmd, "limit") == 0) {
            g_ai_limit[def] = (int32_t)val;
            n_limits++;
        }
    }
    tak_free(txt);
    fprintf(stderr, "AI profile: %d weights, %d limits\n",
            n_weights, n_limits);
}

static int ai_count_owned(const Unit *units, int unit_count,
                          int player_id, int def_idx) {
    int n = 0;
    for (int i = 0; i < unit_count; i++) {
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        if (units[i].player_id != player_id) continue;
        if ((int)units[i].def_idx == def_idx) n++;
    }
    return n;
}

static int ai_limit_allows(const Unit *units, int unit_count,
                           int player_id, int def_idx) {
    if (def_idx < 0 || def_idx >= AI_MAX_DEFS) return 1;
    int32_t lim = g_ai_limit[def_idx];
    if (lim < 0) return 1;
    return ai_count_owned(units, unit_count, player_id, def_idx) < lim;
}

/* The stall test: income under spend, or level with an empty pool
 * (legacy:19933-19934). */
static int ai_player_stalling(const GameWorld *world, int player_id) {
    int32_t diff = Economy_GetIncome(&world->economy, player_id)
                 - Economy_GetSpend(&world->economy, player_id);
    return diff < 0 ||
           (diff == 0 && Economy_GetMana(&world->economy, player_id) <= 0);
}

/* The original's build score for one type (legacy:19944-20054): 1, or
 * 21 armed. A type with the FBI builder key and none in the yard adds
 * a band by what the seat owns against the profile limit, cut while
 * stalling (legacy:19951-19971, key at legacy:162955). First of a type
 * x4, second x2, a ship x3 (legacy:20033-20041), capped at 100. The
 * mana building rules are the economy goal's, and the brake on types
 * costing over 500 (legacy:19980) is not in. */
static int32_t ai_build_score(const Unit *units, int unit_count,
                              int player_id, int def_idx) {
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d || d->is_feature) return 0;   /* legacy:20042 */
    int owned = 0, finished = 0;
    for (int i = 0; i < unit_count; i++) {
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        if (units[i].player_id != player_id) continue;
        if ((int)units[i].def_idx != def_idx) continue;
        owned++;
        if (!units[i].under_construction) finished++;
    }
    const GameWorld *world = World_Get();
    int32_t score = d->num_weapons > 0 ? 21 : 1;
    if ((d->cap_flags & UNIT_CAP_BUILDER) && owned == finished) {
        int stall = world ? ai_player_stalling(world, player_id) : 0;
        int32_t lim = (def_idx >= 0 && def_idx < AI_MAX_DEFS)
                    ? g_ai_limit[def_idx] : -1;
        if (lim < 3)               score += 15 / (stall ? 3 : 1);
        else if (lim / 4 >= owned) score += 30 / (stall + 1);
        else if (lim / 2 >= owned) score += 10 / (stall + 1);
        else                       score += 6 / (stall + 1);
    }
    if (owned == 0) score *= 4;
    else if (owned == 1) score *= 2;
    /* Only the ship classes give a minimum depth (legacy:163200). */
    if (world && d->movement_class[0]) {
        const MoveClassDef *mc = TAK_MoveInfo_Find(&world->moveinfo,
                                                   d->movement_class);
        if (mc && mc->min_water_depth > -1) score *= 3;
    }
    return score > 100 ? 100 : score;
}

/* What a draw weighs a type by: profile weight times build score over
 * 100 in whole numbers, so a low weight on a low score is never drawn
 * (legacy:21281-21294). */
static int32_t ai_desirability(const Unit *units, int unit_count,
                               int player_id, int def_idx) {
    int32_t w = (def_idx >= 0 && def_idx < AI_MAX_DEFS)
              ? (int32_t)g_ai_weight[def_idx] : 50;
    if (w <= 0) return 0;
    return w * ai_build_score(units, unit_count, player_id, def_idx) / 100;
}

int TAK_AI_ClampDifficulty(int difficulty) {
    if (difficulty < 0) return 0;
    if (difficulty > 3) return 3;
    return difficulty;
}

int TAK_AI_PursuitRadius(int sight_distance, int weapon_range, int difficulty) {
    static const int scale_pct[4] = { 50, 100, 150, 200 };
    int base = sight_distance > weapon_range ? sight_distance : weapon_range;
    if (base <= 0) return 0;
    difficulty = TAK_AI_ClampDifficulty(difficulty);
    return (base * scale_pct[difficulty] + 99) / 100;
}

static int ai_unit_is_monarch(const UnitDef *def) {
    if (!def) return 0;
    if (strstr(def->category, "Monarch")) return 1;
    if (strstr(def->unitname, "KING")) return 1;
    if (strstr(def->unitname, "QUEEN")) return 1;
    return 0;
}

static int ai_unit_can_fight_or_move(const Unit *u, const UnitDef *def) {
    if (!u || !def) return 0;
    if (u->alive != UNIT_ALIVE_ACTIVE) return 0;
    if (u->under_construction) return 0;
    return def->max_velocity > 0.0f ||
           def->num_weapons > 0 ||
           (def->cap_flags & UNIT_CAP_BUILDER);
}

static int ai_def_is_mana_economy(const UnitDef *def) {
    if (!def) return 0;
    if (ai_unit_is_monarch(def)) return 0;
    /* Nearly every TAK structure carries some mogrium storage (keeps
     * store 200, lodestones 1000), so raw mana fields can't classify a
     * building. A builder-capable structure's primary function is
     * production — treat only non-builders as economy buildings. */
    if (def->cap_flags & UNIT_CAP_BUILDER) return 0;
    if (def->max_mana > 0 || def->mogrium_storage > 0 ||
        def->mana_recharge_per_sec > 0.0f ||
        def->mogrium_income_per_sec > 0.0f) {
        return 1;
    }
    if (strstr(def->unitname, "LODE") || strstr(def->unitname, "MANA"))
        return 1;
    return 0;
}

static int ai_player_has_pending_economy_build(const Unit *units,
                                               int unit_count,
                                               int player_id) {
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != player_id) continue;
        if (!u->under_construction) continue;
        if (ai_def_is_mana_economy(Units_GetDef(u->def_idx))) return 1;
    }
    return 0;
}

static int ai_def_is_combat_unit(const UnitDef *def) {
    if (!def) return 0;
    if (def->max_velocity <= 0.0f) return 0;
    if (def->num_weapons > 0) return 1;
    if (strstr(def->category, "ATTACK")) return 1;
    return 0;
}

static int ai_player_has_pending_production_structure(const Unit *units,
                                                      int unit_count,
                                                      int player_id) {
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != player_id) continue;
        if (!u->under_construction) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!def || ai_def_is_mana_economy(def)) continue;
        if (def->max_velocity > 0.0f) continue;
        if (def->cap_flags & UNIT_CAP_BUILDER) return 1;
    }
    return 0;
}

static int ai_try_start_build_def(int actor_idx, int build_def);

static int ai_try_start_economy_build(const Unit *units,
                                      int unit_count,
                                      int actor_idx,
                                      const UnitDef *actor_def) {
    if (!units || actor_idx < 0 || actor_idx >= unit_count || !actor_def)
        return 0;
    const Unit *actor = &units[actor_idx];
    if (!(actor_def->cap_flags & UNIT_CAP_BUILDER)) return 0;
    if (actor->cmd_kind == UNIT_CMD_BUILD || actor->build_target >= 0) return 0;
    if (ai_player_has_pending_economy_build(units, unit_count,
                                            actor->player_id)) {
        return 0;
    }

    int buildables[32];
    int n = Units_GetBuildables((int)actor->def_idx, buildables,
                                (int)(sizeof(buildables) / sizeof(buildables[0])));
    int build_def = -1;
    for (int i = 0; i < n; i++) {
        const UnitDef *bd = Units_GetDef(buildables[i]);
        if (!ai_def_is_mana_economy(bd)) continue;
        /* Yardmap-'S' buildings are sited on a sacred site instead of
         * scattered round the builder (legacy:21427). The expansion
         * pass below owns them. */
        if (bd->yardmap_sacred) continue;
        build_def = buildables[i];
        break;
    }
    if (build_def < 0) return 0;
    return ai_try_start_build_def(actor_idx, build_def);
}

/* Route checks one site search may spend, and how near a spot that
 * just failed one a candidate is passed by without spending another. */
#define AI_SITE_REACH_CHECKS 12
#define AI_SITE_SKIP_PX      48
/* Sacred pads one expansion pass may route check. */
#define AI_PAD_REACH_CHECKS  3

static int ai_within(int32_t x, int32_t y, int32_t cx, int32_t cy, int32_t r);
static int ai_site_failed(int player_id, int32_t x, int32_t y, int now);
static void ai_remember_failed_site(int player_id, int32_t x, int32_t y,
                                    int now);

/* Whether the builder can walk to where it would work this site, by
 * the pathfinder on its own movement class, as the original checks
 * the site it chose before ordering the build (legacy:17327). The
 * route has to end inside the builder's working distance, because a
 * search that ran out of nodes hands back the way to its best cell. */
static int ai_site_reachable(const Unit *units, int actor_idx,
                             int build_def, int32_t x, int32_t y) {
    const GameWorld *world = World_Get();
    const Unit *actor = &units[actor_idx];
    const UnitDef *ad = Units_GetDef((int)actor->def_idx);
    const UnitDef *bd = Units_GetDef(build_def);
    if (!world || !ad) return 1;
    /* A structure builds where it stands and a flyer goes straight. */
    if (ad->max_velocity <= 0.0f || ad->can_fly) return 1;
    TAK_PathQuery q;
    memset(&q, 0, sizeof(q));
    q.move_class = ad->movement_class[0]
                 ? TAK_MoveInfo_Find(&world->moveinfo, ad->movement_class)
                 : NULL;
    q.fallback_max_slope = ad->max_slope;
    q.player_id = actor->player_id;
    q.self_plus1 = actor_idx + 1;
    q.compress = 1;
    TAK_Path path;
    int n = TAK_PathPlanQuery(world, actor->world_x, actor->world_y,
                              x, y, &q, &path);
    if (n <= 0) return 0;
    if (n > TAK_PATH_MAX_WAYPOINTS) n = TAK_PATH_MAX_WAYPOINTS;
    int fx = (bd && bd->footprint_x > 0) ? bd->footprint_x : 2;
    int fz = (bd && bd->footprint_z > 0) ? bd->footprint_z : 2;
    int32_t reach = ad->build_distance > 0 ? ad->build_distance : 32;
    return ai_within(path.x[n - 1], path.y[n - 1], x, y,
                     (fx + fz) * 8 + 12 + reach);
}

/* Where a tower's site search starts and how far it keeps from the
 * seat's other towers, set for the length of one tower build. The
 * original keeps a list of its own defences and a query for the ones
 * within a range of a point (legacy:20985), and nothing in it calls the
 * query, so what is done with the knowledge here is ours (A-009). */
#define AI_TOWER_REACH_PX   384
#define AI_TOWER_SPACING_PX 160
static int     g_ai_site_for_tower;
static int32_t g_ai_site_cx, g_ai_site_cy;

static int ai_def_is_tower(const UnitDef *def);

static int ai_near_own_tower(const Unit *units, int p, int32_t x, int32_t y) {
    int count = 0;
    (void)Units_GetActive(&count);
    for (int i = 0; i < count; i++) {
        const Unit *t = &units[i];
        if (t->alive != UNIT_ALIVE_ACTIVE || t->player_id != p) continue;
        if (!ai_def_is_tower(Units_GetDef(t->def_idx))) continue;
        if (ai_within(x, y, t->world_x, t->world_y, AI_TOWER_SPACING_PX))
            return 1;
    }
    return 0;
}

typedef struct AiSiteSearch {
    const Unit *units;
    int      actor_idx;
    int      build_def;
    int      now;
    int      checks;                  /* route checks left */
    int      bad_n;
    int32_t  bad_x[AI_SITE_REACH_CHECKS];
    int32_t  bad_y[AI_SITE_REACH_CHECKS];
} AiSiteSearch;

/* One candidate: 1 taken, 0 passed by, -1 the search is spent. */
static int ai_site_try(AiSiteSearch *s, int32_t x, int32_t y,
                       int32_t *out_x, int32_t *out_y) {
    if (s->checks <= 0) return -1;
    if (!Units_IsBuildSiteClear(s->build_def, x, y)) return 0;
    if (ai_site_failed(s->units[s->actor_idx].player_id, x, y, s->now))
        return 0;
    if (g_ai_site_for_tower &&
        ai_near_own_tower(s->units, s->units[s->actor_idx].player_id, x, y))
        return 0;
    for (int i = 0; i < s->bad_n; i++) {
        if (ai_within(x, y, s->bad_x[i], s->bad_y[i], AI_SITE_SKIP_PX))
            return 0;
    }
    s->checks--;
    if (ai_site_reachable(s->units, s->actor_idx, s->build_def, x, y)) {
        *out_x = x;
        *out_y = y;
        return 1;
    }
    s->bad_x[s->bad_n] = x;
    s->bad_y[s->bad_n] = y;
    s->bad_n++;
    return 0;
}

/* Expanding ring search for a build site around the builder,
 * tile-aligned to 16px attr cells. The legacy engine validates each
 * candidate with Terrain_FindBuildPlacement (legacy:219074 —
 * per-cell feature/occupancy/water/slope checks) and the AI retries
 * placements around its base until one validates; a fixed offset
 * table cannot site large structures (e.g. keeps are 8×20 tiles).
 * A site has to be clear, walkable to, and not one a builder of this
 * seat lately gave up on. */
static int ai_find_clear_site(const Unit *units, int actor_idx, int build_def,
                              int32_t *out_x, int32_t *out_y) {
    const UnitDef *bd = Units_GetDef(build_def);
    const GameWorld *world = World_Get();
    int32_t cx = units[actor_idx].world_x, cy = units[actor_idx].world_y;
    if (g_ai_site_for_tower) { cx = g_ai_site_cx; cy = g_ai_site_cy; }
    int fx = (bd && bd->footprint_x > 0) ? bd->footprint_x : 2;
    int fz = (bd && bd->footprint_z > 0) ? bd->footprint_z : 2;
    int larger = fx > fz ? fx : fz;
    const int step = 16;
    int start_r = larger * 8 + step;
    const int max_r = 768;
    AiSiteSearch s;
    memset(&s, 0, sizeof(s));
    s.units = units;
    s.actor_idx = actor_idx;
    s.build_def = build_def;
    s.now = world ? world->skirmish_elapsed_ticks : 0;
    s.checks = AI_SITE_REACH_CHECKS;
    /* Every footprint the rings can test, with a tile to spare for the
     * snap. */
    int reach = max_r + larger * 8 + 32;
    Terrain_BlockingBegin(world, cx - reach, cy - reach, cx + reach, cy + reach);
    int found = 0;
    for (int r = start_r; r <= max_r && !found; r += step) {
        int t = 0;
        for (int dx = -r; dx <= r && !t; dx += step) {
            if ((t = ai_site_try(&s, cx + dx, cy - r, out_x, out_y)) != 0) break;
            t = ai_site_try(&s, cx + dx, cy + r, out_x, out_y);
        }
        for (int dy = -r + step; dy <= r - step && !t; dy += step) {
            if ((t = ai_site_try(&s, cx - r, cy + dy, out_x, out_y)) != 0) break;
            t = ai_site_try(&s, cx + r, cy + dy, out_x, out_y);
        }
        if (t != 0) found = t;
    }
    Terrain_BlockingEnd();
    return found > 0;
}

static int ai_trace(void);

static int ai_try_start_build_def(int actor_idx, int build_def) {
    const Unit *units = Units_GetActive(NULL);
    if (!units) return 0;
    if (ai_trace()) {
        const UnitDef *tb = Units_GetDef(build_def);
        fprintf(stderr, "AI: start build def %d %s by actor %d\n",
                build_def, tb ? tb->unitname : "?", actor_idx);
    }
    const Unit *actor = &units[actor_idx];
    if (build_def < 0) return 0;
    /* Factory production: a structure producing a mobile unit creates
     * it at its own build spot (legacy:9342+ QueryBuildInfo →
     * Unit_Create), never at a caller-chosen map placement site.
     * Units_BeginBuildingForUnit handles the in-yard spawn. */
    const UnitDef *bd = Units_GetDef(build_def);
    const UnitDef *ad = Units_GetDef((int)actor->def_idx);
    if (bd && ad && bd->max_velocity > 0.0f && ad->max_velocity <= 0.0f) {
        return Units_BeginBuildingForUnit(actor_idx, build_def,
                                          actor->world_x,
                                          actor->world_y) >= 0;
    }
    int32_t bx, by;
    if (!ai_find_clear_site(units, actor_idx, build_def, &bx, &by)) return 0;
    return Units_BeginBuildingForUnit(actor_idx, build_def, bx, by) >= 0;
}

/* Weighted-random pick over qualifying entries, the original's
 * reservoir draw in whole numbers (legacy:21300, the draw itself at
 * legacy:21338-21341), honoring profile limits; falls back to the
 * rest if the pick can't start. */
static int ai_try_start_build_from_list(int actor_idx,
                                        const int *buildables,
                                        int buildable_count,
                                        int (*predicate)(const UnitDef *)) {
    if (!buildables || buildable_count <= 0 || !predicate) return 0;
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    if (!units) return 0;
    int player_id = units[actor_idx].player_id;

    int pick = -1;
    int32_t total = 0;
    for (int i = 0; i < buildable_count; i++) {
        const UnitDef *bd = Units_GetDef(buildables[i]);
        if (!predicate(bd)) continue;
        if (!ai_limit_allows(units, unit_count, player_id, buildables[i]))
            continue;
        int32_t w = ai_desirability(units, unit_count, player_id,
                                    buildables[i]);
        if (w <= 0) continue;
        total += w;
        if ((int32_t)ai_rand((uint32_t)total) < w) pick = i;
    }
    if (pick >= 0 && ai_try_start_build_def(actor_idx, buildables[pick]))
        return 1;
    for (int i = 0; i < buildable_count; i++) {
        if (i == pick) continue;
        const UnitDef *bd = Units_GetDef(buildables[i]);
        if (!predicate(bd)) continue;
        if (!ai_limit_allows(units, unit_count, player_id, buildables[i]))
            continue;
        if (ai_desirability(units, unit_count, player_id,
                            buildables[i]) <= 0) continue;
        if (ai_try_start_build_def(actor_idx, buildables[i])) return 1;
    }
    return 0;
}

/* TAK_AI_TRACE=1 logs build decisions to stderr. */
static int ai_trace(void) {
    static int v = -1;
    if (v < 0) v = getenv("TAK_AI_TRACE") ? 1 : 0;
    return v;
}

static int ai_try_start_production_structure_build(const Unit *units,
                                                   int unit_count,
                                                   int actor_idx,
                                                   const UnitDef *actor_def) {
    if (!units || actor_idx < 0 || actor_idx >= unit_count || !actor_def)
        return 0;
    const Unit *actor = &units[actor_idx];
    if (!(actor_def->cap_flags & UNIT_CAP_BUILDER)) return 0;
    if (actor->cmd_kind == UNIT_CMD_BUILD || actor->build_target >= 0) return 0;
    /* One structure frame at a time. A producer already standing is no
     * reason to stop: the original keeps adding producers up to the
     * profile's limit for the type (legacy:16254-16266), which the
     * candidate loop below checks. */
    if (ai_player_has_pending_production_structure(units, unit_count,
                                                   actor->player_id)) return 0;

    int buildables[32];
    int n = Units_GetBuildables((int)actor->def_idx, buildables,
                                (int)(sizeof(buildables) / sizeof(buildables[0])));
    /* Every candidate in list order, not just the first: a site search
     * can fail for one structure and succeed for the next. Profile
     * limits and weights apply as for any build (legacy:19859). */
    for (int i = 0; i < n; i++) {
        const UnitDef *bd = Units_GetDef(buildables[i]);
        if (!bd || ai_def_is_mana_economy(bd)) continue;
        if (!(bd->cap_flags & UNIT_CAP_BUILDER)) continue;
        int child_buildables[32];
        int cn = Units_GetBuildables(buildables[i], child_buildables,
                                     (int)(sizeof(child_buildables) / sizeof(child_buildables[0])));
        int produces_combat = 0;
        for (int c = 0; c < cn && !produces_combat; c++) {
            if (ai_def_is_combat_unit(Units_GetDef(child_buildables[c])))
                produces_combat = 1;
        }
        int allowed = ai_limit_allows(units, unit_count, actor->player_id, buildables[i]);
        int32_t w = ai_desirability(units, unit_count, actor->player_id, buildables[i]);
        if (ai_trace()) {
            fprintf(stderr, "AI: production candidate %s: children=%d combat=%d limit_ok=%d weight=%d\n",
                    bd->unitname, cn, produces_combat, allowed, (int)w);
        }
        if (!produces_combat || !allowed || w <= 0) continue;
        if (ai_try_start_build_def(actor_idx, buildables[i])) {
            if (ai_trace()) fprintf(stderr, "AI: started production structure %s\n", bd->unitname);
            return 1;
        }
        if (ai_trace()) fprintf(stderr, "AI: could not site %s, trying the next\n", bd->unitname);
    }
    return 0;
}

/* What a producer's draw covers: the troops in its list and the
 * walking builders with them, never a monarch. The original draws over
 * every mobile type in the list (legacy:21300-21343), and its build
 * score is what makes a builder likely early and rare later. */
static int ai_def_is_trainable(const UnitDef *def) {
    if (!def || def->max_velocity <= 0.0f) return 0;
    if (def->commander || ai_unit_is_monarch(def)) return 0;
    return ai_def_is_combat_unit(def) ||
           (def->cap_flags & UNIT_CAP_BUILDER) != 0;
}

static int ai_try_start_combat_production(const Unit *units,
                                          int unit_count,
                                          int actor_idx,
                                          const UnitDef *actor_def) {
    if (!units || actor_idx < 0 || actor_idx >= unit_count || !actor_def)
        return 0;
    const Unit *actor = &units[actor_idx];
    if (!(actor_def->cap_flags & UNIT_CAP_BUILDER)) return 0;
    if (actor->under_construction) return 0;
    /* One order per producer. The original asks the producer's own
     * mission list and never the player (legacy:17992-17997), so a
     * seat with many producers starts many units a pass. */
    if (actor->cmd_kind == UNIT_CMD_BUILD || actor->build_target >= 0) return 0;

    int buildables[32];
    int n = Units_GetBuildables((int)actor->def_idx, buildables,
                                (int)(sizeof(buildables) / sizeof(buildables[0])));
    return ai_try_start_build_from_list(actor_idx, buildables, n,
                                        ai_def_is_trainable);
}

/* A sacred pad this lodestone can take now, placed as the expansion
 * places it (legacy:21442): no mana building within 128 px, site clear,
 * and not one the seat remembers a builder failing to get to.
 * Only sacredsite features count, not the henge decor (legacy:20483). */
static int ai_pad_site(const GameWorld *world, const Unit *units,
                       int unit_count, int p, int feature_idx, int lode_def,
                       int32_t *out_x, int32_t *out_y) {
    const FeatureDef *fd =
        Features_GetByIndex(world->features[feature_idx].global_idx);
    if (!fd || fd->sacred_site <= 0.0f) return 0;
    const UnitDef *ld = Units_GetDef(lode_def);
    int lfx = (ld && ld->footprint_x > 0) ? ld->footprint_x : 2;
    int lfz = (ld && ld->footprint_z > 0) ? ld->footprint_z : 2;
    int32_t wx = world->features[feature_idx].tile_x * 16 + lfx * 8;
    int32_t wy = world->features[feature_idx].tile_z * 16 + lfz * 8;
    for (int u = 0; u < unit_count; u++) {
        if (units[u].alive != UNIT_ALIVE_ACTIVE) continue;
        if (!ai_def_is_mana_economy(Units_GetDef(units[u].def_idx))) continue;
        int64_t dx = (int64_t)units[u].world_x - wx;
        int64_t dy = (int64_t)units[u].world_y - wy;
        if (dx * dx + dy * dy < 128 * 128) return 0;
    }
    if (ai_site_failed(p, wx, wy, world->skirmish_elapsed_ticks)) return 0;
    /* A site the enemy is standing on in more strength than we are is
     * not one to send a builder to, and not a free one to plan on. The
     * original counts the enemies near a site against what it allows
     * before it goes (legacy:16680-16686). */
    if (AI_Influence_At(p, AI_INF_THREAT, wx, wy) >
        AI_Influence_At(p, AI_INF_PRESENCE, wx, wy)) return 0;
    if (!Units_IsBuildSiteClear(lode_def, wx, wy)) return 0;
    *out_x = wx;
    *out_y = wy;
    return 1;
}

/* Expansion (legacy:21427 → :20447): send an idle mobile builder to
 * the nearest sacred site that passes placement and build its
 * lodestone on the pad itself. A blocked pad is skipped, never built
 * beside, because off the pad it is invalid (legacy:20483). */
static int ai_try_expand_to_sacred_site(const GameWorld *world,
                                        const Unit *units, int unit_count,
                                        int actor_idx,
                                        const UnitDef *actor_def) {
    if (!world || !actor_def) return 0;
    if (!(actor_def->cap_flags & UNIT_CAP_BUILDER)) return 0;
    if (actor_def->max_velocity <= 0.0f) return 0;
    const Unit *actor = &units[actor_idx];
    if (actor->cmd_kind == UNIT_CMD_BUILD || actor->build_target >= 0)
        return 0;

    int buildables[32];
    int n = Units_GetBuildables((int)actor->def_idx, buildables, 32);
    int lode = -1;
    for (int i = 0; i < n; i++) {
        const UnitDef *bd = Units_GetDef(buildables[i]);
        /* The yardmap picks the sacred search, exactly as legacy
         * dispatches on the first yardmap code (legacy:21427). */
        if (bd && bd->yardmap_sacred) { lode = buildables[i]; break; }
    }
    if (lode < 0) return 0;
    if (!ai_limit_allows(units, unit_count, actor->player_id, lode)) return 0;

    /* The nearest pad the builder can walk to (legacy:17327). One it
     * cannot is remembered as failed, which takes it off the plan's
     * count of free sites as well, and the next nearest is tried. */
    for (int tries = 0; tries < AI_PAD_REACH_CHECKS; tries++) {
        int64_t best_d2 = INT64_MAX;
        int32_t best_x = 0, best_y = 0;
        for (int i = 0; i < world->feature_count; i++) {
            int32_t wx, wy;
            if (!ai_pad_site(world, units, unit_count, actor->player_id, i,
                             lode, &wx, &wy)) continue;
            int64_t dx = (int64_t)actor->world_x - wx;
            int64_t dy = (int64_t)actor->world_y - wy;
            int64_t d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best_x = wx; best_y = wy; }
        }
        if (best_d2 == INT64_MAX) return 0;
        if (ai_site_reachable(units, actor_idx, lode, best_x, best_y)) {
            return Units_BeginBuildingForUnit(actor_idx, lode,
                                              best_x, best_y) >= 0;
        }
        ai_remember_failed_site(actor->player_id, best_x, best_y,
                                world->skirmish_elapsed_ticks);
    }
    return 0;
}

static int64_t ai_dist2_units(const Unit *a, const Unit *b) {
    int64_t dx = (int64_t)b->world_x - a->world_x;
    int64_t dy = (int64_t)b->world_y - a->world_y;
    return dx * dx + dy * dy;
}

/* ── Strategic layer ──────────────────────────────────────────────────
 *
 * One record per player slot. Bases are tracked for every open slot so
 * an AI can answer a hit on an ally's base; only AI slots act. */

/* A hit this close to the base counts as an attack on it, and units
 * this close to home answer it. Wave units further out keep going. */
#define AI_BASE_RADIUS    1280
#define AI_DEFEND_RADIUS  1536
/* A wave that never reaches its launch strength goes anyway on
 * this cadence, so a seat whose production has stopped still
 * presses. */
#define AI_WAVE_PATIENCE  1800
/* A base threat lapses this long after the last hit (60 Hz ticks). */
#define AI_THREAT_TTL     600
/* Engagement radius floor: 5 x range class cells x 16 px, class 10
 * for mobiles and 5 for builders (legacy:17400, :17428). A unit below
 * a quarter HP divides it by 5 with a 160 px floor (legacy:17705). */
#define AI_ENGAGE_MOBILE  800
#define AI_ENGAGE_BUILDER 400
#define AI_ENGAGE_HURT_MIN 160
/* Build sites a seat remembers failing at: how many, for how long
 * (60 Hz ticks), and how near one a new site may not be. */
#define AI_FAILED_SITES       16
#define AI_FAILED_SITE_TTL    3600
#define AI_FAILED_SITE_RADIUS 128

typedef struct AiPlayer {
    int      active;          /* AI slot */
    int      base_known;
    int32_t  base_x, base_y;
    /* Wave target (legacy:15365 pick, legacy:18250 march). */
    int      target_player;
    int      target_handle;
    uint32_t target_stable_id;
    int32_t  target_x, target_y;
    int      target_reachable;   /* a walker has a route to it */
    /* The enemy strength last seen around the target, which target it
     * was seen around and when. A wave that has been thrown back does
     * not forget what threw it the moment the fog closes. */
    int32_t  target_strength;
    uint32_t target_strength_id;
    int      target_strength_tick;   /* -1 = never seen */
    /* Last enemy that hit something at the base. */
    int      threat_player;
    int      threat_handle;
    uint32_t threat_stable_id;
    int32_t  threat_x, threat_y;
    int      threat_tick;     /* -1 = none */
    int      threat_pending;
    int      threat_from_map; /* read off the influence map, not a hit */
    /* The monarch's build think waits after it takes a hit (legacy:15092). */
    int      build_freeze_until;
    int      freeze_pending;
    /* Sites a builder gave up on or had no route to, each until its
     * tick, written round robin. */
    int32_t  fail_x[AI_FAILED_SITES];
    int32_t  fail_y[AI_FAILED_SITES];
    int      fail_until[AI_FAILED_SITES];
    int      fail_next;
} AiPlayer;

static AiPlayer g_ai_players[TAK_MAX_PLAYERS + 1];
/* [from][to][0 = attack, 1 = march], read by tests. */
static int g_ai_orders[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1][2];
static int g_ai_defence_orders[TAK_MAX_PLAYERS + 1];
static int g_ai_last_tick = -1;
static uint32_t g_ai_seed = 0x2A5F19C7u;

/* Seats told to fight without A-007, for measuring. Debug only. */
static int g_ai_tactics_off[TAK_MAX_PLAYERS + 1];

/* ── Groups ──────────────────────────────────────────────────────────
 *
 * The original gathers a seat's spare fighters into numbered groups,
 * each with its own members, launch threshold, target and mode, odd
 * ids from 21 for attack groups and from 51 for raid groups
 * (legacy:16187 forms them, legacy:18250 runs them). A seat here keeps
 * up to AI_GROUPS of them. A unit's group is kept by handle and
 * cleared when the handle is forgotten. */
#define AI_GROUPS     4
#define AI_MEMBER_CAP 2048

enum { AI_GROUP_FREE = 0, AI_GROUP_FORMING, AI_GROUP_MARCHING };
enum { AI_GROUP_ATTACK = 0, AI_GROUP_RAID };

typedef struct AiGroup {
    int      mode;
    int      kind;
    int      launch;          /* members it went out with */
    int      target_player;
    int      target_handle;   /* -1 for a raid, which goes at a point */
    uint32_t target_stable_id;
    int32_t  target_x, target_y;
    int      formed_tick;
} AiGroup;

static AiGroup g_ai_groups[TAK_MAX_PLAYERS + 1][AI_GROUPS];
static int g_ai_counts[TAK_MAX_PLAYERS + 1][TAK_AI_COUNT_KINDS];
static uint8_t g_ai_member[AI_MEMBER_CAP];      /* slot + 1, 0 for none */

static int ai_group_name(const AiGroup *g, int slot) {
    return (g->kind == AI_GROUP_RAID ? 51 : 21) + 2 * slot;
}

static void ai_reset_state(void) {
    memset(g_ai_players, 0, sizeof(g_ai_players));
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        g_ai_players[p].target_handle = -1;
        g_ai_players[p].target_reachable = 1;
        g_ai_players[p].threat_handle = -1;
        g_ai_players[p].threat_tick = -1;
        g_ai_players[p].target_strength_tick = -1;
    }
    memset(g_ai_orders, 0, sizeof(g_ai_orders));
    memset(g_ai_defence_orders, 0, sizeof(g_ai_defence_orders));
    memset(g_ai_tactics_off, 0, sizeof(g_ai_tactics_off));
    memset(g_ai_groups, 0, sizeof(g_ai_groups));
    memset(g_ai_member, 0, sizeof(g_ai_member));
    memset(g_ai_counts, 0, sizeof(g_ai_counts));
    g_ai_rng = g_ai_seed;     /* derived from the session seed */
    AI_Influence_Reset();
}

void TAK_AI_BeginMatch(uint32_t seed) {
    g_ai_seed = 0x2A5F19C7u ^ (seed * 0x9E3779B1u);
    ai_reset_state();
    g_ai_last_tick = -1;
}

/* The AI's share of the simulation hash. Its state is file static
 * here, so the composite in src/game/sim_hash.c calls in rather than
 * reaching across. Covers the generator, the last tick the AI ran, the
 * per player records and the order matrices.
 *
 * g_ai_last_tick is the one to watch. TAK_AI_TickSkirmish resets every
 * target, threat and order when the tick count did not climb, and
 * neither World_End nor Units_ClearInstances touches it, so a load that
 * does not restore the actual variable quietly amnesias the AI on its
 * first step. */
uint32_t TAK_SimHash_AI(uint32_t h) {
    h = TAK_HashU32(h, g_ai_rng);
    h = TAK_HashI32(h, g_ai_last_tick);
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        const AiPlayer *a = &g_ai_players[p];
        h = TAK_HashI32(h, a->active);
        h = TAK_HashI32(h, a->base_known);
        h = TAK_HashI32(h, a->base_x);
        h = TAK_HashI32(h, a->base_y);
        h = TAK_HashI32(h, a->target_player);
        h = TAK_HashI32(h, a->target_handle);
        h = TAK_HashU32(h, a->target_stable_id);
        h = TAK_HashI32(h, a->target_x);
        h = TAK_HashI32(h, a->target_y);
        h = TAK_HashI32(h, a->threat_player);
        h = TAK_HashI32(h, a->threat_handle);
        h = TAK_HashU32(h, a->threat_stable_id);
        h = TAK_HashI32(h, a->threat_x);
        h = TAK_HashI32(h, a->threat_y);
        h = TAK_HashI32(h, a->threat_tick);
        h = TAK_HashI32(h, a->threat_pending);
        h = TAK_HashI32(h, a->threat_from_map);
        h = TAK_HashI32(h, a->build_freeze_until);
        h = TAK_HashI32(h, a->freeze_pending);
        h = TAK_HashI32(h, g_ai_defence_orders[p]);
        for (int q = 0; q <= TAK_MAX_PLAYERS; q++) {
            h = TAK_HashI32(h, g_ai_orders[p][q][0]);
            h = TAK_HashI32(h, g_ai_orders[p][q][1]);
        }
        h = TAK_HashI32(h, a->fail_next);
        for (int k = 0; k < AI_FAILED_SITES; k++) {
            h = TAK_HashI32(h, a->fail_x[k]);
            h = TAK_HashI32(h, a->fail_y[k]);
            h = TAK_HashI32(h, a->fail_until[k]);
        }
        h = TAK_HashI32(h, a->target_reachable);
        h = TAK_HashI32(h, a->target_strength);
        h = TAK_HashU32(h, a->target_strength_id);
        h = TAK_HashI32(h, a->target_strength_tick);
        for (int s = 0; s < AI_GROUPS; s++) {
            const AiGroup *g = &g_ai_groups[p][s];
            h = TAK_HashI32(h, g->mode);
            h = TAK_HashI32(h, g->kind);
            h = TAK_HashI32(h, g->launch);
            h = TAK_HashI32(h, g->target_player);
            h = TAK_HashI32(h, g->target_handle);
            h = TAK_HashU32(h, g->target_stable_id);
            h = TAK_HashI32(h, g->target_x);
            h = TAK_HashI32(h, g->target_y);
            h = TAK_HashI32(h, g->formed_tick);
        }
    }
    /* Membership, four handles to a word. */
    for (int i = 0; i < AI_MEMBER_CAP; i += 4) {
        h = TAK_HashU32(h, (uint32_t)g_ai_member[i] |
                           ((uint32_t)g_ai_member[i + 1] << 8) |
                           ((uint32_t)g_ai_member[i + 2] << 16) |
                           ((uint32_t)g_ai_member[i + 3] << 24));
    }
    return h;
}

/* ── The AI in a save ────────────────────────────────────────────
 *
 * The same fields TAK_SimHash_AI covers, in the same order, written at
 * explicit widths through tak_bytes.h. Nothing here is a struct handed
 * to a write call, so the 32 bit Windows build, the wasm32 browser
 * build and the 64 bit builds all read each other's saves. */

#define AI_SAVE_PLAYER_FIELDS 19
#define AI_SAVE_PLAYERS       (TAK_MAX_PLAYERS + 1)
#define AI_SAVE_HEAD          8u
#define AI_SAVE_PER_PLAYER    ((AI_SAVE_PLAYER_FIELDS + 1) * 4u)
#define AI_SAVE_ORDERS        ((uint32_t)AI_SAVE_PLAYERS *                                (uint32_t)AI_SAVE_PLAYERS * 2u * 4u)
#define AI_SAVE_BYTES         (AI_SAVE_HEAD +                                (uint32_t)AI_SAVE_PLAYERS * AI_SAVE_PER_PLAYER +                                AI_SAVE_ORDERS)

/* The failed sites follow the order matrices, so a save from before
 * them is this one's prefix: fail_next, then x, y and until a site.
 * The wave target's reachability is the tail after those, so a save
 * from before it is a prefix of this one in the same way. */
#define AI_SAVE_FAIL_PER_PLAYER ((AI_FAILED_SITES * 3 + 1) * 4u)
#define AI_SAVE_FAIL_BYTES    (AI_SAVE_BYTES + (uint32_t)AI_SAVE_PLAYERS * AI_SAVE_FAIL_PER_PLAYER)
#define AI_SAVE_REACH_BYTES   (AI_SAVE_FAIL_BYTES + (uint32_t)AI_SAVE_PLAYERS * 4u)
/* And the strength last seen at the target, three words a seat. */
#define AI_SAVE_STRENGTH_BYTES (AI_SAVE_REACH_BYTES + (uint32_t)AI_SAVE_PLAYERS * 12u)
/* And the groups, nine words each, with who is in which. */
#define AI_SAVE_FULL_BYTES    (AI_SAVE_STRENGTH_BYTES + \
                               (uint32_t)AI_SAVE_PLAYERS * AI_GROUPS * 36u + \
                               (uint32_t)AI_MEMBER_CAP)

unsigned int TAK_AI_StateBytes(void) { return (unsigned int)AI_SAVE_FULL_BYTES; }

void TAK_AI_SaveState(unsigned char *out) {
    if (!out) return;
    uint8_t *p = (uint8_t *)out;
    tak_put_u32(p + 0, g_ai_rng);
    tak_put_i32(p + 4, g_ai_last_tick);
    p += AI_SAVE_HEAD;
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        const AiPlayer *a = &g_ai_players[q];
        tak_put_i32(p + 0,  a->active);
        tak_put_i32(p + 4,  a->base_known);
        tak_put_i32(p + 8,  a->base_x);
        tak_put_i32(p + 12, a->base_y);
        tak_put_i32(p + 16, a->target_player);
        tak_put_i32(p + 20, a->target_handle);
        tak_put_u32(p + 24, a->target_stable_id);
        tak_put_i32(p + 28, a->target_x);
        tak_put_i32(p + 32, a->target_y);
        tak_put_i32(p + 36, a->threat_player);
        tak_put_i32(p + 40, a->threat_handle);
        tak_put_u32(p + 44, a->threat_stable_id);
        tak_put_i32(p + 48, a->threat_x);
        tak_put_i32(p + 52, a->threat_y);
        tak_put_i32(p + 56, a->threat_tick);
        tak_put_i32(p + 60, a->threat_pending);
        tak_put_i32(p + 64, a->threat_from_map);
        tak_put_i32(p + 68, a->build_freeze_until);
        tak_put_i32(p + 72, a->freeze_pending);
        tak_put_i32(p + 76, g_ai_defence_orders[q]);
        p += AI_SAVE_PER_PLAYER;
    }
    for (int from = 0; from < AI_SAVE_PLAYERS; from++) {
        for (int to = 0; to < AI_SAVE_PLAYERS; to++) {
            tak_put_i32(p + 0, g_ai_orders[from][to][0]);
            tak_put_i32(p + 4, g_ai_orders[from][to][1]);
            p += 8;
        }
    }
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        const AiPlayer *a = &g_ai_players[q];
        tak_put_i32(p, a->fail_next);
        p += 4;
        for (int k = 0; k < AI_FAILED_SITES; k++) {
            tak_put_i32(p + 0, a->fail_x[k]);
            tak_put_i32(p + 4, a->fail_y[k]);
            tak_put_i32(p + 8, a->fail_until[k]);
            p += 12;
        }
    }
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        tak_put_i32(p, g_ai_players[q].target_reachable);
        p += 4;
    }
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        const AiPlayer *a = &g_ai_players[q];
        tak_put_i32(p + 0, a->target_strength);
        tak_put_u32(p + 4, a->target_strength_id);
        tak_put_i32(p + 8, a->target_strength_tick);
        p += 12;
    }
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        for (int s = 0; s < AI_GROUPS; s++) {
            const AiGroup *g = &g_ai_groups[q][s];
            tak_put_i32(p + 0,  g->mode);
            tak_put_i32(p + 4,  g->kind);
            tak_put_i32(p + 8,  g->launch);
            tak_put_i32(p + 12, g->target_player);
            tak_put_i32(p + 16, g->target_handle);
            tak_put_u32(p + 20, g->target_stable_id);
            tak_put_i32(p + 24, g->target_x);
            tak_put_i32(p + 28, g->target_y);
            tak_put_i32(p + 32, g->formed_tick);
            p += 36;
        }
    }
    memcpy(p, g_ai_member, AI_MEMBER_CAP);
}

int TAK_AI_LoadState(const unsigned char *in, unsigned int len) {
    if (!in || len < AI_SAVE_BYTES) return -1;
    const uint8_t *p = (const uint8_t *)in;
    g_ai_rng = tak_get_u32(p + 0);
    g_ai_last_tick = tak_get_i32(p + 4);
    p += AI_SAVE_HEAD;
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        AiPlayer *a = &g_ai_players[q];
        a->active             = tak_get_i32(p + 0);
        a->base_known         = tak_get_i32(p + 4);
        a->base_x             = tak_get_i32(p + 8);
        a->base_y             = tak_get_i32(p + 12);
        a->target_player      = tak_get_i32(p + 16);
        a->target_handle      = tak_get_i32(p + 20);
        a->target_stable_id   = tak_get_u32(p + 24);
        a->target_x           = tak_get_i32(p + 28);
        a->target_y           = tak_get_i32(p + 32);
        a->threat_player      = tak_get_i32(p + 36);
        a->threat_handle      = tak_get_i32(p + 40);
        a->threat_stable_id   = tak_get_u32(p + 44);
        a->threat_x           = tak_get_i32(p + 48);
        a->threat_y           = tak_get_i32(p + 52);
        a->threat_tick        = tak_get_i32(p + 56);
        a->threat_pending     = tak_get_i32(p + 60);
        a->threat_from_map    = tak_get_i32(p + 64);
        a->build_freeze_until = tak_get_i32(p + 68);
        a->freeze_pending     = tak_get_i32(p + 72);
        g_ai_defence_orders[q] = tak_get_i32(p + 76);
        p += AI_SAVE_PER_PLAYER;
    }
    for (int from = 0; from < AI_SAVE_PLAYERS; from++) {
        for (int to = 0; to < AI_SAVE_PLAYERS; to++) {
            g_ai_orders[from][to][0] = tak_get_i32(p + 0);
            g_ai_orders[from][to][1] = tak_get_i32(p + 4);
            p += 8;
        }
    }
    /* A save from before the failed sites carries none. */
    int has_fails = len >= AI_SAVE_FAIL_BYTES;
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        AiPlayer *a = &g_ai_players[q];
        a->fail_next = 0;
        memset(a->fail_x, 0, sizeof(a->fail_x));
        memset(a->fail_y, 0, sizeof(a->fail_y));
        memset(a->fail_until, 0, sizeof(a->fail_until));
        if (!has_fails) continue;
        a->fail_next = tak_get_i32(p);
        p += 4;
        for (int k = 0; k < AI_FAILED_SITES; k++) {
            a->fail_x[k]     = tak_get_i32(p + 0);
            a->fail_y[k]     = tak_get_i32(p + 4);
            a->fail_until[k] = tak_get_i32(p + 8);
            p += 12;
        }
    }
    /* A save from before the reachability flag marched everywhere. */
    int has_reach = len >= AI_SAVE_REACH_BYTES;
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        if (!has_reach) { g_ai_players[q].target_reachable = 1; continue; }
        g_ai_players[q].target_reachable = tak_get_i32(p);
        p += 4;
    }
    /* A save from before the tactical layer has seen nothing yet. */
    int has_strength = len >= AI_SAVE_STRENGTH_BYTES;
    for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
        AiPlayer *a = &g_ai_players[q];
        a->target_strength = 0;
        a->target_strength_id = 0;
        a->target_strength_tick = -1;
        if (!has_strength) continue;
        a->target_strength      = tak_get_i32(p + 0);
        a->target_strength_id   = tak_get_u32(p + 4);
        a->target_strength_tick = tak_get_i32(p + 8);
        p += 12;
    }
    /* A save from before the groups has every fighter ungrouped, and
     * the next think gathers them again. */
    memset(g_ai_groups, 0, sizeof(g_ai_groups));
    memset(g_ai_member, 0, sizeof(g_ai_member));
    if (len >= AI_SAVE_FULL_BYTES) {
        for (int q = 0; q < AI_SAVE_PLAYERS; q++) {
            for (int s = 0; s < AI_GROUPS; s++) {
                AiGroup *g = &g_ai_groups[q][s];
                g->mode             = tak_get_i32(p + 0);
                g->kind             = tak_get_i32(p + 4);
                g->launch           = tak_get_i32(p + 8);
                g->target_player    = tak_get_i32(p + 12);
                g->target_handle    = tak_get_i32(p + 16);
                g->target_stable_id = tak_get_u32(p + 20);
                g->target_x         = tak_get_i32(p + 24);
                g->target_y         = tak_get_i32(p + 28);
                g->formed_tick      = tak_get_i32(p + 32);
                p += 36;
            }
        }
        memcpy(g_ai_member, p, AI_MEMBER_CAP);
    }
    /* The influence maps are rebuilt on the next think and are not in
     * the file. Dropping the stale ones keeps a load from planning
     * against ground it read a session ago. */
    AI_Influence_Reset();
    return 0;
}

/* The movement tests want one number for the AI's state. It is the
 * composite's AI share seeded on its own, so the repo keeps a single
 * simulation hash rather than a second one that can drift from it. */
unsigned int TAK_AI_DebugStateHash(void) {
    return (unsigned int)TAK_SimHash_AI(TAK_SIM_HASH_SEED);
}

int TAK_AI_DebugHostileOrders(int from_player, int to_player, int attacks_only) {
    if (from_player < 1 || from_player > TAK_MAX_PLAYERS) return 0;
    if (to_player < 1 || to_player > TAK_MAX_PLAYERS) return 0;
    int n = g_ai_orders[from_player][to_player][0];
    if (!attacks_only) n += g_ai_orders[from_player][to_player][1];
    return n;
}

int TAK_AI_DebugDefenceOrders(int player_id) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    return g_ai_defence_orders[player_id];
}

int TAK_AI_DebugAttackPlayer(int player_id) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    return g_ai_players[player_id].target_player;
}

int TAK_AI_DebugWaveTarget(int player_id) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return -1;
    return g_ai_players[player_id].target_handle;
}

int TAK_AI_DebugWaveTargetReachable(int player_id) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return 1;
    return g_ai_players[player_id].target_reachable;
}

/* Said for tests and the trace only: it is read off the plan each
 * think and decides nothing, so it is in neither the hash nor a save. */
static const char *g_ai_wave_reason[TAK_MAX_PLAYERS + 1];

void TAK_AI_DebugSetTactics(int player_id, int mask) {
    if (player_id < 0 || player_id > TAK_MAX_PLAYERS) return;
    g_ai_tactics_off[player_id] = TAK_AI_TACTIC_ALL & ~mask;
}

const char *TAK_AI_DebugWaveReason(int player_id) {
    if (player_id < 0 || player_id > TAK_MAX_PLAYERS) return "";
    return g_ai_wave_reason[player_id] ? g_ai_wave_reason[player_id] : "";
}

int TAK_AI_DebugGroupOf(int handle) {
    if (handle < 0 || handle >= AI_MEMBER_CAP || !g_ai_member[handle]) return 0;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (!units || handle >= count) return 0;
    int p = units[handle].player_id, slot = g_ai_member[handle] - 1;
    if (p < 0 || p > TAK_MAX_PLAYERS || slot >= AI_GROUPS) return 0;
    return ai_group_name(&g_ai_groups[p][slot], slot);
}

int TAK_AI_DebugCount(int player_id, int kind) {
    if (player_id < 0 || player_id > TAK_MAX_PLAYERS) return 0;
    if (kind < 0 || kind >= TAK_AI_COUNT_KINDS) return 0;
    return g_ai_counts[player_id][kind];
}

int TAK_AI_DebugGroupMode(int player_id, int group_name) {
    if (player_id < 0 || player_id > TAK_MAX_PLAYERS) return 0;
    for (int s = 0; s < AI_GROUPS; s++) {
        const AiGroup *g = &g_ai_groups[player_id][s];
        if (g->mode != AI_GROUP_FREE && ai_group_name(g, s) == group_name)
            return g->mode;
    }
    return 0;
}

void TAK_AI_DebugSetWaveTarget(int player_id, int handle) {
    if (player_id < 0 || player_id > TAK_MAX_PLAYERS) return;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (!units || handle < 0 || handle >= count) return;
    AiPlayer *ap = &g_ai_players[player_id];
    ap->target_player = units[handle].player_id;
    ap->target_handle = handle;
    ap->target_stable_id = units[handle].stable_id;
    ap->target_x = units[handle].world_x;
    ap->target_y = units[handle].world_y;
    ap->target_reachable = 1;
}

static int ai_valid_player(const GameWorld *world, int p) {
    return world && p >= 1 && p <= TAK_MAX_PLAYERS &&
           world->cfg.players[p - 1].kind != TAK_SLOT_CLOSED;
}

static int ai_players_allied(int a, int b) {
    if (a == b) return 0;
    int ta = Units_PlayerTeamId(a);
    int tb = Units_PlayerTeamId(b);
    return ta > 0 && ta == tb;
}

static int ai_within(int32_t x, int32_t y, int32_t cx, int32_t cy, int32_t r) {
    int64_t dx = (int64_t)x - cx;
    int64_t dy = (int64_t)y - cy;
    return dx * dx + dy * dy <= (int64_t)r * r;
}

/* Whether the seat still remembers failing at a site near this one. */
static int ai_site_failed(int player_id, int32_t x, int32_t y, int now) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    const AiPlayer *ap = &g_ai_players[player_id];
    for (int k = 0; k < AI_FAILED_SITES; k++) {
        if (ap->fail_until[k] <= now) continue;
        if (ai_within(x, y, ap->fail_x[k], ap->fail_y[k],
                      AI_FAILED_SITE_RADIUS)) return 1;
    }
    return 0;
}

/* A site remembered already has its time renewed, so one bad spot
 * never fills the list. */
static void ai_remember_failed_site(int player_id, int32_t x, int32_t y,
                                    int now) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return;
    AiPlayer *ap = &g_ai_players[player_id];
    int slot = -1;
    for (int k = 0; k < AI_FAILED_SITES && slot < 0; k++) {
        if (ap->fail_until[k] > now &&
            ap->fail_x[k] == x && ap->fail_y[k] == y) slot = k;
    }
    if (slot < 0) {
        slot = (int)((uint32_t)ap->fail_next % AI_FAILED_SITES);
        ap->fail_next = (slot + 1) % AI_FAILED_SITES;
    }
    ap->fail_x[slot] = x;
    ap->fail_y[slot] = y;
    ap->fail_until[slot] = now + AI_FAILED_SITE_TTL;
}

int TAK_AI_DebugFindSite(int actor_idx, int build_def, int32_t *x, int32_t *y) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (!units || actor_idx < 0 || actor_idx >= count || !x || !y) return 0;
    return ai_find_clear_site(units, actor_idx, build_def, x, y);
}

int TAK_AI_DebugFailedSites(int player_id) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    const GameWorld *w = World_Get();
    int now = w ? w->skirmish_elapsed_ticks : 0;
    int n = 0;
    for (int k = 0; k < AI_FAILED_SITES; k++) {
        if (g_ai_players[player_id].fail_until[k] > now) n++;
    }
    return n;
}

/* Octagonal distance, the shape the original's target scorer uses. */
static int32_t ai_approx_dist(int64_t dx, int64_t dy) {
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int64_t d = dx > dy ? dx + dy / 2 : dy + dx / 2;
    return d > INT32_MAX ? INT32_MAX : (int32_t)d;
}

static int ai_visible_to(const GameWorld *world, int p, const Unit *t) {
    if (!world->cfg.line_of_sight) return 1;
    return Fog_IsVisibleForPlayer(world, p, t->world_x, t->world_y);
}

static int ai_def_is_mobile_combat(const UnitDef *def) {
    return def && !(def->cap_flags & UNIT_CAP_BUILDER) &&
           ai_def_is_combat_unit(def);
}

/* Home is the start position: the map datum the AI builds around.
 * Without one (mission maps) fall back to the monarch, then any
 * structure. */
static void ai_update_bases(const GameWorld *world, const Unit *units,
                            int unit_count) {
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        AiPlayer *ap = &g_ai_players[p];
        ap->active = world->cfg.players[p - 1].kind == TAK_SLOT_AI;
        if (!ai_valid_player(world, p)) { ap->base_known = 0; continue; }
        int found = 0;
        for (int s = 0; s < world->num_start_positions && !found; s++) {
            const StartPos *sp = &world->start_positions[s];
            if (sp->player != p) continue;
            ap->base_x = sp->x * 16;
            ap->base_y = sp->z * 16;
            found = 1;
        }
        int structure = -1;
        for (int i = 0; i < unit_count && !found; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != p) continue;
            const UnitDef *d = Units_GetDef(u->def_idx);
            if (!d) continue;
            if (ai_unit_is_monarch(d)) {
                ap->base_x = u->world_x;
                ap->base_y = u->world_y;
                found = 1;
            } else if (structure < 0 && d->max_velocity <= 0.0f) {
                structure = i;
            }
        }
        if (!found && structure >= 0) {
            ap->base_x = units[structure].world_x;
            ap->base_y = units[structure].world_y;
            found = 1;
        }
        ap->base_known = found;
    }
}

/* units.c reports every enemy hit here. A hit near the base becomes
 * the base threat. A hit on the monarch arms its build freeze and drops
 * its build (legacy:15087-15100). The rest waits for the next AI tick. */
void TAK_AI_ForgetUnit(int handle) {
    if (handle < 0) return;
    if (handle < AI_MEMBER_CAP) g_ai_member[handle] = 0;
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        AiPlayer *ap = &g_ai_players[p];
        if (ap->target_handle == handle) {
            ap->target_handle = -1;
            ap->target_stable_id = 0;
        }
        if (ap->threat_handle == handle) {
            ap->threat_handle = -1;
            ap->threat_stable_id = 0;
            ap->threat_player = 0;
            ap->threat_tick = -1;
            ap->threat_from_map = 0;
        }
    }
}

void TAK_AI_NotifyDamage(int victim_handle, int shooter_handle) {
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    if (!units) return;
    if (victim_handle < 0 || victim_handle >= unit_count) return;
    if (shooter_handle < 0 || shooter_handle >= unit_count) return;
    const Unit *v = &units[victim_handle];
    const Unit *s = &units[shooter_handle];
    int p = v->player_id;
    if (p < 1 || p > TAK_MAX_PLAYERS) return;
    if (s->player_id < 1 || s->player_id > TAK_MAX_PLAYERS) return;
    if (!Units_PlayersAreEnemies(p, s->player_id)) return;
    AiPlayer *ap = &g_ai_players[p];
    const UnitDef *vd = Units_GetDef(v->def_idx);
    /* The seat is read from the world, never from a record an earlier
     * skirmish left behind: a mission map runs no AI tick. */
    const GameWorld *w = World_Get();
    int ai_seat = w && w->loaded && w->mission.objective_count == 0 &&
                  w->mission.placement_count == 0 &&
                  w->cfg.players[p - 1].kind == TAK_SLOT_AI;
    if (ai_seat && vd && vd->commander) {
        ap->freeze_pending = 1;
        /* With the build dropped, the return fire that follows answers
         * the shooter (legacy:15113-15170). */
        if (v->cmd_kind == UNIT_CMD_BUILD) Units_StopUnit(victim_handle);
    }
    if (!ap->base_known) return;
    if (!ai_within(v->world_x, v->world_y, ap->base_x, ap->base_y,
                   AI_BASE_RADIUS)) {
        return;
    }
    ap->threat_player = s->player_id;
    ap->threat_handle = shooter_handle;
    ap->threat_stable_id = s->stable_id;
    ap->threat_x = s->world_x;
    ap->threat_y = s->world_y;
    ap->threat_pending = 1;
}

/* units.c reports a mover that gave its order up. A build order's site
 * goes into the seat's list of failed sites. The seat is read from the
 * world, as it is for a hit. */
void TAK_AI_NotifyGiveUp(int handle) {
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    if (!units || handle < 0 || handle >= unit_count) return;
    const Unit *u = &units[handle];
    if (u->cmd_kind != UNIT_CMD_BUILD) return;
    int p = u->player_id;
    if (p < 1 || p > TAK_MAX_PLAYERS) return;
    const GameWorld *w = World_Get();
    if (!w || !w->loaded || w->mission.objective_count != 0 ||
        w->mission.placement_count != 0 ||
        w->cfg.players[p - 1].kind != TAK_SLOT_AI) return;
    ai_remember_failed_site(p, u->cmd_x, u->cmd_y, w->skirmish_elapsed_ticks);
}

/* Map-driven threat (A-002): the most exposed cell, one where seen
 * enemy strength outweighs our presence and something of ours stands,
 * names the first seen enemy inside it. Refreshed while it lasts. */
static int ai_map_threat(const GameWorld *world, const Unit *units,
                         int unit_count, int p, int now) {
    AiPlayer *ap = &g_ai_players[p];
    int w, h;
    AI_Influence_Size(&w, &h);
    int32_t best = 0;
    int bcx = -1, bcy = -1;
    for (int cy = 0; cy < h; cy++) {
        for (int cx = 0; cx < w; cx++) {
            int32_t e = AI_Influence_Exposure(p, cx << AI_INF_CELL_SHIFT,
                                              cy << AI_INF_CELL_SHIFT);
            if (e > best) { best = e; bcx = cx; bcy = cy; }
        }
    }
    if (bcx < 0) return 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *t = &units[i];
        if (t->alive != UNIT_ALIVE_ACTIVE) continue;
        if (!ai_valid_player(world, t->player_id)) continue;
        if (!Units_PlayersAreEnemies(p, t->player_id)) continue;
        int cx, cy;
        if (!AI_Influence_CellOf(t->world_x, t->world_y, &cx, &cy)) continue;
        if (cx != bcx || cy != bcy) continue;
        if (!ai_visible_to(world, p, t)) continue;
        ap->threat_player = t->player_id;
        ap->threat_handle = i;
        ap->threat_stable_id = t->stable_id;
        ap->threat_x = t->world_x;
        ap->threat_y = t->world_y;
        ap->threat_tick = now;
        ap->threat_from_map = 1;
        return 1;
    }
    return 0;
}

/* A reported hit becomes the live threat, which lapses after the TTL
 * or with its attacker. Runs for every open seat, so an AI ally sees a
 * hit on a human's base too. */
static void ai_promote_threat(const GameWorld *world, const Unit *units,
                              int unit_count, int p, int now) {
    AiPlayer *ap = &g_ai_players[p];
    if (ap->threat_pending) {
        ap->threat_tick = now;
        ap->threat_pending = 0;
        ap->threat_from_map = 0;
    }
    if (ap->threat_tick < 0) return;
    int h = ap->threat_handle;
    int live = h >= 0 && h < unit_count &&
               units[h].alive == UNIT_ALIVE_ACTIVE &&
               units[h].stable_id == ap->threat_stable_id &&
               Units_PlayersAreEnemies(p, units[h].player_id);
    if (!live || now - ap->threat_tick > AI_THREAT_TTL) {
        ap->threat_tick = -1;
        ap->threat_handle = -1;
        ap->threat_player = 0;
        ap->threat_from_map = 0;
    } else if (ai_visible_to(world, p, &units[h])) {
        ap->threat_x = units[h].world_x;
        ap->threat_y = units[h].world_y;
    }
}

static void ai_update_threat(const GameWorld *world, const Unit *units,
                             int unit_count, int p, int now) {
    AiPlayer *ap = &g_ai_players[p];
    ai_promote_threat(world, units, unit_count, p, now);
    /* A hit outranks the map, a lapsed or map-born threat follows it. */
    if (ap->threat_tick < 0 || ap->threat_from_map) {
        ai_map_threat(world, units, unit_count, p, now);
    }
}

/* Own threat first. With none, an ally's (the original only exempts
 * allies from targeting, legacy:15365, it has no help rule). */
static const AiPlayer *ai_effective_threat(const GameWorld *world, int p,
                                           int *out_is_allied) {
    *out_is_allied = 0;
    if (g_ai_players[p].threat_tick >= 0) return &g_ai_players[p];
    for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
        if (!ai_valid_player(world, q) || !ai_players_allied(p, q)) continue;
        if (g_ai_players[q].threat_tick < 0) continue;
        *out_is_allied = 1;
        return &g_ai_players[q];
    }
    return NULL;
}

/* Movement classes one pick asks about (A-005). */
#define AI_WAVE_PROBERS 4

/* Whether a unit's own ground reaches a spot, off the cached
 * connected ground rather than a search (legacy:15473, legacy:18385,
 * A-005). Terrain only, and yes wherever it cannot be sure. */
static int ai_unit_ground_reaches(const Unit *u, const UnitDef *d,
                                  int32_t x, int32_t y) {
    const GameWorld *world = World_Get();
    if (!world || !u || !d) return 1;
    if (d->can_fly || d->max_velocity <= 0.0f) return 1;
    const MoveClassDef *mc = d->movement_class[0]
                           ? TAK_MoveInfo_Find(&world->moveinfo,
                                               d->movement_class)
                           : NULL;
    return TAK_PathGroundConnected(world, mc, d->max_slope,
                                   u->world_x, u->world_y, x, y);
}

/* A spot some walker of the seat reaches, asked one walker a
 * movement class because they do not share ground (A-005). With
 * no walkers nothing marches and the answer is yes. */
static int ai_wave_route_ok(const Unit *units, const int *probers,
                            int n_probers, int32_t x, int32_t y) {
    if (n_probers <= 0) return 1;
    for (int k = 0; k < n_probers; k++) {
        const Unit *u = &units[probers[k]];
        if (ai_unit_ground_reaches(u, Units_GetDef((int)u->def_idx),
                                   x, y)) return 1;
    }
    return 0;
}

/* Wave target (legacy:15365): every unit of every non-allied player is
 * scored INT_MAX over two randomly smoothed distances, then divided
 * for being unseen (2..20), unarmed (2..10), unfinished (1..3),
 * immobile (1..3) and for its owner's small share of the world's
 * units (1..20). Best reachable score wins: the original takes a
 * candidate only when the pathfinder answers for the attacker
 * (legacy:15473), so a target the army cannot walk to never raises
 * the best. With nothing reachable the best of the rest is kept for
 * the seat's flyers and the walkers hold (A-005). */
static void ai_pick_wave_target(const GameWorld *world, const Unit *units,
                                int unit_count, int p) {
    AiPlayer *ap = &g_ai_players[p];
    int64_t sx = 0, sy = 0;
    int mine = 0;
    int counts[TAK_MAX_PLAYERS + 1] = { 0 };
    int total = 0;
    int probers[AI_WAVE_PROBERS];
    int n_probers = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        if (u->player_id >= 1 && u->player_id <= TAK_MAX_PLAYERS) {
            counts[u->player_id]++;
            total++;
        }
        if (u->player_id != p || u->under_construction) continue;
        const UnitDef *ud = Units_GetDef(u->def_idx);
        if (!ai_def_is_mobile_combat(ud)) continue;
        /* One walking fighter per movement class, as the original
         * asks the attacker it is scoring for. */
        if (n_probers < AI_WAVE_PROBERS && !ud->can_fly &&
            ud->max_velocity > 0.0f) {
            int seen = 0;
            for (int k = 0; k < n_probers && !seen; k++) {
                const UnitDef *pd =
                    Units_GetDef((int)units[probers[k]].def_idx);
                if (pd && pd->max_slope == ud->max_slope &&
                    ai_stricmp(pd->movement_class,
                               ud->movement_class) == 0) seen = 1;
            }
            if (!seen) probers[n_probers++] = i;
        }
        sx += u->world_x;
        sy += u->world_y;
        mine++;
    }
    int32_t cx = ap->base_x, cy = ap->base_y;
    int32_t rx = ap->base_x, ry = ap->base_y;
    if (mine > 0) {
        cx = (int32_t)(sx / mine);
        cy = (int32_t)(sy / mine);
        /* The second distance runs from one random member. */
        int pick = (int)ai_rand((uint32_t)mine);
        for (int i = 0; i < unit_count; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != p) continue;
            if (u->under_construction) continue;
            if (!ai_def_is_mobile_combat(Units_GetDef(u->def_idx))) continue;
            if (pick-- == 0) { rx = u->world_x; ry = u->world_y; break; }
        }
    }
    int best = -1;
    int32_t best_score = 0;
    int alt = -1;
    int32_t alt_score = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *t = &units[i];
        if (t->alive != UNIT_ALIVE_ACTIVE) continue;
        int q = t->player_id;
        if (!ai_valid_player(world, q) || !Units_PlayersAreEnemies(p, q)) continue;
        const UnitDef *td = Units_GetDef(t->def_idx);
        if (!td) continue;
        /* A wall scores nothing (legacy:20042). */
        if (td->is_feature) continue;
        uint32_t d1 = (uint32_t)ai_approx_dist((int64_t)t->world_x - cx,
                                               (int64_t)t->world_y - cy);
        uint32_t d2 = (uint32_t)ai_approx_dist((int64_t)t->world_x - rx,
                                               (int64_t)t->world_y - ry);
        int32_t a = (int32_t)((ai_rand(d1 / 8) + ai_rand(d1 / 8)) / 4) + 1;
        int32_t b = (int32_t)((ai_rand(d2 / 2) + ai_rand(d2 / 2)) / 4) + 1;
        int32_t score = (INT32_MAX / a) / b;
        if (!ai_visible_to(world, p, t))
            score /= (int32_t)(ai_rand(10) + 1 + ai_rand(10));
        if (td->num_weapons <= 0)
            score /= (int32_t)(ai_rand(5) + 1 + ai_rand(5));
        if (t->under_construction)
            score /= (int32_t)(ai_rand(3) + 1);
        if (td->max_velocity <= 0.0f)
            score /= (int32_t)(ai_rand(3) + 1);
        int share = total / (counts[q] < 20 ? 20 : counts[q]);
        if (share > 20) share = 20;
        score /= (int32_t)(ai_rand((uint32_t)share) + 1);
        /* A-002: tilt toward cells where the enemy is weak and
         * valuable, a quarter to four times the original's score. */
        int32_t tilt = AI_Influence_Weakness(p, t->world_x, t->world_y) / 8;
        if (tilt < -6) tilt = -6;
        if (tilt > 24) tilt = 24;
        int64_t tilted = (int64_t)score * (8 + tilt) / 8;
        score = tilted > INT32_MAX ? INT32_MAX : (int32_t)tilted;
        if (score > alt_score) { alt_score = score; alt = i; }
        if (score <= best_score) continue;
        if (!ai_wave_route_ok(units, probers, n_probers,
                              t->world_x, t->world_y)) continue;
        best_score = score;
        best = i;
    }
    int reachable = 1;
    if (best < 0) { best = alt; reachable = 0; }
    if (best < 0) {
        ap->target_player = 0;
        ap->target_handle = -1;
        ap->target_reachable = 1;
        return;
    }
    ap->target_player = units[best].player_id;
    ap->target_handle = best;
    ap->target_stable_id = units[best].stable_id;
    ap->target_x = units[best].world_x;
    ap->target_y = units[best].world_y;
    ap->target_reachable = reachable;
    if (ai_trace()) {
        fprintf(stderr, "AI %d: wave target player %d unit %d at (%d,%d)"
                " reachable %d\n",
                p, ap->target_player, best, ap->target_x, ap->target_y,
                reachable);
    }
}

/* A live target is kept and its position followed. The original
 * re-picks 1 pass in 250 (legacy:18250) but every new group picks
 * afresh; with one wave target per player, 1 tick in 30 keeps waves
 * rotating between enemies. */
static void ai_update_wave_target(const GameWorld *world, const Unit *units,
                                  int unit_count, int p) {
    AiPlayer *ap = &g_ai_players[p];
    int h = ap->target_handle;
    int live = h >= 0 && h < unit_count &&
               units[h].alive == UNIT_ALIVE_ACTIVE &&
               units[h].stable_id == ap->target_stable_id &&
               Units_PlayersAreEnemies(p, units[h].player_id);
    if (live && ai_rand(30) != 0) {
        ap->target_x = units[h].world_x;
        ap->target_y = units[h].world_y;
        return;
    }
    ai_pick_wave_target(world, units, unit_count, p);
}

static int32_t ai_engage_radius(const Unit *u, const UnitDef *def) {
    int32_t reach = def->sight_distance;
    for (int w = 0; w < def->num_weapons; w++) {
        if (def->weapons[w].range > reach) reach = def->weapons[w].range;
    }
    int32_t r = (def->cap_flags & UNIT_CAP_BUILDER) ? AI_ENGAGE_BUILDER
                                                     : AI_ENGAGE_MOBILE;
    if (u->max_health > 0 && u->health < u->max_health / 4) {
        r /= 5;
        if (r < AI_ENGAGE_HURT_MIN) r = AI_ENGAGE_HURT_MIN;
    }
    return reach + r;
}

static void ai_count_order(int from, int to, int kind) {
    if (from < 1 || from > TAK_MAX_PLAYERS) return;
    if (to < 1 || to > TAK_MAX_PLAYERS) return;
    g_ai_orders[from][to][kind]++;
}

/* Combat think (legacy:17624): a unit with no fight on its hands takes
 * the nearest seen enemy inside max(sight, range) plus its engagement
 * radius. Marching units divert too, as the original's do. */
static int ai_engage_nearby(const GameWorld *world, const Unit *units,
                            int unit_count, int actor_idx,
                            const UnitDef *def) {
    const Unit *u = &units[actor_idx];
    if (u->cmd_kind != UNIT_CMD_NONE && u->cmd_kind != UNIT_CMD_MOVE) return 0;
    int32_t r = ai_engage_radius(u, def);
    int64_t best_d2 = (int64_t)r * r + 1;
    int best = -1;
    for (int i = 0; i < unit_count; i++) {
        const Unit *t = &units[i];
        if (i == actor_idx || t->alive != UNIT_ALIVE_ACTIVE) continue;
        if (!ai_valid_player(world, t->player_id)) continue;
        if (!Units_PlayersAreEnemies(u->player_id, t->player_id)) continue;
        int64_t d2 = ai_dist2_units(u, t);
        if (d2 >= best_d2) continue;
        if (!ai_visible_to(world, u->player_id, t)) continue;
        if (!Units_CanAttackTarget(actor_idx, i)) continue;
        best_d2 = d2;
        best = i;
    }
    if (best < 0) return 0;
    Units_CommandAttackUnit(actor_idx, best);
    ai_count_order(u->player_id, units[best].player_id, 0);
    return 1;
}

/* Base defence: units at home answer the last hit on the base, or on
 * an ally's base when their own is quiet. Only idle units leave for
 * an ally. Our addition, see docs/MANUAL_DEVIATIONS.md A-001. */
static int ai_defend(const GameWorld *world, const Unit *units,
                     int unit_count, int actor_idx, int p,
                     const AiPlayer *threat, int allied) {
    const AiPlayer *ap = &g_ai_players[p];
    const Unit *u = &units[actor_idx];
    if (!ap->base_known) return 0;
    /* Units at home answer, and so do units already near the threat
     * (an expansion under attack). */
    if (!ai_within(u->world_x, u->world_y, ap->base_x, ap->base_y,
                   AI_DEFEND_RADIUS) &&
        !ai_within(u->world_x, u->world_y, threat->threat_x,
                   threat->threat_y, AI_DEFEND_RADIUS)) {
        return 0;
    }
    if (u->cmd_kind == UNIT_CMD_ATTACK) return 1;   /* already fighting */
    if (allied && u->cmd_kind != UNIT_CMD_NONE) return 0;
    int h = threat->threat_handle;
    if (h >= 0 && h < unit_count && ai_visible_to(world, p, &units[h]) &&
        Units_CanAttackTarget(actor_idx, h)) {
        Units_CommandAttackUnit(actor_idx, h);
        ai_count_order(p, threat->threat_player, 0);
        g_ai_defence_orders[p]++;
        return 1;
    }
    if (u->cmd_kind == UNIT_CMD_MOVE &&
        u->cmd_x == threat->threat_x && u->cmd_y == threat->threat_y) {
        return 1;
    }
    Units_CommandMoveUnit(actor_idx, threat->threat_x, threat->threat_y);
    ai_count_order(p, threat->threat_player, 1);
    g_ai_defence_orders[p]++;
    return 1;
}

/* Wave dispatch (legacy:18250): attack the target when it can be seen,
 * else march on its position. A walker whose own ground does not
 * reach the target is left where it is, the way the original drops a
 * member the pathfinder answers nothing for (legacy:18385). A flyer
 * goes either way. */
static void ai_dispatch_at(const GameWorld *world, const Unit *units,
                           int unit_count, int actor_idx, int p,
                           const UnitDef *def, int target_player, int h,
                           int32_t tx, int32_t ty) {
    if (h < 0 || h >= unit_count) return;
    if (!ai_unit_ground_reaches(&units[actor_idx], def, tx, ty)) return;
    if (ai_visible_to(world, p, &units[h]) &&
        Units_CanAttackTarget(actor_idx, h)) {
        Units_CommandAttackUnit(actor_idx, h);
        ai_count_order(p, target_player, 0);
        return;
    }
    Units_CommandMoveUnit(actor_idx, tx, ty);
    ai_count_order(p, target_player, 1);
}

static void ai_dispatch_wave(const GameWorld *world, const Unit *units,
                             int unit_count, int actor_idx, int p,
                             const UnitDef *def) {
    const AiPlayer *ap = &g_ai_players[p];
    ai_dispatch_at(world, units, unit_count, actor_idx, p, def,
                   ap->target_player, ap->target_handle, ap->target_x,
                   ap->target_y);
}

/* ── A squad on the march (A-010) ───────────────────────────────────
 * Two charges steer a member of an attack group that is out. Far from
 * the target, the group pulls back a member that has run ahead of it,
 * which stands until the rest close. Near it, a ranged member takes a
 * firing position on the side where the enemy it can see stands
 * thinnest (src/game/ai_squad.c). flank picks which of the two runs.
 * 1 when it gave the member its order for this think. */
#define AI_SQUAD_ENGAGE 480
#define AI_SQUAD_RANGED 150
#define AI_SQUAD_CHARGES 64

static int32_t ai_squad_dist(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int64_t dx = (int64_t)ax - bx, dy = (int64_t)ay - by;
    uint64_t v = (uint64_t)(dx * dx + dy * dy), r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int32_t)r;
}

static int ai_squad_step(const GameWorld *world, const Unit *units,
                         int unit_count, int i, int p, const UnitDef *def,
                         int slot, int flank) {
    if (slot < 0 || slot >= AI_GROUPS) return 0;
    const AiGroup *g = &g_ai_groups[p][slot];
    if (g->mode != AI_GROUP_MARCHING || g->kind != AI_GROUP_ATTACK) return 0;
    if (g_ai_tactics_off[p] & TAK_AI_TACTIC_SQUAD) return 0;
    const Unit *u = &units[i];
    int32_t tx = g->target_x, ty = g->target_y;
    int32_t d = ai_squad_dist(u->world_x, u->world_y, tx, ty);
    if ((d > AI_SQUAD_ENGAGE) == flank) return 0;
    if (!flank) {
        int n = 0;
        int64_t sum = 0;
        for (int j = 0; j < unit_count && j < AI_MEMBER_CAP; j++) {
            if (g_ai_member[j] != slot + 1 || units[j].player_id != p) continue;
            if (units[j].alive != UNIT_ALIVE_ACTIVE) continue;
            sum += ai_squad_dist(units[j].world_x, units[j].world_y, tx, ty);
            n++;
        }
        int moving = u->cmd_kind == UNIT_CMD_MOVE;
        if (!AI_Squad_ShouldWait(d, sum, n, moving)) return 0;
        if (moving) {
            Units_OrderStop(i);
            g_ai_counts[p][TAK_AI_COUNT_SQUAD_WAITS]++;
        }
        return 1;
    }
    if (def->num_weapons <= 0 || def->weapons[0].range < AI_SQUAD_RANGED)
        return 0;
    AiSquadCharge charges[AI_SQUAD_CHARGES];
    int count = 0;
    for (int j = 0; j < unit_count && count < AI_SQUAD_CHARGES; j++) {
        const Unit *e = &units[j];
        if (e->alive != UNIT_ALIVE_ACTIVE || e->under_construction) continue;
        if (!Units_PlayersAreEnemies(p, e->player_id)) continue;
        const UnitDef *ed = Units_GetDef(e->def_idx);
        if (!ed || ed->num_weapons <= 0) continue;
        if (!ai_within(e->world_x, e->world_y, tx, ty,
                       AI_SQUAD_ENGAGE + AI_SQUAD_CHARGE_REACH)) continue;
        if (!ai_visible_to(world, p, e)) continue;
        charges[count].x = e->world_x;
        charges[count].y = e->world_y;
        charges[count].value = AI_UnitCombatValue(ed);
        count++;
    }
    if (count == 0) return 0;
    /* An enemy already within its reach is a fight, not an approach. */
    for (int k = 0; k < count; k++) {
        if (ai_within(charges[k].x, charges[k].y, u->world_x, u->world_y,
                      def->weapons[0].range)) return 0;
    }
    int32_t fx, fy;
    AI_Squad_FiringPoint(u->world_x, u->world_y, tx, ty,
                         def->weapons[0].range * 7 / 8, charges, count,
                         &fx, &fy);
    if (ai_within(u->world_x, u->world_y, fx, fy, 48)) return 0;
    if (u->cmd_kind == UNIT_CMD_MOVE && ai_within(u->cmd_x, u->cmd_y, fx, fy, 32))
        return 1;
    if (!ai_unit_ground_reaches(u, def, fx, fy)) return 0;
    Units_CommandMoveUnit(i, fx, fy);
    g_ai_counts[p][TAK_AI_COUNT_FLANKS]++;
    return 1;
}

/* ── The tactical layer ──────────────────────────────────────────────
 *
 * The goal planner says the seat is attacking. tak_ai_htn.h says what
 * the wave does about it: scout, mass, strike or hold. Our addition,
 * see docs/MANUAL_DEVIATIONS.md A-006 and A-007. */

/* The staging point is home. A seat with no base masses where it
 * stands. */
static int ai_at_stage(const AiPlayer *ap, const Unit *u) {
    if (!ap->base_known) return 1;
    return ai_within(u->world_x, u->world_y, ap->base_x, ap->base_y,
                     AI_DEFEND_RADIUS);
}

/* What the wave reader picked besides the state: who would look, who
 * would raid and where. Read fresh every think and kept nowhere. */
typedef struct AiWaveRead {
    int     scout;                          /* handle, or -1 */
    int     raiders[AI_HTN_RAID_SIZE];
    int     raider_count;
    int32_t raid_x, raid_y;
} AiWaveRead;

/* How long a seen strength is believed once the fog has closed over
 * it, and the long wait after which a wave goes whatever waits. */
#define AI_STRENGTH_MEMORY 3600
#define AI_WAVE_SIEGE      (3 * AI_WAVE_PATIENCE)
/* A raid is worth it on a cell at least this much more valuable than
 * it is defended. */
#define AI_RAID_MIN_WEAKNESS 4

/* The softest enemy building the seat can see that is not where the
 * wave is going anyway: the one whose ground is worth most over what
 * defends it. 0 when nothing is worth a raid. The maps spread a unit
 * over the cells round it, so the question is asked of a building and
 * not of a cell, or a raid would be sent at the empty ground beside a
 * monarch. */
static int ai_find_raid(const GameWorld *world, const Unit *units,
                        int unit_count, int p, const AiPlayer *ap,
                        int32_t *out_x, int32_t *out_y) {
    int tcx = -1, tcy = -1;
    if (!AI_Influence_CellOf(ap->target_x, ap->target_y, &tcx, &tcy)) return 0;
    int32_t best = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *t = &units[i];
        if (t->alive != UNIT_ALIVE_ACTIVE) continue;
        if (!ai_valid_player(world, t->player_id) ||
            !Units_PlayersAreEnemies(p, t->player_id)) continue;
        const UnitDef *td = Units_GetDef(t->def_idx);
        if (!td || td->is_feature || td->max_velocity > 0.0f) continue;
        if (AI_UnitAssetValue(td, world->cfg.monarch_expendable) <= 0) continue;
        if (!ai_visible_to(world, p, t)) continue;
        int cx = 0, cy = 0;
        AI_Influence_CellOf(t->world_x, t->world_y, &cx, &cy);
        if (cx == tcx && cy == tcy) continue;
        int32_t k = AI_Influence_Weakness(p, t->world_x, t->world_y);
        if (k < AI_RAID_MIN_WEAKNESS || k <= best) continue;
        best = k;
        *out_x = t->world_x;
        *out_y = t->world_y;
    }
    return best > 0;
}

static int ai_member_slot(const Unit *units, int handle, int p) {
    if (handle < 0 || handle >= AI_MEMBER_CAP || !g_ai_member[handle]) return -1;
    if (units[handle].player_id != p) return -1;
    return g_ai_member[handle] - 1;
}

/* Too hurt to be gathered: under a third of its hit points, the widest
 * of the three marks the original ejects a member at. */
static int ai_unit_hurt(const Unit *u) {
    return u->max_health > 0 && u->health * 3 < u->max_health;
}

/* A march home is not a march out, and only the second holds a scout
 * or a raid back. */
static int ai_marching_out(const AiPlayer *ap, const Unit *u) {
    if (u->cmd_kind != UNIT_CMD_MOVE) return 0;
    return !(ap->base_known && u->cmd_x == ap->base_x && u->cmd_y == ap->base_y);
}

static void ai_group_disband(const Unit *units, int unit_count, int p, int slot) {
    for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++) {
        if (units[i].player_id == p && g_ai_member[i] == slot + 1)
            g_ai_member[i] = 0;
    }
    memset(&g_ai_groups[p][slot], 0, sizeof(AiGroup));
}

/* The radius a group's spread is weighed against, the original's value
 * for its attack groups and for its raid band (legacy:19426,
 * legacy:19450). */
#define AI_COHESION_ATTACK 200000
#define AI_COHESION_RAID   500000

static void ai_fall_back(const Unit *units, int actor_idx, int p);

/* The member furthest from the group's mean position leaves it while
 * its distance squared is at least the member count times the radius,
 * until one is left (legacy:15845-15927). A straggler comes home to be
 * gathered again, as a hurt member does. Gives how many left. */
static int ai_group_stragglers(const Unit *units, int unit_count, int p,
                               int slot, int n) {
    const AiGroup *g = &g_ai_groups[p][slot];
    int64_t radius = g->kind == AI_GROUP_RAID ? AI_COHESION_RAID
                                              : AI_COHESION_ATTACK;
    int64_t sx = 0, sy = 0;
    for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++) {
        if (g_ai_member[i] != slot + 1 || units[i].player_id != p) continue;
        sx += units[i].world_x;
        sy += units[i].world_y;
    }
    int left = 0;
    while (n > 1) {
        int64_t mx = sx / n, my = sy / n;
        int far = -1;
        int64_t far_d = 0;
        for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++) {
            if (g_ai_member[i] != slot + 1 || units[i].player_id != p) continue;
            int64_t dx = units[i].world_x - mx, dy = units[i].world_y - my;
            int64_t d = dx * dx + dy * dy;
            if (d > far_d) { far_d = d; far = i; }
        }
        if (far < 0 || far_d < (int64_t)n * radius) break;
        g_ai_member[far] = 0;
        sx -= units[far].world_x;
        sy -= units[far].world_y;
        n--;
        left++;
        g_ai_counts[p][TAK_AI_COUNT_STRAGGLERS]++;
        ai_fall_back(units, far, p);
    }
    return left;
}

/* The nearest group out in the field, from home, with fewer members
 * than it went out with, or -1 (legacy:16083-16120). */
static int ai_group_to_reinforce(const Unit *units, int unit_count, int p,
                                 const int *members) {
    const AiPlayer *ap = &g_ai_players[p];
    if (!ap->base_known) return -1;
    int best = -1;
    int64_t best_d = 0;
    for (int s = 0; s < AI_GROUPS; s++) {
        const AiGroup *g = &g_ai_groups[p][s];
        if (g->mode != AI_GROUP_MARCHING || g->kind != AI_GROUP_ATTACK) continue;
        if (members[s] <= 0 || members[s] >= g->launch) continue;
        int64_t sx = 0, sy = 0;
        for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++) {
            if (g_ai_member[i] != s + 1 || units[i].player_id != p) continue;
            sx += units[i].world_x;
            sy += units[i].world_y;
        }
        int64_t dx = sx / members[s] - ap->base_x, dy = sy / members[s] - ap->base_y;
        int64_t d = dx * dx + dy * dy;
        if (best < 0 || d < best_d) { best = s; best_d = d; }
    }
    return best;
}

/* Idle at the staging point, in no group and fit to fight. */
static int ai_is_spare(const AiPlayer *ap, const Unit *units, int i, int p) {
    const Unit *u = &units[i];
    if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != p) return 0;
    if (g_ai_member[i] || u->under_construction) return 0;
    if (u->cmd_kind != UNIT_CMD_NONE || ai_unit_hurt(u)) return 0;
    if (!ai_def_is_mobile_combat(Units_GetDef(u->def_idx))) return 0;
    return ai_at_stage(ap, u);
}

static int ai_group_free_slot(int p) {
    for (int s = 0; s < AI_GROUPS; s++)
        if (g_ai_groups[p][s].mode == AI_GROUP_FREE) return s;
    return -1;
}

/* One pass over the seat's groups before it thinks about its army: who
 * is still a member, who is too hurt to stay one, which groups are
 * spent, and which spare fighters join the group that is forming.
 * Gives the forming group's slot, or -1.
 *
 * A member is ejected below its hit points over three, four or five,
 * drawn each time, as the original draws it (legacy:18364). A group
 * down to a third of what it went out with is spent and its members
 * are loose again, the original's disband back into the parent
 * (legacy:18250). A group whose target has died takes the seat's. */
static int ai_groups_update(const Unit *units, int unit_count, int p, int now) {
    AiPlayer *ap = &g_ai_players[p];
    int members[AI_GROUPS] = { 0 };
    for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++) {
        const Unit *u = &units[i];
        if (!g_ai_member[i] || u->player_id != p) continue;
        int slot = g_ai_member[i] - 1;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (u->alive != UNIT_ALIVE_ACTIVE || u->under_construction ||
            slot >= AI_GROUPS || g_ai_groups[p][slot].mode == AI_GROUP_FREE ||
            !ai_def_is_mobile_combat(d)) {
            g_ai_member[i] = 0;
            continue;
        }
        if (ai_unit_hurt(u) &&
            u->health < u->max_health / (int)(ai_rand(3) + 3)) {
            g_ai_member[i] = 0;
            g_ai_counts[p][TAK_AI_COUNT_EJECTED]++;
            continue;
        }
        members[slot]++;
    }
    for (int s = 0; s < AI_GROUPS; s++) {
        if (g_ai_groups[p][s].mode == AI_GROUP_FREE || members[s] < 2) continue;
        members[s] -= ai_group_stragglers(units, unit_count, p, s, members[s]);
    }
    int forming = -1;
    for (int s = 0; s < AI_GROUPS; s++) {
        AiGroup *g = &g_ai_groups[p][s];
        if (g->mode == AI_GROUP_FREE) continue;
        if (g->mode == AI_GROUP_FORMING) {
            if (members[s] == 0) { memset(g, 0, sizeof(*g)); continue; }
            forming = s;
            continue;
        }
        if (members[s] == 0 || members[s] * 3 <= g->launch) {
            ai_group_disband(units, unit_count, p, s);
            g_ai_counts[p][TAK_AI_COUNT_SPENT]++;
            continue;
        }
        if (g->kind != AI_GROUP_ATTACK) continue;
        int h = g->target_handle;
        int live = h >= 0 && h < unit_count &&
                   units[h].alive == UNIT_ALIVE_ACTIVE &&
                   units[h].stable_id == g->target_stable_id;
        if (live) {
            g->target_x = units[h].world_x;
            g->target_y = units[h].world_y;
        } else if (ap->target_handle >= 0 && ap->target_handle < unit_count) {
            g->target_player = ap->target_player;
            g->target_handle = ap->target_handle;
            g->target_stable_id = ap->target_stable_id;
            g->target_x = ap->target_x;
            g->target_y = ap->target_y;
        } else {
            ai_group_disband(units, unit_count, p, s);
        }
    }
    /* Spare fighters: idle at the staging point, in no group and fit
     * to fight. One pass in ten they all go to the nearest group out
     * in the field that is under strength (legacy:16335-16340).
     * Otherwise they join the group that is forming, and open one when
     * there is none and a slot is free. */
    int spares = 0;
    for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++)
        spares += ai_is_spare(ap, units, i, p);
    if (spares > 0 && ai_rand(10) == 0) {
        int s = ai_group_to_reinforce(units, unit_count, p, members);
        if (s >= 0) {
            for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++) {
                if (!ai_is_spare(ap, units, i, p)) continue;
                g_ai_member[i] = (uint8_t)(s + 1);
                g_ai_counts[p][TAK_AI_COUNT_REINFORCED]++;
            }
            return forming;
        }
    }
    for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++) {
        if (!ai_is_spare(ap, units, i, p)) continue;
        if (forming < 0) {
            forming = ai_group_free_slot(p);
            if (forming < 0) break;
            AiGroup *g = &g_ai_groups[p][forming];
            memset(g, 0, sizeof(*g));
            g->mode = AI_GROUP_FORMING;
            g->kind = AI_GROUP_ATTACK;
            g->target_handle = -1;
            g->formed_tick = now;
        }
        g_ai_member[i] = (uint8_t)(forming + 1);
    }
    return forming;
}

/* Read the wave state for the group that is forming and pick the
 * members with a part of their own: the one that would scout, the idle
 * one at the staging point nearest the target, and the ones that would
 * raid, the fastest there. A march out is the only order the seat
 * gives a member that is not fighting, so one member marching out
 * means the seat already has someone out. */
static void ai_read_wave(const GameWorld *world, const Unit *units,
                         int unit_count, int p, int now, int forming,
                         AiWaveState *ws, AiWaveRead *rd) {
    AiPlayer *ap = &g_ai_players[p];
    memset(ws, 0, sizeof(*ws));
    memset(rd, 0, sizeof(*rd));
    rd->scout = -1;
    ws->target_known = ap->target_handle >= 0 && ap->target_handle < unit_count;
    if (ws->target_known)
        ws->target_seen = ai_visible_to(world, p, &units[ap->target_handle]);
    ws->patience_due = (now % AI_WAVE_PATIENCE) == 0;
    ws->siege_due = (now % AI_WAVE_SIEGE) == 0;

    /* The strength at the target is what can be seen there now, or
     * what was last seen there while that is still believed. */
    if (ws->target_known) {
        uint32_t id = units[ap->target_handle].stable_id;
        int32_t seen = AI_Influence_At(p, AI_INF_THREAT, ap->target_x,
                                       ap->target_y);
        if (ws->target_seen || seen > 0) {
            ap->target_strength = seen;
            ap->target_strength_id = id;
            ap->target_strength_tick = now;
        }
        if (ap->target_strength_id == id && ap->target_strength_tick >= 0 &&
            now - ap->target_strength_tick <= AI_STRENGTH_MEMORY) {
            ws->enemy_at_target = ap->target_strength;
        }
    }

    int32_t scout_d = 0;
    int32_t raid_speed[AI_HTN_RAID_SIZE];
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != p) continue;
        if (u->under_construction) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!ai_def_is_mobile_combat(d)) continue;
        ws->members++;
        if (ai_marching_out(ap, u)) ws->marching = 1;
        if (forming < 0 || ai_member_slot(units, i, p) != forming) continue;
        if (u->cmd_kind != UNIT_CMD_NONE || !ai_at_stage(ap, u)) continue;
        ws->massed++;
        ws->wave_value += AI_UnitCombatValue(d);
        int32_t dist = ai_approx_dist((int64_t)u->world_x - ap->target_x,
                                      (int64_t)u->world_y - ap->target_y);
        if (rd->scout < 0 || dist < scout_d) { rd->scout = i; scout_d = dist; }
        /* The fastest at the staging point, earlier handle first. */
        int32_t speed = (int32_t)(d->max_velocity * 256.0f);
        int at = rd->raider_count;
        while (at > 0 && raid_speed[at - 1] < speed) at--;
        if (at < AI_HTN_RAID_SIZE) {
            int last = rd->raider_count < AI_HTN_RAID_SIZE ? rd->raider_count
                                                           : AI_HTN_RAID_SIZE - 1;
            for (int k = last; k > at; k--) {
                rd->raiders[k] = rd->raiders[k - 1];
                raid_speed[k] = raid_speed[k - 1];
            }
            rd->raiders[at] = i;
            raid_speed[at] = speed;
            if (rd->raider_count < AI_HTN_RAID_SIZE) rd->raider_count++;
        }
    }
    ws->launch = AI_Htn_LaunchCount(ws->members);
    if (ws->target_known && rd->raider_count == AI_HTN_RAID_SIZE)
        ws->raid_known = ai_find_raid(world, units, unit_count, p, ap,
                                      &rd->raid_x, &rd->raid_y);
}

/* What the members of one group face in the field, or with slot -1
 * what the seat's loose fighters do. Both sides off the same maps, a
 * cell counted once, so the two are spread alike and can be compared. */
static void ai_read_field(const Unit *units, int unit_count, int p, int slot,
                          AiWaveState *ws) {
    const AiPlayer *ap = &g_ai_players[p];
    int cells[16];
    int n_cells = 0;
    ws->field = 0;
    ws->field_value = ws->field_threat = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != p) continue;
        if (u->under_construction || ai_at_stage(ap, u)) continue;
        if (!ai_def_is_mobile_combat(Units_GetDef(u->def_idx))) continue;
        if (ai_member_slot(units, i, p) != slot) continue;
        ws->field++;
        int cx = 0, cy = 0;
        if (!AI_Influence_CellOf(u->world_x, u->world_y, &cx, &cy)) continue;
        int key = (cy << 8) | cx, dup = 0;
        for (int k = 0; k < n_cells && !dup; k++)
            if (cells[k] == key) dup = 1;
        if (dup || n_cells >= 16) continue;
        cells[n_cells++] = key;
        ws->field_value += AI_Influence_Cell(p, AI_INF_PRESENCE, cx, cy);
        ws->field_threat += AI_Influence_Cell(p, AI_INF_THREAT, cx, cy);
    }
}

/* The same member may be both the nearest and among the fastest, so
 * the part it plays is the one the plan has a use for. */
static AiRole ai_wave_role(const AiWaveRead *rd, const AiWavePlan *plan,
                           int handle) {
    int raids = 0;
    for (int k = 0; k < plan->step_count; k++)
        if (plan->steps[k] == AI_TASK_RAID) raids = 1;
    if (!raids) return handle == rd->scout ? AI_ROLE_SCOUT : AI_ROLE_MEMBER;
    for (int k = 0; k < rd->raider_count; k++)
        if (rd->raiders[k] == handle) return AI_ROLE_RAIDER;
    return AI_ROLE_MEMBER;
}

/* Bring a member in the field home. One order, not one a think. */
static void ai_fall_back(const Unit *units, int actor_idx, int p) {
    const AiPlayer *ap = &g_ai_players[p];
    const Unit *u = &units[actor_idx];
    if (!ap->base_known) return;
    if (u->cmd_kind == UNIT_CMD_MOVE && u->cmd_x == ap->base_x &&
        u->cmd_y == ap->base_y) return;
    Units_CommandMoveUnit(actor_idx, ap->base_x, ap->base_y);
}

static void ai_raid_at(const Unit *units, int actor_idx, int p,
                       const UnitDef *def, int target_player, int32_t x,
                       int32_t y) {
    if (!ai_unit_ground_reaches(&units[actor_idx], def, x, y)) return;
    Units_CommandMoveUnit(actor_idx, x, y);
    ai_count_order(p, target_player, 1);
}


/* A walking builder that is badly hurt with the enemy about drops what
 * it is doing and makes for home (legacy:17205-17231). Badly hurt is
 * under a quarter of its hit points, under an eighth when it carries a
 * weapon it can pay for, and twice either when its own mana is half
 * gone. The original sends it to a random point within 320 of the
 * brain's centre (legacy:17458), and so does this. */
#define AI_BUILDER_HOME_PX 320

static int ai_builder_retreats(const Unit *units, int actor_idx, int p,
                               const UnitDef *def) {
    const AiPlayer *ap = &g_ai_players[p];
    const Unit *u = &units[actor_idx];
    if (!ap->base_known || u->max_health <= 0) return 0;
    int armed = def->num_weapons > 0 &&
                (float)def->weapons[0].mana_per_shot <= u->mana + 0.01f;
    int mult = (u->mana_max > 0.0f && u->mana * 2.0f <= u->mana_max) ? 2 : 1;
    if (u->health >= u->max_health * mult / ((armed + 1) * 4)) return 0;
    if (AI_Influence_At(p, AI_INF_THREAT, u->world_x, u->world_y) <= 0) return 0;
    if (ai_within(u->world_x, u->world_y, ap->base_x, ap->base_y,
                  AI_BUILDER_HOME_PX)) return 0;
    if (u->cmd_kind == UNIT_CMD_MOVE &&
        ai_within(u->cmd_x, u->cmd_y, ap->base_x, ap->base_y,
                  AI_BUILDER_HOME_PX)) return 1;
    if (u->cmd_kind == UNIT_CMD_BUILD) Units_StopUnit(actor_idx);
    g_ai_counts[p][TAK_AI_COUNT_BUILDER_RETREATS]++;
    int32_t dx = (int32_t)ai_rand(2 * AI_BUILDER_HOME_PX + 1) - AI_BUILDER_HOME_PX;
    int32_t dy = (int32_t)ai_rand(2 * AI_BUILDER_HOME_PX + 1) - AI_BUILDER_HOME_PX;
    Units_CommandMoveUnit(actor_idx, ap->base_x + dx / 2, ap->base_y + dy / 2);
    return 1;
}

/* ── Planner glue ────────────────────────────────────────────────────
 *
 * The goal planner (tak_ai_plan.h) reads an abstract state and prices
 * actions from the profile; this is where both come from and where
 * the chosen first step turns into a real build order. */

static int ai_def_is_tower(const UnitDef *def) {
    return def && def->max_velocity <= 0.0f && def->num_weapons > 0 &&
           !(def->cap_flags & UNIT_CAP_BUILDER);
}

static int ai_def_produces_combat(int def_idx) {
    int children[32];
    int n = Units_GetBuildables(def_idx, children, 32);
    for (int c = 0; c < n; c++) {
        if (ai_def_is_combat_unit(Units_GetDef(children[c]))) return 1;
    }
    return 0;
}

static int ai_def_is_factory(int def_idx) {
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d || ai_def_is_mana_economy(d)) return 0;
    if (!(d->cap_flags & UNIT_CAP_BUILDER) || d->max_velocity > 0.0f) return 0;
    return ai_def_produces_combat(def_idx);
}

int TAK_AI_DebugIsProductionStructure(int def_idx) {
    return ai_def_is_factory(def_idx);
}

/* The profile loads on the first tick and resets every entry, so a
 * value set by a test marks the profile loaded to survive that. */
void TAK_AI_DebugSetLimit(int def_idx, int limit) {
    ai_profile_load();
    if (def_idx >= 0 && def_idx < AI_MAX_DEFS) g_ai_limit[def_idx] = limit;
}

void TAK_AI_DebugSetWeight(int def_idx, float weight) {
    ai_profile_load();
    if (def_idx >= 0 && def_idx < AI_MAX_DEFS) g_ai_weight[def_idx] = weight;
}

/* A walking producer: Zhon summons its whole army from beast handlers
 * and their kin, and priests summon dragons. A monarch never counts,
 * as in ai_def_is_factory. */
static int ai_def_is_mobile_producer(int def_idx) {
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d || ai_def_is_mana_economy(d) || d->commander) return 0;
    if (!(d->cap_flags & UNIT_CAP_BUILDER) || d->max_velocity <= 0.0f) return 0;
    int children[32];
    int n = Units_GetBuildables(def_idx, children, 32);
    for (int c = 0; c < n; c++) {
        if (ai_def_is_mobile_combat(Units_GetDef(children[c]))) return 1;
    }
    return 0;
}

static int ai_try_start_tower_build(const Unit *units, int unit_count,
                                    int actor_idx, const UnitDef *actor_def) {
    if (!(actor_def->cap_flags & UNIT_CAP_BUILDER)) return 0;
    int buildables[32];
    int n = Units_GetBuildables((int)units[actor_idx].def_idx, buildables, 32);
    /* A tower goes up between home and what threatens it, no further
     * out than AI_TOWER_REACH_PX, and clear of the seat's other towers
     * so they cover the approach instead of each other. The threat is
     * the last enemy that hit the base, or else the strongest seen
     * enemy cell round it. */
    int p = units[actor_idx].player_id;
    const AiPlayer *ap = &g_ai_players[p];
    if (ap->base_known) {
        int32_t tx = ap->base_x, ty = ap->base_y;
        if (ap->threat_tick >= 0) {
            tx = ap->threat_x;
            ty = ap->threat_y;
        } else {
            int bcx = 0, bcy = 0, w = 0, h = 0;
            int32_t best = 0;
            AI_Influence_Size(&w, &h);
            if (AI_Influence_CellOf(ap->base_x, ap->base_y, &bcx, &bcy)) {
                for (int cy = bcy - 3; cy <= bcy + 3; cy++) {
                    for (int cx = bcx - 3; cx <= bcx + 3; cx++) {
                        if (cx < 0 || cy < 0 || cx >= w || cy >= h) continue;
                        int32_t t = AI_Influence_Cell(p, AI_INF_THREAT, cx, cy);
                        if (t <= best) continue;
                        best = t;
                        tx = (cx << AI_INF_CELL_SHIFT) + AI_INF_CELL_PX / 2;
                        ty = (cy << AI_INF_CELL_SHIFT) + AI_INF_CELL_PX / 2;
                    }
                }
            }
        }
        int64_t dx = (int64_t)tx - ap->base_x, dy = (int64_t)ty - ap->base_y;
        int32_t dist = ai_approx_dist(dx, dy);
        g_ai_site_cx = ap->base_x;
        g_ai_site_cy = ap->base_y;
        if (dist > 0) {
            int32_t reach = dist < AI_TOWER_REACH_PX ? dist : AI_TOWER_REACH_PX;
            g_ai_site_cx += (int32_t)(dx * reach / dist);
            g_ai_site_cy += (int32_t)(dy * reach / dist);
        }
        g_ai_site_cx &= ~15;
        g_ai_site_cy &= ~15;
        g_ai_site_for_tower = 1;
    }
    (void)unit_count;
    int started = ai_try_start_build_from_list(actor_idx, buildables, n,
                                               ai_def_is_tower);
    g_ai_site_for_tower = 0;
    return started;
}

/* Mana cost per profile weight: weight 100 costs face value, weight 25
 * four times it, weight 0 or a full limit forbids (-1). */
static int32_t ai_action_cost(const Unit *units, int unit_count, int p,
                              int def_idx) {
    if (def_idx < 0 || def_idx >= AI_MAX_DEFS) return -1;
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d) return -1;
    int w = (int)g_ai_weight[def_idx];
    if (g_ai_weight[def_idx] <= 0.0f) return -1;
    if (w < 1) w = 1;
    if (!ai_limit_allows(units, unit_count, p, def_idx)) return -1;
    /* What no draw would take is not the plan's to price. A mana
     * building is the economy goal's and never in a draw. */
    if (!ai_def_is_mana_economy(d) &&
        ai_desirability(units, unit_count, p, def_idx) <= 0) return -1;
    int32_t cost = d->build_cost > 0 ? d->build_cost : 1;
    return cost * 100 / w;
}

static void ai_plan_price(const Unit *units, int unit_count, int p,
                          AiPlanCosts *c, AiAction act, int def_idx) {
    if (def_idx < 0) return;
    int32_t cost = ai_action_cost(units, unit_count, p, def_idx);
    if (cost < 0) return;
    c->allowed[act] = 1;
    c->cost[act] = cost;
}

/* The freeze holds only the monarch's build think (legacy:17208, :17257). */
static int ai_build_frozen(const AiPlayer *ap, const UnitDef *def, int now) {
    return def && def->commander && now < ap->build_freeze_until;
}

/* A walking builder fighting on its own account is free to build: our
 * auto-acquire fabricates an order the original never gives, while a
 * mission the AI did give stands (legacy:17229-17238). A structure's
 * fight is its guns, so only an idle one counts. */
static int ai_builder_free(const Unit *u, const UnitDef *def) {
    if (u->under_construction || u->build_target >= 0) return 0;
    if (u->cmd_kind == UNIT_CMD_NONE) return 1;
    return u->cmd_kind == UNIT_CMD_ATTACK && !u->attack_explicit &&
           def->max_velocity > 0.0f;
}

/* The most a standing producer's draw weighs another walking builder
 * by, kept as the seat's builder_want. */
static void ai_note_builder_want(const Unit *units, int unit_count, int p,
                                 const int *buildables, int n,
                                 AiPlanState *s) {
    for (int b = 0; b < n; b++) {
        const UnitDef *bd = Units_GetDef(buildables[b]);
        if (!bd || !(bd->cap_flags & UNIT_CAP_BUILDER)) continue;
        if (!ai_def_is_trainable(bd)) continue;
        if (!ai_limit_allows(units, unit_count, p, buildables[b])) continue;
        int32_t want = ai_desirability(units, unit_count, p, buildables[b]);
        if (want > s->builder_want) s->builder_want = want;
    }
}

static void ai_plan_read(const GameWorld *world, const Unit *units,
                         int unit_count, int p, int now,
                         AiPlanState *s, AiPlanCosts *c) {
    const AiPlayer *ap = &g_ai_players[p];
    memset(s, 0, sizeof(*s));
    memset(c, 0, sizeof(*c));
    int32_t mana = Economy_GetMana(&world->economy, p);
    int32_t cap = Economy_GetMaxMana(&world->economy, p);
    s->mana_pct = cap > 0 ? (int32_t)((int64_t)mana * 100 / cap) : 0;
    s->stalling = ai_player_stalling(world, p);
    /* Build efficiency, the measure the original gates its picks on:
     * the pool over what the frames being fed ask for this tick, and
     * 1.0 with nothing building (legacy:235975-235983). */
    float demand = 0.0f;
    for (int i = 0; i < unit_count; i++) {
        const Unit *b = &units[i];
        if (b->alive != UNIT_ALIVE_ACTIVE || b->player_id != p) continue;
        if (b->cmd_kind != UNIT_CMD_BUILD) continue;
        if (b->build_target < 0 || b->build_target >= unit_count) continue;
        const Unit *f = &units[b->build_target];
        if (f->alive != UNIT_ALIVE_ACTIVE || !f->under_construction) continue;
        const UnitDef *bd = Units_GetDef(b->def_idx);
        const UnitDef *fd = Units_GetDef(f->def_idx);
        if (!bd || !fd || fd->build_cost <= 0) continue;
        float worker = bd->worker_time > 0.0f ? bd->worker_time : 1.0f;
        float btime = fd->buildtime > 0.0f ? fd->buildtime : 100.0f;
        demand += ((float)fd->build_cost * worker) / (btime * 60.0f);
    }
    int32_t demand_milli = (int32_t)(demand * 1000.0f);
    s->build_eff = 100;
    if (demand_milli > 0) {
        int64_t eff = (int64_t)mana * 100000 / demand_milli;
        s->build_eff = eff > 100 ? 100 : (int32_t)eff;
    }
    int lode_def = -1, factory_def = -1, tower_def = -1, train_def = -1;
    int mobile_factory_def = -1;
    int32_t factory_want = 0, mobile_want = 0;
    /* One pad def per builder, the one it would place there. */
    int pad_defs[4];
    int pad_def_count = 0, lode_off_pad = 0;
    int32_t lode_cost = 0;
    int32_t train_cost = 0;

    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != p) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d) continue;
        if (ai_def_is_mana_economy(d)) {
            if (u->under_construction) s->lodestones_pending++;
            else s->lodestones++;
            continue;
        }
        if (d->cap_flags & UNIT_CAP_BUILDER) {
            int buildables[32];
            int n = Units_GetBuildables((int)u->def_idx, buildables, 32);
            if (d->max_velocity > 0.0f) {
                int this_pad = -1;
                for (int b = 0; b < n; b++) {
                    const UnitDef *bd = Units_GetDef(buildables[b]);
                    if (!bd) continue;
                    if (ai_def_is_mana_economy(bd)) {
                        if (!bd->yardmap_sacred) lode_off_pad = 1;
                        else if (this_pad < 0) this_pad = buildables[b];
                        int32_t lc = ai_action_cost(units, unit_count, p,
                                                    buildables[b]);
                        if (lc >= 0 && (lode_def < 0 || lc < lode_cost)) {
                            lode_def = buildables[b];
                            lode_cost = lc;
                        }
                    }
                    if (ai_def_is_tower(bd)) {
                        /* The first tower a draw would take. */
                        if (tower_def < 0 &&
                            ai_action_cost(units, unit_count, p,
                                           buildables[b]) >= 0)
                            tower_def = buildables[b];
                    } else if (ai_def_is_factory(buildables[b])) {
                        /* The most wanted factory the limit still
                         * allows, not the first in the list, so a seat
                         * at its castle limit moves on to the next kind. */
                        if (ai_limit_allows(units, unit_count, p, buildables[b])) {
                            int32_t want = ai_desirability(units, unit_count, p,
                                                           buildables[b]);
                            if (want > factory_want) {
                                factory_def = buildables[b];
                                factory_want = want;
                            }
                        }
                    } else if (ai_def_is_mobile_producer(buildables[b])) {
                        if (ai_limit_allows(units, unit_count, p, buildables[b])) {
                            int32_t want = ai_desirability(units, unit_count, p,
                                                           buildables[b]);
                            if (want > mobile_want) {
                                mobile_factory_def = buildables[b];
                                mobile_want = want;
                            }
                        }
                    }
                }
                if (!d->commander && !ai_unit_is_monarch(d)) {
                    if (u->under_construction) s->builders_pending++;
                    else s->builders++;
                }
                if (this_pad >= 0) {
                    int seen = 0;
                    for (int k = 0; k < pad_def_count; k++)
                        if (pad_defs[k] == this_pad) seen = 1;
                    if (!seen && pad_def_count < 4)
                        pad_defs[pad_def_count++] = this_pad;
                }
                if (!ai_build_frozen(ap, d, now) && ai_builder_free(u, d))
                    s->builders_idle++;
                /* A walking producer also trains as a factory does. */
                if (ai_def_is_mobile_producer((int)u->def_idx)) {
                    if (u->under_construction) { s->factories_pending++; continue; }
                    s->factories++;
                    ai_note_builder_want(units, unit_count, p, buildables, n, s);
                    if (ai_builder_free(u, d)) {
                        s->factories_idle++;
                        for (int b = 0; b < n; b++) {
                            if (!ai_def_is_mobile_combat(Units_GetDef(buildables[b])))
                                continue;
                            int32_t cost = ai_action_cost(units, unit_count, p,
                                                          buildables[b]);
                            if (cost < 0) continue;
                            if (train_def < 0 || cost < train_cost) {
                                train_def = buildables[b];
                                train_cost = cost;
                            }
                        }
                    }
                }
                continue;
            }
            int produces = 0, cheapest = -1;
            int32_t cheapest_cost = 0;
            for (int b = 0; b < n; b++) {
                if (!ai_def_is_combat_unit(Units_GetDef(buildables[b]))) continue;
                produces = 1;
                int32_t cost = ai_action_cost(units, unit_count, p, buildables[b]);
                if (cost < 0) continue;
                if (cheapest < 0 || cost < cheapest_cost) {
                    cheapest = buildables[b];
                    cheapest_cost = cost;
                }
            }
            if (!produces) continue;
            if (u->under_construction) { s->factories_pending++; continue; }
            s->factories++;
            ai_note_builder_want(units, unit_count, p, buildables, n, s);
            if (u->cmd_kind == UNIT_CMD_NONE && u->build_target < 0) {
                s->factories_idle++;
                if (cheapest >= 0 && (train_def < 0 || cheapest_cost < train_cost)) {
                    train_def = cheapest;
                    train_cost = cheapest_cost;
                }
            }
            continue;
        }
        if (!u->under_construction && ai_def_is_mobile_combat(d)) {
            int32_t v = AI_UnitCombatValue(d);
            s->army += v;
            if (ap->base_known && ai_within(u->world_x, u->world_y,
                                            ap->base_x, ap->base_y,
                                            AI_DEFEND_RADIUS)) {
                s->army_home += v;
            }
        }
    }

    for (int i = 0; i < unit_count; i++) {
        const Unit *t = &units[i];
        if (t->alive != UNIT_ALIVE_ACTIVE || t->under_construction) continue;
        if (!ai_valid_player(world, t->player_id)) continue;
        if (!Units_PlayersAreEnemies(p, t->player_id)) continue;
        if (!ai_visible_to(world, p, t)) continue;
        s->threat_total += AI_UnitCombatValue(Units_GetDef(t->def_idx));
    }
    if (ap->base_known) {
        s->threat_home = AI_Influence_At(p, AI_INF_THREAT, ap->base_x, ap->base_y);
    }
    int w, h;
    AI_Influence_Size(&w, &h);
    for (int cy = 0; cy < h; cy++) {
        for (int cx = 0; cx < w; cx++) {
            int32_t x = cx << AI_INF_CELL_SHIFT, y = cy << AI_INF_CELL_SHIFT;
            int32_t e = AI_Influence_Exposure(p, x, y);
            if (e > s->exposure) s->exposure = e;
            if (AI_Influence_Cell(p, AI_INF_ENEMY_VALUE, cx, cy) > 0) {
                int32_t k = AI_Influence_Weakness(p, x, y);
                if (k > s->enemy_weak) s->enemy_weak = k;
            }
        }
    }

    /* A pad is free when some builder could put its own lodestone on
     * it now, which is the def the expansion would place there. */
    for (int i = 0; pad_def_count > 0 && i < world->feature_count; i++) {
        int32_t wx = 0, wy = 0;
        int fits = 0;
        for (int k = 0; k < pad_def_count && !fits; k++)
            fits = ai_pad_site(world, units, unit_count, p, i, pad_defs[k], &wx, &wy);
        if (!fits) continue;
        s->free_sites++;
        if (ap->base_known && ai_within(wx, wy, ap->base_x, ap->base_y, 2048))
            s->site_near++;
    }
    if (lode_def >= 0) {
        s->lode_target = 1 + s->free_sites / 2;
        int32_t lim = g_ai_limit[lode_def];
        if (lim >= 0 && s->lode_target > lim) s->lode_target = lim;
        /* A lodestone bound to a pad is wanted only where one can go. */
        int most = s->lodestones + s->lodestones_pending + s->free_sites;
        if (!lode_off_pad && s->lode_target > most) s->lode_target = most;
    }
    s->target_known = ap->target_handle >= 0;

    /* No structure makes an army here (Zhon): the builders summon a
     * walking producer instead. */
    if (factory_def < 0) factory_def = mobile_factory_def;
    c->allowed[AI_ACT_HOLD] = 1;
    c->allowed[AI_ACT_WAVE] = 1;
    ai_plan_price(units, unit_count, p, c, AI_ACT_BUILD_LODESTONE, lode_def);
    ai_plan_price(units, unit_count, p, c, AI_ACT_BUILD_FACTORY, factory_def);
    ai_plan_price(units, unit_count, p, c, AI_ACT_BUILD_TOWER, tower_def);
    ai_plan_price(units, unit_count, p, c, AI_ACT_TRAIN, train_def);
    if (factory_def >= 0 && !c->allowed[AI_ACT_TRAIN]) {
        /* No factory yet: the plan still needs a price for the unit
         * it will train, the cheapest the factory type produces. */
        int children[32];
        int n = Units_GetBuildables(factory_def, children, 32);
        int cheapest = -1;
        int32_t cheapest_cost = 0;
        for (int b = 0; b < n; b++) {
            if (!ai_def_is_combat_unit(Units_GetDef(children[b]))) continue;
            int32_t cost = ai_action_cost(units, unit_count, p, children[b]);
            if (cost < 0) continue;
            if (cheapest < 0 || cost < cheapest_cost) { cheapest = children[b]; cheapest_cost = cost; }
        }
        if (cheapest >= 0) {
            c->allowed[AI_ACT_TRAIN] = 1;
            c->cost[AI_ACT_TRAIN] = cheapest_cost;
            train_def = cheapest;
        }
    }
    c->unit_value = train_def >= 0 ? AI_UnitCombatValue(Units_GetDef(train_def)) : 0;
    if (c->allowed[AI_ACT_TRAIN] && c->unit_value < 1) c->unit_value = 1;
    c->tower_value = tower_def >= 0 ? AI_UnitCombatValue(Units_GetDef(tower_def)) : 0;
    if (c->allowed[AI_ACT_BUILD_TOWER] && c->tower_value < 1) c->tower_value = 1;
}

/* Book a started action so the next actor this tick plans on it. */
static void ai_plan_note(AiPlanState *s, const AiPlanCosts *c, AiAction act) {
    switch (act) {
    case AI_ACT_BUILD_LODESTONE:
        s->builders_idle--; s->lodestones_pending++; break;
    case AI_ACT_BUILD_FACTORY:
        s->builders_idle--; s->factories_pending++; break;
    case AI_ACT_BUILD_TOWER:
        s->builders_idle--; s->army_home += c->tower_value; break;
    case AI_ACT_TRAIN:
        s->factories_idle--; s->army += c->unit_value; break;
    default:
        break;
    }
}

static int ai_execute_build(const GameWorld *world, const Unit *units,
                            int unit_count, int actor_idx,
                            const UnitDef *def, AiAction act) {
    switch (act) {
    case AI_ACT_BUILD_LODESTONE:
        if (ai_try_expand_to_sacred_site(world, units, unit_count, actor_idx, def))
            return 1;
        return ai_try_start_economy_build(units, unit_count, actor_idx, def);
    case AI_ACT_BUILD_FACTORY:
        return ai_try_start_production_structure_build(units, unit_count,
                                                       actor_idx, def);
    case AI_ACT_BUILD_TOWER:
        return ai_try_start_tower_build(units, unit_count, actor_idx, def);
    case AI_ACT_TRAIN:
        return ai_try_start_combat_production(units, unit_count, actor_idx, def);
    default:
        return 0;
    }
}

static void ai_tick_player(const GameWorld *world, const Unit *units,
                           int unit_count, int p, int now) {
    AiPlayer *ap = &g_ai_players[p];
    ai_update_threat(world, units, unit_count, p, now);
    if (ap->freeze_pending) {
        /* 30 + three rand(300) draws at 30 Hz, doubled for 60 Hz
         * (legacy:15092-15094). */
        int r = (int)ai_rand(300);
        r += (int)ai_rand(300);
        r += (int)ai_rand(300);
        ap->build_freeze_until = now + 2 * (30 + r);
        ap->freeze_pending = 0;
    }
    ai_update_wave_target(world, units, unit_count, p);
    int allied = 0;
    const AiPlayer *threat = ai_effective_threat(world, p, &allied);

    AiPlanState ps;
    AiPlanCosts pc;
    ai_plan_read(world, units, unit_count, p, now, &ps, &pc);
    AiGoal army_goal = AI_GOAL_NONE;
    AiAction army_action = AI_Plan_NextAction(&ps, &pc, AI_ACTOR_ARMY, &army_goal);
    AiWaveState ws;
    AiWaveRead wr;
    AiWavePlan wplan;
    int forming = ai_groups_update(units, unit_count, p, now);
    ai_read_wave(world, units, unit_count, p, now, forming, &ws, &wr);
    if (g_ai_tactics_off[p] & TAK_AI_TACTIC_STRENGTH) {
        ws.wave_value = ws.enemy_at_target = 0;
        ws.siege_due = 0;
    }
    if (g_ai_tactics_off[p] & TAK_AI_TACTIC_RAID) ws.raid_known = 0;
    AI_Htn_Plan(&ws, &wplan);
    g_ai_wave_reason[p] = wplan.reason;

    /* What the forming group's plan does to the groups. A strike sends
     * it out under the seat's target. A raid splits the raiders off as
     * a raid group of their own, which needs a slot to be in. */
    if (forming >= 0 && ws.massed >= ws.launch &&
        wplan.steps[0] != AI_TASK_STRIKE)
        g_ai_counts[p][TAK_AI_COUNT_HELD]++;
    int plan_strikes = 0, plan_raids = 0;
    for (int k = 0; k < wplan.step_count; k++) {
        if (wplan.steps[k] == AI_TASK_STRIKE) plan_strikes = 1;
        if (wplan.steps[k] == AI_TASK_RAID) plan_raids = 1;
    }
    if (forming >= 0 && plan_strikes && ap->target_handle >= 0) {
        AiGroup *g = &g_ai_groups[p][forming];
        int n = 0;
        for (int i = 0; i < unit_count && i < AI_MEMBER_CAP; i++)
            if (units[i].player_id == p && g_ai_member[i] == forming + 1) n++;
        g->mode = AI_GROUP_MARCHING;
        g->launch = n;
        g_ai_counts[p][TAK_AI_COUNT_STRIKES]++;
        g->target_player = ap->target_player;
        g->target_handle = ap->target_handle;
        g->target_stable_id = ap->target_stable_id;
        g->target_x = ap->target_x;
        g->target_y = ap->target_y;
    } else if (forming >= 0 && plan_raids) {
        int slot = ai_group_free_slot(p);
        if (slot >= 0) {
            AiGroup *g = &g_ai_groups[p][slot];
            memset(g, 0, sizeof(*g));
            g->mode = AI_GROUP_MARCHING;
            g->kind = AI_GROUP_RAID;
            g_ai_counts[p][TAK_AI_COUNT_RAIDS]++;
            g->launch = wr.raider_count;
            g->target_player = ap->target_player;
            g->target_handle = -1;
            g->target_x = wr.raid_x;
            g->target_y = wr.raid_y;
            g->formed_tick = now;
            for (int k = 0; k < wr.raider_count; k++)
                g_ai_member[wr.raiders[k]] = (uint8_t)(slot + 1);
        }
    }

    /* Each group out in the field, and the loose fighters as one more,
     * is asked whether it breaks off. A group that does is spent: its
     * members are loose and come home. */
    int loose_broke = 0;
    for (int s = -1; s < AI_GROUPS; s++) {
        if (s >= 0 && g_ai_groups[p][s].mode != AI_GROUP_MARCHING) continue;
        if (g_ai_tactics_off[p] & TAK_AI_TACTIC_BREAK_OFF) break;
        AiWaveState fs;
        AiWavePlan fplan;
        memset(&fs, 0, sizeof(fs));
        fs.target_known = 1;
        fs.siege_due = ws.siege_due;
        ai_read_field(units, unit_count, p, s, &fs);
        AI_Htn_Plan(&fs, &fplan);
        if (fplan.steps[0] != AI_TASK_FALL_BACK) continue;
        g_ai_wave_reason[p] = fplan.reason;
        loose_broke = 1;
        g_ai_counts[p][TAK_AI_COUNT_BREAK_OFFS]++;
        if (ai_trace()) {
            fprintf(stderr, "AI %d: group %d breaks off, %d in the field worth "
                    "%d against %d\n", p,
                    s >= 0 ? ai_group_name(&g_ai_groups[p][s], s) : 0,
                    fs.field, fs.field_value, fs.field_threat);
        }
        if (s >= 0) ai_group_disband(units, unit_count, p, s);
    }
    if (ai_trace()) {
        fprintf(stderr, "AI %d: mana %d%% eff %d%% stall %d lode %d/%d fac %d "
                "builders %d+%d want %d army %d/%d "
                "threat %d exposure %d sites %d -> army goal %d act %d\n",
                p, ps.mana_pct, ps.build_eff, ps.stalling, ps.lodestones, ps.lode_target,
                ps.factories, ps.builders, ps.builders_pending, ps.builder_want,
                ps.army, ps.army_home, ps.threat_home,
                ps.exposure, ps.site_near, (int)army_goal, (int)army_action);
        fprintf(stderr, "AI %d: wave %d/%d massed of %d, value %d against %d, "
                "field %d value %d against %d -> %s (%s)\n",
                p, ws.massed, ws.launch, ws.members, ws.wave_value,
                ws.enemy_at_target, ws.field, ws.field_value, ws.field_threat,
                AI_Htn_TaskName(wplan.steps[0]), wplan.reason);
        if (ws.raid_known)
            fprintf(stderr, "AI %d: soft corner at (%d,%d), weakness %d\n", p,
                    wr.raid_x, wr.raid_y,
                    AI_Influence_Weakness(p, wr.raid_x, wr.raid_y));
    }

    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->player_id != p) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!ai_unit_can_fight_or_move(u, def)) continue;
        if ((def->cap_flags & UNIT_CAP_BUILDER) && def->max_velocity > 0.0f &&
            ai_builder_retreats(units, i, p, def)) {
            continue;
        }
        if (u->cmd_kind == UNIT_CMD_BUILD ||
            u->cmd_kind == UNIT_CMD_REPAIR ||
            u->cmd_kind == UNIT_CMD_RECLAIM ||
            u->cmd_kind == UNIT_CMD_LOAD ||
            u->cmd_kind == UNIT_CMD_UNLOAD ||
            u->cmd_kind == UNIT_CMD_BOARD) {
            continue;
        }
        if (def->cap_flags & UNIT_CAP_BUILDER) {
            /* Builder think: build first, fight only when idle with
             * nothing to build (legacy:17270). Never sent on waves.
             * What to build is the planner's first step for this
             * actor class (A-003). */
            AiActorClass cls = def->max_velocity > 0.0f ? AI_ACTOR_BUILDER
                                                        : AI_ACTOR_FACTORY;
            if (!ai_build_frozen(ap, def, now) && ai_builder_free(u, def)) {
                AiGoal goal = AI_GOAL_NONE;
                AiAction act = AI_ACT_NONE;
                /* A walking producer trains first, as every builder did
                 * before the planner, and builds when there is nothing
                 * to train (Zhon has no other producer). */
                if (cls == AI_ACTOR_BUILDER &&
                    ai_def_is_mobile_producer((int)u->def_idx) &&
                    AI_Plan_NextAction(&ps, &pc, AI_ACTOR_FACTORY, &goal) ==
                        AI_ACT_TRAIN &&
                    ai_execute_build(world, units, unit_count, i, def,
                                     AI_ACT_TRAIN)) {
                    act = AI_ACT_TRAIN;
                } else {
                    goal = AI_GOAL_NONE;
                    act = AI_Plan_NextAction(&ps, &pc, cls, &goal);
                    if (act != AI_ACT_NONE &&
                        !ai_execute_build(world, units, unit_count, i, def, act))
                        act = AI_ACT_NONE;
                }
                if (act != AI_ACT_NONE) {
                    if (ai_trace()) {
                        fprintf(stderr, "AI %d: %s does action %d for goal %d\n",
                                p, def->unitname, (int)act, (int)goal);
                    }
                    ai_plan_note(&ps, &pc, act);
                    continue;
                }
            }
            if (def->num_weapons > 0 && def->max_velocity > 0.0f &&
                u->cmd_kind == UNIT_CMD_NONE) {
                ai_engage_nearby(world, units, unit_count, i, def);
            }
            continue;
        }
        if (!ai_def_is_combat_unit(def)) continue;
        if (threat && ai_defend(world, units, unit_count, i, p,
                                threat, allied)) {
            continue;
        }
        /* A fighter in the field and in no group comes home to be
         * gathered again when it is hurt, when the loose have broken
         * off, or when it has nothing to do there. One that is fit and
         * in a fight it is not losing is left to it. */
        int slot = ai_member_slot(units, i, p);
        int at_stage = ai_at_stage(ap, u);
        if (slot < 0 && !at_stage &&
            (loose_broke || ai_unit_hurt(u) || u->cmd_kind == UNIT_CMD_NONE)) {
            ai_fall_back(units, i, p);
            continue;
        }
        if (u->cmd_kind == UNIT_CMD_ATTACK) continue;
        /* A ranged member closing in takes its firing position before it
         * is drawn into the nearest fight, and one ahead of its group
         * stands after it (A-010). */
        if (ai_squad_step(world, units, unit_count, i, p, def, slot, 1)) continue;
        if (ai_engage_nearby(world, units, unit_count, i, def)) continue;
        if (ai_squad_step(world, units, unit_count, i, p, def, slot, 0)) continue;
        if (u->cmd_kind != UNIT_CMD_NONE) continue;
        /* Defence plans hold the units at home instead of a wave. */
        if (army_action == AI_ACT_HOLD && ap->base_known &&
            ai_within(u->world_x, u->world_y, ap->base_x, ap->base_y,
                      AI_DEFEND_RADIUS)) {
            continue;
        }
        if (slot < 0) continue;
        const AiGroup *g = &g_ai_groups[p][slot];
        if (g->mode == AI_GROUP_FORMING) {
            /* The attack goal decomposed: a member scouts or waits at
             * the staging point for the group (A-006). */
            AiTask task = AI_Htn_MemberTaskIn(&wplan,
                                              ai_wave_role(&wr, &wplan, i),
                                              at_stage);
            if (task == AI_TASK_SCOUT)
                ai_dispatch_wave(world, units, unit_count, i, p, def);
        } else if (g->kind == AI_GROUP_RAID) {
            /* A raider with nothing left to do where it was sent has
             * raided, and is loose. */
            if (ai_within(u->world_x, u->world_y, g->target_x, g->target_y, 96))
                g_ai_member[i] = 0;
            else
                ai_raid_at(units, i, p, def, g->target_player, g->target_x,
                           g->target_y);
        } else {
            /* A member with nothing to do goes on at the group's
             * target, wherever it stands. */
            ai_dispatch_at(world, units, unit_count, i, p, def,
                           g->target_player, g->target_handle, g->target_x,
                           g->target_y);
        }
    }
}

static int g_ai_stagger = 1;
void TAK_AI_DebugSetStagger(int on) { g_ai_stagger = on ? 1 : 0; }
static uint32_t g_ai_thinks[TAK_MAX_PLAYERS + 1];
uint32_t TAK_AI_DebugThinks(int player_id) {
    return player_id >= 1 && player_id <= TAK_MAX_PLAYERS ? g_ai_thinks[player_id] : 0;
}

/* The tick in each second a seat thinks on. The first computer seat
 * keeps tick 0 and the rest spread evenly over the second, so seven
 * seats no longer land on one frame. The original spreads its work
 * the same way, a slice of every player's units each tick
 * (legacy:18938). */
static int ai_seat_phase(const GameWorld *world, int p) {
    if (!g_ai_stagger) return 0;
    int before = 0, seats = 0;
    for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
        if (world->cfg.players[q - 1].kind != TAK_SLOT_AI) continue;
        if (q < p) before++;
        seats++;
    }
    return seats > 0 ? before * 60 / seats : 0;
}

void TAK_AI_TickSkirmish(GameWorld *world) {
    if (!world || !world->loaded || world->skirmish_game_over) return;
    if (world->mission.objective_count > 0 || world->mission.placement_count > 0) return;

    /* A new match resets through TAK_AI_BeginMatch. Guessing one from a
     * tick count that stopped climbing threw the session seed away. */
    int now = world->skirmish_elapsed_ticks;
    g_ai_last_tick = now;

    /* Re-plan at a low cadence. Unit locomotion and combat remain in
     * Units_TickEngines; the AI just issues player-equivalent orders.
     * The shared maps refresh once a second, each seat on its phase. */
    int shared = (now % 60) == 0;
    int due = 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS && !due; p++)
        if (world->cfg.players[p - 1].kind == TAK_SLOT_AI &&
            (now % 60) == ai_seat_phase(world, p)) due = 1;
    if (!shared && !due) return;
    ai_profile_load();

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    if (!units || unit_count <= 0) return;

    if (shared) {
        ai_update_bases(world, units, unit_count);
        AI_Influence_Refresh(world);
        /* Other seats keep no maps, only the hits on their bases. */
        for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
            if (!ai_valid_player(world, p) || g_ai_players[p].active) continue;
            ai_promote_threat(world, units, unit_count, p, now);
        }
    }
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (world->cfg.players[p - 1].kind != TAK_SLOT_AI) continue;
        if ((now % 60) != ai_seat_phase(world, p)) continue;
        g_ai_thinks[p]++;
        ai_tick_player(world, units, unit_count, p, now);
    }
}
