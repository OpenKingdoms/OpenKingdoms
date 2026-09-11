#include "tak_ai_influence.h"
#include "tak_battle_config.h"
#include "tak_features.h"
#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <string.h>

static int32_t g_inf[TAK_MAX_PLAYERS + 1][AI_INF_LAYER_COUNT]
                    [AI_INF_MAX_W * AI_INF_MAX_H];
static int g_inf_w;
static int g_inf_h;

void AI_Influence_Reset(void) {
    memset(g_inf, 0, sizeof(g_inf));
    g_inf_w = 0;
    g_inf_h = 0;
}

void AI_Influence_Size(int *out_w, int *out_h) {
    if (out_w) *out_w = g_inf_w;
    if (out_h) *out_h = g_inf_h;
}

int AI_Influence_CellOf(int32_t world_x, int32_t world_y,
                        int *out_cx, int *out_cy) {
    if (g_inf_w <= 0 || g_inf_h <= 0) return 0;
    int cx = (int)(world_x >> AI_INF_CELL_SHIFT);
    int cy = (int)(world_y >> AI_INF_CELL_SHIFT);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= g_inf_w) cx = g_inf_w - 1;
    if (cy >= g_inf_h) cy = g_inf_h - 1;
    if (out_cx) *out_cx = cx;
    if (out_cy) *out_cy = cy;
    return 1;
}

int32_t AI_Influence_Cell(int player_id, AiInfluenceLayer layer,
                          int cx, int cy) {
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    if (layer < 0 || layer >= AI_INF_LAYER_COUNT) return 0;
    if (cx < 0 || cy < 0 || cx >= g_inf_w || cy >= g_inf_h) return 0;
    return g_inf[player_id][layer][cy * AI_INF_MAX_W + cx];
}

int32_t AI_Influence_At(int player_id, AiInfluenceLayer layer,
                        int32_t world_x, int32_t world_y) {
    int cx, cy;
    if (!AI_Influence_CellOf(world_x, world_y, &cx, &cy)) return 0;
    return AI_Influence_Cell(player_id, layer, cx, cy);
}

int32_t AI_Influence_Weakness(int player_id, int32_t world_x, int32_t world_y) {
    return AI_Influence_At(player_id, AI_INF_ENEMY_VALUE, world_x, world_y)
         - AI_Influence_At(player_id, AI_INF_THREAT, world_x, world_y);
}

int32_t AI_Influence_Exposure(int player_id, int32_t world_x, int32_t world_y) {
    int32_t own = AI_Influence_At(player_id, AI_INF_OWN_VALUE, world_x, world_y);
    int32_t threat = AI_Influence_At(player_id, AI_INF_THREAT, world_x, world_y);
    int32_t presence = AI_Influence_At(player_id, AI_INF_PRESENCE, world_x, world_y);
    if (own <= 0 || threat <= presence) return 0;
    return threat - presence + own;
}

/* Base 1, or 11 for an armed structure; each weapon adds range per
 * 100 plus 5 plus damage per 40; capped at 100 (legacy:19803). */
int32_t AI_UnitCombatValue(const UnitDef *def) {
    if (!def) return 0;
    int32_t v = (def->max_velocity <= 0.0f && def->num_weapons > 0) ? 11 : 1;
    for (int w = 0; w < def->num_weapons && w < 3; w++) {
        v += def->weapons[w].range / 100 + 5 + def->weapons[w].damage / 40;
    }
    return v > 100 ? 100 : v;
}

int32_t AI_UnitAssetValue(const UnitDef *def, int monarch_expendable) {
    if (!def) return 0;
    int is_monarch = strstr(def->category, "Monarch") != NULL;
    int fighter = def->max_velocity > 0.0f && def->num_weapons > 0 &&
                  !(def->cap_flags & UNIT_CAP_BUILDER);
    if (fighter && !is_monarch) return 0;
    int32_t v = def->build_cost / 50;
    if (v < 1) v = 1;
    if (is_monarch) v += monarch_expendable ? 20 : 60;
    return v;
}

static int inf_def_is_lodestone(const UnitDef *def) {
    if (!def || def->max_velocity > 0.0f) return 0;
    if (def->cap_flags & UNIT_CAP_BUILDER) return 0;
    return def->mogrium_income_per_sec > 0.0f || def->mogrium_storage > 0;
}

/* Stamp value on a cell with a two-ring falloff (half, quarter). */
static void inf_stamp(int p, AiInfluenceLayer layer, int cx, int cy,
                      int32_t value) {
    if (value == 0) return;
    int32_t *grid = g_inf[p][layer];
    for (int dy = -2; dy <= 2; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= g_inf_h) continue;
        for (int dx = -2; dx <= 2; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= g_inf_w) continue;
            int ring = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                     ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            grid[y * AI_INF_MAX_W + x] += value >> ring;
        }
    }
}

static int inf_players_allied(int a, int b) {
    if (a == b) return 1;
    int ta = Units_PlayerTeamId(a);
    int tb = Units_PlayerTeamId(b);
    return ta > 0 && ta == tb;
}

void AI_Influence_Refresh(const GameWorld *world) {
    if (!world) return;
    int w = (world->map_pixels_w + AI_INF_CELL_PX - 1) >> AI_INF_CELL_SHIFT;
    int h = (world->map_pixels_h + AI_INF_CELL_PX - 1) >> AI_INF_CELL_SHIFT;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > AI_INF_MAX_W) w = AI_INF_MAX_W;
    if (h > AI_INF_MAX_H) h = AI_INF_MAX_H;
    g_inf_w = w;
    g_inf_h = h;
    memset(g_inf, 0, sizeof(g_inf));

    int ai_slot[TAK_MAX_PLAYERS + 1] = { 0 };
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        ai_slot[p] = world->cfg.players[p - 1].kind == TAK_SLOT_AI;
    }

    /* Sacred sites are map data every player knows. */
    for (int i = 0; i < world->feature_count; i++) {
        const struct MapFeature *mf = &world->features[i];
        if (mf->global_idx < 0) continue;
        const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
        if (!fd || fd->sacred_site <= 0.0f) continue;
        int cx, cy;
        if (!AI_Influence_CellOf(mf->tile_x * 16, mf->tile_z * 16, &cx, &cy))
            continue;
        int32_t tier = (int32_t)(fd->sacred_site * 2.0f);
        for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
            if (ai_slot[p]) inf_stamp(p, AI_INF_WEALTH, cx, cy, tier * 5);
        }
    }

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    if (!units) return;
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        int q = u->player_id;
        if (q < 1 || q > TAK_MAX_PLAYERS) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!def) continue;
        int cx, cy;
        if (!AI_Influence_CellOf(u->world_x, u->world_y, &cx, &cy)) continue;
        int32_t combat = u->under_construction ? 0 : AI_UnitCombatValue(def);
        int32_t asset = AI_UnitAssetValue(def, world->cfg.monarch_expendable);
        if (u->under_construction) asset /= 2;
        int lode = inf_def_is_lodestone(def);
        for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
            if (!ai_slot[p]) continue;
            if (inf_players_allied(p, q)) {
                inf_stamp(p, AI_INF_PRESENCE, cx, cy, combat);
                inf_stamp(p, AI_INF_OWN_VALUE, cx, cy, asset);
                if (lode) inf_stamp(p, AI_INF_WEALTH, cx, cy, asset);
                continue;
            }
            if (!Units_PlayersAreEnemies(p, q)) continue;
            if (world->cfg.line_of_sight &&
                !Fog_IsVisibleForPlayer(world, p, u->world_x, u->world_y)) {
                continue;
            }
            inf_stamp(p, AI_INF_THREAT, cx, cy, combat);
            inf_stamp(p, AI_INF_ENEMY_VALUE, cx, cy, asset);
            if (lode) inf_stamp(p, AI_INF_WEALTH, cx, cy, asset);
        }
    }
}
