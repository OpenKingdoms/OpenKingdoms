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
#include "tak_settings.h"
#include "tak_unit.h"
#include "tak_music.h"
#include "tak_sound.h"
#include "tak_game_sound.h"
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

    /* The volume slider of whichever page is up. Indices into opts.sub,
     * -1 on a page that has none. */
    int          idx_vol_track, idx_vol_thumb, idx_vol_label;
    int          dragging_volume;

    /* Everything the pages edit, as the dialog opened. Cancel puts it
     * back, the way the help strip promises (`#enter#Ok#esc#Cancel` in
     * options.gui), so nothing is written until Ok. The two volumes are
     * in percent, as the dialog shows them. */
    int          snap_music_vol, snap_sound_vol, snap_music_on;
    int          snap_sound_on, snap_unit_voices;
    int          snap_damage_bars, snap_shadows;
    Font        *tooltip_font;
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

/* The Visual page reflects the settings it edits: checkbox frame 3 is
 * off and 4 is on (the sheet the setup screen uses too). */
static void sync_visual_checkboxes(void) {
    if (!opts.sub_rt || opts.active_tab != TAB_VISUAL) return;
    GUIRuntime_SetFrameOverride(opts.sub_rt, "ShowDamage",
                                Settings_GetInt("DisplayDamageBars", 0) ? 4 : 3);
    GUIRuntime_SetFrameOverride(opts.sub_rt, "Shadows",
                                Settings_GetInt("DrawShadows", 1) ? 4 : 3);
}

/* Music On reflects the mode the mixer is in; there is no getter for it,
 * so the setting is the record. Off takes the level with it, the way the
 * original greys the slider out: the scrollbar sheets carry that art on
 * frame 0. */
static const char *const MUSIC_LEVEL_WIDGETS[] = {
    "MusicVolume", "sbutton", "incbutton", "decbutton", "MusicVolumeVal",
};

/* Sound On is the top switch of its page: off greys the level and the
 * Unit Sounds box under it. Unit Sounds itself only silences the voices
 * and leaves the level alone. Neither touches the music. */
static const char *const SOUND_LEVEL_WIDGETS[] = {
    "SoundVolume", "sbutton", "incbutton", "decbutton", "SoundVolumeVal",
};

/* 3 is unticked and 4 is ticked. The .gui gives a checkbox five frames
 * of the six on the sheet, and of those only frame 0 is the greyed art,
 * so a locked box shows that whichever way it is set. */
static int checkbox_frame(int ticked, int live) {
    return live ? (ticked ? 4 : 3) : 0;
}

static int music_is_on(void) { return Settings_GetInt("MusicOn", 1) != 0; }
static int sound_is_on(void) { return Settings_GetInt("SoundOn", 1) != 0; }

static void grey_level(const char *const *names, size_t n, int on) {
    for (size_t i = 0; i < n; i++)
        GUIRuntime_SetFrameOverride(opts.sub_rt, names[i], on ? -1 : 0);
}

static void sync_music_checkbox(void) {
    if (!opts.sub_rt || opts.active_tab != TAB_MUSIC) return;
    int on = music_is_on();
    GUIRuntime_SetFrameOverride(opts.sub_rt, "MusicOn", on ? 4 : 3);
    grey_level(MUSIC_LEVEL_WIDGETS,
               sizeof(MUSIC_LEVEL_WIDGETS) / sizeof(*MUSIC_LEVEL_WIDGETS), on);
}

static void sync_sound_checkboxes(void) {
    if (!opts.sub_rt || opts.active_tab != TAB_SOUND) return;
    int on = sound_is_on();
    GUIRuntime_SetFrameOverride(opts.sub_rt, "SoundOn", on ? 4 : 3);
    GUIRuntime_SetFrameOverride(opts.sub_rt, "UnitSounds",
                                checkbox_frame(GameSound_UnitVoicesOn(), on));
    grey_level(SOUND_LEVEL_WIDGETS,
               sizeof(SOUND_LEVEL_WIDGETS) / sizeof(*SOUND_LEVEL_WIDGETS), on);
}

/* The level control lives further down; the arrows are clicked here. */
static int  volume_slider_live(void);
static int  volume_get(void);
static void volume_set(int percent);
static void sync_volume_widgets(void);

/* A click on the current page. Show Damage flips DisplayDamageBars on
 * the spot (legacy:157728); every page applies its change at once but
 * leaves the keeping to Ok. */
static int handle_sub_click(const char *name) {
    if (tak_stricmp(name, "MusicOn") == 0) {
        int on = !Settings_GetInt("MusicOn", 1);
        Settings_SetInt("MusicOn", on);   /* kept by Ok, undone by Cancel */
        TAK_Music_SetMode(on ? TAK_MUSIC_SEQUENTIAL : TAK_MUSIC_OFF);
        sync_music_checkbox();
        return 1;
    }
    if (tak_stricmp(name, "SoundOn") == 0) {
        int on = !sound_is_on();
        Settings_SetInt("SoundOn", on);   /* kept by Ok, undone by Cancel */
        TAK_Sound_SetEnabled(on);
        sync_sound_checkboxes();
        return 1;
    }
    if (tak_stricmp(name, "UnitSounds") == 0) {
        if (!sound_is_on()) return 1;   /* greyed out under Sound On */
        int on = !GameSound_UnitVoicesOn();
        Settings_SetInt("UnitSounds", on);
        GameSound_SetUnitVoicesOn(on);
        sync_sound_checkboxes();
        return 1;
    }
    /* The arrows either side of the track step the level by one, as the
     * original steps it, and take the same gate as a drag. Every page
     * names its arrows the same way and the Interface page has four
     * sliders of its own, so a page without a level leaves them alone
     * and lets the unhandled report stand. */
    if (tak_stricmp(name, "incbutton") == 0 ||
        tak_stricmp(name, "decbutton") == 0) {
        if (opts.idx_vol_track < 0) return 0;
        if (!volume_slider_live()) return 1;
        int step = (tak_stricmp(name, "incbutton") == 0) ? 1 : -1;
        volume_set(volume_get() + step);
        sync_volume_widgets();
        return 1;
    }
    if (tak_stricmp(name, "ShowDamage") == 0) {
        int on = !Settings_GetInt("DisplayDamageBars", 0);
        Settings_SetInt("DisplayDamageBars", on);  /* kept by Ok */
        Units_SetHealthBarsOn(on);
        sync_visual_checkboxes();
        return 1;
    }
    if (tak_stricmp(name, "Shadows") == 0) {
        int on = !Settings_GetInt("DrawShadows", 1);
        Settings_SetInt("DrawShadows", on);        /* kept by Ok */
        Units_SetShadowsOn(on);
        sync_visual_checkboxes();
        return 1;
    }
    return 0;
}

/* ── The volume sliders on the Sound and Music pages ─────────────────
 * Each page carries a track, an `sbutton` thumb and a "##" label, one
 * slider per page, so the names are unambiguous within a page. */

static int volume_is_music(void) { return opts.active_tab == TAB_MUSIC; }

/* The mixer works in 0-127, the Miles range the original used, and the
 * dialog shows 0-100 the way the original shows it. The rounding makes
 * the ends land exactly: 100 gives 127 and 0 gives 0. */
#define VOL_MAX 127

static int volume_to_percent(int v) { return (v * 100 + VOL_MAX / 2) / VOL_MAX; }
static int percent_to_volume(int p) { return (p * VOL_MAX + 50) / 100; }

static int volume_get(void) {
    return volume_to_percent(volume_is_music() ? TAK_Music_GetVolume()
                                               : TAK_Sound_GetMasterVolume());
}

static void volume_set(int percent) {
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    int music = volume_is_music();
    int was   = volume_get();        /* before the mixer moves */
    if (music) TAK_Music_SetVolume(percent_to_volume(percent));
    else       TAK_Sound_SetMasterVolume(percent_to_volume(percent));
    Settings_SetInt(music ? "MusicVolume" : "SoundVolume", percent);

    /* Silence is off, so the slider drives the Music On box, but only
     * where it crosses zero: a level that is already audible says nothing
     * about a box the player unticked. Unticking leaves the level alone,
     * so ticking again restores what was set. */
    if (music && (percent == 0 || was == 0)) {
        int on = percent > 0;
        if (on != (Settings_GetInt("MusicOn", 1) != 0)) {
            Settings_SetInt("MusicOn", on);
            TAK_Music_SetMode(on ? TAK_MUSIC_SEQUENTIAL : TAK_MUSIC_OFF);
            sync_music_checkbox();
        }
    }
    /* No save here: a drag calls this every frame, and a save rewrites
     * the whole settings file (and, in the browser, copies it out of a
     * filesystem that dies with the tab). Ok is what writes. */
}

/* What the dialog opened on, and putting it back. Every page applies a
 * change at once so it can be heard or seen, but only Ok keeps it, so
 * nothing reaches the file in between. */
static void settings_snapshot(void) {
    opts.snap_music_vol   = volume_to_percent(TAK_Music_GetVolume());
    opts.snap_sound_vol   = volume_to_percent(TAK_Sound_GetMasterVolume());
    opts.snap_music_on    = Settings_GetInt("MusicOn", 1) ? 1 : 0;
    opts.snap_sound_on    = sound_is_on();
    opts.snap_unit_voices = GameSound_UnitVoicesOn() ? 1 : 0;
    opts.snap_damage_bars = Settings_GetInt("DisplayDamageBars", 0) ? 1 : 0;
    opts.snap_shadows     = Settings_GetInt("DrawShadows", 1) ? 1 : 0;
}

static void settings_revert(void) {
    TAK_Music_SetVolume(percent_to_volume(opts.snap_music_vol));
    TAK_Sound_SetMasterVolume(percent_to_volume(opts.snap_sound_vol));
    TAK_Music_SetMode(opts.snap_music_on ? TAK_MUSIC_SEQUENTIAL
                                         : TAK_MUSIC_OFF);
    TAK_Sound_SetEnabled(opts.snap_sound_on);
    GameSound_SetUnitVoicesOn(opts.snap_unit_voices);
    Units_SetHealthBarsOn(opts.snap_damage_bars);
    Units_SetShadowsOn(opts.snap_shadows);

    Settings_SetInt("MusicVolume",       opts.snap_music_vol);
    Settings_SetInt("SoundVolume",       opts.snap_sound_vol);
    Settings_SetInt("MusicOn",           opts.snap_music_on);
    Settings_SetInt("SoundOn",           opts.snap_sound_on);
    Settings_SetInt("UnitSounds",        opts.snap_unit_voices);
    Settings_SetInt("DisplayDamageBars", opts.snap_damage_bars);
    Settings_SetInt("DrawShadows",       opts.snap_shadows);
    /* Nothing was written while the dialog was up, so there is nothing to
     * undo on disk: the file still holds what these five now hold. */
}

/* Cache the slider's widgets for the page just loaded. */
static void find_volume_widgets(int tab) {
    opts.idx_vol_track = opts.idx_vol_thumb = opts.idx_vol_label = -1;
    const char *track = (tab == TAB_MUSIC) ? "MusicVolume"
                      : (tab == TAB_SOUND) ? "SoundVolume" : NULL;
    const char *label = (tab == TAB_MUSIC) ? "MusicVolumeVal"
                      : (tab == TAB_SOUND) ? "SoundVolumeVal" : NULL;
    if (!track || !opts.sub_loaded) return;
    for (int i = 0; i < opts.sub.num_children; i++) {
        const GUIWidget *w = &opts.sub.children[i];
        if (!w->name[0]) continue;
        if (tak_stricmp(w->name, track) == 0 && w->type == GUI_WT_SLIDER)
            opts.idx_vol_track = i;
        else if (tak_stricmp(w->name, "sbutton") == 0)
            opts.idx_vol_thumb = i;
        else if (tak_stricmp(w->name, label) == 0)
            opts.idx_vol_label = i;
    }
}

/* Mouse X along the track as a percentage, measured from the thumb's
 * centre so the grab point does not jump. */
static int volume_from_mouse_x(int mx) {
    SDL_Rect bar, thumb;
    if (opts.idx_vol_track < 0 ||
        GUIRuntime_WidgetDrawRect(opts.sub_rt, opts.idx_vol_track, &bar) != 0)
        return volume_get();
    int tw = 0;
    if (opts.idx_vol_thumb >= 0 &&
        GUIRuntime_WidgetDrawRect(opts.sub_rt, opts.idx_vol_thumb, &thumb) == 0)
        tw = thumb.w;
    int travel = bar.w - tw;
    if (travel < 1) travel = 1;
    int rel = mx - bar.x - tw / 2;
    if (rel < 0) rel = 0;
    if (rel > travel) rel = travel;
    return 100 * rel / travel;
}

/* Put the thumb where the value says and write the value into the label. */
static void sync_volume_widgets(void) {
    if (!opts.sub_loaded || opts.idx_vol_track < 0) return;
    int v = volume_get();

    if (opts.idx_vol_thumb >= 0) {
        SDL_Rect bar, thumb;
        if (GUIRuntime_WidgetDrawRect(opts.sub_rt, opts.idx_vol_track, &bar) == 0 &&
            GUIRuntime_WidgetDrawRect(opts.sub_rt, opts.idx_vol_thumb, &thumb) == 0) {
            int travel = bar.w - thumb.w;
            if (travel < 1) travel = 1;
            GUIWidget *w = &opts.sub.children[opts.idx_vol_thumb];
            /* Rect, not draw position: the renderer re-applies the hotspot. */
            w->rect.x = bar.x + travel * v / 100 + (w->rect.x - thumb.x);
        }
    }
    if (opts.idx_vol_label >= 0) {
        GUIWidget *w = &opts.sub.children[opts.idx_vol_label];
        snprintf(w->display_text, sizeof(w->display_text), "%d", v);
    }
}

/* A slider that will answer: the page has one and it is not the music
 * level with the music switched off. */
static int volume_slider_live(void) {
    if (!opts.sub_rt || opts.idx_vol_track < 0) return 0;
    return volume_is_music() ? music_is_on() : sound_is_on();
}

/* Leaving the dialog. Ok keeps what the pages did, Cancel puts back what
 * they found. Both the buttons and the keys come through here. */
static void options_confirm(void) {
    Settings_Save();
    opts.pending_nextstate = opts.return_state;
}

static void options_cancel(void) {
    settings_revert();
    opts.pending_nextstate = opts.return_state;
}

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

    /* Each sub-dialog is authored at its own place on the screen, but all
     * four are the size of the shell's PlaceHolderForOptionPages widget
     * (311x172) and belong inside it. Shift each one by the difference
     * between where the placeholder sits and where the sub-dialog's root
     * was authored, so every tab lands in the same frame.
     *
     * Observed in the shipped data: the placeholder is at 165,187 and the
     * roots are Interface 52,49 / Music 167,148 / Sound 88,119, which is
     * why a fixed offset cannot serve them all. */
    if (opts.sub_rt) {
        int dx = 0, dy = 0;
        GUIWidget *slot = GUIDialog_FindByName(&opts.shell,
                                               "PlaceHolderForOptionPages");
        if (slot) {
            dx = slot->rect.x - opts.sub.root.rect.x;
            dy = slot->rect.y - opts.sub.root.rect.y;
        }
        GUIRuntime_SetOffset(opts.sub_rt, dx, dy);
    }

    opts.active_tab = tab;

    /* The chosen tab stays pushed in: frame 1 is the selected art, and the
     * others go back to the plain frame 2 by clearing their override. */
    for (int t = 0; t < TAB_COUNT; t++) {
        GUIRuntime_SetFrameOverride(opts.shell_rt, tab_widget_name[t],
                                    t == tab ? 1 : -1);
    }

    find_volume_widgets(tab);
    sync_volume_widgets();
    opts.dragging_volume = 0;
    sync_music_checkbox();
    sync_sound_checkboxes();

    sync_visual_checkboxes();
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

    settings_snapshot();

    opts.tooltip_font = Font_Load("data/fonts/b_times new roman (100b)",
                                   UI_RGBAFormat());

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

    /* Volume slider: grabbing the track drags the value, and it follows
     * the pointer until the button comes up. The zone is a few pixels
     * proud of the track so the grab does not need pixel accuracy. */
    if (volume_slider_live()) {
        SDL_Rect bar;
        if (GUIRuntime_WidgetDrawRect(opts.sub_rt, opts.idx_vol_track, &bar) == 0) {
            SDL_Point pt = { mx, my };
            SDL_Rect zone = { bar.x - 4, bar.y - 4, bar.w + 8, bar.h + 8 };
            if (mouse_down && !opts.dragging_volume && SDL_PointInRect(&pt, &zone))
                opts.dragging_volume = 1;
            if (!mouse_down) opts.dragging_volume = 0;
            if (opts.dragging_volume) {
                volume_set(volume_from_mouse_x(mx));
                sync_volume_widgets();
            }
        }
    }

    /* ── Shell clicks first (tab buttons + Cancel/OK) ────────────── */
    char shell_click[64];
    int s_got = GUIRuntime_Update(opts.shell_rt, mx, my, mouse_down,
                                    shell_click, sizeof(shell_click));
    if (s_got) {
        if (tak_stricmp(shell_click, "Ok") == 0) {
            options_confirm();
        } else if (tak_stricmp(shell_click, "Cancel")   == 0 ||
                   tak_stricmp(shell_click, "Previous") == 0) {
            options_cancel();
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
        if (sub_got && !handle_sub_click(sub_click)) {
            fprintf(stderr, "Options[%s]: '%s' not wired\n",
                    tab_label[opts.active_tab], sub_click);
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    /* The help strip spells the keys out: `#enter#Ok#esc#Cancel`. */
    if (keys[SDL_SCANCODE_ESCAPE]) options_cancel();
    else if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER])
        options_confirm();

    /* ── Render ──────────────────────────────────────────────────── */
    SDL_Surface *off = UI_Offscreen();
    if (opts.backdrop) {
        Blit_RGBA(off, 0, 0, opts.backdrop, 640, 480);
    } else if (opts.return_state != GAMESTATE_IN_GAME) {
        SDL_Rect full = { 0, 0, 640, 480 };
        SDL_FillRect(off, &full, SDL_MapRGBA(off->format, 24, 24, 32, 255));
    }
    /* Opened from a battle the panel is created in place over the game
     * and paints no backdrop of its own (legacy:157830-157836). */

    GUIRuntime_Render(opts.shell_rt);
    if (opts.sub_rt) GUIRuntime_Render(opts.sub_rt);

    /* Tooltip for hovered widget on whichever layer. It goes in the dialog's
     * own HelpText widget, centred in it, the way the main menu does it — the
     * panel already has the dark strip painted, so nothing is drawn behind the
     * text. */
    if (opts.tooltip_font) {
        const GUIWidget *hw = GUIRuntime_HoveredWidget(opts.sub_rt);
        if (!hw || !hw->tooltip[0]) hw = GUIRuntime_HoveredWidget(opts.shell_rt);
        if (hw && hw->tooltip[0]) {
            int tw = Font_MeasureString(opts.tooltip_font, hw->tooltip);
            const GUIWidget *slot = GUIDialog_FindByName(&opts.shell, "HelpText");
            int tx = slot ? slot->rect.x + (slot->rect.w - tw) / 2 : 320 - tw / 2;
            int ty = slot ? slot->rect.y : 404;
            if (slot) {
                /* Centre the ink in the cell, not the line box: the help
                 * strip is 30 px tall and the glyphs cover far less, so
                 * drawing from the top edge leaves the text sitting high. */
                int top = 0, bottom = 0;
                if (Font_InkExtent(opts.tooltip_font, hw->tooltip,
                                   &top, &bottom) != 0) top = bottom = 0;
                ty += (slot->rect.h - (bottom - top)) / 2 - top;
            }
            Font_DrawString(opts.tooltip_font, off, tx, ty, hw->tooltip);
        }
    }

    UI_Present(platform);

    int next = (opts.pending_nextstate >= 0) ? opts.pending_nextstate
                                             : GAMESTATE_OPTIONS;
    opts.pending_nextstate = -1;
    return next;
}

int Options_ClickWidget(const char *name) {
    if (!opts.initialized || !name) return 0;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (tak_stricmp(name, tab_widget_name[i]) == 0) {
            load_tab(i);
            return 1;
        }
    }
    if (tak_stricmp(name, "Ok") == 0)     { options_confirm(); return 1; }
    if (tak_stricmp(name, "Cancel") == 0) { options_cancel();  return 1; }
    return handle_sub_click(name);
}

int Options_DebugVolume(void) {
    if (!opts.initialized || opts.idx_vol_track < 0) return -1;
    return volume_get();
}

int Options_DebugSetVolume(int percent) {
    if (!opts.initialized || !volume_slider_live()) return 0;
    volume_set(percent);
    sync_volume_widgets();
    return 1;
}
