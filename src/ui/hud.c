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
#include "tak_tdf.h"
#include "tak_jpg.h"
#include "tak_util.h"
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

/* Sidebar strings from english/translate/messages.tdf. A miss keeps the
 * key, as the original's lookup does (legacy:267931). */
static char g_msg_carrying[48] = "TRANSPORT_CARRYING_HELPTEXT";
static char g_msg_mana[48]     = "CRYSTALBALL_MOGRIUM_MESSAGE";

/* Command-mode (legacy click-button-then-click-world flow). */
static int g_cmd_mode = HUD_CMD_NONE;

/* When in HUD_CMD_PLACE_BUILD: which buildable the player picked. */
static int g_build_def_idx = -1;

/* Build menu state: list of buildables for the currently selected
 * builder, refreshed when the selection's def changes. */
static int g_build_list[24];
static int g_build_list_n = 0;
static int g_build_list_for_def = -1;  /* def_idx the list was built for */

/* Build slot rects, recomputed each frame in HUD_Draw and consumed by
 * HUD_HandleSidebarClick. `dlg` is dialog (canvas) space, `rect` the
 * same rect mapped to window pixels. `badge` is the queue-count text
 * box in dialog space (w == 0 when the slot has no queue). */
typedef struct {
    SDL_Rect dlg;
    SDL_Rect rect;
    SDL_Rect badge;
    int      def_idx;
} HUDBuildSlot;
static HUDBuildSlot g_build_slots_live[24];
static int          g_build_slots_count = 0;

/* Orders, gates and the build menu belong to the local player's own
 * units. A unit inspected from another side only shows its panel. */
extern int g_units_get_player(int handle);
static int hud_selection_is_own(void) {
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    if (!sel || n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if (g_units_get_player(sel[i]) != 1) return 0;
    return 1;
}

/* Play-area and minimap slot in dialog (640x480 canvas) space, read
 * from the dialog at init. Legacy bounds the play area the same way:
 * left of UnitMenu.x and above BottomBar.y (legacy:150187-150214). */
static SDL_Rect g_viewport_dlg = {   0,   0, 512, 431 };
static SDL_Rect g_minimap_dlg  = { 512,   0, 128, 128 };

/* araingame.gui carries TWO unit-info panels sharing widget names:
 * UnitInfo1 describes the selected unit and UnitInfo2 its target
 * (legacy:152113-152143 refreshes each through the same routine). We
 * resolve the child indices once so each panel can be driven alone. */
/* own_only: shown for the local player's units only. A foreign unit's
 * panel keeps its name and health (reported from play, like the
 * original). */
typedef struct { int index; int is_xp; int own_only; int is_kills; } HUDPanelWidget;
#define HUD_MAX_PANEL_WIDGETS 16
static HUDPanelWidget g_panel1[HUD_MAX_PANEL_WIDGETS];
static int            g_panel1_n = 0;
static HUDPanelWidget g_panel2[HUD_MAX_PANEL_WIDGETS];
static int            g_panel2_n = 0;

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
    { "Active",          HUD_CMD_ACTIVATE,    -1 },
    { "Inactive",        HUD_CMD_DEACTIVATE,  -1 },
};
#define HUD_NUM_BUTTON_BINDINGS \
    (sizeof(g_button_bindings) / sizeof(g_button_bindings[0]))

/* Magic-button widgets, indexed by weapon slot. */
static const char *const g_weapon_widgets[] = {
    "PrimaryWeapon", "SecondaryWeapon", "SpecialWeapon"
};
#define HUD_NUM_WEAPON_WIDGETS \
    ((int)(sizeof(g_weapon_widgets) / sizeof(g_weapon_widgets[0])))

/* Mana / Health / Portrait rects — pulled from the dialog by widget name
 * once at HUD_Init. Used by the per-frame overlay. */
static SDL_Rect g_rect_unit_image;
static SDL_Rect g_rect_health_bar;
static int      g_idx_health_bar = -1;   /* the selection panel's gauges */
static int      g_idx_mana_bar   = -1;
static int      g_idx_unit_text  = -1;   /* the selection panel's name */
static float    g_gauge_health   = 1.0f;
static float    g_gauge_mana     = 1.0f;
static float    g_gauge_pool     = 1.0f;
static SDL_Rect g_rect_mana_bar;
static SDL_Rect g_rect_unit_text;
static SDL_Rect g_rect_mana_text;     /* heuristic: mana box area */
static int      g_rect_have_unit_image = 0;
static int      g_rect_have_health_bar = 0;
static int      g_rect_have_mana_bar   = 0;
static int      g_rect_have_unit_text  = 0;

/* Build-button label font. Legacy creates each build button with
 * "times new roman (100b)" and writes the queued count into it
 * (legacy:150251, legacy:149903-149945). */
static Font *g_font_badge = NULL;

/* ── Helpers ───────────────────────────────────────────────────────── */

static int rect_contains(SDL_Rect outer, SDL_Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

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

/* First child index carrying `name` whose rect sits inside `bounds`.
 * Used to tell the two same-named unit-info panels apart. */
static int widget_index_in(const char *name, SDL_Rect bounds) {
    if (!g_rt || !name) return -1;
    int n = GUIRuntime_NumWidgets(g_rt);
    for (int i = 0; i < n; i++) {
        const GUIWidget *w = GUIRuntime_WidgetAt(g_rt, i);
        if (!w || tak_stricmp(w->name, name) != 0) continue;
        if (rect_contains(bounds, w->rect)) return i;
    }
    return -1;
}

/* Collect every copy of the per-unit info widgets that lives inside
 * `bounds` into `out`. Legacy's null-unit refresh hides exactly this
 * set: the name label, both bars with their backings, the rank pip and
 * the kill tally (legacy:152277-152296, legacy:152496-152506). */
static int collect_panel_widgets(SDL_Rect bounds,
                                 HUDPanelWidget *out, int cap) {
    static const struct { const char *name; int is_xp; int own_only; int is_kills; } kNames[] = {
        { "UnitText",   0, 0 },
        { "HealthBar",  0, 0 },
        { "ManaBar",    0, 1 },
        { "Static0",    0, 0 },   /* araingame.gui's HealthBack/ManaBack */
        { "KillCount",  0, 1, 1 },
        { "Experience", 1, 1 },
    };
    int n = 0;
    if (!g_rt) return 0;
    int total = GUIRuntime_NumWidgets(g_rt);
    for (int i = 0; i < total && n < cap; i++) {
        const GUIWidget *w = GUIRuntime_WidgetAt(g_rt, i);
        if (!w || !rect_contains(bounds, w->rect)) continue;
        for (size_t k = 0; k < sizeof(kNames)/sizeof(kNames[0]); k++) {
            if (tak_stricmp(w->name, kNames[k].name) != 0) continue;
            out[n].index = i;
            out[n].is_xp = kNames[k].is_xp;
            out[n].own_only = kNames[k].own_only;
            out[n].is_kills = kNames[k].is_kills;
            n++;
            break;
        }
    }
    return n;
}

/* Read the play area + minimap slot out of the dialog. Legacy derives
 * the same bounds from the sidebar's x and the bottom strip's y
 * (legacy:150187-150214). The minimap fills the sidebar column above
 * the panel art. */
static void resolve_layout_rects(void) {
    SDL_Rect side, bottom;
    int have_side   = find_widget_rect(&side,   "UnitMenu");
    int have_bottom = find_widget_rect(&bottom, "BottomBar");
    if (have_side)   g_viewport_dlg.w = side.x;
    if (have_bottom) g_viewport_dlg.h = bottom.y;
    g_viewport_dlg.x = 0;
    g_viewport_dlg.y = 0;
    if (have_side) {
        g_minimap_dlg.x = side.x;
        g_minimap_dlg.y = 0;
        g_minimap_dlg.w = 640 - side.x;
        g_minimap_dlg.h = side.y;
    }
}

static void hud_read_message(TDFFile *tdf, const char *key,
                             char *out, size_t cap) {
    if (TDF_PushSection(tdf, key) != 0) return;
    const char *text = TDF_ReadString(tdf, "English", "");
    if (text && *text) snprintf(out, cap, "%s", text);
    TDF_PopSection(tdf);
}

static void hud_load_messages(void) {
    TDFFile *tdf = TDF_Open("english/translate/messages.tdf");
    if (!tdf) return;
    if (TDF_Load(tdf) == 0) {
        hud_read_message(tdf, "TRANSPORT_CARRYING_HELPTEXT",
                         g_msg_carrying, sizeof(g_msg_carrying));
        hud_read_message(tdf, "CRYSTALBALL_MOGRIUM_MESSAGE",
                         g_msg_mana, sizeof(g_msg_mana));
    }
    TDF_Close(tdf);
}

/* ── Public API ────────────────────────────────────────────────────── */

void HUD_Init(TAK_Platform *plat, GameWorld *world) {
    if (!plat || !world) return;
    hud_load_messages();

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

    /* The play area is whatever the dialog leaves free: left of the
     * sidebar, above the bottom strip. The world renders straight to
     * the renderer, so express it in window pixels through the same
     * canvas transform the HUD art is composited with. */
    if (g_rt) {
        resolve_layout_rects();
        SDL_Rect vp = TAK_Platform_CanvasRectToWindow(plat, g_viewport_dlg);
        world->viewport_w = vp.w;
        world->viewport_h = vp.h;
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

        /* Cache rects by widget name once. The per-unit gauges must come
         * from the SELECTED-unit panel (UnitInfo1). A plain name lookup
         * returns the target panel's copy, which is drawn elsewhere. */
        SDL_Rect r_info1, r_info2;
        int have_info1 = find_widget_rect(&r_info1, "UnitInfo1");
        int have_info2 = find_widget_rect(&r_info2, "UnitInfo2");
        g_rect_have_unit_image = find_widget_rect(&g_rect_unit_image, "UnitImage");

        int idx_hp  = have_info1 ? widget_index_in("HealthBar", r_info1) : -1;
        int idx_mp  = have_info1 ? widget_index_in("ManaBar",   r_info1) : -1;
        int idx_txt = have_info1 ? widget_index_in("UnitText",  r_info1) : -1;
        const GUIWidget *w_hp  = GUIRuntime_WidgetAt(g_rt, idx_hp);
        const GUIWidget *w_mp  = GUIRuntime_WidgetAt(g_rt, idx_mp);
        const GUIWidget *w_txt = GUIRuntime_WidgetAt(g_rt, idx_txt);
        if (w_hp)  { g_rect_health_bar = w_hp->rect;  g_rect_have_health_bar = 1; g_idx_health_bar = idx_hp; }
        if (w_mp)  { g_rect_mana_bar   = w_mp->rect;  g_rect_have_mana_bar   = 1; g_idx_mana_bar   = idx_mp; }
        if (w_txt) { g_rect_unit_text  = w_txt->rect; g_rect_have_unit_text  = 1; g_idx_unit_text = idx_txt; }
        if (!g_rect_have_health_bar)
            g_rect_have_health_bar = find_widget_rect(&g_rect_health_bar, "HealthBar");
        if (!g_rect_have_mana_bar)
            g_rect_have_mana_bar = find_widget_rect(&g_rect_mana_bar, "ManaBar");
        if (!g_rect_have_unit_text)
            g_rect_have_unit_text = find_widget_rect(&g_rect_unit_text, "UnitText");

        g_panel1_n = have_info1 ? collect_panel_widgets(r_info1, g_panel1,
                                       HUD_MAX_PANEL_WIDGETS) : 0;
        g_panel2_n = have_info2 ? collect_panel_widgets(r_info2, g_panel2,
                                       HUD_MAX_PANEL_WIDGETS) : 0;
        fprintf(stderr,
            "HUD: rects: img=%d hp=%d mp=%d txt=%d panel1=%d panel2=%d\n",
            g_rect_have_unit_image, g_rect_have_health_bar,
            g_rect_have_mana_bar,   g_rect_have_unit_text,
            g_panel1_n, g_panel2_n);

        if (!g_font_badge)
            g_font_badge = Font_Load("data/fonts/b_times new roman (100b)",
                                     UI_RGBAFormat());

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
    /* One mapping for everything: window -> canvas through the platform,
     * then compare against the dialog-space play area. */
    int dx = 0, dy = 0;
    if (!TAK_Platform_MapMouseToCanvas(plat, win_x, win_y, &dx, &dy)) return 0;
    if (dx >= g_viewport_dlg.x + g_viewport_dlg.w) return 1;
    if (dy >= g_viewport_dlg.y + g_viewport_dlg.h) return 1;
    /* Build buttons float above the bottom bar when a builder is
     * selected, so clicks there count as HUD clicks and
     * HUD_HandleSidebarClick can consume them. */
    for (int i = 0; i < g_build_slots_count; i++) {
        const SDL_Rect *r = &g_build_slots_live[i].dlg;
        if (dx >= r->x && dx < r->x + r->w &&
            dy >= r->y && dy < r->y + r->h)
            return 1;
    }
    return 0;
}

int HUD_GetViewportRect(const TAK_Platform *plat, SDL_Rect *out) {
    if (!plat || !out || !g_rt) return 0;   /* no dialog, no layout */
    *out = TAK_Platform_CanvasRectToWindow(plat, g_viewport_dlg);
    return 1;
}

int HUD_GetViewportCanvasRect(SDL_Rect *out) {
    if (!out || !g_rt) return 0;
    *out = g_viewport_dlg;
    return 1;
}

int HUD_GetMinimapRect(const TAK_Platform *plat, SDL_Rect *out) {
    if (!plat || !out || !g_rt) return 0;
    if (g_minimap_dlg.w <= 0 || g_minimap_dlg.h <= 0) return 0;
    *out = TAK_Platform_CanvasRectToWindow(plat, g_minimap_dlg);
    return 1;
}

/* ── Per-frame: render the dialog + dynamic overlays ───────────────── */

/* Translate a dialog (640×480) rect to window pixels for SDL_Renderer
 * blits. The dialog itself renders into UI_Offscreen at 640×480, but
 * our text/health-bar overlays go directly to the renderer in window
 * pixels — they need to land in the same on-screen position the
 * widget art occupies. */
static SDL_Rect dialog_to_window(TAK_Platform *plat, SDL_Rect r) {
    return TAK_Platform_CanvasRectToWindow(plat, r);
}

static void fill_rect_canvas(SDL_Rect rc, SDL_Color c) {
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    SDL_FillRect(off, &rc, SDL_MapRGBA(off->format, c.r, c.g, c.b, c.a));
}

void HUD_Draw(TAK_Platform *plat, const GameWorld *world) {
    if (!plat || !plat->renderer) return;

    int sel_count = 0;
    const int *sel = Units_GetSelection(&sel_count);
    int have_sel = (sel && sel_count > 0);

    /* Forward mouse state to the runtime so hover frames update and
     * widget hit-testing works. The actual click dispatch happens in
     * HUD_HandleSidebarClick (called from ingame.c). Buttons in the
     * legacy dialog also light up on hover via this update call. */
    if (g_rt) {
        int mx = 0, my = 0;
        Uint32 b = (plat->has_focus ? SDL_GetMouseState(&mx, &my) : 0);
        int dx = 0, dy = 0;
        TAK_Platform_MapMouseToCanvas(plat, mx, my, &dx, &dy);
        char clicked[32] = {0};
        GUIRuntime_Update(g_rt, dx, dy,
                           (b & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0,
                           clicked, sizeof(clicked));
    }

    /* Frames that stay up no matter what is selected: the sidebar panel,
     * the bottom strip, the crystal ball and the mana readouts. Legacy's
     * idle pass hides every sidebar child EXCEPT CrystalBall/HelpText/
     * PositiveM/NegativeM and leaves the panel art itself alone
     * (legacy:151133-151166). The strip art is likewise never touched
     * by the null-unit refresh (legacy:152277-152296). */
    if (g_rt) {
        static const char *kAlwaysVisible[] = {
            "UnitMenu", "BottomBar", "BottomEnd", "UnitInfoGroup",
            "UnitInfo1", "UnitInfo2", "UnitImage", "CrystalBall",
            "HelpText", "PositiveM", "NegativeM", NULL
        };
        for (int i = 0; kAlwaysVisible[i]; i++)
            GUIRuntime_SetWidgetVisible(g_rt, kAlwaysVisible[i], 1);
    }

    /* Per-unit button visibility — hide widgets the selected unit
     * doesn't have caps for. Mirrors legacy
     * legacy:150627+ where each cap bit toggles the
     * corresponding dialog widget's visibility flag. */
    if (g_rt) {
        const UnitDef *seldef_cap = Units_GetSelectedDef();
        /* A unit inspected from another side offers no orders. */
        int own = hud_selection_is_own();
        uint32_t caps = (seldef_cap && own) ? seldef_cap->cap_flags : 0;
        int n_weapons = (seldef_cap && own) ? seldef_cap->num_weapons : 0;
        /* Active/Inactive are the onoffable pair (legacy:150430-150436):
         * for a gate they open and close it. */
        int onoff = own && ((seldef_cap && seldef_cap->onoffable) ||
                            Units_SelectedGateState() >= 0);
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
            { "Active",          onoff },
            { "Inactive",        onoff },
            { "Offensive",       (caps & UNIT_CAP_ATTACK)    != 0 },
            { "Defensive",       (caps & UNIT_CAP_ATTACK)    != 0 },
            { "Passive",         (caps & UNIT_CAP_ATTACK)    != 0 },
            { "PrimaryWeapon",   (caps & UNIT_CAP_W_SWITCH) && n_weapons >= 1 },
            { "SecondaryWeapon", (caps & UNIT_CAP_W_SWITCH) && n_weapons >= 2 },
            { "SpecialWeapon",   (caps & UNIT_CAP_W_SWITCH) && n_weapons >= 3 },
            { "BuildMenu",       (caps & UNIT_CAP_BUILDER)   != 0 },
        };
        for (size_t i = 0; i < sizeof(vis)/sizeof(vis[0]); i++) {
            GUIRuntime_SetWidgetVisible(g_rt, vis[i].name, vis[i].show);
        }

        /* Unit-info panels. With no unit the legacy refresh hides the
         * name label, both gauges with their backings, the rank pip and
         * the kill tally, leaving the strip art up
         * (legacy:152277-152296, legacy:152496-152506). The second panel
         * describes the selection's TARGET (legacy:152140-152143). We
         * do not populate a target panel yet, so it stays down and its
         * copies of those widgets never show empty gauges. */
        int sel_rank = have_sel ? Units_GetSelectedVeteranLevel() : 0;
        int show_xp = sel_rank > 0;
        int sel_own = hud_selection_is_own();
        /* The kill tally shows at one kill and up (legacy:152496-152506). */
        int sel_kills = have_sel ? Units_GetSelectedKills() : 0;
        if (g_panel1_n > 0) {
            for (int i = 0; i < g_panel1_n; i++) {
                int show = g_panel1[i].is_xp ? show_xp : have_sel;
                if (g_panel1[i].own_only && !sel_own) show = 0;
                if (g_panel1[i].is_kills && sel_kills <= 0) show = 0;
                GUIRuntime_SetWidgetVisibleAt(g_rt, g_panel1[i].index, show);
            }
            for (int i = 0; i < g_panel2_n; i++)
                GUIRuntime_SetWidgetVisibleAt(g_rt, g_panel2[i].index, 0);
        } else {
            /* No panel groups in this dialog, drive the set by name. */
            GUIRuntime_SetWidgetVisible(g_rt, "UnitText", have_sel);
            GUIRuntime_SetWidgetVisible(g_rt, "HealthBar", have_sel);
            GUIRuntime_SetWidgetVisible(g_rt, "ManaBar", have_sel && sel_own);
            GUIRuntime_SetWidgetVisible(g_rt, "KillCount",
                                        have_sel && sel_own && sel_kills > 0);
            GUIRuntime_SetWidgetVisible(g_rt, "Experience", show_xp && sel_own);
        }
        /* The shield shows frame rank minus one, and ranks past the
         * three authored frames keep the last (legacy:152439-152449). */
        if (show_xp)
            GUIRuntime_SetFrameOverride(g_rt, "Experience",
                                        (sel_rank < 3 ? sel_rank : 3) - 1);
    }

    /* Per-frame: keep the aggression + active-weapon buttons visually
     * synced with the selected unit's state. Mirror legacy
     * the legacy reference ~150870: active button → frame 1 ("pressed"),
     * inactive → frame 2 ("rest"). For weapon buttons the active slot
     * gets frame 1 too. We use SetFrameOverride so the selected
     * posture stays highlighted regardless of hover. */
    if (g_rt) {
        int aggro = Units_GetSelectedAggroMode();
        /* Aggression buttons (type GUI_WT_BUTTON 4, 3-frame) — pressed
         * frame is index 1 (legacy convention for BUTTONs). For
         * weapon-mode buttons we DON'T override the frame: pick_frame
         * already returns the rest icon (frame 2 for WINDOW) and
         * forcing frame 1 just shows the empty highlight overlay.
         * Active state for weapons is communicated via a halo border
         * drawn separately below. */
        /* The stance and gate pairs are stage buttons. The original
         * sets their frame outright every update: 1 for the state the
         * unit is in, 2 for the others (legacy:150868, legacy:150925).
         * Frame 0 is the disabled slot. Leaving the others with no
         * override showed that dark slot until the mouse arrived. */
        int gate_state = Units_SelectedGateState();
        struct { const char *name; int active; } toggles[] = {
            { "Offensive",       aggro == UNIT_AGGRO_OFFENSIVE },
            { "Defensive",       aggro == UNIT_AGGRO_DEFENSIVE },
            { "Passive",         aggro == UNIT_AGGRO_PASSIVE   },
            /* The current gate state is the lit one (legacy:150430). */
            { "Active",          gate_state == 1 },
            { "Inactive",        gate_state == 0 },
        };
        for (size_t i = 0; i < sizeof(toggles)/sizeof(toggles[0]); i++) {
            const GUIWidget *w = GUIRuntime_WidgetByName(g_rt, toggles[i].name);
            if (!w) continue;
            int frame = -1;
            if (w->num_frames >= 3)      frame = toggles[i].active ? 1 : 2;
            else if (w->num_frames == 2) frame = toggles[i].active ? 1 : 0;
            GUIRuntime_SetFrameOverride(g_rt, toggles[i].name, frame);
        }
        /* Weapon buttons keep their rest icon. The active slot is
         * marked with a halo once the dialog has rendered. */
        for (int i = 0; i < HUD_NUM_WEAPON_WIDGETS; i++)
            GUIRuntime_SetFrameOverride(g_rt, g_weapon_widgets[i], 0);

        /* Push live unit + economy strings into the authored label
         * widgets so they render in the dialog's bottom strip at the
         * exact authored positions, with the legacy fonts. This
         * matches what the legacy engine does. The .gui doesn't know
         * the unit, the engine refreshes label text every
         * frame from selection state (legacy:152219+). With no unit the
         * labels go empty so the authored placeholder never shows. */
        const char *unit_name   = have_sel ? Units_GetSelectedName()   : NULL;
        const char *unit_status = have_sel ? Units_GetSelectedStatus() : NULL;
        GUIRuntime_SetWidgetText(g_rt, "UnitText",
                                  unit_name   ? unit_name   : "");
        GUIRuntime_SetWidgetText(g_rt, "ActionText",
                                  unit_status ? unit_status : "");
        {
            /* The unit's own kills, for your units only and hidden at
             * zero (legacy:152496-152506). */
            char kbuf[8] = "";
            int nk = (have_sel && hud_selection_is_own())
                   ? Units_GetSelectedKills() : 0;
            if (nk > 0) snprintf(kbuf, sizeof(kbuf), "%d", nk);
            GUIRuntime_SetWidgetText(g_rt, "KillCount", kbuf);
        }

        /* HelpText is the desktop's own help line. A transport with
         * passengers reads "Carrying N", TRANSPORT_CARRYING_HELPTEXT
         * through "%s %d" (legacy:152081-152089). Otherwise it is the
         * mana readout, "Mana" over "cur/max" with cur clamped to max
         * (legacy:152100-152110). */
        {
            char help[64] = "";
            const UnitDef *sd = Units_GetSelectedDef();
            int cargo = (sd && (sd->cap_flags & UNIT_CAP_TRANSPORT))
                      ? Units_GetSelectedCargoCount() : 0;
            if (cargo > 0) {
                snprintf(help, sizeof(help), "%s %d", g_msg_carrying, cargo);
            } else if (world) {
                int32_t pool     = Economy_GetMana(&world->economy, 1);
                int32_t pool_max = Economy_GetMaxMana(&world->economy, 1);
                if (pool > pool_max) pool = pool_max;
                snprintf(help, sizeof(help), "%s\n%d/%d",
                         g_msg_mana, pool, pool_max);
            }
            GUIRuntime_SetWidgetText(g_rt, "HelpText", help);
        }

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

    /* Compose the dialog now that every widget's visibility, frame and
     * text is current for THIS frame. Rendering first would paint the
     * previous frame's state and leave placeholders up for a frame. */
    /* The gauges are progress bars: the strip clipped to the value
     * (legacy:152266, the ManaBar and HealthBar are AnimProgressBars).
     * Health from the unit, mana from the unit's own reserve. */
    {
        int n_sel = 0;
        const int *sel = Units_GetSelection(&n_sel);
        float hp_frac = 1.0f, mana_frac = 0.0f;
        if (n_sel > 0) {
            int hp = 0, hp_max = 1;
            Units_GetSelectedHealth(&hp, &hp_max);
            hp_frac = (hp_max > 0) ? (float)hp / (float)hp_max : 0.0f;
            float cur = 0.0f, max = 0.0f;
            if (Units_GetMana(sel[0], &cur, &max) && max > 0.0f)
                mana_frac = cur / max;
        }
        g_gauge_health = hp_frac;
        g_gauge_mana   = mana_frac;
        /* The crystal ball is the player's pool: its frame is the pool
         * fraction scaled to the sheet's last frame (legacy:152158). */
        if (g_rt && world) {
            int32_t pool = Economy_GetMana(&world->economy, 1);
            int32_t pool_max = Economy_GetMaxMana(&world->economy, 1);
            float pf = (pool_max > 0) ? (float)pool / (float)pool_max : 0.0f;
            if (pf < 0.0f) pf = 0.0f;
            if (pf > 1.0f) pf = 1.0f;
            g_gauge_pool = pf;
            const GUIWidget *ball = GUIRuntime_WidgetByName(g_rt, "CrystalBall");
            if (ball && ball->num_frames > 1) {
                int frame = (int)(pf * (float)(ball->num_frames - 1) + 0.5f);
                GUIRuntime_SetFrameOverride(g_rt, "CrystalBall", frame);
            }
        }
        if (g_rt && g_idx_health_bar >= 0)
            GUIRuntime_SetFillFractionAt(g_rt, g_idx_health_bar, hp_frac);
        if (g_rt && g_idx_mana_bar >= 0)
            GUIRuntime_SetFillFractionAt(g_rt, g_idx_mana_bar, mana_frac);
    }
    if (g_rt) GUIRuntime_Render(g_rt);

    /* Per-weapon button icons. Each magic-button widget gets the
     * SELECTED unit's per-weapon icon JPEG blitted on top of the
     * dialog's render at the widget's authored rect. Mirrors
     * `legacy:250088+` (weapon TDF reads `buttonimageup`/`down`/
     * `selected`/`disabled` and resolves to
     * `data/anims/weaponpic/<name>.jpg`). The active weapon uses
     * `icon_selected`, others use `icon_up`. We blit straight into
     * UI_Offscreen so the icon lands in the same compositing layer as
     * the dialog widgets. */
    if (g_rt) {
        /* A foreign unit's weapons are not shown. */
        const UnitDef *seldef = hud_selection_is_own() ? Units_GetSelectedDef()
                                                       : NULL;
        int          wslot    = Units_GetSelectedWeaponSlot();
        SDL_Surface *off      = UI_Offscreen();
        for (int i = 0; i < HUD_NUM_WEAPON_WIDGETS; i++) {
            const char *wname = g_weapon_widgets[i];
            int active = (wslot == i);
            if (!seldef || i >= seldef->num_weapons) continue;

            const UnitWeapon *wp = &seldef->weapons[i];
            const char *icon_name = active ? wp->icon_selected : wp->icon_up;
            if (!icon_name || !icon_name[0]) icon_name = wp->icon_up;
            if (!icon_name || !icon_name[0]) continue;

            SDL_Surface *icon = weapon_icon_load(icon_name);
            if (!icon || !off) continue;

            SDL_Rect dlg_r;
            if (!find_widget_rect(&dlg_r, wname)) continue;
            SDL_BlitScaled(icon, NULL, off, &dlg_r);

            if (active) {
                SDL_Rect win = dialog_to_window(plat, dlg_r);
                SDL_Rect halo = { win.x - 2, win.y - 2,
                                   win.w + 4, win.h + 4 };
                SDL_SetRenderDrawColor(plat->renderer, 240, 220, 90, 255);
                SDL_RenderDrawRect(plat->renderer, &halo);
            }
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

    /* Selected unit overlays (portrait JPG, bars). The name and status
     * strings ride the authored label widgets above, so nothing is
     * drawn twice. */
    if (have_sel) {
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

        /* The portrait frame is authored after the unit name and its art
         * reaches into the name's cell, so the name goes back on top. */
        if (g_rt && g_idx_unit_text >= 0) GUIRuntime_DrawTextAt(g_rt, g_idx_unit_text);
    }

    /* ── Build menu (visible when a builder is selected) ──────────
     *
     * Per-builder canbuild list comes from Units_GetBuildables. Each
     * buildable's portrait JPG goes into a button. Legacy sizes the
     * button from the BuildMenu widget's rect (64x48 default,
     * legacy:150127-150145), takes the column count from the play
     * area's width and the baseline from the bottom strip's top
     * (legacy:150187-150214), then fills left to right and stacks
     * upward from that baseline (legacy:150288-150310). */
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
            if (sd && (sd->cap_flags & UNIT_CAP_BUILDER) &&
                hud_selection_is_own()) {
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
                SDL_Rect cell;
                if (!find_widget_rect(&cell, "BuildMenu") ||
                    cell.w <= 0 || cell.h <= 0) {
                    cell.w = 64;   /* legacy:150140-150143 fallback */
                    cell.h = 48;
                }
                int cols = (cell.w > 0) ? g_viewport_dlg.w / cell.w : 1;
                if (cols < 1) cols = 1;
                const int baseline = g_viewport_dlg.h;
                SDL_Surface *off = UI_Offscreen();
                for (int b = 0; b < g_build_list_n; b++) {
                    SDL_Rect dlg_rect = {
                        (b % cols) * cell.w,
                        baseline - (b / cols + 1) * cell.h,
                        cell.w, cell.h
                    };
                    /* Dark recess in canvas. */
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
                    HUDBuildSlot *bs = &g_build_slots_live[g_build_slots_count++];
                    bs->dlg     = dlg_rect;
                    bs->rect    = dialog_to_window(plat, dlg_rect);
                    bs->def_idx = g_build_list[b];
                    bs->badge.x = bs->badge.y = 0;
                    bs->badge.w = bs->badge.h = 0;

                    /* Queued count, drawn inside the button. Legacy sets
                     * it as the build button's own label text
                     * (legacy:149903-149945) using the button font it
                     * creates the widget with (legacy:150251). */
                    int qn = Units_FactoryQueuedCountForDef(handle,
                                                            bs->def_idx);
                    if (qn > 0 && off && g_font_badge) {
                        char badge[12];
                        snprintf(badge, sizeof(badge), "%d", qn);
                        SDL_Rect br;
                        br.x = dlg_rect.x + 3;
                        br.y = dlg_rect.y + 2;
                        br.w = Font_MeasureString(g_font_badge, badge);
                        br.h = Font_LineHeight(g_font_badge);
                        if (br.w > dlg_rect.w - 4) br.w = dlg_rect.w - 4;
                        if (br.h > dlg_rect.h - 4) br.h = dlg_rect.h - 4;
                        Font_DrawString(g_font_badge, off, br.x, br.y, badge);
                        bs->badge = br;
                    }

                    if (g_cmd_mode == HUD_CMD_PLACE_BUILD &&
                        g_build_def_idx == g_build_list[b])
                    {
                        SDL_Rect halo = { bs->rect.x - 2, bs->rect.y - 2,
                                           bs->rect.w + 4, bs->rect.h + 4 };
                        SDL_SetRenderDrawColor(plat->renderer, 240, 220, 90, 255);
                        SDL_RenderDrawRect(plat->renderer, &halo);
                    }
                    if (g_build_slots_count >=
                        (int)(sizeof(g_build_slots_live)/sizeof(g_build_slots_live[0])))
                        break;
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
void HUD_SetCommandMode(int mode) {
    g_cmd_mode = mode;
    if (mode != HUD_CMD_PLACE_BUILD) g_build_def_idx = -1;
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

/* ── Introspection (layout + widget state, for tests) ──────────────── */

int HUD_WidgetHidden(const char *name) {
    return g_rt ? GUIRuntime_WidgetHidden(g_rt, name) : 0;
}

int HUD_WidgetFrame(const char *name) {
    return g_rt ? GUIRuntime_DrawnFrame(g_rt, name) : -1;
}

int HUD_WidgetText(const char *name, char *out, size_t cap) {
    if (!g_rt || !out || !cap) return 0;
    const GUIWidget *w = GUIRuntime_WidgetByName(g_rt, name);
    if (!w) return 0;
    snprintf(out, cap, "%s", w->display_text);
    return 1;
}

void HUD_GetGaugeFractions(float *out_health, float *out_mana, float *out_pool) {
    if (out_health) *out_health = g_gauge_health;
    if (out_mana)   *out_mana   = g_gauge_mana;
    if (out_pool)   *out_pool   = g_gauge_pool;
}

int HUD_GetUnitInfoRects(SDL_Rect *out_text, SDL_Rect *out_image) {
    if (!g_rect_have_unit_text || !g_rect_have_unit_image) return 0;
    if (out_text)  *out_text  = g_rect_unit_text;
    if (out_image) *out_image = g_rect_unit_image;
    return 1;
}

int HUD_BuildSlotCount(void) { return g_build_slots_count; }

int HUD_WidgetVisible(const char *name) {
    return g_rt && name && GUIRuntime_WidgetByName(g_rt, name) &&
           !GUIRuntime_WidgetHidden(g_rt, name);
}

int HUD_GetActionButtonRect(int mode, SDL_Rect *out) {
    for (int i = 0; i < g_action_slot_count; i++) {
        if (g_action_slots[i].mode != mode) continue;
        if (out) *out = g_action_slots[i].rect;
        return 1;
    }
    return 0;
}

int HUD_GetBuildSlotDialogRect(int slot, SDL_Rect *out, int *out_def_idx) {
    if (slot < 0 || slot >= g_build_slots_count || !out) return 0;
    *out = g_build_slots_live[slot].dlg;
    if (out_def_idx) *out_def_idx = g_build_slots_live[slot].def_idx;
    return 1;
}

int HUD_GetQueueBadgeDialogRect(int slot, SDL_Rect *out) {
    if (slot < 0 || slot >= g_build_slots_count || !out) return 0;
    if (g_build_slots_live[slot].badge.w <= 0) return 0;
    *out = g_build_slots_live[slot].badge;
    return 1;
}

int HUD_HandleSidebarRightClick(int win_x, int win_y, TAK_Platform *plat) {
    (void)plat;
    for (int i = 0; i < g_build_slots_count; i++) {
        const HUDBuildSlot *bs = &g_build_slots_live[i];
        if (win_x < bs->rect.x || win_x >= bs->rect.x + bs->rect.w) continue;
        if (win_y < bs->rect.y || win_y >= bs->rect.y + bs->rect.h) continue;
        int n_sel = 0;
        const int *sel = Units_GetSelection(&n_sel);
        if (n_sel > 0 && hud_selection_is_own() &&
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
            if (n_sel > 0 && bd && sd && hud_selection_is_own() &&
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
        HUD_TriggerCommand(s->mode);
        return 1;
    }
    return 0;
}

/* ACTIVATE/DEACTIVATE reach every selected gate (legacy:151449-151470). */
static void hud_set_selected_gates(int open) {
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    for (int i = 0; i < n; i++) {
        if (g_units_get_player(sel[i]) != 1) continue;   /* not yours */
        if (Units_GateState(sel[i]) >= 0) Units_SetGateOpen(sel[i], open);
    }
}

int HUD_TriggerCommand(int mode) {
    switch (mode) {
        case HUD_CMD_STOP:
            Units_CommandStopSelected();
            g_cmd_mode = HUD_CMD_NONE;
            return 1;
        case HUD_CMD_AGGRO_OFF:
            Units_CommandSetAggroSelected(UNIT_AGGRO_OFFENSIVE);
            return 1;
        case HUD_CMD_AGGRO_DEF:
            Units_CommandSetAggroSelected(UNIT_AGGRO_DEFENSIVE);
            return 1;
        case HUD_CMD_AGGRO_PAS:
            Units_CommandSetAggroSelected(UNIT_AGGRO_PASSIVE);
            return 1;
        case HUD_CMD_W_PRIMARY:
            Units_CommandSetWeaponSlotSelected(0);
            return 1;
        case HUD_CMD_W_SECONDARY:
            Units_CommandSetWeaponSlotSelected(1);
            return 1;
        case HUD_CMD_W_SET_SPEC:
            Units_CommandSetWeaponSlotSelected(2);
            return 1;
        case HUD_CMD_ACTIVATE:
            hud_set_selected_gates(1);
            return 1;
        case HUD_CMD_DEACTIVATE:
            hud_set_selected_gates(0);
            return 1;
        default:
            /* Cloak on/off: no per-unit state implemented yet. */
            return 0;
    }
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
