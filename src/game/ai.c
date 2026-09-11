#include "tak_ai.h"
#include "tak_ai_influence.h"
#include "tak_ai_plan.h"
#include "tak_economy.h"
#include "tak_battle_config.h"
#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "tak_hpi.h"
#include "tak_features.h"
#include "tak_memory.h"

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

/* Effective desirability: profile weight × first-of-type bias
 * (legacy ×4 owned==0, ×2 owned==1 — :19859). */
static float ai_desirability(const Unit *units, int unit_count,
                             int player_id, int def_idx) {
    float w = (def_idx >= 0 && def_idx < AI_MAX_DEFS)
            ? g_ai_weight[def_idx] : 50.0f;
    if (w <= 0.0f) return 0.0f;
    int owned = ai_count_owned(units, unit_count, player_id, def_idx);
    if (owned == 0) w *= 4.0f;
    else if (owned == 1) w *= 2.0f;
    return w;
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

static int ai_player_has_pending_combat_production(const Unit *units,
                                                   int unit_count,
                                                   int player_id) {
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != player_id) continue;
        if (!u->under_construction) continue;
        if (ai_def_is_combat_unit(Units_GetDef(u->def_idx))) return 1;
    }
    return 0;
}

static int ai_player_has_production_structure(const Unit *units,
                                              int unit_count,
                                              int player_id) {
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != player_id) continue;
        if (u->under_construction) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!def || !(def->cap_flags & UNIT_CAP_BUILDER)) continue;
        if (ai_def_is_mana_economy(def)) continue;
        /* Structures only. A monarch who can summon a mobile fighter
         * (Lokken's targod under Iron Plague) is not a factory. */
        if (def->max_velocity > 0.0f) continue;
        int buildables[32];
        int n = Units_GetBuildables((int)u->def_idx, buildables,
                                    (int)(sizeof(buildables) / sizeof(buildables[0])));
        for (int b = 0; b < n; b++) {
            if (ai_def_is_combat_unit(Units_GetDef(buildables[b]))) {
                return 1;
            }
        }
    }
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

/* Expanding ring search for a clear build site around (cx, cy),
 * tile-aligned to 16px attr cells. The legacy engine validates each
 * candidate with Terrain_FindBuildPlacement (legacy:219074 —
 * per-cell feature/occupancy/water/slope checks) and the AI retries
 * placements around its base until one validates; a fixed offset
 * table cannot site large structures (e.g. keeps are 8×20 tiles). */
static int ai_find_clear_site(int build_def, int32_t cx, int32_t cy,
                              int32_t *out_x, int32_t *out_y) {
    const UnitDef *bd = Units_GetDef(build_def);
    int fx = (bd && bd->footprint_x > 0) ? bd->footprint_x : 2;
    int fz = (bd && bd->footprint_z > 0) ? bd->footprint_z : 2;
    int larger = fx > fz ? fx : fz;
    const int step = 16;
    int start_r = larger * 8 + step;
    const int max_r = 768;
    for (int r = start_r; r <= max_r; r += step) {
        for (int dx = -r; dx <= r; dx += step) {
            if (Units_IsBuildSiteClear(build_def, cx + dx, cy - r)) {
                *out_x = cx + dx; *out_y = cy - r; return 1;
            }
            if (Units_IsBuildSiteClear(build_def, cx + dx, cy + r)) {
                *out_x = cx + dx; *out_y = cy + r; return 1;
            }
        }
        for (int dy = -r + step; dy <= r - step; dy += step) {
            if (Units_IsBuildSiteClear(build_def, cx - r, cy + dy)) {
                *out_x = cx - r; *out_y = cy + dy; return 1;
            }
            if (Units_IsBuildSiteClear(build_def, cx + r, cy + dy)) {
                *out_x = cx + r; *out_y = cy + dy; return 1;
            }
        }
    }
    return 0;
}

static int ai_try_start_build_def(int actor_idx, int build_def) {
    const Unit *units = Units_GetActive(NULL);
    if (!units) return 0;
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
    if (!ai_find_clear_site(build_def, actor->world_x, actor->world_y,
                            &bx, &by)) {
        return 0;
    }
    return Units_BeginBuildingForUnit(actor_idx, build_def, bx, by) >= 0;
}

/* Weighted-random pick over qualifying entries (legacy reservoir
 * sampling :21300), honoring profile limits; falls back to the rest
 * if the pick can't start. */
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
    float total = 0.0f;
    for (int i = 0; i < buildable_count; i++) {
        const UnitDef *bd = Units_GetDef(buildables[i]);
        if (!predicate(bd)) continue;
        if (!ai_limit_allows(units, unit_count, player_id, buildables[i]))
            continue;
        float w = ai_desirability(units, unit_count, player_id,
                                  buildables[i]);
        if (w <= 0.0f) continue;
        total += w;
        if ((float)ai_rand(10000) / 10000.0f * total < w) pick = i;
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
                            buildables[i]) <= 0.0f) continue;
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
    if (ai_player_has_production_structure(units, unit_count,
                                           actor->player_id)) return 0;
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
        float w = ai_desirability(units, unit_count, actor->player_id, buildables[i]);
        if (ai_trace()) {
            fprintf(stderr, "AI: production candidate %s: children=%d combat=%d limit_ok=%d weight=%.1f\n",
                    bd->unitname, cn, produces_combat, allowed, w);
        }
        if (!produces_combat || !allowed || w <= 0.0f) continue;
        if (ai_try_start_build_def(actor_idx, buildables[i])) {
            if (ai_trace()) fprintf(stderr, "AI: started production structure %s\n", bd->unitname);
            return 1;
        }
        if (ai_trace()) fprintf(stderr, "AI: could not site %s, trying the next\n", bd->unitname);
    }
    return 0;
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
    if (actor->cmd_kind == UNIT_CMD_BUILD || actor->build_target >= 0) return 0;
    if (ai_player_has_pending_combat_production(units, unit_count,
                                                actor->player_id)) {
        return 0;
    }

    int buildables[32];
    int n = Units_GetBuildables((int)actor->def_idx, buildables,
                                (int)(sizeof(buildables) / sizeof(buildables[0])));
    return ai_try_start_build_from_list(actor_idx, buildables, n,
                                        ai_def_is_combat_unit);
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
    const UnitDef *lode_def = Units_GetDef(lode);
    int lfx = (lode_def && lode_def->footprint_x > 0) ? lode_def->footprint_x : 2;
    int lfz = (lode_def && lode_def->footprint_z > 0) ? lode_def->footprint_z : 2;

    int64_t best_d2 = INT64_MAX;
    int32_t best_x = 0, best_y = 0;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        /* The sacred-site table holds features with a sacredsite tier,
         * not the whole "mana" category. The henge decor around a pad
         * shares that category (legacy:128256, :20483). */
        if (!fd || fd->sacred_site <= 0.0f) continue;
        /* Legacy anchors the build at the pad's own cell, so the
         * footprint's top-left corner lands on it (legacy:21442). */
        int32_t wx = world->features[i].tile_x * 16 + lfx * 8;
        int32_t wy = world->features[i].tile_z * 16 + lfz * 8;
        int claimed = 0;
        for (int u = 0; u < unit_count && !claimed; u++) {
            if (units[u].alive != UNIT_ALIVE_ACTIVE) continue;
            if (!ai_def_is_mana_economy(Units_GetDef(units[u].def_idx)))
                continue;
            int64_t dx = (int64_t)units[u].world_x - wx;
            int64_t dy = (int64_t)units[u].world_y - wy;
            if (dx * dx + dy * dy < 128 * 128) claimed = 1;
        }
        if (claimed) continue;
        if (!Units_IsBuildSiteClear(lode, wx, wy)) continue;
        int64_t dx = (int64_t)actor->world_x - wx;
        int64_t dy = (int64_t)actor->world_y - wy;
        int64_t d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best_x = wx; best_y = wy; }
    }
    if (best_d2 == INT64_MAX) return 0;
    return Units_BeginBuildingForUnit(actor_idx, lode, best_x, best_y) >= 0;
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
/* A base threat lapses this long after the last hit (60 Hz ticks). */
#define AI_THREAT_TTL     600
/* Engagement radius floor: 5 x range class cells x 16 px, class 10
 * for mobiles and 5 for builders (legacy:17400, :17428). A unit below
 * a quarter HP divides it by 5 with a 160 px floor (legacy:17705). */
#define AI_ENGAGE_MOBILE  800
#define AI_ENGAGE_BUILDER 400
#define AI_ENGAGE_HURT_MIN 160

typedef struct AiPlayer {
    int      active;          /* AI slot */
    int      base_known;
    int32_t  base_x, base_y;
    /* Wave target (legacy:15365 pick, legacy:18250 march). */
    int      target_player;
    int      target_handle;
    uint32_t target_stable_id;
    int32_t  target_x, target_y;
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
} AiPlayer;

static AiPlayer g_ai_players[TAK_MAX_PLAYERS + 1];
/* [from][to][0 = attack, 1 = march], read by tests. */
static int g_ai_orders[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1][2];
static int g_ai_defence_orders[TAK_MAX_PLAYERS + 1];
static int g_ai_last_tick = -1;

static void ai_reset_state(void) {
    memset(g_ai_players, 0, sizeof(g_ai_players));
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        g_ai_players[p].target_handle = -1;
        g_ai_players[p].threat_handle = -1;
        g_ai_players[p].threat_tick = -1;
    }
    memset(g_ai_orders, 0, sizeof(g_ai_orders));
    memset(g_ai_defence_orders, 0, sizeof(g_ai_defence_orders));
    g_ai_rng = 0x2A5F19C7u;   /* same seed every match: lockstep safe */
    AI_Influence_Reset();
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
 * the base threat, a hit on the monarch arms its build freeze
 * (legacy:15087-15096). Nothing is acted on until the next AI tick. */
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
    if (ap->active && vd && vd->commander) ap->freeze_pending = 1;
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

/* Wave target (legacy:15365): every unit of every non-allied player is
 * scored INT_MAX over two randomly smoothed distances, then divided
 * for being unseen (2..20), unarmed (2..10), unfinished (1..3),
 * immobile (1..3) and for its owner's small share of the world's
 * units (1..20). Best score wins. The original also demands a route;
 * our movers steer round what A* cannot solve, so that check is
 * skipped. */
static void ai_pick_wave_target(const GameWorld *world, const Unit *units,
                                int unit_count, int p) {
    AiPlayer *ap = &g_ai_players[p];
    int64_t sx = 0, sy = 0;
    int mine = 0;
    int counts[TAK_MAX_PLAYERS + 1] = { 0 };
    int total = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        if (u->player_id >= 1 && u->player_id <= TAK_MAX_PLAYERS) {
            counts[u->player_id]++;
            total++;
        }
        if (u->player_id != p || u->under_construction) continue;
        if (!ai_def_is_mobile_combat(Units_GetDef(u->def_idx))) continue;
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
    for (int i = 0; i < unit_count; i++) {
        const Unit *t = &units[i];
        if (t->alive != UNIT_ALIVE_ACTIVE) continue;
        int q = t->player_id;
        if (!ai_valid_player(world, q) || !Units_PlayersAreEnemies(p, q)) continue;
        const UnitDef *td = Units_GetDef(t->def_idx);
        if (!td) continue;
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
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) {
        ap->target_player = 0;
        ap->target_handle = -1;
        return;
    }
    ap->target_player = units[best].player_id;
    ap->target_handle = best;
    ap->target_stable_id = units[best].stable_id;
    ap->target_x = units[best].world_x;
    ap->target_y = units[best].world_y;
    if (ai_trace()) {
        fprintf(stderr, "AI %d: wave target player %d unit %d at (%d,%d)\n",
                p, ap->target_player, best, ap->target_x, ap->target_y);
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
 * else march on its position. */
static void ai_dispatch_wave(const GameWorld *world, const Unit *units,
                             int unit_count, int actor_idx, int p) {
    const AiPlayer *ap = &g_ai_players[p];
    int h = ap->target_handle;
    if (h < 0 || h >= unit_count) return;
    if (ai_visible_to(world, p, &units[h]) &&
        Units_CanAttackTarget(actor_idx, h)) {
        Units_CommandAttackUnit(actor_idx, h);
        ai_count_order(p, ap->target_player, 0);
        return;
    }
    Units_CommandMoveUnit(actor_idx, ap->target_x, ap->target_y);
    ai_count_order(p, ap->target_player, 1);
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

/* A walking producer: Zhon summons its whole army from beast handlers
 * and their kin, and priests summon dragons. A monarch never counts,
 * as in ai_player_has_production_structure. */
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
    return ai_try_start_build_from_list(actor_idx, buildables, n, ai_def_is_tower);
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

/* A walking builder in a fight is free to build, and the build replaces
 * the chase as it did before the planner (legacy:17163). A structure's
 * fight is its guns, so only an idle one counts. */
static int ai_builder_free(const Unit *u, const UnitDef *def) {
    if (u->under_construction || u->build_target >= 0) return 0;
    if (u->cmd_kind == UNIT_CMD_NONE) return 1;
    return u->cmd_kind == UNIT_CMD_ATTACK && def->max_velocity > 0.0f;
}

static void ai_plan_read(const GameWorld *world, const Unit *units,
                         int unit_count, int p, int now,
                         AiPlanState *s, AiPlanCosts *c) {
    const AiPlayer *ap = &g_ai_players[p];
    memset(s, 0, sizeof(*s));
    memset(c, 0, sizeof(*c));
    int32_t mana = Economy_GetMana(&world->economy, p);
    int32_t cap = Economy_GetMaxMana(&world->economy, p);
    int32_t diff = Economy_GetIncome(&world->economy, p)
                 - Economy_GetSpend(&world->economy, p);
    s->mana_pct = cap > 0 ? (int32_t)((int64_t)mana * 100 / cap) : 0;
    /* legacy:19859: income under spend, or level with an empty pool */
    s->stalling = diff < 0 || (diff == 0 && mana <= 0);
    int lode_def = -1, factory_def = -1, tower_def = -1, train_def = -1;
    int mobile_factory_def = -1;
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
                for (int b = 0; b < n; b++) {
                    const UnitDef *bd = Units_GetDef(buildables[b]);
                    if (!bd) continue;
                    if (lode_def < 0 && ai_def_is_mana_economy(bd))
                        lode_def = buildables[b];
                    else if (tower_def < 0 && ai_def_is_tower(bd))
                        tower_def = buildables[b];
                    else if (factory_def < 0 && ai_def_is_factory(buildables[b]))
                        factory_def = buildables[b];
                    else if (mobile_factory_def < 0 &&
                             ai_def_is_mobile_producer(buildables[b]))
                        mobile_factory_def = buildables[b];
                }
                if (!ai_build_frozen(ap, d, now) && ai_builder_free(u, d))
                    s->builders_idle++;
                /* A walking producer also trains as a factory does. */
                if (ai_def_is_mobile_producer((int)u->def_idx)) {
                    if (u->under_construction) { s->factories_pending++; continue; }
                    s->factories++;
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

    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd = Features_GetByIndex(world->features[i].global_idx);
        if (!fd || fd->sacred_site <= 0.0f) continue;
        int32_t wx = world->features[i].tile_x * 16;
        int32_t wy = world->features[i].tile_z * 16;
        int claimed = 0;
        for (int u = 0; u < unit_count && !claimed; u++) {
            if (units[u].alive != UNIT_ALIVE_ACTIVE) continue;
            if (!ai_def_is_mana_economy(Units_GetDef(units[u].def_idx))) continue;
            if (ai_within(units[u].world_x, units[u].world_y, wx, wy, 128)) claimed = 1;
        }
        if (claimed) continue;
        s->free_sites++;
        if (ap->base_known && ai_within(wx, wy, ap->base_x, ap->base_y, 2048))
            s->site_near++;
    }
    if (lode_def >= 0) {
        s->lode_target = 1 + s->free_sites / 2;
        int32_t lim = g_ai_limit[lode_def];
        if (lim >= 0 && s->lode_target > lim) s->lode_target = lim;
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
    if (ai_trace()) {
        fprintf(stderr, "AI %d: mana %d%% stall %d lode %d/%d fac %d army %d/%d "
                "threat %d exposure %d sites %d -> army goal %d act %d\n",
                p, ps.mana_pct, ps.stalling, ps.lodestones, ps.lode_target,
                ps.factories, ps.army, ps.army_home, ps.threat_home,
                ps.exposure, ps.site_near, (int)army_goal, (int)army_action);
    }

    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->player_id != p) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!ai_unit_can_fight_or_move(u, def)) continue;
        if (u->cmd_kind == UNIT_CMD_BUILD ||
            u->cmd_kind == UNIT_CMD_REPAIR ||
            u->cmd_kind == UNIT_CMD_RECLAIM ||
            u->cmd_kind == UNIT_CMD_LOAD ||
            u->cmd_kind == UNIT_CMD_UNLOAD) {
            continue;
        }
        if (def->cap_flags & UNIT_CAP_BUILDER) {
            /* Builder think: build first, fight only when idle with
             * nothing to build (legacy:17163). Never sent on waves.
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
        if (u->cmd_kind == UNIT_CMD_ATTACK) continue;
        if (ai_engage_nearby(world, units, unit_count, i, def)) continue;
        if (u->cmd_kind != UNIT_CMD_NONE) continue;
        /* Defence plans hold the units at home instead of a wave. */
        if (army_action == AI_ACT_HOLD && ap->base_known &&
            ai_within(u->world_x, u->world_y, ap->base_x, ap->base_y,
                      AI_DEFEND_RADIUS)) {
            continue;
        }
        ai_dispatch_wave(world, units, unit_count, i, p);
    }
}

void TAK_AI_TickSkirmish(GameWorld *world) {
    if (!world || !world->loaded || world->skirmish_game_over) return;
    if (world->mission.objective_count > 0 || world->mission.placement_count > 0) return;

    /* A tick count that stops climbing means a new match. */
    int now = world->skirmish_elapsed_ticks;
    if (g_ai_last_tick < 0 || now <= g_ai_last_tick) ai_reset_state();
    g_ai_last_tick = now;

    /* Re-plan at a low cadence. Unit locomotion and combat remain in
     * Units_TickEngines; the AI just issues player-equivalent orders. */
    if ((now % 60) != 0) return;
    ai_profile_load();

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    if (!units || unit_count <= 0) return;

    ai_update_bases(world, units, unit_count);
    AI_Influence_Refresh(world);
    /* Other seats keep no maps, only the hits on their bases. */
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (!ai_valid_player(world, p) || g_ai_players[p].active) continue;
        ai_promote_threat(world, units, unit_count, p, now);
    }
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (world->cfg.players[p - 1].kind != TAK_SLOT_AI) continue;
        ai_tick_player(world, units, unit_count, p, now);
    }
}
