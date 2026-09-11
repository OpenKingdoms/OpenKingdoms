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
    /* GAF hotspot: the frame is drawn at rect - offset (legacy:45228 hands
     * the widget rect to the sprite blitter, which applies the hotspot). */
    int        frame_ox[GUI_MAX_FRAMES];
    int        frame_oy[GUI_MAX_FRAMES];
    int        frame_override;      /* -1 = no override, else explicit frame */
    int        hidden;              /* 1 = skip render + hit-test entirely  */
    float      fill;                /* progress bars: strip clipped to this */
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
    c->fill = 1.0f;
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
        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(slot->gaf, (uint32_t)entry_off,
                             fr->frame_index, &fh) == 0 && fh) {
            c->frame_ox[i] = fh->offset_x;
            c->frame_oy[i] = fh->offset_y;
        }
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
        /* Widgets the dialog authored invisible stay invisible until a
         * screen asks otherwise (legacy:312621). */
        rt->caches[i].hidden = dialog->children[i].visible ? 0 : 1;
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
        /* A stage button starts life on frame 2 (legacy:346764) and
         * its states run 0 disabled, 1 selected, 2 deselected. The
         * sheets follow: a dark slot at 0, the lit icon at 1, the
         * plain icon at 2. Rest is 2 and a hover lights it. A two
         * frame sheet keeps 0 as rest. */
        if (w->num_frames >= 3) return is_hovered ? 1 : 2;
        return is_hovered ? 1 : 0;
    case GUI_WT_CHECKBOX:
        /* 5-frame checkbox sheet: 3 = unchecked, 4 = checked
         * (legacy:139330 sets the anim to `state + 3`). Frames 0..2 are
         * the greyed-out pair used when the option is locked. */
        if (w->num_frames >= 5) return 3;
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

/* Draw a decoded frame at the destination rect. Art keeps the size the
 * GAF gives it: legacy hands the widget cell to the sprite draw
 * (Static_DrawFrame legacy:319725-319750, Button_DrawFrame
 * legacy:329670-329700, Slider_DrawFrame legacy:318493-318513) and the
 * frame paints at its own size from that origin. Scaling is left for
 * callers that ask for another destination size.
 * Nearest neighbour, alpha 0 skipped, identity when the sizes match. */
/* clip_w limits how many columns of the rect are painted: a progress
 * bar draws its strip up to its fraction and leaves the rest. */
static void blit_frame_to_rect(SDL_Surface *dst, SDL_Rect r,
                               const uint32_t *pixels, int src_w, int src_h,
                               int clip_w) {
    if (!dst || !pixels || src_w <= 0 || src_h <= 0) return;
    if (r.w <= 0 || r.h <= 0) return;
    if (clip_w < 0 || clip_w > r.w) clip_w = r.w;
    if (clip_w == 0) return;
    if (r.w == src_w && r.h == src_h && clip_w == r.w) {
        Blit_RGBA(dst, r.x, r.y, pixels, src_w, src_h);
        return;
    }
    if (!dst->format || dst->format->BytesPerPixel != 4) return;
    if (SDL_LockSurface(dst) != 0 || !dst->pixels) return;
    uint32_t amask = dst->format->Amask;
    for (int y = 0; y < r.h; y++) {
        int dy = r.y + y;
        if (dy < 0 || dy >= dst->h) continue;
        const uint32_t *src_row = pixels + (size_t)(y * src_h / r.h) * src_w;
        uint32_t *dst_row = (uint32_t *)((uint8_t *)dst->pixels + dy * dst->pitch);
        for (int x = 0; x < clip_w; x++) {
            int dx = r.x + x;
            if (dx < 0 || dx >= dst->w) continue;
            uint32_t p = src_row[x * src_w / r.w];
            if (amask ? ((p & amask) == 0) : (p == 0)) continue;
            dst_row[dx] = p;
        }
    }
    SDL_UnlockSurface(dst);
}

/* How big a widget's art lands depends on the widget. A static hands its
 * own rect to the sprite draw and stretches its second layer into it
 * (legacy:319735-319741), so the in-game sidebar's 64x41 portrait frame
 * lands in its 48x36 UnitImage cell and each gauge in its 97x2 cell.
 * Buttons, sliders and scroll nubs keep their art's own size. The
 * battle backgrounds rely on that, leaving a 49x62 well behind a Previous
 * button whose cell is 39x51, and the room's unit bar is 69 px of art in
 * a 91 px cell. A cell with no size takes the art's own size. */
static int widget_art_keeps_own_size(const GUIWidget *w) {
    if (!w || w->rect.w <= 0 || w->rect.h <= 0) return 1;
    switch (w->type) {
    case GUI_WT_BUTTON:
    case GUI_WT_SLIDER:
    case GUI_WT_SCROLLBTN:
        return 1;
    default:
        return 0;
    }
}

/* Where a widget's art lands: at the cell origin less the frame's
 * hotspot, at the size widget_art_keeps_own_size picks. */
static SDL_Rect widget_draw_rect(const GUIWidget *w, const WidgetCache *c,
                                 int fi, int wx, int wy) {
    SDL_Rect r;
    r.x = wx - c->frame_ox[fi];
    r.y = wy - c->frame_oy[fi];
    if (widget_art_keeps_own_size(w)) {
        r.w = c->frame_w[fi];
        r.h = c->frame_h[fi];
    } else {
        r.w = w->rect.w;
        r.h = w->rect.h;
    }
    return r;
}

static Font *pick_font(const GUIRuntime *rt, const char *font_name) {
    if (!font_name || !*font_name) return NULL;
    if (ci_contains(font_name, "100b"))     return rt->font_bold;
    if (ci_contains(font_name, "lombardic")) return rt->font_header;
    return rt->font_body;
}

int GUI_AlignedTextX(const GUIWidget *w, Font *f, const char *text, int wx) {
    if (!w || !f || !text || !text[0] || w->rect.w <= 0) return wx;
    int tw = Font_MeasureString(f, text);
    if (w->text_align == 1) return wx;
    if (w->text_align == 2) return wx + w->rect.w - tw;
    return wx + (w->rect.w - tw) / 2;
}

/* A label draws its string at the alignment its cell asks for. This is
 * the box that ink covers. Render and GUIRuntime_TextDrawRect share it so
 * the two cannot drift. Returns the font, or NULL when nothing draws. */
static Font *label_text_box(const GUIRuntime *rt, const GUIWidget *w,
                            int wx, int wy, SDL_Rect *out) {
    if (w->type != GUI_WT_LABEL || !w->display_text[0]) return NULL;
    Font *f = pick_font(rt, w->font);
    if (!f) return NULL;
    if (out) {
        int top = 0, bottom = 0;
        if (Font_InkExtent(f, w->display_text, &top, &bottom) != 0) top = bottom = 0;
        out->x = GUI_AlignedTextX(w, f, w->display_text, wx);
        out->y = wy + top;
        out->w = Font_MeasureString(f, w->display_text);
        out->h = bottom - top;
    }
    return f;
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
        int rx = rt->dialog->root.rect.x + ox - rt->root_cache.frame_ox[0];
        int ry = rt->dialog->root.rect.y + oy - rt->root_cache.frame_oy[0];
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
            /* widget_draw_rect picks the size by widget kind. The origin
             * is the cell minus the frame's hotspot: BattleBar is authored
             * at (-2,-21) so the track lands between its two nubs. */
            SDL_Rect dst = widget_draw_rect(w, c, fi, wx, wy);
            int clip_w = (c->fill >= 1.0f) ? -1
                       : (int)((float)dst.w * (c->fill > 0.0f ? c->fill : 0.0f) + 0.5f);
            blit_frame_to_rect(offscreen, dst,
                               c->frames[fi], c->frame_w[fi], c->frame_h[fi],
                               clip_w);
        }

        SDL_Rect tb;
        Font *tf = label_text_box(rt, w, wx, wy, &tb);
        if (tf) Font_DrawString(tf, offscreen, tb.x, wy, w->display_text);
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

int GUIRuntime_WidgetDrawRect(const GUIRuntime *rt, int index, SDL_Rect *out) {
    if (!rt || !out || index < 0 || index >= rt->dialog->num_children) return -1;
    const GUIWidget    *w = &rt->dialog->children[index];
    const WidgetCache  *c = &rt->caches[index];
    int fi = pick_frame(w, c, 0);
    if (!c->frames[fi] || c->frame_w[fi] <= 0 || c->frame_h[fi] <= 0) return -1;
    *out = widget_draw_rect(w, c, fi, w->rect.x + rt->offset_x,
                            w->rect.y + rt->offset_y);
    return 0;
}

void GUIRuntime_DrawTextAt(GUIRuntime *rt, int index) {
    if (!rt || index < 0 || index >= rt->dialog->num_children) return;
    if (rt->caches[index].hidden) return;
    const GUIWidget *w = &rt->dialog->children[index];
    int wx = w->rect.x + rt->offset_x;
    int wy = w->rect.y + rt->offset_y;
    SDL_Rect tb;
    Font *f = label_text_box(rt, w, wx, wy, &tb);
    if (f) Font_DrawString(f, UI_Offscreen(), tb.x, wy, w->display_text);
}

int GUIRuntime_TextDrawRect(const GUIRuntime *rt, int index, SDL_Rect *out) {
    if (!rt || !out || index < 0 || index >= rt->dialog->num_children) return -1;
    if (rt->caches[index].hidden) return -1;
    const GUIWidget *w = &rt->dialog->children[index];
    return label_text_box(rt, w, w->rect.x + rt->offset_x,
                          w->rect.y + rt->offset_y, out) ? 0 : -1;
}

/* Name-keyed setters touch EVERY widget carrying the name. The in-game
 * dialogs author duplicates on purpose (araingame.gui has two unit-info
 * panels, so UnitText/HealthBar/ManaBar/Experience each appear twice).
 * Stopping at the first match left the second copy showing its authored
 * placeholder. Index-keyed variants below drive one copy at a time. */
void GUIRuntime_SetFillFractionAt(GUIRuntime *rt, int index, float fraction) {
    if (!rt || index < 0 || index >= rt->dialog->num_children) return;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    rt->caches[index].fill = fraction;
}

void GUIRuntime_SetFrameOverride(GUIRuntime *rt, const char *name, int frame_index) {
    if (!rt || !name) return;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        if (tak_stricmp(rt->dialog->children[i].name, name) == 0)
            rt->caches[i].frame_override = frame_index;
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
        if (tak_stricmp(rt->dialog->children[i].name, name) == 0)
            rt->caches[i].hidden = visible ? 0 : 1;
    }
}

/* 1 only when the name exists and every copy of it is hidden. */
int GUIRuntime_DrawnFrame(const GUIRuntime *rt, const char *name) {
    if (!rt || !name) return -1;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        const GUIWidget *w = &rt->dialog->children[i];
        if (tak_stricmp(w->name, name) != 0) continue;
        return pick_frame(w, &rt->caches[i], rt->hovered == i);
    }
    return -1;
}

int GUIRuntime_WidgetHidden(const GUIRuntime *rt, const char *name) {
    if (!rt || !name) return 0;
    int found = 0;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        if (tak_stricmp(rt->dialog->children[i].name, name) != 0) continue;
        found = 1;
        if (!rt->caches[i].hidden) return 0;
    }
    return found;
}

static void widget_set_text(GUIWidget *w, const char *text) {
    char  *dst = w->display_text;
    size_t cap = sizeof(w->display_text);
    if (!text) text = "";
    size_t n = 0;
    while (n + 1 < cap && text[n]) { dst[n] = text[n]; n++; }
    dst[n] = '\0';
}

void GUIRuntime_SetWidgetText(GUIRuntime *rt, const char *name, const char *text) {
    if (!rt || !name) return;
    for (int i = 0; i < rt->dialog->num_children; i++) {
        if (tak_stricmp(rt->dialog->children[i].name, name) == 0)
            widget_set_text(&rt->dialog->children[i], text);
    }
}

/* ── Index-keyed access (duplicate widget names) ─────────────────────── */

const GUIWidget *GUIRuntime_WidgetAt(GUIRuntime *rt, int index) {
    if (!rt || index < 0 || index >= rt->dialog->num_children) return NULL;
    return &rt->dialog->children[index];
}

void GUIRuntime_SetWidgetVisibleAt(GUIRuntime *rt, int index, int visible) {
    if (!rt || index < 0 || index >= rt->dialog->num_children) return;
    rt->caches[index].hidden = visible ? 0 : 1;
}

void GUIRuntime_SetWidgetTextAt(GUIRuntime *rt, int index, const char *text) {
    if (!rt || index < 0 || index >= rt->dialog->num_children) return;
    widget_set_text(&rt->dialog->children[index], text);
}

int GUIRuntime_WidgetHiddenAt(const GUIRuntime *rt, int index) {
    if (!rt || index < 0 || index >= rt->dialog->num_children) return 0;
    return rt->caches[index].hidden ? 1 : 0;
}
