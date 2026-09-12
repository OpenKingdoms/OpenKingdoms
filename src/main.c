/*
 * main.c -- TAK-RE entry point
 *
 * Command-line flags
 * ------------------
 *   --help, -h           Print this flag list and exit.
 *   --width <px>         Initial window width  (default 1280, min 320).
 *   --height <px>        Initial window height (default  720, min 240).
 *   --fullscreen         Start in fullscreen-desktop mode.
 *   --windowed           Start windowed (default).
 *   --sw-renderer        Force SDL_RENDERER_SOFTWARE instead of accelerated.
 *                        Useful for debugging or systems without a GPU driver.
 *   --no-vsync           Disable vsync (default: enabled).
 *   --pixel-perfect      Lock canvas->window scale to integer multiples.
 *                        Default: off. When off, the 640×480 UI canvas
 *                        scales continuously with bilinear filtering so
 *                        it fills the window as the user resizes. When
 *                        on, scale snaps to 1×, 2×, 3× etc. with nearest
 *                        filtering; this can leave large letterbox bars
 *                        at window sizes between integer multiples.
 *
 * At runtime, Alt+Enter toggles fullscreen regardless of startup mode.
 */

#include "tak_platform.h"
#include "tak_settings.h"
#include "tak_gameloop.h"
#include "tak_memory.h"
#include "tak_hpi.h"
#include "tak_sound.h"
#include "tak_music.h"
#include "tak_soundclass.h"
#include "tak_game_sound.h"
#include "tak_ui.h"
#include "tak_main_menu.h"
#include "tak_battle_setup.h"
#include "tak_ingame.h"
#include "tak_ingame_menu.h"
#include "tak_options.h"
#include "tak_loading.h"
#include "tak_credits.h"
#include "tak_story.h"
#include "tak_multiplayer.h"
#include "tak_world.h"
#include "tak_camera.h"
#include "tak_cursor.h"
#include "tak_crash.h"
#include "tak_perf_probe.h"
#include "tak_dataset.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* All state the per-frame loop iteration needs. Native builds drive
 * app_frame() from a while loop; Emscripten drives it from the
 * browser's requestAnimationFrame via emscripten_set_main_loop_arg
 * (the browser owns the loop — a blocking while() would hang the
 * tab). */
typedef struct AppState {
    TAK_Platform platform;
    Timer        timer;
    GameStateEnum state;
    int menu_initialized;
    int battle_setup_initialized;
    int options_initialized;
    int loading_initialized;
    int credits_initialized;
    int story_initialized;
    int multiplayer_initialized;
    int ingame_initialized;
    int quit_requested;
} AppState;

static void print_help(const char *prog) {
    printf(
        "Usage: %s [flags]\n"
        "\n"
        "Display flags:\n"
        "  --width <px>        initial window width  (default 1280, min 320)\n"
        "  --height <px>       initial window height (default  720, min 240)\n"
        "  --fullscreen        start fullscreen-desktop (Alt+Enter to toggle)\n"
        "  --windowed          start windowed (default)\n"
        "  --sw-renderer       force SDL software renderer\n"
        "  --no-vsync          disable vsync (default: enabled)\n"
        "  --pixel-perfect     snap canvas scale to integer multiples\n"
        "                      (default: continuous bilinear scaling)\n"
        "  --skirmish          skip the menus: start a skirmish with the\n"
        "                      default lineup on the first map (testing)\n"
        "  --perf-probe <name> run a performance scenario (ffa, crowd)\n"
        "                      and print one line per 600 sim ticks\n"
        "  --perf-ticks <n>    shorten that scenario to n sim ticks\n"
        "  -pretendnoexpansion play an Iron Plague install as the base game\n"
        "  --help, -h          print this help and exit\n",
        prog ? prog : "tak-re");
}

static int g_start_skirmish = 0;   /* --skirmish */
static const char *g_perf_scenario = NULL;   /* --perf-probe */
static int g_perf_ticks = 0;                 /* --perf-ticks */

/* Populate cfg from command-line flags. Returns 1 if main should
 * continue, 0 if we should exit early (e.g. --help printed). */
static int parse_cli(int argc, char **argv, TAK_DisplayConfig *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(a, "--width") == 0 && i + 1 < argc) {
            cfg->window_w = atoi(argv[++i]);
        } else if (strcmp(a, "--height") == 0 && i + 1 < argc) {
            cfg->window_h = atoi(argv[++i]);
        } else if (strcmp(a, "--fullscreen") == 0) {
            cfg->fullscreen = 1;
        } else if (strcmp(a, "--windowed") == 0) {
            cfg->fullscreen = 0;
        } else if (strcmp(a, "--sw-renderer") == 0) {
            cfg->use_sw_renderer = 1;
        } else if (strcmp(a, "--no-vsync") == 0) {
            cfg->vsync = 0;
        } else if (strcmp(a, "--pixel-perfect") == 0) {
            cfg->pixel_perfect = 1;
        } else if (strcmp(a, "--skirmish") == 0) {
            g_start_skirmish = 1;
        } else if (strcmp(a, "--perf-probe") == 0 && i + 1 < argc) {
            g_perf_scenario = argv[++i];
        } else if (strcmp(a, "--perf-ticks") == 0 && i + 1 < argc) {
            g_perf_ticks = atoi(argv[++i]);
        } else if ((a[0] == '-' || a[0] == '/') &&
                   tak_stricmp(a + (a[1] == '-' ? 2 : 1),
                               "pretendnoexpansion") == 0) {
            /* Taken after '-' or '/' (legacy:251988-251990). */
            TAK_DataSet_SetPretendNoExpansion(1);
        } else {
            fprintf(stderr, "Unknown flag: %s (use --help for list)\n", a);
        }
    }
    if (g_perf_scenario) {
        if (PerfProbe_Select(g_perf_scenario) != 0) {
            fprintf(stderr, "Unknown perf probe scenario: %s (ffa, crowd)\n",
                    g_perf_scenario);
            return 0;
        }
        PerfProbe_SetTicks(g_perf_ticks);
        /* Frame time is the work, not the wait for the display. */
        cfg->vsync = 0;
    }
    if (cfg->window_w < 320) cfg->window_w = 320;
    if (cfg->window_h < 240) cfg->window_h = 240;
    return 1;
}

/* One frame of the game: update subsystems, run the active screen's
 * tick, present. Shared verbatim between the native while-loop and the
 * browser's requestAnimationFrame callback. */
static double perf_now_ms(void) {
    return (double)SDL_GetPerformanceCounter() * 1000.0 /
           (double)SDL_GetPerformanceFrequency();
}

static void app_frame(AppState *app) {
    double frame_t0 = PerfProbe_Active() ? perf_now_ms() : 0.0;
    Timer_Update(&app->timer);
    TAK_Sound_Update();
    TAK_Music_Update();
    Cursor_Tick();
    TAK_Platform_FrameBegin(&app->platform);

    switch(app->state) {
        case GAMESTATE_QUIT:
            app->quit_requested = 1;
            break;

        case GAMESTATE_MENU: {
            if (!app->menu_initialized) {
                if (MainMenu_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize main menu\n");
                    app->quit_requested = 1;
                    break;
                }
                app->menu_initialized = 1;
            }
            int next_state = MainMenu_Tick(&app->platform, (float)app->timer.frame_dt);
            if (next_state != GAMESTATE_MENU) {
                MainMenu_Shutdown();
                app->menu_initialized = 0;
                app->state = next_state;
            }
            break;
        }

        case GAMESTATE_BATTLE_SETUP: {
            if (!app->battle_setup_initialized) {
                if (BattleSetup_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize battle setup — returning to menu\n");
                    app->state = GAMESTATE_MENU;
                    break;
                }
                app->battle_setup_initialized = 1;
            }
            int next_state = BattleSetup_Tick(&app->platform, (float)app->timer.frame_dt);
            if (next_state != GAMESTATE_BATTLE_SETUP) {
                BattleSetup_Shutdown();
                app->battle_setup_initialized = 0;
                app->state = next_state;
            }
            break;
        }

        case GAMESTATE_OPTIONS: {
            if (!app->options_initialized) {
                if (Options_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize options — returning to menu\n");
                    app->state = GAMESTATE_MENU;
                    break;
                }
                app->options_initialized = 1;
            }
            int next_state = Options_Tick(&app->platform, (float)app->timer.frame_dt);
            if (next_state != GAMESTATE_OPTIONS) {
                Options_Shutdown();
                app->options_initialized = 0;
                app->state = next_state;
            }
            break;
        }

        case GAMESTATE_CREDITS: {
            if (!app->credits_initialized) {
                if (Credits_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize credits — returning to menu\n");
                    app->state = GAMESTATE_MENU;
                    break;
                }
                app->credits_initialized = 1;
            }
            int next_state = Credits_Tick(&app->platform, (float)app->timer.frame_dt);
            if (next_state != GAMESTATE_CREDITS) {
                Credits_Shutdown();
                app->credits_initialized = 0;
                app->state = next_state;
            }
            break;
        }

        case GAMESTATE_GAME_LOADING: {
            if (!app->loading_initialized) {
                if (Loading_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize loading — returning to menu\n");
                    app->state = GAMESTATE_MENU;
                    break;
                }
                app->loading_initialized = 1;
            }
            /* Loading_Tick is now the sole driver of progress. It
             * advances its internal state machine one phase per call
             * and flips to IN_GAME when the final phase (LS_DONE) is
             * reached. No wall-clock ramp, no external SetProgress. */
            int next_state = Loading_Tick(&app->platform, (float)app->timer.frame_dt);
            if (next_state != GAMESTATE_GAME_LOADING) {
                Loading_Shutdown();
                app->loading_initialized = 0;
                app->state = next_state;
            }
            break;
        }

        case GAMESTATE_CAMPAIGN: {
            if (!app->story_initialized) {
                if (Story_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize Story — back to menu\n");
                    app->state = GAMESTATE_MENU;
                    break;
                }
                app->story_initialized = 1;
            }
            int next_state = Story_Tick(&app->platform, (float)app->timer.frame_dt);
            if (next_state != GAMESTATE_CAMPAIGN) {
                Story_Shutdown();
                app->story_initialized = 0;
                app->state = next_state;
            }
            break;
        }

        case GAMESTATE_MULTIPLAYER: {
            if (!app->multiplayer_initialized) {
                if (Multiplayer_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize Multiplayer — back to menu\n");
                    app->state = GAMESTATE_MENU;
                    break;
                }
                app->multiplayer_initialized = 1;
            }
            int next_state = Multiplayer_Tick(&app->platform, (float)app->timer.frame_dt);
            if (next_state != GAMESTATE_MULTIPLAYER) {
                Multiplayer_Shutdown();
                app->multiplayer_initialized = 0;
                app->state = next_state;
            }
            break;
        }

        case GAMESTATE_IN_GAME: {
            if (!app->ingame_initialized) {
                if (InGame_Init(&app->platform) != 0) {
                    fprintf(stderr, "Failed to initialize in-game — back to menu\n");
                    World_End(&app->platform);
                    app->state = GAMESTATE_MENU;
                    break;
                }
                app->ingame_initialized = 1;
            }
            int next_state = InGame_Tick(&app->platform, &app->timer);
            if (next_state != GAMESTATE_IN_GAME) {
                BattleConfig again;
                char again_map[96], again_kingdom[32];
                int restart = InGameMenu_TakeRestart(&again,
                                                     again_map, sizeof(again_map),
                                                     again_kingdom, sizeof(again_kingdom));
                InGame_Shutdown();
                /* Release the loaded map before leaving — next Play
                 * click will World_BeginLoad() a fresh world. */
                World_End(&app->platform);
                app->ingame_initialized = 0;
                app->state = next_state;
                /* Restart plays the same battle again (legacy:156329-156336). */
                if (restart && World_BeginLoad(&app->platform, &again,
                                               again_map, again_kingdom) == 0) {
                    app->state = GAMESTATE_GAME_LOADING;
                }
            }
            break;
        }
    }

    TAK_Platform_Present(&app->platform);

    char title[128];
    double fps = (app->timer.frame_dt > 0.0) ? 1.0 / app->timer.frame_dt : 0.0;
    const char *name =
        app->state == GAMESTATE_MENU          ? "Main Menu"      :
        app->state == GAMESTATE_BATTLE_SETUP  ? "Skirmish Lobby" :
        app->state == GAMESTATE_GAME_LOADING  ? "Loading"        :
        app->state == GAMESTATE_IN_GAME       ? "In Game"        :
        app->state == GAMESTATE_OPTIONS       ? "Options"        :
        app->state == GAMESTATE_CREDITS       ? "Credits"        :
        app->state == GAMESTATE_CAMPAIGN      ? "Campaign"       :
        app->state == GAMESTATE_MULTIPLAYER   ? "Multiplayer"    :
        app->state == GAMESTATE_QUIT          ? "Exiting"        :
                                                "Unknown";
    snprintf(title, sizeof(title), "TAK-RE | %s | fps: %.1f", name, fps);
    SDL_SetWindowTitle(app->platform.window, title);

    if (PerfProbe_Active()) PerfProbe_EndFrame(perf_now_ms() - frame_t0);
    if (PerfProbe_Finished()) app->quit_requested = 1;
}

#ifdef __EMSCRIPTEN__
/* Browser frame callback: pump events, run one frame, cancel the loop
 * on quit (the tab reclaims resources; no explicit teardown). */
static void em_frame(void *arg) {
    AppState *app = (AppState *)arg;
    if (app->quit_requested || !TAK_Platform_PumpEvents(&app->platform)) {
        emscripten_cancel_main_loop();
        return;
    }
    app_frame(app);
}
#endif

/* App state must outlive main() under Emscripten (main returns while
 * the browser keeps calling em_frame). */
static AppState g_app;

int main(int argc, char *argv[]) {
    /* Parse args before touching VFS or SDL so --help doesn't pay the
     * cost of loading HPI archives. */
    TAK_DisplayConfig cfg = TAK_DisplayConfig_Default();
    if (!parse_cli(argc, argv, &cfg)) return 0;

    TAK_Crash_Install();
    tak_mem_init();
    Settings_Load();
    Camera_ResetDefaults();

    /* Initialize VFS so we can load game assets from HPI archives */
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "Failed to initialize VFS\n");
        tak_mem_shutdown();
        return 1;
    }

    /* Initialize sound system (non-fatal — game works without audio) */
    if (TAK_Sound_Init() != 0) {
        fprintf(stderr, "Warning: sound system unavailable\n");
    }

    /* Initialize music streaming (non-fatal — scans Music/ for tracks) */
    if (TAK_Music_Init(TAK_GAME_DIR) != 0) {
        fprintf(stderr, "Warning: music system unavailable\n");
    } else {
        TAK_Music_SetMode(TAK_MUSIC_SEQUENTIAL);
    }

    /* Load sound class definitions from gamedata/soundclasses/ TDFs.
     * Non-fatal — unit sounds just won't play if this fails. */
    SoundClass_LoadAll();

    /* Game-level sound dispatcher (caches loaded WAVs, bridges events
     * to the sound API). */
    GameSound_Init();

    memset(&g_app, 0, sizeof(g_app));
    g_app.state = GAMESTATE_MENU;
    if (g_start_skirmish) {
        g_app.state = GAMESTATE_BATTLE_SETUP;
        BattleSetup_RequestAutoStart();
    }

    if (TAK_Platform_Init(&g_app.platform, &cfg) != 0) {
        fprintf(stderr, "Failed to initialize platform\n");
        VFS_Shutdown();
        tak_mem_shutdown();
        return 1;
    }

    Timer_Init(&g_app.timer);

    /* Shared 640x480 offscreen compositing surface for every screen. */
    if (UI_Init() != 0) {
        fprintf(stderr, "Failed to initialize UI\n");
        TAK_Platform_Shutdown(&g_app.platform);
        VFS_Shutdown();
        tak_mem_shutdown();
        return 1;
    }

    /* Load custom cursors from cursors.gaf. Non-fatal — falls back to
       the default OS cursor if the GAF is missing or corrupt. */
    if (Cursor_Init() != 0) {
        fprintf(stderr, "Warning: custom cursors unavailable, using OS default\n");
    }

    /* --perf-probe owns the battle: build it here and go straight
     * to loading, so every run plays the same scenario. */
    if (PerfProbe_Active()) {
        if (PerfProbe_BeginWorld(&g_app.platform) == 0) {
            g_app.state = GAMESTATE_GAME_LOADING;
        } else {
            g_app.quit_requested = 1;
        }
    }

#ifdef __EMSCRIPTEN__
    /* fps=0 → drive from requestAnimationFrame; simulate_infinite_loop
     * so main() unwinds here and the browser owns the loop. */
    emscripten_set_main_loop_arg(em_frame, &g_app, 0, 1);
#else
    while (!g_app.quit_requested && TAK_Platform_PumpEvents(&g_app.platform)) {
        app_frame(&g_app);
    }
#endif

    if (g_app.menu_initialized)          MainMenu_Shutdown();
    if (g_app.battle_setup_initialized)  BattleSetup_Shutdown();
    if (g_app.options_initialized)       Options_Shutdown();
    if (g_app.loading_initialized)       Loading_Shutdown();
    if (g_app.credits_initialized)       Credits_Shutdown();
    if (g_app.story_initialized)         Story_Shutdown();
    if (g_app.multiplayer_initialized)   Multiplayer_Shutdown();
    if (g_app.ingame_initialized)        InGame_Shutdown();

    /* World_End has to run before TAK_Platform_Shutdown — it releases
     * terrain-grid GPU textures and the minimap texture via the
     * renderer, which TAK_Platform_Shutdown destroys. Running in the
     * wrong order left the SDL_Textures as stale handles and
     * SDL_DestroyTexture crashed post-SDL_Quit. */
    World_End(&g_app.platform);
    Cursor_Shutdown();
    UI_Shutdown();
    TAK_Platform_Shutdown(&g_app.platform);
    GameSound_Shutdown();
    SoundClass_FreeAll();
    TAK_Music_Shutdown();
    TAK_Sound_Shutdown();
    VFS_Shutdown();
    tak_mem_shutdown();

    return 0;
}
