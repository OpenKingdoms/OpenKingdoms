/*
 * main_menu.c -- Main menu screen implementation (GAMESTATE_MENU)
 *
 * Loads and renders the TAK Kingdoms main menu using assets defined
 * in mainmenu.gui. Each sprite loads its own palette from a matching
 * .pcx file alongside the GAF.
 *
 * Layout (from mainmenu.gui, 640x480 coordinates):
 *   Background:  mainscreen.gaf "MainBG" at (0,0)
 *   Skirmish:    singlemachine.gaf at (40,192) 149x188
 *   Story:       bodgirl.gaf at (242,202) 148x192
 *   Multiplayer: multiknight.gaf at (419,136) 161x243
 *   Options:     mainscreen.gaf "OptionsButton" at (524,406) 58x56
 *   Exit:        mainscreen.gaf "ExitButton" at (68,407) 39x51
 *
 * Character doors: the original's button keeps eight states and rests
 * in 2 (legacy:147778). Entering plays clip 5, which hands over to
 * clip 6 and holds on its last frame while the cursor stays; leaving
 * plays clip 7 back to rest (legacy:148022-148076). Clip n is
 * Movies/Gui/<name>n.bik. Without the clips the GAF entries cycle.
 */

#include "tak_main_menu.h"
#include "tak_gameloop.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_blit.h"
#include "tak_bink.h"
#include "tak_font.h"
#include "tak_ui.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BINK_CLIPS_PER_CHAR 4

/* ── Button definitions from mainmenu.gui ───────────────────────── */

typedef enum {
    MENUBTN_SKIRMISH = 0,
    MENUBTN_STORY,
    MENUBTN_MULTI,
    MENUBTN_CREDITS,   /* snort — top-left, plays Credits.bik on click */
    MENUBTN_OPTIONS,
    MENUBTN_EXIT,
    MENUBTN_COUNT
} MenuButtonID;

#define MENU_NUM_CHARACTERS 4   /* skirmish, story, multi, credits */

static const SDL_Rect button_rects[MENUBTN_COUNT] = {
    { 40, 192, 149, 188 },   /* PlayComputer (skirmish) */
    { 242, 202, 148, 192 },  /* PlayStory */
    { 419, 136, 161, 243 },  /* PlayPlayer (multiplayer) */
    { 67,  20, 143, 156 },   /* Credits (snort) — mainmenu.gui line 148 */
    { 524, 406, 58,  56 },   /* Options */
    { 68,  407, 39,  51 },   /* Exit */
};

/* Tighter per-character hit-rect (and draw-anchor) from the legacy menu
 * setup at the legacy reference line 140378-140443. These override the .gui
 * rect for hit-testing (see NetPlayerMap_Method28, line 147978) and serve as
 * the draw anchor: GAF_top_left = (hit_rect.x - hotspot.x, hit_rect.y - hotspot.y).
 * Indexed by the character buttons 0..3. */
static const SDL_Rect character_hit_rects[MENU_NUM_CHARACTERS] = {
    {  71, 219, 101, 158 },  /* PlayComputer (machine) — 0x47,  0xdb, 0x65, 0x9e */
    { 289, 217,  62, 168 },  /* PlayStory    (girl)    — 0x121, 0xd9, 0x3e, 0xa8 */
    { 487, 216,  63, 157 },  /* PlayPlayer   (knight)  — 0x1e7, 0xd8, 0x3f, 0x9d */
    { 124,  42,  71, 130 },  /* Credits      (snort)   — 0x7c,  0x2a, 0x47, 0x82 */
};

static const char *button_tooltips[MENUBTN_COUNT] = {
    "Play the Machine",
    "Play the Adventure",
    "Play an Opponent",
    "Credits",             /* snort — legacy setup line 140437 */
    "Options",
    "Exit to Windows",
};

/* HelpText widget rect from mainmenu.gui line 112: 172 441 296 31. */
static const SDL_Rect helptext_rect = { 172, 441, 296, 31 };

/* ── Character animation state ──────────────────────────────────── */

typedef struct {
    GAFFile *gaf;
    uint32_t rgba_table[256];
    int num_entries;

    /* Pre-decoded RGBA for entry 0 frame 0 (idle sprite) */
    uint32_t *idle_pixels;
    int idle_w, idle_h;
    int idle_ox, idle_oy;  /* hotspot of the idle/reference frame */

    /* Current animation state */
    int current_entry;
    float anim_timer;
    int animating;       /* currently playing hover animation? */
    int returning;       /* playing to completion before reset? */

    /* Most recently decoded frame for rendering */
    uint32_t *current_pixels;
    int current_w, current_h;
    int current_ox, current_oy; /* hotspot offsets for current frame */

    /* Bink video (opened on demand, one clip at a time) */
    BinkPlayer *active_player;
    int active_clip;       /* -1 = no video, 0-3 = clip index (maps to files 4-7) */
    double video_timer;
    int has_video;
    char bink_base[32];    /* e.g. "machine", "girl", "knight" */
    /* The original's button states: 2 rest, 4 still after a click,
     * 5 enter clip, 6 hover clip held on its last frame, 7 leave clip.
     * State n plays clip n. */
    int state;
    int inside;            /* cursor was over the door last tick */
    int hovered_flag;      /* legacy +0x139: set on enter, cleared on leave */
} CharacterAnim;

static int g_debug_force_hover = -2;   /* -2 = follow the cursor */

/* ── Menu state ─────────────────────────────────────────────────── */

static struct {
    int initialized;

    /* Background */
    uint32_t *bg_pixels;

    /* Character animations (skirmish, story, multi, credits/snort) */
    CharacterAnim characters[MENU_NUM_CHARACTERS];

    /* Static button sprites (options, exit) from mainscreen.gaf */
    uint32_t *options_pixels[3]; /* normal, hover, pressed */
    int options_w, options_h;
    uint32_t *exit_pixels[3];
    int exit_w, exit_h;

    /* Tooltip font (times new roman 100b) */
    Font *tooltip_font;

    /* Interaction */
    int hovered_button;  /* -1 = none */
    int pending_nextstate; /* set by click handlers; returned from Tick */

    /* Animation speed */
    float frame_duration;
} menu;

/* The GAF helpers and offscreen surface have been moved to tak_ui.h —
 * call UI_LoadGAFWithPalette, UI_DecodeFrame, UI_DecodeEntryIdle,
 * UI_Offscreen(), UI_RGBAFormat(), UI_Present(). */

/* ── Init character animation ───────────────────────────────────── */

/* init_character(ch, gaf_name, bink_name)
 *   gaf_name — filename base under data/anims (NULL for Bink-only buttons
 *              like Credits that have no static sprite)
 *   bink_name — base for Movies/Gui/<name>{4-7}.bik (NULL = no hover video) */
static void open_bink_clip(CharacterAnim *ch, int clip_index);

static int init_character(CharacterAnim *ch, const char *gaf_name, const char *bink_name) {
    memset(ch, 0, sizeof(*ch));
    ch->active_clip = -1;
    ch->state = 2;

    if (gaf_name) {
        char gaf_path[256], pcx_path[256];
        snprintf(gaf_path, sizeof(gaf_path), "data/anims/%s.gaf", gaf_name);
        snprintf(pcx_path, sizeof(pcx_path), "data/anims/%s.pcx", gaf_name);
        if (UI_LoadGAFWithPalette(gaf_path, pcx_path, &ch->gaf, ch->rgba_table) != 0)
            return -1;

        ch->num_entries = (int)ch->gaf->num_entries;
        ch->idle_pixels = UI_DecodeEntryIdle(ch->gaf, 0, ch->rgba_table,
                                             &ch->idle_w, &ch->idle_h,
                                             &ch->idle_ox, &ch->idle_oy);
        ch->current_pixels = ch->idle_pixels;
        ch->current_w = ch->idle_w;
        ch->current_h = ch->idle_h;
        ch->current_ox = ch->idle_ox;
        ch->current_oy = ch->idle_oy;

        fprintf(stderr, "MainMenu: %s idle %dx%d hotspot=(%d,%d) entries=%d\n",
                gaf_name, ch->idle_w, ch->idle_h,
                ch->idle_ox, ch->idle_oy, ch->num_entries);
    }

    if (bink_name) {
        /* The clips count only when the player can open them: a build
         * without the decoder falls back to the GAF entries. */
        strncpy(ch->bink_base, bink_name, sizeof(ch->bink_base) - 1);
        open_bink_clip(ch, 0);
        if (ch->active_player) {
            BinkPlayer_Close(ch->active_player);
            ch->active_player = NULL;
            ch->active_clip = -1;
            ch->has_video = 1;
            fprintf(stderr, "MainMenu: Bink videos available for %s\n", bink_name);
        } else {
            ch->bink_base[0] = 0;
        }
    }

    return 0;
}

static void update_character_frame(CharacterAnim *ch) {
    /* Don't free idle_pixels -- it's reused */
    if (ch->current_pixels && ch->current_pixels != ch->idle_pixels) {
        tak_free(ch->current_pixels);
    }

    if (ch->current_entry == 0 && !ch->animating) {
        ch->current_pixels = ch->idle_pixels;
        ch->current_w = ch->idle_w;
        ch->current_h = ch->idle_h;
        /* idle hotspot stays from init */
    } else {
        ch->current_pixels = UI_DecodeEntryIdle(
            ch->gaf, ch->current_entry, ch->rgba_table,
            &ch->current_w, &ch->current_h,
            &ch->current_ox, &ch->current_oy);
        if (!ch->current_pixels) {
            ch->current_pixels = ch->idle_pixels;
            ch->current_w = ch->idle_w;
            ch->current_h = ch->idle_h;
        }
    }
}

/* Open a single Bink clip for a character (clip_index 0-3 maps to files 4-7) */
static void open_bink_clip(CharacterAnim *ch, int clip_index) {
    if (ch->active_player) {
        BinkPlayer_Close(ch->active_player);
        ch->active_player = NULL;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/Movies/Gui/%s%d.bik",
             TAK_GAME_DIR, ch->bink_base, clip_index + 4);
    ch->active_player = BinkPlayer_Open(path);
    if (!ch->active_player) {
        /* Try uppercase extension */
        snprintf(path, sizeof(path), "%s/Movies/Gui/%s%d.BIK",
                 TAK_GAME_DIR, ch->bink_base, clip_index + 4);
        ch->active_player = BinkPlayer_Open(path);
    }
    ch->active_clip = ch->active_player ? clip_index : -1;
    ch->video_timer = 0;
    if (ch->active_player) {
        fprintf(stderr, "  Bink OPENED: %s (%dx%d, %.3fs/frame)\n",
                path,
                BinkPlayer_GetWidth(ch->active_player),
                BinkPlayer_GetHeight(ch->active_player),
                BinkPlayer_GetFrameDuration(ch->active_player));
    } else {
        fprintf(stderr, "  Bink OPEN FAILED: %s\n", path);
    }
    fflush(stderr);
}

static void shutdown_character(CharacterAnim *ch) {
    if (ch->current_pixels && ch->current_pixels != ch->idle_pixels)
        tak_free(ch->current_pixels);
    if (ch->idle_pixels) tak_free(ch->idle_pixels);
    if (ch->gaf) GAF_Close(ch->gaf);
    if (ch->active_player) BinkPlayer_Close(ch->active_player);
    memset(ch, 0, sizeof(*ch));
}

/* GAF-only doors (no clips on disk): cycle the entries while hovered
 * and finish the cycle after the cursor leaves. */
static void character_gaf_fallback(CharacterAnim *ch, int is_hovered,
                                   float frame_dt) {
    if (is_hovered && !ch->animating) {
        ch->animating = 1;
        ch->returning = 0;
        ch->anim_timer = 0;
    } else if (!is_hovered && ch->animating && !ch->returning) {
        ch->returning = 1;
    }
    if (!ch->animating) return;
    ch->anim_timer += frame_dt;
    if (ch->anim_timer < menu.frame_duration) return;
    ch->anim_timer -= menu.frame_duration;
    ch->current_entry++;
    if (ch->current_entry >= ch->num_entries) {
        ch->current_entry = 0;
        if (ch->returning || !is_hovered) {
            ch->animating = 0;
            ch->returning = 0;
        }
    }
    update_character_frame(ch);
}

/* ── Public API ─────────────────────────────────────────────────── */

int MainMenu_Init(TAK_Platform *platform) {
    memset(&menu, 0, sizeof(menu));
    menu.hovered_button = -1;
    menu.frame_duration = 1.0f / 8.0f;

    /* The offscreen compositing surface is owned by the UI module now
     * (created once in main()). Just verify it's live. */
    if (!UI_Offscreen()) {
        fprintf(stderr, "MainMenu: UI_Init was not called before entering menu\n");
        return -1;
    }

    /* Load background from mainscreen.gaf */
    GAFFile *bg_gaf = NULL;
    uint32_t bg_table[256];
    if (UI_LoadGAFWithPalette("data/anims/mainscreen.gaf", "data/anims/mainscreen.pcx",
                              &bg_gaf, bg_table) != 0) {
        fprintf(stderr, "MainMenu: failed to load background\n");
        return -1;
    }

    int entry_off = GAF_FindSequence(bg_gaf, "MainBG");
    if (entry_off >= 0) {
        int w, h;
        menu.bg_pixels = UI_DecodeFrame(bg_gaf, entry_off, 0, bg_table, &w, &h);
    }

    /* Load options and exit button sprites (3 states each) */
    int opt_off = GAF_FindSequence(bg_gaf, "OptionsButton");
    if (opt_off >= 0) {
        for (int i = 0; i < 3; i++) {
            menu.options_pixels[i] = UI_DecodeFrame(bg_gaf, opt_off, i, bg_table,
                                                    &menu.options_w, &menu.options_h);
        }
    }
    int exit_off = GAF_FindSequence(bg_gaf, "ExitButton");
    if (exit_off >= 0) {
        for (int i = 0; i < 3; i++) {
            menu.exit_pixels[i] = UI_DecodeFrame(bg_gaf, exit_off, i, bg_table,
                                                 &menu.exit_w, &menu.exit_h);
        }
    }
    GAF_Close(bg_gaf);

    /* Load character animations. Credits (snort) has no GAF sprite — it's
     * invisible when idle and only shows its Bink clip on hover. */
    init_character(&menu.characters[0], "singlemachine", "machine");
    init_character(&menu.characters[1], "bodgirl",        "girl");
    init_character(&menu.characters[2], "multiknight",    "knight");
    init_character(&menu.characters[3], NULL,             "snort");

    /* Tooltip font — mainmenu.gui HelpText uses "times new roman (100b).gaf".
     * The matching .gaf/.pcx live under data/fonts/ with a "b_" prefix. */
    menu.tooltip_font = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    if (!menu.tooltip_font) {
        menu.tooltip_font = Font_Load("data/fonts/b_times new roman (100)", UI_RGBAFormat());
    }

    menu.pending_nextstate = -1;
    menu.initialized = 1;
    return 0;
}

int MainMenu_Tick(TAK_Platform *platform, float frame_dt) {
    if (!menu.initialized) return GAMESTATE_MENU;

    int wx, wy, mx, my;
    SDL_GetMouseState(&wx, &wy);
    int mouse_in_canvas = TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my);
    if (!mouse_in_canvas) { mx = -1; my = -1; }
    /* TAK_FORCE_HOVER=<index> simulates hover on that button for headless
     * debugging (SDL mouse state is zeroed when the window isn't focused). */
    static int force_hover = -2;
    if (force_hover == -2) {
        const char *fh = getenv("TAK_FORCE_HOVER");
        force_hover = fh ? atoi(fh) : -1;
    }
    menu.hovered_button = -1;
    if (g_debug_force_hover != -2) {
        if (g_debug_force_hover >= 0 && g_debug_force_hover < MENUBTN_COUNT)
            menu.hovered_button = g_debug_force_hover;
    } else if (force_hover >= 0 && force_hover < MENUBTN_COUNT) {
        menu.hovered_button = force_hover;
    } else for (int i = 0; i < MENUBTN_COUNT; i++) {
        SDL_Point pt = { mx, my };
        /* Character buttons (0..3) use the tight body hit-rect. Others use
         * the .gui rect. Matches NetPlayerMap_Method28 behavior. */
        const SDL_Rect *hit = (i < MENU_NUM_CHARACTERS)
                              ? &character_hit_rects[i] : &button_rects[i];
        if (SDL_PointInRect(&pt, hit)) {
            menu.hovered_button = i;
            break;
        }
    }

    /* The doors run the original's button states (legacy:148022-148076
     * per tick, legacy:148064 on enter). A clip only hands over once
     * it has played to its end; 6 holds its last frame until the
     * cursor leaves. Enter counts on the crossing, as the original's
     * cursor-widget change does. */
    for (int i = 0; i < MENU_NUM_CHARACTERS; i++) {
        CharacterAnim *ch = &menu.characters[i];
        int inside = (menu.hovered_button == i);
        int entered = inside && !ch->inside;
        ch->inside = inside;
        if (!ch->has_video) {
            character_gaf_fallback(ch, inside, frame_dt);
            continue;
        }
        int next = ch->state;
        if (entered) {
            ch->hovered_flag = 1;
            if (ch->state == 2) next = 5;
        }
        if (ch->hovered_flag && !inside && ch->state == 6) {
            ch->hovered_flag = 0;
            next = 7;
        }
        if (next == ch->state) {
            int finished = 1;
            if (ch->active_player) {
                if (!BinkPlayer_IsFinished(ch->active_player)) {
                    /* Advance at the clip's own rate; the last decoded
                     * frame stays on screen once it ends. */
                    double fd = BinkPlayer_GetFrameDuration(ch->active_player);
                    if (fd <= 0) fd = 1.0 / 30.0;
                    ch->video_timer += frame_dt;
                    while (ch->video_timer >= fd) {
                        ch->video_timer -= fd;
                        if (!BinkPlayer_NextFrame(ch->active_player)) break;
                    }
                }
                finished = BinkPlayer_IsFinished(ch->active_player);
            }
            if (finished) {
                switch (ch->state) {
                case 4: next = ch->hovered_flag ? 6 : 2; break;
                case 5: next = 6; break;
                case 7: next = 2; break;
                default: break;
                }
            }
        }
        if (next != ch->state) {
            ch->state = next;
            if (next >= 4 && next <= 7) {
                open_bink_clip(ch, next - 4);
            } else if (ch->active_player) {
                BinkPlayer_Close(ch->active_player);
                ch->active_player = NULL;
                ch->active_clip = -1;
            }
        }
    }

    /* Handle click (on mouse button release over a hovered button). */
    static int prev_mouse_down = 0;
    int mouse_down = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT);
    if (!mouse_down && prev_mouse_down && menu.hovered_button >= 0) {
        if (menu.hovered_button < MENU_NUM_CHARACTERS &&
            menu.characters[menu.hovered_button].has_video) {
            /* A click drops the door to its still (legacy:148002). */
            CharacterAnim *ch = &menu.characters[menu.hovered_button];
            ch->state = 4;
            open_bink_clip(ch, 0);
        }
        switch (menu.hovered_button) {
        case MENUBTN_EXIT: {
            menu.pending_nextstate = GAMESTATE_QUIT;
            break;
        }
        case MENUBTN_SKIRMISH:
            fprintf(stderr, "MainMenu: Skirmish clicked — TODO: open battle setup\n");
            menu.pending_nextstate = GAMESTATE_BATTLE_SETUP;
            /* TODO: transition to GAMESTATE_BATTLE_SETUP (battlemenusingle.gui) */
            break;
        case MENUBTN_STORY:
            menu.pending_nextstate = GAMESTATE_CAMPAIGN;
            break;
        case MENUBTN_MULTI:
            menu.pending_nextstate = GAMESTATE_MULTIPLAYER;
            break;
        case MENUBTN_CREDITS:
            menu.pending_nextstate = GAMESTATE_CREDITS;
            break;
        case MENUBTN_OPTIONS:
            menu.pending_nextstate = GAMESTATE_OPTIONS;
            break;
        }
    }
    prev_mouse_down = mouse_down;

    /* ── Render to offscreen RGBA surface ────────────────────────── */

    SDL_Surface *offscreen = UI_Offscreen();

    /* Background (opaque, fills entire surface) */
    if (menu.bg_pixels) {
        Blit_RGBA(offscreen, 0, 0, menu.bg_pixels, 640, 480);
    }

    /* Character sprites -- use Bink video frames when available, else GAF */
    for (int i = 0; i < MENU_NUM_CHARACTERS; i++) {
        CharacterAnim *ch = &menu.characters[i];

        /* Draw anchor comes from the hit-rect top-left; the GAF idle frame's
         * hotspot gets placed there. For animation frames, substitute idle's
         * hotspot for per-frame hotspot so cycles don't jitter.
         * Reference: the legacy reference line 140378-140424 (hit-rect setup)
         * and NetPlayerMap_Method28 at line 147978. */
        const SDL_Rect *hr = &character_hit_rects[i];

        if (ch->has_video && ch->active_player && ch->active_clip >= 0) {
            const uint32_t *vpixels = BinkPlayer_GetPixels(ch->active_player);
            int vw = BinkPlayer_GetWidth(ch->active_player);
            int vh = BinkPlayer_GetHeight(ch->active_player);
            if (vpixels && vw > 0 && vh > 0) {
                /* The original game draws the Bink frame with its top-left
                 * at the .gui rect's (x, y), using the video's natural
                 * width/height — see the legacy reference CheatDetect_Process
                 * at line 35248+ (quad vertices set from stored button.x,
                 * button.y plus the Bink handle's intrinsic w/h). */
                const SDL_Rect *gr = &button_rects[i];
                Blit_RGBA(offscreen, gr->x, gr->y, vpixels, vw, vh);
                continue;
            }
        }

        if (ch->current_pixels) {
            int dx = hr->x - ch->current_ox;
            int dy = hr->y - ch->current_oy;
            Blit_RGBA(offscreen, dx, dy,
                      ch->current_pixels, ch->current_w, ch->current_h);
        }
    }

    /* Options button */
    if (menu.options_pixels[0]) {
        int state = (menu.hovered_button == MENUBTN_OPTIONS) ? 1 : 0;
        if (menu.options_pixels[state]) {
            Blit_RGBA(offscreen, 524, 406,
                      menu.options_pixels[state], menu.options_w, menu.options_h);
        }
    }

    /* Exit button */
    if (menu.exit_pixels[0]) {
        int state = (menu.hovered_button == MENUBTN_EXIT) ? 1 : 0;
        if (menu.exit_pixels[state]) {
            Blit_RGBA(offscreen, 68, 407,
                      menu.exit_pixels[state], menu.exit_w, menu.exit_h);
        }
    }

    /* Version text — bottom strip above HelpText, mirroring legacy
     * ("v 2.0 Demo" in the reference image). Shown whenever nothing is
     * hovered so it doesn't fight the hover tooltip. */
    if (menu.tooltip_font && menu.hovered_button < 0) {
        const char *version = "v 0.1 TAK-RE";
        int vw = Font_MeasureString(menu.tooltip_font, version);
        int vx = 320 - vw / 2;
        int vy = helptext_rect.y - 18;
        Font_DrawString(menu.tooltip_font, offscreen, vx, vy, version);
    }

    /* Tooltip text — centered horizontally in the HelpText rect. Mirrors the
     * original's behavior of showing the hovered button's help string in the
     * bottom strip (mainmenu.gui HelpText at 172,441,296,31). */
    if (menu.tooltip_font && menu.hovered_button >= 0 &&
        menu.hovered_button < MENUBTN_COUNT) {
        const char *text = button_tooltips[menu.hovered_button];
        int tw = Font_MeasureString(menu.tooltip_font, text);
        int tx = helptext_rect.x + (helptext_rect.w - tw) / 2;
        int ty = helptext_rect.y;
        Font_DrawString(menu.tooltip_font, offscreen, tx, ty, text);
    }

    /* Hand the composited surface to the window. */
    UI_Present(platform);

    int next = (menu.pending_nextstate >=0 ) ? menu.pending_nextstate : GAMESTATE_MENU;
    menu.pending_nextstate = -1;
    return next;
}

void MainMenu_DebugForceHover(int button) {
    g_debug_force_hover = button;
}

int MainMenu_DebugCharacterState(int character) {
    if (!menu.initialized) return -1;
    if (character < 0 || character >= MENU_NUM_CHARACTERS) return -1;
    if (!menu.characters[character].has_video) return -1;
    return menu.characters[character].state;
}

void MainMenu_Shutdown(void) {
    if (!menu.initialized) return;

    /* The offscreen surface is owned by the UI module; main() frees it. */
    if (menu.bg_pixels) tak_free(menu.bg_pixels);

    for (int i = 0; i < MENU_NUM_CHARACTERS; i++) {
        shutdown_character(&menu.characters[i]);
    }
    for (int i = 0; i < 3; i++) {
        if (menu.options_pixels[i]) tak_free(menu.options_pixels[i]);
        if (menu.exit_pixels[i]) tak_free(menu.exit_pixels[i]);
    }

    if (menu.tooltip_font) Font_Free(menu.tooltip_font);

    memset(&menu, 0, sizeof(menu));
}
