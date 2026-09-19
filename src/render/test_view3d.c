/*
 * test_view3d.c -- the 3D view leaves the classic view untouched.
 *
 * A battle drawn once in the classic view, then in the 3D view, then
 * in the classic view again has to give the same classic pixels as if
 * the 3D view had never been up, the camera has to come back over the
 * same ground, and the selection has to survive. The 3D frame itself
 * has to be a real picture, and its pointer has to land where the
 * camera looks. The read only accessors the view depends on are
 * checked here too.
 *
 * It needs game data and a window with a GL context, so it carries
 * the needs-data label and runs under the shared test lock. Where the
 * opengl renderer cannot be made, every case skips.
 */

#include "test_framework.h"

#include "tak_battle_config.h"
#include "tak_crash.h"
#include "tak_features.h"
#include "tak_gameloop.h"
#include "tak_hpi.h"
#include "tak_hud.h"
#include "tak_ingame.h"
#include "tak_loading.h"
#include "tak_platform.h"
#include "tak_terrain.h"
#include "tak_ui.h"
#include "tak_unit.h"
#include "tak_fog.h"
#include "tak_view.h"
#include "tak_view3d.h"
#include "tak_model_store.h"
#include "tak_gl3d.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define MAP_NAME  "two castles"
#define MAP_WORLD "aramon"
#define WIN_W 800
#define WIN_H 600

/* The classic renderer's frame counter steps once per render and picks
 * frame counter/4 of every animated feature sprite (and counter/2 of a
 * flyer's shadow strip), so two classic frames of a frozen world draw
 * the same picture only when they are a whole number of every one of
 * those cycles apart. This works that number out for the map. */
static int gcd_int(int a, int b) {
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

static int classic_phase(const GameWorld *world) {
    int phase = 4;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd = Features_GetByIndex(world->features[i].global_idx);
        if (!fd || fd->object[0] || !fd->filename[0]) continue;
        const uint32_t *px = NULL;
        int w = 0, h = 0, ox = 0, oy = 0;
        int frames = Units_FeatureSpriteFrame(fd, world->features_rgba, 0,
                                              &px, &w, &h, &ox, &oy);
        if (frames <= 1) continue;
        int cycle = 4 * frames;
        phase = phase / gcd_int(phase, cycle) * cycle;
    }
    /* A flyer's shadow strip runs its frames every two renders. */
    phase = phase / gcd_int(phase, 20) * 20;
    return phase;
}

static int setup_platform(TAK_Platform *p) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SKIP_MARK("SDL init failed: %s", SDL_GetError());
        return -1;
    }
    memset(p, 0, sizeof(*p));
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    p->window = SDL_CreateWindow("tak-view3d", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H,
                                 SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!p->window) { SKIP_MARK("window failed"); return -1; }
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_ACCELERATED);
    if (!p->renderer) { SKIP_MARK("no opengl renderer: %s", SDL_GetError()); return -1; }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(p->renderer, &info) != 0 ||
        !info.name || strcmp(info.name, "opengl") != 0) {
        SKIP_MARK("renderer is not opengl");
        return -1;
    }
    p->renderer_gen = TAK_Platform_NewRendererGen();
    p->canvas_w = 640; p->canvas_h = 480;
    p->window_w = WIN_W; p->window_h = WIN_H;
    p->scale = 1.0f;
    /* No focus: no cursor animation and no scroll, so a frame is a
     * function of the world alone. */
    p->has_focus = 0;
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

/* 0 on success, 1 when there is no data or no GL to run against.
 * `data_dir` is the loose file folder, the build's own by default. */
static int boot_with_data(TAK_Platform *plat, GameWorld **out, const char *data_dir);
static int boot(TAK_Platform *plat, GameWorld **out) {
    return boot_with_data(plat, out, TAK_DATA_DIR);
}
static int boot_with_data(TAK_Platform *plat, GameWorld **out, const char *data_dir) {
    if (VFS_IsInitialized()) VFS_Shutdown();
    if (VFS_Init(TAK_GAME_DIR, data_dir) != 0) {
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
    if (!*out) return -1;
    if (InGame_Init(plat) != 0) return -1;
    return 0;
}

static void shutdown_all(TAK_Platform *plat) {
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(plat);
    UI_Shutdown();
    teardown_platform(plat);
    VFS_Shutdown();
}

/* One frame with no simulation time: the world stays where it is. */
static int frame(TAK_Platform *plat, Timer *timer) {
    timer->accumulator = 0.0;
    TAK_Platform_FrameBegin(plat);
    return InGame_Tick(plat, timer) == GAMESTATE_IN_GAME;
}

static int capture(TAK_Platform *plat, uint32_t *out) {
    return SDL_RenderReadPixels(plat->renderer, NULL, SDL_PIXELFORMAT_RGBA32,
                                out, WIN_W * 4) == 0;
}

/* Counts the pixels that differ, and on a mismatch says where they are
 * and writes both frames beside the binary for a look. */
static int differing_pixels(const uint32_t *a, const uint32_t *b) {
    int n = 0, x0 = WIN_W, y0 = WIN_H, x1 = -1, y1 = -1;
    for (int i = 0; i < WIN_W * WIN_H; i++) {
        if (a[i] == b[i]) continue;
        n++;
        int x = i % WIN_W, y = i / WIN_W;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
    }
    if (n > 0) {
        printf("(%d pixels differ in x %d..%d y %d..%d) ", n, x0, x1, y0, y1);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)a, WIN_W, WIN_H, 32,
                                                           WIN_W * 4, SDL_PIXELFORMAT_RGBA32);
        if (s) { SDL_SaveBMP(s, "view3d-diff-a.bmp"); SDL_FreeSurface(s); }
        s = SDL_CreateRGBSurfaceWithFormatFrom((void *)b, WIN_W, WIN_H, 32,
                                               WIN_W * 4, SDL_PIXELFORMAT_RGBA32);
        if (s) { SDL_SaveBMP(s, "view3d-diff-b.bmp"); SDL_FreeSurface(s); }
    }
    return n;
}

/* Lit pixels and distinct colours, the same measure the screen suite
 * uses to tell a picture from a blank. */
static void signal_of(const uint32_t *px, int *out_lit, int *out_bins) {
    uint32_t bins[64];
    int nb = 0, lit = 0;
    for (int i = 0; i < WIN_W * WIN_H; i += 3) {
        uint32_t p = px[i];
        uint8_t r = (uint8_t)p, g = (uint8_t)(p >> 8), b = (uint8_t)(p >> 16);
        if (!(r || g || b)) continue;
        lit++;
        uint32_t bin = ((uint32_t)(r >> 4) << 8) | ((uint32_t)(g >> 4) << 4) | (b >> 4);
        int seen = 0;
        for (int k = 0; k < nb; k++) if (bins[k] == bin) { seen = 1; break; }
        if (!seen && nb < 64) bins[nb++] = bin;
    }
    *out_lit = lit;
    *out_bins = nb;
}

TEST(toggling_3d_on_and_off_leaves_the_classic_frame_byte_identical) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    Timer timer;
    Timer_Init(&timer);
    uint32_t *a = (uint32_t *)malloc((size_t)WIN_W * WIN_H * 4);
    uint32_t *b = (uint32_t *)malloc((size_t)WIN_W * WIN_H * 4);
    uint32_t *c = (uint32_t *)malloc((size_t)WIN_W * WIN_H * 4);
    ASSERT(a && b && c);

    /* Something on screen worth comparing: the monarch, selected. */
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    ASSERT(n > 0);
    Units_SelectSingle(0);
    world->cam_x = units[0].world_x - world->viewport_w / 2;
    world->cam_y = units[0].world_y - world->viewport_h / 2;
    if (world->cam_x < 0) world->cam_x = 0;
    if (world->cam_y < 0) world->cam_y = 0;
    const int32_t cam_x0 = world->cam_x, cam_y0 = world->cam_y;

    /* The control: with the world frozen, two classic frames a phase
     * apart draw the same bytes. This is what makes the comparison
     * below mean something. */
    const int phase = classic_phase(world);
    printf("(phase %d) ", phase);
    for (int i = 0; i < phase; i++) ASSERT(frame(&platform, &timer));
    ASSERT(capture(&platform, a));
    for (int i = 0; i < phase; i++) ASSERT(frame(&platform, &timer));
    ASSERT(capture(&platform, b));
    ASSERT_EQ_INT(0, differing_pixels(a, b));
    int lit = 0, bins = 0;
    signal_of(a, &lit, &bins);
    ASSERT(lit > 20000);
    ASSERT(bins >= 8);

    /* Into the 3D view, draw a few frames there, and back. */
    ASSERT_EQ_INT(1, InGame_SetView3D(1));
    ASSERT_EQ_INT(1, InGame_IsView3D());
    for (int i = 0; i < 3; i++) ASSERT(frame(&platform, &timer));
    ASSERT(capture(&platform, c));
    ASSERT_EQ_INT(1, InGame_SetView3D(0));
    ASSERT_EQ_INT(0, InGame_IsView3D());
    for (int i = 0; i < phase; i++) ASSERT(frame(&platform, &timer));
    ASSERT(capture(&platform, b));
    ASSERT_EQ_INT(0, differing_pixels(a, b));

    /* The camera came back over the same ground and the selection
     * survived the trip. */
    ASSERT_EQ_INT((int)cam_x0, (int)world->cam_x);
    ASSERT_EQ_INT((int)cam_y0, (int)world->cam_y);
    int sel_n = 0;
    const int *sel = Units_GetSelection(&sel_n);
    ASSERT_EQ_INT(1, sel_n);
    ASSERT_EQ_INT(0, sel[0]);

    /* And the 3D frame was a different, real picture. */
    ASSERT(differing_pixels(a, c) > 20000);
    signal_of(c, &lit, &bins);
    ASSERT(lit > 20000);
    ASSERT(bins >= 8);

    free(a); free(b); free(c);
    shutdown_all(&platform);
}

/* Height of the ground at a point, for the pointer check. */
TEST(the_3d_pointer_lands_where_the_camera_looks) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    Timer timer;
    Timer_Init(&timer);
    ASSERT(frame(&platform, &timer));
    ASSERT_EQ_INT(1, InGame_SetView3D(1));
    ASSERT(frame(&platform, &timer));
    const Camera3D *cam = View3D_Camera();
    /* The play area's middle pixel looks at the target. */
    SDL_Rect play = { 0, 0, WIN_W, WIN_H };
    (void)HUD_GetViewportRect(&platform, &play);
    int mx = play.x + play.w / 2, my = play.y + play.h / 2;
    int32_t fx = 0, fy = 0;
    ASSERT_EQ_INT(1, View_3D()->pointer_to_world(world, &platform, mx, my, &fx, &fy));
    int h = Terrain_SampleHeight(world, (int32_t)cam->target_x, (int32_t)cam->target_z);
    int32_t want_y = (int32_t)cam->target_z - (int32_t)((float)h * Units_GetTanTilt());
    ASSERT(abs(fx - (int32_t)cam->target_x) <= 2);
    ASSERT(abs(fy - want_y) <= 3);
    /* Off the play area, over the sidebar, is off the world. */
    ASSERT_EQ_INT(0, View_3D()->pointer_to_world(world, &platform, WIN_W + 50, my, &fx, &fy));
    shutdown_all(&platform);
}

TEST(a_scroll_in_3d_moves_the_classic_camera_with_it) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    Timer timer;
    Timer_Init(&timer);
    world->cam_x = 400; world->cam_y = 400;
    ASSERT(frame(&platform, &timer));
    ASSERT_EQ_INT(1, InGame_SetView3D(1));
    ASSERT(frame(&platform, &timer));
    const Camera3D *cam = View3D_Camera();
    float tx = cam->target_x, tz = cam->target_z;
    /* The classic preset looks north, so right is east and down is south. */
    View_3D()->scroll(world, 100, 50);
    float scale = cam->dist / 1000.0f;
    ASSERT(cam->target_x > tx + 50.0f * scale);
    ASSERT(cam->target_z > tz + 25.0f * scale);
    ASSERT_EQ_INT((int)cam->target_x - world->viewport_w / 2, (int)world->cam_x);
    ASSERT_EQ_INT((int)cam->target_z - world->viewport_h / 2, (int)world->cam_y);
    /* A minimap jump moves the classic camera; the 3D view follows. */
    world->cam_x = 900; world->cam_y = 700;
    ASSERT(frame(&platform, &timer));
    ASSERT_EQ_INT(900 + world->viewport_w / 2, (int)cam->target_x);
    ASSERT_EQ_INT(700 + world->viewport_h / 2, (int)cam->target_z);
    shutdown_all(&platform);
}

TEST(the_accessors_hand_out_the_baked_model_and_its_pose) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);

    UnitMesh *m = Units_BakeObjectMesh("araking", 0);
    ASSERT_NOT_NULL(m);
    ASSERT(m->node_count > 1);
    ASSERT(m->vert_count > 0);
    ASSERT_EQ_INT(-1, (int)m->nodes[0].parent);
    /* Every vertex names a node the mesh has. */
    for (int v = 0; v < m->vert_count; v++) ASSERT(m->vert_node_idx[v] < m->node_count);
    /* At rest each node sits at its parent plus its own offset. */
    UnitNodeXform xf[UNIT_MESH_MAX_NODES];
    Units_ComposeNodeXforms(m, NULL, xf, 1);
    for (int i = 1; i < m->node_count; i++) {
        int p = m->nodes[i].parent;
        ASSERT(p >= 0 && p < i);
        for (int k = 0; k < 3; k++) {
            /* Offsets run to a million model units, so a float sum is
             * only good to a fraction of a unit. */
            float want = xf[p].trans[k] + m->nodes[i].offset[k];
            ASSERT(xf[i].trans[k] > want - 1.0f && xf[i].trans[k] < want + 1.0f);
        }
    }
    /* A missing model is a NULL, not a crash. */
    ASSERT_NULL(Units_BakeObjectMesh("no_such_model_here", 0));
    Units_FreeBakedMesh(m);

    /* A feature with a sprite on this map decodes to a frame. */
    int found = 0;
    for (int i = 0; i < world->feature_count && !found; i++) {
        const FeatureDef *fd = Features_GetByIndex(world->features[i].global_idx);
        if (!fd || fd->object[0] || !fd->filename[0]) continue;
        const uint32_t *px = NULL;
        int w = 0, h = 0, ox = 0, oy = 0;
        int frames = Units_FeatureSpriteFrame(fd, world->features_rgba, 0,
                                              &px, &w, &h, &ox, &oy);
        if (frames <= 0) continue;
        ASSERT_NOT_NULL(px);
        ASSERT(w > 0 && h > 0);
        found = 1;
    }
    ASSERT_EQ_INT(1, found);
    shutdown_all(&platform);
}

/* Which nodes of a mesh moved between two poses. */
static int nodes_that_moved(const UnitMesh *m, const UnitNodeXform *a,
                            const UnitNodeXform *b) {
    int moved = 0;
    for (int i = 0; i < m->node_count; i++) {
        for (int k = 0; k < 9; k++) {
            if (a[i].rot[k] != b[i].rot[k]) { moved++; break; }
        }
    }
    return moved;
}

/* The walk cycle reaches the 3D pose: a unit ordered to march has piece
 * rotations that change from one moment to the next, composed over the
 * store's own bake of its model, which is what the 3D view draws. */
TEST(a_walking_units_pieces_move_in_the_3d_pose) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    ASSERT(n > 0);
    const UnitDef *def = Units_GetDef(units[0].def_idx);
    ASSERT_NOT_NULL(def);
    UnitMesh *m = Units_BakeObjectMesh(def->objectname, units[0].team_color_idx);
    ASSERT_NOT_NULL(m);
    ASSERT_NOT_NULL(units[0].cob);

    /* At rest nothing moves between two moments. */
    static UnitNodeXform a[UNIT_MESH_MAX_NODES], b[UNIT_MESH_MAX_NODES];
    InGame_DebugRunSimTicks(30);
    Units_ComposeNodeXforms(m, units[0].cob->pieces, a, 1);
    InGame_DebugRunSimTicks(30);
    Units_ComposeNodeXforms(m, units[0].cob->pieces, b, 1);
    ASSERT_EQ_INT(0, nodes_that_moved(m, a, b));

    /* Marching, the legs and arms swing. */
    ASSERT_EQ_INT(1, Units_OrderMove(0, units[0].world_x + 600, units[0].world_y));
    InGame_DebugRunSimTicks(90);
    Units_ComposeNodeXforms(m, units[0].cob->pieces, a, 1);
    InGame_DebugRunSimTicks(7);
    Units_ComposeNodeXforms(m, units[0].cob->pieces, b, 1);
    ASSERT(nodes_that_moved(m, a, b) >= 2);
    Units_FreeBakedMesh(m);
    shutdown_all(&platform);
}

/* Reported from play: in the 3D view a lodestone's placement ghost
 * was always red. The ghost was judged at the classic camera's flat
 * reading of the pointer, a point the 3D camera is not looking at. */
/* The monarch fires at the ground; the frame after the shot leaves the
 * muzzle has to show it. */
TEST(a_projectile_in_flight_is_drawn_in_the_3d_view) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    Timer timer;
    Timer_Init(&timer);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    ASSERT(n > 0);
    world->cam_x = units[0].world_x - world->viewport_w / 2;
    world->cam_y = units[0].world_y - world->viewport_h / 2;
    if (world->cam_x < 0) world->cam_x = 0;
    if (world->cam_y < 0) world->cam_y = 0;
    ASSERT_EQ_INT(1, InGame_SetView3D(1));
    ASSERT(frame(&platform, &timer));
    ASSERT_EQ_INT(1, Units_OrderAttackGround(0, units[0].world_x + 300, units[0].world_y));
    int in_flight = 0;
    for (int i = 0; i < 600 && !in_flight; i++) {
        InGame_DebugRunSimTicks(1);
        int pn = 0;
        const Projectile *ps = Units_GetProjectiles(&pn);
        for (int k = 0; k < pn; k++) if (ps[k].alive) { in_flight = 1; break; }
    }
    ASSERT(in_flight);
    ASSERT(frame(&platform, &timer));
    View3DDrawCounts c = View3D_DebugDrawCounts();
    printf("(units %d projectiles %d beams %d) ", c.units, c.projectiles, c.beams);
    ASSERT(c.projectiles + c.beams >= 1);
    shutdown_all(&platform);
}

/* When the shot lands, the impact sprite plays where it hit. The
 * computer player's own build sparkles under fog come first and do
 * not count. */
TEST(an_impact_effect_is_drawn_in_the_3d_view) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    Timer timer;
    Timer_Init(&timer);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    ASSERT(n > 0);
    world->cam_x = units[0].world_x - world->viewport_w / 2;
    world->cam_y = units[0].world_y - world->viewport_h / 2;
    if (world->cam_x < 0) world->cam_x = 0;
    if (world->cam_y < 0) world->cam_y = 0;
    ASSERT_EQ_INT(1, InGame_SetView3D(1));
    ASSERT(frame(&platform, &timer));
    /* A shooter whose weapon has an explosion class and a projectile;
     * the monarch's beam has neither. */
    int shooter_def = -1;
    int32_t range = 0;
    for (int i = 0; i < Units_GetDefCount() && shooter_def < 0; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || d->num_weapons <= 0) continue;
        const UnitWeapon *w = &d->weapons[0];
        if (!w->explosion_class[0] || !(w->model[0] || w->weapon_art[0])) continue;
        if (w->range < 200) continue;
        shooter_def = i;
        range = w->range;
    }
    ASSERT(shooter_def >= 0);
    int shooter = Units_Spawn(shooter_def, units[0].player_id, units[0].team_color_idx,
                              units[0].world_x + 96, units[0].world_y);
    ASSERT(shooter >= 0);
    printf("(%s) ", Units_GetDef(shooter_def)->weapons[0].name);
    ASSERT_EQ_INT(1, Units_OrderAttackGround(shooter, units[0].world_x + 96 + range / 2,
                                             units[0].world_y));
    int playing = 0;
    for (int i = 0; i < 900 && !playing; i++) {
        InGame_DebugRunSimTicks(1);
        int en = 0;
        const ProjectileEffect *es = Units_GetProjectileEffects(&en);
        for (int k = 0; k < en; k++) {
            if (es[k].alive && Fog_ShowsAt(world, es[k].world_x, es[k].world_y)) { playing = 1; break; }
        }
    }
    ASSERT(playing);
    ASSERT(frame(&platform, &timer));
    View3DDrawCounts c = View3D_DebugDrawCounts();
    printf("(effects %d) ", c.effects);
    ASSERT(c.effects >= 1);
    shutdown_all(&platform);
}

/* The first def whose weapon in some slot passes `want`; -1 if none. */
static int find_weapon(int (*want)(const UnitWeapon *), int *out_slot) {
    for (int i = 0; i < Units_GetDefCount(); i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d) continue;
        for (int k = 0; k < d->num_weapons && k < 3; k++) {
            if (want(&d->weapons[k])) { *out_slot = k; return i; }
        }
    }
    return -1;
}
static int is_flame(const UnitWeapon *w) { return w->los_kind == 2; }
static int is_rings(const UnitWeapon *w) { return w->remote_kind == 1; }
static int is_rain(const UnitWeapon *w)  { return w->remote_kind == 2; }

/* Spawns a def beside the monarch for the local player. */
static int spawn_beside_monarch(int def, int32_t *out_x, int32_t *out_y) {
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n <= 0) return -1;
    *out_x = units[0].world_x + 96;
    *out_y = units[0].world_y;
    return Units_Spawn(def, units[0].player_id, units[0].team_color_idx, *out_x, *out_y);
}

static int count_effects(int sprite, int *out_delayed) {
    int en = 0, c = 0;
    const ProjectileEffect *es = Units_GetProjectileEffects(&en);
    if (out_delayed) *out_delayed = 0;
    for (int i = 0; i < en; i++) {
        if (!es[i].alive || es[i].sprite_idx != sprite) continue;
        c++;
        if (out_delayed && es[i].delay_ticks) (*out_delayed)++;
    }
    return c;
}

/* A flame weapon sheds a particle a tick along its line and draws no ray. */
TEST(a_flame_weapon_streams_particles_instead_of_a_ray) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int slot = 0;
    int def = find_weapon(is_flame, &slot);
    ASSERT(def >= 0);
    int32_t x = 0, y = 0;
    int h = spawn_beside_monarch(def, &x, &y);
    ASSERT(h >= 0);
    printf("(%s) ", Units_GetDef(def)->weapons[slot].name);
    ASSERT_EQ_INT(1, Units_DebugFireGround(h, slot, x + 150, y));
    InGame_DebugRunSimTicks(12);
    int pn = 0, flame_beams = 0;
    const Projectile *ps = Units_GetProjectiles(&pn);
    for (int i = 0; i < pn; i++) {
        if (ps[i].alive && ps[i].is_beam && ps[i].visual_kind == UNIT_PROJECTILE_VIS_FLAME) flame_beams++;
    }
    ASSERT_EQ_INT(1, flame_beams);
    int sprite = Units_FindSpriteArt("flame");
    ASSERT(sprite >= 0);
    ASSERT(count_effects(sprite, NULL) >= 6);
    shutdown_all(&platform);
}

/* A ring spell lays ringcount rings of spritecount radiusart sprites
 * that travel outward, and its own shot goes unseen. */
TEST(a_ring_spell_lays_its_rings_from_the_data) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int slot = 0;
    int def = find_weapon(is_rings, &slot);
    ASSERT(def >= 0);
    const UnitWeapon *w = &Units_GetDef(def)->weapons[slot];
    int32_t x = 0, y = 0;
    int h = spawn_beside_monarch(def, &x, &y);
    ASSERT(h >= 0);
    printf("(%s) ", w->name);
    int32_t cx = x + 60, cy = y;
    ASSERT_EQ_INT(1, Units_DebugFireGround(h, slot, cx, cy));
    int pn = 0, hidden = 0;
    const Projectile *ps = Units_GetProjectiles(&pn);
    for (int i = 0; i < pn; i++) if (ps[i].alive && ps[i].hidden) hidden++;
    ASSERT_EQ_INT(1, hidden);
    InGame_DebugRunSimTicks(w->buildup_ticks + (w->ring_count - 1) * w->ring_delay_ticks + 2);
    int total = 0;
    for (int k = 0; k < w->ring_count && k < 3; k++) {
        ASSERT(w->radius_sprite[k] >= 0);
        ASSERT_EQ_INT(w->radius_sprite[k], Units_FindSpriteArt(w->radius_art[k]));
    }
    int en = 0, outward = 0;
    const ProjectileEffect *es = Units_GetProjectileEffects(&en);
    for (int i = 0; i < en; i++) {
        const ProjectileEffect *e = &es[i];
        if (!e->alive) continue;
        int ring = 0;
        for (int k = 0; k < 3; k++) if (w->radius_sprite[k] >= 0 && e->sprite_idx == w->radius_sprite[k]) ring = 1;
        if (!ring) continue;
        total++;
        long long away = (long long)(e->world_x - cx) * e->vx_fp + (long long)(e->world_y - cy) * e->vy_fp;
        if (!e->delay_ticks && (e->vx_fp || e->vy_fp) && away >= 0) outward++;
    }
    printf("(%d rings of %d, %d moving outward) ", w->ring_count, w->sprite_count, outward);
    ASSERT_EQ_INT(w->ring_count * w->sprite_count, total);
    ASSERT(outward >= w->sprite_count);
    shutdown_all(&platform);
}

/* A storm queues particlespersecond drops a second for its duration,
 * each to burst with the weapon's explosionclass where it lands. */
TEST(a_storm_rains_its_drops_from_the_data) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    int slot = 0;
    int def = find_weapon(is_rain, &slot);
    ASSERT(def >= 0);
    const UnitWeapon *w = &Units_GetDef(def)->weapons[slot];
    int32_t x = 0, y = 0;
    int h = spawn_beside_monarch(def, &x, &y);
    ASSERT(h >= 0);
    printf("(%s) ", w->name);
    ASSERT_EQ_INT(1, Units_DebugFireGround(h, slot, x + 200, y));
    InGame_DebugRunSimTicks(1);
    ASSERT(w->rain_sprite >= 0);
    ASSERT_EQ_INT(w->rain_sprite, Units_FindSpriteArt(w->weapon_art));
    int delayed = 0;
    int drops = count_effects(w->rain_sprite, &delayed);
    printf("(%d drops, %d still to fall) ", drops, delayed);
    ASSERT_EQ_INT(w->rain_per_second * w->rain_ticks / 60, drops);
    int en = 0, bursting = 0;
    const ProjectileEffect *es = Units_GetProjectileEffects(&en);
    for (int i = 0; i < en; i++) {
        if (es[i].alive && es[i].sprite_idx == w->rain_sprite && es[i].land_explosion == w->explosion_idx) bursting++;
    }
    ASSERT_EQ_INT(drops, bursting);
    shutdown_all(&platform);
}

TEST(the_build_ghost_in_3d_is_judged_where_the_pointer_lands) {
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot(&platform, &world);
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    Timer timer;
    Timer_Init(&timer);
    ASSERT(frame(&platform, &timer));
    ASSERT_EQ_INT(1, InGame_SetView3D(1));
    ASSERT(frame(&platform, &timer));

    /* Something the local monarch can build. */
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    int builder = -1;
    for (int i = 0; i < n && builder < 0; i++) {
        const UnitDef *d = Units_GetDef(units[i].def_idx);
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].player_id == 1 &&
            d && (d->cap_flags & UNIT_CAP_BUILDER)) builder = i;
    }
    ASSERT(builder >= 0);
    int buildables[32];
    int bn = Units_GetBuildables((int)units[builder].def_idx, buildables, 32);
    ASSERT(bn > 0);
    Units_SelectSingle(builder);
    HUD_SetCommandMode(HUD_CMD_PLACE_BUILD);
    HUD_BeginBuildPlacement(buildables[0]);

    /* The pointer at the play area's middle, and the ground the 3D
     * view says is under it. */
    SDL_Rect play = { 0, 0, WIN_W, WIN_H };
    (void)HUD_GetViewportRect(&platform, &play);
    int mx = play.x + play.w / 2, my = play.y + play.h / 2;
    int32_t flat_x = 0, flat_y = 0;
    ASSERT_EQ_INT(1, View_3D()->pointer_to_world(world, &platform, mx, my,
                                                 &flat_x, &flat_y));
    int32_t want_x = flat_x, want_y = flat_y;
    Units_GroundUnderPoint(flat_x, flat_y, &want_x, &want_y);

    HUD_DrawCommandCursor(&platform, mx, my, flat_x, flat_y);
    int32_t got_x = 0, got_y = 0;
    int verdict = HUD_DebugGhost(&got_x, &got_y);
    printf("[ghost judged at %d,%d wanted %d,%d verdict %d] ",
           got_x, got_y, want_x, want_y, verdict);
    HUD_ClearCommandMode();
    shutdown_all(&platform);
    ASSERT(verdict >= 0);
    ASSERT_EQ_INT(want_x, got_x);
    ASSERT_EQ_INT(want_y, got_y);
}

/* ── an artist's model ────────────────────────────────────────────── */

#ifdef _WIN32
#  include <direct.h>
#  define probe_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define probe_mkdir(p) mkdir(p, 0755)
#endif

/* Copies one file. Returns 0 on success. */
static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return -1;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    char buf[8192];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) {
        if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    }
    fclose(a);
    fclose(b);
    return ok ? 0 : -1;
}

/* One triangle in a .glb, written where a data dir of ours will find
 * it. Returns 0 on success. */
static int write_probe_glb(const char *path) {
    static const char json[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"name\":\"spire\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]}";
    static const float pos[9] = { 0,0,0, 32,0,0, 0,48,0 };
    static const uint16_t idx[3] = { 0, 1, 2 };
    uint8_t bin[48];
    memcpy(bin, pos, 36);
    memcpy(bin + 36, idx, 6);
    memset(bin + 42, 0, 6);

    const uint32_t jlen = (uint32_t)((strlen(json) + 3u) & ~3u);
    const uint32_t blen = 48;
    const uint32_t total = 12 + 8 + jlen + 8 + blen;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t head[3] = { 0x46546C67u, 2u, total };
    uint32_t jh[2] = { jlen, 0x4E4F534Au };
    uint32_t bh[2] = { blen, 0x004E4942u };
    char padded[1024];
    memset(padded, ' ', sizeof(padded));
    memcpy(padded, json, strlen(json));
    int ok = fwrite(head, 4, 3, f) == 3 &&
             fwrite(jh, 4, 2, f) == 2 &&
             fwrite(padded, 1, jlen, f) == jlen &&
             fwrite(bh, 4, 2, f) == 2 &&
             fwrite(bin, 1, blen, f) == blen;
    fclose(f);
    return ok ? 0 : -1;
}

TEST(the_3d_view_takes_an_artists_model_over_the_shipped_one) {
    /* A data dir of the test's own, beside the binary. */
    probe_mkdir("gltf_probe");
    probe_mkdir("gltf_probe/models3d");
    if (write_probe_glb("gltf_probe/models3d/aralode.glb") != 0) {
        SKIP("cannot write beside the binary");
    }
    if (VFS_IsInitialized()) VFS_Shutdown();
    if (VFS_Init(TAK_GAME_DIR, "gltf_probe") != 0) {
        remove("gltf_probe/models3d/aralode.glb");
        SKIP("no game dir");
    }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) {
        VFS_Shutdown();
        remove("gltf_probe/models3d/aralode.glb");
        return;
    }
    if (GL3D_Init(platform.window, platform.renderer) != 0) {
        SKIP_MARK("no GL context");
        teardown_platform(&platform);
        VFS_Shutdown();
        remove("gltf_probe/models3d/aralode.glb");
        return;
    }

    /* The lodestone has a shipped model, and this one wins. */
    const GpuModel *m = ModelStore_Get("ARALODE", 0);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, (int)m->from_gltf);
    ASSERT_EQ_INT(1, m->mesh->node_count);
    ASSERT(strcmp(m->mesh->nodes[0].name, "spire") == 0);
    ASSERT_EQ_INT(3, m->mesh->vert_count);
    ASSERT(m->height_px > 0.0f);
    printf("(%d pieces, %d verts, %.0f px tall) ",
           m->mesh->node_count, m->mesh->vert_count, m->height_px);

    /* Asked twice, built once. */
    ASSERT(ModelStore_Get("ARALODE", 0) == m);

    /* A name with neither an artist's model nor a shipped one is
     * nothing, rather than a crash or a stand in. */
    ASSERT(ModelStore_Get("no_such_object_at_all", 0) == NULL);

    /* A name that would leave the folder never reaches the disk. */
    ASSERT(ModelStore_Get("../../aralode", 0) == NULL);

    ModelStore_Clear();
    ModelStore_Clear();
    GL3D_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
    remove("gltf_probe/models3d/aralode.glb");
}

/* The standing stones around a mana site are sprites, with no object
 * name to find a model by. An artist's model named after the sprite's
 * sequence stands where the picture would have lain. The world here
 * is loaded from the archives alone, with a data folder of the test's
 * own holding the model. */
TEST(the_3d_view_stands_an_artists_model_where_a_sprite_feature_lies) {
    probe_mkdir("feat_probe");
    probe_mkdir("feat_probe/models3d");
    TAK_Platform platform;
    GameWorld *world = NULL;
    int rc = boot_with_data(&platform, &world, "feat_probe");
    if (rc == 1) return;
    ASSERT_EQ_INT(0, rc);
    Timer timer;
    Timer_Init(&timer);

    /* The sprite feature nearest the player's first unit that is out
     * from under the fog, since a fogged feature is not drawn at all. */
    int n_units = 0;
    const Unit *units = Units_GetActive(&n_units);
    ASSERT(n_units > 0);
    const FeatureDef *fd = NULL;
    int32_t wx = 0, wy = 0;
    long long best = -1;
    for (int i = 0; i < world->feature_count; i++) {
        const struct MapFeature *mf = &world->features[i];
        const FeatureDef *cand = Features_GetByIndex(mf->global_idx);
        if (!cand || cand->object[0] || !cand->seqname[0]) continue;
        int fp_x = cand->footprint_x > 0 ? cand->footprint_x : 1;
        int fp_z = cand->footprint_z > 0 ? cand->footprint_z : 1;
        int32_t cx = mf->tile_x * 16 + fp_x * 8;
        int32_t cy = mf->tile_z * 16 + fp_z * 8;
        if (Fog_StateAt(world, cx, cy) == TAK_FOG_UNEXPLORED) continue;
        long long dx = cx - units[0].world_x, dy = cy - units[0].world_y;
        long long d = dx * dx + dy * dy;
        if (best < 0 || d < best) { best = d; fd = cand; wx = cx; wy = cy; }
    }
    if (!fd) {
        SKIP_MARK("no sprite feature on the map");
        shutdown_all(&platform);
        return;
    }
    char path[128];
    size_t n = 0;
    strcpy(path, "feat_probe/models3d/");
    n = strlen(path);
    for (const char *p = fd->seqname; *p && n + 5 < sizeof(path); p++)
        path[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    path[n] = '\0';
    strcat(path, ".glb");

    world->cam_x = wx - world->viewport_w / 2;
    world->cam_y = wy - world->viewport_h / 2;
    if (world->cam_x < 0) world->cam_x = 0;
    if (world->cam_y < 0) world->cam_y = 0;
    if (InGame_SetView3D(1) != 1) {
        SKIP_MARK("no GL context");
        shutdown_all(&platform);
        return;
    }

    /* As a sprite, then as a model, from the same camera. */
    ASSERT(frame(&platform, &timer));
    View3DDrawCounts as_sprite = View3D_DebugDrawCounts();
    uint32_t *a = (uint32_t *)malloc((size_t)WIN_W * WIN_H * 4);
    uint32_t *b = (uint32_t *)malloc((size_t)WIN_W * WIN_H * 4);
    ASSERT(a && b);
    ASSERT(capture(&platform, a));

    ASSERT_EQ_INT(0, write_probe_glb(path));
    ModelStore_Clear();
    ASSERT(frame(&platform, &timer));
    View3DDrawCounts as_model = View3D_DebugDrawCounts();
    ASSERT(capture(&platform, b));

    /* The frame built it, not this test: asking now adds nothing. */
    int built = ModelStore_Count();
    const GpuModel *m = ModelStore_GetArtists(fd->seqname);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, (int)m->from_gltf);
    ASSERT_EQ_INT(built, ModelStore_Count());
    /* Still drawn as a feature. The count may differ from the sprite
     * pass, since a model's cull sphere is not a picture's. */
    ASSERT(as_sprite.features >= 1);
    ASSERT(as_model.features >= 1);
    int differ = differing_pixels(a, b);
    printf("(%s as %s, %d pixels changed) ", fd->name, path, differ);
    ASSERT(differ > 0);

    /* A miss is remembered, so a name with no model is asked once. */
    ASSERT(ModelStore_GetArtists("NoSuchStone99") == NULL);
    ASSERT_EQ_INT(built, ModelStore_Count());

    free(a);
    free(b);
    remove(path);
    shutdown_all(&platform);
}

/* A release binary is built with no data folder, so the only place it
 * can find a model is the folder the game is in. Both folders here are
 * the test's own, and the game one holds no archives at all, which is
 * the same shape as an install with a models3d folder added to it. */
TEST(a_model_in_the_game_folder_is_found_without_a_data_folder) {
    probe_mkdir("gltf_game");
    probe_mkdir("gltf_game/models3d");
    if (write_probe_glb("gltf_game/models3d/aralode.glb") != 0) {
        SKIP("cannot write beside the binary");
    }
    /* A folder with no archives at all is not an install, and the
     * engine says so. The smallest of the shipped ones stands in for
     * the hundreds of megabytes a real one holds. */
    if (copy_file(TAK_GAME_DIR "/boneyards2.hpi", "gltf_game/boneyards2.hpi") != 0) {
        remove("gltf_game/models3d/aralode.glb");
        SKIP("no game dir to take an archive from");
    }
    if (VFS_IsInitialized()) VFS_Shutdown();
    /* No data folder, exactly as a release is built. */
    if (VFS_Init("gltf_game", NULL) != 0) {
        remove("gltf_game/models3d/aralode.glb");
        remove("gltf_game/boneyards2.hpi");
        SKIP("the test's own game folder would not mount");
    }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) {
        VFS_Shutdown();
        remove("gltf_game/models3d/aralode.glb");
        remove("gltf_game/boneyards2.hpi");
        return;
    }
    if (GL3D_Init(platform.window, platform.renderer) != 0) {
        SKIP_MARK("no GL context");
        teardown_platform(&platform);
        VFS_Shutdown();
        remove("gltf_game/models3d/aralode.glb");
        remove("gltf_game/boneyards2.hpi");
        return;
    }

    const GpuModel *m = ModelStore_Get("ARALODE", 0);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, (int)m->from_gltf);
    ASSERT(strcmp(m->mesh->nodes[0].name, "spire") == 0);
    printf("(read from the game folder) ");

    /* And nothing is invented for a name with no model anywhere. */
    ASSERT(ModelStore_Get("no_such_object_at_all", 0) == NULL);

    ModelStore_Clear();
    GL3D_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
    remove("gltf_game/models3d/aralode.glb");
    remove("gltf_game/boneyards2.hpi");
}

/* An argument runs only the cases whose name contains it. */
#define RUN_NAMED(name) do { \
        if (argc < 2 || strstr(#name, argv[1])) RUN(name); \
    } while (0)

int main(int argc, char **argv) {
    TAK_Crash_Install();
    printf("test_view3d\n");
    TEST_ALLOW_SKIPS("no game data or no opengl renderer on this machine");
    RUN_NAMED(toggling_3d_on_and_off_leaves_the_classic_frame_byte_identical);
    RUN_NAMED(the_3d_pointer_lands_where_the_camera_looks);
    RUN_NAMED(a_scroll_in_3d_moves_the_classic_camera_with_it);
    RUN_NAMED(the_accessors_hand_out_the_baked_model_and_its_pose);
    RUN_NAMED(a_walking_units_pieces_move_in_the_3d_pose);
    RUN_NAMED(the_build_ghost_in_3d_is_judged_where_the_pointer_lands);
    RUN_NAMED(a_projectile_in_flight_is_drawn_in_the_3d_view);
    RUN_NAMED(an_impact_effect_is_drawn_in_the_3d_view);
    RUN_NAMED(a_flame_weapon_streams_particles_instead_of_a_ray);
    RUN_NAMED(a_ring_spell_lays_its_rings_from_the_data);
    RUN_NAMED(a_storm_rains_its_drops_from_the_data);
    RUN_NAMED(the_3d_view_takes_an_artists_model_over_the_shipped_one);
    RUN_NAMED(the_3d_view_stands_an_artists_model_where_a_sprite_feature_lies);
    RUN_NAMED(a_model_in_the_game_folder_is_found_without_a_data_folder);
    TEST_REPORT();
}
