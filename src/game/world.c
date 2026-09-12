/*
 * world.c -- GameWorld lifecycle.
 *
 * The module-global pointer is NULL iff no world is live. Every public
 * function is safe to call in any lifecycle state; calling World_End()
 * on no-world or double-calling it is a no-op, and World_BeginLoad on
 * an already-live world performs an implicit tear-down first.
 */

#include "tak_world.h"
#include "tak_terrain.h"
#include "tak_minimap.h"
#include "tak_unit.h"
#include "tak_tex_atlas.h"
#include "tak_economy.h"
#include "tak_pathing.h"
#include "tak_occupancy.h"
#include "tak_memory.h"
#include "tak_mission.h"
#include "tak_fog.h"
#include "tak_command_queue.h"
#include "tak_sim_rand.h"
#include "tak_ai.h"
#include <string.h>

static GameWorld *g_world = NULL;

static void copy_bounded(char *dst, size_t cap, const char *src) {
    if (!src || cap == 0) { if (cap > 0) dst[0] = '\0'; return; }
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

int World_BeginLoad(TAK_Platform       *plat,
                    const BattleConfig *cfg,
                    const char         *map_name,
                    const char         *kingdom) {
    if (!cfg || !map_name) return -1;

    /* Defensive tear-down: a second Play click, or any caller that
     * forgot to pair End/BeginLoad, must not leak. */
    if (g_world) World_End(plat);

    g_world = (GameWorld *)tak_malloc(sizeof(GameWorld));
    if (!g_world) return -1;
    memset(g_world, 0, sizeof(GameWorld));

    g_world->cfg = *cfg;
    copy_bounded(g_world->map_name,    sizeof(g_world->map_name),    map_name);
    copy_bounded(g_world->map_kingdom, sizeof(g_world->map_kingdom), kingdom);
    g_world->loaded = 0;
    Economy_Init(&g_world->economy);
    /* One seed for every simulation draw, and the AI starts the match
     * from it rather than from whatever the last battle left behind. */
    World_SeedRand(cfg->seed);
    TAK_AI_BeginMatch(cfg->seed);
    /* A new battle starts with an empty queue at tick zero, so an order
     * left over from the last one cannot reach a unit that reuses its
     * stable id. */
    TAK_CmdQueue_Reset(0);
    TAK_PathCacheReset();
    return 0;
}

GameWorld *World_Get(void) {
    return g_world;
}

void World_MarkLoaded(void) {
    if (g_world) g_world->loaded = 1;
}

void World_End(TAK_Platform *plat) {
    if (!g_world) return;
    TAK_CmdQueue_Reset(0);
    /* Release any loader-owned sub-resources in reverse dependency
     * order. TerrainGrid_Free walks every cell and destroys GPU
     * textures via plat->renderer, so it must run before the platform
     * itself is torn down. TNT_Close handles the raw buffer, the
     * RGBA minimap, and the ingame_minimap_bg pointer. */
    Minimap_Shutdown(plat);
    Units_ClearInstances();
    Units_FreeDefs();
    /* Texture atlases outlive individual unit defs — they're shared
     * across the whole map. Free after defs because mesh-bake (later
     * milestone) might hold UV refs that point into them; Units_FreeDefs
     * walks those down first. */
    TexAtlas_Shutdown(plat);
    if (g_world->grid) {
        TerrainGrid_Free(g_world->grid, plat);
        g_world->grid = NULL;
    }
    if (g_world->features) {
        tak_free(g_world->features);
        g_world->features = NULL;
        g_world->feature_count = 0;
        g_world->feature_cap = 0;
    }
    Fog_Free(g_world);
    Occ_Free(g_world);
    TNT_Close(&g_world->tnt);
    Mission_Free(&g_world->mission);
    tak_free(g_world);
    g_world = NULL;
}
