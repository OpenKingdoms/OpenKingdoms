/*
 * ingame_menu.c -- The F1 menu a battle opens over itself.
 *
 * F1 is bound to F2Menu (keys.tdf:169, loader legacy:122746-122785).
 * The dialog is picked by mode (legacy:154643-154655) and sits over the
 * running battle, so the terrain and the sidebar stay drawn behind it.
 * Enter and Escape press whatever the root's accelerator string names,
 * "#Enter#Resume#Esc#Resume", read the way the end screen reads its
 * own. Resume closes it (legacy:154721-154726). Game Information, Load
 * Game and Save Game are drawn by the shipped art and do nothing: we
 * have no save system and no game information screen yet.
 */

#include "tak_ingame_menu.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_font.h"
#include "tak_ui.h"
#include "tak_world.h"
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
    char        path[128];
    char        enter_widget[32];
    char        esc_widget[32];
    char        last_sound[64];
    int         prev_enter;
    int         prev_esc;
} m;

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
    m.prev_enter = m.prev_esc = 0;
    return 0;
}

int InGameMenu_Open(void) {
    if (m.open) return 0;
    memset(&m, 0, sizeof(m));
    if (load_dialog(menu_file()) != 0) return -1;
    m.font_help = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    m.open = 1;
    return 0;
}

void InGameMenu_Close(void) {
    free_dialog();
    if (m.font_help) Font_Free(m.font_help);
    memset(&m, 0, sizeof(m));
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
    /* GameInfo, LoadGame and SaveGame stay drawn and do nothing. */
    return GAMESTATE_IN_GAME;
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
    int top = 0, bottom = 0;
    if (Font_InkExtent(m.font_help, hover->tooltip, &top, &bottom) != 0) return;
    SDL_Rect r = help->rect;
    Font_DrawString(m.font_help, off, r.x + (r.w - tw) / 2,
                    r.y + (r.h - (bottom - top)) / 2 - top, hover->tooltip);
}

int InGameMenu_Tick(TAK_Platform *platform) {
    if (!m.open || !m.rt) return GAMESTATE_IN_GAME;

    int wx = 0, wy = 0, mx = -1, my = -1;
    int mouse_down = 0;
    int focus = platform && platform->has_focus;
    if (focus) {
        uint32_t buttons = SDL_GetMouseState(&wx, &wy);
        mouse_down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
            mx = -1; my = -1;
        }
    }
    char clicked[64];
    int got = GUIRuntime_Update(m.rt, mx, my, mouse_down, clicked, sizeof(clicked));

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int enter = focus && (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]);
    int esc   = focus && keys[SDL_SCANCODE_ESCAPE];
    const char *key_widget = NULL;
    if (enter && !m.prev_enter && m.enter_widget[0]) key_widget = m.enter_widget;
    if (esc && !m.prev_esc && m.esc_widget[0])       key_widget = m.esc_widget;
    m.prev_enter = enter;
    m.prev_esc = esc;

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
