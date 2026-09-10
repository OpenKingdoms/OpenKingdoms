#include "tak_ai.h"
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

static int ai_player_team(const GameWorld *world, int player_id) {
    if (!world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    const PlayerSlot *slot = &world->cfg.players[player_id - 1];
    if (slot->kind == TAK_SLOT_CLOSED) return 0;
    return slot->team > 0 ? slot->team : player_id;
}

static int ai_players_are_enemies(const GameWorld *world, int a, int b) {
    int ta = ai_player_team(world, a);
    int tb = ai_player_team(world, b);
    return ta > 0 && tb > 0 && ta != tb;
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

static int ai_player_economy_count(const Unit *units,
                                   int unit_count,
                                   int player_id) {
    int n = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != player_id) continue;
        if (ai_def_is_mana_economy(Units_GetDef(u->def_idx))) n++;
    }
    return n;
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

/* March an idle combat unit toward the nearest enemy start position.
 * With line-of-sight on, freshly produced units see no targets — the
 * original AI still routes attack waves at enemy bases (start
 * positions are map data the AI player legitimately knows). Once fog
 * lifts en route, the regular target-selection pass takes over. */
/* Returns 1 when the actor already stands at that start (within 256 px),
 * so the caller knows the march is over and nothing was there. */
static int ai_march_to_enemy_start(const GameWorld *world,
                                   const Unit *actor, int actor_idx) {
    int best = -1;
    int64_t best_d2 = INT64_MAX;
    for (int s = 0; s < world->num_start_positions; s++) {
        const StartPos *sp = &world->start_positions[s];
        if (sp->player < 1 || sp->player > TAK_MAX_PLAYERS) continue;
        if (!ai_players_are_enemies(world, actor->player_id, sp->player))
            continue;
        int64_t dx = (int64_t)sp->x * 16 - actor->world_x;
        int64_t dy = (int64_t)sp->z * 16 - actor->world_y;
        int64_t d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best = s; }
    }
    if (best < 0) return 0;
    if (best_d2 <= (int64_t)256 * 256) return 1;
    Units_CommandMoveUnit(actor_idx,
                          world->start_positions[best].x * 16,
                          world->start_positions[best].z * 16);
    return 0;
}

/* Nearest enemy unit. With seen_only the fog gates the pick, the way a
 * unit's own acquisition works. The original's attack groups score every
 * enemy unit and only discount an unseen one (legacy:15365), which is
 * how a lone structure far from the base still gets found. */
static int ai_select_target(const GameWorld *world,
                            const Unit *units,
                            int unit_count,
                            const Unit *actor,
                            int seen_only) {
    int best = -1;
    int64_t best_score = INT64_MAX;
    for (int i = 0; i < unit_count; i++) {
        const Unit *target = &units[i];
        if (target == actor) continue;
        if (target->alive != UNIT_ALIVE_ACTIVE) continue;
        if (!ai_players_are_enemies(world, actor->player_id, target->player_id)) continue;
        if (seen_only && world->cfg.line_of_sight &&
            !Fog_IsVisibleForPlayer(world, actor->player_id,
                                    target->world_x, target->world_y)) {
            continue;
        }
        const UnitDef *td = Units_GetDef(target->def_idx);
        int64_t score = ai_dist2_units(actor, target);
        if (!world->cfg.monarch_expendable && ai_unit_is_monarch(td)) {
            score -= (int64_t)1024 * 1024 * 1024;
        }
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

void TAK_AI_TickSkirmish(GameWorld *world) {
    if (!world || !world->loaded || world->skirmish_game_over) return;
    if (world->mission.objective_count > 0 || world->mission.placement_count > 0) return;

    /* Re-plan at a low cadence. Unit locomotion and combat remain in
     * Units_TickEngines; the AI just issues player-equivalent orders. */
    if ((world->skirmish_elapsed_ticks % 60) != 0) return;
    ai_profile_load();

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    if (!units || unit_count <= 0) return;

    for (int i = 0; i < unit_count; i++) {
        const Unit *actor_ro = &units[i];
        if (actor_ro->player_id < 1 || actor_ro->player_id > TAK_MAX_PLAYERS) continue;
        const PlayerSlot *slot = &world->cfg.players[actor_ro->player_id - 1];
        if (slot->kind != TAK_SLOT_AI) continue;

        const UnitDef *def = Units_GetDef(actor_ro->def_idx);
        if (!ai_unit_can_fight_or_move(actor_ro, def)) continue;
        if (actor_ro->cmd_kind == UNIT_CMD_BUILD ||
            actor_ro->cmd_kind == UNIT_CMD_REPAIR ||
            actor_ro->cmd_kind == UNIT_CMD_RECLAIM ||
            actor_ro->cmd_kind == UNIT_CMD_LOAD ||
            actor_ro->cmd_kind == UNIT_CMD_UNLOAD) {
            continue;
        }

        int target = ai_select_target(world, units, unit_count, actor_ro, 1);
        if (target < 0) {
            if (ai_try_start_combat_production(units, unit_count, i, def))
                continue;
            if (ai_player_economy_count(units, unit_count,
                                        actor_ro->player_id) < 1 &&
                ai_try_start_economy_build(units, unit_count, i, def))
                continue;
            if (ai_try_start_production_structure_build(units, unit_count,
                                                        i, def))
                continue;
            if (ai_try_expand_to_sacred_site(world, units, unit_count,
                                             i, def))
                continue;
            if (!(def->cap_flags & UNIT_CAP_BUILDER) &&
                def->num_weapons > 0 && def->max_velocity > 0.0f &&
                actor_ro->cmd_kind == UNIT_CMD_NONE) {
                if (ai_march_to_enemy_start(world, actor_ro, i)) {
                    /* The base is gone or was never here: hunt what is
                     * left of the enemy wherever it stands, so a last
                     * lodestone cannot stall the battle. */
                    int far = ai_select_target(world, units, unit_count,
                                               actor_ro, 0);
                    if (far >= 0) Units_CommandAttackUnit(i, far);
                }
            }
            continue;
        }

        const Unit *target_ro = &units[target];
        if (def->num_weapons > 0) {
            if (actor_ro->cmd_kind == UNIT_CMD_ATTACK &&
                actor_ro->target == target) {
                continue;
            }
            Units_CommandAttackUnit(i, target);
        } else if (def->max_velocity > 0.0f) {
            if (actor_ro->cmd_kind == UNIT_CMD_MOVE &&
                actor_ro->cmd_x == target_ro->world_x &&
                actor_ro->cmd_y == target_ro->world_y) {
                continue;
            }
            Units_CommandMoveUnit(i, target_ro->world_x, target_ro->world_y);
        }
    }
}
