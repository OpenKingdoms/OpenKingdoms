/*
 * gui_render.c -- generic renderer for a parsed GUIDialog.
 *
 * For every child widget in a dialog we pre-decode all its frames into
 * RGBA once (on creation), load its font if specified, then on each
 * frame we:
 *   1. Pick the right frame index (hover/pressed/off/on as appropriate)
 *   2. Blit the frame at the widget's rect.x/.y
 *   3. Render any text the widget declares (labels / tooltip strip)
 *
 * Screens configure behavior via name-keyed overrides.
 */

#include "tak_gui_render.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_blit.h"
#include "tak_font.h"
#include "tak_ui.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <stdio.h>
#include <string.h>

/* Per-widget cached state: decoded frame pixels + font reference. */
typedef struct {
    /* Owned GAF file that backs this widget's frames. May be shared
     * (one GAF per dialog in practice) — we track a pointer to the
     * runtime's shared GAF cache for lifecycle, not owned here. */
    uint32_t  *frames[GUI_MAX_FRAMES];
    int        frame_w[GUI_MAX_FRAMES];
    int        frame_h[GUI_MAX_FRAMES];
    int        frame_override;      /* -1 = no override, else explicit frame */
    int        hidden;              /* 1 = skip render + hit-test entirely  */
} WidgetCache;

/* One GAF loaded on demand, keyed by its path. A dialog typically uses
 * 1–3 GAFs across its widgets (mainscreen.gaf, ScrollBars.gaf, …) so a
 * small open-address cache is plenty. */
#define GUI_MAX_GAFS 8
typedef struct {
    char      name[64];             /* "mainscreen.gaf" */
    GAFFile  *gaf;
    uint32_t  rgba_table[256];
} GAFSlot;

struct GUIRuntime {
    GUIDialog   *dialog;
    WidgetCache *caches;            /* parallel to dialog->children */
    WidgetCache  root_cache;

    GAFSlot      gafs[GUI_MAX_GAFS];
    int          num_gafs;

    Font        *font_body;         /* times new roman 100  */
    Font        *font_bold;         /* times new roman 100b */
    Font        *font_header;       /* lombardic (cd)       */

    int          hovered;           /* child index, or -1 for root */
    int          prev_mouse_down;

    int          offset_x;          /* added to every widget rect at draw/hit-test */
    int          offset_y;
};

/* ── GAF slot management ─────────────────────────────────────────────── */

static GAFSlot *runtime_get_gaf(GUIRuntime *rt, const char *gaf_name) {
    if (!gaf_name || !*gaf_name) return NULL;
    for (int i = 0; i < rt->num_gafs; i++) {
        if (tak_stricmp(rt->gafs[i].name, gaf_name) == 0) return &rt->gafs[i];
    }
    if (rt->num_gafs >= GUI_MAX_GAFS) {
        fprintf(stderr, "GUIRuntime: GAF slot overflow (>%d); skipping %s\n",
                GUI_MAX_GAFS, gaf_name);
        return NULL;
    }

    /* Load from data/anims/<name>.gaf (pcx has matching stem). The .gui
     * sometimes stores mixed-case names — open as-is first, then try
     * common case conversions. */
    GAFSlot *s = &rt->gafs[rt->num_gafs++];
    strncpy(s->name, gaf_name, sizeof(s->name) - 1);

    char base[128];
    strncpy(base, gaf_name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    /* Drop ".gaf" suffix (case-insensitive) to get the stem. */
    size_t blen = strlen(base);
    if (blen > 4 && tak_stricmp(base + blen - 4, ".gaf") == 0) base[blen - 4] = '\0';

    char gaf_path[256], pcx_path[256];
    snprintf(gaf_path, sizeof(gaf_path), "data/anims/%s.gaf", base);
    snprintf(pcx_path, sizeof(pcx_path), "data/anims/%s.pcx", base);

    if (UI_LoadGAFWithPalette(gaf_path, pcx_path, &s->gaf, s->rgba_table) != 0) {
        fprintf(stderr, "GUIRuntime: failed to load %s\n", gaf_path);
        s->gaf = NULL;
    }
    return s;
}

/* ── Frame decoding ──────────────────────────────────────────────────── */

static void widget_decode_frames(GUIRuntime *rt, const GUIWidget *w, WidgetCache *c) {
    memset(c, 0, sizeof(*c));
    c->frame_override = -1;
    for (int i = 0; i < w->num_frames; i++) {
        const GUIFrameRef *fr = &w->frames[i];
        if (!fr->gaf[0] || !fr->sequence[0]) continue;

        GAFSlot *slot = runtime_get_gaf(rt, fr->gaf);
        if (!slot || !slot->gaf) continue;

        int entry_off = GAF_FindSequence(slot->gaf, fr->sequence);
        if (entry_off < 0) continue;

        int w_px = 0, h_px = 0;
        c->frames[i] = UI_DecodeFrame(slot->gaf, entry_off, fr->frame_index,
                                       slot->rgba_table, &w_px, &h_px);
        c->frame_w[i] = w_px;
        c->frame_h[i] = h_px;
        /* Diagnostic for the magic-weapon button frames so we can see
         * if their pixel data is truly uniform or varies. Count
         * distinct RGBA values + show the top 4 frequencies. */
        if (c->frames[i] && fr->sequence[0] == 'W' && w_px == 32 && h_px == 32 &&
            fr->frame_index == 0)
        {
            int total = w_px * h_px;
            uint32_t vals[16] = {0};
            int      cnts[16] = {0};
            int      n_distinct = 0;
            for (int p = 0; p < total; p++) {
                uint32_t v = c->frames[i][p];
                int found = 0;
                for (int k = 0; k < n_distinct; k++) {
                    if (vals[k] == v) { cnts[k]++; found = 1; break; }
                }
                if (!found && n_distinct < 16) {
                    vals[n_distinct] = v;
                    cnts[n_distinct] = 1;
                    n_distinct++;
                }
            }
            fprintf(stderr, "GUI-distinct: %s frame=0  %d distinct values:\n",
                    w->name, n_distinct);
            for (int k = 0; k < n_distinct; k++) {
                fprintf(stderr, "    0x%08x x %d\n", vals[k], cnts[k]);
            }
        }
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

GUIRuntime *GUIRuntime_Create(GUIDialog *dialog) {
    if (!dialog) return NULL;
    GUIRuntime *rt = (GUIRuntime *)tak_malloc(sizeof(GUIRuntime));
    if (!rt) return NULL;
    memset(rt, 0, sizeof(*rt));
    rt->dialog = dialog;
    rt->hovered = -1;

    /* Decode the root widget's background frame(s). */
    widget_decode_frames(rt, &dialog->root, &rt->root_cache);

    /* Decode children. */
    rt->caches = (WidgetCache *)tak_malloc(
        (size_t)dialog->num_children * sizeof(WidgetCache));
    if (!rt->caches) { tak_free(rt); return NULL; }
    for (int i = 0; i < dialog->num_children; i++) {
        widget_decode_frames(rt, &dialog->children[i], &rt->caches[i]);
    }

    /* Eager font loads — dialogs typically use 2–3 fonts and we want them
     * ready. Failure is silent: text just won't render for that font. */
    rt->font_body   = Font_Load("data/fonts/b_times new roman (100)",  UI_RGBAFormat());
    rt->font_bold   = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    rt->font_header = Font_Load("data/fonts/lombardic (cd)",           UI_RGBAFormat());

    return rt;
}

void GUIRuntime_Destroy(GUIRuntime *rt) {
    if (!rt) return;
    for (int f = 0; f < GUI_MAX_FRAMES; f++) {
        if (rt->root_cache.frames[f]) tak_free(rt->root_cache.frames[f]);
    }
    if (rt->caches) {
        for (int i = 0; i < rt->dialog->num_children; i++) {
            for (int f = 0; f < GUI_MAX_FRAMES; f++) {
                if (rt->caches[i].frames[f]) tak_free(rt->caches[i].frames[f]);
            }
        }
        tak_free(rt->caches);
    }
    for (int i = 0; i < rt->num_gafs; i++) {
        if (rt->gafs[i].gaf) GAF_Close(rt->gafs[i].gaf);
    }
    if (rt->font_body)   Font_Free(rt->font_body);
    if (rt->font_bold)   Font_Free(rt->font_bold);
    if (rt->font_header) Font_Free(rt->font_header);
    tak_free(rt);
}

/* ── Update / Render ─────────────────────────────────────────────────── */

static int widget_is_interactive(const GUIWidget *w) {
    switch (w->type) {
    case GUI_WT_BUTTON:
    case GUI_WT_STAGEBUTTON:
    case GUI_WT_CHECKBOX:
    case GUI_WT_MULTISTATE:
    case GUI_WT_SCROLLBTN:
    case GUI_WT_SLIDER:       /* MaxUnits etc. — click on track sets value */
    case GUI_WT_LISTBOX:      /* Map list — click selects */
        return 1;
    default:
        return 0;
    }
}

int GUIRuntime_UpdateEx(GUIRuntime *rt, int mx, int my, int mouse_down,
                        char *out_clicked, size_t out_clicked_cap,
                        int *out_index) {
    if (out_clicked && out_clicked_cap) out_clicked[0] = '\0';
    if (out_index) *out_index = -1;
    if (!rt) return 0;

    rt->hovered = -1;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        const GUIWidget *w = &rt->dialog->children[i];
        if (!widget_is_interactive(w)) continue;
        if (rt->caches[i].hidden) continue;
        SDL_Rect r = w->rect;
        r.x += rt->offset_x;
        r.y += rt->offset_y;
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &r)) {
            rt->hovered = i;
            break;
        }
    }

    int clicked = 0;
    if (!mouse_down && rt->prev_mouse_down && rt->hovered >= 0) {
        const GUIWidget *w = &rt->dialog->children[rt->hovered];
        if (out_clicked && out_clicked_cap) {
            strncpy(out_clicked, w->name, out_clicked_cap - 1);
            out_clicked[out_clicked_cap - 1] = '\0';
        }
        if (out_index) *out_index = rt->hovered;
        clicked = 1;
    }
    rt->prev_mouse_down = mouse_down;
    return clicked;
}

int GUIRuntime_Update(GUIRuntime *rt, int mx, int my, int mouse_down,
                      char *out_clicked, size_t out_clicked_cap) {
    return GUIRuntime_UpdateEx(rt, mx, my, mouse_down,
                                out_clicked, out_clicked_cap, NULL);
}

/* Pick which frame to draw for a widget.
 *
 * Legacy frame convention varies by widget TYPE:
 *   - GUI_WT_BUTTON (type 4) / GUI_WT_STAGEBUTTON (type 17) with 3 frames:
 *     frame 2 = REST (icon at idle), frames 0/1 = pressed/highlight overlays.
 *     Per the legacy reference ~139312 (InGameUI_UpdateButtonStates).
 *   - GUI_WT_WINDOW (type 2) hosting an icon sprite (PrimaryWeapon /
 *     SecondaryWeapon / SpecialWeapon in araingame.gui — 3 frames):
 *     frame 0 = rest icon, frame 1 = active/pressed, frame 2 = disabled.
 *     Different convention because the panel-widget origin sees the
 *     state from the OTHER side — it's the legacy "state index" not
 *     the button's animation index.
 *   - 2-frame menu controls: frame 0 = normal, frame 1 = hover. */
static int pick_frame(const GUIWidget *w, const WidgetCache *c, int is_hovered) {
    if (c->frame_override >= 0 && c->frame_override < w->num_frames)
        return c->frame_override;
    if (w->num_frames <= 1) return 0;
    switch (w->type) {
    case GUI_WT_BUTTON:
        /* BUTTON 3-frame (e.g. MOVE/ATTACK action row): rest=2, hover=1.
         * 2-frame: rest=0, hover=1. */
        if (w->num_frames >= 3) return is_hovered ? 1 : 2;
        return is_hovered ? 1 : 0;
    case GUI_WT_STAGEBUTTON:
        /* STAGEBUTTON 3-frame (e.g. PrimaryWeapon/SecondaryWeapon/
         * SpecialWeapon): legacy convention is rest=0, frame 1 is the
         * pressed/active overlay, frame 2 is "deactivated" overlay.
         * Different from BUTTON because the engine treats StageBtn as
         * a panel-hosted icon rather than an animated button. */
        return is_hovered ? 1 : 0;
    case GUI_WT_CHECKBOX:
        return is_hovered ? 1 : 0;
    case GUI_WT_WINDOW:
        /* Panel-hosted icon. Empirically (igcommonbuttons.gaf) frame 2
         * is the visible rest icon (matches BUTTON convention) — frame 0
         * is mostly transparent and frame 1 is a highlight overlay.
         * Default to frame 2 when 3 frames exist; 2-frame WINDOWs
         * just show their first art. */
        if (w->num_frames >= 3) return is_hovered ? 1 : 2;
        return 0;
    default:
        if (w->num_frames >= 3) return 2;
        return 0;
    }
}

/* Case-insensitive substring match — simpler than pulling a new helper
 * into util.c for this single use. */
static int ci_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b || !p[i]) break;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static Font *pick_font(const GUIRuntime *rt, const char *font_name) {
    if (!font_name || !*font_name) return NULL;
    if (ci_contains(font_name, "100b"))     return rt->font_bold;
    if (ci_contains(font_name, "lombardic")) return rt->font_header;
    return rt->font_body;
}

void GUIRuntime_Render(GUIRuntime *rt) {
    if (!rt) return;
    SDL_Surface *offscreen = UI_Offscreen();

    int ox = rt->offset_x;
    int oy = rt->offset_y;

    /* Root background. Full-screen dialogs (e.g. mainmenu at 0,0,640,480)
     * draw from the origin; smaller panels (e.g. options at 77,68,486,344)
     * draw at their declared top-left so they appear centered. */
    if (rt->root_cache.frames[0]) {
        int rx = rt->dialog->root.rect.x + ox;
        int ry = rt->dialog->root.rect.y + oy;
        Blit_RGBA(offscreen, rx, ry,
                  rt->root_cache.frames[0],
                  rt->root_cache.frame_w[0],
                  rt->root_cache.frame_h[0]);
    }

    /* Children */
    for (int i = 0; i < rt->dialog->num_children; i++) {
        const GUIWidget *w = &rt->dialog->children[i];
        const WidgetCache *c = &rt->caches[i];
        if (c->hidden) continue;
        int hovered = (rt->hovered == i);
        int wx = w->rect.x + ox;
        int wy = w->rect.y + oy;

        int fi = pick_frame(w, c, hovered);
        if (c->frames[fi]) {
            Blit_RGBA(offscreen, wx, wy,
                      c->frames[fi], c->frame_w[fi], c->frame_h[fi]);
        }

        if (w->type == GUI_WT_LABEL) {
            const char *text = w->display_text[0] ? w->display_text : "";
            if (text[0]) {
                Font *f = pick_font(rt, w->font);
                if (f) Font_DrawString(f, offscreen, wx, wy, text);
            }
        }
    }
}

void GUIRuntime_SetOffset(GUIRuntime *rt, int dx, int dy) {
    if (!rt) return;
    rt->offset_x = dx;
    rt->offset_y = dy;
}

/* ── Accessors ───────────────────────────────────────────────────────── */

int GUIRuntime_HoveredIndex(const GUIRuntime *rt) {
    return rt ? rt->hovered : -1;
}

const GUIWidget *GUIRuntime_HoveredWidget(const GUIRuntime *rt) {
    if (!rt || rt->hovered < 0) return NULL;
    return &rt->dialog->children[rt->hovered];
}

const GUIWidget *GUIRuntime_WidgetByName(GUIRuntime *rt, const char *name) {
    return rt ? GUIDialog_FindByName(rt->dialog, name) : NULL;
}

int GUIRuntime_NumWidgets(const GUIRuntime *rt) {
    return rt ? rt->dialog->num_children : 0;
}

void GUIRuntime_SetFrameOverride(GUIRuntime *rt, const char *name, int frame_index) {
    if (!rt || !name) return;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        if (tak_stricmp(rt->dialog->children[i].name, name) == 0) {
            rt->caches[i].frame_override = frame_index;
            return;
        }
    }
}

void GUIRuntime_HideRoot(GUIRuntime *rt) {
    if (!rt) return;
    for (int f = 0; f < GUI_MAX_FRAMES; f++) {
        if (rt->root_cache.frames[f]) {
            tak_free(rt->root_cache.frames[f]);
            rt->root_cache.frames[f] = NULL;
        }
        rt->root_cache.frame_w[f] = 0;
        rt->root_cache.frame_h[f] = 0;
    }
}

void GUIRuntime_SetWidgetVisible(GUIRuntime *rt, const char *name, int visible) {
    if (!rt || !name) return;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        if (tak_stricmp(rt->dialog->children[i].name, name) == 0) {
            rt->caches[i].hidden = visible ? 0 : 1;
            return;
        }
    }
}

int GUIRuntime_WidgetHidden(const GUIRuntime *rt, const char *name) {
    if (!rt || !name) return 0;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        if (tak_stricmp(rt->dialog->children[i].name, name) == 0)
            return rt->caches[i].hidden ? 1 : 0;
    }
    return 0;
}

void GUIRuntime_SetWidgetText(GUIRuntime *rt, const char *name, const char *text) {
    if (!rt || !name) return;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        if (tak_stricmp(rt->dialog->children[i].name, name) == 0) {
            char *dst = rt->dialog->children[i].display_text;
            size_t cap = sizeof(rt->dialog->children[i].display_text);
            if (!text) text = "";
            size_t n = 0;
            while (n + 1 < cap && text[n]) { dst[n] = text[n]; n++; }
            dst[n] = '\0';
            return;
        }
    }
}
