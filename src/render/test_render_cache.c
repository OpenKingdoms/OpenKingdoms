/*
 * test_render_cache.c -- a cached texture is never handed to a
 * renderer that did not make it.
 *
 * A texture dies with the renderer that made it, and the allocator can
 * hand the same address to the next renderer, so a cache that matches
 * its entry by pointer alone can decide a dead texture is live and
 * copy from it. That is what killed a screen suite slice inside
 * SDL_RenderCopy about one run in five (#116).
 *
 * The unit renderer keeps two such caches, the shadow mask and the
 * projectile art strips, and both compare the platform's renderer
 * generation as well as the pointer. This asserts they do.
 *
 * Why this is a binary of its own rather than a case in the screen
 * suite. The fixture claims a new generation without really replacing
 * the renderer, because the crash needs the allocator to hand the old
 * address back and no test can insist on that. A new generation on a
 * renderer that is still alive makes the mask be abandoned rather than
 * freed, which is right when the renderer is gone and leaks one screen
 * sized render target when it is not. In the screen suite that leak
 * moved a later pixel probe by six pixels. Really replacing the
 * renderer was tried too and walks every other cache in the engine
 * into the same hazard, terrain and minimap and HUD art among them,
 * which is more than this rule is about. Alone in a process the leak
 * costs one texture and reaches nobody.
 *
 * It needs game data and it opens a window, so it carries the
 * needs-data label and runs under the shared test lock.
 */

#include "test_framework.h"

#include "tak_battle_config.h"
#include "tak_crash.h"
#include "tak_gameloop.h"
#include "tak_hpi.h"
#include "tak_ingame.h"
#include "tak_loading.h"
#include "tak_platform.h"
#include "tak_ui.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define MAP_NAME  "two castles"
#define MAP_WORLD "aramon"

static int setup_platform(TAK_Platform *p) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SKIP_MARK("SDL init failed: %s", SDL_GetError());
        return -1;
    }
    memset(p, 0, sizeof(*p));
    p->window = SDL_CreateWindow("tak-render-cache", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, 640, 480,
                                 SDL_WINDOW_HIDDEN);
    if (!p->window) { SKIP_MARK("window failed"); return -1; }
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_SOFTWARE);
    if (!p->renderer) { SKIP_MARK("renderer failed"); return -1; }
    p->renderer_gen = TAK_Platform_NewRendererGen();
    p->canvas_w = 640; p->canvas_h = 480;
    p->window_w = 640; p->window_h = 480;
    p->scale = 1.0f;
    p->has_focus = 1;
    p->canvas_tex = SDL_CreateTexture(p->renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      p->canvas_w, p->canvas_h);
    if (!p->canvas_tex) { SKIP_MARK("canvas texture failed"); return -1; }
    return 0;
}

static void teardown_platform(TAK_Platform *p) {
    if (p->canvas_tex) SDL_DestroyTexture(p->canvas_tex);
    if (p->renderer) SDL_DestroyRenderer(p->renderer);
    if (p->window) SDL_DestroyWindow(p->window);
    SDL_Quit();
}

/* 0 on success, 1 when there is no data to run against. */
static int boot(TAK_Platform *plat, GameWorld **out) {
    if (VFS_IsInitialized()) VFS_Shutdown();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        SKIP_MARK("no data dir");
        return 1;
    }
    if (setup_platform(plat) != 0) { VFS_Shutdown(); return 1; }
    if (UI_Init() != 0) return -1;
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, MAP_NAME, sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.line_of_sight = 0;
    if (World_BeginLoad(plat, &cfg, MAP_NAME, MAP_WORLD) != 0) return -1;
    if (Loading_Init(plat) != 0) return -1;
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 4000 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(plat, 1.0f / 60.0f);
    }
    if (next != GAMESTATE_IN_GAME) return -1;
    *out = World_Get();
    return *out ? 0 : -1;
}

static void shutdown_all(TAK_Platform *plat) {
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(plat);
    UI_Shutdown();
    teardown_platform(plat);
    VFS_Shutdown();
}

TEST(a_cached_texture_is_never_reused_across_renderers) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(world);

    /* A swordsman, because it blits a shadow sprite out of the strip
     * cache as well as darkening ground through the mask. */
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    ASSERT(n > 0);
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int32_t gx = units[0].world_x + 420, gy = units[0].world_y;
    ASSERT(Units_Spawn(sword_def, 1, 0, gx, gy) >= 0);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Units_SetShadowsOn(1);
    world->cam_x = gx - world->viewport_w / 2;
    world->cam_y = gy - world->viewport_h / 2;
    Units_Render(world, &platform);

    /* Both caches now say which renderer made what they hold. */
    const uint32_t gen1 = platform.renderer_gen;
    ASSERT(gen1 != 0);
    ASSERT_EQ_INT((int)gen1, (int)Units_DebugShadowMaskGen());
    ASSERT(Units_DebugProjStripsOnGen(gen1) > 0);

    /* A different renderer on the same address. Nothing has told the
     * caches, which is the whole hazard. */
    platform.renderer_gen = TAK_Platform_NewRendererGen();
    const uint32_t gen2 = platform.renderer_gen;
    ASSERT(gen2 != gen1);
    Units_Render(world, &platform);

    ASSERT_EQ_INT((int)gen2, (int)Units_DebugShadowMaskGen());
    /* Only the art this frame asked for is uploaded again, so the old
     * generation may still hold strips nothing drew twice. What must
     * be true is that the strips this frame drew came back under the
     * new generation rather than being taken from the old one. */
    ASSERT(Units_DebugProjStripsOnGen(gen2) > 0);

    shutdown_all(&platform);
}

/* A platform that never claimed a generation keeps nothing, so a new
 * renderer site that forgets the call re-uploads every frame instead
 * of handing over a dead texture. */
TEST(a_platform_with_no_generation_caches_nothing) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(world);

    int n = 0;
    const Unit *units = Units_GetActive(&n);
    ASSERT(n > 0);
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int32_t gx = units[0].world_x + 420, gy = units[0].world_y;
    ASSERT(Units_Spawn(sword_def, 1, 0, gx, gy) >= 0);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Units_SetShadowsOn(1);
    world->cam_x = gx - world->viewport_w / 2;
    world->cam_y = gy - world->viewport_h / 2;

    platform.renderer_gen = 0;
    Units_Render(world, &platform);
    Units_Render(world, &platform);
    ASSERT_EQ_INT(0, (int)Units_DebugShadowMaskGen());
    ASSERT_EQ_INT(0, Units_DebugProjStripsOnGen(0));

    shutdown_all(&platform);
}

int main(void) {
    TAK_Crash_Install();
    TEST_SUITE("Renderer owned texture caches");
    RUN(a_cached_texture_is_never_reused_across_renderers);
    RUN(a_platform_with_no_generation_caches_nothing);
    TEST_REPORT();
}
