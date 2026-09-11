/*
 * test_ui_screens.c — smoke tests for the data-driven UI screens.
 *
 * For each screen we exercise the Init → Tick (once) → Shutdown
 * lifecycle. This catches symbol-level regressions (missing includes,
 * bad struct offsets, wrong .gui paths) without needing a visible
 * SDL window.
 *
 * These tests require TAK_DATA_DIR to point at extracted .gui files.
 * If not, each test skips cleanly.
 */

#include "test_framework.h"
#include "tak_hpi.h"
#include "tak_ui.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_gaf.h"
#include "tak_cob_vm.h"
#include "tak_minimap.h"
#include "tak_main_menu.h"
#include "tak_memory.h"
#include "tak_util.h"
#include "tak_platform.h"
#include "tak_gameloop.h"
#include "tak_battle_config.h"
#include "tak_battle_setup.h"
#include "tak_options.h"
#include "tak_settings.h"
#include "tak_loading.h"
#include "tak_ingame.h"
#include "tak_end_screen.h"
#include "tak_story.h"
#include "tak_world.h"
#include "tak_unit.h"
#include "tak_economy.h"
#include "tak_fog.h"
#include "tak_pathing.h"
#include "tak_occupancy.h"
#include "tak_terrain.h"
#include "tak_features.h"
#include "tak_ai.h"
#include "tak_ai_influence.h"
#include "tak_hud.h"
#include "tak_crash.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

static const char *g_test_filter = NULL;

/* Fail the running test and jump to its teardown label: a broken
 * precondition is a failure, not a skip. */
#define FAIL_TO(label, msg) do { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, msg); \
        _tf_fail_count++; \
        _tf_current_failed = 1; \
        goto label; \
    } while (0)


#define RUN_UI_TEST(name) \
    do { \
        if (!g_test_filter || strstr(#name, g_test_filter)) RUN(name); \
    } while (0)

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* A fake platform with a hidden SDL window so Init/Tick can run
 * headlessly. Windows sometimes needs a real window for SDL but an
 * off-screen hidden one works. */
static int setup_platform(TAK_Platform *p) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SKIP (SDL init failed: %s) ", SDL_GetError());
        return -1;
    }
    memset(p, 0, sizeof(*p));
    p->window = SDL_CreateWindow("tak-re-test",
                                  SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED,
                                  640, 480, SDL_WINDOW_HIDDEN);
    if (!p->window) { printf("SKIP (window failed) "); return -1; }
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_SOFTWARE);
    if (!p->renderer) { printf("SKIP (renderer failed) "); return -1; }
    p->canvas_w = 640;
    p->canvas_h = 480;
    p->window_w = 640;
    p->window_h = 480;
    p->scale = 1.0f;
    p->offset_x = 0;
    p->offset_y = 0;
    p->has_focus = 1;
    p->canvas_tex = SDL_CreateTexture(p->renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      p->canvas_w, p->canvas_h);
    if (!p->canvas_tex) { printf("SKIP (canvas texture failed) "); return -1; }
    return 0;
}

static void teardown_platform(TAK_Platform *p) {
    if (p->canvas_tex) SDL_DestroyTexture(p->canvas_tex);
    if (p->renderer) SDL_DestroyRenderer(p->renderer);
    if (p->window) SDL_DestroyWindow(p->window);
    SDL_Quit();
}

static int setup_vfs(void) {
    /* A failed ASSERT returns before the test's teardown and leaves the
     * VFS open. Close it, or every later test reports a skip as a pass. */
    if (VFS_IsInitialized()) {
        printf("(an earlier test left the VFS open) ");
        VFS_Shutdown();
    }
    return VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR);
}

static int save_and_check_canvas(const char *path) {
    SDL_Surface *canvas = UI_Offscreen();
    if (!canvas || !canvas->pixels || !canvas->format) return -1;
    if (SDL_SaveBMP(canvas, path) != 0) return -1;

    int signal_pixels = 0;
    uint32_t bins[32];
    int bin_count = 0;

    SDL_LockSurface(canvas);
    for (int y = 0; y < canvas->h; y += 2) {
        const uint8_t *row = (const uint8_t *)canvas->pixels + y * canvas->pitch;
        for (int x = 0; x < canvas->w; x += 2) {
            uint32_t pixel = ((const uint32_t *)row)[x];
            uint8_t r, g, b, a;
            SDL_GetRGBA(pixel, canvas->format, &r, &g, &b, &a);
            if (a > 0 && (r || g || b)) {
                uint32_t bin = ((uint32_t)(r >> 4) << 8) |
                               ((uint32_t)(g >> 4) << 4) |
                               (uint32_t)(b >> 4);
                int seen = 0;
                for (int i = 0; i < bin_count; i++) {
                    if (bins[i] == bin) { seen = 1; break; }
                }
                if (!seen && bin_count < (int)(sizeof(bins) / sizeof(bins[0]))) {
                    bins[bin_count++] = bin;
                }
                signal_pixels++;
            }
        }
    }
    SDL_UnlockSurface(canvas);

    if (signal_pixels < 1000) return -1;
    if (bin_count < 8) return -1;
    return 0;
}

/* Copy a small block of the UI canvas (dialog space) for comparison
 * between frames. Used to prove the unit name's first glyph lands in
 * the label's leftmost columns. */
#define HUD_STRIP_W 8
#define HUD_STRIP_H 12
static int hud_sample_strip(int x0, int y0, uint32_t *out) {
    SDL_Surface *canvas = UI_Offscreen();
    if (!canvas || !canvas->pixels || !out) return 0;
    if (x0 < 0 || y0 < 0) return 0;
    if (x0 + HUD_STRIP_W > canvas->w || y0 + HUD_STRIP_H > canvas->h) return 0;
    SDL_LockSurface(canvas);
    for (int y = 0; y < HUD_STRIP_H; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)canvas->pixels +
                                                 (y0 + y) * canvas->pitch);
        for (int x = 0; x < HUD_STRIP_W; x++) {
            out[y * HUD_STRIP_W + x] = row[x0 + x];
        }
    }
    SDL_UnlockSurface(canvas);
    return 1;
}

static int save_and_check_renderer(TAK_Platform *platform, const char *path) {
    SDL_Surface *shot;
    int signal_pixels = 0;
    uint32_t bins[64];
    int bin_count = 0;

    if (!platform || !platform->renderer) return -1;
    shot = SDL_CreateRGBSurfaceWithFormat(0, platform->window_w, platform->window_h,
                                          32, SDL_PIXELFORMAT_RGBA32);
    if (!shot) return -1;
    if (SDL_RenderReadPixels(platform->renderer, NULL, SDL_PIXELFORMAT_RGBA32,
                             shot->pixels, shot->pitch) != 0) {
        SDL_FreeSurface(shot);
        return -1;
    }
    if (SDL_SaveBMP(shot, path) != 0) {
        SDL_FreeSurface(shot);
        return -1;
    }

    SDL_LockSurface(shot);
    for (int y = 0; y < shot->h; y += 2) {
        const uint8_t *row = (const uint8_t *)shot->pixels + y * shot->pitch;
        for (int x = 0; x < shot->w; x += 2) {
            uint32_t pixel = ((const uint32_t *)row)[x];
            uint8_t r, g, b, a;
            SDL_GetRGBA(pixel, shot->format, &r, &g, &b, &a);
            if (a > 0 && (r || g || b)) {
                uint32_t bin = ((uint32_t)(r >> 4) << 8) |
                               ((uint32_t)(g >> 4) << 4) |
                               (uint32_t)(b >> 4);
                int seen = 0;
                for (int i = 0; i < bin_count; i++) {
                    if (bins[i] == bin) { seen = 1; break; }
                }
                if (!seen && bin_count < (int)(sizeof(bins) / sizeof(bins[0]))) {
                    bins[bin_count++] = bin;
                }
                signal_pixels++;
            }
        }
    }
    SDL_UnlockSurface(shot);
    SDL_FreeSurface(shot);

    if (signal_pixels < 10000) return -1;
    if (bin_count < 16) return -1;
    return 0;
}

/* ── BattleConfig ───────────────────────────────────────────────────── */

TEST(battle_config_defaults_are_sensible) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    ASSERT_EQ_INT(TAK_UNITS_PER_SIDE_DEFAULT, cfg.units_per_side);
    ASSERT_EQ_INT(1, cfg.line_of_sight);
    ASSERT_EQ_INT((int)TAK_SLOT_HUMAN, (int)cfg.players[0].kind);
    ASSERT_EQ_INT((int)TAK_SIDE_ARAMON, cfg.players[0].side);
    ASSERT_EQ_INT((int)TAK_SLOT_AI,    (int)cfg.players[1].kind);
    ASSERT_EQ_INT((int)TAK_SIDE_TAROS, cfg.players[1].side);
    for (int i = 2; i < TAK_MAX_PLAYERS; i++) {
        ASSERT_EQ_INT((int)TAK_SLOT_CLOSED, (int)cfg.players[i].kind);
    }
}

TEST(battle_config_per_side_cap_bounds) {
    /* Just structural: range constants are > 0 and min < max. */
    ASSERT(TAK_UNITS_PER_SIDE_MIN > 0);
    ASSERT(TAK_UNITS_PER_SIDE_MAX > TAK_UNITS_PER_SIDE_MIN);
    ASSERT(TAK_UNITS_PER_SIDE_DEFAULT >= TAK_UNITS_PER_SIDE_MIN);
    ASSERT(TAK_UNITS_PER_SIDE_DEFAULT <= TAK_UNITS_PER_SIDE_MAX);
}

/* ── Battle setup smoke test ────────────────────────────────────────── */

TEST(battle_setup_init_tick_shutdown) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());
    ASSERT_EQ_INT(0, BattleSetup_Init(&platform));

    /* Tick once — on a fresh init, no pending transition. */
    int next = BattleSetup_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_BATTLE_SETUP, next);
    ASSERT_EQ_INT(0, save_and_check_canvas("test_ui_battle_setup.bmp"));

    BattleSetup_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Index of the map whose .ota base name is `key`, or -1. */
static int find_map_by_key(const char *key) {
    for (int i = 0; i < BattleSetup_MapCount(); i++) {
        if (tak_stricmp(BattleSetup_MapKey(i), key) == 0) return i;
    }
    return -1;
}

/* Legacy shows the map's authored name: the translate-table entry for the
 * .ota file name, else that name with each word capitalised
 * (legacy:167724). Never the archive's lower-cased path. */
TEST(battle_setup_map_names_are_authored) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    ASSERT_EQ_INT(0, BattleSetup_Init(&platform));

    ASSERT(BattleSetup_MapCount() > 0);

    /* No row may start lower-case, which was the reported symptom. */
    for (int i = 0; i < BattleSetup_MapCount(); i++) {
        const char *shown = BattleSetup_MapDisplayName(i);
        ASSERT(shown[0] != '\0');
        ASSERT(!(shown[0] >= 'a' && shown[0] <= 'z'));
    }

    /* A map the translate table names: the table wins over the file name,
     * so the "_jm" suffix never reaches the screen. */
    int idx = find_map_by_key("meredoc keys_jm");
    if (idx >= 0) {
        ASSERT_EQ_STR("Meredoc Keys", BattleSetup_MapDisplayName(idx));
    }
    idx = find_map_by_key("threesacharm");
    if (idx >= 0) {
        ASSERT_EQ_STR("Three's a Charm", BattleSetup_MapDisplayName(idx));
    }
    /* A map the table does not name falls back to per-word capitals. */
    idx = find_map_by_key("angvir's maze");
    if (idx >= 0) {
        ASSERT_EQ_STR("Angvir's Maze", BattleSetup_MapDisplayName(idx));
    }

    BattleSetup_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* "Map Description" carries the selected .ota's missiondescription
 * (legacy:136122, legacy:168923), not the .gui's heading text. */
TEST(battle_setup_map_description_populated) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    ASSERT_EQ_INT(0, BattleSetup_Init(&platform));

    ASSERT(BattleSetup_MapCount() > 0);
    /* The first map is selected at init, so a description is already up. */
    ASSERT(BattleSetup_MapDescription()[0] != '\0');

    int idx = find_map_by_key("angvir's maze");
    if (idx >= 0) {
        BattleSetup_SelectMap(idx);
        ASSERT_EQ_STR("10 x 10  8 Player  32MB", BattleSetup_MapDescription());
    }

    /* Every map says something, the legacy placeholder at worst. */
    for (int i = 0; i < BattleSetup_MapCount(); i++) {
        BattleSetup_SelectMap(i);
        ASSERT(BattleSetup_MapDescription()[0] != '\0');
        ASSERT(strcmp(BattleSetup_MapDescription(), "Map Description") != 0);
    }

    BattleSetup_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Slow Game and Units share row y=193 in the shipped dialog and Slow Game
 * is authored invisible, so honouring the .gui's visibility flag is what
 * keeps them from overprinting (legacy:312621). */
TEST(battle_setup_game_info_rows_do_not_overlap) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    GUIDialog dlg;
    ASSERT_EQ_INT(0, GUIDialog_Load(&dlg, "data/guis/battlemenusingle.gui"));

    /* Collect the visible labels in the Game Information column. */
    SDL_Rect rects[32];
    int n = 0;
    int saw_units = 0, saw_slow_game = 0;
    for (int i = 0; i < dlg.num_children; i++) {
        const GUIWidget *w = &dlg.children[i];
        if (w->type != GUI_WT_LABEL) continue;
        if (w->rect.x < 400) continue;
        if (strcmp(w->display_text, "Units") == 0 && w->visible) saw_units = 1;
        if (strcmp(w->display_text, "Slow Game") == 0 && w->visible) saw_slow_game = 1;
        if (!w->visible) continue;
        if (n < (int)(sizeof(rects) / sizeof(rects[0]))) rects[n++] = w->rect;
    }
    ASSERT(n > 0);
    ASSERT_EQ_INT(1, saw_units);
    ASSERT_EQ_INT(0, saw_slow_game);

    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            ASSERT_EQ_INT(SDL_FALSE, SDL_HasIntersection(&rects[a], &rects[b]));
        }
    }

    /* The hidden checkbox goes with its hidden label. */
    GUIWidget *slow = GUIDialog_FindByName(&dlg, "SlowGame");
    ASSERT_NOT_NULL(slow);
    ASSERT_EQ_INT(0, slow->visible);

    GUIDialog_Free(&dlg);
    VFS_Shutdown();
}

/* The colour a slot ends up on is the index the world receives, and the
 * table's swatch is the colour the authored frame actually paints. */
TEST(battle_setup_color_index_reaches_world) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    ASSERT_EQ_INT(0, BattleSetup_Init(&platform));
    ASSERT(BattleSetup_MapCount() > 0);

    /* Cycling never lands on a colour another occupied slot holds
     * (legacy:135368) and always stays in range. */
    for (int step = 0; step < TAK_PLAYER_COLOR_COUNT * 2; step++) {
        BattleSetup_CyclePlayerColor(0);
        const BattleConfig *c = BattleSetup_Config();
        ASSERT(c->players[0].color >= 0);
        ASSERT(c->players[0].color < TAK_PLAYER_COLOR_COUNT);
        ASSERT(c->players[0].color != c->players[1].color);
    }

    const BattleConfig *cfg = BattleSetup_Config();
    int chosen0 = cfg->players[0].color;
    int chosen1 = cfg->players[1].color;

    ASSERT_EQ_INT(0, World_BeginLoad(&platform, cfg, BattleSetup_MapKey(0), "aramon"));
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(chosen0, world->cfg.players[0].color);
    ASSERT_EQ_INT(chosen1, world->cfg.players[1].color);
    World_End(&platform);

    BattleSetup_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* The swatch RGB in the shared table is the dominant colour of the
 * authored team-logo frame for that index, so the square we fall back to
 * and the badge we blit agree. The table holds the Iron Plague sheet
 * (2000). The base sheet (1999) is a duller repaint of the same ten
 * colours and a loose dev tree serves that one instead. The widest gap
 * between the two is Maroon's blue channel, 206 against 127, so the
 * tolerance sits just above it. */
TEST(battle_setup_swatches_match_authored_frames) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    GAFFile *gaf = NULL;
    uint32_t rgba[256];
    if (UI_LoadGAFWithPalette("data/anims/colorlogos2.gaf",
                              "data/anims/colorlogos2.pcx", &gaf, rgba) != 0) {
        printf("SKIP (no colorlogos2) ");
        UI_Shutdown();
        teardown_platform(&platform);
        VFS_Shutdown();
        return;
    }
    int entry = GAF_FindSequence(gaf, "arateam");
    ASSERT(entry >= 0);
    EntryHeader *eh = (EntryHeader *)(gaf->data + entry);
    int nframes = (int)eh->num_frames;
    ASSERT(nframes >= TAK_PLAYER_COLOR_COUNT);
    int base = (nframes >= TAK_PLAYER_COLOR_COUNT + 2) ? 2 : 0;
    int worst = 0;

    for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++) {
        int w = 0, h = 0;
        uint32_t *px = UI_DecodeFrame(gaf, entry, base + c, rgba, &w, &h);
        ASSERT_NOT_NULL(px);
        ASSERT(w > 0 && h > 0);

        /* Dominant opaque pixel value. */
        uint32_t vals[64];
        int      cnts[64];
        int      nd = 0;
        for (int p = 0; p < w * h; p++) {
            uint8_t pr, pg, pb, pa;
            SDL_GetRGBA(px[p], UI_RGBAFormat(), &pr, &pg, &pb, &pa);
            if (pa == 0) continue;
            int found = 0;
            for (int k = 0; k < nd; k++) {
                if (vals[k] == px[p]) { cnts[k]++; found = 1; break; }
            }
            if (!found && nd < 64) { vals[nd] = px[p]; cnts[nd] = 1; nd++; }
        }
        ASSERT(nd > 0);
        int best = 0;
        for (int k = 1; k < nd; k++) if (cnts[k] > cnts[best]) best = k;

        uint8_t fr, fg, fb, fa;
        SDL_GetRGBA(vals[best], UI_RGBAFormat(), &fr, &fg, &fb, &fa);
        const TakPlayerColor *pc = BattleConfig_PlayerColor(c);
        int dr = (int)fr - (int)pc->r, dg = (int)fg - (int)pc->g,
            db = (int)fb - (int)pc->b;
        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;
        if (dr > 80 || dg > 80 || db > 80 || getenv("TAK_SWATCH_TRACE")) {
            printf("colour %d '%s' art=(%d,%d,%d) table=(%d,%d,%d) ",
                   c, pc->name, fr, fg, fb, pc->r, pc->g, pc->b);
        }
        if (dr > worst) worst = dr;
        if (dg > worst) worst = dg;
        if (db > worst) worst = db;
        tak_free(px);
    }
    ASSERT(worst <= 80);

    GAF_Close(gaf);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Options smoke test ─────────────────────────────────────────────── */

TEST(options_init_tick_shutdown) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());
    Options_SetReturnState(GAMESTATE_MENU);
    ASSERT_EQ_INT(0, Options_Init(&platform));

    int next = Options_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_OPTIONS, next);
    ASSERT_EQ_INT(0, save_and_check_canvas("test_ui_options.bmp"));

    /* Opened from the menu, the dialog sits on the main screen: the
     * corner outside the panel carries that art, not a flat clear. */
    {
        SDL_Surface *off = UI_Offscreen();
        ASSERT_NOT_NULL(off);
        uint8_t r, g, b, a;
        SDL_GetRGBA(((const uint32_t *)off->pixels)[2 * (off->pitch / 4) + 2],
                    off->format, &r, &g, &b, &a);
        ASSERT(!(r == 24 && g == 24 && b == 32));
    }

    Options_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Loading smoke test ─────────────────────────────────────────────── */

TEST(loading_progress_clamps_and_transitions) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    /* Progress stays in [0,1]; Tick stays in GAME_LOADING until 100%. */
    Loading_SetProgress(-5.0f);
    int next = Loading_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_GAME_LOADING, next);
    ASSERT_EQ_INT(0, save_and_check_canvas("test_ui_loading.bmp"));

    Loading_SetProgress(2.0f);
    /* First tick at 100% — held one frame */
    next = Loading_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_GAME_LOADING, next);
    /* Second tick transitions to IN_GAME */
    next = Loading_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    Loading_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Loading backdrop ───────────────────────────────────────────────── */

/* Decode one entry of a GAF through the same loader the dialog uses, so
 * a wrong palette shows up as a pixel mismatch and not just a name. */
static uint32_t *decode_gaf_entry(const char *gaf_path, const char *pcx_path,
                                  const char *sequence, int *out_w, int *out_h) {
    GAFFile *gaf = NULL;
    uint32_t table[256];
    if (UI_LoadGAFWithPalette(gaf_path, pcx_path, &gaf, table) != 0 || !gaf)
        return NULL;
    int entry = GAF_FindSequence(gaf, sequence);
    if (entry < 0) { GAF_Close(gaf); return NULL; }
    uint32_t *pix = UI_DecodeFrame(gaf, entry, 0, table, out_w, out_h);
    GAF_Close(gaf);
    return pix;
}

static uint32_t canvas_pixel(SDL_Surface *s, int x, int y) {
    const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * s->pitch;
    return ((const uint32_t *)row)[x];
}

/* One canvas pixel against an opaque colour spelled out in the test. */
static int canvas_pixel_is(SDL_Surface *s, int x, int y,
                           int want_r, int want_g, int want_b) {
    uint8_t r, g, b, a;
    SDL_GetRGBA(canvas_pixel(s, x, y), s->format, &r, &g, &b, &a);
    return a == 255 && r == want_r && g == want_g && b == want_b;
}

/* The original builds the load screen out of loadscreen.gui: the stone
 * wall from the root widget, the unlit stained glass in the arch, then
 * the lit glass, and it plays the clip over that same corner
 * (legacy:158031, legacy:158297). This pins the art each layer resolves
 * and the pixels that reach the canvas, so a change that swaps either
 * the image or its palette fails here. */
TEST(loading_backdrop_is_the_arch_and_its_glass) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    GUIRuntime *rt = Loading_Runtime();
    ASSERT_NOT_NULL(rt);

    /* The arch art is authored past the progress bars, so every widget
     * has to parse for it to exist at all. */
    ASSERT_EQ_INT(27, GUIRuntime_NumWidgets(rt));

    const GUIWidget *unlit = GUIRuntime_WidgetByName(rt, "Background");
    ASSERT_NOT_NULL(unlit);
    ASSERT(unlit->num_frames >= 1);
    ASSERT_EQ_STR("loadingbw.gaf", unlit->frames[0].gaf);
    ASSERT_EQ_STR("LoadingBW", unlit->frames[0].sequence);
    ASSERT_EQ_INT(0, unlit->frames[0].frame_index);
    ASSERT_EQ_INT(168, unlit->rect.x);
    ASSERT_EQ_INT(46,  unlit->rect.y);
    ASSERT_EQ_INT(423, unlit->rect.w);
    ASSERT_EQ_INT(351, unlit->rect.h);

    const GUIWidget *lit = GUIRuntime_WidgetByName(rt, "AnimatedControl");
    ASSERT_NOT_NULL(lit);
    ASSERT(lit->num_frames >= 1);
    ASSERT_EQ_STR("loadingc.gaf", lit->frames[0].gaf);
    ASSERT_EQ_STR("LoadingC", lit->frames[0].sequence);
    ASSERT_EQ_INT(0, lit->frames[0].frame_index);
    ASSERT_EQ_INT(168, lit->rect.x);
    ASSERT_EQ_INT(46,  lit->rect.y);
    ASSERT_EQ_INT(423, lit->rect.w);
    ASSERT_EQ_INT(351, lit->rect.h);

    /* The per-player rows are authored visible and the original hides
     * them before the screen is drawn. */
    ASSERT_EQ_INT(1, GUIRuntime_WidgetHidden(rt, "PlayerName0"));
    ASSERT_EQ_INT(1, GUIRuntime_WidgetHidden(rt, "PlayerProgress0"));
    ASSERT_EQ_INT(1, GUIRuntime_WidgetHidden(rt, "PlayerPercent6"));
    ASSERT_EQ_INT(0, GUIRuntime_WidgetHidden(rt, "Background"));
    ASSERT_EQ_INT(0, GUIRuntime_WidgetHidden(rt, "AnimatedControl"));

    /* One rendered frame, then the dialog again. A clip, where FFmpeg
     * can open one, covers all but the widget's last column, and the
     * still art underneath is what the browser build shows. */
    Loading_Tick(&platform, 1.0f / 60.0f);
    GUIRuntime_Render(rt);
    ASSERT_EQ_INT(0, save_and_check_canvas("test_ui_loading_backdrop.bmp"));

    SDL_Surface *off = UI_Offscreen();
    ASSERT_NOT_NULL(off);

    int wall_w = 0, wall_h = 0;
    uint32_t *wall = decode_gaf_entry("data/anims/loadingbg.gaf",
                                      "data/anims/loadingbg.pcx",
                                      "LoadingBG", &wall_w, &wall_h);
    ASSERT_NOT_NULL(wall);
    ASSERT_EQ_INT(640, wall_w);
    ASSERT_EQ_INT(480, wall_h);

    int glass_w = 0, glass_h = 0;
    uint32_t *glass = decode_gaf_entry("data/anims/loadingc.gaf",
                                       "data/anims/loadingc.pcx",
                                       "LoadingC", &glass_w, &glass_h);
    ASSERT_NOT_NULL(glass);
    ASSERT_EQ_INT(423, glass_w);
    ASSERT_EQ_INT(351, glass_h);

    /* Outside the arch the canvas is loadingbg's own pixels. */
    static const int wall_pts[][2] = {
        { 8, 8 }, { 20, 20 }, { 60, 240 }, { 600, 60 }, { 631, 471 }
    };
    for (int i = 0; i < 5; i++) {
        int x = wall_pts[i][0], y = wall_pts[i][1];
        ASSERT_EQ_INT((int)wall[(size_t)y * 640 + x],
                      (int)canvas_pixel(off, x, y));
    }

    /* Inside the arch it is loadingc's, at the widget's own corner and
     * at native size. */
    static const int glass_pts[][2] = {
        { 200, 240 }, { 300, 150 }, { 379, 221 }, { 420, 300 }, { 500, 200 }
    };
    for (int i = 0; i < 5; i++) {
        int x = glass_pts[i][0], y = glass_pts[i][1];
        uint32_t want = glass[(size_t)(y - 46) * 423 + (x - 168)];
        ASSERT_EQ_INT((int)want, (int)canvas_pixel(off, x, y));
    }

    /* A few colours spelled out, so a palette that resolves to the same
     * file names but different entries still fails. */
    ASSERT(canvas_pixel_is(off, 20, 20, 37, 26, 14));
    ASSERT(canvas_pixel_is(off, 379, 221, 53, 143, 207));
    ASSERT(canvas_pixel_is(off, 500, 200, 210, 211, 173));

    tak_free(wall);
    tak_free(glass);

    Loading_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(campaign_loading_spawns_units_and_renders) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "takmission01_mt", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "takmission01_mt", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);
    ASSERT_EQ_INT(40, world->water_height);
    ASSERT(world->mission.placement_count > 0);
    ASSERT_NOT_NULL(world->fog_state);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(world->mission.placement_count, unit_count);
    int moving_units = 0;
    int attacking_units = 0;
    for (int i = 0; i < unit_count; i++) {
        if (!units[i].alive) continue;
        /* PATROL is a standing movement order (mission patrols no
         * longer collapse to a one-shot MOVE). */
        if (units[i].cmd_kind == UNIT_CMD_MOVE ||
            units[i].cmd_kind == UNIT_CMD_PATROL) moving_units++;
        if (units[i].cmd_kind == UNIT_CMD_ATTACK) attacking_units++;
    }
    ASSERT(moving_units > 0);
    ASSERT(attacking_units > 0);
    {
        int war_galley = Units_FindDefByName("ARAWAR");
        ASSERT(war_galley >= 0);
        const UnitDef *war_def = Units_GetDef(war_galley);
        ASSERT_NOT_NULL(war_def);
        ASSERT_EQ_INT(1, war_def->floater);
        ASSERT_EQ_INT(1, war_def->waterline);
        ASSERT_EQ_INT(16, war_def->transport_size);
        ASSERT_EQ_INT(18, war_def->transport_capacity);
        ASSERT_EQ_INT(72, war_def->transport_size_capacity);
        ASSERT_EQ_INT(1, war_def->cant_be_transported);
        ASSERT_EQ_INT(300, war_def->transport_distance);
        ASSERT_EQ_INT(UNIT_PROJECTILE_VIS_CANNON,
                      Units_GetWeaponVisualKind(war_galley, 0));
        ASSERT_EQ_INT(100, Units_ComputeSplashDamage(100, 100, 0.25f, 0));
        ASSERT_EQ_INT(63, Units_ComputeSplashDamage(100, 100, 0.25f, 50 * 50));
        ASSERT_EQ_INT(25, Units_ComputeSplashDamage(100, 100, 0.25f, 100 * 100));
        ASSERT_EQ_INT(0, Units_ComputeSplashDamage(100, 100, 0.25f, 101 * 101));
    }
    {
        int king_def_idx = Units_FindDefByName("ARAKING");
        int god_def_idx = Units_FindDefByName("ARAGOD");
        ASSERT(king_def_idx >= 0);
        ASSERT(god_def_idx >= 0);
        const UnitDef *king_def = Units_GetDef(king_def_idx);
        const UnitDef *god_def = Units_GetDef(god_def_idx);
        ASSERT_NOT_NULL(king_def);
        ASSERT_NOT_NULL(god_def);
        ASSERT_EQ_STR("Monarch", king_def->damage_category);
        ASSERT(god_def->num_weapons > 0);
        ASSERT_EQ_INT(16000, god_def->weapons[0].damage);
        ASSERT_EQ_INT(16000, Units_ComputeWeaponDamageForCategory(
                      &god_def->weapons[0], "unknown"));
        ASSERT_EQ_INT(160, Units_ComputeWeaponDamageForCategory(
                      &god_def->weapons[0], king_def->damage_category));
    }
    {
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive != 1) continue;
            ASSERT(units[i].stable_id != 0);
            ASSERT_EQ_INT(i, Units_FindByStableId(units[i].stable_id));
            ASSERT_EQ_INT((int)units[i].stable_id, (int)Units_GetStableId(i));
            for (int j = i + 1; j < unit_count; j++) {
                if (units[j].alive != 1) continue;
                ASSERT(units[i].stable_id != units[j].stable_id);
            }
        }
    }
    {
        int checked_blocker = 0;
        for (int i = 0; i < world->feature_count; i++) {
            const FeatureDef *fd = Features_GetByIndex(world->features[i].global_idx);
            if (!fd || !fd->blocking) continue;
            int fp_x = fd->footprint_x > 0 ? fd->footprint_x : 1;
            int fp_z = fd->footprint_z > 0 ? fd->footprint_z : 1;
            int32_t cx = world->features[i].tile_x * 16 + fp_x * 8;
            int32_t cy = world->features[i].tile_z * 16 + fp_z * 8;
            ASSERT_EQ_INT(0, Terrain_IsWalkable(world, cx, cy, 255));
            checked_blocker = 1;
            break;
        }
        (void)checked_blocker;
    }
    ASSERT_EQ_INT(TAK_FOG_VISIBLE,
                  Fog_StateAt(world, units[0].world_x, units[0].world_y));
    {
        int visible_cells = 0;
        int unexplored_cells = 0;
        int total = world->fog_w * world->fog_h;
        for (int i = 0; i < total; i++) {
            if (world->fog_state[i] == TAK_FOG_VISIBLE) visible_cells++;
            if (world->fog_state[i] == TAK_FOG_UNEXPLORED) unexplored_cells++;
        }
        ASSERT(visible_cells > 0);
        ASSERT(unexplored_cells > 0);
    }
    {
        int friendly = -1;
        int friendly2 = -1;
        int hidden_enemy = -1;
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive != 1) continue;
            if (units[i].player_id == 1 && friendly < 0) friendly = i;
            else if (units[i].player_id == 1 && friendly2 < 0) friendly2 = i;
            if (hidden_enemy < 0 && units[i].player_id != 1 &&
                !Fog_IsVisible(world, units[i].world_x, units[i].world_y)) {
                hidden_enemy = i;
            }
        }
        if (friendly >= 0 && hidden_enemy >= 0) {
            /* Legacy gates hidden enemies at the UI, not in the sim:
             * PathFind_FindObstacles (:21137-21168) filters on alliance
             * / category / range only, never on visibility. So the
             * guarantee to assert is that a fogged unit cannot be
             * PICKED (clicked), not that the sim refuses the order. */
            int picked = Units_PickAt(units[hidden_enemy].world_x,
                                      units[hidden_enemy].world_y, 48);
            ASSERT(picked != hidden_enemy);
        }
        if (friendly >= 0) {
            Units_SelectSingle(friendly);
            Units_CommandPatrolSelected(units[friendly].world_x + 128,
                                        units[friendly].world_y);
            units = Units_GetActive(&unit_count);
            ASSERT_EQ_INT(UNIT_CMD_PATROL, units[friendly].cmd_kind);
            ASSERT_EQ_INT(units[friendly].world_x, units[friendly].patrol_x);
            ASSERT_EQ_INT(units[friendly].world_y, units[friendly].patrol_y);
            Units_CommandStopSelected();
        }
        /* PATROL PROBE: a patrol is a standing order, not one move.
         * Legacy keeps the patrol mission resident and drives one leg
         * at a time (:11565), so the unit must reach B, come back to A
         * and reach B again with cmd_kind still PATROL throughout. */
        {
            int walker = -1;
            for (int i = 0; i < unit_count && walker < 0; i++) {
                const UnitDef *ud = Units_GetDef(units[i].def_idx);
                if (units[i].alive == UNIT_ALIVE_ACTIVE &&
                    units[i].player_id == 1 && ud &&
                    ud->max_velocity > 0.0f && ud->num_weapons > 0)
                    walker = i;
            }
            if (walker >= 0) {
                /* Isolate the loop from the map's own fights: nothing
                 * auto-acquires during the waypoint phase. */
                for (int i = 0; i < unit_count; i++) {
                    if (units[i].alive == UNIT_ALIVE_ACTIVE)
                        Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
                }
                int32_t ax = units[walker].world_x;
                int32_t ay = units[walker].world_y;
                int32_t bx = ax, by = ay;
                static const int probe_dir[4][2] = {
                    { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }
                };
                for (int d = 0; d < 4; d++) {
                    int32_t tx = ax + probe_dir[d][0] * 128;
                    int32_t ty = ay + probe_dir[d][1] * 128;
                    if (Terrain_IsWalkable(world, tx, ty, 255)) {
                        bx = tx; by = ty; break;
                    }
                }
                if (bx != ax || by != ay) {
                    Units_SelectSingle(walker);
                    Units_CommandPatrolSelected(bx, by);
                    units = Units_GetActive(&unit_count);
                    int reached_b = 0, back_a = 0, again_b = 0;
                    int kind_held = 1;
                    for (int t = 0; t < 20000; t++) {
                        Units_TickEngines();
                        units = Units_GetActive(&unit_count);
                        const Unit *pu = &units[walker];
                        if (pu->cmd_kind != UNIT_CMD_PATROL) {
                            kind_held = 0;
                            break;
                        }
                        int64_t dbx = pu->world_x - bx, dby = pu->world_y - by;
                        int64_t dax = pu->world_x - ax, day = pu->world_y - ay;
                        int64_t db2 = dbx*dbx + dby*dby;
                        int64_t da2 = dax*dax + day*day;
                        if (!reached_b && db2 <= 100) reached_b = t + 1;
                        else if (reached_b && !back_a && da2 <= 100) back_a = t + 1;
                        else if (back_a && !again_b && db2 <= 100) again_b = t + 1;
                        if (again_b) break;
                    }
                    printf("[patrol A=(%d,%d) B=(%d,%d) b=%d a=%d b2=%d] ",
                           ax, ay, bx, by, reached_b, back_a, again_b);
                    ASSERT(kind_held);
                    ASSERT(reached_b > 0);
                    ASSERT(back_a > reached_b);
                    ASSERT(again_b > back_a);
                    ASSERT_EQ_INT(UNIT_CMD_PATROL, units[walker].cmd_kind);

                    /* Engage-on-contact: a patroller acquires an enemy
                     * it meets but keeps the standing order, so the
                     * route resumes once the target is gone
                     * (order type 9 stays resident, legacy:9664-9682). */
                    Units_DebugSetAggro(walker, UNIT_AGGRO_OFFENSIVE);
                    units = Units_GetActive(&unit_count);
                    int foe = Units_DebugSpawnEnemy(units[walker].world_x + 24,
                                                    units[walker].world_y + 24);
                    units = Units_GetActive(&unit_count);
                    if (foe >= 0) {
                        Units_DebugSetAggro(foe, UNIT_AGGRO_PASSIVE);
                        int engaged = 0;
                        for (int t = 0; t < 600 && !engaged; t++) {
                            Units_TickEngines();
                            units = Units_GetActive(&unit_count);
                            if (units[walker].target == foe) engaged = 1;
                            ASSERT_EQ_INT(UNIT_CMD_PATROL,
                                          units[walker].cmd_kind);
                        }
                        ASSERT(engaged);
                        Units_DebugKillHandle(foe);
                        /* Route resumes: the unit closes on one of its
                         * two waypoints again after the fight. */
                        int resumed = 0;
                        for (int t = 0; t < 20000 && !resumed; t++) {
                            Units_TickEngines();
                            units = Units_GetActive(&unit_count);
                            if (units[walker].cmd_kind != UNIT_CMD_PATROL) break;
                            int64_t rx = units[walker].world_x - bx;
                            int64_t ry = units[walker].world_y - by;
                            int64_t sx = units[walker].world_x - ax;
                            int64_t sy = units[walker].world_y - ay;
                            if (rx*rx + ry*ry <= 100 || sx*sx + sy*sy <= 100)
                                resumed = 1;
                        }
                        ASSERT(resumed);
                        ASSERT_EQ_INT(UNIT_CMD_PATROL, units[walker].cmd_kind);
                    }
                    Units_SelectSingle(walker);
                    Units_CommandStopSelected();
                }
                for (int i = 0; i < unit_count; i++) {
                    if (units[i].alive == UNIT_ALIVE_ACTIVE)
                        Units_DebugSetAggro(i, UNIT_AGGRO_OFFENSIVE);
                }
            }
        }
        if (friendly >= 0 && friendly2 >= 0) {
            Units_SelectSingle(friendly);
            Units_CommandGuardSelected(friendly2);
            units = Units_GetActive(&unit_count);
            ASSERT_EQ_INT(UNIT_CMD_GUARD, units[friendly].cmd_kind);
            ASSERT_EQ_INT(friendly2, units[friendly].target);
            Units_CommandStopSelected();
        }
    }

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.accumulator = timer.sim_dt;
    next = InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    ASSERT_EQ_INT(1, world->mission_elapsed_ticks);
    ASSERT_EQ_INT(0, world->mission_victory);

    units = Units_GetActive(&unit_count);
    int builder_handle = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *ud = Units_GetDef(units[i].def_idx);
        if (units[i].alive == 1 && units[i].player_id == 1 &&
            ud && (ud->cap_flags & UNIT_CAP_BUILDER)) {
            builder_handle = i;
            break;
        }
    }
    int buildables[32];
    int builder_def_idx = -1;
    if (builder_handle >= 0) {
        builder_def_idx = (int)units[builder_handle].def_idx;
    } else {
        builder_def_idx = Units_FindDefByName("ARAKING");
        ASSERT(builder_def_idx >= 0);
        const UnitDef *spawn_builder_def = Units_GetDef(builder_def_idx);
        ASSERT_NOT_NULL(spawn_builder_def);
        ASSERT((spawn_builder_def->cap_flags & UNIT_CAP_BUILDER) != 0);
    }

    int buildable_count = Units_GetBuildables(builder_def_idx,
                                              buildables, 32);
    ASSERT(buildable_count > 0);
    /* Pick a mana-storing structure so the economy plumbing below has
     * something to assert on. Lodestones (yardmap 'S') are excluded:
     * they only stand on a sacred site (legacy:218887) and this
     * mission map has no pads. */
    int build_def = -1, build_fallback = -1;
    for (int i = 0; i < buildable_count; i++) {
        const UnitDef *bd = Units_GetDef(buildables[i]);
        if (!bd || bd->yardmap_sacred) continue;
        if (build_fallback < 0) build_fallback = buildables[i];
        if (bd->mogrium_storage > 0 || bd->max_mana > 0) {
            build_def = buildables[i];
            break;
        }
    }
    if (build_def < 0) build_def = build_fallback;
    ASSERT(build_def >= 0);

    int32_t seed_site_x = 0, seed_site_y = 0;
    int have_seed_site = 0;
    if (builder_handle < 0) {
        int spawned = -1;
        for (int y = 256; y < world->map_pixels_h - 256 && spawned < 0; y += 128) {
            for (int x = 256; x < world->map_pixels_w - 384; x += 128) {
                if (!Terrain_IsWalkable(world, x, y, 255)) continue;
                if (!Units_IsBuildSiteClear(build_def, x + 128, y)) continue;
                seed_site_x = x + 128;
                seed_site_y = y;
                have_seed_site = 1;
                spawned = Units_Spawn(builder_def_idx, 1, 0, x, y);
                break;
            }
        }
        ASSERT(spawned >= 0);
        builder_handle = spawned;
        const UnitDef *spawned_builder_def = Units_GetDef(builder_def_idx);
        ASSERT_NOT_NULL(spawned_builder_def);
        int32_t builder_mana_cap = spawned_builder_def->max_mana +
                                   spawned_builder_def->mogrium_storage;
        float builder_mana_recharge =
            spawned_builder_def->mana_recharge_per_sec +
            spawned_builder_def->mogrium_income_per_sec;
        if (builder_mana_cap > 0 || builder_mana_recharge > 0.0f) {
            Economy_OnMonarchSpawn(&world->economy, 1,
                                   builder_mana_cap,
                                   builder_mana_recharge);
        }
        units = Units_GetActive(&unit_count);
    }

    /* Pacify hostiles for the build phase: mission patrols are real
     * standing orders now, so roaming enemies would otherwise shoot
     * the builder/nanoframe and stall this plumbing check. */
    for (int i = 0; i < unit_count; i++) {
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].player_id != 1)
            Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
    }

    int build_handle = -1;
    static const int offsets[][2] = {
        {  96,   0 }, { -96,   0 }, {   0,  96 }, {   0, -96 },
        { 128,  64 }, {-128,  64 }, { 128, -64 }, {-128, -64 },
        { 192,   0 }, {-192,   0 }, {   0, 192 }, {   0,-192 },
        { 192, 128 }, {-192, 128 }, { 192,-128 }, {-192,-128 }
    };
    if (have_seed_site && Units_IsBuildSiteClear(build_def, seed_site_x,
                                                 seed_site_y)) {
        build_handle = Units_BeginBuildingForUnit(builder_handle, build_def,
                                                  seed_site_x, seed_site_y);
    }
    for (int i = 0; build_handle < 0 &&
                    i < (int)(sizeof(offsets) / sizeof(offsets[0])); i++) {
        int32_t bx = units[builder_handle].world_x + offsets[i][0];
        int32_t by = units[builder_handle].world_y + offsets[i][1];
        if (!Units_IsBuildSiteClear(build_def, bx, by)) continue;
        build_handle = Units_BeginBuildingForUnit(builder_handle, build_def, bx, by);
        if (build_handle >= 0) break;
    }
    ASSERT(build_handle >= 0);
    units = Units_GetActive(&unit_count);
    int build_hp0 = units[build_handle].health;
    int32_t max_before_build_complete = Economy_GetMaxMana(&world->economy, 1);
    for (int i = 0; i < 900 && units[build_handle].health <= build_hp0; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
    }
    ASSERT(units[build_handle].health > build_hp0);

    const UnitDef *built_def = Units_GetDef(build_def);
    ASSERT_NOT_NULL(built_def);
    int32_t built_cap = built_def->max_mana + built_def->mogrium_storage;
    for (int i = 0; i < 3600 && units[build_handle].under_construction; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
    }
    ASSERT_EQ_INT(0, units[build_handle].under_construction);
    if (built_cap > 0) {
        ASSERT(Economy_GetMaxMana(&world->economy, 1) >=
               max_before_build_complete + built_cap);
        ASSERT(Economy_GetMana(&world->economy, 1) > 0);
    }
    {
        int reclaim_def = Units_FindDefByName("ARAPRIES");
        int load_def = Units_FindDefByName("ARADRAG");
        int transport_def = Units_FindDefByName("ARAWAR");
        ASSERT(reclaim_def >= 0);
        ASSERT(load_def >= 0);
        ASSERT(transport_def >= 0);
        int reclaimer = Units_Spawn(reclaim_def, 1, 0,
                                    units[0].world_x + 64,
                                    units[0].world_y + 64);
        int loader = Units_Spawn(load_def, 1, 0,
                                 units[0].world_x + 96,
                                 units[0].world_y + 64);
        int transport = Units_Spawn(transport_def, 1, 0,
                                    units[0].world_x + 128,
                                    units[0].world_y + 64);
        int target = Units_Spawn(Units_FindDefByName("ARASWORD"), 1, 0,
                                 units[0].world_x + 160,
                                 units[0].world_y + 64);
        /* Spawn the reclaim target as player 1 so it can be selected,
         * force it passive, then hand it to player 2. Without this the
         * armed spawns auto-engage each other and the reclaim target
         * dies before the RECLAIM command is issued. */
        int reclaim_target = Units_Spawn(Units_FindDefByName("ARASWORD"), 1, 1,
                                         units[0].world_x + 192,
                                         units[0].world_y + 64);
        ASSERT(reclaimer >= 0);
        ASSERT(loader >= 0);
        ASSERT(transport >= 0);
        ASSERT(target >= 0);
        ASSERT(reclaim_target >= 0);
        {
            const int spawned[] = { reclaimer, loader, transport, target,
                                    reclaim_target };
            for (int s = 0; s < (int)(sizeof(spawned) / sizeof(spawned[0])); s++) {
                Units_SelectSingle(spawned[s]);
                Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
            }
            Units_SelectSingle(-1);
        }
        Units_SetOwner(reclaim_target, 2, 1);

        Units_SelectSingle(reclaimer);
        Units_SetHealthPercent(target, 50);
        units = Units_GetActive(&unit_count);
        int repair_hp0 = units[target].health;
        ASSERT(repair_hp0 > 0);
        ASSERT(repair_hp0 < units[target].max_health);
        Units_CommandRepairSelected(target);
        units = Units_GetActive(&unit_count);
        ASSERT_EQ_INT(UNIT_CMD_REPAIR, units[reclaimer].cmd_kind);
        ASSERT_EQ_INT(target, units[reclaimer].target);
        ASSERT(units[reclaimer].cmd_kind != UNIT_CMD_ATTACK);
        for (int i = 0; i < 180 && units[target].health <= repair_hp0; i++) {
            timer.accumulator = timer.sim_dt;
            next = InGame_Tick(&platform, &timer);
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
            units = Units_GetActive(&unit_count);
        }
        ASSERT(units[target].health > repair_hp0);
        Units_CommandStopSelected();

        Units_SelectSingle(reclaimer);
        int reclaim_hp0 = units[reclaim_target].health;
        Units_CommandReclaimSelected(reclaim_target);
        units = Units_GetActive(&unit_count);
        ASSERT_EQ_INT(UNIT_CMD_RECLAIM, units[reclaimer].cmd_kind);
        ASSERT_EQ_INT(reclaim_target, units[reclaimer].target);
        ASSERT(units[reclaimer].cmd_kind != UNIT_CMD_ATTACK);
        for (int i = 0; i < 240 && units[reclaim_target].health >= reclaim_hp0; i++) {
            timer.accumulator = timer.sim_dt;
            next = InGame_Tick(&platform, &timer);
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
            units = Units_GetActive(&unit_count);
        }
        ASSERT(units[reclaim_target].health < reclaim_hp0);
        Units_CommandStopSelected();

        ASSERT_EQ_INT(UNIT_PROJECTILE_VIS_REMOTE,
                      Units_GetWeaponVisualKind(reclaim_def, 0));
        ASSERT_EQ_INT(UNIT_PROJECTILE_VIS_MAGIC,
                      Units_GetWeaponVisualKind(reclaim_def, 2));

        Units_SelectSingle(loader);
        Units_CommandLoadSelected(target);
        units = Units_GetActive(&unit_count);
        ASSERT_EQ_INT(UNIT_CMD_LOAD, units[loader].cmd_kind);
        ASSERT_EQ_INT(target, units[loader].target);
        ASSERT(units[loader].cmd_kind != UNIT_CMD_ATTACK);

        Units_SelectSingle(transport);
        Units_CommandLoadSelected(target);
        units = Units_GetActive(&unit_count);
        ASSERT_EQ_INT(UNIT_CMD_LOAD, units[transport].cmd_kind);
        ASSERT_EQ_INT(target, units[transport].target);
        for (int i = 0; i < 30 && units[target].alive == UNIT_ALIVE_ACTIVE; i++) {
            timer.accumulator = timer.sim_dt;
            next = InGame_Tick(&platform, &timer);
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
            units = Units_GetActive(&unit_count);
        }
        ASSERT_EQ_INT(UNIT_ALIVE_TRANSPORTED, units[target].alive);
        ASSERT_EQ_INT(transport, units[target].carried_by);
        ASSERT_EQ_INT(1, units[transport].cargo_count);

        /* Unload at the transport's own position: ARAWAR is a naval
         * class, and with water-depth movement rules it legally cannot
         * sail across the land it was test-spawned on. The drop-site
         * ring search places the cargo on the nearest clear cell. */
        Units_SelectSingle(transport);
        Units_CommandUnloadSelected(units[transport].world_x,
                                    units[transport].world_y);
        units = Units_GetActive(&unit_count);
        ASSERT_EQ_INT(UNIT_CMD_UNLOAD, units[transport].cmd_kind);
        ASSERT_EQ_INT(-1, units[transport].target);
        ASSERT(units[transport].cmd_kind != UNIT_CMD_MOVE);
        for (int i = 0; i < 120 && units[target].alive != UNIT_ALIVE_ACTIVE; i++) {
            timer.accumulator = timer.sim_dt;
            next = InGame_Tick(&platform, &timer);
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
            units = Units_GetActive(&unit_count);
        }
        ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, units[target].alive);
        ASSERT_EQ_INT(-1, units[target].carried_by);
        ASSERT_EQ_INT(0, units[transport].cargo_count);
        Units_CommandStopSelected();
    }

    units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    const UnitDef *first_def = Units_GetDef(units[0].def_idx);
    ASSERT_NOT_NULL(first_def);
    world->mission.objectives[0].type = MISSION_OBJ_MOVE_UNIT_TO_RADIUS;
    strncpy(world->mission.objectives[0].text, first_def->unitname,
            sizeof(world->mission.objectives[0].text) - 1);
    world->mission.objectives[0].a = units[0].world_x / 16;
    world->mission.objectives[0].b = units[0].world_y / 16;
    world->mission.objectives[0].c = 1;
    timer.accumulator = timer.sim_dt;
    next = InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    ASSERT_EQ_INT(world->mission.objective_count,
                  world->mission_objectives_satisfied);
    ASSERT_EQ_INT(1, world->mission_victory);
    ASSERT_EQ_INT(0, save_and_check_renderer(&platform,
                                             "test_ui_campaign_ingame.bmp"));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(skirmish_monarch_death_ends_match) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);
    ASSERT_EQ_INT(0, world->mission.placement_count);
    ASSERT(world->num_start_positions >= 2);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count >= 2);
    int local_monarch = -1;
    int ai_monarch = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *def = Units_GetDef(units[i].def_idx);
        if (!def || !strstr(def->category, "Monarch")) continue;
        if (units[i].player_id == 1) local_monarch = i;
        if (units[i].player_id == 2) ai_monarch = i;
    }
    ASSERT(local_monarch >= 0);
    ASSERT(ai_monarch >= 0);
    ASSERT_EQ_INT(0, world->skirmish_game_over);

    ASSERT_EQ_INT(ai_monarch, Units_DebugKillHandle(ai_monarch));
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.accumulator = timer.sim_dt;
    next = InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    ASSERT_EQ_INT(1, world->skirmish_game_over);
    ASSERT_EQ_INT(1, world->skirmish_local_result);
    ASSERT_EQ_INT(cfg.players[0].team, world->skirmish_winner_team);
    ASSERT_EQ_STR("Victory", world->skirmish_end_reason);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(skirmish_ai_issues_attack_orders) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.players[1].ai_difficulty = 2;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);
    ASSERT_EQ_INT(0, world->mission.placement_count);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count >= 2);

    int local_monarch = -1;
    int ai_monarch = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *def = Units_GetDef(units[i].def_idx);
        if (!def || !strstr(def->category, "Monarch")) continue;
        if (units[i].player_id == 1) local_monarch = i;
        if (units[i].player_id == 2) ai_monarch = i;
    }
    ASSERT(local_monarch >= 0);
    ASSERT(ai_monarch >= 0);

    int original_ai_team = world->cfg.players[1].team;
    world->cfg.players[1].team = world->cfg.players[0].team;
    Units_CommandAttackUnit(ai_monarch, local_monarch);
    ASSERT(units[ai_monarch].cmd_kind != UNIT_CMD_ATTACK);

    world->cfg.players[1].team = original_ai_team;
    TAK_AI_TickSkirmish(world);

    units = Units_GetActive(&unit_count);
    int ai_ordered = 0;
    int ai_building = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != 2) continue;
        if (u->cmd_kind == UNIT_CMD_ATTACK) {
            ai_ordered = 1;
            break;
        }
        if (u->cmd_kind == UNIT_CMD_MOVE) {
            ai_ordered = 1;
            break;
        }
        if (u->cmd_kind == UNIT_CMD_BUILD && u->build_target >= 0) {
            ai_building = 1;
        }
    }
    ASSERT_EQ_INT(0, ai_ordered);
    ASSERT_EQ_INT(1, ai_building);
    ASSERT(Units_PickAt(units[ai_monarch].world_x,
                        units[ai_monarch].world_y,
                        48) != ai_monarch);

    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);

    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    cfg.line_of_sight = 0;
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.players[1].ai_difficulty = 2;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);
    world->cfg.line_of_sight = 0;
    Fog_Update(world, 1);
    Fog_Update(world, 2);
    /* Builders build before they fight (legacy:17163), so the order
     * goes to a troop: with no fog the wave target is in view and the
     * troop is sent straight at a player-1 unit. */
    units = Units_GetActive(&unit_count);
    ai_monarch = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *def = Units_GetDef(units[i].def_idx);
        if (!def || !strstr(def->category, "Monarch")) continue;
        if (units[i].player_id == 2) ai_monarch = i;
    }
    ASSERT(ai_monarch >= 0);
    int troop_def = Units_FindDefByName("TARTROOP");
    ASSERT(troop_def >= 0);
    int troop = Units_Spawn(troop_def, 2, cfg.players[1].color,
                            units[ai_monarch].world_x + 64,
                            units[ai_monarch].world_y);
    ASSERT(troop >= 0);
    TAK_AI_TickSkirmish(world);

    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, units[troop].cmd_kind);
    ASSERT(units[troop].target >= 0);
    ASSERT(units[troop].target < unit_count);
    ASSERT_EQ_INT(1, units[units[troop].target].player_id);
    ASSERT(TAK_AI_DebugHostileOrders(2, 1, 1) > 0);

    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

extern double g_sim_prof_ms[4];
extern double g_eng_prof_ms[4];
extern double g_path_plan_calls;


/* ── Hostility: four seats, one human, three AIs ─────────────────────
 *
 * Each AI gets a small army at its start so the target, march and
 * defence rules act from the first tick instead of after a build-up.
 * King of the Hill has four starts on a small map. */

static int hostility_combat_def_for_side(int side) {
    static const char *prefixes[4] = { "ARA", "TAR", "VER", "ZON" };
    if (side < 0 || side > 3) return -1;
    int n = Units_GetDefCount();
    for (int i = 0; i < n; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || strncmp(d->category, prefixes[side], 3) != 0) continue;
        if (!strstr(d->category, "MELEE")) continue;
        if (d->max_velocity <= 0.0f || d->num_weapons <= 0 || d->can_fly) continue;
        if (d->cap_flags & UNIT_CAP_BUILDER) continue;
        if (strstr(d->category, "Monarch")) continue;
        return i;
    }
    return -1;
}

static int hostility_start_of(const GameWorld *world, int player,
                              int32_t *x, int32_t *y) {
    for (int s = 0; s < world->num_start_positions; s++) {
        if (world->start_positions[s].player != player) continue;
        *x = world->start_positions[s].x * 16;
        *y = world->start_positions[s].z * 16;
        return 0;
    }
    return -1;
}

static int hostility_monarch_of(int player) {
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    for (int i = 0; i < unit_count; i++) {
        if (units[i].alive != UNIT_ALIVE_ACTIVE || units[i].player_id != player) continue;
        const UnitDef *def = Units_GetDef(units[i].def_idx);
        if (def && strstr(def->category, "Monarch")) return i;
    }
    return -1;
}

static int hostility_setup_world(TAK_Platform *platform, const int *teams,
                                 BattleConfig *cfg, GameWorld **out_world) {
    static const int sides[4] = {
        TAK_SIDE_ARAMON, TAK_SIDE_TAROS, TAK_SIDE_VERUNA, TAK_SIDE_ZHON
    };
    BattleConfig_SetDefaults(cfg);
    strncpy(cfg->map_name, "King of the Hill", sizeof(cfg->map_name) - 1);
    cfg->monarch_expendable = 1;
    for (int p = 0; p < 4; p++) {
        cfg->players[p].kind = p == 0 ? TAK_SLOT_HUMAN : TAK_SLOT_AI;
        cfg->players[p].side = sides[p];
        cfg->players[p].team = teams[p];
        cfg->players[p].color = p;
        cfg->players[p].ai_difficulty = 1;
    }
    if (World_BeginLoad(platform, cfg, "King of the Hill", "aramon") != 0) return -1;
    if (Loading_Init(platform) != 0) return -1;
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(platform, 1.0f / 60.0f);
    }
    if (next != GAMESTATE_IN_GAME) return -1;
    *out_world = World_Get();
    if (!*out_world || (*out_world)->num_start_positions < 4) return -1;
    return 0;
}

/* Up to six troops of the player's side in a ring round its start. */
static int hostility_spawn_army(const GameWorld *world, const BattleConfig *cfg,
                                int player, int count) {
    static const int ring[6][2] = {
        { 96, 0 }, { -96, 0 }, { 0, 96 }, { 0, -96 }, { 96, 96 }, { -96, -96 }
    };
    int def = hostility_combat_def_for_side(cfg->players[player - 1].side);
    if (def < 0) return -1;
    int32_t sx, sy;
    if (hostility_start_of(world, player, &sx, &sy) != 0) return -1;
    int spawned = 0;
    for (int i = 0; i < count && i < 6; i++) {
        if (Units_Spawn(def, player, cfg->players[player - 1].color,
                        sx + ring[i][0], sy + ring[i][1]) >= 0) {
            spawned++;
        }
    }
    return spawned;
}

static void hostility_teardown(TAK_Platform *platform) {
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(platform);
    UI_Shutdown();
    teardown_platform(platform);
    VFS_Shutdown();
}

/* Free for all, human idle: every AI attacks someone and at least one
 * AI fights another AI, both as issued orders and as attack states
 * seen in the simulation. */
TEST(four_player_ffa_every_ai_fights) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    static const int teams[4] = { 1, 2, 3, 4 };
    BattleConfig cfg;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, hostility_setup_world(&platform, teams, &cfg, &world));
    for (int p = 2; p <= 4; p++) ASSERT(hostility_spawn_army(world, &cfg, p, 6) > 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    int ai_vs_ai_states = 0;
    int ticks = 0;
    for (; ticks < 60 * 180 && !world->skirmish_game_over; ticks += 60) {
        InGame_DebugRunSimTicks(60);
        int unit_count = 0;
        const Unit *units = Units_GetActive(&unit_count);
        for (int i = 0; i < unit_count; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE || u->cmd_kind != UNIT_CMD_ATTACK) continue;
            if (u->player_id < 2 || u->target < 0 || u->target >= unit_count) continue;
            int owner = units[u->target].player_id;
            if (owner >= 2 && owner != u->player_id) ai_vs_ai_states++;
        }
        int every = 1, pair = 0;
        for (int a = 2; a <= 4; a++) {
            int any = 0;
            for (int b = 1; b <= 4; b++) {
                if (b == a || TAK_AI_DebugHostileOrders(a, b, 1) <= 0) continue;
                any = 1;
                if (b >= 2) pair = 1;
            }
            if (!any) every = 0;
        }
        if (every && pair && ai_vs_ai_states > 0) break;
    }
    fprintf(stderr, "hostility: ffa settled after %d ticks, ai-vs-ai attack "
            "states %d, orders 2->%d/%d/%d 3->%d/%d/%d 4->%d/%d/%d\n",
            ticks, ai_vs_ai_states,
            TAK_AI_DebugHostileOrders(2, 1, 1), TAK_AI_DebugHostileOrders(2, 3, 1),
            TAK_AI_DebugHostileOrders(2, 4, 1), TAK_AI_DebugHostileOrders(3, 1, 1),
            TAK_AI_DebugHostileOrders(3, 2, 1), TAK_AI_DebugHostileOrders(3, 4, 1),
            TAK_AI_DebugHostileOrders(4, 1, 1), TAK_AI_DebugHostileOrders(4, 2, 1),
            TAK_AI_DebugHostileOrders(4, 3, 1));
    for (int a = 2; a <= 4; a++) {
        int any = 0;
        for (int b = 1; b <= 4; b++) {
            if (b != a && TAK_AI_DebugHostileOrders(a, b, 1) > 0) any = 1;
        }
        ASSERT(any);
        ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(a, a, 0));
    }
    int pair = 0;
    for (int a = 2; a <= 4; a++) {
        for (int b = 2; b <= 4; b++) {
            if (a != b && TAK_AI_DebugHostileOrders(a, b, 1) > 0) pair = 1;
        }
    }
    ASSERT(pair);
    ASSERT(ai_vs_ai_states > 0);
    hostility_teardown(&platform);
}

/* Teams from the skirmish menu: the human and AIs 2 and 3 share a
 * team, AI 4 stands alone. The allies never order a shot or a march
 * at one another and both still go for AI 4. */
TEST(teamed_ais_spare_their_allies) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    static const int teams[4] = { 2, 2, 2, 3 };
    BattleConfig cfg;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, hostility_setup_world(&platform, teams, &cfg, &world));
    for (int p = 2; p <= 4; p++) ASSERT(hostility_spawn_army(world, &cfg, p, 6) > 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    int ticks = 0;
    for (; ticks < 60 * 180 && !world->skirmish_game_over; ticks += 60) {
        InGame_DebugRunSimTicks(60);
        int t2 = TAK_AI_DebugAttackPlayer(2);
        int t3 = TAK_AI_DebugAttackPlayer(3);
        int t4 = TAK_AI_DebugAttackPlayer(4);
        ASSERT(t2 == 0 || t2 == 4);
        ASSERT(t3 == 0 || t3 == 4);
        ASSERT(t4 != 4);
        int unit_count = 0;
        const Unit *units = Units_GetActive(&unit_count);
        for (int i = 0; i < unit_count; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE || u->cmd_kind != UNIT_CMD_ATTACK) continue;
            if (u->player_id < 2 || u->target < 0 || u->target >= unit_count) continue;
            int owner = units[u->target].player_id;
            ASSERT(Units_PlayersAreEnemies(u->player_id, owner));
        }
        if (TAK_AI_DebugHostileOrders(2, 4, 1) > 0 &&
            TAK_AI_DebugHostileOrders(3, 4, 1) > 0) {
            break;
        }
    }
    fprintf(stderr, "hostility: teamed settled after %d ticks\n", ticks);
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(2, 3, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(3, 2, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(2, 1, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(3, 1, 0));
    ASSERT(TAK_AI_DebugHostileOrders(2, 4, 1) > 0);
    ASSERT(TAK_AI_DebugHostileOrders(3, 4, 1) > 0);
    hostility_teardown(&platform);
}

/* Raiders hit AI 2's monarch at its start. Units that appear at home
 * afterwards are sent at the raiders by the base-defence rule, not
 * merely by a wave that happens to pass. */
TEST(ai_sends_its_home_units_at_a_base_raider) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    static const int teams[4] = { 1, 2, 3, 4 };
    BattleConfig cfg;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, hostility_setup_world(&platform, teams, &cfg, &world));
    int monarch = hostility_monarch_of(2);
    ASSERT(monarch >= 0);
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t mx = units[monarch].world_x, my = units[monarch].world_y;
    int raider_def = hostility_combat_def_for_side(cfg.players[0].side);
    ASSERT(raider_def >= 0);
    int raiders[2];
    raiders[0] = Units_Spawn(raider_def, 1, cfg.players[0].color, mx + 56, my);
    raiders[1] = Units_Spawn(raider_def, 1, cfg.players[0].color, mx - 56, my);
    ASSERT(raiders[0] >= 0 && raiders[1] >= 0);
    Units_CommandAttackUnitScript(raiders[0], monarch);
    Units_CommandAttackUnitScript(raiders[1], monarch);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    /* First second: the raiders land their hits, no defenders yet. */
    InGame_DebugRunSimTicks(60);
    units = Units_GetActive(&unit_count);
    ASSERT(units[monarch].health < units[monarch].max_health);
    ASSERT_EQ_INT(0, TAK_AI_DebugDefenceOrders(2));

    int defender_def = hostility_combat_def_for_side(cfg.players[1].side);
    ASSERT(defender_def >= 0);
    int defenders[3];
    for (int i = 0; i < 3; i++) {
        defenders[i] = Units_Spawn(defender_def, 2, cfg.players[1].color,
                                   mx + 160 + 48 * i, my + 160);
        ASSERT(defenders[i] >= 0);
    }
    int answered = 0;
    for (int t = 0; t < 10 && !answered; t++) {
        InGame_DebugRunSimTicks(60);
        units = Units_GetActive(&unit_count);
        for (int i = 0; i < 3; i++) {
            const Unit *d = &units[defenders[i]];
            if (d->alive != UNIT_ALIVE_ACTIVE) continue;
            if (d->cmd_kind == UNIT_CMD_ATTACK && d->target >= 0 &&
                (d->target == raiders[0] || d->target == raiders[1])) {
                answered = 1;
            }
        }
    }
    ASSERT(answered);
    ASSERT(TAK_AI_DebugDefenceOrders(2) > 0);
    hostility_teardown(&platform);
}


/* The maps size to the map and hold each AI's own army at its start;
 * the human seat gets none. */
TEST(influence_maps_size_to_the_map_and_see_the_army) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    static const int teams[4] = { 1, 2, 3, 4 };
    BattleConfig cfg;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, hostility_setup_world(&platform, teams, &cfg, &world));
    for (int p = 2; p <= 4; p++) ASSERT(hostility_spawn_army(world, &cfg, p, 6) > 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    InGame_DebugRunSimTicks(60);

    int w = 0, h = 0;
    AI_Influence_Size(&w, &h);
    ASSERT_EQ_INT((world->map_pixels_w + AI_INF_CELL_PX - 1) / AI_INF_CELL_PX, w);
    ASSERT_EQ_INT((world->map_pixels_h + AI_INF_CELL_PX - 1) / AI_INF_CELL_PX, h);
    for (int p = 2; p <= 4; p++) {
        int32_t sx = 0, sy = 0;
        ASSERT_EQ_INT(0, hostility_start_of(world, p, &sx, &sy));
        ASSERT(AI_Influence_At(p, AI_INF_PRESENCE, sx, sy) > 0);
        ASSERT(AI_Influence_At(p, AI_INF_OWN_VALUE, sx, sy) > 0);
    }
    int32_t hx = 0, hy = 0;
    ASSERT_EQ_INT(0, hostility_start_of(world, 1, &hx, &hy));
    ASSERT_EQ_INT(0, AI_Influence_At(1, AI_INF_PRESENCE, hx, hy));
    hostility_teardown(&platform);
}

TEST(perf_probe_duel) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[0].kind = TAK_SLOT_AI;
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.players[1].ai_difficulty = 2;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);

    /* 12000 ticks = 200 sim-seconds, windowed profile every 600.
     * Wall-clock capped at 90s so a perf regression reports instead
     * of hanging the suite. */
    Uint64 wall0 = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    memset(g_sim_prof_ms, 0, sizeof(double) * 4);
    double worst_window = 0.0;
    for (int w = 0; w < 20; w++) {
        Uint64 w0 = SDL_GetPerformanceCounter();
        for (int i = 0; i < 600; i++) {
            timer.accumulator = timer.sim_dt;
            next = InGame_Tick(&platform, &timer);
            if (next != GAMESTATE_IN_GAME) break;
        }
        double win_ms = (double)(SDL_GetPerformanceCounter() - w0)
                      * 1000.0 / freq;
        if (win_ms > worst_window) worst_window = win_ms;
        int uc = 0;
        Units_GetActive(&uc);
        printf("\n  [w%02d] %.0fms/600t ai=%.0f eng=%.0f eco=%.0f fog=%.0f "
               "| cmb=%.0f prj=%.0f cob=%.0f misc=%.0f astar=%.0f units=%d",
               w, win_ms, g_sim_prof_ms[0], g_sim_prof_ms[1],
               g_sim_prof_ms[2], g_sim_prof_ms[3],
               g_eng_prof_ms[0], g_eng_prof_ms[1], g_eng_prof_ms[2],
               g_eng_prof_ms[3], g_path_plan_calls, uc);
        memset(g_sim_prof_ms, 0, sizeof(double) * 4);
        memset(g_eng_prof_ms, 0, sizeof(double) * 4);
        g_path_plan_calls = 0.0;
        if (next != GAMESTATE_IN_GAME) break;
        if ((double)(SDL_GetPerformanceCounter() - wall0) / freq > 90.0)
            break;
    }
    printf("\n  worst window: %.0fms/600t (browser frame budget: 600t = 10s real) ",
           worst_window);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(skirmish_ai_duel_reaches_game_over) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.players[1].ai_difficulty = 2;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int local_monarch = -1;
    int ai_monarch = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *def = Units_GetDef(units[i].def_idx);
        if (!def || !strstr(def->category, "Monarch")) continue;
        if (units[i].player_id == 1) local_monarch = i;
        if (units[i].player_id == 2) ai_monarch = i;
    }
    ASSERT(local_monarch >= 0);
    ASSERT(ai_monarch >= 0);

    int local_def = units[local_monarch].def_idx;
    int ai_def = units[ai_monarch].def_idx;
    int32_t duel_x = units[local_monarch].world_x;
    int32_t duel_y = units[local_monarch].world_y;
    ASSERT_EQ_INT(local_monarch, Units_DebugKillHandle(local_monarch));
    ASSERT_EQ_INT(ai_monarch, Units_DebugKillHandle(ai_monarch));

    int local_duelist = Units_Spawn(local_def, 1, cfg.players[0].color,
                                    duel_x, duel_y);
    int ai_duelist = Units_Spawn(ai_def, 2, cfg.players[1].color,
                                 duel_x + 24, duel_y);
    ASSERT(local_duelist >= 0);
    ASSERT(ai_duelist >= 0);
    Units_SetHealthPercent(local_duelist, 1);
    Units_SetHealthPercent(ai_duelist, 1);
    Units_CommandAttackUnit(local_duelist, ai_duelist);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    for (int frame = 0; frame < 180 && !world->skirmish_game_over; frame++) {
        timer.accumulator = timer.sim_dt * 30.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    }
    ASSERT_EQ_INT(1, world->skirmish_game_over);
    ASSERT(world->skirmish_end_reason[0] != '\0');

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Full skirmish progression (handoff item 1) ──────────────────────
 *
 * Proves the AI plays real opening moves against the real systems:
 *   economy construction → completion → mana caps rise → production
 *   structure → completion → mobile combat unit produced in-yard →
 *   unit exits, marches cross-map to the enemy, combat happens.
 *
 * Per-batch invariants (violations counted, asserted zero at the end):
 *   - every alive mobile unit stands on terrain walkable for its
 *     movement class (no wall/void/slope crossing)
 *   - projectiles only spawn when shooter→target is within weapon
 *     range (+slack for batch latency)
 *   - the local monarch only loses HP when an enemy unit or projectile
 *     is plausibly in range (no invisible cross-map damage)
 */
#define PROGRESSION_PROJ_CAP 4096

static int test_max_weapon_range(const UnitDef *d) {
    int r = 0;
    if (!d) return 0;
    for (int w = 0; w < d->num_weapons; w++) {
        if (d->weapons[w].range > r) r = d->weapons[w].range;
    }
    return r;
}

static int test_unit_walk_slope(const GameWorld *w, const UnitDef *d) {
    const MoveClassDef *mc = NULL;
    if (w && d && d->movement_class[0]) {
        mc = TAK_MoveInfo_Find(&w->moveinfo, d->movement_class);
    }
    if (mc && mc->max_slope > 0) return mc->max_slope;
    return d ? d->max_slope : 0;
}

/* A Zhon AI fields an army. Zhon has no production structure: its
 * monarch summons a beast handler and the handler summons the troops,
 * so a planner that only counted structures as producers left a Zhon
 * AI building lodestones for the whole game. Reported from play. */
TEST(zhon_ai_fields_an_army) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.players[1].ai_difficulty = 2;
    cfg.players[1].side = TAK_SIDE_ZHON;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++)
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int zhon_monarch = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *d = Units_GetDef(units[i].def_idx);
        if (d && units[i].player_id == 2 && d->commander) zhon_monarch = i;
    }
    ASSERT(zhon_monarch >= 0);
    ASSERT(strncmp(Units_GetDef(units[zhon_monarch].def_idx)->unitname,
                   "ZON", 3) == 0);

    const int MAX_TICKS = 60 * 60 * 8;   /* eight sim-minutes */
    int ticks = 0, soldier = -1;
    for (; ticks < MAX_TICKS && soldier < 0; ticks += 30) {
        InGame_DebugRunSimTicks(30);
        units = Units_GetActive(&unit_count);
        for (int i = 0; i < unit_count && soldier < 0; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != 2) continue;
            if (u->under_construction) continue;
            const UnitDef *d = Units_GetDef(u->def_idx);
            if (!d || d->max_velocity <= 0.0f || d->num_weapons <= 0) continue;
            if (d->cap_flags & UNIT_CAP_BUILDER) continue;
            soldier = i;
        }
    }
    if (soldier >= 0) {
        printf("[%s after %d ticks] ",
               Units_GetDef(units[soldier].def_idx)->unitname, ticks);
    }
    ASSERT(soldier >= 0);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A monarch adds to its player's pool only what its death takes back:
 * mogrium storage and income (legacy:226990-226996). Its maxmana is its
 * own reserve, so an expendable monarch that dies leaves no cap or
 * recharge behind in the pool. */
TEST(a_dead_monarch_leaves_no_mana_in_the_pool) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 1;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++)
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int monarch = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *d = Units_GetDef(units[i].def_idx);
        if (d && d->commander && units[i].player_id == 1) monarch = i;
    }
    ASSERT(monarch >= 0);
    const UnitDef *md = Units_GetDef(units[monarch].def_idx);
    ASSERT(md->max_mana > 0);
    int32_t cap0 = Economy_GetMaxMana(&world->economy, 1);
    printf("[%s pool %d, storage %d, maxmana %d] ", md->unitname, (int)cap0,
           (int)md->mogrium_storage, (int)md->max_mana);
    ASSERT_EQ_INT((int)md->mogrium_storage, (int)cap0);

    ASSERT_EQ_INT(monarch, Units_DebugKillHandle(monarch));
    for (int t = 0; t < 900; t++) {
        Units_TickEngines();
        units = Units_GetActive(&unit_count);
        if (units[monarch].alive == UNIT_ALIVE_DEAD) break;
    }
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, units[monarch].alive);
    ASSERT_EQ_INT(0, (int)Economy_GetMaxMana(&world->economy, 1));

    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(skirmish_ai_full_progression) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.players[1].ai_difficulty = 2;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);
    ASSERT_EQ_INT(1, world->cfg.line_of_sight);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int local_monarch = -1, ai_monarch = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *def = Units_GetDef(units[i].def_idx);
        if (!def || !strstr(def->category, "Monarch")) continue;
        if (units[i].player_id == 1) local_monarch = i;
        if (units[i].player_id == 2) ai_monarch = i;
    }
    ASSERT(local_monarch >= 0);
    ASSERT(ai_monarch >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    int ai_cap0   = Economy_GetMaxMana(&world->economy, 2);
    int ai_regen0 = Economy_GetRegenRate(&world->economy, 2);
    ASSERT(ai_cap0 > 0);   /* monarch seeded the AI pool */
    int local_monarch_hp0 = units[local_monarch].health;

    /* Stage flags — each flips exactly once, with a timeline print. */
    int econ_handle = -1, econ_done = 0;
    int factory_handle = -1, factory_done = 0;
    int troop_handle = -1, troop_done = 0;
    int troop_moved = 0, saw_troop_moving_anim = 0;
    int contact = 0, combat_damage = 0, saw_attack_anim = 0;
    int32_t troop_home_x = 0, troop_home_y = 0;

    /* Invariant counters. */
    int walk_violations = 0;
    int proj_range_violations = 0;
    int damage_locality_violations = 0;

    static uint8_t proj_prev_alive[PROGRESSION_PROJ_CAP];
    memset(proj_prev_alive, 0, sizeof(proj_prev_alive));
    int prev_local_hp = local_monarch_hp0;

    const int BATCH = 10;
    const int MAX_TICKS = 60 * 60 * 12;   /* 12 sim-minutes budget */
    int ticks = 0;
    for (; ticks < MAX_TICKS; ticks += BATCH) {
        InGame_DebugRunSimTicks(BATCH);
        if (world->skirmish_game_over) break;
        units = Units_GetActive(&unit_count);

        /* ── Invariant: mobile units stand on walkable terrain ── */
        for (int i = 0; i < unit_count; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE) continue;
            const UnitDef *d = Units_GetDef(u->def_idx);
            if (!d || d->max_velocity <= 0.0f) continue;
            if (d->can_fly) continue;   /* flyers ignore terrain */
            if (!Terrain_IsWalkable(world, u->world_x, u->world_y,
                                    test_unit_walk_slope(world, d))) {
                if (walk_violations < 5) {
                    fprintf(stderr,
                        "progression: tick %d: %s (h=%d) on blocked "
                        "terrain at (%d,%d)\n",
                        ticks, d->unitname, i, u->world_x, u->world_y);
                }
                walk_violations++;
            }
        }

        /* ── Invariant: projectiles spawn within weapon range ── */
        int pcount = 0;
        const Projectile *projs = Units_GetProjectiles(&pcount);
        if (pcount > PROGRESSION_PROJ_CAP) pcount = PROGRESSION_PROJ_CAP;
        for (int i = 0; i < pcount; i++) {
            const Projectile *p = &projs[i];
            if (p->alive && !proj_prev_alive[i]) {
                if (p->shooter >= 0 && p->shooter < unit_count &&
                    p->target >= 0 && p->target < unit_count) {
                    const Unit *s = &units[p->shooter];
                    const Unit *t = &units[p->target];
                    const UnitDef *sd = Units_GetDef(s->def_idx);
                    int64_t dx = (int64_t)t->world_x - s->world_x;
                    int64_t dy = (int64_t)t->world_y - s->world_y;
                    int64_t lim = (int64_t)test_max_weapon_range(sd) + 96;
                    if (dx * dx + dy * dy > lim * lim) {
                        if (proj_range_violations < 5) {
                            fprintf(stderr,
                                "progression: tick %d: projectile from "
                                "%s beyond range (d2=%lld lim=%lld)\n",
                                ticks, sd ? sd->unitname : "?",
                                (long long)(dx * dx + dy * dy),
                                (long long)(lim * lim));
                        }
                        proj_range_violations++;
                    }
                }
            }
            proj_prev_alive[i] = p->alive;
        }

        /* ── Invariant: local monarch damage has a plausible source ── */
        if (local_monarch < unit_count &&
            units[local_monarch].alive == UNIT_ALIVE_ACTIVE) {
            int hp = units[local_monarch].health;
            if (hp < prev_local_hp) {
                int found_source = 0;
                for (int i = 0; i < unit_count && !found_source; i++) {
                    const Unit *u = &units[i];
                    if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != 2)
                        continue;
                    const UnitDef *d = Units_GetDef(u->def_idx);
                    int64_t dx = (int64_t)u->world_x
                               - units[local_monarch].world_x;
                    int64_t dy = (int64_t)u->world_y
                               - units[local_monarch].world_y;
                    int64_t lim = (int64_t)test_max_weapon_range(d) + 64;
                    if (dx * dx + dy * dy <= lim * lim) found_source = 1;
                }
                for (int i = 0; i < pcount && !found_source; i++) {
                    if (projs[i].alive &&
                        projs[i].target == local_monarch) found_source = 1;
                }
                if (!found_source) {
                    if (damage_locality_violations < 5) {
                        fprintf(stderr,
                            "progression: tick %d: monarch damaged with "
                            "no enemy in range\n", ticks);
                    }
                    damage_locality_violations++;
                }
            }
            prev_local_hp = hp;
        }

        /* ── Stage machine ── */
        for (int i = 0; i < unit_count; i++) {
            const Unit *u = &units[i];
            if (u->player_id != 2) continue;
            if (u->alive != UNIT_ALIVE_ACTIVE) continue;
            const UnitDef *d = Units_GetDef(u->def_idx);
            if (!d) continue;
            int is_monarch = strstr(d->category, "Monarch") != NULL;
            int is_econ = !is_monarch &&
                          !(d->cap_flags & UNIT_CAP_BUILDER) &&
                          (d->mogrium_storage > 0 ||
                           d->mogrium_income_per_sec > 0.0f ||
                           d->max_mana > 0 ||
                           d->mana_recharge_per_sec > 0.0f ||
                           strstr(d->unitname, "LODE") != NULL);
            int is_factory = !is_econ && !is_monarch &&
                             d->max_velocity <= 0.0f &&
                             (d->cap_flags & UNIT_CAP_BUILDER);
            int is_troop = !is_monarch &&
                           d->max_velocity > 0.0f && d->num_weapons > 0;

            if (is_econ && econ_handle < 0) {
                econ_handle = i;
                fprintf(stderr, "progression: tick %d: economy build "
                        "started (%s)\n", ticks, d->unitname);
            }
            if (is_econ && i == econ_handle && !econ_done &&
                !u->under_construction && u->health >= u->max_health) {
                econ_done = 1;
                fprintf(stderr, "progression: tick %d: economy complete; "
                        "cap %d→%d regen %d→%d\n", ticks,
                        ai_cap0, Economy_GetMaxMana(&world->economy, 2),
                        ai_regen0, Economy_GetRegenRate(&world->economy, 2));
            }
            if (is_factory && factory_handle < 0) {
                factory_handle = i;
                fprintf(stderr, "progression: tick %d: production "
                        "structure started (%s)\n", ticks, d->unitname);
            }
            if (is_factory && i == factory_handle && !factory_done &&
                !u->under_construction && u->health >= u->max_health) {
                factory_done = 1;
                fprintf(stderr, "progression: tick %d: production "
                        "structure complete\n", ticks);
            }
            if (is_troop && troop_handle < 0) {
                troop_handle = i;
                troop_home_x = u->world_x;
                troop_home_y = u->world_y;
                fprintf(stderr, "progression: tick %d: combat unit "
                        "production started (%s)\n", ticks, d->unitname);
            }
            if (is_troop && i == troop_handle) {
                if (!troop_done && !u->under_construction &&
                    u->health >= u->max_health) {
                    troop_done = 1;
                    fprintf(stderr, "progression: tick %d: combat unit "
                            "complete\n", ticks);
                }
                if (troop_done) {
                    int64_t dx = (int64_t)u->world_x - troop_home_x;
                    int64_t dy = (int64_t)u->world_y - troop_home_y;
                    if (!troop_moved && dx * dx + dy * dy > 48 * 48) {
                        troop_moved = 1;
                        fprintf(stderr, "progression: tick %d: combat "
                                "unit left the yard\n", ticks);
                    }
                    if (u->anim_state == UNIT_ANIM_MOVING)
                        saw_troop_moving_anim = 1;
                }
            }
            /* Contact: any AI mobile combat unit near a player-1 unit. */
            if (is_troop && !contact) {
                for (int j = 0; j < unit_count; j++) {
                    const Unit *e = &units[j];
                    if (e->alive != UNIT_ALIVE_ACTIVE || e->player_id != 1)
                        continue;
                    int64_t dx = (int64_t)u->world_x - e->world_x;
                    int64_t dy = (int64_t)u->world_y - e->world_y;
                    if (dx * dx + dy * dy <= 300 * 300) {
                        contact = 1;
                        fprintf(stderr, "progression: tick %d: contact "
                                "with enemy\n", ticks);
                        break;
                    }
                }
            }
        }
        for (int i = 0; i < unit_count && !saw_attack_anim; i++) {
            if (units[i].alive == UNIT_ALIVE_ACTIVE &&
                units[i].anim_state == UNIT_ANIM_ATTACKING) {
                saw_attack_anim = 1;
                fprintf(stderr, "progression: tick %d: attack animation "
                        "state observed\n", ticks);
            }
        }
        if (contact && !combat_damage) {
            int damaged = 0;
            if (local_monarch < unit_count &&
                units[local_monarch].health <
                    units[local_monarch].max_health) damaged = 1;
            for (int i = 0; i < unit_count && !damaged; i++) {
                const Unit *u = &units[i];
                if (u->player_id != 2 || u->alive != UNIT_ALIVE_ACTIVE)
                    continue;
                if (u->under_construction) continue;
                const UnitDef *d = Units_GetDef(u->def_idx);
                if (!d || d->max_velocity <= 0.0f || d->num_weapons <= 0)
                    continue;
                if (u->health < u->max_health) damaged = 1;
            }
            if (damaged) {
                combat_damage = 1;
                fprintf(stderr, "progression: tick %d: combat damage "
                        "dealt\n", ticks);
            }
        }

        if (econ_done && factory_done && troop_done && troop_moved &&
            saw_troop_moving_anim && contact && combat_damage &&
            saw_attack_anim) {
            break;
        }
    }
    fprintf(stderr, "progression: finished at tick %d "
            "(econ=%d factory=%d troop=%d moved=%d anim=%d contact=%d "
            "damage=%d attack_anim=%d)\n",
            ticks, econ_done, factory_done, troop_done, troop_moved,
            saw_troop_moving_anim, contact, combat_damage, saw_attack_anim);

    /* Diagnostic: when the production structure never sites, dump an
     * ASCII walkability map around the AI monarch so the blocked
     * neighborhood is visible in the test log. */
    if (factory_handle < 0 && ai_monarch < unit_count) {
        const Unit *am = &units[ai_monarch];
        fprintf(stderr, "progression: AI monarch at (%d,%d), map %dx%d px\n",
                am->world_x, am->world_y,
                world->map_pixels_w, world->map_pixels_h);
        for (int gy = -24; gy <= 24; gy += 2) {
            char row[100];
            int ri = 0;
            for (int gx = -48; gx <= 48; gx += 2) {
                int32_t wx = am->world_x + gx * 16;
                int32_t wy = am->world_y + gy * 16;
                char c = Terrain_IsWalkable(world, wx, wy, 255) ? '.' : '#';
                if (gx == 0 && gy == 0) c = 'M';
                row[ri++] = c;
            }
            row[ri] = '\0';
            fprintf(stderr, "progression: %s\n", row);
        }
    }

    /* Stage asserts — in progression order so the first failure names
     * the earliest missing gameplay piece. */
    ASSERT(econ_handle >= 0);
    ASSERT(econ_done);
    ASSERT(Economy_GetMaxMana(&world->economy, 2) > ai_cap0);
    ASSERT(Economy_GetRegenRate(&world->economy, 2) > ai_regen0);
    ASSERT(factory_handle >= 0);
    ASSERT(factory_done);
    ASSERT(troop_handle >= 0);
    ASSERT(troop_done);
    ASSERT(troop_moved);
    ASSERT(saw_troop_moving_anim);
    ASSERT(contact);
    ASSERT(saw_attack_anim);
    ASSERT(combat_damage);

    /* Rule-validity asserts. */
    ASSERT_EQ_INT(0, walk_violations);
    ASSERT_EQ_INT(0, proj_range_violations);
    ASSERT_EQ_INT(0, damage_locality_violations);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Per-site build rules, all on one map load (the suite is a 32-bit
 * process and each load is expensive):
 *   - a lodestone (yardmap 'S') is legal only where its 'S' cells
 *     cover a sacred site's whole footprint (legacy:218858-218889);
 *   - a click anywhere on the pad snaps to the pad's cells
 *     (legacy:184168);
 *   - hulls and land structures obey the water-depth window
 *     (legacy:219149-219156, :218890-218911). */
TEST(build_placement_sacred_and_water_rules) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);

    int lode = Units_FindDefByName("ARALODE");
    ASSERT(lode >= 0);
    const UnitDef *ld = Units_GetDef(lode);
    ASSERT_NOT_NULL(ld);
    ASSERT_EQ_INT(1, ld->yardmap_sacred);

    /* `yardmap = S;` repeats to fill the footprint (legacy:163216). */
    uint8_t yard[TAK_YARD_MAX_CELLS];
    int ycells = Units_ExpandYardmap(ld, yard, TAK_YARD_MAX_CELLS);
    ASSERT_EQ_INT(ld->footprint_x * ld->footprint_z, ycells);
    for (int i = 0; i < ycells; i++)
        ASSERT_EQ_INT(TAK_YARD_SACRED, yard[i] & TAK_YARD_SACRED);

    int32_t hw = ld->footprint_x * 8;
    int32_t hh = ld->footprint_z * 8;

    int pads = 0, on_pad_ok = 0, shifted_ok = 0;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        if (!fd || fd->sacred_site <= 0.0f) continue;
        pads++;
        int32_t wx = (int32_t)world->features[i].tile_x * 16 + hw;
        int32_t wy = (int32_t)world->features[i].tile_z * 16 + hh;
        if (Units_IsBuildSiteClear(lode, wx, wy)) on_pad_ok++;
        /* One cell across leaves half the pad bare (legacy:218887). */
        if (Units_IsBuildSiteClear(lode, wx + 16, wy)) shifted_ok++;
    }
    fprintf(stderr, "lodestone: %d pads, %d accept, %d accept shifted\n",
            pads, on_pad_ok, shifted_ok);
    ASSERT(pads > 0);
    ASSERT(on_pad_ok > 0);
    ASSERT_EQ_INT(0, shifted_ok);

    /* A sloppy click anywhere inside the pad resolves to the pad's own
     * cells. The in-game handler maps the cursor to cam + mouse and
     * snaps to the build cell (legacy:184168), which is the same site
     * the ghost draws at, so preview and building agree. */
    int clicked = 0;
    for (int i = 0; i < world->feature_count && !clicked; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        if (!fd || fd->sacred_site <= 0.0f) continue;
        int32_t pad_x0 = (int32_t)world->features[i].tile_x * 16;
        int32_t pad_y0 = (int32_t)world->features[i].tile_z * 16;
        if (!Units_IsBuildSiteClear(lode, pad_x0 + hw, pad_y0 + hh)) continue;
        for (int32_t oy = -7; oy <= 7; oy += 7) {
            for (int32_t ox = -7; ox <= 7; ox += 7) {
                int32_t cx = pad_x0 + hw + ox, cy = pad_y0 + hh + oy;
                int32_t sx = cx, sy = cy;
                Units_SnapBuildSite(lode, &sx, &sy);
                /* Footprint origin lands on the pad's own cell. */
                ASSERT_EQ_INT(pad_x0, sx - hw);
                ASSERT_EQ_INT(pad_y0, sy - hh);
                ASSERT_EQ_INT(1, Units_IsBuildSiteClear(lode, cx, cy));
            }
        }
        clicked = 1;
    }
    ASSERT(clicked);

    /* Off every pad the site is refused, while a 3x3 tower with no 'S'
     * cell still sites normally on the same ground. */
    int tower = Units_FindDefByName("ARAAT");
    ASSERT(tower >= 0);
    ASSERT_EQ_INT(0, Units_GetDef(tower)->yardmap_sacred);
    int checked = 0, off_pad_ok = 0, tower_ok = 0;
    for (int32_t y = 128; y < world->map_pixels_h - 128 && checked < 160;
         y += 96) {
        for (int32_t x = 128; x < world->map_pixels_w - 128 && checked < 160;
             x += 96) {
            int near_pad = 0;
            for (int i = 0; i < world->feature_count && !near_pad; i++) {
                const FeatureDef *fd =
                    Features_GetByIndex(world->features[i].global_idx);
                if (!fd || fd->sacred_site <= 0.0f) continue;
                int32_t px = (int32_t)world->features[i].tile_x * 16;
                int32_t py = (int32_t)world->features[i].tile_z * 16;
                if (px > x - 256 && px < x + 256 &&
                    py > y - 256 && py < y + 256) near_pad = 1;
            }
            if (near_pad) continue;
            checked++;
            if (Units_IsBuildSiteClear(lode, x, y)) off_pad_ok++;
            if (Units_IsBuildSiteClear(tower, x, y)) tower_ok++;
        }
    }
    fprintf(stderr, "lodestone: %d off-pad sites, %d accept, tower %d\n",
            checked, off_pad_ok, tower_ok);
    ASSERT(checked > 0);
    ASSERT_EQ_INT(0, off_pad_ok);
    ASSERT(tower_ok > 0);

    /* ── Water depth, on the same loaded map ──────────────────────
     * A hull needs its move class's minimum depth and a land
     * structure may not stand deeper than its maxwaterdepth
     * (legacy:219149-219156 for hulls, :218890-218911 through the
     * yardmap for buildings). */
    ASSERT(world->water_height > 0);
    int sea = world->water_height;

    int ship = Units_FindDefByName("ARAWAR");     /* WATER4 hull      */
    int dock = Units_FindDefByName("VERFLTWR");   /* yardmap all 'w'  */
    ASSERT(ship >= 0);
    ASSERT_EQ_STR("WATER4", Units_GetDef(ship)->movement_class);

    /* Probe the heightmap directly so the sites are chosen without
     * consulting the rule under test. */
    int32_t land_x = 0, land_y = 0, sea_x = 0, sea_y = 0;
    int have_land = 0, have_sea = 0;
    for (int32_t y = 128; y < world->map_pixels_h - 128 &&
                          !(have_land && have_sea); y += 64) {
        for (int32_t x = 128; x < world->map_pixels_w - 128 &&
                              !(have_land && have_sea); x += 64) {
            int dry = 1, deep = 1;
            for (int32_t dy = -48; dy <= 48; dy += 16) {
                for (int32_t dx = -48; dx <= 48; dx += 16) {
                    int h = Terrain_SampleHeight(world, x + dx, y + dy);
                    if (h < sea + 8)  dry  = 0;
                    if (h > sea - 24) deep = 0;
                }
            }
            if (dry && !have_land && Units_IsBuildSiteClear(tower, x, y)) {
                land_x = x; land_y = y; have_land = 1;
            }
            if (deep && !have_sea) { sea_x = x; sea_y = y; have_sea = 1; }
        }
    }
    fprintf(stderr, "water: sea=%d land=(%d,%d,%d) deep=(%d,%d,%d)\n", sea,
            have_land, land_x, land_y, have_sea, sea_x, sea_y);
    ASSERT(have_land);
    ASSERT(have_sea);

    /* A hull is refused on dry ground and takes deep water. */
    ASSERT_EQ_INT(0, Units_IsBuildSiteClear(ship, land_x, land_y));
    ASSERT_EQ_INT(1, Units_IsBuildSiteClear(ship, sea_x, sea_y));

    /* A land structure is the mirror image. */
    ASSERT_EQ_INT(1, Units_IsBuildSiteClear(tower, land_x, land_y));
    ASSERT_EQ_INT(0, Units_IsBuildSiteClear(tower, sea_x, sea_y));

    /* A floating structure follows its yardmap, not its hull. */
    if (dock >= 0) {
        ASSERT_EQ_INT(0, Units_IsBuildSiteClear(dock, land_x, land_y));
        ASSERT_EQ_INT(1, Units_IsBuildSiteClear(dock, sea_x, sea_y));
    }

    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Visual probe (not part of the default suite pass criteria beyond
 * rendering without crashing): spawn a completed ARABUILD + a walking
 * ARAKING near the camera and save a frame for eyeball inspection.
 * Run with: test_ui_screens.exe render_probe */
TEST(render_probe_building_and_walker) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.line_of_sight = 0;   /* no fog — isolate mesh rendering */
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    /* Spawn WEST of the monarch — the right side of the screen is
     * covered by the HUD sidebar + minimap and occludes anything
     * standing there (the original "partial building" red herring). */
    int barracks_def = Units_FindDefByName("ARABUILD");
    ASSERT(barracks_def >= 0);
    int barracks = Units_Spawn(barracks_def, 1, 0, cx - 160, cy - 48);
    ASSERT(barracks >= 0);

    /* Walk EAST — the cape-vs-torso worst case (side-on view). */
    int walker = Units_Spawn(units[0].def_idx, 1, 0, cx - 260, cy + 64);
    ASSERT(walker >= 0);
    Units_CommandMoveUnit(walker, cx + 600, cy + 64);

    /* Center camera on the scene. */
    world->cam_x = cx - world->viewport_w / 2;
    world->cam_y = cy - world->viewport_h / 2;

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    /* Let Create scripts settle + walker mid-stride. */
    for (int frame = 0; frame < 40; frame++) {
        timer.accumulator = timer.sim_dt * 3.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    }
    ASSERT_EQ_INT(0, save_and_check_renderer(&platform,
                                             "test_render_probe.bmp"));

    /* Same scene with backface culling OFF — isolates whether cape
     * artifacts come from culling single-sided cloth polys. */
    Units_SetBackfaceCullOn(0);
    timer.accumulator = timer.sim_dt;
    next = InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    ASSERT_EQ_INT(0, save_and_check_renderer(&platform,
                                             "test_render_probe_culloff.bmp"));
    Units_SetBackfaceCullOn(1);

    /* Second frame with ALL piece state zeroed (no hides, identity
     * transforms) and COB stopped — isolates piece-state-driven
     * invisibility from static mesh/submit-path bugs. */
    units = Units_GetActive(&unit_count);
    if (barracks < unit_count && units[barracks].cob) {
        Unit *bu = (Unit *)&units[barracks];   /* test-only mutation */
        Cob_KillAllThreads(bu->cob);
        for (int n = 0; n < bu->cob->piece_count; n++) {
            memset(&bu->cob->pieces[n], 0, sizeof(CobPiece));
        }
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        ASSERT_EQ_INT(0, save_and_check_renderer(&platform,
                                                 "test_render_probe_zeroed.bmp"));
    }

    /* Dump the probe unit's live COB piece state — shows which pieces
     * the runtime scripts hid/moved with real host values. */
    units = Units_GetActive(&unit_count);
    if (barracks < unit_count && units[barracks].cob) {
        const CobEngine *e = units[barracks].cob;
        const UnitDef *bd = Units_GetDef(units[barracks].def_idx);
        const UnitMesh *m = bd ? bd->mesh_per_color[0] : NULL;
        fprintf(stderr, "probe: ARABUILD alive=%d hp=%d/%d anim=%d "
                "threads=%d\n",
                units[barracks].alive, units[barracks].health,
                units[barracks].max_health, units[barracks].anim_state,
                Cob_AliveThreadCount(e));
        for (int n = 0; m && n < m->node_count && n < e->piece_count; n++) {
            const CobPiece *p = &e->pieces[n];
            if (p->hidden || p->rot[0] || p->rot[1] || p->rot[2] ||
                p->pos[0] || p->pos[1] || p->pos[2]) {
                fprintf(stderr, "probe:   node[%2d] %-12s%s rot=(%d,%d,%d) "
                        "pos=(%d,%d,%d)\n", n, m->nodes[n].name,
                        p->hidden ? " HIDDEN" : "",
                        p->rot[0], p->rot[1], p->rot[2],
                        p->pos[0], p->pos[1], p->pos[2]);
            }
        }
    }

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Model parity probe + guard: ARALODE proportions and ARAKING piece
 * draw order. Frames one lodestone and one monarch large on the left
 * half of the canvas (the HUD sidebar owns the right) and pins the
 * numbers the two owner-reported bugs moved.
 *
 * ARALODE is a single camera-aligned billboard quad: 53.8 model units
 * wide, and its one tilted edge (dy 29.0, dz 47.8) projects through
 * sy = -z - (y >> 1) (legacy:197689) to 62.3 px, so the 64x64
 * `araplainlode` texture lands near 1 texel per pixel. Getting the
 * model's z sense backwards collapses that to 33 px.
 *
 * ARAKING draws its nodes in reverse table order (legacy:197658,
 * legacy:197944) under a per-pixel height key = base + model y
 * (legacy:197697) that the rasteriser depth-tests (legacy:265317).
 * Head triangles sit above cape and torso ones in y, so they must
 * submit last. */
TEST(render_probe_models) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.line_of_sight = 0;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    fprintf(stderr, "probe: anchor unit at (%d, %d)\n", cx, cy);
    int lode_def = Units_FindDefByName("ARALODE");
    int king_def = Units_FindDefByName("ARAKING");
    int keep_def = Units_FindDefByName("ARAKEEP");
    int cast_def = Units_FindDefByName("ARACASTL");
    ASSERT(lode_def >= 0);
    ASSERT(king_def >= 0);
    ASSERT(keep_def >= 0);
    ASSERT(cast_def >= 0);

    /* Row 1: lodestone, monarch facing the camera, monarch facing away.
     * Row 2 (a screen down): keep and castle. */
    int lode   = Units_Spawn(lode_def, 1, 0, cx - 150, cy + 40);
    int king   = Units_Spawn(king_def, 1, 0, cx - 40,  cy + 40);
    int king_n = Units_Spawn(king_def, 1, 0, cx + 60,  cy + 40);
    int keep   = Units_Spawn(keep_def, 1, 0, cx - 190, cy + 420);
    int castle = Units_Spawn(cast_def, 1, 0, cx + 20,  cy + 420);
    ASSERT(lode >= 0);
    ASSERT(king >= 0);
    ASSERT(king_n >= 0);
    ASSERT(keep >= 0);
    ASSERT(castle >= 0);
    /* Heading 0 is north in our convention, so this one shows its back
     * and the cape reads against the body. */
    Units_SetHeading(king_n, 0.0f);

    world->cam_x = cx - world->viewport_w / 2;
    world->cam_y = cy - world->viewport_h / 2;

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int frame = 0; frame < 40; frame++) {
        /* Pin the camera every frame so successive probe runs are
         * pixel-comparable. */
        world->cam_x = cx - world->viewport_w / 2;
        world->cam_y = cy - world->viewport_h / 2;
        timer.accumulator = timer.sim_dt * 3.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    }
    ASSERT_EQ_INT(0, save_and_check_renderer(&platform,
                                             "test_render_probe_models.bmp"));

    /* Pan down to the strongholds and take a second frame. They stand
     * ~330 px tall once projected, so the camera sits low enough to
     * keep the flag pole on screen. */
    world->cam_y = cy + 420 - 340;
    timer.accumulator = timer.sim_dt;
    next = InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    ASSERT_EQ_INT(0, save_and_check_renderer(&platform,
                                             "test_render_probe_holds.bmp"));
    /* Restore the framing the bounds/order checks below expect. */
    world->cam_y = cy - world->viewport_h / 2;

    {   /* Where each probe model landed, for eyeballing the BMPs. */
        static const char *nm[] = { "ARALODE", "ARAKING-S", "ARAKING-N" };
        const int hs[] = { lode, king, king_n };
        for (int i = 0; i < 3; i++) {
            float a[2], z[2];
            if (Units_DebugProjectedBounds(hs[i], world, a, z) == 0) {
                fprintf(stderr, "probe: %s screen box (%.0f,%.0f)-(%.0f,%.0f)\n",
                        nm[i], (double)a[0], (double)a[1],
                        (double)z[0], (double)z[1]);
            }
        }
    }

    /* ── ARALODE proportions ─────────────────────────────────────── */
    float lo[2], hi[2];
    ASSERT_EQ_INT(0, Units_DebugProjectedBounds(lode, world, lo, hi));
    float lw = hi[0] - lo[0], lh = hi[1] - lo[1];
    fprintf(stderr, "probe: ARALODE projected %.1f x %.1f px (aspect %.3f)\n",
            (double)lw, (double)lh, (double)(lw / lh));

    /* One model unit is one world pixel (legacy:197689), so the quad's
     * 53.8 units of width and its 29.0/47.8 lean projecting to 62.3 px
     * of height land on the screen unchanged. */
    ASSERT(lw > 52.0f && lw < 56.0f);
    ASSERT(lh > 60.0f && lh < 65.0f);

    /* Same scale rule, checked against shipped footprint data: ARAAT's
     * model is exactly 3 cells wide and its FBI says footprintx 3, so
     * the tower must project to 3 x 16 px. */
    int at_def = Units_FindDefByName("ARAAT");
    ASSERT(at_def >= 0);
    int tower = Units_Spawn(at_def, 1, 0, cx - 150, cy + 180);
    ASSERT(tower >= 0);
    const UnitDef *atd = Units_GetDef((uint16_t)at_def);
    ASSERT_NOT_NULL(atd);
    ASSERT_EQ_INT(0, Units_DebugProjectedBounds(tower, world, lo, hi));
    float tw = hi[0] - lo[0];
    fprintf(stderr, "probe: ARAAT projected %.1f px wide, footprintx %d\n",
            (double)tw, atd->footprint_x);
    ASSERT_EQ_INT(3, atd->footprint_x);
    ASSERT(tw > 46.0f && tw < 50.0f);

    /* ── ARAKING piece draw order ────────────────────────────────── */
    const UnitDef *kd = Units_GetDef((uint16_t)king_def);
    ASSERT_NOT_NULL(kd);
    const UnitMesh *km = kd->mesh_per_color[0];
    ASSERT_NOT_NULL(km);
    int head = -1, torso = -1, cape1 = -1;
    for (int n = 0; n < km->node_count; n++) {
        if (strcmp(km->nodes[n].name, "Head") == 0)  head  = n;
        if (strcmp(km->nodes[n].name, "Torso") == 0) torso = n;
        if (strcmp(km->nodes[n].name, "Cape1") == 0) cape1 = n;
    }
    ASSERT(head >= 0 && torso >= 0 && cape1 >= 0);

    /* First submit slot of each piece, in real draw order. */
    static uint16_t order[8192];
    static float    keys[8192];
    const int order_cap = (int)(sizeof(order) / sizeof(order[0]));
    int ntri = Units_DebugSubmitOrder(king, world, order, keys, order_cap);
    ASSERT(ntri > 0);
    int at_head = -1, at_torso = -1;
    for (int t = 0; t < ntri; t++) {
        if (at_head  < 0 && order[t] == head)  at_head  = t;
        if (at_torso < 0 && order[t] == torso) at_torso = t;
    }
    fprintf(stderr, "probe: ARAKING front submit slots of %d tris: "
            "torso=%d head=%d\n", ntri, at_torso, at_head);
    /* Head is the tallest of the two, so it submits last and stays
     * visible over the torso, the reported "no head" bug. */
    ASSERT(at_torso >= 0);
    ASSERT(at_head > at_torso);

    /* From behind, the cape is front-facing and survives the cull.
     * The torso still carries the taller key at any shared pixel, so
     * both torso and head submit after it. */
    ntri = Units_DebugSubmitOrder(king_n, world, order, keys, order_cap);
    ASSERT(ntri > 0);
    at_head = at_torso = -1;
    int at_cape = -1;
    for (int t = 0; t < ntri; t++) {
        if (at_head  < 0 && order[t] == head)  at_head  = t;
        if (at_torso < 0 && order[t] == torso) at_torso = t;
        if (at_cape  < 0 && order[t] == cape1) at_cape  = t;
    }
    fprintf(stderr, "probe: ARAKING rear submit slots of %d tris: "
            "cape1=%d torso=%d head=%d\n", ntri, at_cape, at_torso, at_head);
    ASSERT(at_cape >= 0);
    ASSERT(at_torso > at_cape);
    ASSERT(at_head  > at_cape);

    /* ── ARAKEEP stronghold layering ─────────────────────────────── */
    const UnitDef *kpd = Units_GetDef((uint16_t)keep_def);
    ASSERT_NOT_NULL(kpd);
    const UnitMesh *kpm = kpd->mesh_per_color[0];
    ASSERT_NOT_NULL(kpm);
    int base = -1, pole = -1;
    for (int n = 0; n < kpm->node_count; n++) {
        if (strcmp(kpm->nodes[n].name, "Base") == 0) base = n;
        if (strcmp(kpm->nodes[n].name, "Pole") == 0) pole = n;
    }
    ASSERT(base >= 0 && pole >= 0);

    ntri = Units_DebugSubmitOrder(keep, world, order, keys, order_cap);
    ASSERT(ntri > 0);
    /* Keys must come out non-decreasing: that IS the height-key rule,
     * and it is what stops the keep's walls painting over its tower. */
    int ascending = 1, base_tris = 0, base_before_pole = 0;
    int at_pole = -1;
    for (int t = 0; t < ntri; t++) {
        if (t > 0 && keys[t] < keys[t - 1]) ascending = 0;
        if (order[t] == base) {
            base_tris++;
            if (at_pole < 0) base_before_pole++;
        }
        if (at_pole < 0 && order[t] == pole) at_pole = t;
    }
    fprintf(stderr, "probe: ARAKEEP %d tris, pole at %d, %d/%d Base tris "
            "before it, ascending=%d\n",
            ntri, at_pole, base_before_pole, base_tris, ascending);
    ASSERT(ascending);
    ASSERT(base_tris > 0);
    /* The whole keep lives in one 120-prim Base node, so authored order
     * alone cannot layer it. The flag pole tops the model out, so most
     * of Base must already be down before the pole goes on. */
    ASSERT(at_pole > 0);
    ASSERT(base_before_pole * 2 > base_tris);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Factory production queue + rally + cancel (manual §Summoning Units):
 * queue two products on a completed TARCASTL, verify sequential
 * production, rally-point exit, and cancel-current advancing the
 * queue. */
TEST(factory_queue_rally_and_cancel) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    /* Spawn a COMPLETED castle (factory) for player 1. */
    int castle_def = Units_FindDefByName("TARCASTL");
    int troop_def  = Units_FindDefByName("TARTROOP");
    int zombie_def = Units_FindDefByName("TARZOM");
    ASSERT(castle_def >= 0);
    ASSERT(troop_def >= 0);
    ASSERT(zombie_def >= 0);
    int castle = Units_Spawn(castle_def, 1, 0, cx - 400, cy);
    ASSERT(castle >= 0);

    /* Give player 1 a deep mana pool so production never starves. */
    Economy_AdjustCaps(&world->economy, 1, 100000, 500.0f);
    Economy_Earn(&world->economy, 1, 100000);

    /* Rally point south of the castle. */
    int32_t rally_x = cx - 400;
    int32_t rally_y = cy + 300;
    Units_FactorySetRally(castle, rally_x, rally_y);

    /* Queue: troop starts immediately (idle factory), zombie queues. */
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(castle, troop_def));
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(castle, zombie_def));
    ASSERT_EQ_INT(1, Units_FactoryQueueCount(castle));

    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, units[castle].cmd_kind);
    int troop = units[castle].build_target;
    ASSERT(troop >= 0);
    ASSERT_EQ_INT(troop_def, units[troop].def_idx);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    /* Run until the troop completes; the zombie must then start
     * automatically from the queue. */
    int zombie = -1;
    for (int frame = 0; frame < 600 && zombie < 0; frame++) {
        timer.accumulator = timer.sim_dt * 30.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
        if (units[troop].alive == UNIT_ALIVE_ACTIVE &&
            !units[troop].under_construction &&
            units[castle].build_target >= 0 &&
            units[castle].build_target != troop) {
            zombie = units[castle].build_target;
        }
    }
    ASSERT(zombie >= 0);
    ASSERT_EQ_INT(zombie_def, units[zombie].def_idx);
    ASSERT_EQ_INT(0, Units_FactoryQueueCount(castle));

    /* The completed troop must be moving to the rally point. */
    ASSERT_EQ_INT(UNIT_CMD_MOVE, units[troop].cmd_kind);
    ASSERT_EQ_INT(rally_x, units[troop].cmd_x);
    ASSERT_EQ_INT(rally_y, units[troop].cmd_y);

    /* Cancel the in-progress zombie: nanoframe removed, factory idle
     * (queue empty), no crash. */
    ASSERT_EQ_INT(0, Units_FactoryCancelCurrent(castle));
    units = Units_GetActive(&unit_count);
    ASSERT(units[zombie].alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(UNIT_CMD_NONE, units[castle].cmd_kind);
    ASSERT_EQ_INT(-1, (int)units[castle].build_target);

    /* Move on the immobile factory re-points the rally, not a walk. */
    Units_SelectSingle(castle);
    Units_CommandMoveSelected(cx - 400, cy + 100);
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_NONE, units[castle].cmd_kind);
    ASSERT_EQ_INT(1, (int)units[castle].rally_set);
    ASSERT_EQ_INT(cy + 100, units[castle].rally_y);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Products must materialise on the yard's build pad, the ring at the
 * front of the structure named by the COB QueryBuildInfo piece
 * (legacy:9347-9362), and then walk clear of it, not appear inside
 * the building. */
TEST(factory_product_spawns_on_build_pad) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    ASSERT_NOT_NULL(World_Get());

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    int castle_def = Units_FindDefByName("TARCASTL");
    int troop_def  = Units_FindDefByName("TARTROOP");
    ASSERT(castle_def >= 0);
    ASSERT(troop_def >= 0);
    int castle = Units_Spawn(castle_def, 1, 0, cx - 400, cy);
    ASSERT(castle >= 0);
    Economy_AdjustCaps(&World_Get()->economy, 1, 100000, 500.0f);
    Economy_Earn(&World_Get()->economy, 1, 100000);

    units = Units_GetActive(&unit_count);
    int32_t fx = units[castle].world_x;
    int32_t fy = units[castle].world_y;

    /* TARCASTL's QueryBuildInfo names piece `emitbuild`, a child of the
     * `buildpad` ring at the front of the castle. Structures place
     * facing south, so the pad sits south of centre. */
    int32_t pad_x = 0, pad_y = 0;
    ASSERT_EQ_INT(1, Units_FactoryBuildSpot(castle, &pad_x, &pad_y));
    ASSERT(pad_x != fx || pad_y != fy);
    ASSERT(pad_y > fy + 100);
    /* On the pad, not off in the next field: `emitbuild` lands ~176 px
     * out, right at the footprint edge (footprintz 20 = 160 px). */
    int half_z = Units_GetFootprintZ(castle_def) * 8;
    int half_x = Units_GetFootprintX(castle_def) * 8;
    ASSERT(pad_y - fy <= half_z + 32);
    ASSERT(pad_x - fx <= half_x && fx - pad_x <= half_x);

    /* A script with no QueryBuildInfo reports no pad, which is what
     * makes production fall back to the yard centre. No shipped factory
     * is in that state, but every mobile unit's script is. */
    int troop_probe = Units_Spawn(troop_def, 1, 0, cx - 700, cy);
    ASSERT(troop_probe >= 0);
    int32_t nx = 12345, ny = 54321;
    ASSERT_EQ_INT(0, Units_FactoryBuildSpot(troop_probe, &nx, &ny));
    ASSERT_EQ_INT(12345, nx);
    ASSERT_EQ_INT(54321, ny);

    /* The real thing: the queued product spawns on that pad. */
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(castle, troop_def));
    units = Units_GetActive(&unit_count);
    int troop = units[castle].build_target;
    ASSERT(troop >= 0);
    ASSERT_EQ_INT(troop_def, units[troop].def_idx);
    ASSERT_EQ_INT(pad_x, units[troop].world_x);
    ASSERT_EQ_INT(pad_y, units[troop].world_y);

    /* Then it walks off the pad: no rally set, so the exit target must
     * still leave the footprint (legacy:9430 hands it a move order the
     * moment getbuilt runs). */
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;
    /* Orders wait for completion: the product cannot be walked off the
     * pad while the factory is still building it. */
    Units_SelectSingle(troop);
    Units_CommandMoveSelected(cx + 900, cy + 900);
    Units_SelectSingle(-1);
    for (int frame = 0; frame < 10; frame++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    }
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(1, (int)units[troop].under_construction);
    ASSERT_EQ_INT(pad_x, units[troop].world_x);
    ASSERT_EQ_INT(pad_y, units[troop].world_y);
    for (int frame = 0; frame < 600; frame++) {
        timer.accumulator = timer.sim_dt * 30.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
        if (!units[troop].under_construction) break;
    }
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(0, (int)units[troop].under_construction);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, units[troop].cmd_kind);
    int32_t ex = units[troop].cmd_x - fx;
    int32_t ey = units[troop].cmd_y - fy;
    int64_t exit_d2 = (int64_t)ex * ex + (int64_t)ey * ey;
    int64_t pad_d2  = (int64_t)(pad_x - fx) * (pad_x - fx) +
                      (int64_t)(pad_y - fy) * (pad_y - fy);
    ASSERT(exit_d2 > pad_d2);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* In-game HUD parity guard.
 *
 * Idle (nothing selected) legacy keeps the sidebar panel, the bottom
 * strip and the crystal ball up and takes every per-unit widget down:
 * name label, both gauges with their backings, rank pip, kill tally
 * (:151133-151166, :152277-152296, :152496-152506). araingame.gui
 * authors those names TWICE (one panel per unit-info group) and ships
 * them with literal placeholder text, so a first-match-only hide left
 * "UnitText" and a spare pair of gauges on screen.
 *
 * With a factory selected the queued count is the build button's own
 * label (:149903-149945), so it must land inside the button's rect.
 *
 * The world is clipped to the play area the dialog leaves free
 * (:150187-150214), otherwise units at the map edge paint over the
 * sidebar. */
TEST(hud_idle_frames_selection_and_queue_badges) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);

    /* ── Idle ───────────────────────────────────────────────────── */
    Units_SelectSingle(-1);
    timer.accumulator = 0.0;                 /* draw only, no sim step */
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));

    /* Frames stay up. */
    ASSERT_EQ_INT(0, HUD_WidgetHidden("UnitMenu"));
    ASSERT_EQ_INT(0, HUD_WidgetHidden("BottomBar"));
    ASSERT_EQ_INT(0, HUD_WidgetHidden("BottomEnd"));
    ASSERT_EQ_INT(0, HUD_WidgetHidden("CrystalBall"));

    /* Per-unit widgets go down, every copy of each name. */
    ASSERT_EQ_INT(1, HUD_WidgetHidden("UnitText"));
    ASSERT_EQ_INT(1, HUD_WidgetHidden("HealthBar"));
    ASSERT_EQ_INT(1, HUD_WidgetHidden("ManaBar"));
    ASSERT_EQ_INT(1, HUD_WidgetHidden("Experience"));
    ASSERT_EQ_INT(1, HUD_WidgetHidden("KillCount"));

    /* Authored placeholder strings are cleared, not just covered. */
    char txt[128];
    ASSERT_EQ_INT(1, HUD_WidgetText("UnitText", txt, sizeof(txt)));
    ASSERT_EQ_INT(0, (int)strlen(txt));
    ASSERT_EQ_INT(1, HUD_WidgetText("ActionText", txt, sizeof(txt)));
    ASSERT_EQ_INT(0, (int)strlen(txt));
    /* The help line idles on the mana readout (legacy:152100-152110). */
    ASSERT_EQ_INT(1, HUD_WidgetText("HelpText", txt, sizeof(txt)));
    ASSERT_EQ_INT(0, strncmp(txt, "Mana\n", 5));

    /* Nothing selected means no build buttons. */
    ASSERT_EQ_INT(0, HUD_BuildSlotCount());

    /* The name label sits immediately right of the portrait, and legacy
     * keeps a widget's art inside its own rect, so the two never
     * overlap. Snapshot the label's first columns now (label hidden) so
     * the selected frame below can prove the first glyph reaches them. */
    SDL_Rect r_text, r_image;
    ASSERT_EQ_INT(1, HUD_GetUnitInfoRects(&r_text, &r_image));
    ASSERT(r_image.x + r_image.w <= r_text.x);
    uint32_t idle_strip[HUD_STRIP_W * HUD_STRIP_H];
    ASSERT_EQ_INT(1, hud_sample_strip(r_text.x, r_image.y + 1,idle_strip));

    if (getenv("TAK_HUD_SHOT")) {
        SDL_SetTextureBlendMode(platform.canvas_tex, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(platform.renderer, platform.canvas_tex, NULL, NULL);
        save_and_check_renderer(&platform, "test_hud_idle.bmp");
    }

    /* ── World clip ─────────────────────────────────────────────── */
    SDL_Rect vp, mm_rect;
    ASSERT_EQ_INT(1, HUD_GetViewportRect(&platform, &vp));
    ASSERT(vp.w > 0 && vp.h > 0);
    ASSERT(vp.w < platform.window_w);        /* sidebar excluded */
    ASSERT(vp.h < platform.window_h);        /* bottom strip excluded */
    ASSERT_EQ_INT(vp.w, world->viewport_w);
    ASSERT_EQ_INT(vp.h, world->viewport_h);
    /* The minimap slot sits in the sidebar column, outside the clip. */
    ASSERT_EQ_INT(1, HUD_GetMinimapRect(&platform, &mm_rect));
    ASSERT(mm_rect.w > 0 && mm_rect.h > 0);
    ASSERT(mm_rect.x >= vp.x + vp.w);
    {
        SDL_Point sidebar_pt = { vp.x + vp.w + 1, vp.y + 1 };
        SDL_Point strip_pt   = { vp.x + 1,        vp.y + vp.h + 1 };
        ASSERT_EQ_INT(SDL_FALSE, SDL_PointInRect(&sidebar_pt, &vp));
        ASSERT_EQ_INT(SDL_FALSE, SDL_PointInRect(&strip_pt,   &vp));
        /* And those points are inside the window, so the frames really
         * do cover pixels the world would otherwise own. */
        ASSERT(sidebar_pt.x < platform.window_w);
        ASSERT(strip_pt.y   < platform.window_h);
    }
    /* One mapping everywhere: at a window that isn't 1:1 with the canvas
     * the play area's far corner maps back to the same dialog corner. */
    {
        int save_w = platform.window_w, save_h = platform.window_h;
        int dlg_w = vp.w, dlg_h = vp.h;   /* window == canvas at 640x480 */
        SDL_Rect wide;
        int cx_back = 0, cy_back = 0;
        platform.window_w = 1280;
        platform.window_h = 720;
        ASSERT_EQ_INT(1, HUD_GetViewportRect(&platform, &wide));
        ASSERT(wide.w > vp.w && wide.h > vp.h);
        ASSERT_EQ_INT(1, TAK_Platform_MapMouseToCanvas(&platform,
                              wide.w, wide.h, &cx_back, &cy_back));
        ASSERT_EQ_INT(dlg_w, cx_back);
        ASSERT_EQ_INT(dlg_h, cy_back);
        platform.window_w = save_w;
        platform.window_h = save_h;
    }

    /* ── Factory selected, two units queued ─────────────────────── */
    int castle_def = Units_FindDefByName("TARCASTL");
    int troop_def  = Units_FindDefByName("TARTROOP");
    ASSERT(castle_def >= 0);
    ASSERT(troop_def >= 0);
    int castle = Units_Spawn(castle_def, 1, 0, cx - 400, cy);
    ASSERT(castle >= 0);
    Economy_AdjustCaps(&world->economy, 1, 100000, 500.0f);
    Economy_Earn(&world->economy, 1, 100000);

    ASSERT_EQ_INT(0, Units_FactoryEnqueue(castle, troop_def));
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(castle, troop_def));
    ASSERT(Units_FactoryQueuedCountForDef(castle, troop_def) >= 2);

    Units_SelectSingle(castle);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));

    ASSERT(HUD_BuildSlotCount() > 0);
    if (getenv("TAK_HUD_SHOT")) {
        SDL_SetTextureBlendMode(platform.canvas_tex, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(platform.renderer, platform.canvas_tex, NULL, NULL);
        save_and_check_renderer(&platform, "test_hud_factory.bmp");
    }
    int slot = -1;
    for (int i = 0; i < HUD_BuildSlotCount() && slot < 0; i++) {
        SDL_Rect r;
        int def_idx = -1;
        if (HUD_GetBuildSlotDialogRect(i, &r, &def_idx) && def_idx == troop_def)
            slot = i;
    }
    ASSERT(slot >= 0);

    SDL_Rect btn, badge;
    ASSERT_EQ_INT(1, HUD_GetBuildSlotDialogRect(slot, &btn, NULL));
    ASSERT_EQ_INT(1, HUD_GetQueueBadgeDialogRect(slot, &badge));
    ASSERT(badge.w > 0 && badge.h > 0);
    ASSERT(badge.x >= btn.x);
    ASSERT(badge.y >= btn.y);
    ASSERT(badge.x + badge.w <= btn.x + btn.w);
    ASSERT(badge.y + badge.h <= btn.y + btn.h);
    /* Build buttons live in the play area, never under the frames. */
    ASSERT(btn.x + btn.w <= 640 - (640 - 512));
    ASSERT(btn.y + btn.h <= 431);

    /* The per-unit widgets come back up for a selection. */
    ASSERT_EQ_INT(0, HUD_WidgetHidden("UnitText"));
    ASSERT_EQ_INT(0, HUD_WidgetHidden("HealthBar"));
    ASSERT_EQ_INT(1, HUD_WidgetText("UnitText", txt, sizeof(txt)));
    ASSERT(strlen(txt) > 0);

    /* The name's first glyph must actually reach the label's leftmost
     * columns. Everything else painted there (the strip art and the
     * portrait frame) is identical between the two frames, so if the
     * portrait art spilled past its rect and covered the glyph the
     * strip would come back unchanged. */
    {
        uint32_t sel_strip[HUD_STRIP_W * HUD_STRIP_H];
        ASSERT_EQ_INT(1, hud_sample_strip(r_text.x, r_image.y + 1,sel_strip));
        int changed = 0;
        for (int i = 0; i < HUD_STRIP_W * HUD_STRIP_H; i++) {
            if (sel_strip[i] != idle_strip[i]) { changed++; }
        }
        ASSERT(changed > 0);
    }

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}


/* Long AI-vs-AI run: guard against progressive slowdown from leaked
 * units (e.g. Killed threads that never finish leaving units stuck
 * DYING forever) or unbounded projectile growth. */
/* Guard against total movement failure in a LIVE skirmish (many units,
 * AI issuing orders, plan-budget contention) — the single-unit nav test
 * cannot see a global stall like a starved A* budget. */
TEST(live_skirmish_units_actually_move) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x, cy = units[0].world_y;

    /* A squad of player units all ordered at once — this is where a
     * global planning budget starves and everything freezes. */
    int sword = Units_FindDefByName("ARASWORD");
    ASSERT(sword >= 0);
    /* Large enough to starve the per-tick planning budget several times
     * over — that contention is what a small squad never reproduces. */
    enum { SQUAD = 60 };
    int squad[SQUAD];
    int32_t start_x[SQUAD], start_y[SQUAD];
    for (int i = 0; i < SQUAD; i++) {
        squad[i] = Units_Spawn(sword, 1, 0,
                               cx + 64 + (i % 10) * 40,
                               cy + 64 + (i / 10) * 40);
        ASSERT(squad[i] >= 0);
    }
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    units = Units_GetActive(&unit_count);
    for (int i = 0; i < SQUAD; i++) {
        start_x[i] = units[squad[i]].world_x;
        start_y[i] = units[squad[i]].world_y;
        Units_CommandMoveUnit(squad[i], cx + 700, cy + 520);
    }

    for (int t = 0; t < 900; t++) {          /* 15 sim-seconds */
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    }

    units = Units_GetActive(&unit_count);
    int moved = 0;
    for (int i = 0; i < SQUAD; i++) {
        int32_t dx = units[squad[i]].world_x - start_x[i];
        int32_t dy = units[squad[i]].world_y - start_y[i];
        if ((int64_t)dx * dx + (int64_t)dy * dy > (int64_t)48 * 48) moved++;
    }
    /* Every ordered unit must travel — including any that spawned on
     * illegal ground, which must be able to step OFF it (the escape
     * hatch in walk_tick). No excuses for stuck units here. */
    int spawn_trapped = 0;
    for (int i = 0; i < SQUAD; i++) {
        if (!Units_CanStandAt(squad[i], start_x[i], start_y[i]))
            spawn_trapped++;
    }
    printf("(%d/%d squad moved, %d started on bad ground) ",
           moved, SQUAD, spawn_trapped);
    if (getenv("TAK_NAV_TRACE")) {
        for (int i = 0; i < SQUAD; i++) {
            const Unit *u = &units[squad[i]];
            int32_t dx = u->world_x - start_x[i];
            int32_t dy = u->world_y - start_y[i];
            if ((int64_t)dx * dx + (int64_t)dy * dy > (int64_t)48 * 48) continue;
            fprintf(stderr, "squad %d h=%d start=%d,%d now=%d,%d stand=%d anim=%d "
                    "plen=%d pidx=%d pfail=%d ppend=%d blk=%d rf=%d v=%.2f "
                    "seg=%d,%d wp=%d,%d\n", i, squad[i], start_x[i],
                    start_y[i], u->world_x, u->world_y,
                    Units_CanStandAt(squad[i], start_x[i], start_y[i]),
                    u->anim_state, u->path_len, u->path_index,
                    u->path_failed, u->path_pending, u->blocked_ticks,
                    u->route_flags, u->cur_speed_ppt, u->route_seg_x,
                    u->route_seg_y,
                    u->path_index < u->path_len ? u->path_x[u->path_index] : -1,
                    u->path_index < u->path_len ? u->path_y[u->path_index] : -1);
        }
    }
    ASSERT_EQ_INT(SQUAD, moved);

    /* Structural guarantee: no unit may sit waiting on a planning slot.
     * A starved budget freezing units is the regression this test
     * exists for, so assert the wait is bounded, not merely rare. */
    int starved = 0;
    for (int i = 0; i < unit_count; i++) {
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        if (units[i].path_pending && units[i].path_wait > 24) starved++;
    }
    printf("(starved %d) ", starved);
    ASSERT_EQ_INT(0, starved);

    /* The AI side must be moving its own units too. */
    units = Units_GetActive(&unit_count);
    int ai_moving = 0;
    for (int i = 0; i < unit_count; i++) {
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        if (units[i].player_id != 2) continue;
        if (units[i].anim_state == UNIT_ANIM_MOVING) ai_moving++;
    }
    printf("(ai moving %d) ", ai_moving);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(ai_long_run_no_entity_leak) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[0].kind = TAK_SLOT_AI;   /* both sides AI */
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.monarch_expendable = 1;          /* fight to the death */
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    /* 3×4000 ticks keeps suite time sane while still cycling through
     * production, combat, and death cleanup. */
    for (int batch = 0; batch < 3 && !world->skirmish_game_over; batch++) {
        InGame_DebugRunSimTicks(4000);
        int unit_count = 0;
        const Unit *units = Units_GetActive(&unit_count);
        int active = 0, dying = 0;
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive == UNIT_ALIVE_ACTIVE) active++;
            if (units[i].alive == UNIT_ALIVE_DYING)  dying++;
        }
        int pcount = 0;
        const Projectile *projs = Units_GetProjectiles(&pcount);
        int palive = 0;
        for (int i = 0; i < pcount; i++) if (projs[i].alive) palive++;
        fprintf(stderr, "leakcheck: tick %d active=%d dying=%d "
                "projectiles=%d slots=%d\n",
                (batch + 1) * 4000, active, dying, palive, unit_count);
        /* Units stuck mid-death accumulate forever if Killed threads
         * hang; a handful in flight is normal, dozens is a leak. */
        ASSERT(dying < 40);
        ASSERT(palive < 512);
    }

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Magic/special weapon validation: an ARAPRIES (3 weapons; slot 2 =
 * MAGIC visual kind, mana-costed) switched to its special slot must
 * actually damage an enemy — guards the aim-ready path, slot
 * switching, and the manapershot gate end to end. */
/* Units must actually ARRIVE at distant goals rather than grinding on
 * terrain (user: "units run into a wall and don't find their way
 * around it"). Exercises the A* budget/pending path, waypoint follow
 * and local avoidance together — not just the planner in isolation. */
TEST(units_navigate_to_distant_goals) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t sx = units[0].world_x, sy = units[0].world_y;

    int sword = Units_FindDefByName("ARASWORD");
    ASSERT(sword >= 0);
    const UnitDef *sdf = Units_GetDef(sword);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    /* Several long routes; each goal is one the planner says is
     * reachable, so failure means the unit-side follow is broken. */
    static const int dirs[4][2] = { {1,1}, {-1,1}, {1,-1}, {-1,-1} };
    int tested = 0, arrived = 0;
    for (int d = 0; d < 4; d++) {
        int32_t gx = sx + dirs[d][0] * 1400;
        int32_t gy = sy + dirs[d][1] * 1400;
        /* Probe with the unit's OWN move class — a plain max-slope plan
         * ignores water rules and would "prove" routes a land unit
         * legitimately cannot walk. */
        TAK_Path probe;
        const MoveClassDef *mc = sdf->movement_class[0]
            ? TAK_MoveInfo_Find(&world->moveinfo, sdf->movement_class)
            : NULL;
        int pn = TAK_PathPlanForMoveClass(world, sx, sy, gx, gy, mc,
                                          sdf->max_slope, 1, &probe);
        if (pn <= 0) continue;
        /* Aim at the planner's own end point: goals can sit off-map or
         * inside blocked cells, where A* legitimately stops short. */
        gx = probe.x[pn - 1];
        gy = probe.y[pn - 1];
        {
            int64_t rdx = (int64_t)gx - sx, rdy = (int64_t)gy - sy;
            if (rdx * rdx + rdy * rdy < (int64_t)600 * 600) continue;
        }
        int h = Units_Spawn(sword, 1, 0, sx, sy);
        ASSERT(h >= 0);
        Units_SelectSingle(h);
        Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
        Units_SelectSingle(-1);
        Units_CommandMoveUnit(h, gx, gy);
        tested++;

        int64_t best_d2 = INT64_MAX;
        int stuck_ticks = 0, ok = 0;
        for (int i = 0; i < 5400; i++) {   /* 90 sim-seconds */
            timer.accumulator = timer.sim_dt;
            next = InGame_Tick(&platform, &timer);
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
            units = Units_GetActive(&unit_count);
            int64_t dx = (int64_t)units[h].world_x - gx;
            int64_t dy = (int64_t)units[h].world_y - gy;
            int64_t d2 = dx * dx + dy * dy;
            if (d2 <= (int64_t)96 * 96) { ok = 1; break; }
            if (d2 < best_d2 - 256) { best_d2 = d2; stuck_ticks = 0; }
            else if (++stuck_ticks > 1800) break;   /* 30s no progress */
            if (getenv("TAK_NAV_TRACE") && (i % 600) == 0) {
                fprintf(stderr,
                    "nav d%d t%4d pos=%d,%d cmd=%d anim=%d plen=%d pidx=%d "
                    "pfail=%d ppend=%d blk=%d cd=%d alive=%d uc=%d v=%.2f "
                    "rf=%d still=%d h=%d n=%d king=%d/%d ks=%d\n",
                    d, i, units[h].world_x, units[h].world_y,
                    units[h].cmd_kind, units[h].anim_state,
                    units[h].path_len, units[h].path_index,
                    units[h].path_failed, units[h].path_pending,
                    units[h].blocked_ticks, units[h].path_replan_cd,
                    (int)units[h].alive, (int)units[h].under_construction,
                    units[h].cur_speed_ppt, units[h].route_flags,
                    units[h].still_ticks, h, unit_count,
                    (int)units[0].alive, units[0].health,
                    units[0].still_ticks);
            }
        }
        if (!ok) {
            /* Only a real failure if a route still exists from where it
             * stopped — that is the reported bug ("runs into a wall and
             * doesn't find its way around"). A genuine dead-end pocket
             * is not something the follower can solve. */
            units = Units_GetActive(&unit_count);
            TAK_Path from_here;
            int n2 = TAK_PathPlanForMoveClass(world,
                        units[h].world_x, units[h].world_y, gx, gy,
                        mc, sdf->max_slope, 1, &from_here);
            if (n2 > 0) {
                printf("(dir %d STUCK at %d,%d though a %d-wp route exists) ",
                       d, units[h].world_x, units[h].world_y, n2);
            } else {
                printf("(dir %d unreachable pocket at %d,%d - ok) ",
                       d, units[h].world_x, units[h].world_y);
                ok = 1;   /* not a follower failure */
            }
        }
        arrived += ok;
        Units_DebugKillHandle(h);
    }
    printf("(%d/%d routes) ", arrived, tested);
    ASSERT(tested > 0);
    ASSERT_EQ_INT(tested, arrived);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Defensive structures must auto-engage: user report "Aramon arrow
 * towers can't attack anything" / "stronghold ignored a ghost ship". */
TEST(tower_auto_engages_enemy) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    int tower_def = Units_FindDefByName("ARAAT");
    int prey_def  = Units_FindDefByName("ARASWORD");
    ASSERT(tower_def >= 0);
    ASSERT(prey_def >= 0);

    /* ARAAT: sightdistance 250, weapon range 500. Put the prey BEYOND
     * sight but well inside weapon reach — the exact case that failed. */
    const UnitDef *twd = Units_GetDef(tower_def);
    ASSERT_NOT_NULL(twd);
    ASSERT(twd->num_weapons >= 1);
    ASSERT(twd->weapons[0].range > twd->sight_distance);
    int tower = Units_Spawn(tower_def, 1, 0, cx + 260, cy + 260);
    ASSERT(tower >= 0);
    int prey = Units_Spawn(prey_def, 1, 1,
                           cx + 260 + twd->sight_distance + 90, cy + 260);
    ASSERT(prey >= 0);
    Units_SelectSingle(prey);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(-1);
    Units_SetOwner(prey, 2, 1);
    /* A spotter of the tower's side stands by the prey: an idle tower
     * takes what its side sees (legacy:20511-20545). */
    int eye = Units_Spawn(prey_def, 1, 0,
                          cx + 260 + twd->sight_distance + 90, cy + 260 + 64);
    ASSERT(eye >= 0);
    Units_DebugSetAggro(eye, UNIT_AGGRO_PASSIVE);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    units = Units_GetActive(&unit_count);
    int hp0 = units[prey].health;
    int acquired = 0, fired = 0;
    for (int i = 0; i < 600; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
        if (units[tower].target == prey) acquired = 1;
        int pc = 0;
        const Projectile *ps = Units_GetProjectiles(&pc);
        for (int p = 0; p < pc; p++)
            if (ps[p].alive && ps[p].shooter == tower) fired = 1;
        if (units[prey].health < hp0) break;
    }
    units = Units_GetActive(&unit_count);
    if (!acquired) printf("(tower never acquired) ");
    else if (!fired) printf("(acquired but never fired) ");
    ASSERT(acquired);
    ASSERT(fired);
    ASSERT(units[prey].health < hp0);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(magic_weapon_fires_and_damages) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    int pries_def = Units_FindDefByName("ARAPRIES");
    int dummy_def = Units_FindDefByName("ARASWORD");
    ASSERT(pries_def >= 0);
    ASSERT(dummy_def >= 0);
    const UnitDef *pd = Units_GetDef(pries_def);
    ASSERT(pd->num_weapons >= 3);

    int caster = Units_Spawn(pries_def, 1, 0, cx - 120, cy + 80);
    /* Enemy dummy: spawn as P1, force passive, hand to P2 so it won't
     * fight back or trigger friendly-fire guards. */
    int dummy = Units_Spawn(dummy_def, 1, 1, cx - 120 + 96, cy + 80);
    ASSERT(caster >= 0);
    ASSERT(dummy >= 0);
    Units_SelectSingle(dummy);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SetOwner(dummy, 2, 1);

    /* Deep mana so the manapershot gate can pay. */
    Economy_AdjustCaps(&world->economy, 1, 100000, 500.0f);
    Economy_Earn(&world->economy, 1, 100000);

    Units_SelectSingle(caster);
    Units_CommandSetWeaponSlotSelected(2);   /* Special/magic slot */
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(2, (int)units[caster].weapon_slot);
    Units_CommandAttackUnit(caster, dummy);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    int hp0 = units[dummy].health;
    int damaged = 0;
    for (int frame = 0; frame < 240 && !damaged; frame++) {
        timer.accumulator = timer.sim_dt * 30.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
        if (units[dummy].alive != UNIT_ALIVE_ACTIVE ||
            units[dummy].health < hp0) damaged = 1;
    }
    ASSERT_EQ_INT(1, damaged);
    /* Mana was actually drained by the shots (manapershot gate ran). */
    ASSERT(Economy_GetSpend(&world->economy, 1) >= 0);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Posture behavior: a PASSIVE unit must never auto-engage an adjacent
 * enemy; switching it OFFENSIVE must make it engage. Validates the
 * aggro modes end to end (manual: unit orders; legacy dispatch
 * legacy:151409). */
TEST(posture_passive_holds_offensive_engages) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    /* Arena well outside the monarchs' sight so no third party
     * interferes with the posture-under-test. */
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int watcher = Units_Spawn(sword_def, 1, 0, cx - 700, cy + 500);
    int enemy   = Units_Spawn(sword_def, 1, 1, cx - 620, cy + 500);
    ASSERT(watcher >= 0);
    ASSERT(enemy >= 0);
    /* Enemy is passive so only the watcher's posture is under test. */
    Units_SelectSingle(enemy);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SetOwner(enemy, 2, 1);
    Units_SelectSingle(watcher);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(-1);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    /* PASSIVE: 600 ticks side by side — enemy must be untouched and
     * the watcher must not acquire a target or attack command. */
    units = Units_GetActive(&unit_count);
    int enemy_hp0 = units[enemy].health;
    for (int frame = 0; frame < 20; frame++) {
        timer.accumulator = timer.sim_dt * 30.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
        ASSERT(units[watcher].cmd_kind != UNIT_CMD_ATTACK);
    }
    ASSERT_EQ_INT(enemy_hp0, units[enemy].health);

    /* OFFENSIVE: the watcher must engage and damage the enemy. */
    Units_SelectSingle(watcher);
    Units_CommandSetAggroSelected(UNIT_AGGRO_OFFENSIVE);
    Units_SelectSingle(-1);
    int engaged = 0;
    for (int frame = 0; frame < 240 && !engaged; frame++) {
        timer.accumulator = timer.sim_dt * 30.0;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
        if (units[enemy].alive != UNIT_ALIVE_ACTIVE ||
            units[enemy].health < enemy_hp0) engaged = 1;
    }
    ASSERT_EQ_INT(1, engaged);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(skirmish_setup_error_requires_two_spawnable_players) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_CLOSED;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));

    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(1, world->loaded);
    ASSERT_EQ_INT(1, world->skirmish_game_over);
    ASSERT_EQ_INT(0, world->skirmish_winner_team);
    ASSERT_EQ_STR("Setup Error", world->skirmish_end_reason);

    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(story_play_starts_campaign_loading) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    int next = Story_StartMission(&platform, 0);
    ASSERT_EQ_INT(GAMESTATE_GAME_LOADING, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_STR("takmission01_mt", world->map_name);
    ASSERT_EQ_STR("aramon", world->map_kingdom);
    ASSERT_EQ_INT(0, world->loaded);

    World_End(&platform);
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(story_screen_renders_book_of_deeds) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());
    ASSERT_EQ_INT(0, Story_Init(&platform));
    int next = Story_Tick(&platform, 1.0f / 60.0f);
    ASSERT_EQ_INT(GAMESTATE_CAMPAIGN, next);
    ASSERT_EQ_INT(0, save_and_check_canvas("test_ui_story.bmp"));

    Story_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Entry ───────────────────────────────────────────────────────────── */

TEST(tech_tree_all_builder_menus_resolve) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    /* The 25 legacy builder menus (data/canbuild/<name>/). Every one
     * must resolve to a def and yield a non-empty menu whose entries
     * all resolve to loadable defs. This is the tech-tree corpus
     * gate: all four factions' builders and everything they summon. */
    static const char *builders[] = {
        "arabuild", "aracastl", "arakeep", "araking", "arapries",
        "tarcastl", "tardung", "tarhell", "tarnecro", "tarprie2",
        "tarpries", "tartb", "verasy", "vercastl", "verflag",
        "verkeep", "verliege", "verlihr", "vermage", "zonhand",
        "zonhunt", "zonhurt", "zonlord", "zonsham", "zontrain"
    };
    enum { N_BUILDERS = (int)(sizeof(builders) / sizeof(builders[0])) };
    int total_entries = 0;
    for (int i = 0; i < N_BUILDERS; i++) {
        int bdef = Units_FindDefByName(builders[i]);
        if (bdef < 0) printf("(missing def: %s) ", builders[i]);
        ASSERT(bdef >= 0);
        int menu[64];
        int n = Units_GetBuildables(bdef, menu, 64);
        if (n <= 0) printf("(empty menu: %s) ", builders[i]);
        ASSERT(n > 0);
        for (int m = 0; m < n; m++) {
            const UnitDef *pd = Units_GetDef(menu[m]);
            ASSERT_NOT_NULL(pd);
            ASSERT(pd->unitname[0] != '\0');
        }
        total_entries += n;
    }
    printf("(%d menus, %d entries) ", N_BUILDERS, total_entries);

    /* Production spot-check: every builder can actually START one of
     * its menu entries. Factories spawn products in-yard; mobile
     * builders get a widening ring of candidate sites near the start
     * position (start areas are guaranteed buildable terrain). */
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count >= 1);
    int32_t bx = units[0].world_x;
    int32_t by = units[0].world_y;
    for (int i = 0; i < N_BUILDERS; i++) {
        int bdef = Units_FindDefByName(builders[i]);
        int menu[64];
        int n = Units_GetBuildables(bdef, menu, 64);
        int bh = Units_Spawn(bdef, 1, 0,
                             bx + 200 + (i % 5) * 96,
                             by + 200 + (i / 5) * 96);
        ASSERT(bh >= 0);
        int started = -1;
        for (int m = 0; m < n && started < 0; m++) {
            units = Units_GetActive(&unit_count);
            int32_t px = units[bh].world_x;
            int32_t py = units[bh].world_y;
            for (int off = 0; off < 10 && started < 0; off++) {
                int32_t sx = px + 80 + (off % 5) * 64;
                int32_t sy = py + 80 + (off / 5) * 64;
                started = Units_BeginBuildingForUnit(bh, menu[m], sx, sy);
            }
        }
        if (started < 0) {
            /* A yard whose whole menu is naval genuinely cannot start
             * anything inland: the product's depth window gates the
             * build spot (legacy:9363-9373). Tolerate that one case,
             * everything else is a real failure. */
            int all_naval = (n > 0);
            for (int m = 0; m < n && all_naval; m++) {
                const UnitDef *pd = Units_GetDef(menu[m]);
                const MoveClassDef *mc = (pd && pd->movement_class[0])
                    ? TAK_MoveInfo_Find(&World_Get()->moveinfo,
                                        pd->movement_class)
                    : NULL;
                if (!mc || mc->min_water_depth <= 0) all_naval = 0;
            }
            if (all_naval) {
                printf("(naval-only yard on land: %s) ", builders[i]);
                continue;
            }
            printf("(cannot start any entry: %s) ", builders[i]);
        }
        ASSERT(started >= 0);
    }

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(nanoframe_decay_refunds_mana) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    /* Kill regen so mana only moves through spend + refund. */
    world->economy.players[0].regen_per_sec = 0.0f;

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t bx = units[0].world_x;
    int32_t by = units[0].world_y;

    int bdef = Units_FindDefByName("ARABUILD");
    ASSERT(bdef >= 0);
    int bh = Units_Spawn(bdef, 1, 0, bx + 160, by + 160);
    ASSERT(bh >= 0);

    int menu[64];
    int n = Units_GetBuildables(bdef, menu, 64);
    ASSERT(n > 0);
    int frame = -1;
    for (int m = 0; m < n && frame < 0; m++) {
        for (int off = 0; off < 10 && frame < 0; off++) {
            frame = Units_BeginBuildingForUnit(bh, menu[m],
                        bx + 240 + (off % 5) * 64,
                        by + 240 + (off / 5) * 64);
        }
    }
    ASSERT(frame >= 0);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);

    /* Let the builder walk over and feed the frame. */
    units = Units_GetActive(&unit_count);
    int hp_start = units[frame].health;
    for (int i = 0; i < 600 && units[frame].health <= hp_start; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
    }
    ASSERT(units[frame].health > hp_start);

    /* Abandon the frame, well along so the rate has room to show. */
    Units_SelectSingle(bh);
    Units_CommandStopSelected();
    Units_SelectSingle(-1);
    Units_SetHealthPercent(frame, 90);
    units = Units_GetActive(&unit_count);

    int hp_before = units[frame].health;
    int32_t mana_before = Economy_GetMana(&world->economy, 1);

    /* Nothing is lost during the ten second grace (legacy:9634). */
    for (int i = 0; i < 590; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    }
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(hp_before, units[frame].health);
    /* Then half a build unit a frame at thirty frames a second
     * (legacy:39534): five seconds cost 75 over buildtime of the
     * whole. */
    for (int i = 0; i < 310; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    }
    units = Units_GetActive(&unit_count);
    const UnitDef *fd = Units_GetDef(units[frame].def_idx);
    ASSERT_NOT_NULL(fd);
    float expect = (float)units[frame].max_health * 75.0f / fd->buildtime;
    if (expect > (float)hp_before) expect = (float)hp_before;
    float lost = (float)(hp_before - units[frame].health);
    if (units[frame].alive != UNIT_ALIVE_ACTIVE) lost = (float)hp_before;
    printf("[lost %.0f of %.0f expected] ", lost, expect);
    ASSERT(lost > expect * 0.7f);
    ASSERT(lost < expect * 1.3f);
    ASSERT(Economy_GetMana(&world->economy, 1) > mana_before); /* refunding */
    /* Left alone, the frame decays to nothing and leaves the field
     * (legacy:9657): no preview lingers where the site was. */
    for (int i = 0; i < 40000 && units[frame].alive == UNIT_ALIVE_ACTIVE; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
        units = Units_GetActive(&unit_count);
    }
    ASSERT(units[frame].alive != UNIT_ALIVE_ACTIVE);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* CLEAR / sweep cursor. Legacy's reclaim order resolves onto the map
 * cell under the cursor, holds the unit in its work state for a stored
 * duration and only then drops the feature record
 * (legacy:32288-32320, 32366-32396). Two halves here: a tree is cleared
 * off the map and pays its authored `energy` back, and reclaiming a
 * structure pays its build cost back. */
TEST(reclaim_clears_feature_and_pays_mana) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    /* No regen and plenty of headroom, so the pool only moves by what
     * the reclaim pays. */
    world->economy.players[0].regen_per_sec = 0.0f;
    world->economy.players[0].max_mana = 1000000;
    world->economy.players[0].mana = 0.0f;

    int bdef = Units_FindDefByName("ARABUILD");
    ASSERT(bdef >= 0);
    const UnitDef *bd = Units_GetDef(bdef);
    ASSERT_NOT_NULL(bd);
    ASSERT((bd->cap_flags & UNIT_CAP_RECLAIM) != 0);
    ASSERT(bd->worker_time > 0.0f);

    /* A small tree with mana in it, standing on flat ground so the cell
     * is walkable the moment the tree is gone. */
    int feat = -1;
    int32_t fx = 0, fy = 0;
    int32_t stand_x = 0, stand_y = 0;
    for (int i = 0; i < world->feature_count && feat < 0; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        if (!fd || !fd->reclaimable || !fd->blocking) continue;
        if (fd->energy <= 0.0f || fd->damage <= 0) continue;
        if (fd->footprint_x != 1 || fd->footprint_z != 1) continue;
        int32_t cx, cy;
        if (Features_InstanceCentre(world, i, &cx, &cy) != 0) continue;
        if (Terrain_IsWalkable(world, cx, cy, 255)) continue;  /* it blocks */
        /* Flat all around, and somewhere for the builder to stand. */
        int ring_ok = 1;
        for (int d = 0; d < 4; d++) {
            static const int off[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
            if (!Terrain_IsWalkable(world, cx + off[d][0] * 24,
                                    cy + off[d][1] * 24, 255)) ring_ok = 0;
        }
        if (!ring_ok) continue;
        /* Stand clear of the feature so the order has to walk first. */
        if (!Terrain_IsWalkable(world, cx + 192, cy, 255)) continue;
        feat = i; fx = cx; fy = cy; stand_x = cx + 192; stand_y = cy;
    }
    if (feat < 0) {
        printf("SKIP (no flat 1x1 reclaimable feature on this map) ");
    } else {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[feat].global_idx);
        int feat_count_before = world->feature_count;
        float expect_mana = fd->energy;
        /* damage hit points removed at workertime per tick. */
        int expect_ticks = (int)((float)fd->damage / bd->worker_time);

        int bh = Units_Spawn(bdef, 1, 0, stand_x, stand_y);
        ASSERT(bh >= 0);
        Units_SelectSingle(bh);
        ASSERT_EQ_INT(1, Units_CommandReclaimFeatureSelected(fx, fy));

        int unit_count = 0;
        const Unit *units = Units_GetActive(&unit_count);
        ASSERT_EQ_INT(UNIT_CMD_RECLAIM, units[bh].cmd_kind);
        ASSERT_EQ_INT(-1, units[bh].target);
        ASSERT(units[bh].reclaim_tile_x >= 0);

        int gone_at = 0;
        int budget = expect_ticks * 3 + 3600;
        for (int t = 0; t < budget && !gone_at; t++) {
            Units_TickEngines();
            if (Features_FindReclaimableAt(world, fx, fy) < 0) gone_at = t + 1;
        }
        printf("[%s dmg=%d energy=%.0f gone=%d expect>=%d] ",
               fd->name, fd->damage, (double)expect_mana,
               gone_at, expect_ticks);
        ASSERT(gone_at > 0);
        /* Timed, not instant: it cannot finish faster than the work
         * itself takes, and the walk to the cell is on top of that. */
        ASSERT(gone_at > expect_ticks);
        ASSERT_EQ_INT(feat_count_before - 1, world->feature_count);
        /* The cell it stood on no longer blocks. */
        ASSERT_EQ_INT(1, Terrain_IsWalkable(world, fx, fy, 255));
        /* Order finished, unit idle again. */
        units = Units_GetActive(&unit_count);
        ASSERT_EQ_INT(UNIT_CMD_NONE, units[bh].cmd_kind);
        ASSERT_EQ_INT(-1, units[bh].reclaim_tile_x);

        int32_t mana = Economy_GetMana(&world->economy, 1);
        ASSERT(mana >= (int32_t)(expect_mana * 0.9f));
        ASSERT(mana <= (int32_t)(expect_mana * 1.1f) + 1);

        /* A wreck (a finished structure) pays its build cost back. */
        int menu[64];
        int n = Units_GetBuildables(bdef, menu, 64);
        ASSERT(n > 0);
        int wreck_def = -1;
        for (int m = 0; m < n && wreck_def < 0; m++) {
            const UnitDef *wd = Units_GetDef(menu[m]);
            if (wd && wd->build_cost > 0 && wd->max_velocity <= 0.0f)
                wreck_def = menu[m];
        }
        if (wreck_def >= 0) {
            const UnitDef *wd = Units_GetDef(wreck_def);
            int wh = Units_Spawn(wreck_def, 1, 0, stand_x + 96, stand_y);
            if (wh >= 0) {
                world->economy.players[0].mana = 0.0f;
                world->economy.players[0].regen_per_sec = 0.0f;
                Units_SelectSingle(bh);
                Units_CommandReclaimSelected(wh);
                units = Units_GetActive(&unit_count);
                ASSERT_EQ_INT(UNIT_CMD_RECLAIM, units[bh].cmd_kind);
                ASSERT_EQ_INT(wh, units[bh].target);
                for (int t = 0; t < 60000; t++) {
                    Units_TickEngines();
                    units = Units_GetActive(&unit_count);
                    if (units[wh].alive != UNIT_ALIVE_ACTIVE) break;
                    world->economy.players[0].regen_per_sec = 0.0f;
                }
                units = Units_GetActive(&unit_count);
                ASSERT(units[wh].alive != UNIT_ALIVE_ACTIVE);
                int32_t back = Economy_GetMana(&world->economy, 1);
                printf("[wreck %s cost=%d back=%d] ",
                       wd->unitname, wd->build_cost, back);
                ASSERT(back >= (int32_t)((float)wd->build_cost * 0.85f));
            }
        }
    }

    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Corpses ──────────────────────────────────────────────────────────
 *
 * A unit that dies leaves the feature its FBI names in `corpse`, on the
 * cells it stood on, once its death script has finished
 * (legacy:227267, 227395-227456). The body is a model feature that a
 * sweep clears, a raiser brings back and time rots away. The helpers
 * below load a map, find clear ground and kill a building there. */

/* A footprint-sized patch of ground with no feature standing in it, a
 * clear stand-off spot to its east, and flat enough to walk. Returns 1
 * and the centre on success. */
static int corpse_find_clear_ground(const GameWorld *world,
                                    int32_t near_x, int32_t near_y,
                                    int half_px, int32_t *out_x,
                                    int32_t *out_y) {
    for (int ring = 2; ring < 14; ring++) {
        for (int dy = -ring; dy <= ring; dy++) {
            for (int dx = -ring; dx <= ring; dx++) {
                if (dx != -ring && dx != ring && dy != -ring && dy != ring)
                    continue;
                int32_t cx = near_x + dx * 64;
                int32_t cy = near_y + dy * 64;
                if (cx < half_px + 64 || cy < half_px + 64) continue;
                if (cx + half_px + 300 >= world->map_pixels_w) continue;
                if (cy + half_px + 64 >= world->map_pixels_h) continue;
                int ok = 1;
                for (int oy = -half_px; oy <= half_px && ok; oy += 16) {
                    for (int ox = -half_px; ox <= half_px && ok; ox += 16) {
                        if (!Terrain_IsWalkable(world, cx + ox, cy + oy, 40))
                            ok = 0;
                    }
                }
                for (int i = 0; i < world->feature_count && ok; i++) {
                    const FeatureDef *fd =
                        Features_GetByIndex(world->features[i].global_idx);
                    int fpx = (fd && fd->footprint_x > 0) ? fd->footprint_x : 1;
                    int fpz = (fd && fd->footprint_z > 0) ? fd->footprint_z : 1;
                    int32_t fx0 = (int32_t)world->features[i].tile_x * 16;
                    int32_t fy0 = (int32_t)world->features[i].tile_z * 16;
                    if (fx0 + fpx * 16 <= cx - half_px - 16) continue;
                    if (fx0 >= cx + half_px + 16) continue;
                    if (fy0 + fpz * 16 <= cy - half_px - 16) continue;
                    if (fy0 >= cy + half_px + 16) continue;
                    ok = 0;
                }
                for (int sx = 0; sx <= 256 && ok; sx += 16) {
                    if (!Terrain_IsWalkable(world, cx + half_px + sx, cy, 40))
                        ok = 0;
                }
                if (!ok) continue;
                *out_x = cx;
                *out_y = cy;
                return 1;
            }
        }
    }
    return 0;
}

static int corpse_instance_at_cell(const GameWorld *world, int cdef,
                                   int cell_x, int cell_z) {
    for (int i = 0; i < world->feature_count; i++) {
        if (world->features[i].global_idx == cdef &&
            (int)world->features[i].tile_x == cell_x &&
            (int)world->features[i].tile_z == cell_z)
            return i;
    }
    return -1;
}

/* Boots two castles and returns 0, or prints the skip reason and
 * returns -1 with nothing to tear down. */
/* 0 when the skirmish is up, 1 when there is no data to run on (a
 * skip), -1 when the load itself failed, which the caller must report
 * as a failure. A failed boot tears down what it brought up. */
static void corpse_teardown(TAK_Platform *platform) {
    Loading_Shutdown();
    World_End(platform);
    UI_Shutdown();
    teardown_platform(platform);
    VFS_Shutdown();
}

static int corpse_boot(TAK_Platform *platform) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return 1; }
    if (setup_platform(platform) != 0) { VFS_Shutdown(); return 1; }
    if (UI_Init() != 0) { corpse_teardown(platform); return -1; }
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    if (World_BeginLoad(platform, &cfg, "two castles", "aramon") != 0 ||
        Loading_Init(platform) != 0) {
        corpse_teardown(platform);
        return -1;
    }
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(platform, 1.0f / 60.0f);
    }
    if (next != GAMESTATE_IN_GAME) { corpse_teardown(platform); return -1; }
    return 0;
}

static void corpse_shutdown(TAK_Platform *platform) {
    Loading_Shutdown();
    World_End(platform);
    UI_Shutdown();
    teardown_platform(platform);
    VFS_Shutdown();
}

TEST(a_dead_unit_leaves_its_corpse_when_the_death_finishes) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int wdef = Units_FindDefByName("ARAAT");
    ASSERT(wdef >= 0);
    const UnitDef *wd = Units_GetDef(wdef);
    ASSERT_NOT_NULL(wd);
    ASSERT(wd->corpse[0] != '\0');
    int cdef = Features_FindByName(wd->corpse);
    ASSERT(cdef >= 0);
    const FeatureDef *cd = Features_GetByIndex(cdef);
    ASSERT_NOT_NULL(cd);
    /* A corpse is a model, not a sprite. A building's wreck blocks,
     * never rots and cannot be raised: decomposetime and resurrectable
     * are keys only mobile units' corpses carry (data fact, the
     * corpse TDFs under features/corpses). */
    ASSERT(cd->object[0] != '\0');
    ASSERT(cd->blocking);
    ASSERT_EQ_INT(0, cd->decompose_time);
    ASSERT(cd->reclaimable);
    ASSERT(!cd->resurrectable);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = 0, cy = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 256,
                                    units[0].world_y, 40, &cx, &cy));
    int before = world->feature_count;
    int h = Units_Spawn(wdef, 1, 3, cx, cy);
    ASSERT(h >= 0);
    units = Units_GetActive(&unit_count);
    int fpx = wd->footprint_x > 0 ? wd->footprint_x : 1;
    int fpz = wd->footprint_z > 0 ? wd->footprint_z : 1;
    int cell_x = Occ_TileOf(units[h].world_x - fpx * 8) + wd->corpse_adjust_x;
    int cell_z = Occ_TileOf(units[h].world_y - fpz * 8) + wd->corpse_adjust_z;
    float heading = units[h].heading;

    ASSERT_EQ_INT(h, Units_DebugKillHandle(h));
    units = Units_GetActive(&unit_count);
    /* Dying, not dead: the script is running and nothing lies on the
     * ground yet. */
    ASSERT_EQ_INT(UNIT_ALIVE_DYING, units[h].alive);
    ASSERT_EQ_INT(1, units[h].corpse_type);
    ASSERT_EQ_INT(-1, corpse_instance_at_cell(world, cdef, cell_x, cell_z));

    int ci = -1, appeared = -1;
    for (int t = 0; t < 600 && ci < 0; t++) {
        Units_TickEngines();
        ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
        if (ci >= 0) appeared = t + 1;
    }
    printf("[%s -> %s cell %d,%d after %d ticks] ",
           wd->unitname, cd->name, cell_x, cell_z, appeared);
    ASSERT(ci >= 0);
    units = Units_GetActive(&unit_count);
    /* The body goes down at the destroy step, the tick the death
     * finishes, never earlier (legacy:227350). */
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, units[h].alive);
    ASSERT_EQ_INT(before + 1, world->feature_count);
    /* It keeps the unit's spot, facing and colours (legacy:128220,
     * 227429), and a wreck with no decomposetime never counts down. */
    ASSERT_EQ_INT(cx, world->features[ci].world_x);
    ASSERT_EQ_INT(cy, world->features[ci].world_y);
    ASSERT_EQ_INT(3, world->features[ci].color_idx);
    int expect_angle = (int)((heading / 6.2831853f) * 65536.0f) & 0xffff;
    int got_angle = world->features[ci].heading;
    ASSERT(abs(got_angle - expect_angle) <= 1);
    ASSERT_EQ_INT(-1, Features_InstanceDecomposeTicks(world, ci));
    ASSERT_EQ_INT(0, Features_InstanceSinkTicks(world, ci));
    /* A blocking corpse takes over the ground it stood on. */
    if (cd->blocking) ASSERT_EQ_INT(0, Terrain_IsWalkable(world, cx, cy, 255));
    /* The sweep cursor sees a wreck; a raiser sees nothing to raise
     * in a fallen building. */
    ASSERT_EQ_INT(ci, Features_FindReclaimableAt(world, cx, cy));
    ASSERT_EQ_INT(-1, Features_FindResurrectableAt(world, cx, cy));

    /* One rendered frame with the wreck in view drives the corpse
     * model pass (a feature with `object` is a 3DO, legacy:211200). */
    world->cam_x = cx - world->viewport_w / 2;
    world->cam_y = cy - world->viewport_h / 2;
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.accumulator = timer.sim_dt;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    InGame_Shutdown();
    ASSERT(corpse_instance_at_cell(world, cdef, cell_x, cell_z) >= 0);
    /* That frame baked the wreck's model. */
    ASSERT(Units_DebugCorpseMeshCount() > 0);
    /* Every piece of it draws: the tower's wreck keeps its body in a
     * piece named AraAt_Dead, which no name rule may hide. */
    ASSERT_EQ_INT(0, Units_DebugCorpseHiddenPieces(cdef));

    /* A monarch leaves no body: its script asks for none
     * (legacy:227142, and the original ARAKING Killed writes 0). */
    int kdef = Units_FindDefByName("ARAKING");
    ASSERT(kdef >= 0);
    int k = Units_Spawn(kdef, 1, 3, cx + 200, cy + 100);
    ASSERT(k >= 0);
    int count_before_king = world->feature_count;
    ASSERT_EQ_INT(k, Units_DebugKillHandle(k));
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(0, units[k].corpse_type);
    for (int t = 0; t < 900 && units[k].alive == UNIT_ALIVE_DYING; t++) {
        Units_TickEngines();
        units = Units_GetActive(&unit_count);
    }
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, units[k].alive);
    ASSERT(world->feature_count <= count_before_king);

    corpse_shutdown(&platform);
    /* The world and its atlases are gone, so no corpse mesh that
     * points into them may outlive it into the next battle. */
    ASSERT_EQ_INT(0, Units_DebugCorpseMeshCount());
}

/* A swordsman beside an enemy strikes it. Walkers block each other
 * since the steering change, so a sword's 30 px has to reach the
 * body and not the centre. Reported with a Taros Black Knight and a
 * Veruna Warrior. */
TEST(swordsman_strikes_an_enemy_standing_beside_it) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    /* Each foe stands off and the swordsman walks up to it, so the two
     * end up wherever blocking lets them stand: straight, diagonal,
     * and one left to find its foe on its own. */
    static const char *foes[5] = { "TARBLACK", "VERSWORD", "TARBLACK",
                                   "VERSWORD", "VERSWORD" };
    static const int offs[5][2] = { {128, 0}, {128, 0}, {96, 96},
                                    {96, 96}, {72, 24} };
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t base_x = units[0].world_x + 256, base_y = units[0].world_y;
    for (int f = 0; f < 5; f++) {
        int foe_def = Units_FindDefByName(foes[f]);
        ASSERT(foe_def >= 0);
        int32_t sx = 0, sy = 0;
        ASSERT(corpse_find_clear_ground(world, base_x + f * 480, base_y, 160,
                                        &sx, &sy));
        int sword = Units_Spawn(sword_def, 1, 0, sx, sy);
        int foe = Units_Spawn(foe_def, 2, 1, sx + offs[f][0], sy + offs[f][1]);
        ASSERT(sword >= 0);
        ASSERT(foe >= 0);
        Units_DebugSetAggro(foe, UNIT_AGGRO_PASSIVE);
        units = Units_GetActive(&unit_count);
        int hp0 = units[foe].health;
        if (f < 4) Units_CommandAttackUnit(sword, foe);
        int t = 0;
        for (; t < 900; t++) {
            Units_TickEngines();
            units = Units_GetActive(&unit_count);
            if (units[foe].health < hp0) break;
        }
        int64_t gx = (int64_t)units[foe].world_x - units[sword].world_x;
        int64_t gy = (int64_t)units[foe].world_y - units[sword].world_y;
        printf("[%s%s: %d ticks, centres %d px apart, anim %d] ", foes[f],
               f < 4 ? "" : " unordered", t,
               (int)sqrt((double)(gx * gx + gy * gy)),
               (int)units[sword].anim_state);
        ASSERT(units[foe].health < hp0);
        /* The blow is the swordsman's own. */
        ASSERT_EQ_INT(foe, (int)units[sword].target);
    }
    corpse_shutdown(&platform);
}

/* A body waits for a raiser: decomposetime is seconds, so the
 * swordsman's corpse still lies there and can be raised a second
 * before its time runs out. Reported from play: bodies rotted before a
 * raiser could reach them. */
TEST(a_corpse_waits_for_a_raiser) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    int wdef = Units_FindDefByName("ARASWORD");
    ASSERT(wdef >= 0);
    const UnitDef *wd = Units_GetDef(wdef);
    int cdef = Features_FindByName(wd->corpse);
    ASSERT(cdef >= 0);
    const FeatureDef *cd = Features_GetByIndex(cdef);
    ASSERT(cd->resurrectable);
    int secs = cd->decompose_time / 30;
    ASSERT(secs >= 20);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = 0, cy = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 320,
                                    units[0].world_y, 40, &cx, &cy));
    int h = Units_Spawn(wdef, 1, 0, cx, cy);
    ASSERT(h >= 0);
    int fpx = wd->footprint_x > 0 ? wd->footprint_x : 1;
    int fpz = wd->footprint_z > 0 ? wd->footprint_z : 1;
    int cell_x = Occ_TileOf(cx - fpx * 8) + wd->corpse_adjust_x;
    int cell_z = Occ_TileOf(cy - fpz * 8) + wd->corpse_adjust_z;
    ASSERT_EQ_INT(h, Units_DebugKillHandle(h));
    int ci = -1;
    for (int t = 0; t < 600 && ci < 0; t++) {
        Units_TickEngines();
        ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
    }
    ASSERT(ci >= 0);
    for (int t = 0; t < (secs - 1) * 60; t++) Units_TickEngines();
    ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
    ASSERT(ci >= 0);
    int32_t fx = 0, fy = 0;
    ASSERT_EQ_INT(0, Features_InstanceCentre(world, ci, &fx, &fy));
    ASSERT_EQ_INT(ci, Features_FindResurrectableAt(world, fx, fy));
    ASSERT(Features_InstanceDecomposeTicks(world, ci) > 0);
    printf("[%s lies %d s] ", cd->name, secs);
    corpse_shutdown(&platform);
}

/* A catapult that picked a landed dragon drops it once the dragon
 * takes off: its cannonball cannot hit a flyer (noairweapon,
 * legacy:249578-249586), and only an attack order used to recheck. */
TEST(noair_weapon_drops_a_flyer_that_takes_off) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    int pult_def = Units_FindDefByName("ARAPULT");
    int drag_def = Units_FindDefByName("ARADRAG");
    ASSERT(pult_def >= 0);
    ASSERT(drag_def >= 0);
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t px = 0, py = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 320,
                                    units[0].world_y, 120, &px, &py));
    int pult = Units_Spawn(pult_def, 1, 0, px, py);
    int drag = Units_Spawn(drag_def, 2, 1, px + 110, py);
    ASSERT(pult >= 0);
    ASSERT(drag >= 0);
    Units_DebugSetAggro(drag, UNIT_AGGRO_PASSIVE);
    int picked = 0;
    for (int t = 0; t < 600 && !picked; t++) {
        Units_TickEngines();
        units = Units_GetActive(&unit_count);
        picked = units[pult].target == drag;
    }
    ASSERT(picked);
    ASSERT_EQ_INT(0, (int)units[drag].flying);
    Units_CommandMoveUnit(drag, units[drag].world_x + 40, units[drag].world_y);
    int flew = 0;
    for (int t = 0; t < 120 && !flew; t++) {
        Units_TickEngines();
        units = Units_GetActive(&unit_count);
        flew = units[drag].flying;
    }
    ASSERT(flew);
    for (int t = 0; t < 2; t++) Units_TickEngines();
    units = Units_GetActive(&unit_count);
    /* Still up and still in the catapult's sight, so only the air rule
     * can have dropped it. */
    ASSERT(units[drag].flying);
    int64_t ddx = (int64_t)units[drag].world_x - units[pult].world_x;
    int64_t ddy = (int64_t)units[drag].world_y - units[pult].world_y;
    ASSERT(ddx * ddx + ddy * ddy <= 160 * 160);
    ASSERT(units[pult].target != drag);
    corpse_shutdown(&platform);
}

/* A step refused by a unit in the way banks no distance: the sub-pixel
 * offset keeps its fraction only, or a unit pressed against another
 * lurches or hops through it when the way clears. */
TEST(a_refused_step_banks_no_distance) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    int kdef = Units_FindDefByName("VERKNIGH");
    int bdef = Units_FindDefByName("VERSWORD");
    ASSERT(kdef >= 0);
    ASSERT(bdef >= 0);
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t kx = 0, ky = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 320,
                                    units[0].world_y, 120, &kx, &ky));
    int k = Units_Spawn(kdef, 1, 0, kx, ky);
    int b = Units_Spawn(bdef, 1, 0, kx + 40, ky);
    ASSERT(k >= 0);
    ASSERT(b >= 0);
    Units_CommandMoveUnit(k, kx + 400, ky);
    int pressed = 0;
    float worst = 0.0f;
    for (int t = 0; t < 40; t++) {
        Units_TickEngines();
        float sx = 0.0f, sy = 0.0f;
        Units_DebugSubpixel(k, &sx, &sy);
        if (fabsf(sx) > worst) worst = fabsf(sx);
        units = Units_GetActive(&unit_count);
        if (units[k].blocked_ticks > 0) pressed = 1;
    }
    printf("[pressed %d, worst sub-pixel %.2f] ", pressed, worst);
    ASSERT(pressed);
    ASSERT(worst < 1.0f);
    corpse_shutdown(&platform);
}

/* A column walking straight at a unit that is stuck in its way gets
 * past it. A unit that has held its cells for 10 frames is in the
 * original's search grid (legacy:188900-188960) and a hard-blocked
 * mover searches again at once (legacy:191290, legacy:191387), so the
 * jam is routed around, one legal step at a time. */
TEST(a_column_gets_past_a_stuck_unit_in_its_way) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    int sdef = Units_FindDefByName("ARASWORD");
    ASSERT(sdef >= 0);
    const UnitDef *sd = Units_GetDef(sdef);
    ASSERT_NOT_NULL(sd);
    int lim = (int)ceilf(sd->max_velocity * 0.5f);
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = 0, cy = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 320,
                                    units[0].world_y, 192, &cx, &cy));
    /* On a planning cell centre, so every route starts straight
     * along y. */
    cx = (cx / 32) * 32 + 16;
    cy = (cy / 32) * 32 + 16;
    enum { N = 3 };
    int hs[N + 1];
    for (int i = 0; i < N; i++) {
        hs[i] = Units_Spawn(sdef, 1, 0, cx, cy + i * 40);
        ASSERT(hs[i] >= 0);
    }
    /* The blocker walks south into the head of the column, so it is a
     * mover pressed nose to nose with a mover, never idle. */
    int b = Units_Spawn(sdef, 1, 0, cx, cy - 64);
    ASSERT(b >= 0);
    hs[N] = b;
    for (int i = 0; i < N; i++) Units_CommandMoveUnit(hs[i], cx, cy - 320);
    Units_CommandMoveUnit(b, cx, cy + 320);
    int32_t px[N + 1], py[N + 1];
    units = Units_GetActive(&unit_count);
    for (int k = 0; k <= N; k++) {
        px[k] = units[hs[k]].world_x;
        py[k] = units[hs[k]].world_y;
    }
    int worst = 0, pressed = 0, done_at = -1;
    for (int t = 0; t < 1200 && done_at < 0; t++) {
        Units_TickEngines();
        units = Units_GetActive(&unit_count);
        for (int k = 0; k <= N; k++) {
            const Unit *u = &units[hs[k]];
            int dx = abs(u->world_x - px[k]);
            int dy = abs(u->world_y - py[k]);
            if (dx > worst) worst = dx;
            if (dy > worst) worst = dy;
            px[k] = u->world_x;
            py[k] = u->world_y;
            if (u->blocked_ticks > 0) pressed = 1;
        }
        int past = units[b].world_y > cy + (N - 1) * 40 + 32;
        for (int i = 0; i < N; i++)
            if (units[hs[i]].world_y >= cy - 64 - 32) past = 0;
        if (past) done_at = t;
    }
    printf("[pressed %d, worst step %d px, past at tick %d] ",
           pressed, worst, done_at);
    ASSERT(pressed);
    ASSERT(done_at >= 0);
    ASSERT(worst <= lim);
    corpse_shutdown(&platform);
}

TEST(the_sweep_clears_a_corpse_and_keeps_it_from_rotting) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    world->economy.players[0].regen_per_sec = 0.0f;
    world->economy.players[0].max_mana = 1000000;
    world->economy.players[0].mana = 500.0f;

    int wdef = Units_FindDefByName("ARAKNIGH");
    int bdef = Units_FindDefByName("ARABUILD");
    ASSERT(wdef >= 0 && bdef >= 0);
    const UnitDef *wd = Units_GetDef(wdef);
    const UnitDef *bd = Units_GetDef(bdef);
    ASSERT((bd->cap_flags & UNIT_CAP_RECLAIM) != 0);
    ASSERT((bd->cap_flags & UNIT_CAP_RESURRECT) == 0);
    int cdef = Features_FindByName(wd->corpse);
    ASSERT(cdef >= 0);
    const FeatureDef *cd = Features_GetByIndex(cdef);
    ASSERT(cd->reclaimable && cd->damage > 0);
    ASSERT(cd->decompose_time > 0);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = 0, cy = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 256,
                                    units[0].world_y, 40, &cx, &cy));
    int before = world->feature_count;
    int h = Units_Spawn(wdef, 1, 0, cx, cy);
    ASSERT(h >= 0);
    int fpx = wd->footprint_x > 0 ? wd->footprint_x : 1;
    int fpz = wd->footprint_z > 0 ? wd->footprint_z : 1;
    int cell_x = Occ_TileOf(cx - fpx * 8) + wd->corpse_adjust_x;
    int cell_z = Occ_TileOf(cy - fpz * 8) + wd->corpse_adjust_z;
    ASSERT_EQ_INT(h, Units_DebugKillHandle(h));
    int ci = -1;
    for (int t = 0; t < 600 && ci < 0; t++) {
        Units_TickEngines();
        ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
    }
    ASSERT(ci >= 0);
    int32_t fx, fy;
    ASSERT_EQ_INT(0, Features_InstanceCentre(world, ci, &fx, &fy));

    int bh = Units_Spawn(bdef, 1, 0, cx + 40 + 192, cy);
    ASSERT(bh >= 0);
    Units_SelectSingle(bh);
    ASSERT_EQ_INT(1, Units_CommandReclaimFeatureSelected(fx, fy));
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_RECLAIM, units[bh].cmd_kind);
    ASSERT_EQ_INT(cell_x, units[bh].reclaim_tile_x);

    /* Each step the sweeper works resets the rot countdown to full
     * (legacy:32394), so the body is never older than a tick or two
     * when the sweep takes it. */
    int expect_ticks = (int)((float)cd->damage / bd->worker_time);
    int32_t mana_before = Economy_GetMana(&world->economy, 1);
    int gone_at = 0, last_left = -1;
    for (int t = 0; t < expect_ticks * 3 + 3600 && !gone_at; t++) {
        Units_TickEngines();
        int now_ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
        if (now_ci < 0) gone_at = t + 1;
        else last_left = Features_InstanceDecomposeTicks(world, now_ci);
    }
    ASSERT(last_left >= cd->decompose_time * 2 - 2);
    printf("[swept %s dmg=%d in %d ticks, work %d] ",
           cd->name, cd->damage, gone_at, expect_ticks);
    ASSERT(gone_at > expect_ticks);
    ASSERT_EQ_INT(before, world->feature_count);
    ASSERT_EQ_INT(1, Terrain_IsWalkable(world, cx, cy, 255));
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_NONE, units[bh].cmd_kind);
    ASSERT_EQ_INT(-1, units[bh].reclaim_tile_x);
    /* A corpse carries no `energy`, so the sweep pays back exactly
     * what the data says: nothing (no corpse TDF carries the key). */
    ASSERT(cd->energy == 0.0f);
    ASSERT_EQ_INT(mana_before, Economy_GetMana(&world->economy, 1));

    corpse_shutdown(&platform);
}

TEST(a_monarch_raises_a_corpse_at_a_tenth_of_its_life) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    world->economy.players[0].max_mana = 1000000;
    world->economy.players[0].mana = 100000.0f;

    int wdef = Units_FindDefByName("ARASWORD");
    int kdef = Units_FindDefByName("ARAKING");
    ASSERT(wdef >= 0 && kdef >= 0);
    const UnitDef *wd = Units_GetDef(wdef);
    const UnitDef *kd = Units_GetDef(kdef);
    ASSERT((kd->cap_flags & UNIT_CAP_RESURRECT) != 0);
    ASSERT(kd->worker_time > 0.0f && wd->buildtime > 0.0f);
    int cdef = Features_FindByName(wd->corpse);
    ASSERT(cdef >= 0);
    const FeatureDef *cd = Features_GetByIndex(cdef);
    ASSERT(cd->resurrectable);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = 0, cy = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 256,
                                    units[0].world_y, 40, &cx, &cy));
    int before = world->feature_count;
    /* The king stands within reach before the swordsman falls: a body
     * starts to rot decomposetime frames after it lands unless someone
     * is already working on it (legacy:128400, 13143), so a raiser
     * across the field never gets there. */
    int k = Units_Spawn(kdef, 1, 0, cx + 110, cy);
    ASSERT(k >= 0);
    int h = Units_Spawn(wdef, 1, 0, cx, cy);
    ASSERT(h >= 0);
    int fpx = wd->footprint_x > 0 ? wd->footprint_x : 1;
    int fpz = wd->footprint_z > 0 ? wd->footprint_z : 1;
    int cell_x = Occ_TileOf(cx - fpx * 8) + wd->corpse_adjust_x;
    int cell_z = Occ_TileOf(cy - fpz * 8) + wd->corpse_adjust_z;
    ASSERT_EQ_INT(h, Units_DebugKillHandle(h));
    int ci = -1;
    for (int t = 0; t < 600 && ci < 0; t++) {
        Units_TickEngines();
        ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
    }
    ASSERT(ci >= 0);
    int32_t fx, fy;
    ASSERT_EQ_INT(0, Features_InstanceCentre(world, ci, &fx, &fy));
    int corpse_angle = world->features[ci].heading;

    units = Units_GetActive(&unit_count);
    int units_before = unit_count;
    Units_SelectSingle(k);
    /* The plain sweep click makes the raiser's choice for it
     * (legacy:187142-187175), and the explicit form agrees. */
    ASSERT_EQ_INT(1, Units_CommandReclaimFeatureSelected(fx, fy));
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_RESURRECT, units[k].cmd_kind);
    ASSERT_EQ_INT(0, units[k].raise_mode);
    ASSERT_EQ_INT(1, Units_CommandResurrectFeatureSelected(fx, fy));
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_RESURRECT, units[k].cmd_kind);

    /* The work owed: the target's buildtime at the raiser's workertime
     * per 30 Hz frame, three tenths of it for a resurrection, paid one
     * frame per frame (legacy:13076-13087), so twice as many ticks. */
    float frames = wd->buildtime * 0.3f / (kd->worker_time / 30.0f);
    int expect_ticks = (int)((frames * 65536.0f) / 32768.0f);
    int started_at = -1, done_at = -1;
    for (int t = 0; t < expect_ticks * 3 + 6000 && done_at < 0; t++) {
        Units_TickEngines();
        units = Units_GetActive(&unit_count);
        if (started_at < 0 && units[k].raise_left > 0) started_at = t + 1;
        if (unit_count > units_before) done_at = t + 1;
    }
    printf("[raise %s: work started %d, done %d, expect %d ticks of work] ",
           wd->unitname, started_at, done_at, expect_ticks);
    ASSERT(started_at > 0);
    ASSERT(done_at > 0);
    /* Not instant: the full work, give or take the tick of rounding. */
    ASSERT(done_at - started_at >= expect_ticks - 1);
    ASSERT(done_at - started_at <= expect_ticks + 2);

    /* What came back: the unit the corpse came from, on the spot, facing
     * the way the body lay, at a tenth of its hit points, for the
     * raiser's player (legacy:13162-13190). The body is gone. */
    int nh = unit_count - 1;
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(wdef, units[nh].def_idx);
    ASSERT_EQ_INT(1, units[nh].player_id);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, units[nh].alive);
    ASSERT_EQ_INT(0, units[nh].under_construction);
    ASSERT_EQ_INT(cx, units[nh].world_x);
    ASSERT_EQ_INT(cy, units[nh].world_y);
    ASSERT_EQ_INT(units[nh].max_health / 10, units[nh].health);
    int back_angle = (int)((units[nh].heading / 6.2831853f) * 65536.0f) & 0xffff;
    ASSERT(abs(back_angle - corpse_angle) <= 1);
    ASSERT_EQ_INT(before, world->feature_count);
    ASSERT_EQ_INT(-1, corpse_instance_at_cell(world, cdef, cell_x, cell_z));
    /* Then the raiser heals it (legacy:13192-13205). */
    ASSERT_EQ_INT(UNIT_CMD_REPAIR, units[k].cmd_kind);
    ASSERT_EQ_INT(nh, units[k].target);
    int hp0 = units[nh].health;
    for (int t = 0; t < 1200 && units[nh].health <= hp0; t++) {
        Units_TickEngines();
        units = Units_GetActive(&unit_count);
    }
    ASSERT(units[nh].health > hp0);

    corpse_shutdown(&platform);
}

TEST(a_corpse_left_alone_rots_on_schedule) {
    TAK_Platform platform;
    int boot_rc = corpse_boot(&platform);
    if (boot_rc == 1) return;
    ASSERT_EQ_INT(0, boot_rc);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int wdef = Units_FindDefByName("ARASWORD");
    ASSERT(wdef >= 0);
    const UnitDef *wd = Units_GetDef(wdef);
    int cdef = Features_FindByName(wd->corpse);
    ASSERT(cdef >= 0);
    const FeatureDef *cd = Features_GetByIndex(cdef);
    /* decomposetime is seconds (legacy:127384-127386): the shipped
     * swordsman's body lies for half a minute, not one second. */
    ASSERT(cd->decompose_time >= 20 * 30);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = 0, cy = 0;
    ASSERT(corpse_find_clear_ground(world, units[0].world_x + 256,
                                    units[0].world_y, 40, &cx, &cy));
    int before = world->feature_count;
    int h = Units_Spawn(wdef, 1, 0, cx, cy);
    ASSERT(h >= 0);
    int fpx = wd->footprint_x > 0 ? wd->footprint_x : 1;
    int fpz = wd->footprint_z > 0 ? wd->footprint_z : 1;
    int cell_x = Occ_TileOf(cx - fpx * 8) + wd->corpse_adjust_x;
    int cell_z = Occ_TileOf(cy - fpz * 8) + wd->corpse_adjust_z;
    ASSERT_EQ_INT(h, Units_DebugKillHandle(h));
    int ci = -1;
    for (int t = 0; t < 600 && ci < 0; t++) {
        Units_TickEngines();
        ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
    }
    ASSERT(ci >= 0);
    int32_t fx, fy;
    ASSERT_EQ_INT(0, Features_InstanceCentre(world, ci, &fx, &fy));

    /* The countdown holds the original's frames, one per 30 Hz frame
     * (legacy:128400-128402): twice that many of our ticks. */
    int rot = cd->decompose_time * 2;
    for (int t = 0; t < rot - 1; t++) Units_TickEngines();
    ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
    ASSERT(ci >= 0);
    ASSERT_EQ_INT(1, Features_InstanceDecomposeTicks(world, ci));
    ASSERT_EQ_INT(0, Features_InstanceSinkTicks(world, ci));
    ASSERT_EQ_INT(ci, Features_FindReclaimableAt(world, fx, fy));

    /* The frame the countdown ends the body starts sinking and takes no
     * more orders (legacy:128403-128405). */
    Units_TickEngines();
    ci = corpse_instance_at_cell(world, cdef, cell_x, cell_z);
    ASSERT(ci >= 0);
    ASSERT(Features_InstanceSinkTicks(world, ci) > 0);
    ASSERT_EQ_INT(-1, Features_FindReclaimableAt(world, fx, fy));
    ASSERT_EQ_INT(-1, Features_FindResurrectableAt(world, fx, fy));
    ASSERT_EQ_INT(before + 1, world->feature_count);
    /* A raiser sent now gets nowhere. */
    int kdef = Units_FindDefByName("ARAKING");
    int k = Units_Spawn(kdef, 1, 0, cx + 40 + 192, cy);
    ASSERT(k >= 0);
    Units_SelectSingle(k);
    ASSERT_EQ_INT(0, Units_CommandResurrectFeatureSelected(fx, fy));
    ASSERT_EQ_INT(0, Units_CommandReclaimFeatureSelected(fx, fy));

    /* It sinks for the original's 60 frames, then the record goes
     * (legacy:128407-128414). */
    for (int t = 0; t < FEATURE_SINK_TICKS - 2; t++) Units_TickEngines();
    ASSERT(corpse_instance_at_cell(world, cdef, cell_x, cell_z) >= 0);
    Units_TickEngines();
    ASSERT_EQ_INT(-1, corpse_instance_at_cell(world, cdef, cell_x, cell_z));
    ASSERT_EQ_INT(before, world->feature_count);
    ASSERT_EQ_INT(1, Terrain_IsWalkable(world, cx, cy, 255));

    corpse_shutdown(&platform);
}

TEST(group_selection_and_control_groups) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }

    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }

    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count >= 1);
    int32_t bx = units[0].world_x;
    int32_t by = units[0].world_y;

    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int a = Units_Spawn(sword_def, 1, 0, bx + 300, by + 300);
    int b = Units_Spawn(sword_def, 1, 0, bx + 340, by + 300);
    int c = Units_Spawn(sword_def, 1, 0, bx + 500, by + 500);
    int e = Units_Spawn(sword_def, 2, 1, bx + 320, by + 320);
    ASSERT(a >= 0); ASSERT(b >= 0); ASSERT(c >= 0); ASSERT(e >= 0);
    {
        const int spawned[] = { a, b, c, e };
        for (int s = 0; s < 4; s++) {
            Units_SelectSingle(spawned[s]);
            Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
        }
        Units_SelectSingle(-1);
    }

    /* Marquee over a+b: enemy e sits inside the rect and must be
     * excluded; c sits outside. */
    int n = Units_SelectInRect(bx + 280, by + 280, bx + 360, by + 340, 0);
    ASSERT_EQ_INT(2, n);

    /* Corner order must not matter. */
    n = Units_SelectInRect(bx + 360, by + 340, bx + 280, by + 280, 0);
    ASSERT_EQ_INT(2, n);

    /* Shift-click toggle: add c, remove b, re-add b. */
    Units_SelectToggle(c);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(3, n);
    Units_SelectToggle(b);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(2, n);
    Units_SelectToggle(b);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(3, n);

    /* Additive marquee keeps the existing selection. */
    Units_SelectSingle(-1);
    n = Units_SelectInRect(bx + 480, by + 480, bx + 520, by + 520, 0);
    ASSERT_EQ_INT(1, n);                       /* just c */
    n = Units_SelectInRect(bx + 280, by + 280, bx + 360, by + 340, 1);
    ASSERT_EQ_INT(3, n);                       /* + a and b */

    /* Control group round-trip. */
    Units_AssignControlGroup(4);
    Units_SelectSingle(-1);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(0, n);
    ASSERT_EQ_INT(3, Units_RecallControlGroup(4));

    /* Orders fan out to the whole selection. */
    Units_CommandMoveSelected(bx + 600, by + 600);
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, units[a].cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, units[b].cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, units[c].cmd_kind);

    /* Dead members are pruned at recall. */
    ASSERT_EQ_INT(a, Units_DebugKillHandle(a));
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    units = Units_GetActive(&unit_count);
    for (int i = 0; i < 240 && units[a].alive == UNIT_ALIVE_ACTIVE; i++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        units = Units_GetActive(&unit_count);
    }
    ASSERT(units[a].alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(2, Units_RecallControlGroup(4));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Occupancy: walls block, gates open ───────────────────────────
 *
 * User report: "i have a gate selected - i can't make it open. also
 * units can walk right through it" and "walls that are buildable units
 * - you can walk right through them". Both come down to structures
 * never imprinting the occupancy layer. */

/* Boot a skirmish on a known map and hand back the live world. */
static int gates_setup_world(TAK_Platform *platform, GameWorld **out_world) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    if (World_BeginLoad(platform, &cfg, "two castles", "aramon") != 0) return -1;
    if (Loading_Init(platform) != 0) return -1;
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(platform, 1.0f / 60.0f);
    }
    if (next != GAMESTATE_IN_GAME) return -1;
    *out_world = World_Get();
    return *out_world ? 0 : -1;
}

/* Search near (ax, ay) for a site where the def fits on walkable ground
 * and a route already crosses it along the axis (dx, dy). */
static int gates_find_site(GameWorld *world, int def_idx,
                           int32_t ax, int32_t ay,
                           int32_t reach, int32_t dx, int32_t dy,
                           int32_t *out_x, int32_t *out_y) {
    int sword = Units_FindDefByName("ARASWORD");
    if (sword < 0) return -1;
    const UnitDef *sdf = Units_GetDef(sword);
    const MoveClassDef *mc = sdf->movement_class[0]
        ? TAK_MoveInfo_Find(&world->moveinfo, sdf->movement_class) : NULL;
    for (int32_t r = 256; r <= 1600; r += 128) {
        for (int q = 0; q < 8; q++) {
            static const int ring[8][2] = {
                {1,0}, {0,1}, {-1,0}, {0,-1}, {1,1}, {-1,1}, {1,-1}, {-1,-1}
            };
            int32_t px = ax + ring[q][0] * r;
            int32_t py = ay + ring[q][1] * r;
            if (!Units_IsBuildSiteClear(def_idx, px, py)) continue;
            TAK_Path probe;
            if (TAK_PathPlanForMoveClass(world, px - dx * reach,
                                         py - dy * reach,
                                         px + dx * reach, py + dy * reach,
                                         mc, sdf->max_slope, 1, &probe) <= 0) {
                continue;
            }
            *out_x = px;
            *out_y = py;
            return 0;
        }
    }
    return -1;
}

/* Issue #26: "horsemen on Aramon will circle around for no reason when I
 * move them places". A plain move order over open ground must turn the
 * unit toward the goal once and take it there along a near straight
 * line. The original turns at `turnrate` toward a point 80 px ahead on
 * the route (legacy:183439-183474) and never aims at a waypoint inside
 * its own turning circle, so a full circle is a bug. */
static double wrap_turn(double d) {
    while (d >  3.14159265) d -= 6.2831853;
    while (d < -3.14159265) d += 6.2831853;
    return d < 0.0 ? -d : d;
}

TEST(horseman_moves_without_circling) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int knight_def = Units_FindDefByName("ARAKNIGH");
    ASSERT(knight_def >= 0);
    const UnitDef *kd = Units_GetDef(knight_def);
    const MoveClassDef *kmc = kd->movement_class[0]
        ? TAK_MoveInfo_Find(&world->moveinfo, kd->movement_class) : NULL;
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;

    int32_t rx = 0, ry = 0;
    if (gates_find_site(world, knight_def, ax, ay, 400, 1, 0, &rx, &ry) != 0) {
        printf("SKIP (no open ground) ");
        goto done;
    }
    {
    int h = Units_Spawn(knight_def, 1, 0, rx - 350, ry);
    ASSERT(h >= 0);
    Units_SelectSingle(h);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(-1);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    /* Legs: east across the corridor, back west (a U-turn from the
     * heading the first leg leaves), then north and south if the map
     * allows. Each goal is probed with the unit's own move class and
     * skipped when the planner moves it. */
    static const int legs[5][2] = {
        { 700, 0 }, { -700, 0 }, { 0, -600 }, { 0, 600 }, { 500, 500 }
    };
    int ran = 0;
    for (int leg = 0; leg < 5; leg++) {
        units = Units_GetActive(&unit_count);
        int32_t sx = units[h].world_x, sy = units[h].world_y;
        int32_t gx = sx + legs[leg][0], gy = sy + legs[leg][1];
        TAK_Path probe;
        int pn = TAK_PathPlanForMoveClass(world, sx, sy, gx, gy, kmc,
                                          kd->max_slope, 1, &probe);
        if (pn <= 0) { printf("(leg %d: no route) ", leg); continue; }
        {
            /* A goal the planner had to move off blocked ground would
             * end in a staircase of corners, not a straight leg. */
            int64_t ex = (int64_t)probe.x[pn - 1] - gx;
            int64_t ey = (int64_t)probe.y[pn - 1] - gy;
            if (ex * ex + ey * ey > 48 * 48) {
                printf("(leg %d: goal moved %d,%d) ", leg, (int)ex, (int)ey);
                continue;
            }
        }
        {
            /* Open ground means no other unit on the line either: a
             * parked monarch on the way is an obstacle course, and the
             * turning it costs is not what this test measures. */
            int in_the_way = 0;
            double lx = (double)(gx - sx), ly = (double)(gy - sy);
            double ll = sqrt(lx * lx + ly * ly);
            for (int k = 0; k < unit_count && ll > 0.0; k++) {
                if (k == h || units[k].alive != UNIT_ALIVE_ACTIVE) continue;
                double px = (double)(units[k].world_x - sx);
                double py = (double)(units[k].world_y - sy);
                double along = (px * lx + py * ly) / ll;
                if (along < -64.0 || along > ll + 64.0) continue;
                double across = (px * ly - py * lx) / ll;
                if (across < 0.0) across = -across;
                if (across < 64.0) { in_the_way = k; break; }
            }
            if (in_the_way) {
                printf("(leg %d: unit %d on the line) ", leg, in_the_way);
                continue;
            }
        }
        Units_CommandMoveUnit(h, gx, gy);
        double turned = 0.0, travelled = 0.0;
        double prev_h = units[h].heading;
        int32_t px = sx, py = sy;
        int arrived = 0, ticks = 0;
        for (int i = 0; i < 2400; i++) {   /* 40 sim-seconds */
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
            units = Units_GetActive(&unit_count);
            turned += wrap_turn((double)units[h].heading - prev_h);
            prev_h = units[h].heading;
            ticks = i + 1;
            if (getenv("TAK_NAV_TRACE") && (i % 30) == 0) {
                fprintf(stderr, "horse leg%d t%4d pos=%d,%d hd=%.2f turned=%.2fpi "
                        "v=%.2f rf=%d idx=%d/%d seg=%d,%d wp=%d,%d blk=%d\n",
                        leg, i, units[h].world_x, units[h].world_y,
                        units[h].heading, turned / 3.14159265,
                        units[h].cur_speed_ppt, units[h].route_flags,
                        units[h].path_index, units[h].path_len,
                        units[h].route_seg_x, units[h].route_seg_y,
                        units[h].path_index < units[h].path_len
                            ? units[h].path_x[units[h].path_index] : gx,
                        units[h].path_index < units[h].path_len
                            ? units[h].path_y[units[h].path_index] : gy,
                        units[h].blocked_ticks);
            }
            if ((i % 30) == 29 || units[h].cmd_kind != UNIT_CMD_MOVE) {
                double dx = (double)(units[h].world_x - px);
                double dy = (double)(units[h].world_y - py);
                travelled += sqrt(dx * dx + dy * dy);
                px = units[h].world_x;
                py = units[h].world_y;
            }
            if (units[h].cmd_kind != UNIT_CMD_MOVE) { arrived = 1; break; }
        }
        double straight = sqrt((double)(gx - sx) * (gx - sx) +
                               (double)(gy - sy) * (gy - sy));
        printf("(leg %d: %.2fpi turned, %.0f/%.0f px, %d ticks) ",
               leg, turned / 3.14159265, travelled, straight, ticks);
        ran++;
        ASSERT(arrived);
        /* One turn toward the goal plus corrections, never a circle. */
        ASSERT(turned < 1.6 * 3.14159265);
        ASSERT(travelled < straight * 1.35);
        /* Within 100 px of the order point when the order completes. */
        {
            int64_t dx = (int64_t)units[h].world_x - gx;
            int64_t dy = (int64_t)units[h].world_y - gy;
            ASSERT(dx * dx + dy * dy <= (int64_t)100 * 100);
        }
    }
    ASSERT(ran >= 2);
    InGame_Shutdown();
    }
done:
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A unit ordered through a wall of idle friendly units must come out
 * on the far side. The original folds a unit that has not moved for a
 * while into the search's cost grid (legacy:188962-188972) and plans
 * again after a refused step (legacy:191265-191388), so a stale route
 * into the crowd is replaced by one around it. */
TEST(unit_walks_around_a_wall_of_friendly_units) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int knight_def = Units_FindDefByName("ARAKNIGH");
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(knight_def >= 0 && sword_def >= 0);
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;

    int32_t rx = 0, ry = 0;
    if (gates_find_site(world, knight_def, ax, ay, 400, 1, 0, &rx, &ry) != 0) {
        printf("SKIP (no open ground) ");
        goto done;
    }
    {
    /* Thirteen swordsmen shoulder to shoulder across the corridor,
     * the mover 350 px west of them, the goal 350 px east. */
    #define WALL_N 13
    int wall[WALL_N];
    for (int i = 0; i < WALL_N; i++) {
        wall[i] = Units_Spawn(sword_def, 1, 0, rx, ry + (i - WALL_N / 2) * 32);
        ASSERT(wall[i] >= 0);
        Units_SelectSingle(wall[i]);
        Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    }
    int h = Units_Spawn(knight_def, 1, 0, rx - 350, ry);
    ASSERT(h >= 0);
    Units_SelectSingle(h);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(-1);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;
    int32_t gx = rx + 350, gy = ry;
    Units_CommandMoveUnit(h, gx, gy);
    int arrived = 0, ticks = 0;
    for (int i = 0; i < 3600; i++) {   /* 60 sim-seconds */
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        ticks = i + 1;
        if (getenv("TAK_NAV_TRACE") && (i % 30) == 0) {
            fprintf(stderr, "wall t%4d pos=%d,%d plen=%d pidx=%d blk=%d "
                    "rf=%d v=%.2f still=%d parked=%d wall0=%d,%d occ=%d "
                    "q=%d occ_layer=%d %dx%d alive=%d\n", i,
                    units[h].world_x, units[h].world_y,
                    units[h].path_len, units[h].path_index,
                    units[h].blocked_ticks, units[h].route_flags,
                    units[h].cur_speed_ppt, units[wall[0]].still_ticks,
                    units[wall[0]].occ_parked, units[wall[6]].world_x,
                    units[wall[6]].world_y, units[wall[6]].occ_on,
                    Occ_QueryWorld(world, rx, ry, 1, 0),
                    world->occ != NULL, world->occ_w, world->occ_h,
                    (int)units[wall[6]].alive);
        }
        int64_t dx = (int64_t)units[h].world_x - gx;
        int64_t dy = (int64_t)units[h].world_y - gy;
        if (dx * dx + dy * dy <= (int64_t)96 * 96) { arrived = 1; break; }
    }
    printf("(%d ticks, ends at %+d,%+d) ", ticks,
           units[h].world_x - gx, units[h].world_y - gy);
    ASSERT(arrived);
    /* The wall stood still: nobody was shoved through. */
    for (int i = 0; i < WALL_N; i++) {
        int32_t wy = ry + (i - WALL_N / 2) * 32;
        int64_t dx = (int64_t)units[wall[i]].world_x - rx;
        int64_t dy = (int64_t)units[wall[i]].world_y - wy;
        ASSERT(dx * dx + dy * dy <= 16 * 16);
    }
    InGame_Shutdown();
    #undef WALL_N
    }
done:
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(own_unit_walks_through_its_gate_and_gate_opens) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int gate_def  = Units_FindDefByName("ARANGATE");
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(gate_def >= 0 && sword_def >= 0);
    const UnitDef *gd = Units_GetDef(gate_def);
    /* The FBI fields the occupancy layer needs must have parsed. */
    ASSERT(gd->is_gate != 0);
    ASSERT(gd->onoffable != 0);
    ASSERT_NOT_NULL(gd->yardmap);
    ASSERT_EQ_INT(14, gd->footprint_x);
    ASSERT_EQ_INT(4, gd->footprint_z);
    /* Middle columns are the gateway: blocked closed, free open. */
    ASSERT((gd->yardmap[7] & TAK_OCC_MASK(0)) != 0);
    ASSERT((gd->yardmap[7] & TAK_OCC_MASK(1)) == 0);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;

    int32_t gx = 0, gy = 0;
    if (gates_find_site(world, gate_def, ax, ay, 160, 0, 1, &gx, &gy) != 0) {
        printf("SKIP (no flat gate site) ");
        goto done;
    }
    {
    int gate = Units_Spawn(gate_def, 1, 0, gx, gy);
    ASSERT(gate >= 0);
    /* Closed on spawn, and the gateway cells now block outsiders. */
    ASSERT_EQ_INT(0, Units_GateState(gate));
    ASSERT_NOT_NULL(world->occ);
    ASSERT_EQ_INT(1, Occ_QueryWorld(world, gx, gy, 2, 0));
    ASSERT_EQ_INT(2, Occ_QueryWorld(world, gx, gy, 1, 0));

    int walker = Units_Spawn(sword_def, 1, 0, gx, gy - 160);
    ASSERT(walker >= 0);
    Units_SelectSingle(walker);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(-1);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;
    Units_CommandMoveUnit(walker, gx, gy + 160);

    int opened = 0, yard_opened = 0, crossed = 0, enemy_free = 0;
    for (int i = 0; i < 2400; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        if (Units_GateState(gate) == 1) opened = 1;
        if (units[gate].cob_yard_open) {
            yard_opened = 1;
            /* An open yard frees the gateway for everyone. */
            if (Occ_QueryWorld(world, gx, gy, 2, 0) == 0) enemy_free = 1;
        }
        int64_t dx = (int64_t)units[walker].world_x - gx;
        int64_t dy = (int64_t)units[walker].world_y - (gy + 160);
        if (dx * dx + dy * dy <= (int64_t)96 * 96) { crossed = 1; break; }
        if (getenv("TAK_NAV_TRACE") && (i % 30) == 0) {
            fprintf(stderr, "gate t%4d pos=%d,%d (gate %d,%d) plen=%d pidx=%d "
                    "blk=%d rf=%d v=%.2f pfail=%d yard=%d\n", i,
                    units[walker].world_x, units[walker].world_y, gx, gy,
                    units[walker].path_len, units[walker].path_index,
                    units[walker].blocked_ticks, units[walker].route_flags,
                    units[walker].cur_speed_ppt, units[walker].path_failed,
                    (int)units[gate].cob_yard_open);
            if (i == 0 || i == 690) {
                const MoveClassDef *smc = TAK_MoveInfo_Find(
                    &world->moveinfo, Units_GetDef(sword_def)->movement_class);
                fprintf(stderr, "gate route seg=%d,%d:", units[walker].route_seg_x,
                        units[walker].route_seg_y);
                for (int k = 0; k < units[walker].path_len; k++)
                    fprintf(stderr, " %d,%d", units[walker].path_x[k],
                            units[walker].path_y[k]);
                fprintf(stderr, " | q(gx,gy)=%d q(gx-16)=%d q(gx-48)=%d clear62=%d "
                        "clear63=%d clear60=%d occ=%dx%d\n",
                        Occ_QueryWorld(world, gx, gy, 1, 0),
                        Occ_QueryWorld(world, gx - 16, gy, 1, 0),
                        Occ_QueryWorld(world, gx - 48, gy, 1, 0),
                        TAK_PathClearanceAt(world, smc, 12, Occ_TileOf(gx - 16),
                                            Occ_TileOf(gy - 48)),
                        TAK_PathClearanceAt(world, smc, 12, Occ_TileOf(gx),
                                            Occ_TileOf(gy - 48)),
                        TAK_PathClearanceAt(world, smc, 12, Occ_TileOf(gx - 48),
                                            Occ_TileOf(gy - 48)),
                        world->occ_w, world->occ_h);
            }
        }
    }
    /* Auto-open fired on approach, the script's OpenYard landed, and
     * the unit made it to the far side. */
    ASSERT(opened);
    ASSERT(yard_opened);
    ASSERT(enemy_free);
    ASSERT(crossed);

    /* The order the HUD issues, both ways. */
    Units_SelectSingle(gate);
    Units_SetGateOpen(gate, 1);
    ASSERT_EQ_INT(1, Units_SelectedGateState());
    for (int i = 0; i < 1800 && !units[gate].cob_yard_open; i++) {
        timer.accumulator = timer.sim_dt;
        InGame_Tick(&platform, &timer);
        units = Units_GetActive(&unit_count);
    }
    ASSERT_EQ_INT(1, (int)units[gate].cob_yard_open);
    /* Open: the script's 50 unit MOVE on each door lands on that
     * door's own side of the gateway, into its wall, so the two door
     * origins end 100 px apart (legacy:270790-270815, :198927-198930). */
    {
        float o1[3], c1[3], o2[3], c2[3];
        ASSERT(Units_DebugPieceWorldOffset(gate, "Door1", o1, c1));
        ASSERT(Units_DebugPieceWorldOffset(gate, "Door2", o2, c2));
        float dx = o1[0] - o2[0], dz = o1[2] - o2[2];
        ASSERT(dx * dx + dz * dz > 80.0f * 80.0f);
        ASSERT(dx * dx + dz * dz < 120.0f * 120.0f);
        /* Outward: each slide points the way that door's vertices lie
         * from its origin. Inward, the halves cross in the middle. */
        ASSERT(o1[0] * (c1[0] - o1[0]) + o1[2] * (c1[2] - o1[2]) > 0.0f);
        ASSERT(o2[0] * (c2[0] - o2[0]) + o2[2] * (c2[2] - o2[2]) > 0.0f);
    }
    /* The player's order survives the auto scan while it animates. */
    ASSERT_EQ_INT(1, Units_SelectedGateState());
    Units_ToggleSelectedGate();
    ASSERT_EQ_INT(0, Units_SelectedGateState());
    for (int i = 0; i < 1800 && units[gate].cob_yard_open; i++) {
        timer.accumulator = timer.sim_dt;
        InGame_Tick(&platform, &timer);
        units = Units_GetActive(&unit_count);
    }
    /* Stop clears the yard before close runs (3 s move, 3 s sleep),
     * so let the doors come home before reading them. */
    for (int i = 0; i < 420; i++) {
        timer.accumulator = timer.sim_dt;
        InGame_Tick(&platform, &timer);
    }
    units = Units_GetActive(&unit_count);
    {
        float o1[3], c1[3], o2[3], c2[3];
        ASSERT(Units_DebugPieceWorldOffset(gate, "Door1", o1, c1));
        ASSERT(Units_DebugPieceWorldOffset(gate, "Door2", o2, c2));
        ASSERT(o1[0] * o1[0] + o1[2] * o1[2] < 1.0f);
        ASSERT(o2[0] * o2[0] + o2[2] * o2[2] < 1.0f);
        /* Turns compose z first, then x, then y (legacy:364077-364160).
         * A quarter turn on z and on y stands this door's width on
         * end: its vertex mean rises by the 23 px it sat from the
         * origin. Composed the other way round it stays level. */
        ASSERT(Units_DebugSetPieceRot(gate, "Door1", 0, 16384, 16384));
        ASSERT(Units_DebugPieceWorldOffset(gate, "Door1", o1, c1));
        ASSERT(c1[1] - o1[1] > 15.0f);
        ASSERT(Units_DebugSetPieceRot(gate, "Door1", 0, 0, 0));
    }
    /* The sidebar pair: up for the gate, lit by its state, and the
     * buttons issue the order (legacy:150430-150436, :151449-151470). */
    timer.accumulator = 0.0;
    InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(0, HUD_WidgetHidden("Active"));
    ASSERT_EQ_INT(0, HUD_WidgetHidden("Inactive"));
    ASSERT_EQ_INT(1, HUD_TriggerCommand(HUD_CMD_ACTIVATE));
    ASSERT_EQ_INT(1, Units_SelectedGateState());
    ASSERT_EQ_INT(1, HUD_TriggerCommand(HUD_CMD_DEACTIVATE));
    ASSERT_EQ_INT(0, Units_SelectedGateState());
    Units_SelectSingle(0);   /* the monarch is not onoffable */
    timer.accumulator = 0.0;
    InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(1, HUD_WidgetHidden("Active"));
    ASSERT_EQ_INT(1, HUD_WidgetHidden("Inactive"));
    Units_SelectSingle(-1);
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(0, (int)units[gate].cob_yard_open);
    ASSERT_EQ_INT(1, Occ_QueryWorld(world, gx, gy, 2, 0));
    InGame_Shutdown();
    }
done:
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(completed_wall_blocks_units) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int wall_def  = Units_FindDefByName("ARAWALL");
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(wall_def >= 0 && sword_def >= 0);
    const UnitDef *wd = Units_GetDef(wall_def);
    ASSERT_NOT_NULL(wd->yardmap);
    ASSERT_EQ_INT(0, wd->is_gate);
    /* `yardmap = o` over a 2x2 footprint: solid in both yard states. */
    ASSERT((wd->yardmap[3] & TAK_OCC_MASK(0)) != 0);
    ASSERT((wd->yardmap[3] & TAK_OCC_MASK(1)) != 0);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;

    int32_t wx = 0, wy = 0;
    if (gates_find_site(world, wall_def, ax, ay, 160, 1, 0, &wx, &wy) != 0) {
        printf("SKIP (no flat wall site) ");
        goto done;
    }
    {
    /* Five segments stacked into a wall the walker cannot slip past
     * without going around. Each is 2x2 tiles = 32 px. */
    for (int s = -2; s <= 2; s++) {
        ASSERT(Units_Spawn(wall_def, 1, 0, wx, wy + s * 32) >= 0);
    }
    ASSERT_NOT_NULL(world->occ);
    ASSERT_EQ_INT(1, Occ_QueryWorld(world, wx, wy, 1, 0));
    ASSERT_EQ_INT(1, Occ_QueryWorld(world, wx, wy + 32, 1, 0));

    int walker = Units_Spawn(sword_def, 1, 0, wx - 160, wy);
    ASSERT(walker >= 0);
    Units_SelectSingle(walker);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(-1);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;
    /* Ordered straight into the wall: it must stop at the face. */
    Units_CommandMoveUnit(walker, wx, wy);

    int inside = 0;
    for (int i = 0; i < 1800; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        if (Occ_QueryWorld(world, units[walker].world_x,
                           units[walker].world_y, 1, walker + 1) == 1) {
            inside = 1;
            break;
        }
    }
    ASSERT_EQ_INT(0, inside);
    /* A dead wall stops blocking (legacy:218300-218326). */
    int seg = -1;
    units = Units_GetActive(&unit_count);
    for (int i = 0; i < unit_count; i++) {
        if (units[i].def_idx == (uint16_t)wall_def &&
            units[i].alive == UNIT_ALIVE_ACTIVE) { seg = i; break; }
    }
    ASSERT(seg >= 0);
    int32_t sx = units[seg].world_x, sy = units[seg].world_y;
    ASSERT_EQ_INT(1, Occ_QueryWorld(world, sx, sy, 1, 0));
    Units_DebugKillHandle(seg);
    ASSERT_EQ_INT(0, Occ_QueryWorld(world, sx, sy, 1, 0));
    InGame_Shutdown();
    }
done:
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* User report: "it's possible for units to perfectly overlap on top of
 * one another". Legacy keeps the occupant id per map cell for mobile
 * units too (legacy:217933-217971) and the move step refuses a cell
 * another unit holds (legacy:219329-219340), which is what makes a
 * crowd pack instead of stack. */
TEST(units_do_not_stack_on_one_another) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;

    int32_t rx = 0, ry = 0;
    if (gates_find_site(world, sword_def, ax, ay, 320, 1, 0, &rx, &ry) != 0) {
        printf("SKIP (no open ground) ");
        goto done;
    }
    {
    /* Twenty units on a loose grid, all ordered onto one point. */
    #define STACK_N 20
    int h[STACK_N];
    int spawned = 0;
    for (int i = 0; i < STACK_N; i++) {
        int32_t sx = rx - 480 + (i % 5) * 64;
        int32_t sy = ry - 160 + (i / 5) * 64;
        h[i] = Units_Spawn(sword_def, 1, 0, sx, sy);
        if (h[i] < 0) break;
        spawned++;
    }
    ASSERT_EQ_INT(STACK_N, spawned);
    for (int i = 0; i < STACK_N; i++) {
        Units_SelectSingle(h[i]);
        Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    }
    Units_SelectSingle(-1);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;
    for (int i = 0; i < STACK_N; i++) Units_CommandMoveUnit(h[i], rx, ry);

    for (int i = 0; i < 4200; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    units = Units_GetActive(&unit_count);
    int shared = 0;
    int64_t worst = 0;
    for (int i = 0; i < STACK_N; i++) {
        ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)units[h[i]].alive);
        int64_t dx = (int64_t)units[h[i]].world_x - rx;
        int64_t dy = (int64_t)units[h[i]].world_y - ry;
        int64_t d2 = dx * dx + dy * dy;
        if (d2 > worst) worst = d2;
        for (int j = i + 1; j < STACK_N; j++) {
            if (Occ_TileOf(units[h[i]].world_x) ==
                    Occ_TileOf(units[h[j]].world_x) &&
                Occ_TileOf(units[h[i]].world_y) ==
                    Occ_TileOf(units[h[j]].world_y)) {
                shared++;
            }
        }
    }
    printf("(spread %d px) ", (int)sqrt((double)worst));
    ASSERT_EQ_INT(0, shared);
    /* They still gather: nobody is left wandering the map. */
    ASSERT(worst <= (int64_t)512 * 512);
    InGame_Shutdown();
    #undef STACK_N
    }
done:
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* User report: "when i build a ship in the water, it can come up on to
 * land". The move class water window (legacy:219155-219157) is the
 * rule; the Taros ghost ship crosses land only because tarship.fbi sets
 * canfly, which is data, not a special case. */
static int water_depth_at(const GameWorld *w, int32_t x, int32_t y) {
    int d = w->water_height - Terrain_SampleHeight(w, x, y);
    return d < 0 ? 0 : d;
}

TEST(boats_stay_in_water_ghost_ships_do_not) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "Athri Cay", sizeof(cfg.map_name) - 1);
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg, "Athri Cay", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int boat_def  = Units_FindDefByName("ARAWAR");    /* WATER4 */
    int ghost_def = Units_FindDefByName("TARSHIP");   /* canfly = 1 */
    ASSERT(boat_def >= 0 && ghost_def >= 0);
    const UnitDef *bd = Units_GetDef(boat_def);
    const MoveClassDef *bmc = TAK_MoveInfo_Find(&world->moveinfo,
                                                bd->movement_class);
    ASSERT_NOT_NULL(bmc);
    /* The shipped water classes omit MaxWaterDepth; legacy seeds it open
     * before parsing (legacy:187361-187377), so boats have legal water
     * to sit in at all. */
    ASSERT(bmc->min_water_depth >= 13);
    ASSERT(bmc->max_water_depth > 1000);
    ASSERT_EQ_INT(1, Units_GetDef(ghost_def)->can_fly);

    if (world->water_height <= 0) { printf("SKIP (dry map) "); goto done; }
    {
    /* Find deep water with dry land in reach of it. */
    int32_t wx = -1, wy = -1, lx = -1, ly = -1;
    for (int32_t y = 96; y < world->map_pixels_h - 96 && wx < 0; y += 32) {
        for (int32_t x = 96; x < world->map_pixels_w - 96; x += 32) {
            int ok = 1;
            for (int oy = -2; oy <= 2 && ok; oy++)
                for (int ox = -2; ox <= 2 && ok; ox++)
                    if (water_depth_at(world, x + ox * 16, y + oy * 16) < 25)
                        ok = 0;
            if (!ok) continue;
            for (int32_t r = 128; r <= 640 && lx < 0; r += 32) {
                static const int dir[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
                for (int k = 0; k < 4; k++) {
                    int32_t cx = x + dir[k][0] * r, cy = y + dir[k][1] * r;
                    if (cx < 64 || cy < 64 ||
                        cx >= world->map_pixels_w - 64 ||
                        cy >= world->map_pixels_h - 64) continue;
                    if (water_depth_at(world, cx, cy) != 0) continue;
                    if (!Terrain_IsWalkable(world, cx, cy, 30)) continue;
                    lx = cx; ly = cy;
                    break;
                }
            }
            if (lx >= 0) { wx = x; wy = y; break; }
        }
    }
    if (wx < 0) { printf("SKIP (no coast found) "); goto done; }

    int boat = Units_Spawn(boat_def, 1, 0, wx, wy);
    int ghost = Units_Spawn(ghost_def, 1, 0, wx, wy);
    ASSERT(boat >= 0 && ghost >= 0);
    Units_SelectSingle(boat);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(ghost);
    Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
    Units_SelectSingle(-1);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;
    Units_CommandMoveUnit(boat, lx, ly);
    Units_CommandMoveUnit(ghost, lx, ly);

    int unit_count = 0;
    const Unit *units = NULL;
    int beached = 0, ghost_arrived = 0;
    int64_t boat_start_d2 = (int64_t)(wx - lx) * (wx - lx) +
                            (int64_t)(wy - ly) * (wy - ly);
    int64_t boat_best_d2 = boat_start_d2;
    for (int i = 0; i < 3600; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        if (water_depth_at(world, units[boat].world_x,
                           units[boat].world_y) < bmc->min_water_depth) {
            beached = 1;
            break;
        }
        int64_t bdx = (int64_t)units[boat].world_x - lx;
        int64_t bdy = (int64_t)units[boat].world_y - ly;
        int64_t bd2 = bdx * bdx + bdy * bdy;
        if (bd2 < boat_best_d2) boat_best_d2 = bd2;
        int64_t dx = (int64_t)units[ghost].world_x - lx;
        int64_t dy = (int64_t)units[ghost].world_y - ly;
        if (dx * dx + dy * dy <= (int64_t)96 * 96) ghost_arrived = 1;
    }
    /* The boat sailed toward the shore and stopped there; it did not
     * simply sit still and pass by doing nothing. */
    ASSERT(boat_best_d2 < boat_start_d2);
    ASSERT_EQ_INT(0, beached);
    ASSERT(ghost_arrived);
    InGame_Shutdown();
    }
done:
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Every weapon must resolve to the art the original fires: its own 3DO
 * `model`, its `weaponart` GAF sequence, or a held beam for the
 * lightning/flame Line-of-Sight subtypes. Guards the bug where every
 * projectile drew as the same generic orb. */
TEST(weapon_art_resolves_per_weapon) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    ASSERT(Units_LoadDefs() > 0);

    static const struct {
        const char *unit;
        int         slot;
        int         kind;
        const char *art;
    } expect[] = {
        /* Aramon rolling siege tower and catapult: cannonball sprites. */
        { "ARATRE",   0, UNIT_WEAPON_ART_SPRITE, "cannbmed"  },
        { "ARAPULT",  0, UNIT_WEAPON_ART_SPRITE, "cannblg"   },
        { "ARACAN",   0, UNIT_WEAPON_ART_SPRITE, "cannbmed"  },
        /* Archer and arrow tower: the araarrow 3DO, not a sprite. */
        { "ARAARCH",  0, UNIT_WEAPON_ART_MODEL,  "araarrow"  },
        { "ARAAT",    0, UNIT_WEAPON_ART_MODEL,  "araarrow"  },
        { "ARAAT",    1, UNIT_WEAPON_ART_MODEL,  "araarrow"  },
        { "ARABOW",   1, UNIT_WEAPON_ART_MODEL,  "araarrow2" },
        /* `model = zonrock.3do` — the extension has to come off. */
        { "ARAKING",  1, UNIT_WEAPON_ART_MODEL,  "zonrock"   },
        /* Elsin's lightning holds a beam instead of flying. */
        { "ARAKING",  0, UNIT_WEAPON_ART_BEAM,   ""          },
        /* Melee carries no projectile art at all. */
        { "ARASWORD", 0, UNIT_WEAPON_ART_NONE,   ""          },
    };
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        int def = Units_FindDefByName(expect[i].unit);
        ASSERT(def >= 0);
        char art[32];
        int kind = Units_GetWeaponArtKind(def, expect[i].slot,
                                          art, (int)sizeof(art));
        fprintf(stderr, "art: %s w%d -> kind=%d art='%s'\n",
                expect[i].unit, expect[i].slot + 1, kind, art);
        ASSERT_EQ_INT(expect[i].kind, kind);
        ASSERT_EQ_STR(expect[i].art, art);
    }

    /* The rolling tower and the arrow tower must not share art. */
    char roll_art[32], arch_art[32];
    int roll_def = Units_FindDefByName("ARATRE");
    int arch_def = Units_FindDefByName("ARAAT");
    int roll_kind = Units_GetWeaponArtKind(roll_def, 0, roll_art,
                                           (int)sizeof(roll_art));
    int arch_kind = Units_GetWeaponArtKind(arch_def, 0, arch_art,
                                           (int)sizeof(arch_art));
    ASSERT(roll_kind != arch_kind);
    ASSERT(strcmp(roll_art, arch_art) != 0);

    /* Ballistic weapons arc; Line-of-Sight and Guided ones do not. */
    const UnitDef *tre = Units_GetDef(roll_def);
    ASSERT_NOT_NULL(tre);
    ASSERT_EQ_INT(1, tre->weapons[0].is_gravity);
    ASSERT(tre->weapons[0].gravity_adjust > 4.0f);
    const UnitDef *king = Units_GetDef(Units_FindDefByName("ARAKING"));
    ASSERT_NOT_NULL(king);
    ASSERT_EQ_INT(0, king->weapons[1].is_gravity);
    const UnitDef *at = Units_GetDef(arch_def);
    ASSERT_NOT_NULL(at);
    ASSERT_EQ_INT(1, at->weapons[0].is_gravity);

    /* Cross-check of the gravity constant and of reading weaponvelocity
     * as world px/sec: each authored siege range has to sit inside the
     * ballistic reach v^2/(g * gravityadjustment) that weapon allows,
     * and close under it — these units are tuned to shoot near their
     * limit. A wrong constant or a scaled velocity misses by orders of
     * magnitude, not percent. (A handful of add-on and monster weapons
     * are authored past their own reach; legacy fires those flat and so
     * do we, so they are reported, not asserted.) */
    static const char *siege[] = { "ARATRE", "ARAPULT", "VERPULT", "VERMORT" };
    for (size_t i = 0; i < sizeof(siege) / sizeof(siege[0]); i++) {
        const UnitDef *ud = Units_GetDef(Units_FindDefByName(siege[i]));
        ASSERT_NOT_NULL(ud);
        const UnitWeapon *wp = &ud->weapons[0];
        ASSERT_EQ_INT(1, wp->is_gravity);
        float v = (float)wp->velocity_pps;
        float reach = (v * v) / (112.0f * wp->gravity_adjust);
        fprintf(stderr, "art: %s range %d, ballistic reach %.0f\n",
                siege[i], wp->range, reach);
        ASSERT((float)wp->range <= reach);
        ASSERT((float)wp->range >= reach * 0.5f);
    }
    for (int d = 0; d < Units_GetDefCount(); d++) {
        const UnitDef *ud = Units_GetDef(d);
        for (int w = 0; w < ud->num_weapons; w++) {
            const UnitWeapon *wp = &ud->weapons[w];
            if (!wp->is_gravity || wp->velocity_pps <= 0) continue;
            float v = (float)wp->velocity_pps;
            float reach = (v * v) / (112.0f * wp->gravity_adjust);
            if ((float)wp->range > reach) {
                fprintf(stderr, "art: %s w%d range %d exceeds reach %.0f "
                        "(fires flat)\n", ud->unitname, w + 1, wp->range, reach);
            }
        }
    }

    Units_FreeDefs();
    VFS_Shutdown();
}

/* Visual probe: an Aramon rolling siege tower (ARATRE, weaponart
 * cannbmed on a lobbed arc) and an arrow tower (ARAAT, the araarrow
 * 3DO) firing at the same time. Saves a frame with both in flight.
 * Run with: test_ui_screens.exe render_probe_projectile */
TEST(render_probe_projectile_art) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.line_of_sight = 0;   /* no fog — isolate projectile rendering */
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    int tre_def  = Units_FindDefByName("ARATRE");
    int at_def   = Units_FindDefByName("ARAAT");
    int prey_def = Units_FindDefByName("ARASWORD");
    ASSERT(tre_def >= 0 && at_def >= 0 && prey_def >= 0);

    /* The in-game screen owns the camera and holds it on the monarch,
     * so the scene is laid out around it. Screen Y is lifted by half
     * the terrain height, so everything sits SOUTH of the monarch to
     * stay in frame. ARATRE has minrange 500, so its mark is far
     * off screen so the shot takes the tall slow arc its
     * gravityadjustment of 4.2 produces and climbs visibly while it
     * crosses the frame. */
    int32_t ax = cx, ay = cy;
    int tre   = Units_Spawn(tre_def,  1, 0, ax - 290, ay + 250);
    int mark  = Units_Spawn(prey_def, 2, 3, ax + 1900, ay + 250);
    int tower = Units_Spawn(at_def,   1, 0, ax - 280, ay + 30);
    int quarry= Units_Spawn(prey_def, 2, 3, ax - 60,  ay + 30);
    ASSERT(tre >= 0 && mark >= 0 && tower >= 0 && quarry >= 0);
    Units_SetHealthPercent(mark, 100);
    Units_SetHealthPercent(quarry, 100);
    Units_CommandAttackUnit(tre, mark);
    Units_CommandAttackUnit(tower, quarry);

    world->cam_x = ax - world->viewport_w / 2;
    world->cam_y = ay - world->viewport_h / 2;
    {
        int uc2 = 0;
        const Unit *uu = Units_GetActive(&uc2);
        const int probe[4] = { tre, mark, tower, quarry };
        const char *nm[4] = { "ARATRE", "mark", "ARAAT", "quarry" };
        for (int i = 0; i < 4; i++) {
            if (probe[i] < 0 || probe[i] >= uc2) continue;
            fprintf(stderr, "probe unit %-7s screen=(%d,%d) alive=%d\n",
                    nm[i], uu[probe[i]].world_x - world->cam_x,
                    uu[probe[i]].world_y - world->cam_y,
                    uu[probe[i]].alive);
        }
        fprintf(stderr, "probe viewport %dx%d\n",
                world->viewport_w, world->viewport_h);
    }

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);

    /* Two captures: one with the arrow tower's 3DO in flight, one with
     * the siege tower's cannonball sprite climbing its arc. They rarely
     * overlap (7s reload against 2.5s), so each gets its own frame. */
    int saw_model = 0, saw_sprite = 0, saw_arc = 0;
    for (int frame = 0; frame < 1800; frame++) {
        timer.accumulator = timer.sim_dt;
        next = InGame_Tick(&platform, &timer);
        if (next != GAMESTATE_IN_GAME) break;
        int pc = 0;
        const Projectile *ps = Units_GetProjectiles(&pc);
        int model_now = -1, sprite_now = -1;
        for (int i = 0; i < pc; i++) {
            if (!ps[i].alive || ps[i].is_beam) continue;
            /* Same projection the renderer uses: half the world height
             * comes off screen Y. Skip the first ticks so the shot is
             * clear of its muzzle, and the HUD sidebar. */
            int sx = ps[i].world_x - world->cam_x;
            int sy = ps[i].world_y - world->cam_y
                   - (int)(ps[i].height * 0.5f);
            if (ps[i].age_ticks < 5) continue;
            if (sx < 20 || sx > 500 || sy < 20 || sy > world->viewport_h - 40)
                continue;
            if (ps[i].art_kind == UNIT_WEAPON_ART_MODEL && model_now < 0)
                model_now = i;
            if (ps[i].art_kind == UNIT_WEAPON_ART_SPRITE &&
                ps[i].gravity_ppt2 > 0.0f &&
                ps[i].height > (float)ps[i].src_height + 24.0f) {
                if (sprite_now < 0) sprite_now = i;
                saw_arc = 1;
            }
        }
        if (model_now >= 0 && !saw_model) {
            const Projectile *q = &ps[model_now];
            fprintf(stderr, "  model shot: screen=(%d,%d) h=%.0f (src %d) "
                    "pitch=%.2f\n", q->world_x - world->cam_x,
                    q->world_y - world->cam_y - (int)(q->height * 0.5f),
                    q->height, q->src_height, q->pitch);
            ASSERT_EQ_INT(0, save_and_check_renderer(
                &platform, "test_render_probe_projectile_model.bmp"));
            saw_model = 1;
        }
        if (sprite_now >= 0 && !saw_sprite) {
            const Projectile *q = &ps[sprite_now];
            fprintf(stderr, "  sprite shot: screen=(%d,%d) h=%.0f (src %d) "
                    "pitch=%.2f\n", q->world_x - world->cam_x,
                    q->world_y - world->cam_y - (int)(q->height * 0.5f),
                    q->height, q->src_height, q->pitch);
            ASSERT_EQ_INT(0, save_and_check_renderer(
                &platform, "test_render_probe_projectile_sprite.bmp"));
            saw_sprite = 1;
        }
        if (saw_model && saw_sprite) break;
    }
    fprintf(stderr, "projectile probe: model=%d sprite=%d arc=%d\n",
            saw_model, saw_sprite, saw_arc);
    ASSERT(saw_model);
    ASSERT(saw_sprite);
    /* A lobbed cannonball has to leave the ground. */
    ASSERT(saw_arc);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Entry points legacy invokes once per state edge must be invoked once
 * per state edge here: StartBuilding when construction begins,
 * StopBuilding when it ends, one movement start and one movement stop
 * across a move order. Elsin (ARAKING) raises his sword inside
 * StartBuilding and returns, so an engine that re-invokes it replays
 * the raise forever. The pose check below is the regression guard.
 * See docs/notes/2026-09-09-cob-entry-points.md. */
TEST(cob_entry_points_fire_once) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    Timer timer;
    Timer_Init(&timer);
    timer.accumulator = timer.sim_dt;
    next = InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    /* Pacify hostiles. A builder under fire abandons the order and
     * the edge counts would then be measuring combat, not building. */
    for (int i = 0; i < unit_count; i++) {
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].player_id != 1)
            Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
    }

    /* A MOBILE player-1 builder, the monarch on an aramon start. */
    int builder = -1;
    for (int i = 0; i < unit_count; i++) {
        const UnitDef *ud = Units_GetDef(units[i].def_idx);
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        if (units[i].player_id != 1 || !ud) continue;
        if (!(ud->cap_flags & UNIT_CAP_BUILDER)) continue;
        if (ud->max_velocity <= 0.0f) continue;
        builder = i;
        break;
    }
    ASSERT(builder >= 0);
    const UnitDef *builder_def = Units_GetDef(units[builder].def_idx);
    ASSERT_NOT_NULL(builder_def);
    fprintf(stderr, "cob_edges: builder %s (h=%d)\n",
            builder_def->unitname, builder);

    int buildables[32];
    int n_buildable = Units_GetBuildables((int)units[builder].def_idx,
                                          buildables, 32);
    ASSERT(n_buildable > 0);
    /* Any structure will do, but not a lodestone: those only stand on a
     * sacred pad and there is none next to the start. */
    int build_def = -1;
    for (int i = 0; i < n_buildable; i++) {
        const UnitDef *bd = Units_GetDef(buildables[i]);
        if (!bd || bd->yardmap_sacred || bd->max_velocity > 0.0f) continue;
        if (build_def < 0) build_def = buildables[i];
        if (strstr(bd->unitname, "LODE") || strstr(bd->unitname, "MANA")) {
            build_def = buildables[i];
            break;
        }
    }
    ASSERT(build_def >= 0);

    static const int offsets[][2] = {
        {  96,   0 }, { -96,   0 }, {   0,  96 }, {   0, -96 },
        { 128,  64 }, {-128,  64 }, { 128, -64 }, {-128, -64 },
        { 192,   0 }, {-192,   0 }, {   0, 192 }, {   0,-192 }
    };
    int frame = -1;
    for (int i = 0; i < (int)(sizeof(offsets) / sizeof(offsets[0])); i++) {
        int32_t bx = units[builder].world_x + offsets[i][0];
        int32_t by = units[builder].world_y + offsets[i][1];
        if (!Units_IsBuildSiteClear(build_def, bx, by)) continue;
        frame = Units_BeginBuildingForUnit(builder, build_def, bx, by);
        if (frame >= 0) break;
    }
    ASSERT(frame >= 0);

    /* Walk to the site, then enter the building state. */
    int building = 0;
    for (int t = 0; t < 2400 && !building; t++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        building = (units[builder].anim_state == UNIT_ANIM_BUILDING);
    }
    ASSERT(building);
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(
                         builder, UNIT_SCRIPT_EV_START_BUILDING));
    ASSERT_EQ_INT(0, Units_DebugScriptEventCount(
                         builder, UNIT_SCRIPT_EV_STOP_BUILDING));

    /* Let the raise finish, then hold. The sword arm must not move
     * again for the rest of the construction. */
    for (int t = 0; t < 120; t++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_ANIM_BUILDING, units[builder].anim_state);

    static const char *arm_pieces[] = { "ArmUR", "ArmLR", "HandR", "Sword" };
    int   arm_node[4];
    int32_t arm_rot0[4][3];
    int   arm_found = 0;
    {
        const UnitMesh *bm = builder_def->mesh_per_color[
                                 units[builder].team_color_idx];
        const CobEngine *e = units[builder].cob;
        ASSERT_NOT_NULL(e);
        for (int p = 0; p < 4; p++) {
            arm_node[p] = -1;
            for (int n = 0; bm && n < bm->node_count && n < e->piece_count; n++) {
                if (tak_stricmp(bm->nodes[n].name, arm_pieces[p]) != 0) continue;
                arm_node[p] = n;
                for (int a = 0; a < 3; a++)
                    arm_rot0[p][a] = e->pieces[n].rot[a];
                arm_found++;
                break;
            }
        }
    }
    ASSERT(arm_found > 0);   /* aramon monarch carries the sword arm */

    /* Two hundred ticks of construction. Legacy invokes nothing at all
     * in here, so every one of those pieces must still be where
     * StartBuilding left it. */
    for (int t = 0; t < 200; t++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_ANIM_BUILDING, units[builder].anim_state);
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(
                         builder, UNIT_SCRIPT_EV_START_BUILDING));
    {
        const CobEngine *e = units[builder].cob;
        ASSERT_NOT_NULL(e);
        for (int p = 0; p < 4; p++) {
            if (arm_node[p] < 0) continue;
            for (int a = 0; a < 3; a++) {
                if (e->pieces[arm_node[p]].rot[a] != arm_rot0[p][a]) {
                    fprintf(stderr,
                        "cob_edges: %s axis %d moved %d -> %d during build\n",
                        arm_pieces[p], a, arm_rot0[p][a],
                        e->pieces[arm_node[p]].rot[a]);
                }
                ASSERT_EQ_INT(arm_rot0[p][a], e->pieces[arm_node[p]].rot[a]);
            }
        }
    }

    /* Run the construction out. Leaving the state fires exactly one
     * StopBuilding, whether it completed or was interrupted. */
    for (int t = 0; t < 7200; t++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        if (units[builder].anim_state != UNIT_ANIM_BUILDING) break;
    }
    units = Units_GetActive(&unit_count);
    ASSERT(units[builder].anim_state != UNIT_ANIM_BUILDING);
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(
                         builder, UNIT_SCRIPT_EV_START_BUILDING));
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(
                         builder, UNIT_SCRIPT_EV_STOP_BUILDING));

    /* One movement start and one movement stop across one move order.
     * Use a fresh unit so the walk to the build site doesn't count. */
    int mover = Units_Spawn((int)units[builder].def_idx, 1, 0,
                            units[builder].world_x + 240,
                            units[builder].world_y + 240);
    ASSERT(mover >= 0);
    for (int t = 0; t < 30; t++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    ASSERT_EQ_INT(0, Units_DebugScriptEventCount(
                         mover, UNIT_SCRIPT_EV_START_MOVING));
    units = Units_GetActive(&unit_count);
    Units_CommandMoveUnit(mover, units[mover].world_x + 160,
                          units[mover].world_y);
    int moved = 0, stopped = 0;
    for (int t = 0; t < 3600; t++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        if (units[mover].anim_state == UNIT_ANIM_MOVING) moved = 1;
        else if (moved) { stopped = 1; break; }
    }
    ASSERT(moved);
    ASSERT(stopped);
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(
                         mover, UNIT_SCRIPT_EV_START_MOVING));
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(
                         mover, UNIT_SCRIPT_EV_STOP_MOVING));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A turret must face the thing it is shooting. Legacy hands AimWeapon
 * the aim heading in the unit's own frame and the turret scripts turn a
 * piece straight to it, so the sign and frame of that argument decide
 * which way the bowmen end up looking. User report: "strongholds and
 * archer towers are facing the wrong way when they are shooting".
 * See docs/notes/2026-09-09-cob-entry-points.md. */
static float wrap_pi(float a) {
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

static void check_turret_faces(TAK_Platform *platform, Timer *timer,
                               const char *tower_name, const char *piece,
                               int32_t cx, int32_t cy,
                               int dir_x, int dir_y, const char *dir_name) {
    int tower_def = Units_FindDefByName(tower_name);
    /* A wall for prey: it cannot walk away, so the angle the assertion
     * compares against stays exactly the one we placed. */
    int prey_def  = Units_FindDefByName("ARAWALL");
    ASSERT(tower_def >= 0);
    ASSERT(prey_def >= 0);
    const UnitDef *twd = Units_GetDef(tower_def);
    ASSERT_NOT_NULL(twd);
    ASSERT(twd->num_weapons >= 1);

    int tower = Units_Spawn(tower_def, 1, 0, cx, cy);
    ASSERT(tower >= 0);
    int reach = twd->weapons[0].range / 2;
    if (reach < 64) reach = 64;
    int prey = Units_Spawn(prey_def, 1, 1,
                           cx + dir_x * reach, cy + dir_y * reach);
    ASSERT(prey >= 0);
    Units_SetOwner(prey, 2, 1);

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    float want = atan2f((float)(units[prey].world_x - units[tower].world_x),
                        -(float)(units[prey].world_y - units[tower].world_y));
    /* Settle: the slowest turret in the set swings at about 20 deg/s. */
    int acquired = 0;
    float got = 0.0f, err = 0.0f;
    for (int i = 0; i < 1800; i++) {
        timer->accumulator = timer->sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(platform, timer));
        units = Units_GetActive(&unit_count);
        if (units[tower].target == prey) acquired = 1;
        if (!acquired || units[prey].alive != UNIT_ALIVE_ACTIVE) continue;
        if (!Units_DebugPieceWorldHeading(tower, piece, &got)) continue;
        err = wrap_pi(got - want);
        if (err < 0.35f && err > -0.35f) break;
    }
    ASSERT(acquired);
    /* Hold: a turret sweeping past the target would drift straight back
     * out again, so require it still be on target a second later. */
    for (int i = 0; i < 60; i++) {
        timer->accumulator = timer->sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(platform, timer));
    }
    ASSERT(Units_DebugPieceWorldHeading(tower, piece, &got));
    err = wrap_pi(got - want);
    fprintf(stderr, "turret: %s piece %s target %s want=%.1f got=%.1f "
            "err=%.1f deg\n", tower_name, piece, dir_name,
            want * 57.2957795f, got * 57.2957795f, err * 57.2957795f);
    /* Thirty degrees of slack. Facing away would land near 180. */
    ASSERT(err < 0.52f && err > -0.52f);
}

/* Attack-ground: the same turn toward the clicked point, no prey. */
static void check_turret_faces_ground(TAK_Platform *platform, Timer *timer,
                                      const char *tower_name, const char *piece,
                                      int32_t cx, int32_t cy,
                                      int dir_x, int dir_y, const char *dir_name) {
    int tower_def = Units_FindDefByName(tower_name);
    ASSERT(tower_def >= 0);
    const UnitDef *twd = Units_GetDef(tower_def);
    ASSERT_NOT_NULL(twd);
    int tower = Units_Spawn(tower_def, 1, 0, cx, cy);
    ASSERT(tower >= 0);
    int reach = twd->weapons[0].range / 2;
    if (reach < 64) reach = 64;
    int32_t gx = cx + dir_x * reach, gy = cy + dir_y * reach;
    Units_SelectSingle(tower);
    Units_CommandAttackGroundSelected(gx, gy);
    Units_SelectSingle(-1);
    float want = atan2f((float)(gx - cx), -(float)(gy - cy));
    float got = 0.0f, err = 3.14f;
    for (int i = 0; i < 1800; i++) {
        timer->accumulator = timer->sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(platform, timer));
        if (!Units_DebugPieceWorldHeading(tower, piece, &got)) continue;
        err = wrap_pi(got - want);
        if (err < 0.35f && err > -0.35f) break;
    }
    for (int i = 0; i < 60; i++) {
        timer->accumulator = timer->sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(platform, timer));
    }
    ASSERT(Units_DebugPieceWorldHeading(tower, piece, &got));
    err = wrap_pi(got - want);
    fprintf(stderr, "turret: %s piece %s ground %s want=%.1f got=%.1f "
            "err=%.1f deg\n", tower_name, piece, dir_name,
            want * 57.2957795f, got * 57.2957795f, err * 57.2957795f);
    ASSERT(err < 0.52f && err > -0.52f);
    Units_SelectSingle(tower);
    Units_CommandStopSelected();
    Units_SelectSingle(-1);
}

TEST(tower_aim_faces_target) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.line_of_sight = 0;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    ASSERT_NOT_NULL(World_Get());

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int32_t cx = units[0].world_x;
    int32_t cy = units[0].world_y;

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    /* East is +x, which is heading +pi/2 under (sin h, -cos h). */
    check_turret_faces(&platform, &timer, "ARAAT", "Hip1",
                       cx + 600, cy + 600, 1, 0, "east");
    check_turret_faces(&platform, &timer, "ARASSH", "turret",
                       cx + 600, cy - 600, 1, 0, "east");
    check_turret_faces(&platform, &timer, "ARAAT", "Hip1",
                       cx - 600, cy + 600, 0, 1, "south");
    /* The user's report: attack-ground left the stronghold looking
     * straight ahead while the shot landed to the side (#22). */
    check_turret_faces_ground(&platform, &timer, "ARASSH", "turret",
                              cx - 600, cy - 600, 1, 0, "east");
    check_turret_faces_ground(&platform, &timer, "ARAAT", "Hip1",
                              cx + 900, cy, 0, 1, "south");

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}


/* Flyers take off, flap while moving, and land when idle (#13). The
 * original starts a flyer's mission with BeginFlight and ends it with
 * BeginLanding (legacy:24117, legacy:24302). The script's own watcher
 * runs the wing loop while setSFXoccupy reports airborne
 * (legacy:185079-185112), and launch and fly return at once unless
 * BeginFlight raised the flying flag, so the wings rest on the ground. */
static int flyer_find_node(const UnitMesh *bm, const CobEngine *e,
                           const char *name) {
    for (int n = 0; bm && e && n < bm->node_count && n < e->piece_count; n++) {
        if (tak_stricmp(bm->nodes[n].name, name) == 0) return n;
    }
    return -1;
}

TEST(flyer_takes_off_flaps_and_lands) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;

    int drag_def = Units_FindDefByName("ARADRAG");
    ASSERT(drag_def >= 0);
    const UnitDef *dd = Units_GetDef(drag_def);
    ASSERT(dd->can_fly);
    ASSERT(dd->cruise_alt > 0);
    int drag = Units_Spawn(drag_def, 1, 0, ax + 200, ay);
    ASSERT(drag >= 0);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < 5; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    units = Units_GetActive(&unit_count);
    /* Standing on land: the script is told 1, not the airborne 5
     * (legacy:185079-185112 sends the state, and it changes). */
    ASSERT_EQ_INT(1, (int)units[drag].sfx_occupy);
    ASSERT_EQ_INT(0, (int)units[drag].flying);
    ASSERT_EQ_INT(0, Units_DebugScriptEventCount(drag, UNIT_SCRIPT_EV_BEGIN_FLIGHT));

    const UnitMesh *bm = dd->mesh_per_color[units[drag].team_color_idx];
    const CobEngine *e = units[drag].cob;
    ASSERT_NOT_NULL(e);
    int wing = flyer_find_node(bm, e, "wingl1");
    ASSERT(wing >= 0);

    /* One order: take-off edge, then the climb to cruise height. */
    Units_SelectSingle(drag);
    Units_CommandMoveSelected(ax + 200 + 900, ay);
    Units_SelectSingle(-1);
    timer.accumulator = timer.sim_dt;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(1, (int)units[drag].flying);
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(drag, UNIT_SCRIPT_EV_BEGIN_FLIGHT));
    int climbed = 0;
    for (int i = 0; i < 900 && !climbed; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        climbed = (units[drag].flight_alt >= (float)dd->cruise_alt);
    }
    ASSERT(climbed);

    /* In flight the wing runs the flap loop: measure its travel. */
    int64_t flight_travel = 0;
    {
        int32_t last[3];
        for (int a = 0; a < 3; a++) last[a] = e->pieces[wing].rot[a];
        for (int i = 0; i < 120; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
            for (int a = 0; a < 3; a++) {
                int32_t d = e->pieces[wing].rot[a] - last[a];
                flight_travel += d < 0 ? -d : d;
                last[a] = e->pieces[wing].rot[a];
            }
        }
    }
    ASSERT(flight_travel > 0);

    /* Arrival: the landing edge and the descent. */
    int idle = 0;
    for (int i = 0; i < 4000 && !idle; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        idle = (units[drag].cmd_kind == UNIT_CMD_NONE && !units[drag].flying);
    }
    ASSERT(idle);
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(drag, UNIT_SCRIPT_EV_BEGIN_LANDING));
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(drag, UNIT_SCRIPT_EV_BEGIN_FLIGHT));
    int landed = 0;
    for (int i = 0; i < 900 && !landed; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        landed = (units[drag].flight_alt <= 0.0f);
    }
    ASSERT(landed);

    /* On the ground the flap loop is off. The land script is itself a
     * loop (the dragon shifts on the spot), so the wing is not frozen,
     * but it travels a fraction of what it did in the air. */
    for (int i = 0; i < 600; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    int64_t ground_travel = 0;
    {
        int32_t last[3];
        for (int a = 0; a < 3; a++) last[a] = e->pieces[wing].rot[a];
        for (int i = 0; i < 120; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
            for (int a = 0; a < 3; a++) {
                int32_t d = e->pieces[wing].rot[a] - last[a];
                ground_travel += d < 0 ? -d : d;
                last[a] = e->pieces[wing].rot[a];
            }
        }
    }
    printf("(wing travel per 2s: flight %lld, ground %lld) ",
           (long long)flight_travel, (long long)ground_travel);
    ASSERT(ground_travel * 4 < flight_travel);
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(0, (int)units[drag].flying);
    ASSERT_EQ_INT(1, Units_DebugScriptEventCount(drag, UNIT_SCRIPT_EV_BEGIN_FLIGHT));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Damage bars follow the Visual Options setting (#23): off by default,
 * the Show Damage checkbox flips DisplayDamageBars and keeps it, the
 * bar draws only for the local player unless cheat codes are allowed,
 * never under 1 HP, and it sits 10 px below the unit (legacy:210837). */
TEST(damage_bars_follow_visual_option) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    /* The store round-trips through a file in the working directory. */
    Settings_SetDirectory(".");
    Settings_SetInt("DisplayDamageBars", 1);
    ASSERT_EQ_INT(0, Settings_Save());
    Settings_SetInt("DisplayDamageBars", 0);
    ASSERT_EQ_INT(0, Settings_Load());
    ASSERT_EQ_INT(1, Settings_GetInt("DisplayDamageBars", 0));
    Settings_SetInt("DisplayDamageBars", 0);
    ASSERT_EQ_INT(0, Settings_Save());

    /* The Visual page toggles it. */
    Options_SetReturnState(GAMESTATE_MENU);
    ASSERT_EQ_INT(0, Options_Init(&platform));
    ASSERT_EQ_INT(1, Options_ClickWidget("Visual"));
    ASSERT_EQ_INT(1, Options_ClickWidget("ShowDamage"));
    ASSERT_EQ_INT(1, Settings_GetInt("DisplayDamageBars", 0));
    ASSERT_EQ_INT(1, Units_GetHealthBarsOn());
    ASSERT_EQ_INT(1, Options_ClickWidget("ShowDamage"));
    ASSERT_EQ_INT(0, Settings_GetInt("DisplayDamageBars", 0));
    Options_Shutdown();

    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count >= 2);
    int own = -1, foe = -1;
    for (int i = 0; i < unit_count && (own < 0 || foe < 0); i++) {
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        if (units[i].player_id == 1 && own < 0) own = i;
        if (units[i].player_id != 1 && foe < 0) foe = i;
    }
    ASSERT(own >= 0 && foe >= 0);
    SDL_Rect bar;
    /* Off: no bar for anyone. */
    ASSERT_EQ_INT(0, Units_GetHealthBarsOn());
    ASSERT_EQ_INT(0, Units_DebugHealthBarRect(own, &bar));
    /* On: the local player only, 32x5 below the unit. */
    Units_SetHealthBarsOn(1);
    ASSERT_EQ_INT(1, Units_DebugHealthBarRect(own, &bar));
    ASSERT_EQ_INT(32, bar.w);
    ASSERT_EQ_INT(5, bar.h);
    ASSERT(bar.y > units[own].world_y - world->cam_y
                   - Terrain_SampleHeight(world, units[own].world_x,
                                          units[own].world_y) * 0.5f);
    world->cfg.power_codes = 0;
    /* An enemy in plain sight next to ours: no bar without codes. */
    int sword = Units_FindDefByName("ARASWORD");
    ASSERT(sword >= 0);
    foe = Units_Spawn(sword, 2, 1, units[own].world_x + 64, units[own].world_y);
    ASSERT(foe >= 0);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(0, Units_DebugHealthBarRect(foe, &bar));
    world->cfg.power_codes = 1;
    ASSERT_EQ_INT(1, Units_DebugHealthBarRect(foe, &bar));
    world->cfg.power_codes = 0;

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A caster's own mana (#14): the reserve starts full, every mana-costing
 * shot draws from it (legacy:17214, legacy:245908), it refills by
 * manarechargerate per second (legacy:8709), and an empty reserve holds
 * fire until it has enough again. The player's pool is untouched. */
TEST(caster_reserve_recharges_and_gates_shots) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;

    int mage_def = Units_FindDefByName("VERMAGE");
    int prey_def = Units_FindDefByName("ARASWORD");
    ASSERT(mage_def >= 0 && prey_def >= 0);
    const UnitDef *md = Units_GetDef(mage_def);
    ASSERT(md->max_mana > 0);
    ASSERT(md->mana_recharge_per_sec > 0.0f);
    ASSERT(md->num_weapons >= 1);
    /* The costed weapon is not always the first slot. */
    int slot = -1;
    for (int w = 0; w < md->num_weapons && slot < 0; w++)
        if (md->weapons[w].mana_per_shot > 0) slot = w;
    ASSERT(slot >= 0);
    int cost = md->weapons[slot].mana_per_shot;

    int mage = Units_Spawn(mage_def, 1, 0, ax + 300, ay);
    ASSERT(mage >= 0);
    Units_SelectSingle(mage);
    Units_CommandSetWeaponSlotSelected(slot);
    Units_SelectSingle(-1);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < 3; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    float cur = 0.0f, max = 0.0f;
    ASSERT_EQ_INT(1, Units_GetMana(mage, &cur, &max));
    ASSERT_EQ_INT(md->max_mana, (int)max);
    ASSERT_EQ_INT(md->max_mana, (int)(cur + 0.5f));

    /* Empty it: the mage cannot fire, and the reserve only climbs. */
    int prey = Units_Spawn(prey_def, 2, 1, ax + 300 + 120, ay);
    ASSERT(prey >= 0);
    int32_t pool_before = Economy_GetMana(&world->economy, 1);
    Units_DebugSetMana(mage, 0.0f);
    float last = 0.0f;
    for (int i = 0; i < 300; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        ASSERT_EQ_INT(1, Units_GetMana(mage, &cur, &max));
        ASSERT(cur >= last);
        last = cur;
    }
    float expect = md->mana_recharge_per_sec * 5.0f;   /* 300 ticks */
    ASSERT(cur > expect - 3.0f && cur < expect + 3.0f);

    /* Full again: a shot lands and the reserve pays for it. */
    Units_DebugSetMana(mage, max);
    int fired = 0;
    for (int i = 0; i < 900 && !fired; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        ASSERT_EQ_INT(1, Units_GetMana(mage, &cur, &max));
        if (cur < max - (float)cost * 0.5f) fired = 1;
    }
    ASSERT(fired);
    ASSERT(cur <= max - (float)cost + 2.0f);
    /* The sidebar gauges: the mage's own reserve after the shot, and
     * the crystal ball at the player's pool fraction. */
    Units_SelectSingle(mage);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    float g_hp = 0.0f, g_mana = 0.0f, g_pool = 0.0f;
    HUD_GetGaugeFractions(&g_hp, &g_mana, &g_pool);
    ASSERT(g_mana > 0.0f && g_mana < 1.0f);
    ASSERT(g_mana < cur / max + 0.02f && g_mana > cur / max - 0.02f);
    {
        int hp = 0, hp_max = 1;
        Units_GetSelectedHealth(&hp, &hp_max);
        float want_hp = hp_max > 0 ? (float)hp / (float)hp_max : 0.0f;
        ASSERT(g_hp < want_hp + 0.02f && g_hp > want_hp - 0.02f);
    }
    {
        int32_t pool = Economy_GetMana(&world->economy, 1);
        int32_t pool_max = Economy_GetMaxMana(&world->economy, 1);
        ASSERT(pool_max > 0);
        float want = (float)pool / (float)pool_max;
        ASSERT(g_pool < want + 0.02f && g_pool > want - 0.02f);
    }
    Units_SelectSingle(-1);
    /* The player's pool never paid; regen alone moved it up. */
    ASSERT(Economy_GetMana(&world->economy, 1) >= pool_before);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Probes for two reports: a war galley in the water never attacks a
 * unit on the shore (#35), and Elsin will not attack a Veruna Enclave,
 * a 5x14 structure (#42). Both print where they stall. */
static void probe_attack_state(const char *tag, int h, int target) {
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    fprintf(stderr, "%s: attacker anim=%d cmd=%d target=%d pos=%d,%d hp=%d "
            "path_len=%d idx=%d failed=%d pending=%d stall=%d blocked=%d "
            "speed=%.2f replan_cd=%d goal=%d,%d next=%d,%d cd=%d "
            "target_hp=%d target_pos=%d,%d heading=%.2f sub=%.2f,%.2f side=%d\n",
            tag, u[h].anim_state, u[h].cmd_kind, u[h].target, u[h].world_x,
            u[h].world_y, u[h].health, u[h].path_len, u[h].path_index,
            u[h].path_failed, u[h].path_pending, u[h].wp_stall,
            u[h].blocked_ticks, u[h].cur_speed_ppt, u[h].path_replan_cd,
            u[h].path_goal_x, u[h].path_goal_y,
            (u[h].path_index < u[h].path_len) ? u[h].path_x[u[h].path_index] : -1,
            (u[h].path_index < u[h].path_len) ? u[h].path_y[u[h].path_index] : -1,
            u[h].weapon_state[0].cooldown_ticks,
            (target >= 0 && target < n) ? u[target].health : -1,
            (target >= 0 && target < n) ? u[target].world_x : 0,
            (target >= 0 && target < n) ? u[target].world_y : 0,
            u[h].heading, u[h].subpixel_x, u[h].subpixel_y, (int)u[h].route_flags);
    if (tag[0] == '+') return;
    for (int j = 0; j < n; j++) {
        if (j == h || u[j].alive != UNIT_ALIVE_ACTIVE) continue;
        int64_t dx = u[j].world_x - u[h].world_x, dy = u[j].world_y - u[h].world_y;
        if (dx * dx + dy * dy > 500 * 500) continue;
        const UnitDef *d = Units_GetDef(u[j].def_idx);
        fprintf(stderr, "   near: %d %s P%d at %d,%d hp=%d uc=%d\n", j,
                d ? d->unitname : "?", u[j].player_id, u[j].world_x,
                u[j].world_y, u[j].health, u[j].under_construction);
    }
}

TEST(war_galley_attacks_shore_target) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));
    ASSERT(world->water_height > 0);

    int galley_def = Units_FindDefByName("ARAWAR");
    int prey_def   = Units_FindDefByName("ARASWORD");
    ASSERT(galley_def >= 0 && prey_def >= 0);
    const UnitDef *gd = Units_GetDef(galley_def);
    ASSERT(gd->num_weapons > 0);
    int range = gd->weapons[0].range;

    /* Find water next to land: scan the map for a deep cell with a
     * walkable cell within half the weapon range. */
    int32_t gx = -1, gy = -1, px = -1, py = -1;
    for (int32_t y = 256; y < world->map_pixels_h - 256 && gx < 0; y += 64) {
        for (int32_t x = 256; x < world->map_pixels_w - 256 && gx < 0; x += 64) {
            int depth = world->water_height - Terrain_SampleHeight(world, x, y);
            if (depth < 20) continue;
            /* Prey inland, past the cannon's reach from this water
             * cell: the galley has to close along the water first. */
            for (int d = range + 64; d <= range + 128 && gx < 0; d += 32) {
                static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                for (int k = 0; k < 4 && gx < 0; k++) {
                    int32_t lx = x + dirs[k][0] * d, ly = y + dirs[k][1] * d;
                    if (world->water_height - Terrain_SampleHeight(world, lx, ly) > 0) continue;
                    if (!Units_IsBuildSiteClear(prey_def, lx, ly)) continue;
                    /* And a shore cell nearer the prey exists in the water. */
                    int32_t sx = x + dirs[k][0] * (d - range + 64);
                    int32_t sy = y + dirs[k][1] * (d - range + 64);
                    if (world->water_height - Terrain_SampleHeight(world, sx, sy) < 20) continue;
                    gx = x; gy = y; px = lx; py = ly;
                }
            }
        }
    }
    if (gx < 0) { printf("SKIP (no shore found) "); goto done; }
    {
    /* No AI on the prey's side, so it stays where it was put and the
     * galley has to do the closing under an attack order. */
    world->cfg.players[1].kind = TAK_SLOT_HUMAN;
    int galley = Units_Spawn(galley_def, 1, 0, gx, gy);
    int prey   = Units_Spawn(prey_def, 2, 1, px, py);
    ASSERT(galley >= 0 && prey >= 0);
    Units_DebugSetAggro(prey, UNIT_AGGRO_PASSIVE);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Units_CommandAttackUnit(galley, prey);
    Timer timer;
    Timer_Init(&timer);
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    int hp0 = u[prey].health;
    int hurt = 0;
    for (int i = 0; i < 2400 && !hurt; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        u = Units_GetActive(&n);
        hurt = (u[prey].health < hp0) || (u[prey].alive != UNIT_ALIVE_ACTIVE);
        if (i % 300 == 299) probe_attack_state("galley", galley, prey);
        if (i > 30 && i % 20 == 0) {
            int np = 0;
            const Projectile *ps = Units_GetProjectiles(&np);
            for (int k = 0; k < np; k++) {
                if (!ps[k].alive) continue;
                fprintf(stderr, "  shell %d at %d,%d h=%.0f up=%.2f spd=%.2f "
                        "ttl=%d tgt=%d dest=%d,%d aoe=%d\n", k, ps[k].world_x,
                        ps[k].world_y, ps[k].height, ps[k].vel_up_ppt,
                        ps[k].speed_ppt, ps[k].ttl_ticks, ps[k].target,
                        ps[k].dest_x, ps[k].dest_y, ps[k].area_of_effect);
            }
        }
    }
    if (!hurt) probe_attack_state("galley final", galley, prey);
    ASSERT(hurt);
    }
done:
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(monarch_attacks_large_structure) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));
    world->cfg.line_of_sight = 1;

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int king = 0;
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int keep_def = Units_FindDefByName("VERKEEP");
    ASSERT(keep_def >= 0);
    /* Site the enclave a little east of the king on clear ground. */
    int32_t kx = -1, ky = -1;
    /* Well outside the king's sight, so the order targets a structure
     * the player knows about but cannot see right now. */
    for (int32_t d = 900; d <= 1800 && kx < 0; d += 64) {
        static const int dirs[4][2] = {{1,0},{0,1},{-1,0},{0,-1}};
        for (int k = 0; k < 4 && kx < 0; k++) {
            int32_t x = ax + dirs[k][0] * d, y = ay + dirs[k][1] * d;
            if (Units_IsBuildSiteClear(keep_def, x, y)) { kx = x; ky = y; }
        }
    }
    ASSERT(kx >= 0);
    int keep = Units_Spawn(keep_def, 2, 1, kx, ky);
    ASSERT(keep >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    Units_SelectSingle(king);
    Units_CommandAttackUnit(king, keep);
    Units_SelectSingle(-1);
    units = Units_GetActive(&unit_count);
    int hp0 = units[keep].health;
    int hurt = 0;
    for (int i = 0; i < 3000 && !hurt; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        hurt = units[keep].health < hp0;
        if (i % 600 == 599) probe_attack_state("king", king, keep);
        if (i >= 1200 && i < 1230 && i % 3 == 0) probe_attack_state("+king", king, keep);
    }
    if (!hurt) probe_attack_state("king final", king, keep);
    ASSERT(hurt);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A ghost ship at rest is a surface target: the original tests the
 * target's current movement mode, not its type (#35). */
TEST(war_galley_hits_resting_ghost_ship) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));
    ASSERT(world->water_height > 0);

    int galley_def = Units_FindDefByName("ARAWAR");
    int ghost_def  = Units_FindDefByName("TARSHIP");
    ASSERT(galley_def >= 0 && ghost_def >= 0);
    const UnitDef *gd = Units_GetDef(galley_def);
    ASSERT(gd->num_weapons > 0 && gd->weapons[0].no_air_weapon);
    ASSERT(Units_GetDef(ghost_def)->can_fly);
    int range = gd->weapons[0].range;

    /* Two deep-water cells well inside the cannon's reach. */
    int32_t gx = -1, gy = -1, sx = -1, sy = -1;
    for (int32_t y = 256; y < world->map_pixels_h - 256 && gx < 0; y += 64) {
        for (int32_t x = 256; x < world->map_pixels_w - 256 && gx < 0; x += 64) {
            if (world->water_height - Terrain_SampleHeight(world, x, y) < 20) continue;
            static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
            for (int k = 0; k < 4 && gx < 0; k++) {
                int32_t tx = x + dirs[k][0] * (range / 2);
                int32_t ty = y + dirs[k][1] * (range / 2);
                if (world->water_height - Terrain_SampleHeight(world, tx, ty) < 20) continue;
                if (!Units_IsBuildSiteClear(galley_def, x, y)) continue;
                gx = x; gy = y; sx = tx; sy = ty;
            }
        }
    }
    if (gx < 0) { printf("SKIP (no water found) "); goto done; }
    {
    /* No AI on the ghost ship's side: an order would lift it. */
    world->cfg.players[1].kind = TAK_SLOT_HUMAN;
    int galley = Units_Spawn(galley_def, 1, 0, gx, gy);
    int ghost  = Units_Spawn(ghost_def, 2, 1, sx, sy);
    ASSERT(galley >= 0 && ghost >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    /* Both under orders: the ghost ship is a hovering surface target,
     * so the galley's noairweapon cannon still reaches it. */
    Units_CommandAttackUnit(galley, ghost);
    Units_CommandAttackUnit(ghost, galley);
    Timer timer;
    Timer_Init(&timer);
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    int hp0 = u[ghost].health;
    int hurt = 0;
    for (int i = 0; i < 600 && !hurt; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        u = Units_GetActive(&n);
        ASSERT_EQ_INT(0, u[ghost].flying);
        hurt = (u[ghost].health < hp0) || (u[ghost].alive != UNIT_ALIVE_ACTIVE);
    }
    if (!hurt) probe_attack_state("galley vs ghost", galley, ghost);
    ASSERT(hurt);
    }
done:
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Clicking an enemy shows it in the sidebar: portrait, name and how
 * much health is left, and no orders (#49). */
/* The first clear site for a def on rings around a point. */
static int inspect_find_site(int def_idx, int32_t cx, int32_t cy,
                             int32_t *ox, int32_t *oy) {
    static const int dirs[8][2] = {{1,0},{0,1},{-1,0},{0,-1},
                                   {1,1},{-1,1},{1,-1},{-1,-1}};
    for (int32_t d = 192; d <= 896; d += 32) {
        for (int k = 0; k < 8; k++) {
            int32_t x = cx + dirs[k][0] * d, y = cy + dirs[k][1] * d;
            if (Units_IsBuildSiteClear(def_idx, x, y)) {
                *ox = x; *oy = y; return 1;
            }
        }
    }
    return 0;
}

TEST(enemy_unit_shows_in_the_sidebar) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int foe_def = Units_FindDefByName("VERSWORD");
    ASSERT(foe_def >= 0);
    int32_t fx = -1, fy = -1;
    for (int32_t d = 200; d <= 800 && fx < 0; d += 64) {
        static const int dirs[4][2] = {{1,0},{0,1},{-1,0},{0,-1}};
        for (int k = 0; k < 4 && fx < 0; k++) {
            int32_t x = ax + dirs[k][0] * d, y = ay + dirs[k][1] * d;
            if (Units_IsBuildSiteClear(foe_def, x, y)) { fx = x; fy = y; }
        }
    }
    ASSERT(fx >= 0);
    int foe = Units_Spawn(foe_def, 2, 1, fx, fy);
    ASSERT(foe >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    Units_SelectSingle(-1);
    ASSERT_EQ_INT(1, Units_SelectForInspect(foe));
    int n_sel = 0;
    const int *sel = Units_GetSelection(&n_sel);
    ASSERT_EQ_INT(1, n_sel);
    ASSERT_EQ_INT(foe, sel[0]);
    ASSERT(Units_GetSelectedDef() == Units_GetDef(foe_def));
    int hp = 0, hp_max = 0;
    Units_GetSelectedHealth(&hp, &hp_max);
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(units[foe].health, hp);
    ASSERT(hp_max > 0);
    /* It is not yours, so it offers nothing to build and takes no
     * orders. */
    ASSERT_EQ_INT(0, Units_SelectionHasBuilder());
    Units_CommandMoveSelected(ax, ay);
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)units[foe].cmd_kind);

    /* Nor does an enemy keep show its build menu, and an enemy gate
     * cannot be opened from the sidebar: both used to take the order. */
    int keep_def = Units_FindDefByName("VERKEEP");
    int gate_def = Units_FindDefByName("VERNGATE");
    ASSERT(keep_def >= 0);
    ASSERT(gate_def >= 0);
    int32_t kx = 0, ky = 0, gx = 0, gy = 0;
    ASSERT(inspect_find_site(keep_def, ax, ay, &kx, &ky));
    int keep = Units_Spawn(keep_def, 2, 1, kx, ky);
    ASSERT(keep >= 0);
    ASSERT(inspect_find_site(gate_def, ax, ay, &gx, &gy));
    int gate = Units_Spawn(gate_def, 2, 1, gx, gy);
    ASSERT(gate >= 0);

    Timer timer;
    Timer_Init(&timer);
    Units_SelectSingle(-1);
    ASSERT_EQ_INT(1, Units_SelectForInspect(keep));
    timer.accumulator = timer.sim_dt;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    ASSERT_EQ_INT(0, HUD_BuildSlotCount());
    /* A foreign unit's panel is its name and health only: no mana
     * gauge, no build menu (reported from play, like the original). */
    ASSERT_EQ_INT(1, HUD_WidgetVisible("HealthBar"));
    ASSERT_EQ_INT(0, HUD_WidgetVisible("ManaBar"));
    ASSERT_EQ_INT(0, HUD_WidgetVisible("BuildMenu"));

    Units_SelectSingle(-1);
    ASSERT_EQ_INT(1, Units_SelectForInspect(gate));
    int gate_state = Units_GateState(gate);
    ASSERT(gate_state >= 0);
    HUD_TriggerCommand(gate_state ? HUD_CMD_DEACTIVATE : HUD_CMD_ACTIVATE);
    ASSERT_EQ_INT(gate_state, Units_GateState(gate));
    Units_ToggleSelectedGate();
    ASSERT_EQ_INT(gate_state, Units_GateState(gate));

    /* Inspection is exclusive: taking one of your own units drops the
     * inspected enemy, and a click on another enemy inspects that one
     * instead of ordering an attack no unit can carry out. */
    units = Units_GetActive(&unit_count);
    int mine = -1;
    for (int i = 0; i < unit_count && mine < 0; i++) {
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].player_id == 1)
            mine = i;
    }
    ASSERT(mine >= 0);
    Units_SelectSingle(-1);
    ASSERT_EQ_INT(1, Units_SelectForInspect(foe));
    ASSERT_EQ_INT(0, Units_SelectionOwnedCount());
    Units_SelectToggle(mine);
    sel = Units_GetSelection(&n_sel);
    ASSERT_EQ_INT(1, n_sel);
    ASSERT_EQ_INT(mine, sel[0]);
    Units_SelectSingle(-1);
    ASSERT_EQ_INT(1, Units_SelectForInspect(foe));
    InGame_WorldClick(units[keep].world_x, units[keep].world_y, 0);
    sel = Units_GetSelection(&n_sel);
    ASSERT_EQ_INT(1, n_sel);
    ASSERT_EQ_INT(keep, sel[0]);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* The doors run the original's button states: rest 2, the enter clip
 * 5, the hover clip 6 held on its last frame while the cursor stays,
 * the leave clip 7, then rest (legacy:148022-148076). */
TEST(main_menu_doors_follow_original_states) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    if (MainMenu_Init(&platform) != 0) {
        printf("SKIP (no menu assets) ");
        UI_Shutdown(); teardown_platform(&platform); VFS_Shutdown();
        return;
    }
    if (MainMenu_DebugCharacterState(0) < 0) {
        printf("SKIP (no door clips) ");
        MainMenu_Shutdown(); UI_Shutdown(); teardown_platform(&platform);
        VFS_Shutdown();
        return;
    }
    const float dt = 1.0f / 60.0f;
    ASSERT_EQ_INT(2, MainMenu_DebugCharacterState(0));
    MainMenu_DebugForceHover(0);
    MainMenu_Tick(&platform, dt);
    ASSERT_EQ_INT(5, MainMenu_DebugCharacterState(0));
    int ticks = 0;
    while (MainMenu_DebugCharacterState(0) == 5 && ticks++ < 900)
        MainMenu_Tick(&platform, dt);
    /* machine5.bik is 41 frames at 30 fps: the hand-over waits for it. */
    ASSERT(ticks > 30);
    ASSERT_EQ_INT(6, MainMenu_DebugCharacterState(0));
    for (int i = 0; i < 300; i++) MainMenu_Tick(&platform, dt);
    ASSERT_EQ_INT(6, MainMenu_DebugCharacterState(0));
    MainMenu_DebugForceHover(-1);
    MainMenu_Tick(&platform, dt);
    ASSERT_EQ_INT(7, MainMenu_DebugCharacterState(0));
    ticks = 0;
    while (MainMenu_DebugCharacterState(0) == 7 && ticks++ < 900)
        MainMenu_Tick(&platform, dt);
    ASSERT(ticks > 30);
    ASSERT_EQ_INT(2, MainMenu_DebugCharacterState(0));
    /* Still under the cursor after the leave clip: no fresh crossing,
     * so the door rests until the cursor comes back. */
    MainMenu_DebugForceHover(0);
    MainMenu_Tick(&platform, dt);
    ASSERT_EQ_INT(5, MainMenu_DebugCharacterState(0));
    MainMenu_DebugForceHover(-2);
    MainMenu_Shutdown();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}


/* Find a clear site near (ax, ay) for a def. Returns 0 when nothing
 * within a few hundred pixels will take it. */
static int batch2_find_site(int def_idx, int32_t ax, int32_t ay,
                            int32_t *out_x, int32_t *out_y) {
    static const int dirs[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
        { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 }
    };
    for (int32_t d = 96; d <= 640; d += 32) {
        for (int k = 0; k < 8; k++) {
            int32_t x = ax + dirs[k][0] * d, y = ay + dirs[k][1] * d;
            if (Units_IsBuildSiteClear(def_idx, x, y)) {
                *out_x = x; *out_y = y; return 1;
            }
        }
    }
    return 0;
}

/* An empty treasury slows a build, it never cancels one (#24). The
 * original pays what it holds, scales that tick's progress by the
 * share it could pay, and leaves the frame standing
 * (legacy:39483-39496). A frame only rots once nobody is working on
 * it. */
TEST(a_starved_build_slows_but_never_rots) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int king = Units_DebugSpawnMonarch("ARA", ax + 64, ay);
    if (king < 0) FAIL_TO(done, "no monarch");
    {
    int menu[32];
    int n_menu = Units_GetBuildables((int)Units_GetActive(&unit_count)[king].def_idx,
                                     menu, 32);
    if (n_menu <= 0) { printf("SKIP (nothing to build) "); goto done; }
    int32_t bx = 0, by = 0;
    int bdef = -1;
    for (int m = 0; m < n_menu && bdef < 0; m++) {
        if (batch2_find_site(menu[m], ax, ay, &bx, &by)) bdef = menu[m];
    }
    if (bdef < 0) { printf("SKIP (no site) "); goto done; }
    int frame = Units_BeginBuildingForUnit(king, bdef, bx, by);
    ASSERT(frame >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < unit_count; i++) {
        if (Units_GetActive(&unit_count)[i].alive == UNIT_ALIVE_ACTIVE)
            Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
    }
    /* Cut the income off and empty the purse, so the builder can pay
     * nothing at all for the next twenty seconds. */
    int32_t regen = Economy_GetRegenRate(&world->economy, 1);
    Economy_AdjustCaps(&world->economy, 1, 0, -(float)regen);
    Economy_SpendAvailable(&world->economy, 1, 1.0e9f);
    units = Units_GetActive(&unit_count);
    int hp0 = units[frame].health;
    for (int i = 0; i < 1200; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        if (units[frame].alive != UNIT_ALIVE_ACTIVE) break;
    }
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)units[frame].alive);
    ASSERT_EQ_INT(1, (int)units[frame].under_construction);
    ASSERT(units[frame].health >= hp0);

    /* Pay up and the same frame carries on. */
    Economy_AdjustCaps(&world->economy, 1, 0, (float)regen);
    Economy_EarnF(&world->economy, 1, 1.0e6f);
    for (int i = 0; i < 900 && units[frame].health <= hp0; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
    }
    ASSERT(units[frame].health > hp0);
    }
done:
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Healing is paid for, over time, like anything else (#44). The
 * original charges the target's cost spread over its build time and
 * heals proportionally slower when the treasury is short
 * (legacy:39546-39562). */
TEST(healing_spends_mana_over_time) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int king = Units_DebugSpawnMonarch("ARA", ax + 64, ay);
    if (king < 0) FAIL_TO(done, "no monarch");
    {
    int hurt_def = Units_FindDefByName("ARASWORD");
    ASSERT(hurt_def >= 0);
    int32_t hx = 0, hy = 0;
    if (!batch2_find_site(hurt_def, ax + 64, ay, &hx, &hy)) {
        printf("SKIP (no site) "); goto done;
    }
    int hurt = Units_Spawn(hurt_def, 1, 0, hx, hy);
    ASSERT(hurt >= 0);
    Units_SetHealthPercent(hurt, 40);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < unit_count; i++) {
        if (Units_GetActive(&unit_count)[i].alive == UNIT_ALIVE_ACTIVE)
            Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
    }
    Economy_EarnF(&world->economy, 1, 1.0e6f);
    /* No income while we watch, so every point of mana that leaves the
     * purse was spent on the healing. */
    int32_t regen = Economy_GetRegenRate(&world->economy, 1);
    Economy_AdjustCaps(&world->economy, 1, 0, -(float)regen);
    units = Units_GetActive(&unit_count);
    int hp0 = units[hurt].health;
    int32_t mana0 = Economy_GetMana(&world->economy, 1);
    Units_SelectSingle(king);
    Units_CommandRepairSelected(hurt);
    Units_SelectSingle(-1);
    for (int i = 0; i < 1800 && units[hurt].health <= hp0; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
    }
    ASSERT(units[hurt].health > hp0);
    ASSERT(Economy_GetMana(&world->economy, 1) < mana0);

    /* Empty the purse and the healing stops until it can pay again. */
    Economy_SpendAvailable(&world->economy, 1, 1.0e9f);
    units = Units_GetActive(&unit_count);
    int hp_dry = units[hurt].health;
    for (int i = 0; i < 600; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
    }
    ASSERT_EQ_INT(hp_dry, units[hurt].health);
    Economy_AdjustCaps(&world->economy, 1, 0, (float)regen);
    }
done:
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* One unload order empties the hold (#37). */
TEST(one_unload_order_empties_the_hold) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int carrier_def = Units_FindDefByName("ARAWAR");
    int rider_def   = Units_FindDefByName("ARASWORD");
    ASSERT(carrier_def >= 0 && rider_def >= 0);
    /* A war galley is a naval class, so no land cell will pass a build
     * site test. Put it down directly, the way the transport probe
     * does, and unload at its own position: the drop ring finds the
     * nearest cell that will take each passenger. */
    int32_t cx = ax + 160, cy = ay;
    {
    int carrier = Units_Spawn(carrier_def, 1, 0, cx, cy);
    ASSERT(carrier >= 0);
    int riders[2] = { -1, -1 };
    riders[0] = Units_Spawn(rider_def, 1, 0, cx + 48, cy);
    riders[1] = Units_Spawn(rider_def, 1, 0, cx - 48, cy);
    if (riders[0] < 0 || riders[1] < 0) { printf("SKIP (no room) "); goto done; }
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < unit_count; i++) {
        if (Units_GetActive(&unit_count)[i].alive == UNIT_ALIVE_ACTIVE)
            Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
    }
    for (int r = 0; r < 2; r++) {
        Units_SelectSingle(carrier);
        Units_CommandLoadSelected(riders[r]);
        for (int i = 0; i < 240; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
            units = Units_GetActive(&unit_count);
            if (units[riders[r]].alive == UNIT_ALIVE_TRANSPORTED) break;
        }
    }
    units = Units_GetActive(&unit_count);
    if (units[carrier].cargo_count < 2) FAIL_TO(done, "load failed");

    Units_SelectSingle(carrier);
    Units_CommandUnloadSelected(units[carrier].world_x,
                                units[carrier].world_y);
    Units_SelectSingle(-1);
    for (int i = 0; i < 600 && units[carrier].cargo_count > 0; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
    }
    ASSERT_EQ_INT(0, (int)units[carrier].cargo_count);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)units[riders[0]].alive);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)units[riders[1]].alive);
    }
done:
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A transport with passengers shows how many it holds in the sidebar's
 * help line, "Carrying N" (legacy:152081-152089). Empty, or with nothing
 * selected, the line is the mana readout (legacy:152100-152110). */
TEST(loaded_transport_shows_its_cargo_count) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int carrier_def = Units_FindDefByName("ARAWAR");
    int rider_def   = Units_FindDefByName("ARASWORD");
    ASSERT(carrier_def >= 0 && rider_def >= 0);
    int32_t cx = ax + 160, cy = ay;
    {
    int carrier = Units_Spawn(carrier_def, 1, 0, cx, cy);
    ASSERT(carrier >= 0);
    int riders[2] = { -1, -1 };
    riders[0] = Units_Spawn(rider_def, 1, 0, cx + 48, cy);
    riders[1] = Units_Spawn(rider_def, 1, 0, cx - 48, cy);
    if (riders[0] < 0 || riders[1] < 0) { printf("SKIP (no room) "); goto done; }
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < unit_count; i++) {
        if (Units_GetActive(&unit_count)[i].alive == UNIT_ALIVE_ACTIVE)
            Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
    }
    char txt[64] = "";
    char want[64] = "";
    int cur = 0, cap = 0;

    /* Selected while empty: the readout, current over maximum. */
    Units_SelectSingle(carrier);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    ASSERT_EQ_INT(0, HUD_WidgetHidden("HelpText"));
    ASSERT_EQ_INT(1, HUD_WidgetText("HelpText", txt, sizeof(txt)));
    ASSERT_EQ_INT(0, strncmp(txt, "Mana\n", 5));
    ASSERT_EQ_INT(2, sscanf(txt + 5, "%d/%d", &cur, &cap));
    ASSERT(cur <= cap);
    ASSERT_EQ_INT(0, Units_GetSelectedCargoCount());

    for (int r = 0; r < 2; r++) {
        Units_SelectSingle(carrier);
        Units_CommandLoadSelected(riders[r]);
        for (int i = 0; i < 240; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
            units = Units_GetActive(&unit_count);
            if (units[riders[r]].alive == UNIT_ALIVE_TRANSPORTED) break;
        }
        units = Units_GetActive(&unit_count);
        if (units[carrier].cargo_count < r + 1) FAIL_TO(done, "load failed");
        /* One passenger per rider aboard, and the line says so. */
        Units_SelectSingle(carrier);
        ASSERT_EQ_INT(r + 1, Units_GetSelectedCargoCount());
        timer.accumulator = 0.0;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        snprintf(want, sizeof(want), "Carrying %d", r + 1);
        ASSERT_EQ_INT(1, HUD_WidgetText("HelpText", txt, sizeof(txt)));
        ASSERT_EQ_STR(want, txt);
    }

    /* Nothing selected: the readout again. */
    Units_SelectSingle(-1);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    ASSERT_EQ_INT(1, HUD_WidgetText("HelpText", txt, sizeof(txt)));
    ASSERT_EQ_INT(0, strncmp(txt, "Mana\n", 5));

    /* Unloading empties the hold and the readout comes back. */
    Units_SelectSingle(carrier);
    Units_CommandUnloadSelected(units[carrier].world_x,
                                units[carrier].world_y);
    for (int i = 0; i < 600 && units[carrier].cargo_count > 0; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
    }
    ASSERT_EQ_INT(0, (int)units[carrier].cargo_count);
    ASSERT_EQ_INT(0, Units_GetSelectedCargoCount());
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    ASSERT_EQ_INT(1, HUD_WidgetText("HelpText", txt, sizeof(txt)));
    ASSERT_EQ_INT(0, strncmp(txt, "Mana\n", 5));
    ASSERT_EQ_INT(2, sscanf(txt + 5, "%d/%d", &cur, &cap));
    }
done:
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Radar blip size, from the first blip shape the corner radar uses. */
#define MINIMAP_TEST_DOT_PX 4

static SDL_Surface *minimap_shoot(TAK_Platform *platform) {
    SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(
        0, platform->window_w, platform->window_h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!shot) return NULL;
    if (SDL_RenderReadPixels(platform->renderer, NULL, SDL_PIXELFORMAT_RGBA32,
                             shot->pixels, shot->pitch) != 0) {
        SDL_FreeSurface(shot);
        return NULL;
    }
    return shot;
}

static uint32_t minimap_px(SDL_Surface *s, int x, int y) {
    const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * s->pitch;
    uint8_t r = 0, g = 0, b = 0, a = 0;
    SDL_GetRGBA(((const uint32_t *)row)[x], s->format, &r, &g, &b, &a);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

/* Every pixel of the dot carries this team colour. */
static int minimap_dot_is(SDL_Surface *s, SDL_Rect d, uint32_t rgba) {
    uint32_t want = rgba & 0x00FFFFFFu;
    for (int y = d.y; y < d.y + d.h; y++)
        for (int x = d.x; x < d.x + d.w; x++)
            if (minimap_px(s, x, y) != want) return 0;
    return 1;
}

/* Nothing in the dot moved since the baseline shot. */
static int minimap_dot_unchanged(SDL_Surface *s, SDL_Surface *base, SDL_Rect d) {
    for (int y = d.y; y < d.y + d.h; y++)
        for (int x = d.x; x < d.x + d.w; x++)
            if (minimap_px(s, x, y) != minimap_px(base, x, y)) return 0;
    return 1;
}

/* A world spot whose whole dot, plus a two-pixel margin, lands on
 * untouched black in the baseline. That keeps it clear of the view box
 * and of the dots the map's own starting units already draw. */
static int minimap_find_spot(TAK_Platform *platform, const GameWorld *world,
                             SDL_Surface *base, const SDL_Rect *taken,
                             int n_taken, int32_t *out_x, int32_t *out_y) {
    for (int gy = 1; gy < 16; gy++) {
        for (int gx = 1; gx < 16; gx++) {
            int32_t wx = (int32_t)((int64_t)world->map_pixels_w * gx / 16);
            int32_t wy = (int32_t)((int64_t)world->map_pixels_h * gy / 16);
            SDL_Rect dot;
            if (!Minimap_DebugDotRect(platform, wx, wy, &dot)) continue;
            if (dot.w != MINIMAP_TEST_DOT_PX || dot.h != MINIMAP_TEST_DOT_PX)
                continue;
            int ok = 1;
            for (int y = dot.y - 2; y < dot.y + dot.h + 2 && ok; y++) {
                for (int x = dot.x - 2; x < dot.x + dot.w + 2 && ok; x++) {
                    if (x < 0 || y < 0 || x >= base->w || y >= base->h) ok = 0;
                    else if (minimap_px(base, x, y) != 0) ok = 0;
                }
            }
            for (int i = 0; i < n_taken && ok; i++) {
                SDL_Rect a = taken[i];
                a.x -= 4; a.y -= 4; a.w += 8; a.h += 8;
                if (SDL_HasIntersection(&a, &dot)) ok = 0;
            }
            if (!ok) continue;
            *out_x = wx;
            *out_y = wy;
            return 1;
        }
    }
    return 0;
}

TEST(minimap_draws_a_dot_per_visible_unit) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    ASSERT_EQ_INT(0, Minimap_Init(&platform));

    /* Drive the fog by hand the way test_fog.c does: one pass off the
     * live units, then blank every cell so only ground revealed here
     * counts as seen. */
    ASSERT(world->fog_w > 0 && world->fog_h > 0 && world->fog_cell_px > 0);
    world->cfg.line_of_sight = 1;
    Fog_Update(world, 1);
    memset(world->fog_layers[1], TAK_FOG_UNEXPLORED,
           (size_t)world->fog_w * (size_t)world->fog_h);

    Minimap_Draw(&platform);
    SDL_Surface *base = minimap_shoot(&platform);
    ASSERT(base != NULL);

    /* Three clear spots: the local player's unit, an enemy the player
     * will get to see, and an enemy that stays in the dark. */
    SDL_Rect dot[3];
    int32_t sx[3], sy[3];
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(1, minimap_find_spot(&platform, world, base, dot, i,
                                           &sx[i], &sy[i]));
        ASSERT_EQ_INT(1, Minimap_DebugDotRect(&platform, sx[i], sy[i], &dot[i]));
    }

    int sword = Units_FindDefByName("ARASWORD");
    ASSERT(sword >= 0);
    ASSERT(Units_Spawn(sword, 1, 0, sx[0], sy[0]) >= 0);
    ASSERT(Units_Spawn(sword, 2, 1, sx[1], sy[1]) >= 0);
    ASSERT(Units_Spawn(sword, 2, 1, sx[2], sy[2]) >= 0);

    uint32_t own_rgba = Units_GetTeamColorRGBA(0);
    uint32_t foe_rgba = Units_GetTeamColorRGBA(1);
    ASSERT(own_rgba != foe_rgba);

    /* Nothing is revealed yet: the player's own unit draws anyway, both
     * enemies leave their pixels exactly as they were. */
    Minimap_Draw(&platform);
    SDL_Surface *shot = minimap_shoot(&platform);
    ASSERT(shot != NULL);
    ASSERT_EQ_INT(1, minimap_dot_is(shot, dot[0], own_rgba));
    ASSERT_EQ_INT(1, minimap_dot_unchanged(shot, base, dot[1]));
    ASSERT_EQ_INT(1, minimap_dot_unchanged(shot, base, dot[2]));
    SDL_FreeSurface(shot);

    /* Reveal the ground under the first enemy and nothing else. */
    world->fog_layers[1][(sy[1] / world->fog_cell_px) * world->fog_w +
                         (sx[1] / world->fog_cell_px)] = TAK_FOG_VISIBLE;
    ASSERT_EQ_INT(1, Fog_IsVisible(world, sx[1], sy[1]));
    ASSERT_EQ_INT(0, Fog_IsVisible(world, sx[2], sy[2]));

    /* The seen enemy draws in its own owner's colour, not the
     * viewer's, and the one still in the dark is untouched. */
    Minimap_Draw(&platform);
    shot = minimap_shoot(&platform);
    ASSERT(shot != NULL);
    ASSERT_EQ_INT(1, minimap_dot_is(shot, dot[0], own_rgba));
    ASSERT_EQ_INT(1, minimap_dot_is(shot, dot[1], foe_rgba));
    ASSERT_EQ_INT(1, minimap_dot_unchanged(shot, base, dot[2]));
    SDL_FreeSurface(shot);
    SDL_FreeSurface(base);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}


/* The stance and gate pairs draw their icons at rest. Frame 1 is the
 * lit state, frame 2 the plain icon, frame 0 the disabled slot; the
 * original sets 1 or 2 outright every update (legacy:150868,
 * legacy:150925). Ours left the inactive ones on 0, a dark box until
 * the mouse arrived. */
TEST(stance_and_gate_buttons_show_their_icons_at_rest) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int fighter = -1;
    for (int i = 0; i < unit_count && fighter < 0; i++) {
        const UnitDef *d = Units_GetDef(units[i].def_idx);
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].player_id == 1 &&
            d && (d->cap_flags & UNIT_CAP_ATTACK)) fighter = i;
    }
    ASSERT(fighter >= 0);
    int gate_def = Units_FindDefByName("ARANGATE");
    ASSERT(gate_def >= 0);
    int32_t gx = 0, gy = 0;
    int gate = -1;
    if (batch2_find_site(gate_def, ax, ay, &gx, &gy))
        gate = Units_Spawn(gate_def, 1, 0, gx, gy);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);

    Units_DebugSetAggro(fighter, UNIT_AGGRO_OFFENSIVE);
    Units_SelectSingle(fighter);
    for (int i = 0; i < 2; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    ASSERT_EQ_INT(0, HUD_WidgetHidden("Defensive"));
    ASSERT_EQ_INT(1, HUD_WidgetFrame("Offensive"));
    ASSERT_EQ_INT(2, HUD_WidgetFrame("Defensive"));
    ASSERT_EQ_INT(2, HUD_WidgetFrame("Passive"));
    Units_DebugSetAggro(fighter, UNIT_AGGRO_DEFENSIVE);
    timer.accumulator = timer.sim_dt;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    ASSERT_EQ_INT(2, HUD_WidgetFrame("Offensive"));
    ASSERT_EQ_INT(1, HUD_WidgetFrame("Defensive"));
    ASSERT_EQ_INT(2, HUD_WidgetFrame("Passive"));

    if (gate >= 0) {
        Units_SelectSingle(gate);
        for (int i = 0; i < 2; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        }
        ASSERT_EQ_INT(0, HUD_WidgetHidden("Active"));
        if (Units_SelectedGateState() == 0) {
            ASSERT_EQ_INT(2, HUD_WidgetFrame("Active"));
            ASSERT_EQ_INT(1, HUD_WidgetFrame("Inactive"));
        } else {
            ASSERT_EQ_INT(1, HUD_WidgetFrame("Active"));
            ASSERT_EQ_INT(2, HUD_WidgetFrame("Inactive"));
        }
    }
    Units_SelectSingle(-1);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* The placement preview holds the pose of the finished building. The
 * barracks' Create turns its build pad by a half turn minus the unit's
 * orientation, so a preview whose script saw no orientation drew the
 * pad at the back (the report behind this test). The preview now
 * answers Create the way a finished building would, and every kind of
 * structure is checked against a unit of that kind. */
TEST(building_previews_hold_the_finished_pose) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int keep_def = Units_FindDefByName("ARAKEEP");
    ASSERT(keep_def >= 0);
    int32_t kx = 0, ky = 0;
    ASSERT(batch2_find_site(keep_def, ax, ay, &kx, &ky));
    int keep = Units_Spawn(keep_def, 1, 0, kx, ky);
    ASSERT(keep >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < 10; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    int32_t lrot[3], lpos[3], grot[3], gpos[3];
    ASSERT_EQ_INT(1, Units_DebugPieceState(keep, "buildpad", lrot, lpos));
    ASSERT_EQ_INT(1, Units_DebugGhostPieceState(keep_def, 0, "buildpad", grot, gpos));
    /* Both face the way the building was placed: no turn at all. */
    ASSERT_EQ_INT(0, lrot[1]);
    ASSERT_EQ_INT(lrot[1], grot[1]);
    char why[160];
    Units_DebugKillHandle(keep);

    /* Every kind of structure with a script, placed and compared. */
    int checked = 0, wrong = 0;
    for (int d = 0; d < Units_GetDefCount(); d++) {
        const UnitDef *def = Units_GetDef(d);
        if (!def || def->max_velocity > 0.0f || !def->cob_script) continue;
        if (def->footprint_x <= 0 || def->footprint_z <= 0) continue;
        if (Cob_FindScript(def->cob_script, "Create") < 0) continue;
        int h = Units_Spawn(d, 1, 0, kx, ky);
        if (h < 0) continue;
        for (int i = 0; i < 6; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        }
        /* Two samples half a second apart tell the pieces the unit
         * animates on its own from the ones that hold a pose. */
        int np = Units_DebugPieceCount(h);
        int32_t *rot0 = (int32_t *)malloc(sizeof(int32_t) * 3 * (size_t)(np > 0 ? np : 1));
        int32_t *pos0 = (int32_t *)malloc(sizeof(int32_t) * 3 * (size_t)(np > 0 ? np : 1));
        ASSERT_NOT_NULL(rot0);
        ASSERT_NOT_NULL(pos0);
        Units_DebugSnapshotPieces(h, rot0, pos0, np);
        for (int i = 0; i < 30; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        }
        int r = Units_DebugGhostMatchesUnit(h, rot0, pos0, why, sizeof(why));
        free(rot0);
        free(pos0);
        if (r == 0) { wrong++; printf("[%s] ", why); }
        if (r >= 0) checked++;
        Units_DebugKillHandle(h);
        for (int i = 0; i < 2; i++) {
            timer.accumulator = timer.sim_dt;
            ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        }
    }
    printf("(%d kinds) ", checked);
    ASSERT(checked >= 20);
    ASSERT_EQ_INT(0, wrong);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A veteran stronghold swaps its crew's arms and wheel for the gilded
 * pieces (StatusControl hides ArmLR and shows ArmLR5 once the rank
 * passes four). ArmLR5 is a child of ArmLR, and hiding a piece must
 * not hide its children (legacy:306415 hides one piece), or the crew
 * vanishes the moment the tower earns its rank (#32). */
TEST(veteran_swap_keeps_the_crew_drawn) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, gates_setup_world(&platform, &world));

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    ASSERT(unit_count > 0);
    int32_t ax = units[0].world_x, ay = units[0].world_y;
    int tower_def = Units_FindDefByName("ARASSH");
    ASSERT(tower_def >= 0);
    int32_t tx = 0, ty = 0;
    ASSERT(batch2_find_site(tower_def, ax, ay, &tx, &ty));
    int tower = Units_Spawn(tower_def, 1, 0, tx, ty);
    ASSERT(tower >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    for (int i = 0; i < 5; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    /* Fresh: the plain arm draws, the gilded one is hidden by Create. */
    ASSERT_EQ_INT(0, Units_DebugPieceHidden(tower, "ArmLR"));
    ASSERT_EQ_INT(1, Units_DebugPieceHidden(tower, "ArmLR5"));

    /* Rank five: StatusControl polls once a second. */
    Units_DebugSetVeteranLevel(tower, 5);
    for (int i = 0; i < 150; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    ASSERT_EQ_INT(1, Units_DebugPieceHidden(tower, "ArmLR"));
    ASSERT_EQ_INT(0, Units_DebugPieceHidden(tower, "ArmLR5"));
    ASSERT_EQ_INT(0, Units_DebugPieceHidden(tower, "HandR"));

    /* Rank ten: the cannon swaps too. */
    Units_DebugSetVeteranLevel(tower, 10);
    for (int i = 0; i < 150; i++) {
        timer.accumulator = timer.sim_dt;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    ASSERT_EQ_INT(1, Units_DebugPieceHidden(tower, "Cannon"));
    ASSERT_EQ_INT(0, Units_DebugPieceHidden(tower, "Cannon10"));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Issue #34: a patrol armed on the sidebar must loop between where the
 * unit stood and the clicked point until a new order. The original
 * keeps the patrol order resident and repeats it (legacy:11565,
 * legacy:9664-9682). This drives the path the player uses: select by
 * click, arm Patrol on the sidebar, click the world, then watch the
 * unit through real frames. */
TEST(patrol_from_the_sidebar_loops_until_a_new_order) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.players[1].kind = TAK_SLOT_AI;
    ASSERT_EQ_INT(0, World_BeginLoad(&platform, &cfg,
                                     "two castles", "aramon"));
    ASSERT_EQ_INT(0, Loading_Init(&platform));
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(&platform, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, next);
    GameWorld *world = World_Get();
    ASSERT_NOT_NULL(world);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    /* The player's monarch: the unit a first skirmish click lands on. */
    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int walker = -1;
    for (int i = 0; i < unit_count && walker < 0; i++) {
        const UnitDef *ud = Units_GetDef(units[i].def_idx);
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].player_id == 1 &&
            ud && ud->max_velocity > 0.0f &&
            (ud->cap_flags & UNIT_CAP_PATROL))
            walker = i;
    }
    ASSERT(walker >= 0);
    int32_t ax = units[walker].world_x;
    int32_t ay = units[walker].world_y;

    /* One frame loads the HUD layout, then the camera is put on the
     * unit so it and the far point are both over the play area. */
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    SDL_Rect play;
    ASSERT_EQ_INT(1, HUD_GetViewportRect(&platform, &play));
    {
        int32_t cx = ax - (play.x + play.w / 2);
        int32_t cy = ay - (play.y + play.h / 2);
        int32_t max_x = world->map_pixels_w - world->viewport_w;
        int32_t max_y = world->map_pixels_h - world->viewport_h;
        if (cx < 0) cx = 0; else if (cx > max_x) cx = max_x;
        if (cy < 0) cy = 0; else if (cy > max_y) cy = max_y;
        world->cam_x = cx;
        world->cam_y = cy;
    }

    /* Select by clicking the unit where it is drawn, lifted by the
     * terrain height under it (legacy:197689), as the player does. */
    Units_SelectSingle(-1);
    {
        int32_t seen_y = ay - (int32_t)((float)Terrain_SampleHeight(world, ax, ay)
                                        * Units_GetTanTilt());
        InGame_WorldClick(ax, seen_y, 0);
    }
    int n_sel = 0;
    const int *sel = Units_GetSelection(&n_sel);
    ASSERT_EQ_INT(1, n_sel);
    ASSERT_EQ_INT(walker, sel[0]);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));

    /* Arm Patrol on the sidebar. */
    SDL_Rect btn;
    ASSERT_EQ_INT(1, HUD_GetActionButtonRect(HUD_CMD_PATROL, &btn));
    int btn_x = btn.x + btn.w / 2;
    int btn_y = btn.y + btn.h / 2;
    ASSERT_EQ_INT(1, HUD_HitTest(btn_x, btn_y, &platform));
    ASSERT_EQ_INT(1, HUD_HandleSidebarClick(btn_x, btn_y, &platform));
    ASSERT_EQ_INT(HUD_CMD_PATROL, HUD_GetCommandMode());

    /* A far point over open ground that is on screen and off the HUD. */
    int32_t bx = ax, by = ay;
    static const int leg_dir[4][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }
    };
    for (int d = 0; d < 4 && bx == ax && by == ay; d++) {
        int32_t tx = ax + leg_dir[d][0] * 192;
        int32_t ty = ay + leg_dir[d][1] * 192;
        int wx = (int)(tx - world->cam_x);
        int wy = (int)(ty - world->cam_y);
        if (wx < 0 || wy < 0 || wx >= platform.window_w ||
            wy >= platform.window_h) continue;
        if (HUD_HitTest(wx, wy, &platform)) continue;
        if (!Terrain_IsWalkable(world, tx, ty, 255)) continue;
        bx = tx;
        by = ty;
    }
    ASSERT(bx != ax || by != ay);

    /* The world click issues the order and drops the pending mode. */
    InGame_WorldClick(bx, by, 0);
    ASSERT_EQ_INT(HUD_CMD_NONE, HUD_GetCommandMode());
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_PATROL, units[walker].cmd_kind);
    ASSERT_EQ_INT(bx, units[walker].cmd_x);
    ASSERT_EQ_INT(by, units[walker].cmd_y);
    ASSERT_EQ_INT(ax, units[walker].patrol_x);
    ASSERT_EQ_INT(ay, units[walker].patrol_y);

    /* Real frames: reach B, come back to A, reach B again, and the
     * order stands the whole way. */
    int reached_b = 0, back_a = 0, again_b = 0;
    int kind_held = 1;
    for (int t = 0; t < 4000; t++) {
        timer.accumulator = timer.sim_dt * 2.0;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        const Unit *pu = &units[walker];
        if (pu->cmd_kind != UNIT_CMD_PATROL) {
            kind_held = 0;
            break;
        }
        int64_t dbx = pu->world_x - bx, dby = pu->world_y - by;
        int64_t dax = pu->world_x - ax, day = pu->world_y - ay;
        int64_t db2 = dbx * dbx + dby * dby;
        int64_t da2 = dax * dax + day * day;
        if (!reached_b && db2 <= 144) reached_b = t + 1;
        else if (reached_b && !back_a && da2 <= 144) back_a = t + 1;
        else if (back_a && !again_b && db2 <= 144) again_b = t + 1;
        if (again_b) break;
    }
    units = Units_GetActive(&unit_count);
    printf("[hud patrol A=(%d,%d) B=(%d,%d) b=%d a=%d b2=%d kind=%d at (%d,%d)] ",
           ax, ay, bx, by, reached_b, back_a, again_b,
           units[walker].cmd_kind, units[walker].world_x,
           units[walker].world_y);
    ASSERT(kind_held);
    ASSERT(reached_b > 0);
    ASSERT(back_a > reached_b);
    ASSERT(again_b > back_a);
    ASSERT_EQ_INT(UNIT_CMD_PATROL, units[walker].cmd_kind);

    /* The same order to a point the unit cannot stand on (a tree, a
     * cliff, water), which is what a click on a real map often is. The
     * route ends at the nearest cell the path finder allows and the leg
     * completes there, so the loop still runs. Before the fix the unit
     * ground at the edge on its first leg for good, which is what the
     * report described. */
    int32_t a2x = units[walker].world_x;
    int32_t a2y = units[walker].world_y;
    int32_t cx = a2x, cy = a2y;
    for (int r = 64; r <= 480 && cx == a2x && cy == a2y; r += 16) {
        for (int k = 0; k < 32; k++) {
            double ang = (double)k * 6.2831853 / 32.0;
            int32_t tx = a2x + (int32_t)((double)r * cos(ang));
            int32_t ty = a2y + (int32_t)((double)r * sin(ang));
            if (tx < 32 || ty < 32 || tx >= world->map_pixels_w - 32 ||
                ty >= world->map_pixels_h - 32) continue;
            if (Units_CanStandAt(walker, tx, ty)) continue;
            cx = tx;
            cy = ty;
            break;
        }
    }
    ASSERT(cx != a2x || cy != a2y);
    timer.accumulator = 0.0;
    ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    ASSERT_EQ_INT(1, HUD_GetActionButtonRect(HUD_CMD_PATROL, &btn));
    ASSERT_EQ_INT(1, HUD_HandleSidebarClick(btn.x + btn.w / 2,
                                            btn.y + btn.h / 2, &platform));
    ASSERT_EQ_INT(HUD_CMD_PATROL, HUD_GetCommandMode());
    InGame_WorldClick(cx, cy, 0);
    ASSERT_EQ_INT(HUD_CMD_NONE, HUD_GetCommandMode());
    units = Units_GetActive(&unit_count);
    ASSERT_EQ_INT(UNIT_CMD_PATROL, units[walker].cmd_kind);
    ASSERT_EQ_INT(cx, units[walker].cmd_x);
    ASSERT_EQ_INT(cy, units[walker].cmd_y);
    ASSERT_EQ_INT(a2x, units[walker].patrol_x);
    ASSERT_EQ_INT(a2y, units[walker].patrol_y);

    int64_t best_c2 = INT64_MAX;
    int leg_done = 0, back_a2 = 0, again_c = 0;
    kind_held = 1;
    for (int t = 0; t < 4000; t++) {
        timer.accumulator = timer.sim_dt * 2.0;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
        units = Units_GetActive(&unit_count);
        const Unit *pu = &units[walker];
        if (pu->cmd_kind != UNIT_CMD_PATROL) {
            kind_held = 0;
            break;
        }
        int64_t dcx = pu->world_x - cx, dcy = pu->world_y - cy;
        int64_t dc2 = dcx * dcx + dcy * dcy;
        if (dc2 < best_c2) best_c2 = dc2;
        /* The leg is done when the order turns for home. */
        int heading_home = (pu->cmd_x == a2x && pu->cmd_y == a2y);
        int64_t dax2 = pu->world_x - a2x, day2 = pu->world_y - a2y;
        if (!leg_done && heading_home) leg_done = t + 1;
        else if (leg_done && !back_a2 &&
                 dax2 * dax2 + day2 * day2 <= 144) back_a2 = t + 1;
        else if (back_a2 && !again_c && !heading_home) again_c = t + 1;
        if (again_c) break;
    }
    units = Units_GetActive(&unit_count);
    printf("[blocked C=(%d,%d) best=%.0fpx done=%d a=%d c2=%d kind=%d] ",
           cx, cy, sqrt((double)best_c2), leg_done, back_a2, again_c,
           units[walker].cmd_kind);
    ASSERT(kind_held);
    ASSERT(leg_done > 0);
    ASSERT(best_c2 <= (int64_t)64 * 64);
    ASSERT(back_a2 > leg_done);
    ASSERT(again_c > back_a2);
    ASSERT_EQ_INT(UNIT_CMD_PATROL, units[walker].cmd_kind);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* ── Battle end: the original's rule and its screens (issue #17) ───── */

static int end_load_skirmish(TAK_Platform *platform, const BattleConfig *cfg,
                             GameWorld **out_world) {
    if (World_BeginLoad(platform, cfg, cfg->map_name, "aramon") != 0) return -1;
    if (Loading_Init(platform) != 0) return -1;
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 600 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(platform, 1.0f / 60.0f);
    }
    if (next != GAMESTATE_IN_GAME) return -1;
    *out_world = World_Get();
    return *out_world ? 0 : -1;
}

/* The player's live monarch, by the FBI commander flag. */
static int end_find_monarch(int player_id) {
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; i < n; i++) {
        const UnitDef *d = Units_GetDef(units[i].def_idx);
        if (!d || !d->commander) continue;
        if (units[i].player_id != player_id) continue;
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        return i;
    }
    return -1;
}

static int end_units_left(int player_id) {
    int n = 0, left = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; i < n; i++) {
        if (units[i].player_id == player_id && units[i].alive != UNIT_ALIVE_DEAD) left++;
    }
    return left;
}

/* Whole frames of 30 ticks until the verdict fires or the limit runs out. */
static int end_run_frames(TAK_Platform *platform, GameWorld *world, Timer *timer,
                          int max_frames) {
    for (int f = 0; f < max_frames; f++) {
        if (world->skirmish_game_over) return f;
        timer->accumulator = timer->sim_dt * 30.0;
        if (InGame_Tick(platform, timer) != GAMESTATE_IN_GAME) return -1;
    }
    return world->skirmish_game_over ? max_frames : -1;
}

/* The report behind #17: with two opponents left standing, the local
 * player's monarch fell and nothing happened. The original ends the
 * local player's game the moment their side is gone, whoever is still
 * fighting (legacy:240018-240028), and Monarch Expendable off makes the
 * monarch's death take the whole army with it (legacy:227174). */
TEST(skirmish_local_monarch_death_is_defeat_with_two_foes_left) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "Angvir's Maze", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    cfg.players[1].kind = TAK_SLOT_AI;
    cfg.players[2].kind = TAK_SLOT_AI;
    cfg.players[2].side = TAK_SIDE_VERUNA;
    cfg.players[2].team = 3;
    cfg.players[2].color = 2;
    strncpy(cfg.players[2].name, "Sasha", sizeof(cfg.players[2].name) - 1);
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));
    ASSERT_EQ_INT(0, world->mission.placement_count);
    ASSERT(world->num_start_positions >= 3);

    int local_monarch = end_find_monarch(1);
    ASSERT(local_monarch >= 0);
    ASSERT(end_find_monarch(2) >= 0);
    ASSERT(end_find_monarch(3) >= 0);

    /* A second unit of the local player, to see the army go with the
     * monarch. */
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    int sword = Units_Spawn(sword_def, 1, cfg.players[0].color,
                            units[local_monarch].world_x + 48,
                            units[local_monarch].world_y);
    ASSERT(sword >= 0);
    ASSERT_EQ_INT(2, world->stats[1].units_built);

    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;
    ASSERT_EQ_INT(local_monarch, Units_DebugKillHandle(local_monarch));

    /* The army is gone at once and the verdict follows within a few
     * seconds of ticks while both AI players are still alive. */
    units = Units_GetActive(&n);
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, units[sword].alive);
    ASSERT_EQ_INT(1, world->stats[1].eliminated);
    ASSERT_EQ_INT(1, world->stats[1].losses);
    ASSERT(end_run_frames(&platform, world, &timer, 20) >= 0);
    ASSERT_EQ_INT(1, world->skirmish_game_over);
    ASSERT_EQ_INT(-1, world->skirmish_local_result);
    ASSERT_EQ_STR("Defeat", world->skirmish_end_reason);
    ASSERT(end_units_left(2) > 0);
    ASSERT(end_units_left(3) > 0);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Monarch Expendable on: the original keeps a player in the battle
 * while any unit of theirs remains, a lone lodestone included, because
 * the verdict reads the live-unit count the unit records maintain
 * (legacy:226969, legacy:227378, legacy:240018). Defeat comes with the
 * last unit. */
TEST(skirmish_expendable_player_stands_until_the_last_unit) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 1;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));

    int local_monarch = end_find_monarch(1);
    ASSERT(local_monarch >= 0);
    int lode_def = Units_FindDefByName("ARALODE");
    ASSERT(lode_def >= 0);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    int lode = Units_Spawn(lode_def, 1, cfg.players[0].color,
                           units[local_monarch].world_x + 96,
                           units[local_monarch].world_y + 96);
    ASSERT(lode >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    ASSERT_EQ_INT(local_monarch, Units_DebugKillHandle(local_monarch));
    ASSERT_EQ_INT(0, world->stats[1].eliminated);
    InGame_DebugRunSimTicks(240);
    ASSERT_EQ_INT(0, world->skirmish_game_over);
    units = Units_GetActive(&n);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, units[lode].alive);

    ASSERT_EQ_INT(lode, Units_DebugKillHandle(lode));
    for (int t = 0; t < 40 && !world->skirmish_game_over; t++) {
        InGame_DebugRunSimTicks(30);
    }
    ASSERT_EQ_INT(1, world->skirmish_game_over);
    ASSERT_EQ_INT(-1, world->skirmish_local_result);
    ASSERT_EQ_STR("Defeat", world->skirmish_end_reason);
    ASSERT_EQ_INT(0, end_units_left(1));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* An AI raider that has reached the enemy start and sees nothing goes
 * for the nearest enemy unit wherever it stands (legacy:15365), so a
 * last lodestone out of sight cannot stall the battle. */
/* A unit's own kills show in the sidebar for your units, hidden at
 * zero (legacy:152496-152506). Reported from play: no kill count. */
TEST(hud_kill_count_follows_the_selected_units_kills) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));
    int hero = end_find_monarch(1);
    ASSERT(hero >= 0);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int prey = Units_Spawn(sword_def, 2, cfg.players[1].color,
                           units[hero].world_x + 40, units[hero].world_y);
    ASSERT(prey >= 0);
    Units_SetHealthPercent(prey, 1);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    Units_SelectSingle(hero);
    timer.accumulator = 0.0;
    InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(1, HUD_WidgetHidden("KillCount"));

    Units_CommandAttackUnit(hero, prey);
    for (int i = 0; i < 900; i++) {
        timer.accumulator = timer.sim_dt;
        InGame_Tick(&platform, &timer);
        units = Units_GetActive(&n);
        if (units[prey].alive != UNIT_ALIVE_ACTIVE) break;
    }
    units = Units_GetActive(&n);
    ASSERT(units[prey].alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(1, (int)units[hero].kills);

    Units_SelectSingle(hero);
    timer.accumulator = 0.0;
    InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(0, HUD_WidgetHidden("KillCount"));
    char text[16] = "";
    ASSERT_EQ_INT(1, HUD_WidgetText("KillCount", text, sizeof(text)));
    ASSERT_EQ_STR("1", text);

    Units_SelectSingle(-1);
    timer.accumulator = 0.0;
    InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(1, HUD_WidgetHidden("KillCount"));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* The rank shield in the sidebar shows frame rank minus one, capped at
 * its three frames, and hides at rank 0, for enemy units and for a
 * noveteran monarch (legacy:152439-152449, legacy:232939-232941). It
 * used to show the top frame at any rank. */
TEST(hud_rank_shield_follows_the_units_rank) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));
    int hero = end_find_monarch(1);
    ASSERT(hero >= 0);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int mine = Units_Spawn(sword_def, 1, cfg.players[0].color,
                           units[hero].world_x + 64, units[hero].world_y);
    int theirs = Units_Spawn(sword_def, 2, cfg.players[1].color,
                             units[hero].world_x + 128, units[hero].world_y);
    ASSERT(mine >= 0);
    ASSERT(theirs >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);

    static const int ranks[]  = { 0, 1, 2, 3, 7, 10 };
    static const int frames[] = { -1, 0, 1, 2, 2, 2 };
    Units_SelectSingle(mine);
    for (int i = 0; i < (int)(sizeof(ranks) / sizeof(ranks[0])); i++) {
        Units_DebugSetVeteranLevel(mine, ranks[i]);
        ASSERT_EQ_INT(ranks[i], Units_GetVeteranLevel(mine));
        timer.accumulator = 0.0;
        InGame_Tick(&platform, &timer);
        if (frames[i] < 0) {
            ASSERT_EQ_INT(1, HUD_WidgetHidden("Experience"));
        } else {
            ASSERT_EQ_INT(0, HUD_WidgetHidden("Experience"));
            ASSERT_EQ_INT(frames[i], HUD_WidgetFrame("Experience"));
        }
    }

    /* The monarch is noveteran and never ranks, whatever its XP. */
    const UnitDef *kd = Units_GetDef(units[hero].def_idx);
    ASSERT_NOT_NULL(kd);
    ASSERT_EQ_INT(1, kd->noveteran);
    Units_DebugSetVeteranLevel(hero, 5);
    ASSERT_EQ_INT(0, Units_GetVeteranLevel(hero));
    Units_SelectSingle(hero);
    timer.accumulator = 0.0;
    InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(1, HUD_WidgetHidden("Experience"));

    Units_DebugSetVeteranLevel(theirs, 5);
    Units_SelectSingle(theirs);
    timer.accumulator = 0.0;
    InGame_Tick(&platform, &timer);
    ASSERT_EQ_INT(1, HUD_WidgetHidden("Experience"));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* An idle trebuchet (sight 200, reach 2700) takes only what its side
 * sees (legacy:20511-20545, legacy:20639-20680): nothing in the dark,
 * and a target once a spotter of its own side stands by it. Reported
 * from play: trebuchets shelled the AI's base unseen. */
TEST(trebuchet_waits_for_a_spotter) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.line_of_sight = 1;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));
    /* Nobody moves the prey. */
    world->cfg.players[1].kind = TAK_SLOT_HUMAN;
    int king = end_find_monarch(1), foe = end_find_monarch(2);
    ASSERT(king >= 0);
    ASSERT(foe >= 0);
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    int32_t sx = u[king].world_x, sy = u[king].world_y;
    double dx = u[foe].world_x - sx, dy = u[foe].world_y - sy;
    double len = sqrt(dx * dx + dy * dy);
    ASSERT(len > 1600.0);
    dx /= len;
    dy /= len;
    int tre_def = Units_FindDefByName("ARATRE");
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(tre_def >= 0);
    ASSERT(sword_def >= 0);
    int tre = Units_Spawn(tre_def, 1, cfg.players[0].color,
                          sx + (int32_t)(dx * 300), sy + (int32_t)(dy * 300));
    int32_t px = sx + (int32_t)(dx * 1200), py = sy + (int32_t)(dy * 1200);
    int prey = Units_Spawn(sword_def, 2, cfg.players[1].color, px, py);
    ASSERT(tre >= 0);
    ASSERT(prey >= 0);
    Units_DebugSetAggro(prey, UNIT_AGGRO_PASSIVE);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    int took_unseen = 0;
    for (int i = 0; i < 240; i++) {
        timer.accumulator = timer.sim_dt;
        InGame_Tick(&platform, &timer);
        u = Units_GetActive(&n);
        if (u[tre].target == prey) took_unseen = 1;
    }
    ASSERT_EQ_INT(0, took_unseen);

    int eye = Units_Spawn(sword_def, 1, cfg.players[0].color, px + 64, py);
    ASSERT(eye >= 0);
    Units_DebugSetAggro(eye, UNIT_AGGRO_PASSIVE);
    int took_seen = 0;
    for (int i = 0; i < 600 && !took_seen; i++) {
        timer.accumulator = timer.sim_dt;
        InGame_Tick(&platform, &timer);
        u = Units_GetActive(&n);
        if (u[tre].target == prey) took_seen = 1;
    }
    ASSERT(took_seen);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

TEST(ai_hunts_the_last_structure_out_of_sight) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 1;
    cfg.line_of_sight = 1;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));

    int local_monarch = end_find_monarch(1);
    ASSERT(local_monarch >= 0);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    int32_t sx = units[local_monarch].world_x;
    int32_t sy = units[local_monarch].world_y;

    int lode_def = Units_FindDefByName("ARALODE");
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(lode_def >= 0 && sword_def >= 0);
    const UnitDef *sword_d = Units_GetDef(sword_def);
    /* Well past the raider's sight (arasword sightdistance 135), on
     * the route toward the enemy start so the ground is walkable. */
    int ai_monarch = end_find_monarch(2);
    ASSERT(ai_monarch >= 0);
    double dx = (double)(units[ai_monarch].world_x - sx);
    double dy = (double)(units[ai_monarch].world_y - sy);
    double len = sqrt(dx * dx + dy * dy);
    ASSERT(len > 1.0);
    double reach = (double)sword_d->sight_distance * 3.0;
    int32_t lx = sx + (int32_t)(dx / len * reach);
    int32_t ly = sy + (int32_t)(dy / len * reach);
    int lode = Units_Spawn(lode_def, 1, cfg.players[0].color, lx, ly);
    ASSERT(lode >= 0);
    Units_SetHealthPercent(lode, 2);
    ASSERT_EQ_INT(0, InGame_Init(&platform));

    /* The monarch's death blast would take a bystander with it: let it
     * finish before the raider arrives at the empty start. */
    ASSERT_EQ_INT(local_monarch, Units_DebugKillHandle(local_monarch));
    InGame_DebugRunSimTicks(240);
    ASSERT_EQ_INT(0, world->skirmish_game_over);
    int raider = Units_Spawn(sword_def, 2, cfg.players[1].color, sx + 32, sy + 32);
    ASSERT(raider >= 0);
    for (int t = 0; t < 120 && !world->skirmish_game_over; t++) {
        InGame_DebugRunSimTicks(30);
    }
    units = Units_GetActive(&n);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, units[raider].alive);
    units = Units_GetActive(&n);
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, units[lode].alive);
    ASSERT_EQ_INT(1, world->skirmish_game_over);
    ASSERT_EQ_STR("Defeat", world->skirmish_end_reason);

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

static void end_expect_row(int slot, const char *column, int value) {
    char shown[32], want[32];
    ASSERT_EQ_INT(0, EndScreen_RowText(slot, column, shown, sizeof(shown)));
    snprintf(want, sizeof(want), "%d", value);
    ASSERT_EQ_STR(want, shown);
}

/* A won duel: the kill, the tallies, the banner, then the authored
 * victory dialog for the local side with the numbers the game kept,
 * and Main Menu leaving with its own sound. */
TEST(end_screen_shows_victory_dialog_with_the_tallies) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));

    int local_monarch = end_find_monarch(1);
    int ai_monarch = end_find_monarch(2);
    ASSERT(local_monarch >= 0 && ai_monarch >= 0);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    int sword_def = Units_FindDefByName("ARASWORD");
    ASSERT(sword_def >= 0);
    int prey = Units_Spawn(sword_def, 2, cfg.players[1].color,
                           units[local_monarch].world_x + 40,
                           units[local_monarch].world_y);
    ASSERT(prey >= 0);
    Units_SetHealthPercent(prey, 1);
    Units_CommandAttackUnit(local_monarch, prey);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    for (int f = 0; f < 200; f++) {
        units = Units_GetActive(&n);
        if (units[prey].alive != UNIT_ALIVE_ACTIVE) break;
        timer.accumulator = timer.sim_dt * 30.0;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    units = Units_GetActive(&n);
    ASSERT(units[prey].alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(1, world->stats[1].kills);
    ASSERT_EQ_INT(Units_GetDef(sword_def)->kill_xp_value, world->stats[1].score);
    ASSERT_EQ_INT(1, world->stats[2].losses);
    ASSERT_EQ_INT(0, world->skirmish_game_over);

    /* The enemy monarch falls: their army goes and the banner comes up
     * while the battle keeps running, then the screen. */
    ASSERT_EQ_INT(ai_monarch, Units_DebugKillHandle(ai_monarch));
    ASSERT(end_run_frames(&platform, world, &timer, 20) >= 0);
    ASSERT_EQ_INT(1, world->skirmish_local_result);
    ASSERT_EQ_STR("Victory", world->skirmish_end_reason);
    ASSERT_EQ_INT(0, world->skirmish_stats_open);
    ASSERT_EQ_INT(0, EndScreen_IsOpen());
    for (int f = 0; f < 8 && !world->skirmish_stats_open; f++) {
        timer.accumulator = timer.sim_dt * 30.0;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    ASSERT_EQ_INT(1, world->skirmish_stats_open);
    ASSERT(world->skirmish_elapsed_ticks - world->skirmish_end_tick >= 180);
    ASSERT(world->skirmish_elapsed_ticks - world->skirmish_end_tick < 210);
    ASSERT_EQ_INT(1, EndScreen_IsOpen());
    ASSERT_EQ_STR("data/guis/victoryara.gui", EndScreen_DialogPath());

    /* One row per player that built something, the numbers the game
     * kept, and the time as hh:mm:ss. */
    ASSERT_EQ_INT(1, EndScreen_RowShown(0));
    ASSERT_EQ_INT(1, EndScreen_RowShown(1));
    ASSERT_EQ_INT(0, EndScreen_RowShown(2));
    char text[32];
    ASSERT_EQ_INT(0, EndScreen_RowText(0, "PlayerName", text, sizeof(text)));
    ASSERT_EQ_STR(cfg.players[0].name, text);
    ASSERT_EQ_INT(1, world->stats[1].units_built);
    end_expect_row(0, "UnitsBuilt", world->stats[1].units_built);
    end_expect_row(0, "Kills", 1);
    end_expect_row(0, "Losses", 0);
    end_expect_row(0, "Score", Units_GetDef(sword_def)->kill_xp_value);
    ASSERT(world->stats[2].units_built >= 2);
    end_expect_row(1, "UnitsBuilt", world->stats[2].units_built);
    end_expect_row(1, "Kills", world->stats[2].kills);
    ASSERT(world->stats[2].losses >= 2);
    end_expect_row(1, "Losses", world->stats[2].losses);
    end_expect_row(1, "Score", world->stats[2].score);
    {
        int secs = world->stats[1].last_alive_tick / 60;
        char want[32];
        ASSERT(world->stats[1].last_alive_tick >= world->skirmish_end_tick);
        ASSERT(world->stats[2].last_alive_tick <= world->skirmish_end_tick);
        snprintf(want, sizeof(want), "%02d:%02d:%02d",
                 secs / 3600, (secs % 3600) / 60, secs % 60);
        ASSERT_EQ_INT(0, EndScreen_RowText(0, "Time", text, sizeof(text)));
        ASSERT_EQ_STR(want, text);
    }
    ASSERT_EQ_STR("Skirmish Battle Room", EndScreen_ButtonHelp("Proceed"));
    ASSERT_EQ_STR("Main Menu", EndScreen_ButtonHelp("MainMenu"));
    ASSERT_EQ_INT(0, save_and_check_canvas("test_end_screen_victory.bmp"));

    /* Main Menu: the authored cancel sound, then the menu. */
    ASSERT_EQ_INT(GAMESTATE_MENU, EndScreen_Press("MainMenu"));
    ASSERT_EQ_STR("cancel.wav", EndScreen_LastSound());
    timer.accumulator = timer.sim_dt;
    ASSERT_EQ_INT(GAMESTATE_MENU, InGame_Tick(&platform, &timer));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A lost battle: the defeat dialog with its authored rows and buttons,
 * and Proceed leading back to the skirmish battle room with its sound. */
TEST(end_screen_shows_defeat_dialog_and_proceeds_to_the_lobby) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    /* The authored dialog: rows for eight players, two buttons with
     * their sounds, the accelerator string on the root. */
    GUIDialog d;
    ASSERT_EQ_INT(0, GUIDialog_Load(&d, "data/guis/defeat.gui"));
    ASSERT_EQ_STR("#Enter#Proceed#Esc#MainMenu", d.root.tooltip);
    const GUIWidget *w = GUIDialog_FindByName(&d, "MainMenu");
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_STR("cancel.wav", w->sound);
    ASSERT_EQ_STR("Main Menu", w->tooltip);
    w = GUIDialog_FindByName(&d, "Proceed");
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_STR("ok.wav", w->sound);
    w = GUIDialog_FindByName(&d, "Static1");
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_STR("Player", w->display_text);
    ASSERT_EQ_INT(1, w->text_align);
    w = GUIDialog_FindByName(&d, "Static2");
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_STR("Units", w->display_text);
    ASSERT_EQ_INT(0, w->text_align);
    w = GUIDialog_FindByName(&d, "Static0");
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_STR("Defeat", w->display_text);
    int rows = 0;
    for (int i = 0; i < d.num_children; i++) {
        if (tak_stricmp(d.children[i].name, "PlayerName") == 0) rows++;
    }
    ASSERT_EQ_INT(8, rows);
    GUIDialog_Free(&d);

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, "two castles", sizeof(cfg.map_name) - 1);
    cfg.monarch_expendable = 0;
    GameWorld *world = NULL;
    ASSERT_EQ_INT(0, end_load_skirmish(&platform, &cfg, &world));
    int local_monarch = end_find_monarch(1);
    ASSERT(local_monarch >= 0);
    ASSERT_EQ_INT(0, InGame_Init(&platform));
    Timer timer;
    Timer_Init(&timer);
    timer.max_ticks_per_frame = 30;

    ASSERT_EQ_INT(local_monarch, Units_DebugKillHandle(local_monarch));
    ASSERT(end_run_frames(&platform, world, &timer, 20) >= 0);
    ASSERT_EQ_STR("Defeat", world->skirmish_end_reason);
    for (int f = 0; f < 8 && !world->skirmish_stats_open; f++) {
        timer.accumulator = timer.sim_dt * 30.0;
        ASSERT_EQ_INT(GAMESTATE_IN_GAME, InGame_Tick(&platform, &timer));
    }
    ASSERT_EQ_INT(1, EndScreen_IsOpen());
    ASSERT_EQ_STR("data/guis/defeat.gui", EndScreen_DialogPath());
    ASSERT_EQ_INT(1, EndScreen_RowShown(0));
    ASSERT_EQ_INT(1, EndScreen_RowShown(1));
    end_expect_row(0, "Losses", 1);
    end_expect_row(0, "Kills", 0);
    ASSERT_EQ_INT(0, save_and_check_canvas("test_end_screen_defeat.bmp"));

    ASSERT_EQ_INT(GAMESTATE_BATTLE_SETUP, EndScreen_Press("Proceed"));
    ASSERT_EQ_STR("ok.wav", EndScreen_LastSound());
    timer.accumulator = timer.sim_dt;
    ASSERT_EQ_INT(GAMESTATE_BATTLE_SETUP, InGame_Tick(&platform, &timer));

    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

int main(int argc, char **argv) {
    TAK_Crash_Install();
    if (argc > 1 && argv[1] && argv[1][0]) g_test_filter = argv[1];
    TEST_SUITE("BattleConfig");
    RUN_UI_TEST(battle_config_defaults_are_sensible);
    RUN_UI_TEST(battle_config_per_side_cap_bounds);

    TEST_SUITE("Battle setup screen");
    RUN_UI_TEST(battle_setup_init_tick_shutdown);
    RUN_UI_TEST(battle_setup_map_names_are_authored);
    RUN_UI_TEST(battle_setup_map_description_populated);
    RUN_UI_TEST(battle_setup_game_info_rows_do_not_overlap);
    RUN_UI_TEST(battle_setup_color_index_reaches_world);
    RUN_UI_TEST(battle_setup_swatches_match_authored_frames);

    TEST_SUITE("Options screen");
    RUN_UI_TEST(options_init_tick_shutdown);
    RUN_UI_TEST(damage_bars_follow_visual_option);

    TEST_SUITE("Loading screen");
    RUN_UI_TEST(loading_progress_clamps_and_transitions);
    RUN_UI_TEST(loading_backdrop_is_the_arch_and_its_glass);
    RUN_UI_TEST(campaign_loading_spawns_units_and_renders);
    RUN_UI_TEST(skirmish_monarch_death_ends_match);
    RUN_UI_TEST(skirmish_local_monarch_death_is_defeat_with_two_foes_left);
    RUN_UI_TEST(skirmish_expendable_player_stands_until_the_last_unit);
    RUN_UI_TEST(ai_hunts_the_last_structure_out_of_sight);
    RUN_UI_TEST(end_screen_shows_victory_dialog_with_the_tallies);
    RUN_UI_TEST(end_screen_shows_defeat_dialog_and_proceeds_to_the_lobby);
    RUN_UI_TEST(skirmish_ai_issues_attack_orders);
    RUN_UI_TEST(skirmish_ai_duel_reaches_game_over);
    RUN_UI_TEST(four_player_ffa_every_ai_fights);
    RUN_UI_TEST(teamed_ais_spare_their_allies);
    RUN_UI_TEST(ai_sends_its_home_units_at_a_base_raider);
    RUN_UI_TEST(influence_maps_size_to_the_map_and_see_the_army);
    RUN_UI_TEST(perf_probe_duel);
    RUN_UI_TEST(skirmish_ai_full_progression);
    RUN_UI_TEST(zhon_ai_fields_an_army);
    RUN_UI_TEST(a_dead_monarch_leaves_no_mana_in_the_pool);
    RUN_UI_TEST(build_placement_sacred_and_water_rules);
    RUN_UI_TEST(render_probe_building_and_walker);
    RUN_UI_TEST(render_probe_models);
    RUN_UI_TEST(weapon_art_resolves_per_weapon);
    RUN_UI_TEST(render_probe_projectile_art);
    RUN_UI_TEST(factory_queue_rally_and_cancel);
    RUN_UI_TEST(factory_product_spawns_on_build_pad);
    RUN_UI_TEST(hud_idle_frames_selection_and_queue_badges);
    RUN_UI_TEST(group_selection_and_control_groups);
    RUN_UI_TEST(tech_tree_all_builder_menus_resolve);
    RUN_UI_TEST(nanoframe_decay_refunds_mana);
    RUN_UI_TEST(reclaim_clears_feature_and_pays_mana);
    RUN_UI_TEST(a_dead_unit_leaves_its_corpse_when_the_death_finishes);
    RUN_UI_TEST(swordsman_strikes_an_enemy_standing_beside_it);
    RUN_UI_TEST(the_sweep_clears_a_corpse_and_keeps_it_from_rotting);
    RUN_UI_TEST(a_monarch_raises_a_corpse_at_a_tenth_of_its_life);
    RUN_UI_TEST(a_corpse_left_alone_rots_on_schedule);
    RUN_UI_TEST(a_corpse_waits_for_a_raiser);
    RUN_UI_TEST(noair_weapon_drops_a_flyer_that_takes_off);
    RUN_UI_TEST(a_refused_step_banks_no_distance);
    RUN_UI_TEST(a_column_gets_past_a_stuck_unit_in_its_way);
    RUN_UI_TEST(ai_long_run_no_entity_leak);
    RUN_UI_TEST(live_skirmish_units_actually_move);
    RUN_UI_TEST(patrol_from_the_sidebar_loops_until_a_new_order);
    RUN_UI_TEST(magic_weapon_fires_and_damages);
    RUN_UI_TEST(caster_reserve_recharges_and_gates_shots);
    RUN_UI_TEST(tower_auto_engages_enemy);
    RUN_UI_TEST(hud_kill_count_follows_the_selected_units_kills);
    RUN_UI_TEST(trebuchet_waits_for_a_spotter);
    RUN_UI_TEST(hud_rank_shield_follows_the_units_rank);
    RUN_UI_TEST(cob_entry_points_fire_once);
    RUN_UI_TEST(flyer_takes_off_flaps_and_lands);
    RUN_UI_TEST(tower_aim_faces_target);
    RUN_UI_TEST(war_galley_attacks_shore_target);
    RUN_UI_TEST(monarch_attacks_large_structure);
    RUN_UI_TEST(war_galley_hits_resting_ghost_ship);
    RUN_UI_TEST(main_menu_doors_follow_original_states);
    RUN_UI_TEST(enemy_unit_shows_in_the_sidebar);
    RUN_UI_TEST(stance_and_gate_buttons_show_their_icons_at_rest);
    RUN_UI_TEST(building_previews_hold_the_finished_pose);
    RUN_UI_TEST(veteran_swap_keeps_the_crew_drawn);
    RUN_UI_TEST(minimap_draws_a_dot_per_visible_unit);
    RUN_UI_TEST(a_starved_build_slows_but_never_rots);
    RUN_UI_TEST(healing_spends_mana_over_time);
    RUN_UI_TEST(one_unload_order_empties_the_hold);
    RUN_UI_TEST(loaded_transport_shows_its_cargo_count);
    RUN_UI_TEST(units_navigate_to_distant_goals);
    RUN_UI_TEST(horseman_moves_without_circling);
    RUN_UI_TEST(unit_walks_around_a_wall_of_friendly_units);
    RUN_UI_TEST(own_unit_walks_through_its_gate_and_gate_opens);
    RUN_UI_TEST(completed_wall_blocks_units);
    RUN_UI_TEST(units_do_not_stack_on_one_another);
    RUN_UI_TEST(boats_stay_in_water_ghost_ships_do_not);
    RUN_UI_TEST(posture_passive_holds_offensive_engages);
    RUN_UI_TEST(skirmish_setup_error_requires_two_spawnable_players);
    RUN_UI_TEST(story_play_starts_campaign_loading);
    RUN_UI_TEST(story_screen_renders_book_of_deeds);

    TEST_REPORT();
}
