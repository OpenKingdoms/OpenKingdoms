/*
 * hud.c -- In-game HUD via the legacy .gui dialog system.
 *
 * Mirrors the legacy engine: the in-game UI is described entirely by
 * `data/guis/<faction>ingame.gui` — a parsed GUIDialog whose widgets
 * reference GAFs/entries/frames + fonts at authored positions. The
 * generic GUIRuntime renderer (already used by every menu screen)
 * walks that tree and composes the HUD pixel-for-pixel like the
 * original. We don't hand-roll panels, recesses, or button positions.
 *
 * On-top of the rendered dialog we overlay only the dynamic content
 * the .gui can't express: the selected unit's portrait JPG, the live
 * "Mana N/M" + rate strings, the unit's name + status text, the
 * health/mana bars, and the live action-button command-mode halo.
 */

#include "tak_hud.h"
#include "tak_ui.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_gpu.h"
#include "tak_memory.h"
#include "tak_world.h"
#include "tak_battle_config.h"
#include "tak_unit.h"
#include "tak_game_sound.h"
#include "tak_font.h"
#include "tak_hud_text.h"
#include "tak_hpi.h"
#include "tak_jpg.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* ── State ──────────────────────────────────────────────────────────── */

static GUIDialog   g_dialog;
static GUIRuntime *g_rt          = NULL;
static int         g_dialog_loaded = 0;
static int         g_assets_loaded = 0;
static Font       *g_font        = NULL;
static HUDText    *g_text        = NULL;

/* Command-mode (legacy click-button-then-click-world flow). */
static int g_cmd_mode = HUD_CMD_NONE;

/* When in HUD_CMD_PLACE_BUILD: which buildable the player picked. */
static int g_build_def_idx = -1;

/* Build menu state: list of buildables for the currently selected
 * builder, refreshed when the selection's def changes. */
static int g_build_list[24];
static int g_build_list_n = 0;
static int g_build_list_for_def = -1;  /* def_idx the list was built for */

/* Build slot rects (window pixels) — recomputed each frame in
 * HUD_Draw and consumed by HUD_HandleSidebarClick. */
typedef struct { SDL_Rect rect; int def_idx; } HUDBuildSlot;
static HUDBuildSlot g_build_slots_live[24];
static int          g_build_slots_count = 0;

/* Portrait cache by def_idx. We keep BOTH raw RGBA pixels (so we can
 * blit into the UI canvas at the same compositing layer as the HUD
 * widgets) and a GPU_Texture (so we can blit to renderer if needed).
 * Without the canvas blit the GUI's empty UnitPic frame draws on top
 * of any GPU overlay. */
static SDL_Surface *g_portrait_surf[64];
static int          g_portraits_def[64];

/* Per-weapon icon cache, keyed by JPEG basename (e.g. "LightningSB").
 * Loaded lazily from `data/anims/weaponpic/<basename>.jpg`. Mirrors
 * legacy `legacy:250088` which reads each weapon TDF's
 * `buttonimage*` fields and resolves them under
 * `Path_BuildFullPath(..., str_anims_WeaponPic, ...)`. */
typedef struct WeaponIconEntry {
    char         key[40];
    SDL_Surface *surf;
} WeaponIconEntry;
#define HUD_MAX_WEAPON_ICONS 64
static WeaponIconEntry g_weapon_icons[HUD_MAX_WEAPON_ICONS];
static int             g_weapon_icon_count = 0;

/* Action-button live rects, rebuilt every frame from the parsed dialog
 * so HUD_HandleSidebarClick can hit-test correctly. */
typedef struct {
    SDL_Rect rect;
    int      mode;
} HUDActionSlot;
static HUDActionSlot g_action_slots[32];
static int           g_action_slot_count = 0;

/* Cursor sprite cache, indexed by HUD_CMD_*. NULL = no cursor for
 * that mode (immediate-action button — no cursor swap). */
static GPU_Texture *g_cursors[128];
static int          g_cursors_w[128];
static int          g_cursors_h[128];
static int          g_cursors_off_x[128];
static int          g_cursors_off_y[128];

/* Mapping from .gui widget name to HUD_CMD_* code. The legacy
 * araingame.gui authors these widget names; we use them as the
 * canonical button registry. Order doesn't matter — lookups are
 * by name. */
typedef struct {
    const char *widget_name;
    int         mode;
    int         cursor_entry;  /* index into cursors.gaf, or -1 if none */
} HUDButtonBinding;

static const HUDButtonBinding g_button_bindings[] = {
    /* Movement / targeting (cursor swap + world-click executes). */
    { "MOVE",            HUD_CMD_MOVE,         6 /* CursorMove   */ },
    { "ATTACK",          HUD_CMD_ATTACK,       0 /* CursorAttack */ },
    { "GUARD",           HUD_CMD_GUARD,        1 /* CursorDefend */ },
    { "PATROL",          HUD_CMD_PATROL,       8 /* CursorPatrol */ },
    { "LOAD",            HUD_CMD_LOAD,         5 /* Cursorload   */ },
    { "UNLOAD",          HUD_CMD_UNLOAD,      16 /* CursorUnload */ },
    { "HEAL",            HUD_CMD_HEAL,        15 /* cursorrepair */ },
    { "CLEAR",           HUD_CMD_CLEAR,       22 /* Cursorreclamate */ },
    { "SpecialWeapon",   HUD_CMD_W_SPECIAL,    0 /* CursorAttack */ },
    /* Immediate (no cursor swap; click executes order on selection). */
    { "STOP",            HUD_CMD_STOP,        -1 },
    { "Offensive",       HUD_CMD_AGGRO_OFF,   -1 },
    { "Defensive",       HUD_CMD_AGGRO_DEF,   -1 },
    { "Passive",         HUD_CMD_AGGRO_PAS,   -1 },
    { "PrimaryWeapon",   HUD_CMD_W_PRIMARY,   -1 },
    { "SecondaryWeapon", HUD_CMD_W_SECONDARY, -1 },
    { "CloakOn",         HUD_CMD_CLOAK_ON,    -1 },
    { "CloakOff",        HUD_CMD_CLOAK_OFF,   -1 },
};
#define HUD_NUM_BUTTON_BINDINGS \
    (sizeof(g_button_bindings) / sizeof(g_button_bindings[0]))

/* Mana / Health / Portrait rects — pulled from the dialog by widget name
 * once at HUD_Init. Used by the per-frame overlay. */
static SDL_Rect g_rect_unit_image;
static SDL_Rect g_rect_health_bar;
static SDL_Rect g_rect_mana_bar;
static SDL_Rect g_rect_unit_text;
static SDL_Rect g_rect_mana_text;     /* heuristic: mana box area */
static int      g_rect_have_unit_image = 0;
static int      g_rect_have_health_bar = 0;
static int      g_rect_have_mana_bar   = 0;
static int      g_rect_have_unit_text  = 0;

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Decode + cache the buildpic JPG for a unit def as an SDL_Surface.
 * The surface is what we blit into the UI canvas (UI_Offscreen) so
 * the portrait lands in the SAME compositing layer as the dialog's
 * widget frames — otherwise the empty UnitPic frame from the .gui
 * (drawn last as part of the canvas) covers any renderer-side blit. */
static SDL_Surface *portrait_surface_for_def(TAK_Platform *plat, int def_idx) {
    (void)plat;
    if (def_idx < 0) return NULL;
    for (int i = 0; i < 64; i++) {
        if (g_portraits_def[i] == def_idx) return g_portrait_surf[i];
    }
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d || !d->unitname[0]) return NULL;

    char path[128], lower[40];
    int n = 0;
    while (d->unitname[n] && n < (int)sizeof(lower) - 1) {
        char c = d->unitname[n];
        lower[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        n++;
    }
    lower[n] = '\0';
    snprintf(path, sizeof(path), "data/anims/buildpic/%s.jpg", lower);

    void *jpg_bytes = NULL;
    uint32_t jpg_size = 0;
    if (VFS_ReadFile(path, &jpg_bytes, &jpg_size) != 0 || !jpg_bytes) return NULL;
    uint32_t *rgba = NULL;
    int w = 0, h = 0;
    int ok = (JPG_DecodeRGBA((const uint8_t *)jpg_bytes, jpg_size,
                              &rgba, &w, &h) == 0) && rgba;
    VFS_FreeBuffer(jpg_bytes);
    if (!ok) return NULL;

    /* Wrap the decoded RGBA in an SDL_Surface (deep-copy via
     * CreateRGBSurfaceFrom semantics: we keep ownership of `rgba`
     * inside the surface's userdata so it lives as long as the
     * surface). Simpler path: copy the pixels into a fresh surface
     * we own outright. */
    SDL_Surface *src = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    if (!src) { tak_free(rgba); return NULL; }
    SDL_LockSurface(src);
    memcpy(src->pixels, rgba, (size_t)w * h * 4);
    SDL_UnlockSurface(src);
    tak_free(rgba);

    for (int i = 0; i < 64; i++) {
        if (g_portraits_def[i] < 0) {
            g_portrait_surf[i] = src;
            g_portraits_def[i] = def_idx;
            fprintf(stderr, "HUD: portrait surface cached %s (%dx%d)\n",
                    path, w, h);
            return src;
        }
    }
    return src;
}

/* Load (or fetch from cache) a weaponpic JPEG by basename. The legacy
 * engine resolves each weapon's `buttonimage*` field under
 * `data/anims/weaponpic/<name>.jpg`; we mirror that path. NULL on
 * miss. Returns an SDL_Surface owned by the cache (do not free). */
static SDL_Surface *weapon_icon_load(const char *basename) {
    if (!basename || !basename[0]) return NULL;
    /* Cache hit (case-insensitive). */
    for (int i = 0; i < g_weapon_icon_count; i++) {
        int eq = 1;
        for (int k = 0; ; k++) {
            char a = g_weapon_icons[i].key[k];
            char b = basename[k];
            char la = (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
            char lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != lb) { eq = 0; break; }
            if (!a) break;
        }
        if (eq) return g_weapon_icons[i].surf;
    }
    if (g_weapon_icon_count >= HUD_MAX_WEAPON_ICONS) return NULL;

    /* Lower-case the basename so the on-disk filename (which is also
     * lower-case) matches. */
    char lower[40];
    int n = 0;
    while (basename[n] && n < (int)sizeof(lower) - 1) {
        char c = basename[n];
        lower[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        n++;
    }
    lower[n] = '\0';

    char path[160];
    snprintf(path, sizeof(path), "data/anims/weaponpic/%s.jpg", lower);
    void *jpg_bytes = NULL;
    uint32_t jpg_size = 0;
    if (VFS_ReadFile(path, &jpg_bytes, &jpg_size) != 0 || !jpg_bytes) {
        fprintf(stderr, "HUD: weapon icon miss: %s\n", path);
        return NULL;
    }
    uint32_t *rgba = NULL;
    int w = 0, h = 0;
    int ok = (JPG_DecodeRGBA((const uint8_t *)jpg_bytes, jpg_size,
                              &rgba, &w, &h) == 0) && rgba;
    VFS_FreeBuffer(jpg_bytes);
    if (!ok) return NULL;

    SDL_Surface *src = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    if (!src) { tak_free(rgba); return NULL; }
    SDL_LockSurface(src);
    memcpy(src->pixels, rgba, (size_t)w * h * 4);
    SDL_UnlockSurface(src);
    tak_free(rgba);

    WeaponIconEntry *e = &g_weapon_icons[g_weapon_icon_count++];
    snprintf(e->key, sizeof(e->key), "%s", basename);
    e->surf = src;
    fprintf(stderr, "HUD: weapon icon cached %s (%dx%d)\n", path, w, h);
    return src;
}

/* Resolve the .gui path for the local player's faction. */
static const char *local_player_gui_path(const GameWorld *w) {
    if (!w) return "data/guis/araingame.gui";
    int side = w->cfg.players[0].side;
    switch (side) {
        case TAK_SIDE_ARAMON: return "data/guis/araingame.gui";
        case TAK_SIDE_TAROS:  return "data/guis/taringame.gui";
        case TAK_SIDE_VERUNA: return "data/guis/veringame.gui";
        case TAK_SIDE_ZHON:   return "data/guis/zoningame.gui";
        default:              return "data/guis/araingame.gui";
    }
}

/* Cache widget rects by name from the parsed dialog. The names match
 * what araingame.gui declares (see legacy file): UnitImage, HealthBar,
 * ManaBar, UnitText, plus the action buttons (MOVE/ATTACK/GUARD/PATROL/STOP). */
static int find_widget_rect(SDL_Rect *out, const char *name) {
    if (!g_rt || !out || !name) return 0;
    const GUIWidget *w = GUIRuntime_WidgetByName(g_rt, name);
    if (!w) return 0;
    *out = w->rect;
    return 1;
}

/* ── Public API ────────────────────────────────────────────────────── */

void HUD_Init(TAK_Platform *plat, GameWorld *world) {
    if (!plat || !world) return;

    /* The legacy araingame.gui was authored for 640×480. The
     * GUIRuntime renders into UI_Offscreen() at native 640×480
     * coordinates and the platform up-scales to the actual window
     * on present. So we use legacy coords directly. */
    world->viewport_w = plat->window_w;
    world->viewport_h = plat->window_h;

    if (!g_dialog_loaded) {
        const char *path = local_player_gui_path(world);
        if (GUIDialog_Load(&g_dialog, path) == 0) {
            g_rt = GUIRuntime_Create(&g_dialog);
            g_dialog_loaded = 1;
            fprintf(stderr, "HUD: loaded %s (root %dx%d, %d widgets)\n",
                    path,
                    g_dialog.root.rect.w, g_dialog.root.rect.h,
                    g_dialog.num_children);
            /* Suppress the root background — it's a 256x256 gui.gaf
             * DefaultPanel that would otherwise paint over the top-
             * left of the world view. Only the per-widget art (sidebar
             * + bottom strip) should render. */
            if (g_rt) GUIRuntime_HideRoot(g_rt);

            /* Clear placeholder labels authored into the .gui — they
             * render as literal "UnitText"/"ActionText"/etc. strings
             * until the engine binds them. The legacy engine refreshes
             * these labels every frame from the selected unit's data;
             * we do the same in HUD_Draw, but for unbound labels
             * (HelpText, etc.) start them empty. */
            /* Confirmed widget names from data/guis/araingame.gui:
             *   UnitText / UnitInfo1 / UnitInfo2  — selected unit text
             *   ActionText                         — current order
             *   HelpText                           — tooltip
             *   KillCount                          — kills tally
             *   PositiveM                          — "+NNNN" mana income
             *   NegativeM                          — "-NNNN" mana spend
             * All ship with literal placeholder text in the .gui. We
             * clear them here; HUD_Draw refills the ones we have live
             * data for each frame. */
            if (g_rt) {
                static const char *kPlaceholders[] = {
                    "UnitText", "UnitInfo1", "UnitInfo2",
                    "ActionText", "HelpText", "KillCount",
                    "PositiveM", "NegativeM",
                    NULL
                };
                for (int k = 0; kPlaceholders[k]; k++) {
                    GUIRuntime_SetWidgetText(g_rt, kPlaceholders[k], "");
                }
            }
        } else {
            fprintf(stderr, "HUD: failed to load %s\n", path);
        }
    }

    /* The legacy dialog reserves the right strip and bottom bar. We
     * compute the in-window viewport reservation from the root dialog
     * size — but since the dialog is 640×480 and our window may be
     * larger, the platform's UI present blits the offscreen at fit/
     * letterbox. The unit-rendering path uses world->viewport_w/h to
     * compute camera bounds and clamping, so we expose the legacy
     * proportions: bottom strip ~49px at base 480, sidebar ~128px at
     * base 640. */
    if (g_rt) {
        const float base_w = 640.0f, base_h = 480.0f;
        float sx = (float)plat->window_w / base_w;
        float sy = (float)plat->window_h / base_h;
        world->viewport_w = plat->window_w - (int)(128 * sx);
        world->viewport_h = plat->window_h - (int)( 49 * sy);
    }

    if (!g_assets_loaded) {
        SDL_PixelFormat *fmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
        g_font = Font_Load("data/anims/font12", fmt);
        SDL_FreeFormat(fmt);
        if (g_font) g_text = HUDText_Load(plat, g_font);
        for (int i = 0; i < 64; i++) {
            g_portrait_surf[i] = NULL;
            g_portraits_def[i] = -1;
        }
        for (int i = 0; i < 128; i++) g_cursors[i] = NULL;
        g_assets_loaded = 1;

        /* Cache rects by widget name once. */
        g_rect_have_unit_image = find_widget_rect(&g_rect_unit_image, "UnitImage");
        g_rect_have_health_bar = find_widget_rect(&g_rect_health_bar, "HealthBar");
        g_rect_have_mana_bar   = find_widget_rect(&g_rect_mana_bar,   "ManaBar");
        g_rect_have_unit_text  = find_widget_rect(&g_rect_unit_text,  "UnitText");
        fprintf(stderr,
            "HUD: rects: img=%d hp=%d mp=%d txt=%d\n",
            g_rect_have_unit_image, g_rect_have_health_bar,
            g_rect_have_mana_bar,   g_rect_have_unit_text);

        /* Load cursor sprites for every targeting mode in the binding
         * table. Hotspots come from the GAF frame headers (off=(x,y)
         * fields), which we read via FrameHeader after decoding. */
        const char *cur_gaf = "data/anims/cursors.gaf";
        Palette pal;
        int pal_ok = (Palette_LoadPCX(&pal, "data/anims/cursors.pcx") == 0);
        SDL_PixelFormat *cf = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
        uint32_t cur_table[256];
        if (pal_ok) Palette_BuildRGBATable(&pal, cf, cur_table, 9);
        SDL_FreeFormat(cf);
        GAFFile *cgaf = NULL;
        int gaf_ok = pal_ok && (GAF_Open(&cgaf, cur_gaf) == 0) && cgaf;
        for (size_t b = 0; b < HUD_NUM_BUTTON_BINDINGS && gaf_ok; b++) {
            int e = g_button_bindings[b].cursor_entry;
            int m = g_button_bindings[b].mode;
            if (e < 0 || m < 0 || m >= 128) continue;
            if (g_cursors[m]) continue;  /* already loaded for this mode */
            if ((uint32_t)e >= cgaf->num_entries) continue;
            uint32_t entry_off = *(const uint32_t *)(cgaf->data + 12 + e * 4);
            FrameHeader *fh = NULL;
            if (GAF_GetFrameInfo(cgaf, entry_off, 0, &fh) != 0 || !fh) continue;
            uint32_t *pix = GAF_DecodeFrameRGBA(cgaf, fh, cur_table);
            if (!pix) continue;
            GPU_Texture *t = GPU_UploadRGBA(plat, pix, fh->width, fh->height);
            tak_free(pix);
            if (t) {
                GPU_SetTextureFilter(t, 0);
                GPU_SetTextureBlend(t, 1);
                g_cursors[m]       = t;
                g_cursors_w[m]     = fh->width;
                g_cursors_h[m]     = fh->height;
                g_cursors_off_x[m] = fh->offset_x;
                g_cursors_off_y[m] = fh->offset_y;
            }
        }
        /* Context cursors, located by GAF sequence name (the legacy
         * cursors.gaf carries select/normal/red among its entries —
         * digs:render asset table :161475-161505). Each id tries a
         * couple of historical spellings. */
        if (gaf_ok) {
            static const struct { int id; const char *names[3]; } ctx[] = {
                { HUD_CUR_SELECT, { "cursorselect", "select",  NULL } },
                { HUD_CUR_NORMAL, { "cursornormal", "normal",  NULL } },
                { HUD_CUR_RED,    { "cursorred",    "red",     NULL } },
            };
            for (size_t c = 0; c < sizeof(ctx) / sizeof(ctx[0]); c++) {
                int m = ctx[c].id;
                if (m < 0 || m >= 128 || g_cursors[m]) continue;
                int e = -1;
                for (int nn = 0; nn < 3 && e < 0 && ctx[c].names[nn]; nn++)
                    e = GAF_FindSequence(cgaf, ctx[c].names[nn]);
                if (e < 0 || (uint32_t)e >= cgaf->num_entries) continue;
                uint32_t entry_off =
                    *(const uint32_t *)(cgaf->data + 12 + e * 4);
                FrameHeader *fh = NULL;
                if (GAF_GetFrameInfo(cgaf, entry_off, 0, &fh) != 0 || !fh)
                    continue;
                uint32_t *pix = GAF_DecodeFrameRGBA(cgaf, fh, cur_table);
                if (!pix) continue;
                GPU_Texture *t = GPU_UploadRGBA(plat, pix,
                                                fh->width, fh->height);
                tak_free(pix);
                if (t) {
                    GPU_SetTextureFilter(t, 0);
                    GPU_SetTextureBlend(t, 1);
                    g_cursors[m]       = t;
                    g_cursors_w[m]     = fh->width;
                    g_cursors_h[m]     = fh->height;
                    g_cursors_off_x[m] = fh->offset_x;
                    g_cursors_off_y[m] = fh->offset_y;
                }
            }
        }
        if (cgaf) GAF_Close(cgaf);
    }
}

int HUD_HitTest(int win_x, int win_y, TAK_Platform *plat) {
    if (!plat) return 0;
    if (!g_dialog_loaded) return 0;
    /* Translate window-pixel back to dialog 640×480 space. */
    const float sx = 640.0f / (float)plat->window_w;
    const float sy = 480.0f / (float)plat->window_h;
    int dx = (int)(win_x * sx);
    int dy = (int)(win_y * sy);
    /* Sidebar = right of viewport, bottom strip = below viewport. */
    if (dx >= 640 - 128) return 1;
    if (dy >= 480 -  49) return 1;
    /* Build-queue strip floats above the bottom bar (when a builder is
     * selected) — clicks on its slots must be treated as HUD clicks
     * so HUD_HandleSidebarClick can consume them, otherwise the world
     * handler eats the click. The slot rects are in window pixels. */
    for (int i = 0; i < g_build_slots_count; i++) {
        const HUDBuildSlot *bs = &g_build_slots_live[i];
        if (win_x >= bs->rect.x && win_x < bs->rect.x + bs->rect.w &&
            win_y >= bs->rect.y && win_y < bs->rect.y + bs->rect.h)
            return 1;
    }
    return 0;
}

/* ── Per-frame: render the dialog + dynamic overlays ───────────────── */

/* Translate a dialog (640×480) rect to window pixels for SDL_Renderer
 * blits. The dialog itself renders into UI_Offscreen at 640×480, but
 * our text/health-bar overlays go directly to the renderer in window
 * pixels — they need to land in the same on-screen position the
 * widget art occupies. */
static SDL_Rect dialog_to_window(TAK_Platform *plat, SDL_Rect r) {
    const float sx = (float)plat->window_w / 640.0f;
    const float sy = (float)plat->window_h / 480.0f;
    SDL_Rect o;
    o.x = (int)(r.x * sx + 0.5f);
    o.y = (int)(r.y * sy + 0.5f);
    o.w = (int)(r.w * sx + 0.5f);
    o.h = (int)(r.h * sy + 0.5f);
    return o;
}

static void overlay_text(TAK_Platform *plat, int x, int y,
                          const char *s, SDL_Color col) {
    if (g_text && s) HUDText_DrawString(plat, g_text, x, y, s, col);
}

static void fill_rect_w(SDL_Renderer *r, SDL_Rect rc, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rc);
}

static void fill_rect_canvas(SDL_Rect rc, SDL_Color c) {
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    SDL_FillRect(off, &rc, SDL_MapRGBA(off->format, c.r, c.g, c.b, c.a));
}

void HUD_Draw(TAK_Platform *plat, const GameWorld *world) {
    if (!plat || !plat->renderer) return;

    /* Forward mouse state to the runtime so hover frames update and
     * widget hit-testing works. The actual click dispatch happens in
     * HUD_HandleSidebarClick (called from ingame.c). Buttons in the
     * legacy dialog also light up on hover via this update call. */
    if (g_rt) {
        int mx = 0, my = 0;
        Uint32 b = (plat->has_focus ? SDL_GetMouseState(&mx, &my) : 0);
        const float sx = 640.0f / (float)plat->window_w;
        const float sy = 480.0f / (float)plat->window_h;
        int dx = (int)(mx * sx);
        int dy = (int)(my * sy);
        char clicked[32] = {0};
        GUIRuntime_Update(g_rt, dx, dy,
                           (b & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0,
                           clicked, sizeof(clicked));
        GUIRuntime_Render(g_rt);
    }

    /* Per-unit button visibility — hide widgets the selected unit
     * doesn't have caps for. Mirrors legacy
     * legacy:150627+ where each cap bit toggles the
     * corresponding dialog widget's visibility flag. */
    if (g_rt) {
        const UnitDef *seldef_cap = Units_GetSelectedDef();
        uint32_t caps = seldef_cap ? seldef_cap->cap_flags : 0;
        int n_weapons = seldef_cap ? seldef_cap->num_weapons : 0;
        struct { const char *name; int show; } vis[] = {
            { "MOVE",            (caps & UNIT_CAP_MOVE)      != 0 },
            { "ATTACK",          (caps & UNIT_CAP_ATTACK)    != 0 },
            { "GUARD",           (caps & UNIT_CAP_GUARD)     != 0 },
            { "PATROL",          (caps & UNIT_CAP_PATROL)    != 0 },
            { "STOP",            (caps & UNIT_CAP_STOP)      != 0 },
            { "HEAL",            (caps & UNIT_CAP_REPAIR)    != 0 },
            { "LOAD",            ((caps & UNIT_CAP_LOAD) ||
                                  (caps & UNIT_CAP_TRANSPORT)) != 0 },
            { "UNLOAD",          (caps & UNIT_CAP_TRANSPORT) != 0 },
            { "CLEAR",           (caps & UNIT_CAP_RECLAIM)   != 0 },
            { "Cloaked",         (caps & UNIT_CAP_CLOAK)     != 0 },
            { "Uncloaked",       (caps & UNIT_CAP_CLOAK)     != 0 },
            { "Active",          (caps & UNIT_CAP_CLOAK)     != 0 },
            { "Inactive",        (caps & UNIT_CAP_CLOAK)     != 0 },
            { "Offensive",       (caps & UNIT_CAP_ATTACK)    != 0 },
            { "Defensive",       (caps & UNIT_CAP_ATTACK)    != 0 },
            { "Passive",         (caps & UNIT_CAP_ATTACK)    != 0 },
            { "PrimaryWeapon",   (caps & UNIT_CAP_W_SWITCH) && n_weapons >= 1 },
            { "SecondaryWeapon", (caps & UNIT_CAP_W_SWITCH) && n_weapons >= 2 },
            { "SpecialWeapon",   (caps & UNIT_CAP_W_SWITCH) && n_weapons >= 3 },
            /* Rank pip: legacy shows it only once a unit is a veteran. */
            { "Experience",      Units_GetSelectedVeteranLevel() > 0 },
            { "BuildMenu",       (caps & UNIT_CAP_BUILDER)   != 0 },
            { "UnitMenu",        (caps & UNIT_CAP_BUILDER)   != 0 },
            { "CrystalBall",     (caps & UNIT_CAP_BUILDER)   != 0 },
        };
        for (size_t i = 0; i < sizeof(vis)/sizeof(vis[0]); i++) {
            GUIRuntime_SetWidgetVisible(g_rt, vis[i].name, vis[i].show);
        }
    }

    /* Per-frame: keep the aggression + active-weapon buttons visually
     * synced with the selected unit's state. Mirror legacy
     * the legacy reference ~150870: active button → frame 1 ("pressed"),
     * inactive → frame 2 ("rest"). For weapon buttons the active slot
     * gets frame 1 too. We use SetFrameOverride so the selected
     * posture stays highlighted regardless of hover. */
    if (g_rt) {
        int aggro = Units_GetSelectedAggroMode();
        int wslot = Units_GetSelectedWeaponSlot();
        /* Aggression buttons (type GUI_WT_BUTTON 4, 3-frame) — pressed
         * frame is index 1 (legacy convention for BUTTONs). For
         * weapon-mode buttons we DON'T override the frame: pick_frame
         * already returns the rest icon (frame 2 for WINDOW) and
         * forcing frame 1 just shows the empty highlight overlay.
         * Active state for weapons is communicated via a halo border
         * drawn separately below. */
        struct { const char *name; int active; } toggles[] = {
            { "Offensive",       aggro == UNIT_AGGRO_OFFENSIVE },
            { "Defensive",       aggro == UNIT_AGGRO_DEFENSIVE },
            { "Passive",         aggro == UNIT_AGGRO_PASSIVE   },
        };
        for (size_t i = 0; i < sizeof(toggles)/sizeof(toggles[0]); i++) {
            const GUIWidget *w = GUIRuntime_WidgetByName(g_rt, toggles[i].name);
            if (!w) continue;
            int frame = (toggles[i].active && w->num_frames >= 2) ? 1 : -1;
            GUIRuntime_SetFrameOverride(g_rt, toggles[i].name, frame);
        }
        /* DIAGNOSTIC (one-shot): dump widget type + frame count for
         * the magic-weapon buttons so we can see what convention they
         * actually follow in araingame.gui. */
        {
            static int once = 0;
            if (!once) {
                once = 1;
                static const char *names[] = {
                    "PrimaryWeapon", "SecondaryWeapon", "SpecialWeapon", NULL
                };
                for (int j = 0; names[j]; j++) {
                    const GUIWidget *ww = GUIRuntime_WidgetByName(g_rt, names[j]);
                    if (ww) {
                        fprintf(stderr, "WeaponBtn debug: %s type=%d frames=%d rect=%d,%d,%d,%d\n",
                                names[j], ww->type, ww->num_frames,
                                ww->rect.x, ww->rect.y, ww->rect.w, ww->rect.h);
                    } else {
                        fprintf(stderr, "WeaponBtn debug: %s NOT FOUND\n", names[j]);
                    }
                }
            }
        }

        /* Weapon-button highlight: thin yellow border on the active
         * slot (legacy "selected" state). Read the rect for each weapon
         * widget and paint after the dialog renders. */
        /* Per-weapon button rendering. Each magic-button widget gets
         * the SELECTED unit's per-weapon icon JPEG blitted on top of
         * the dialog's render at the widget's authored rect. Mirrors
         * legacy `legacy:250088+` (weapon TDF reads
         * `buttonimageup`/`down`/`selected`/`disabled` and resolves to
         * `data/anims/weaponpic/<name>.jpg`).
         *
         * The active weapon uses `icon_selected`; others use
         * `icon_up`. We blit straight into UI_Offscreen so the icon
         * lands in the same compositing layer as the dialog widgets
         * (rather than getting hidden behind them). */
        const UnitDef *seldef = Units_GetSelectedDef();
        struct { const char *name; int slot; } weapons[] = {
            { "PrimaryWeapon",   0 },
            { "SecondaryWeapon", 1 },
            { "SpecialWeapon",   2 },
        };
        SDL_Surface *off = UI_Offscreen();
        for (size_t i = 0; i < sizeof(weapons)/sizeof(weapons[0]); i++) {
            const char *wname = weapons[i].name;
            int wsl = weapons[i].slot;
            int active = (wslot == wsl);

            GUIRuntime_SetFrameOverride(g_rt, wname, 0);

            if (!seldef || wsl >= seldef->num_weapons) continue;

            const UnitWeapon *wp = &seldef->weapons[wsl];
            const char *icon_name = active ? wp->icon_selected : wp->icon_up;
            if (!icon_name || !icon_name[0]) icon_name = wp->icon_up;
            if (!icon_name || !icon_name[0]) continue;

            SDL_Surface *icon = weapon_icon_load(icon_name);
            if (!icon || !off) continue;

            SDL_Rect dlg_r;
            if (!find_widget_rect(&dlg_r, wname)) continue;
            /* Blit the icon into the dialog's authored 32×32 rect. */
            SDL_BlitScaled(icon, NULL, off, &dlg_r);

            if (active) {
                SDL_Rect win = dialog_to_window(plat, dlg_r);
                SDL_Rect halo = { win.x - 2, win.y - 2,
                                   win.w + 4, win.h + 4 };
                SDL_SetRenderDrawColor(plat->renderer, 240, 220, 90, 255);
                SDL_RenderDrawRect(plat->renderer, &halo);
            }
        }

        /* Push live unit + economy strings into the authored label
         * widgets so they render in the dialog's bottom strip at the
         * exact authored positions, with the legacy fonts. This
         * matches what the legacy engine does — the .gui doesn't
         * "know" the unit; the engine refreshes label text every
         * frame from selection state. */
        const char *unit_name = Units_GetSelectedName();
        const char *unit_status = Units_GetSelectedStatus();
        if (unit_name)   GUIRuntime_SetWidgetText(g_rt, "UnitText",  unit_name);
        else             GUIRuntime_SetWidgetText(g_rt, "UnitText",  "");
        if (unit_status) GUIRuntime_SetWidgetText(g_rt, "ActionText", unit_status);
        else             GUIRuntime_SetWidgetText(g_rt, "ActionText", "");

        if (world) {
            char buf[24];
            int32_t income = Economy_GetRegenRate(&world->economy, 1);
            int32_t spend  = Economy_GetSpend    (&world->economy, 1);
            snprintf(buf, sizeof(buf), "+%d", income);
            GUIRuntime_SetWidgetText(g_rt, "PositiveM", buf);
            snprintf(buf, sizeof(buf), "-%d", spend);
            GUIRuntime_SetWidgetText(g_rt, "NegativeM", buf);
        }
    }

    /* Rebuild the live action-button hit-rect list from the dialog
     * each frame using the central widget→mode binding table. The
     * legacy araingame.gui authors widget names (MOVE/ATTACK/GUARD/
     * PATROL/STOP/Offensive/Defensive/Passive/PrimaryWeapon/etc.); we
     * walk every binding and capture whichever ones the loaded dialog
     * actually has. Yellow halo on the rect of the *targeting* mode
     * currently active. */
    g_action_slot_count = 0;
    if (g_rt) {
        for (size_t i = 0;
             i < HUD_NUM_BUTTON_BINDINGS &&
             g_action_slot_count < (int)(sizeof(g_action_slots)/sizeof(g_action_slots[0]));
             i++) {
            SDL_Rect r;
            if (!find_widget_rect(&r, g_button_bindings[i].widget_name)) continue;
            /* Paired buttons share a rect (LOAD/CLEAR, UNLOAD/HEAL);
             * without this the hidden one wins the hit test and the
             * broom armed UNLOAD. */
            if (GUIRuntime_WidgetHidden(g_rt, g_button_bindings[i].widget_name))
                continue;
            SDL_Rect win = dialog_to_window(plat, r);
            g_action_slots[g_action_slot_count].rect = win;
            g_action_slots[g_action_slot_count].mode = g_button_bindings[i].mode;
            g_action_slot_count++;

            if (g_cmd_mode == g_button_bindings[i].mode &&
                HUD_IsTargetingMode(g_cmd_mode))
            {
                SDL_Rect halo = { win.x - 2, win.y - 2,
                                   win.w + 4, win.h + 4 };
                SDL_SetRenderDrawColor(plat->renderer, 240, 220, 90, 255);
                SDL_RenderDrawRect(plat->renderer, &halo);
            }
        }
    }

    /* Selected unit overlays (portrait JPG, name/status text, bars). */
    int sel_count = 0;
    const int *sel = Units_GetSelection(&sel_count);
    if (sel_count > 0 && sel) {
        int handle = sel[0];
        extern int g_units_get_def_idx(int handle);
        int def_idx = g_units_get_def_idx(handle);

        /* Portrait into the UnitImage widget rect. We blit into the
         * UI canvas (UI_Offscreen) rather than direct-to-renderer so
         * the portrait sits in the SAME compositing layer as the
         * dialog widgets — otherwise the empty UnitPic frame from the
         * .gui draws on top of any renderer-side blit. The widget rect
         * is in dialog 640×480 space; we blit at that exact rect, then
         * the platform-present scales the entire canvas to the window. */
        if (g_rect_have_unit_image) {
            SDL_Surface *off = UI_Offscreen();
            SDL_Surface *port = portrait_surface_for_def(plat, def_idx);
            if (off && port) {
                SDL_Rect dst = g_rect_unit_image;
                SDL_BlitScaled(port, NULL, off, &dst);
            }
        }

        /* Unit name + status text into UnitText. */
        if (g_rect_have_unit_text && g_text) {
            SDL_Rect dst = dialog_to_window(plat, g_rect_unit_text);
            const char *name = Units_GetSelectedName();
            const char *status = Units_GetSelectedStatus();
            SDL_Color col = { 230, 215, 170, 255 };
            if (name) overlay_text(plat, dst.x, dst.y, name, col);
            if (status) {
                int sw = HUDText_Measure(g_text, status);
                overlay_text(plat, dst.x + (dst.w - sw)/2,
                              dst.y, status, col);
            }
        }

        /* Health bar fill at the authored position. */
        if (g_rect_have_health_bar) {
            int hp = 0, hp_max = 1;
            Units_GetSelectedHealth(&hp, &hp_max);
            if (hp_max > 0) {
                int filled = (int)((int64_t)g_rect_health_bar.w * hp / hp_max);
                int pct = hp * 100 / hp_max;
                SDL_Color fc = (pct >= 50) ? (SDL_Color){80, 170, 60, 255}
                              : (pct >= 25) ? (SDL_Color){200,170, 40, 255}
                              :                (SDL_Color){200, 60, 30, 255};
                SDL_Rect fb = g_rect_health_bar; fb.w = filled;
                fill_rect_canvas(fb, fc);
            }
        }
    }

    /* Mana display: the dialog has Mana label widgets but no bound
     * value source; we overlay "Mana cur/max" + "+I -S" text near
     * the authored mana label position. The mana bar widget gives us
     * the right anchor since it's positioned with the mana label. */
    /* ManaBar is the SELECTED UNIT's gauge (araingame.gui: 97×2 strip
     * under HealthBar in the unit-info panel), not the player pool.
     * Only units that actually hold mana show a fill — in TAK the pool
     * lives on the monarch, so his bar tracks the player economy;
     * everything else with maxmana uses its own reserve. */
    if (world && g_rect_have_mana_bar) {
        const UnitDef *sd = Units_GetSelectedDef();
        if (sd && sd->max_mana > 0) {
            int32_t cur = Economy_GetMana(&world->economy, 1);
            int32_t max = Economy_GetMaxMana(&world->economy, 1);
            if (max > 0) {
                if (cur < 0) cur = 0;
                if (cur > max) cur = max;
                SDL_Rect fb = g_rect_mana_bar;
                fb.w = (int)((int64_t)fb.w * cur / max);
                fill_rect_canvas(fb, (SDL_Color){80, 130, 220, 255});
            }
        }
    }

    /* ── Build menu (visible when a builder is selected) ──────────
     *
     * Per-builder canbuild list comes from Units_GetBuildables. Each
     * buildable's portrait JPG goes into a slot. The grid lives in
     * the lower portion of the sidebar — dialog-local coords picked
     * to land below the action-button row + above the mana area. */
    g_build_slots_count = 0;
    {
        int sel_count2 = 0;
        const int *sel2 = Units_GetSelection(&sel_count2);
        if (sel2 && sel_count2 > 0) {
            int handle = sel2[0];
            extern int g_units_get_def_idx(int handle);
            int sel_def = g_units_get_def_idx(handle);
            const UnitDef *sd = Units_GetDef(sel_def);
            /* One-time print when the selection's def changes so we
             * can see in stderr whether cap_flags is recognised as a
             * builder for the current unit. */
            static int last_logged_def = -2;
            if (sd && sel_def != last_logged_def) {
                fprintf(stderr,
                    "HUD: selected def=%d (%s) cap_flags=0x%x builder=%d\n",
                    sel_def, sd->unitname, sd->cap_flags,
                    (sd->cap_flags & UNIT_CAP_BUILDER) ? 1 : 0);
                last_logged_def = sel_def;
            }
            if (sd && (sd->cap_flags & UNIT_CAP_BUILDER)) {
                /* (Re)build the cached buildables list when selection
                 * changes its def. */
                if (sel_def != g_build_list_for_def) {
                    g_build_list_n = Units_GetBuildables(
                        sel_def, g_build_list,
                        (int)(sizeof(g_build_list)/sizeof(g_build_list[0])));
                    g_build_list_for_def = sel_def;
                    fprintf(stderr, "HUD: build list for def=%d -> %d entries\n",
                            sel_def, g_build_list_n);
                }
                /* Place build queue ABOVE the bottom strip — same row
                 * position the legacy game uses (see image #114): a
                 * horizontal band of large building icons floating
                 * just above the filigree bar, anchored to the left
                 * edge of the screen. Bottom strip starts at y=431,
                 * so we sit the slots at y≈395..432 and span x=0
                 * onwards. */
                const int slot_w_dlg = 50;
                const int slot_h_dlg = 36;
                const int gap_dlg    = 2;
                const int grid_x0_dlg = 0;
                const int grid_y0_dlg = 395;
                for (int b = 0; b < g_build_list_n; b++) {
                    SDL_Rect dlg_rect = {
                        grid_x0_dlg + b * (slot_w_dlg + gap_dlg),
                        grid_y0_dlg,
                        slot_w_dlg, slot_h_dlg
                    };
                    /* Dark recess in canvas. */
                    SDL_Surface *off = UI_Offscreen();
                    if (off) {
                        SDL_Rect fill = dlg_rect;
                        SDL_FillRect(off, &fill,
                            SDL_MapRGBA(off->format, 30, 24, 16, 255));
                        SDL_Surface *port = portrait_surface_for_def(plat, g_build_list[b]);
                        if (port) {
                            SDL_Rect dst = dlg_rect;
                            SDL_BlitScaled(port, NULL, off, &dst);
                        }
                    }
                    /* Window-pixel rect for hit-test + active halo. */
                    SDL_Rect win = dialog_to_window(plat, dlg_rect);
                    g_build_slots_live[g_build_slots_count].rect = win;
                    g_build_slots_live[g_build_slots_count].def_idx = g_build_list[b];
                    g_build_slots_count++;
                    if (g_cmd_mode == HUD_CMD_PLACE_BUILD &&
                        g_build_def_idx == g_build_list[b])
                    {
                        SDL_Rect halo = { win.x - 2, win.y - 2,
                                           win.w + 4, win.h + 4 };
                        SDL_SetRenderDrawColor(plat->renderer, 240, 220, 90, 255);
                        SDL_RenderDrawRect(plat->renderer, &halo);
                    }
                }
            } else {
                /* Selection lost builder cap — clear the cached list so
                 * a future builder selection rebuilds it. */
                g_build_list_for_def = -1;
                g_build_list_n = 0;
            }
        } else {
            g_build_list_for_def = -1;
            g_build_list_n = 0;
        }
    }
}

/* ── Command-mode interaction ──────────────────────────────────────── */

int HUD_GetCommandMode(void) { return g_cmd_mode; }
void HUD_ClearCommandMode(void) {
    g_cmd_mode = HUD_CMD_NONE;
    g_build_def_idx = -1;
}

int  HUD_GetBuildPlacementDefIdx(void) {
    return (g_cmd_mode == HUD_CMD_PLACE_BUILD) ? g_build_def_idx : -1;
}
void HUD_BeginBuildPlacement(int def_idx) {
    g_build_def_idx = def_idx;
    g_cmd_mode = HUD_CMD_PLACE_BUILD;
}

int HUD_IsTargetingMode(int mode) {
    /* Targeting modes hold the cursor and wait for a world-click.
     * Immediate modes execute on the button-click and clear. The
     * legacy distinction is whether the engine sets a "pending order"
     * cursor (the legacy reference ~151328 Console_RegisterHotkeys
     * branch) or fires the order directly. */
    switch (mode) {
        case HUD_CMD_MOVE:
        case HUD_CMD_ATTACK:
        case HUD_CMD_GUARD:
        case HUD_CMD_PATROL:
        case HUD_CMD_LOAD:
        case HUD_CMD_UNLOAD:
        case HUD_CMD_HEAL:
        case HUD_CMD_CLEAR:
        case HUD_CMD_W_SPECIAL:
        case HUD_CMD_PLACE_BUILD:
            return 1;
        default:
            return 0;
    }
}

/* Queue-count badges — drawn AFTER UI_Present so they sit on top of
 * the composited card art (drawing earlier gets covered by the
 * offscreen canvas upload). */
void HUD_DrawQueueBadges(TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;
    int n_sel = 0;
    const int *sel = Units_GetSelection(&n_sel);
    if (n_sel <= 0) return;
    for (int i = 0; i < g_build_slots_count; i++) {
        const HUDBuildSlot *bs = &g_build_slots_live[i];
        int qn = Units_FactoryQueuedCountForDef(sel[0], bs->def_idx);
        if (qn <= 0) continue;
        char badge[8];
        snprintf(badge, sizeof(badge), "%d", qn);
        SDL_Color bc = { 255, 240, 160, 255 };
        overlay_text(plat, bs->rect.x + 3, bs->rect.y + 2, badge, bc);
    }
}

int HUD_HandleSidebarRightClick(int win_x, int win_y, TAK_Platform *plat) {
    (void)plat;
    for (int i = 0; i < g_build_slots_count; i++) {
        const HUDBuildSlot *bs = &g_build_slots_live[i];
        if (win_x < bs->rect.x || win_x >= bs->rect.x + bs->rect.w) continue;
        if (win_y < bs->rect.y || win_y >= bs->rect.y + bs->rect.h) continue;
        int n_sel = 0;
        const int *sel = Units_GetSelection(&n_sel);
        if (n_sel > 0 &&
            Units_FactoryDequeueDef(sel[0], bs->def_idx) == 0) {
            GameSound_PlayUI("MenuButton");
        }
        return 1;
    }
    return 0;
}

int HUD_HandleSidebarClick(int win_x, int win_y, TAK_Platform *plat) {
    (void)plat;
    /* Build-menu icons take precedence — they sit in the lower
     * sidebar region and would otherwise miss the action-slot table. */
    for (int i = 0; i < g_build_slots_count; i++) {
        const HUDBuildSlot *bs = &g_build_slots_live[i];
        if (win_x < bs->rect.x || win_x >= bs->rect.x + bs->rect.w) continue;
        if (win_y < bs->rect.y || win_y >= bs->rect.y + bs->rect.h) continue;
        /* Factory (immobile builder) producing a mobile unit: enqueue
         * in-yard, no placement ghost — legacy queue counts. */
        {
            int n_sel = 0;
            const int *sel = Units_GetSelection(&n_sel);
            const UnitDef *bd = Units_GetDef(bs->def_idx);
            const UnitDef *sd = Units_GetSelectedDef();
            if (n_sel > 0 && bd && sd &&
                sd->max_velocity <= 0.0f && bd->max_velocity > 0.0f) {
                Units_FactoryEnqueue(sel[0], bs->def_idx);
                GameSound_PlayUI("addbuild");
                return 1;
            }
        }
        HUD_BeginBuildPlacement(bs->def_idx);
        GameSound_PlayUI("addbuild");   /* legacy queue-add cue (:150087) */
        return 1;
    }
    for (int i = 0; i < g_action_slot_count; i++) {
        const HUDActionSlot *s = &g_action_slots[i];
        if (win_x < s->rect.x || win_x >= s->rect.x + s->rect.w) continue;
        if (win_y < s->rect.y || win_y >= s->rect.y + s->rect.h) continue;

        GameSound_PlayUI("MenuButton");

        /* Targeting modes set g_cmd_mode and wait for a world-click;
         * clicking the same button again toggles off. */
        if (HUD_IsTargetingMode(s->mode)) {
            g_cmd_mode = (g_cmd_mode == s->mode) ? HUD_CMD_NONE : s->mode;
            return 1;
        }

        /* Immediate-action modes dispatch through the unit selection
         * commands directly. Mirrors legacy NetPacket_Method03 which
         * fires the order without a pending-cursor state. */
        switch (s->mode) {
            case HUD_CMD_STOP:
                Units_CommandStopSelected();
                g_cmd_mode = HUD_CMD_NONE;
                break;
            case HUD_CMD_AGGRO_OFF:
                Units_CommandSetAggroSelected(UNIT_AGGRO_OFFENSIVE);
                break;
            case HUD_CMD_AGGRO_DEF:
                Units_CommandSetAggroSelected(UNIT_AGGRO_DEFENSIVE);
                break;
            case HUD_CMD_AGGRO_PAS:
                Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
                break;
            case HUD_CMD_W_PRIMARY:
                Units_CommandSetWeaponSlotSelected(0);
                break;
            case HUD_CMD_W_SECONDARY:
                Units_CommandSetWeaponSlotSelected(1);
                break;
            case HUD_CMD_W_SET_SPEC:
                Units_CommandSetWeaponSlotSelected(2);
                break;
            default:
                /* Cloak on/off etc. — no per-unit state implemented yet. */
                break;
        }
        return 1;
    }
    return 0;
}

void HUD_DrawCommandCursor(TAK_Platform *plat, int win_x, int win_y) {
    if (!plat || !plat->renderer) return;
    if (g_cmd_mode <= 0) return;

    /* Building placement cursor: render the actual building mesh at
     * the cursor position, semi-transparent and tinted green/red
     * to indicate placement validity. Falls back to a simple footprint
     * outline only when no world is loaded. */
    if (g_cmd_mode == HUD_CMD_PLACE_BUILD && g_build_def_idx >= 0) {
        const GameWorld *wd = World_Get();
        if (wd) {
            int32_t world_x = wd->cam_x + win_x;
            int32_t world_y = wd->cam_y + win_y;
            int valid = Units_IsBuildSiteClear(g_build_def_idx, world_x, world_y);
            /* Match the player's current team colour so the ghost reads
             * as theirs. Pull from the first selected unit (the builder)
             * if any; default 0 otherwise. */
            int color_idx = 0;
            int n_sel = 0;
            const int *sel = Units_GetSelection(&n_sel);
            if (sel && n_sel > 0) {
                const Unit *active = Units_GetActive(NULL);
                if (active) color_idx = active[sel[0]].team_color_idx;
            }
            Units_RenderBuildGhost(plat, wd, g_build_def_idx, color_idx,
                                    world_x, world_y,
                                    /*alpha255=*/140, valid);
        }
        return;
    }

    if (g_cmd_mode >= 128) return;
    if (HUD_DrawCursorById(plat, g_cmd_mode, win_x, win_y)) return;
    /* Fallback — should not normally hit. */
    SDL_SetRenderDrawColor(plat->renderer, 240, 220, 90, 255);
    SDL_RenderDrawLine(plat->renderer, win_x - 8, win_y, win_x + 8, win_y);
    SDL_RenderDrawLine(plat->renderer, win_x, win_y - 8, win_x, win_y + 8);
}

int HUD_DrawCursorById(TAK_Platform *plat, int cursor_id,
                       int win_x, int win_y) {
    if (!plat || !plat->renderer) return 0;
    if (cursor_id < 0 || cursor_id >= 128) return 0;
    GPU_Texture *t = g_cursors[cursor_id];
    if (!t) return 0;
    SDL_Rect dst = { win_x - g_cursors_off_x[cursor_id],
                     win_y - g_cursors_off_y[cursor_id],
                     g_cursors_w[cursor_id],
                     g_cursors_h[cursor_id] };
    GPU_DrawToWindow(plat, t, NULL, &dst);
    return 1;
}
