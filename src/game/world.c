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
#include <string.h>

static GameWorld *g_world = NULL;

/* The one simulation generator: a Lehmer sequence, multiplier 16807
 * over 2^31-1 folded the Schrage way, seeded by xor with 0x66e29572
 * and forced odd (legacy:254475-254490). Scripts draw from it too. */
static uint32_t g_sim_rand_state = 1;

void World_SeedRand(uint32_t seed) {
    g_sim_rand_state = (seed ^ 0x66e29572u) | 1u;
}

uint32_t World_Rand(uint32_t n) {
    if (n < 2) return 0;
    int32_t x = (int32_t)g_sim_rand_state;
    int32_t hi = x / 0x1f31d;
    int32_t lo = x % 0x1f31d;
    x = 0x41a7 * lo - 0x2781 * hi;
    if (x < 1) x += 0x7fffffff;
    g_sim_rand_state = (uint32_t)x;
    return g_sim_rand_state % n;
}

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
    TAK_PathCacheReset();
    /* A fixed seed until the battle room shares one: every peer of a
     * lockstep game has to draw the same sequence. */
    World_SeedRand(0x4d2);
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
