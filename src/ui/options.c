/*
 * options.c -- Options screen (GAMESTATE_OPTIONS).
 *
 * Tabbed dialog (options.gui) whose four stage-buttons — Interface,
 * Music, Sound, Visual — each load a sub-dialog into the groupbox:
 *   Interface → data/guis/interfaceoptions.gui
 *   Music     → data/guis/musicoptions.gui
 *   Sound     → data/guis/soundoptions.gui
 *   Visual    → data/guis/visualoptions.gui
 *
 * Clicking Cancel / OK / ESC returns to whichever screen pushed Options
 * (main menu or battle setup, tracked via Options_SetReturnState).
 */

#include "tak_options.h"
#include "tak_gameloop.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_font.h"
#include "tak_blit.h"
#include "tak_ui.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    TAB_INTERFACE = 0,
    TAB_MUSIC,
    TAB_SOUND,
    TAB_VISUAL,
    TAB_COUNT
} OptionsTab;

static const char *tab_gui[TAB_COUNT] = {
    "data/guis/interfaceoptions.gui",
    "data/guis/musicoptions.gui",
    "data/guis/soundoptions.gui",
    "data/guis/visualoptions.gui",
};
static const char *tab_widget_name[TAB_COUNT] = {
    "Interface", "Music", "Sound", "Visual"
};
static const char *tab_label[TAB_COUNT] = {
    "Interface", "Music", "Sound", "Visual"
};

static struct {
    int          initialized;
    GUIDialog    shell;           /* options.gui — the outer panel + tabs */
    GUIRuntime  *shell_rt;
    GUIDialog    sub;
    GUIRuntime  *sub_rt;
    int          sub_loaded;      /* 1 when `sub` holds a valid dialog */
    int          active_tab;
    Font        *tooltip_font;
    Font        *header_font;
    int          return_state;
    int          pending_nextstate;
    /* The main screen the dialog sits on when opened from the menu: the
     * menu's own button handler creates the dialog in place
     * (legacy:137341-137349), so the screen stays beneath. The panel art
     * is smaller than the screen and its button notches are cut to the
     * art, so whatever lies beneath shows through them. */
    uint32_t    *backdrop;
} opts;

/* Paint one menu sprite (rest frame) into the backdrop buffer. */
static void composite_menu_sprite(uint32_t *bg, GAFFile *gaf,
                                  const uint32_t *table, const char *name,
                                  int at_x, int at_y) {
    int entry = GAF_FindSequence(gaf, name);
    if (entry < 0) return;
    int w = 0, h = 0;
    uint32_t *sp = UI_DecodeFrame(gaf, entry, 0, table, &w, &h);
    if (!sp) return;
    for (int y = 0; y < h; y++) {
        int dy = at_y + y;
        if (dy < 0 || dy >= 480) continue;
        for (int x = 0; x < w; x++) {
            int dx = at_x + x;
            if (dx < 0 || dx >= 640) continue;
            uint8_t r, g, b, a;
            SDL_GetRGBA(sp[y * w + x], UI_RGBAFormat(), &r, &g, &b, &a);
            if (a) bg[dy * 640 + dx] = sp[y * w + x];
        }
    }
    tak_free(sp);
}

static uint32_t *load_menu_backdrop(void) {
    GAFFile *gaf = NULL;
    uint32_t table[256];
    if (UI_LoadGAFWithPalette("data/anims/mainscreen.gaf",
                              "data/anims/mainscreen.pcx", &gaf, table) != 0)
        return NULL;
    uint32_t *px = NULL;
    int entry = GAF_FindSequence(gaf, "MainBG");
    if (entry >= 0) {
        int w = 0, h = 0;
        px = UI_DecodeFrame(gaf, entry, 0, table, &w, &h);
        if (px && (w != 640 || h != 480)) { tak_free(px); px = NULL; }
    }
    /* The screen art has holes where the menu draws its own two
     * buttons; fill them the way the live menu does. */
    if (px) {
        composite_menu_sprite(px, gaf, table, "OptionsButton", 524, 406);
        composite_menu_sprite(px, gaf, table, "ExitButton",    68, 407);
    }
    GAF_Close(gaf);
    return px;
}

void Options_SetReturnState(int state) { opts.return_state = state; }

/* Load the sub-dialog for the given tab. Replaces any previous sub.
 * The sub uses coordinates relative to its own root (e.g. its widgets
 * live at x=52..363 with root at (52,49)). Shift them so they draw
 * inside the outer shell's content area by applying a runtime offset
 * equal to the shell's root origin. */
static int load_tab(int tab) {
    if (tab < 0 || tab >= TAB_COUNT) return -1;
    if (opts.sub_rt) { GUIRuntime_Destroy(opts.sub_rt); opts.sub_rt = NULL; }
    if (opts.sub_loaded) { GUIDialog_Free(&opts.sub); opts.sub_loaded = 0; }

    if (GUIDialog_Load(&opts.sub, tab_gui[tab]) != 0) {
        fprintf(stderr, "Options: sub-dialog %s missing (tab renders as blank panel)\n",
                tab_gui[tab]);
        return -1;
    }
    opts.sub_loaded = 1;
    opts.sub_rt = GUIRuntime_Create(&opts.sub);

    /* Sub-dialog widgets are declared in absolute screen coordinates
     * (they inherit positioning from the main dialog they're parented
     * to in the original game), so draw them at their natural rects —
     * no offset. */
    if (opts.sub_rt) {
        int dx = (opts.sub.root.rect.x < opts.shell.root.rect.x)
               ? opts.shell.root.rect.x : 0;
        int dy = (opts.sub.root.rect.y < opts.shell.root.rect.y)
               ? opts.shell.root.rect.y : 0;
        if (tab == TAB_INTERFACE) {
            dx = 105;
            dy = 130;
        }
        GUIRuntime_SetOffset(opts.sub_rt, dx, dy);
    }

    opts.active_tab = tab;
    return 0;
}

int Options_Init(TAK_Platform *platform) {
    (void)platform;
    int prev_return = opts.return_state;
    memset(&opts, 0, sizeof(opts));
    opts.return_state = prev_return > 0 ? prev_return : GAMESTATE_MENU;
    opts.pending_nextstate = -1;
    opts.active_tab = -1;

    if (GUIDialog_Load(&opts.shell, "data/guis/options.gui") != 0) {
        fprintf(stderr, "Options: failed to parse options.gui\n");
        return -1;
    }
    opts.shell_rt = GUIRuntime_Create(&opts.shell);
    if (!opts.shell_rt) { GUIDialog_Free(&opts.shell); return -1; }

    opts.tooltip_font = Font_Load("data/fonts/b_times new roman (100b)",
                                   UI_RGBAFormat());
    opts.header_font  = opts.tooltip_font;   /* reuse */

    if (opts.return_state == GAMESTATE_MENU)
        opts.backdrop = load_menu_backdrop();

    /* Default tab: Interface. */
    load_tab(TAB_INTERFACE);

    opts.initialized = 1;
    return 0;
}

void Options_Shutdown(void) {
    if (!opts.initialized) return;
    if (opts.sub_rt)       GUIRuntime_Destroy(opts.sub_rt);
    if (opts.shell_rt)     GUIRuntime_Destroy(opts.shell_rt);
    if (opts.tooltip_font) Font_Free(opts.tooltip_font);
    if (opts.sub_loaded)   GUIDialog_Free(&opts.sub);
    if (opts.backdrop)     tak_free(opts.backdrop);
    GUIDialog_Free(&opts.shell);
    int saved = opts.return_state;
    memset(&opts, 0, sizeof(opts));
    opts.return_state = saved;
}

int Options_Tick(TAK_Platform *platform, float frame_dt) {
    (void)frame_dt;
    if (!opts.initialized) return GAMESTATE_OPTIONS;

    int wx, wy, mx, my;
    SDL_GetMouseState(&wx, &wy);
    if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
        mx = -1; my = -1;
    }
    int mouse_down = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT);

    /* ── Shell clicks first (tab buttons + Cancel/OK) ────────────── */
    char shell_click[64];
    int s_got = GUIRuntime_Update(opts.shell_rt, mx, my, mouse_down,
                                    shell_click, sizeof(shell_click));
    if (s_got) {
        if (tak_stricmp(shell_click, "Cancel")   == 0 ||
            tak_stricmp(shell_click, "OK")       == 0 ||
            tak_stricmp(shell_click, "Previous") == 0) {
            opts.pending_nextstate = opts.return_state;
        } else {
            for (int t = 0; t < TAB_COUNT; t++) {
                if (tak_stricmp(shell_click, tab_widget_name[t]) == 0) {
                    load_tab(t);
                    break;
                }
            }
        }
    }

    /* ── Sub-dialog clicks (volume sliders, checkboxes, etc.) ───── */
    if (opts.sub_rt) {
        char sub_click[64];
        int sub_got = GUIRuntime_Update(opts.sub_rt, mx, my, mouse_down,
                                         sub_click, sizeof(sub_click));
        if (sub_got) {
            /* Sub-dialog buttons are local to this tab; we don't have
             * per-option persistence yet, so just log. */
            fprintf(stderr, "Options[%s]: '%s' clicked (TODO wire up)\n",
                    tab_label[opts.active_tab], sub_click);
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_ESCAPE]) opts.pending_nextstate = opts.return_state;

    /* ── Render ──────────────────────────────────────────────────── */
    SDL_Surface *off = UI_Offscreen();
    if (opts.backdrop) {
        Blit_RGBA(off, 0, 0, opts.backdrop, 640, 480);
    } else {
        SDL_Rect full = { 0, 0, 640, 480 };
        SDL_FillRect(off, &full, SDL_MapRGBA(off->format, 24, 24, 32, 255));
    }

    GUIRuntime_Render(opts.shell_rt);
    if (opts.sub_rt) GUIRuntime_Render(opts.sub_rt);

    /* Tab label banner at the top so the user can see which tab is active. */
    if (opts.header_font && opts.active_tab >= 0 &&
        opts.active_tab < TAB_COUNT) {
        const char *label = tab_label[opts.active_tab];
        int tw = Font_MeasureString(opts.header_font, label);
        Font_DrawString(opts.header_font, off, 320 - tw / 2, 95, label);
    }

    /* Tooltip for hovered widget on whichever layer. */
    if (opts.tooltip_font) {
        const GUIWidget *hw = GUIRuntime_HoveredWidget(opts.sub_rt);
        if (!hw || !hw->tooltip[0]) hw = GUIRuntime_HoveredWidget(opts.shell_rt);
        if (hw && hw->tooltip[0]) {
            int tw = Font_MeasureString(opts.tooltip_font, hw->tooltip);
            SDL_Rect strip = { 120, 400, 400, 22 };
            SDL_FillRect(off, &strip, SDL_MapRGBA(off->format, 18, 14, 8, 255));
            Font_DrawString(opts.tooltip_font, off,
                             320 - tw / 2, 404, hw->tooltip);
        }
    }

    UI_Present(platform);

    int next = (opts.pending_nextstate >= 0) ? opts.pending_nextstate
                                             : GAMESTATE_OPTIONS;
    opts.pending_nextstate = -1;
    return next;
}
