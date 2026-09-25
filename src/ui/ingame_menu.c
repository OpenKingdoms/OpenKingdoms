/*
 * ingame_menu.c -- The F1 menu a battle opens over itself.
 *
 * F1 is bound to F2Menu (keys.tdf:169, loader legacy:122746-122785).
 * The dialog is picked by mode (legacy:154643-154655) and sits over the
 * running battle, so the terrain and the sidebar stay drawn behind it.
 * Enter and Escape press whatever the root's accelerator string names,
 * "#Enter#Resume#Esc#Resume", read the way the end screen reads its
 * own. Resume closes it (legacy:154721-154726). Load Game and Save
 * Game open the original's own dialogs over the menu, which stays
 * drawn behind them, the way Options does (legacy:154703-154712).
 * Game Information opens the original's own dialog over the menu the
 * same way (legacy:154864-154930).
 */

#include "tak_ingame_menu.h"
#include "tak_ingame.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_font.h"
#include "tak_hud.h"
#include "tak_ui.h"
#include "tak_world.h"
#include "tak_options.h"
#include "tak_game_info.h"
#include "tak_save_browser.h"
#include "tak_savegame.h"
#include "tak_loading.h"
#include "tak_game_sound.h"
#include "tak_gameloop.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    IGM_SINGLE = 0,
    IGM_SKIRMISH,
    IGM_MULTI
} IGMMode;

static struct {
    int         open;
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    Font       *font_help;
    int         off_x;      /* the dialog stands in the middle of the play area */
    int         off_y;
    char        path[128];
    char        enter_widget[32];
    char        esc_widget[32];
    char        last_sound[64];
    int         prev_enter;
    int         prev_esc;
    int         prev_paused;
    int         options_open;
    int         info_open;
    int         browser_open;
} m;

/* A restart request outlives the dialog that made it, so the frame loop
 * can pick it up once the menu is gone. */
static struct {
    int          pending;
    BattleConfig cfg;
    char         map[96];
    char         kingdom[32];
} igm_restart;

static void set_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

/* We have no netplay, so a battle is a campaign mission when the map
 * carried one and a skirmish otherwise. */
static IGMMode igm_mode(void) {
    const GameWorld *w = World_Get();
    if (w && (w->mission.objective_count > 0 || w->mission.placement_count > 0))
        return IGM_SINGLE;
    return IGM_SKIRMISH;
}

/* The exit submenu (legacy:156257-156272). The multiplayer file has no
 * Restart button, which is what keeps a restart to single player and
 * skirmish. */
static const char *exit_file(void) {
    return igm_mode() == IGM_MULTI ? "data/guis/multiplayerexitmenu.gui"
                                   : "data/guis/singleplayerexitmenu.gui";
}

static const char *menu_file(void) {
    switch (igm_mode()) {
    case IGM_SINGLE: return "data/guis/f2menusingleplayer.gui";
    case IGM_MULTI:  return "data/guis/f2menumultiplayer.gui";
    default:         return "data/guis/f2menuskirmish.gui";
    }
}

/* Root tooltip "#Enter#Resume#Esc#Resume": key, widget pairs. */
static void parse_accelerators(const char *spec) {
    m.enter_widget[0] = m.esc_widget[0] = '\0';
    if (!spec) return;
    char buf[128];
    set_text(buf, sizeof(buf), spec);
    char *p = buf;
    while (*p == '#') p++;
    while (*p) {
        char *key = p;
        char *sep = strchr(key, '#');
        if (!sep) break;
        *sep = '\0';
        char *widget = sep + 1;
        char *next = strchr(widget, '#');
        if (next) *next = '\0';
        if (tak_stricmp(key, "Enter") == 0) set_text(m.enter_widget, sizeof(m.enter_widget), widget);
        if (tak_stricmp(key, "Esc") == 0)   set_text(m.esc_widget, sizeof(m.esc_widget), widget);
        if (!next) break;
        p = next + 1;
    }
}

static void free_dialog(void) {
    if (m.rt) { GUIRuntime_Destroy(m.rt); m.rt = NULL; }
    if (m.has_dialog) { GUIDialog_Free(&m.dialog); m.has_dialog = 0; }
    m.path[0] = '\0';
}

static int load_dialog(const char *path) {
    free_dialog();
    if (GUIDialog_Load(&m.dialog, path) != 0) {
        fprintf(stderr, "InGameMenu: failed to load %s\n", path);
        return -1;
    }
    m.has_dialog = 1;
    m.rt = GUIRuntime_Create(&m.dialog);
    if (!m.rt) { free_dialog(); return -1; }
    set_text(m.path, sizeof(m.path), path);
    parse_accelerators(m.dialog.root.tooltip);

    /* The menu stands in the middle of the play area, not of the screen:
     * the sidebar and the bottom strip are not ground it may cover. */
    SDL_Rect area;
    HUD_DialogArea(&area);
    GUI_CenterOffset(&m.dialog, area, &m.off_x, &m.off_y);
    GUIRuntime_SetOffset(m.rt, m.off_x, m.off_y);

    m.prev_enter = m.prev_esc = 0;
    return 0;
}

int InGameMenu_Open(void) {
    if (m.open) return 0;
    memset(&m, 0, sizeof(m));
    if (load_dialog(menu_file()) != 0) return -1;
    m.font_help = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    /* The clock stops while the menu is up in single player and in
     * skirmish, and keeps running in multiplayer (legacy:145870-145873,
     * legacy:145921-145924). We have no netplay, so the multiplayer
     * branch is written and never taken. */
    if (igm_mode() != IGM_MULTI) {
        m.prev_paused = InGame_IsPaused();
        InGame_SetPaused(1);
    }
    m.open = 1;
    return 0;
}

void InGameMenu_Close(void) {
    int was_open = m.open;
    int restore = m.prev_paused;
    int multiplayer = (igm_mode() == IGM_MULTI);
    if (m.options_open) Options_Shutdown();
    if (m.info_open) GameInfo_Close();
    if (m.browser_open) SaveBrowser_Close();
    free_dialog();
    if (m.font_help) Font_Free(m.font_help);
    memset(&m, 0, sizeof(m));
    /* The clock picks up where the dialog found it (legacy:145921-145924). */
    if (was_open && !multiplayer) InGame_SetPaused(restore);
}

int InGameMenu_IsOpen(void) { return m.open; }

static void play_widget_sound(const GUIWidget *w) {
    if (!w || !w->sound[0]) return;
    set_text(m.last_sound, sizeof(m.last_sound), w->sound);
    GameSound_PlayUI(w->sound);
}

static int press_named(const char *name) {
    const GUIWidget *w = name ? GUIDialog_FindByName(&m.dialog, name) : NULL;
    if (!w) return GAMESTATE_IN_GAME;
    play_widget_sound(w);
    if (tak_stricmp(name, "Resume") == 0) {
        InGameMenu_Close();
        return GAMESTATE_IN_GAME;
    }
    if (tak_stricmp(name, "Exit") == 0) {            /* legacy:154732 */
        (void)load_dialog(exit_file());
        return GAMESTATE_IN_GAME;
    }
    if (tak_stricmp(name, "Options") == 0) {         /* legacy:154740 */
        Options_SetReturnState(GAMESTATE_IN_GAME);
        if (Options_Init(NULL) == 0) m.options_open = 1;
        return GAMESTATE_IN_GAME;
    }
    if (tak_stricmp(name, "Cancel") == 0) {          /* legacy:156337-156341 */
        (void)load_dialog(menu_file());
        return GAMESTATE_IN_GAME;
    }
    if (tak_stricmp(name, "ExitToWindows") == 0) {   /* legacy:156312-156326 */
        InGameMenu_Close();
        return GAMESTATE_QUIT;
    }
    if (tak_stricmp(name, "ExitToMainMenu") == 0) {  /* legacy:156327-156400 */
        InGameMenu_Close();
        return GAMESTATE_MENU;
    }
    if (tak_stricmp(name, "Restart") == 0) {         /* legacy:156329-156336 */
        const GameWorld *w2 = World_Get();
        if (!w2) return GAMESTATE_IN_GAME;
        igm_restart.pending = 1;
        igm_restart.cfg = w2->cfg;
        set_text(igm_restart.map, sizeof(igm_restart.map), w2->map_name);
        set_text(igm_restart.kingdom, sizeof(igm_restart.kingdom), w2->map_kingdom);
        InGameMenu_Close();
        return GAMESTATE_GAME_LOADING;
    }
    /* Both dialogs are the original's own files, opened over the menu
     * (legacy:154703-154712 into legacy:158806-158813). */
    if (tak_stricmp(name, "SaveGame") == 0) {
        if (SaveBrowser_Open(SAVEBROWSER_SAVE) == 0) m.browser_open = 1;
        return GAMESTATE_IN_GAME;
    }
    if (tak_stricmp(name, "LoadGame") == 0) {
        if (SaveBrowser_Open(SAVEBROWSER_LOAD) == 0) m.browser_open = 1;
        return GAMESTATE_IN_GAME;
    }
    if (tak_stricmp(name, "GameInfo") == 0) {
        if (GameInfo_Open(World_Get()) == 0) m.info_open = 1;
        return GAMESTATE_IN_GAME;
    }
    return GAMESTATE_IN_GAME;
}

/* A save the player picked becomes the same handoff a Restart makes:
 * the battle it names is brought up from scratch and the save is
 * applied on top of it once the definitions are in memory. */
int InGameMenu_TakeBrowserResult(SaveBrowserResult r) {
    if (r == SAVEBROWSER_OPEN) return GAMESTATE_IN_GAME;
    if (r == SAVEBROWSER_LOAD_READY) {
        TAK_SaveGame *sg = SaveBrowser_TakeLoad();
        const TAK_SaveInfo *info = sg ? Save_Info(sg) : NULL;
        if (info) {
            igm_restart.pending = 1;
            igm_restart.cfg = info->cfg;
            set_text(igm_restart.map, sizeof(igm_restart.map), info->map_name);
            set_text(igm_restart.kingdom, sizeof(igm_restart.kingdom),
                     info->map_kingdom);
            Loading_SetPendingSave(sg);
            SaveBrowser_Close();
            m.browser_open = 0;
            InGameMenu_Close();
            return GAMESTATE_GAME_LOADING;
        }
        if (sg) Save_ReadClose(sg);
    }
    /* Cancelled, or saved: back to the menu behind it
     * (legacy:156337-156341 is the same return). */
    SaveBrowser_Close();
    m.browser_open = 0;
    return GAMESTATE_IN_GAME;
}

int InGameMenu_TakeRestart(BattleConfig *out_cfg,
                           char *out_map, size_t map_cap,
                           char *out_kingdom, size_t kingdom_cap) {
    if (!igm_restart.pending) return 0;
    if (out_cfg) *out_cfg = igm_restart.cfg;
    set_text(out_map, map_cap, igm_restart.map);
    set_text(out_kingdom, kingdom_cap, igm_restart.kingdom);
    memset(&igm_restart, 0, sizeof(igm_restart));
    return 1;
}

/* The hovered button's help string, in the dialog's HelpText label
 * (legacy:46918). */
static void draw_help_strip(void) {
    if (!m.font_help || !m.rt) return;
    const GUIWidget *hover = GUIRuntime_HoveredWidget(m.rt);
    const GUIWidget *help = GUIDialog_FindByName(&m.dialog, "HelpText");
    if (!hover || !help || !hover->tooltip[0]) return;
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    int tw = Font_MeasureString(m.font_help, hover->tooltip);
    SDL_Rect r = help->rect;
    Font_DrawString(m.font_help, off, r.x + m.off_x + (r.w - tw) / 2,
                    Font_CenterY(m.font_help, r.y + m.off_y, r.h),
                    hover->tooltip);
}

int InGameMenu_Tick(TAK_Platform *platform) {
    if (!m.open || !m.rt) return GAMESTATE_IN_GAME;

    int focus = platform && platform->has_focus;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int enter = focus && (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]);
    int esc   = focus && keys[SDL_SCANCODE_ESCAPE];
    const char *key_widget = NULL;
    /* An open options dialog takes the keys first (legacy:243003-243004),
     * so a key held through its close presses nothing here. */
    if (!m.options_open && !m.browser_open && !m.info_open) {
        if (enter && !m.prev_enter && m.enter_widget[0]) key_widget = m.enter_widget;
        if (esc && !m.prev_esc && m.esc_widget[0])       key_widget = m.esc_widget;
    }
    m.prev_enter = enter;
    m.prev_esc = esc;

    if (m.browser_open) {
        /* The dialog sits over the menu, which stays drawn behind it,
         * and the battle is never left (legacy:158806-158813). */
        GUIRuntime_Render(m.rt);
        SaveBrowserResult r = SaveBrowser_Tick(platform);
        return InGameMenu_TakeBrowserResult(r);
    }

    if (m.info_open) {
        /* Game Information sits over the menu the same way
         * (legacy:154864). */
        GUIRuntime_Render(m.rt);
        if (GameInfo_Tick(platform) || !GameInfo_IsOpen()) m.info_open = 0;
        return GAMESTATE_IN_GAME;
    }

    if (m.options_open) {
        /* Options sits inside the menu, which stays drawn behind it,
         * and the battle is never left (legacy:157830-157836). */
        GUIRuntime_Render(m.rt);
        if (Options_Tick(platform, 0.0f) != GAMESTATE_OPTIONS) {
            Options_Shutdown();
            m.options_open = 0;
        }
        return GAMESTATE_IN_GAME;
    }

    int wx = 0, wy = 0, mx = -1, my = -1;
    int mouse_down = 0;
    if (focus) {
        uint32_t buttons = SDL_GetMouseState(&wx, &wy);
        mouse_down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
            mx = -1; my = -1;
        }
    }
    char clicked[64];
    int got = GUIRuntime_Update(m.rt, mx, my, mouse_down, clicked, sizeof(clicked));

    GUIRuntime_Render(m.rt);
    draw_help_strip();

    if (got) return press_named(clicked);
    if (key_widget) return press_named(key_widget);
    return GAMESTATE_IN_GAME;
}

/* ── Introspection ─────────────────────────────────────────────────── */

const char *InGameMenu_DialogPath(void) { return m.open ? m.path : ""; }

int InGameMenu_HasButton(const char *name) {
    if (!m.open || !name) return 0;
    return GUIDialog_FindByName(&m.dialog, name) != NULL;
}

const char *InGameMenu_ButtonHelp(const char *name) {
    if (!m.open || !name) return "";
    const GUIWidget *w = GUIDialog_FindByName(&m.dialog, name);
    return w ? w->tooltip : "";
}

const char *InGameMenu_Accelerators(void) {
    return m.open ? m.dialog.root.tooltip : "";
}

int InGameMenu_Press(const char *name) {
    if (!m.open) return GAMESTATE_IN_GAME;
    return press_named(name);
}

int InGameMenu_PressKey(const char *key) {
    if (!m.open || !key) return GAMESTATE_IN_GAME;
    const char *widget = NULL;
    if (tak_stricmp(key, "Enter") == 0) widget = m.enter_widget;
    if (tak_stricmp(key, "Esc") == 0)   widget = m.esc_widget;
    if (!widget || !widget[0]) return GAMESTATE_IN_GAME;
    return press_named(widget);
}

const char *InGameMenu_LastSound(void) { return m.last_sound; }

int InGameMenu_BrowserOpen(void) { return m.open && m.browser_open; }
