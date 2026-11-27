/*
 * keybinds_screen.c -- the key binds dialog (D-057).
 *
 * See tak_keybinds_screen.h. The panel, its seven ruled rows, Ok, Cancel
 * and the help strip are data/guis/diplomacy.gui as shipped, with the
 * diplomacy widgets hidden. The scrollbar, the scheme dots, the box and
 * the shield are frames of the shipped sheets placed by hand, and the
 * rows are drawn here, as every list in the engine is.
 */

#include "tak_keybinds_screen.h"
#include "tak_keybinds.h"
#include "tak_keybinds_view.h"
#include "tak_message_box.h"
#include "tak_settings.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_font.h"
#include "tak_blit.h"
#include "tak_hud.h"
#include "tak_ui.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KS_GUI "data/guis/diplomacy.gui"

enum { KS_ROWS = 7 };
enum { Q_NONE = 0, Q_CONFLICT, Q_SCHEME, Q_RESET };

/* Columns, off the row's left edge. The first 17 are the row's own
 * small cell, where the diamond goes. */
enum {
    KS_MARK_W  = 17,
    KS_NAME_X  = 21,
    KS_NAME_W  = 160,
    KS_SLOT_X  = 186,
    KS_SLOT_W  = 82,
    KS_SLOT_GAP = 4,
    KS_BAR_X   = 360
};

typedef struct KsSprite {
    uint32_t *px;
    int w, h;
} KsSprite;

static struct {
    int         open;
    int         kept;
    int         have_snap;   /* the keys it found, to put back */
    GUIDialog   dialog;
    GUIRuntime *rt;
    int         off_x, off_y;
    Font       *font_row;
    Font       *font_bold;
    Font       *font_head;

    SDL_Rect    row[KS_ROWS];    /* the art's rows, dialog coordinates */
    int         row_count;
    int         group_y, head_y; /* the two header lines over the pane */
    SDL_Rect    help;

    /* [0] at rest or off, [1] pressed or on. */
    KsSprite    inc[2], dec[2], radio[2], box[2], shield[2];
    KsSprite    thumb, bar;

    KeybindsState snap;
    int         group;
    int         scroll;
    int         cur_row, cur_slot;
    int         cursor_on;
    int         last_mx, last_my;

    int         armed;
    KeyAction   armed_action;
    int         armed_slot;

    int         question;
    KeyAction   q_action;
    int         q_slot;
    KeyChord    q_chord;
    int         q_custom_was;    /* the player's keys were on before the ask */

    uint8_t     prev_keys[SDL_NUM_SCANCODES];
    int         prev_left, prev_right;
    int         wait_up;         /* a press that has been spent */
    int         dragging, grab_dy;
    char        note[64];

} ks;

/* ── art ─────────────────────────────────────────────────────────── */

static void ks_sprite(GAFFile *gaf, const uint32_t *table, const char *seq,
                      int frame, KsSprite *out) {
    int entry = GAF_FindSequence(gaf, seq);
    if (entry < 0) return;
    out->px = UI_DecodeFrame(gaf, entry, frame, table, &out->w, &out->h);
    if (!out->px) { out->w = 0; out->h = 0; }
}

static void ks_free_sprite(KsSprite *s) {
    if (s->px) tak_free(s->px);
    memset(s, 0, sizeof(*s));
}

/* A sheet runs greyed, pushed in, at rest. A box is 3 off and 4 on, as
 * Options sets it. A dot is dark on 0 and lit on 1. */
static void ks_load_art(void) {
    GAFFile *gaf = NULL;
    uint32_t table[256];
    if (UI_LoadGAFWithPalette("data/anims/scrollbars.gaf",
                              "data/anims/scrollbars.pcx", &gaf, table) == 0 && gaf) {
        ks_sprite(gaf, table, "ScrollInc", 2, &ks.inc[0]);
        ks_sprite(gaf, table, "ScrollInc", 1, &ks.inc[1]);
        ks_sprite(gaf, table, "ScrollDec", 2, &ks.dec[0]);
        ks_sprite(gaf, table, "ScrollDec", 1, &ks.dec[1]);
        ks_sprite(gaf, table, "ScrollThumb", 2, &ks.thumb);
        ks_sprite(gaf, table, "LoadDBar", 2, &ks.bar);
        ks_sprite(gaf, table, "RadioButton", 0, &ks.radio[0]);
        ks_sprite(gaf, table, "RadioButton", 1, &ks.radio[1]);
        ks_sprite(gaf, table, "CheckBox", 3, &ks.box[0]);
        ks_sprite(gaf, table, "CheckBox", 4, &ks.box[1]);
        GAF_Close(gaf);
    }
    gaf = NULL;
    if (UI_LoadGAFWithPalette("data/anims/optionsdialogue.gaf",
                              "data/anims/optionsdialogue.pcx", &gaf, table) == 0 && gaf) {
        ks_sprite(gaf, table, "DeleteButton", 2, &ks.shield[0]);
        ks_sprite(gaf, table, "DeleteButton", 1, &ks.shield[1]);
        GAF_Close(gaf);
    }
}

static void ks_free_art(void) {
    for (int i = 0; i < 2; i++) {
        ks_free_sprite(&ks.inc[i]);
        ks_free_sprite(&ks.dec[i]);
        ks_free_sprite(&ks.radio[i]);
        ks_free_sprite(&ks.box[i]);
        ks_free_sprite(&ks.shield[i]);
    }
    ks_free_sprite(&ks.thumb);
    ks_free_sprite(&ks.bar);
}

static void ks_blit(const KsSprite *s, int x, int y) {
    SDL_Surface *off = UI_Offscreen();
    if (off && s->px) Blit_RGBA(off, x, y, s->px, s->w, s->h);
}

/* The shield was cut from under the Options pane, and its first rows
 * carry that pane's shadow. They are left off. */
enum { KS_SHIELD_SKIP = 7 };

static void ks_blit_shield(const KsSprite *s, int x, int y) {
    SDL_Surface *off = UI_Offscreen();
    if (!off || !s->px || s->h <= KS_SHIELD_SKIP) return;
    Blit_RGBA(off, x, y + KS_SHIELD_SKIP, s->px + s->w * KS_SHIELD_SKIP,
              s->w, s->h - KS_SHIELD_SKIP);
}

static void ks_fill(int x, int y, int w, int h, int r, int g, int b) {
    SDL_Surface *off = UI_Offscreen();
    if (!off || w <= 0 || h <= 0) return;
    SDL_Rect rc = { x, y, w, h };
    SDL_FillRect(off, &rc, SDL_MapRGBA(off->format, (Uint8)r, (Uint8)g, (Uint8)b, 255));
}

static void ks_outline(SDL_Rect r, int cr, int cg, int cb) {
    ks_fill(r.x, r.y, r.w, 1, cr, cg, cb);
    ks_fill(r.x, r.y + r.h - 1, r.w, 1, cr, cg, cb);
    ks_fill(r.x, r.y, 1, r.h, cr, cg, cb);
    ks_fill(r.x + r.w - 1, r.y, 1, r.h, cr, cg, cb);
}

static void ks_text(Font *f, int x, int y, const char *s) {
    SDL_Surface *off = UI_Offscreen();
    if (off && f && s && s[0]) Font_DrawString(f, off, x, y, s);
}

static int ks_measure(void *font, const char *text) {
    return font ? Font_MeasureString((Font *)font, text) : 0;
}

/* ── the dialog's parts ──────────────────────────────────────────── */

static int ks_by_y(const void *a, const void *b) {
    return ((const SDL_Rect *)a)->y - ((const SDL_Rect *)b)->y;
}

/* The rows are the file's seven group boxes and the header lines its
 * labels above them. Everything but Ok and Cancel is then hidden. */
static void ks_read_dialog(void) {
    SDL_Rect rows[16];
    int n = 0;
    for (int i = 0; i < ks.dialog.num_children; i++) {
        const GUIWidget *w = &ks.dialog.children[i];
        if (w->type == GUI_WT_PANEL && w->rect.w > 200 && n < 16) rows[n++] = w->rect;
    }
    qsort(rows, (size_t)n, sizeof(rows[0]), ks_by_y);
    ks.row_count = n < KS_ROWS ? n : KS_ROWS;
    for (int i = 0; i < ks.row_count; i++) ks.row[i] = rows[i];

    int top = ks.row_count ? ks.row[0].y : 0;
    ks.head_y = top - 31;
    ks.group_y = top - 51;
    int lo = -1, hi = -1;
    for (int i = 0; i < ks.dialog.num_children; i++) {
        const GUIWidget *w = &ks.dialog.children[i];
        if (w->type != GUI_WT_LABEL || w->rect.y >= top || w->rect.h > 24) continue;
        if (lo < 0 || w->rect.y < lo) lo = w->rect.y;
        if (hi < 0 || w->rect.y > hi) hi = w->rect.y;
    }
    if (lo >= 0 && hi > lo) { ks.group_y = lo; ks.head_y = hi; }

    const GUIWidget *help = GUIDialog_FindByName(&ks.dialog, "HelpText");
    if (help) ks.help = help->rect;
    else { SDL_Rect r = { ks.dialog.root.rect.x + 125, ks.dialog.root.rect.y + 310, 236, 31 }; ks.help = r; }

    for (int i = 0; i < ks.dialog.num_children; i++) {
        const char *name = ks.dialog.children[i].name;
        if (tak_stricmp(name, "Ok") == 0 || tak_stricmp(name, "Cancel") == 0) continue;
        GUIRuntime_SetWidgetVisibleAt(ks.rt, i, 0);
    }
}

static SDL_Rect ks_row_rect(int i) {
    SDL_Rect r = ks.row[i];
    r.x += ks.off_x;
    r.y += ks.off_y;
    return r;
}

static SDL_Rect ks_slot_rect(int i, int slot) {
    SDL_Rect r = ks_row_rect(i);
    r.x += KS_SLOT_X + slot * (KS_SLOT_W + KS_SLOT_GAP);
    r.w = KS_SLOT_W;
    return r;
}

static SDL_Rect ks_mark_rect(int i) {
    SDL_Rect r = ks_row_rect(i);
    r.w = KS_MARK_W;
    return r;
}

static int ks_in(SDL_Rect r, int x, int y) {
    SDL_Point p = { x, y };
    return SDL_PointInRect(&p, &r);
}

/* ── the list ────────────────────────────────────────────────────── */

static int ks_actions(KeyAction *out) {
    return KeyView_GroupActions(ks.group, out, KEYVIEW_MAX_ROWS);
}

static int ks_count(void) { return KeyView_GroupActions(ks.group, NULL, 0); }

static void ks_clamp(void) {
    int n = ks_count();
    ks.scroll = KeyView_ClampScroll(ks.scroll, n, ks.row_count);
    if (ks.cur_row >= n) ks.cur_row = n - 1;
    if (ks.cur_row < 0) ks.cur_row = 0;
}

static void ks_show_cursor(void) {
    if (ks.cur_row < ks.scroll) ks.scroll = ks.cur_row;
    if (ks.cur_row >= ks.scroll + ks.row_count) ks.scroll = ks.cur_row - ks.row_count + 1;
    ks_clamp();
}

static void ks_set_group(int g) {
    if (g < 0) g = KEYVIEW_GROUPS - 1;
    if (g >= KEYVIEW_GROUPS) g = 0;
    ks.group = g;
    ks.scroll = 0;
    ks.cur_row = 0;
}

/* The scrollbar: a nub at each end, the bar art cut to fit between. */
typedef struct KsBar {
    int shown;
    SDL_Rect inc, dec, track, thumb;
} KsBar;

static KsBar ks_bar(void) {
    KsBar b;
    memset(&b, 0, sizeof(b));
    int n = ks_count();
    if (ks.row_count < 1 || n <= ks.row_count) return b;
    SDL_Rect first = ks_row_rect(0), last = ks_row_rect(ks.row_count - 1);
    int x = first.x + KS_BAR_X;
    int top = first.y - 1, bottom = last.y + last.h + 1;
    int nub_w = ks.inc[0].w ? ks.inc[0].w : 24;
    int inc_h = ks.inc[0].h ? ks.inc[0].h : 21;
    int dec_h = ks.dec[0].h ? ks.dec[0].h : 21;
    int th = ks.thumb.h ? ks.thumb.h : 22, tw = ks.thumb.w ? ks.thumb.w : 22;
    b.shown = 1;
    b.inc = (SDL_Rect){ x, top, nub_w, inc_h };
    b.dec = (SDL_Rect){ x, bottom - dec_h, nub_w, dec_h };
    b.track = (SDL_Rect){ x + 1, top + inc_h, nub_w - 2, bottom - dec_h - top - inc_h };
    int travel = b.track.h - th;
    b.thumb = (SDL_Rect){ x + (nub_w - tw) / 2,
                          b.track.y + KeyView_ThumbOffset(ks.scroll, n, ks.row_count, travel),
                          tw, th };
    return b;
}

/* ── the lines above and below the pane ──────────────────────────── */

/* The groups are tabs in a dark strip over the pane. The game has no
 * dark font small enough to set seven names on the parchment. */
enum { KS_STRIP_H = 18 };

static SDL_Rect ks_strip_rect(void) {
    SDL_Rect first = ks_row_rect(0);
    SDL_Rect r = { first.x, ks.group_y + ks.off_y, first.w, KS_STRIP_H };
    return r;
}

static void ks_group_rects(SDL_Rect out[KEYVIEW_GROUPS]) {
    SDL_Rect strip = ks_strip_rect();
    int total = 0, w[KEYVIEW_GROUPS];
    for (int g = 0; g < KEYVIEW_GROUPS; g++) {
        w[g] = ks_measure(ks.font_bold, KeyView_GroupLabel(g));
        total += w[g];
    }
    int pad = (strip.w - 2 - total) / KEYVIEW_GROUPS;
    if (pad < 2) pad = 2;
    int x = strip.x + 1;
    for (int g = 0; g < KEYVIEW_GROUPS; g++) {
        out[g] = (SDL_Rect){ x, strip.y + 1, w[g] + pad, strip.h - 2 };
        x += w[g] + pad;
    }
    SDL_Rect *last = &out[KEYVIEW_GROUPS - 1];
    if (last->x + last->w > strip.x + strip.w - 1) last->w = strip.x + strip.w - 1 - last->x;
}

static void ks_strip(SDL_Rect r) {
    ks_fill(r.x, r.y, r.w, r.h, 33, 28, 18);
    ks_outline(r, 84, 78, 58);
}

static int ks_below_y(int line) {
    SDL_Rect last = ks_row_rect(ks.row_count > 0 ? ks.row_count - 1 : 0);
    return last.y + last.h + 13 + line * 16;
}

static void ks_scheme_rects(SDL_Rect out[KEY_SCHEME_COUNT]) {
    SDL_Rect first = ks_row_rect(0);
    int x = first.x + 2;
    int y = ks_below_y(0);
    for (int s = 0; s < KEY_SCHEME_COUNT; s++) {
        int w = 13 + 4 + ks_measure(ks.font_head, Keybinds_SchemeLabel((KeyScheme)s));
        out[s] = (SDL_Rect){ x, y, w, 14 };
        x += w + 12;
    }
}

/* The line that counts the keys of the player, in a strip of its own. */
static SDL_Rect ks_status_rect(void) {
    SDL_Rect first = ks_row_rect(0);
    SDL_Rect r = { first.x + 39, ks_below_y(1), 238, 16 };
    return r;
}

static SDL_Rect ks_box_rect(void) {
    SDL_Rect first = ks_row_rect(0);
    SDL_Rect r = { first.x + 46, ks_below_y(2) + 3,
                   13 + 4 + ks_measure(ks.font_row, "Custom keys") + 10, 16 };
    return r;
}

static SDL_Rect ks_shield_rect(void) {
    SDL_Rect first = ks_row_rect(0);
    SDL_Rect r = { first.x + 281, ks_below_y(0) - 3,
                   ks.shield[0].w ? ks.shield[0].w : 61,
                   ks.shield[0].h ? ks.shield[0].h : 47 };
    return r;
}

/* ── what a press does ───────────────────────────────────────────── */

static void ks_ask(int question, const char *text) {
    ks.question = question;
    (void)MessageBox_OpenYesNo(text);
    if (!MessageBox_IsOpen()) ks.question = Q_NONE;
}

static void ks_bind(KeyAction a, int slot, KeyChord chord) {
    KeyChord none = { 0, 0 };
    KeyChord twin = Keybinds_Get(a, 1 - slot);
    if (chord.key && twin.key == chord.key && twin.mods == chord.mods)
        Keybinds_Set(a, 1 - slot, none);
    Keybinds_Set(a, slot, chord);
}

/* A key came down for the armed slot. One that another action answers
 * is asked about first. */
static void ks_take(KeyAction a, int slot, KeyChord chord) {
    /* A key set turns the player's keys on, so ask of the table that
     * will answer then. */
    int custom_was = Keybinds_CustomOn();
    Keybinds_SetCustomOn(1);
    int other_slot = 0;
    KeyAction other = Keybinds_Conflict(a, chord, &other_slot);
    if (other == KA_NONE) { ks_bind(a, slot, chord); return; }
    char key[48], text[200];
    Keybinds_ChordText(chord, 0, key, (int)sizeof(key));
    const KeyActionInfo *mine = Keybinds_Info(a), *theirs = Keybinds_Info(other);
    snprintf(text, sizeof(text), "%s is already bound to %s.\nChange its binding to %s?",
             key, theirs ? theirs->label : "", mine ? mine->label : "");
    ks.q_action = a;
    ks.q_slot = slot;
    ks.q_chord = chord;
    ks.q_custom_was = custom_was;
    ks_ask(Q_CONFLICT, text);
    if (ks.question != Q_CONFLICT) Keybinds_SetCustomOn(custom_was);
}

static void ks_answer(int yes) {
    int q = ks.question;
    ks.question = Q_NONE;
    if (!yes) {
        if (q == Q_CONFLICT) Keybinds_SetCustomOn(ks.q_custom_was);
        return;
    }
    if (q == Q_CONFLICT) {
        KeyChord none = { 0, 0 };
        for (int guard = 0; guard < KA_COUNT * KEYBIND_SLOTS; guard++) {
            int slot = 0;
            KeyAction other = Keybinds_Conflict(ks.q_action, ks.q_chord, &slot);
            if (other == KA_NONE) break;
            Keybinds_Set(other, slot, none);
        }
        ks_bind(ks.q_action, ks.q_slot, ks.q_chord);
    } else if (q == Q_SCHEME || q == Q_RESET) {
        Keybinds_ResetAll();
    }
}

static void ks_use_scheme(KeyScheme s) {
    if (s == Keybinds_Scheme()) return;
    Keybinds_UseScheme(s);
    if (!Keybinds_HasCustom()) return;
    char text[160];
    snprintf(text, sizeof(text), "%s is now the default.\nReset to default?",
             Keybinds_SchemeLabel(s));
    ks_ask(Q_SCHEME, text);
}

static void ks_reset(void) {
    if (!Keybinds_HasCustom()) return;
    char text[160];
    snprintf(text, sizeof(text), "Reset to default %s?",
             Keybinds_SchemeLabel(Keybinds_Scheme()));
    ks_ask(Q_RESET, text);
}

static void ks_arm(int list_row, int slot) {
    KeyAction acts[KEYVIEW_MAX_ROWS];
    int n = ks_actions(acts);
    if (list_row < 0 || list_row >= n) return;
    ks.armed = 1;
    ks.armed_action = acts[list_row];
    ks.armed_slot = slot;
    ks.cur_row = list_row;
    ks.cur_slot = slot;
    ks.note[0] = '\0';
}

static void ks_clear(int list_row, int slot) {
    KeyAction acts[KEYVIEW_MAX_ROWS];
    int n = ks_actions(acts);
    KeyChord none = { 0, 0 };
    if (list_row >= 0 && list_row < n) Keybinds_Set(acts[list_row], slot, none);
}

static void ks_give_back(int list_row) {
    KeyAction acts[KEYVIEW_MAX_ROWS];
    int n = ks_actions(acts);
    if (list_row >= 0 && list_row < n) Keybinds_ResetAction(acts[list_row]);
}

static void ks_leave(int keep) {
    if (keep) {
        (void)Keybinds_Store();
        Settings_Save();
        ks.have_snap = 0;
    }
    int kept = keep;
    KeyScreen_Close();
    ks.kept = kept;
}

/* ── drawing ─────────────────────────────────────────────────────── */

static void ks_diamond(int cx, int cy) {
    for (int d = -2; d <= 2; d++) {
        int half = 2 - abs(d);
        ks_fill(cx - half, cy + d, half * 2 + 1, 1, 255, 214, 107);
    }
}

/* The text a slot shows: its key, the chord so far while it waits, or
 * the key being asked about. */
static const char *ks_slot_text(KeyAction a, int slot, const uint8_t *keys,
                                char *buf, int cap) {
    if (ks.armed && ks.armed_action == a && ks.armed_slot == slot) {
        int ctrl = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
        int shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        if (ctrl || shift)
            snprintf(buf, (size_t)cap, "%s%s...", ctrl ? "Ctrl+" : "", shift ? "Shift+" : "");
        else
            snprintf(buf, (size_t)cap, "Press a key");
        return buf;
    }
    KeyChord c = Keybinds_Get(a, slot);
    if (ks.question == Q_CONFLICT && ks.q_action == a && ks.q_slot == slot) c = ks.q_chord;
    return KeyView_ChordFit(c, KS_SLOT_W - 8, ks_measure, ks.font_row, buf, cap);
}

static void ks_help_line(const char *text) {
    if (!ks.font_bold || !text || !text[0]) return;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", text);
    int lines = 1;
    for (const char *c = buf; *c; c++) lines += *c == '\n';
    int lh = Font_LineHeight(ks.font_bold);
    int y = Font_CenterY(ks.font_bold, ks.help.y + ks.off_y, ks.help.h) - (lines - 1) * lh / 2;
    for (char *line = buf; line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        int w = Font_MeasureString(ks.font_bold, line);
        ks_text(ks.font_bold, ks.help.x + ks.off_x + (ks.help.w - w) / 2, y, line);
        y += lh;
        line = next;
    }
}

/* Draw it all. `mx`,`my` light what is under the pointer, -1 for none. */
static void ks_render(int mx, int my, int left, const uint8_t *keys) {
    GUIRuntime_Render(ks.rt);
    char help[160] = "";
    char buf[64];

    SDL_Rect groups[KEYVIEW_GROUPS];
    ks_group_rects(groups);
    ks_strip(ks_strip_rect());
    for (int g = 0; g < KEYVIEW_GROUPS; g++) {
        SDL_Rect t = groups[g];
        int hot = ks_in(t, mx, my);
        if (g == ks.group) {
            ks_fill(t.x, t.y, t.w, t.h, 74, 62, 40);
            ks_fill(t.x, t.y + t.h - 2, t.w, 2, 210, 174, 82);
        } else if (hot) {
            ks_fill(t.x, t.y, t.w, t.h, 52, 44, 29);
        }
        Font *f = g == ks.group ? ks.font_bold : ks.font_row;
        const char *label = KeyView_GroupLabel(g);
        ks_text(f, t.x + (t.w - ks_measure(f, label)) / 2, Font_CenterY(f, t.y, t.h - 1), label);
        if (hot) snprintf(help, sizeof(help), "The %s keys", label);
    }

    if (ks.row_count > 0) {
        SDL_Rect first = ks_row_rect(0);
        int hy = ks.head_y + ks.off_y;
        ks_text(ks.font_head, first.x + KS_NAME_X, hy, "Action");
        ks_text(ks.font_head, first.x + KS_SLOT_X + 4, hy, "Primary");
        ks_text(ks.font_head, first.x + KS_SLOT_X + KS_SLOT_W + KS_SLOT_GAP + 4, hy, "Alternate");
    }

    KsBar bar = ks_bar();
    KeyAction acts[KEYVIEW_MAX_ROWS];
    int n = ks_actions(acts);
    int cursor = ks.cursor_on || ks.armed;
    for (int i = 0; i < ks.row_count; i++) {
        int idx = ks.scroll + i;
        if (idx >= n) break;
        KeyAction a = acts[idx];
        const KeyActionInfo *info = Keybinds_Info(a);
        SDL_Rect r = ks_row_rect(i);
        int band_w = bar.shown ? KS_BAR_X - 2 : r.w;
        SDL_Rect whole = { r.x, r.y, band_w, r.h };
        int hot_row = cursor ? idx == ks.cur_row : ks_in(whole, mx, my);
        int asked = ks.question == Q_CONFLICT && ks.q_action == a;
        if (Keybinds_HasConflict(a) || asked) ks_fill(r.x, r.y, band_w, r.h, 74, 29, 20);
        else if (hot_row)                     ks_fill(r.x, r.y, band_w, r.h, 60, 50, 35);

        if (Keybinds_IsCustom(a)) ks_diamond(r.x + 8, r.y + r.h / 2);
        int ty = Font_CenterY(ks.font_row, r.y, r.h);
        ks_text(ks.font_row, r.x + KS_NAME_X, ty, info ? info->label : "");
        if (ks_in(ks_mark_rect(i), mx, my) && Keybinds_IsCustom(a) && !cursor)
            snprintf(help, sizeof(help), "Back to the %s key",
                     Keybinds_SchemeLabel(Keybinds_Scheme()));

        for (int slot = 0; slot < KEYBIND_SLOTS; slot++) {
            SDL_Rect sr = ks_slot_rect(i, slot);
            int armed = ks.armed && ks.armed_action == a && ks.armed_slot == slot;
            int bad = asked && ks.q_slot == slot;
            int hot = cursor ? (idx == ks.cur_row && slot == ks.cur_slot) : ks_in(sr, mx, my);
            if (armed)    ks_fill(sr.x, sr.y, sr.w, sr.h, 106, 38, 22);
            else if (bad) ks_fill(sr.x, sr.y, sr.w, sr.h, 107, 26, 18);
            const char *text = ks_slot_text(a, slot, keys, buf, (int)sizeof(buf));
            if (text[0]) ks_text(ks.font_row, sr.x + 4, ty, text);
            else         ks_fill(sr.x + 4, sr.y + sr.h / 2, 14, 1, 123, 113, 89);
            if (armed)    ks_outline(sr, 255, 214, 107);
            else if (bad) ks_outline(sr, 255, 154, 122);
            else if (hot) ks_outline(sr, 201, 168, 74);
            if (hot && !armed && !help[0]) {
                const char *risk = KeyView_InBrowser()
                    ? KeyView_BrowserNote(Keybinds_Get(a, slot)) : "";
                snprintf(help, sizeof(help), "%s\n%s", info ? info->label : "",
                         risk[0] ? risk : "LMB to change. RMB to clear");
            }
        }
    }

    if (bar.shown) {
        SDL_Surface *off = UI_Offscreen();
        if (ks.bar.px && off) {
            int h = ks.bar.h < bar.track.h ? ks.bar.h : bar.track.h;
            Blit_RGBA(off, bar.track.x, bar.track.y, ks.bar.px, ks.bar.w, h);
        } else {
            ks_fill(bar.track.x, bar.track.y, bar.track.w, bar.track.h, 27, 26, 18);
        }
        ks_blit(&ks.inc[left && ks_in(bar.inc, mx, my)], bar.inc.x, bar.inc.y);
        ks_blit(&ks.dec[left && ks_in(bar.dec, mx, my)], bar.dec.x, bar.dec.y);
        ks_blit(&ks.thumb, bar.thumb.x, bar.thumb.y);
    }

    SDL_Rect schemes[KEY_SCHEME_COUNT];
    ks_scheme_rects(schemes);
    for (int s = 0; s < KEY_SCHEME_COUNT; s++) {
        ks_blit(&ks.radio[(KeyScheme)s == Keybinds_Scheme()], schemes[s].x, schemes[s].y);
        ks_text(ks.font_head, schemes[s].x + 17, schemes[s].y, Keybinds_SchemeLabel((KeyScheme)s));
        if (ks_in(schemes[s], mx, my))
            snprintf(help, sizeof(help), "Make %s the default keys",
                     Keybinds_SchemeLabel((KeyScheme)s));
    }
    SDL_Rect status = ks_status_rect();
    ks_strip(status);
    ks_text(ks.font_row, status.x + 5, Font_CenterY(ks.font_row, status.y, status.h),
            KeyView_Status(buf, (int)sizeof(buf)));

    if (Keybinds_HasCustom()) {
        SDL_Rect box = ks_box_rect();
        SDL_Rect cap = { box.x + 17, box.y, box.w - 17, box.h };
        ks_blit(&ks.box[Keybinds_CustomOn() != 0], box.x, box.y + 1);
        ks_strip(cap);
        ks_text(ks.font_row, cap.x + 5, Font_CenterY(ks.font_row, cap.y, cap.h), "Custom keys");
        if (ks_in(box, mx, my))
            snprintf(help, sizeof(help), "Toggle your custom keys on or off");
        SDL_Rect sh = ks_shield_rect();
        int over = ks_in(sh, mx, my);
        ks_blit_shield(&ks.shield[left && over], sh.x, sh.y);
        if (over)
            snprintf(help, sizeof(help), "Reset to default %s",
                     Keybinds_SchemeLabel(Keybinds_Scheme()));
    }

    if (ks.armed) {
        const KeyActionInfo *info = Keybinds_Info(ks.armed_action);
        if (ks.note[0])
            snprintf(help, sizeof(help), "%s\nEsc discards changes", ks.note);
        else
            snprintf(help, sizeof(help), "Press a key for %s\nEsc discards changes",
                     info ? info->label : "");
    } else if (!help[0]) {
        const GUIWidget *hw = GUIRuntime_HoveredWidget(ks.rt);
        if (hw && hw->tooltip[0]) snprintf(help, sizeof(help), "%s", hw->tooltip);
    }
    if (!MessageBox_IsOpen()) ks_help_line(help);
}

/* ── the frame ───────────────────────────────────────────────────── */

#define KS_EDGE(sc) (keys[sc] && !ks.prev_keys[sc])

static void ks_keys(const uint8_t *keys) {
    int n = ks_count();
    int moved = 0;
    if (KS_EDGE(SDL_SCANCODE_UP))    { ks.cur_row--; moved = 1; }
    if (KS_EDGE(SDL_SCANCODE_DOWN))  { ks.cur_row++; moved = 1; }
    if (KS_EDGE(SDL_SCANCODE_LEFT))  { ks.cur_slot = 0; moved = 1; }
    if (KS_EDGE(SDL_SCANCODE_RIGHT)) { ks.cur_slot = 1; moved = 1; }
    if (KS_EDGE(SDL_SCANCODE_PAGEUP))   { ks.cur_row -= ks.row_count; moved = 1; }
    if (KS_EDGE(SDL_SCANCODE_PAGEDOWN)) { ks.cur_row += ks.row_count; moved = 1; }
    if (KS_EDGE(SDL_SCANCODE_TAB)) {
        int back = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        ks_set_group(ks.group + (back ? -1 : 1));
        n = ks_count();
        moved = 1;
    }
    if (moved) {
        if (ks.cur_row >= n) ks.cur_row = n - 1;
        if (ks.cur_row < 0) ks.cur_row = 0;
        ks.cursor_on = 1;
        ks_show_cursor();
    }
    if (!ks.cursor_on) return;
    if (KS_EDGE(SDL_SCANCODE_SPACE)) ks_arm(ks.cur_row, ks.cur_slot);
    if (KS_EDGE(SDL_SCANCODE_DELETE) || KS_EDGE(SDL_SCANCODE_BACKSPACE))
        ks_clear(ks.cur_row, ks.cur_slot);
}

/* A left press that is not on Ok or Cancel. Returns 1 when it hit. */
static int ks_press(int mx, int my) {
    SDL_Rect groups[KEYVIEW_GROUPS];
    ks_group_rects(groups);
    for (int g = 0; g < KEYVIEW_GROUPS; g++)
        if (ks_in(groups[g], mx, my)) { ks_set_group(g); return 1; }

    SDL_Rect schemes[KEY_SCHEME_COUNT];
    ks_scheme_rects(schemes);
    for (int s = 0; s < KEY_SCHEME_COUNT; s++)
        if (ks_in(schemes[s], mx, my)) { ks_use_scheme((KeyScheme)s); return 1; }

    if (Keybinds_HasCustom()) {
        if (ks_in(ks_shield_rect(), mx, my)) { ks_reset(); return 1; }
        if (ks_in(ks_box_rect(), mx, my)) {
            Keybinds_SetCustomOn(!Keybinds_CustomOn());
            return 1;
        }
    }

    KsBar bar = ks_bar();
    if (bar.shown) {
        if (ks_in(bar.inc, mx, my)) { ks.scroll--; ks_clamp(); return 1; }
        if (ks_in(bar.dec, mx, my)) { ks.scroll++; ks_clamp(); return 1; }
        if (ks_in(bar.thumb, mx, my)) {
            ks.dragging = 1;
            ks.grab_dy = my - bar.thumb.y;
            return 1;
        }
        if (ks_in(bar.track, mx, my)) {
            ks.scroll += my < bar.thumb.y ? -ks.row_count : ks.row_count;
            ks_clamp();
            return 1;
        }
    }

    int n = ks_count();
    for (int i = 0; i < ks.row_count && ks.scroll + i < n; i++) {
        if (ks_in(ks_mark_rect(i), mx, my)) { ks_give_back(ks.scroll + i); return 1; }
        for (int slot = 0; slot < KEYBIND_SLOTS; slot++)
            if (ks_in(ks_slot_rect(i, slot), mx, my)) { ks_arm(ks.scroll + i, slot); return 1; }
    }
    return 0;
}

/* A right press clears the slot under it. */
static int ks_right_press(int mx, int my) {
    int n = ks_count();
    for (int i = 0; i < ks.row_count && ks.scroll + i < n; i++)
        for (int slot = 0; slot < KEYBIND_SLOTS; slot++)
            if (ks_in(ks_slot_rect(i, slot), mx, my)) {
                ks_clear(ks.scroll + i, slot);
                return 1;
            }
    return 0;
}

int KeyScreen_Tick(TAK_Platform *platform) {
    if (!ks.open) return 0;

    int focus = platform && platform->has_focus;
    uint8_t keys[SDL_NUM_SCANCODES];
    memset(keys, 0, sizeof(keys));
    if (focus) memcpy(keys, SDL_GetKeyboardState(NULL), sizeof(keys));

    int mx = -1, my = -1, left = 0, right = 0;
    if (focus) {
        left = TAK_Platform_MouseThisFrame(platform, &mx, &my);
        right = (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    }
    if (ks.wait_up) {
        if (!left && !right) ks.wait_up = 0;
        left = right = 0;
    }
    int left_edge = left && !ks.prev_left;
    int right_edge = right && !ks.prev_right;
    int enter_edge = KS_EDGE(SDL_SCANCODE_RETURN) || KS_EDGE(SDL_SCANCODE_KP_ENTER);
    int esc_edge = KS_EDGE(SDL_SCANCODE_ESCAPE);
    if (mx != ks.last_mx || my != ks.last_my) ks.cursor_on = 0;
    ks.last_mx = mx;
    ks.last_my = my;

    /* The platform's pump has taken the wheel events off the queue. */
    int wheel = focus ? platform->wheel_dy : 0;

    int leave = -1;
    if (MessageBox_IsOpen()) {
        /* The question owns the keys and the mouse until it is answered. */
        ks_render(-1, -1, 0, keys);
        int answer = MessageBox_Tick(mx, my, left, enter_edge, esc_edge);
        if (answer) {
            ks_answer(answer == 1);
            ks.wait_up = 1;
            ks_clamp();
        }
    } else if (ks.armed) {
        if (esc_edge || left_edge || right_edge) {
            ks.armed = 0;
            ks.wait_up = 1;
        } else {
            KeyChord chord;
            int rc = Keybinds_Capture(ks.armed_action, keys, ks.prev_keys, &chord);
            if (rc > 0) {
                ks.armed = 0;
                ks_take(ks.armed_action, ks.armed_slot, chord);
            } else if (rc < 0) {
                snprintf(ks.note, sizeof(ks.note), "%s",
                         KeyView_RefusalNote(keys, ks.prev_keys));
            }
        }
        (void)GUIRuntime_Update(ks.rt, -1, -1, 0, NULL, 0);
        ks_render(-1, -1, 0, keys);
    } else {
        char clicked[64] = "";
        int got = GUIRuntime_Update(ks.rt, mx, my, left, clicked, sizeof(clicked));
        if (!left) ks.dragging = 0;
        if (ks.dragging) {
            KsBar bar = ks_bar();
            ks.scroll = KeyView_ScrollAt(my - ks.grab_dy - bar.track.y, ks_count(),
                                         ks.row_count, bar.track.h - bar.thumb.h);
        } else if (got && tak_stricmp(clicked, "Ok") == 0) {
            leave = 1;
        } else if (got && tak_stricmp(clicked, "Cancel") == 0) {
            leave = 0;
        } else if (left_edge) {
            (void)ks_press(mx, my);
        } else if (right_edge) {
            (void)ks_right_press(mx, my);
        }
        if (wheel) { ks.scroll -= wheel; ks_clamp(); }
        if (leave < 0 && !ks.armed && !MessageBox_IsOpen()) {
            ks_keys(keys);
            if (enter_edge)    leave = 1;
            else if (esc_edge) leave = 0;
        }
        if (leave < 0) {
            ks_render(mx, my, left, keys);
            if (MessageBox_IsOpen()) MessageBox_Render();
        }
    }

    memcpy(ks.prev_keys, keys, sizeof(ks.prev_keys));
    ks.prev_left = left;
    ks.prev_right = right;
    if (leave >= 0) { ks_leave(leave); return 0; }
    return 1;
}

/* ── open and close ──────────────────────────────────────────────── */

int KeyScreen_Open(void) {
    KeyScreen_Close();
    if (GUIDialog_Load(&ks.dialog, KS_GUI) != 0) {
        fprintf(stderr, "KeyScreen: %s will not load\n", KS_GUI);
        return -1;
    }
    ks.rt = GUIRuntime_Create(&ks.dialog);
    if (!ks.rt) { GUIDialog_Free(&ks.dialog); return -1; }
    ks.open = 1;
    ks.kept = 0;

    SDL_Rect area;
    HUD_DialogArea(&area);
    GUI_CenterOffset(&ks.dialog, area, &ks.off_x, &ks.off_y);
    GUIRuntime_SetOffset(ks.rt, ks.off_x, ks.off_y);
    ks_read_dialog();
    ks_load_art();

    ks.font_row  = Font_Load("data/fonts/b_times new roman (100)", UI_RGBAFormat());
    ks.font_bold = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    ks.font_head = Font_Load("data/fonts/lombardic (cd)", UI_RGBAFormat());
    if (!ks.font_row || !ks.font_bold || !ks.font_head || ks.row_count < 1) {
        fprintf(stderr, "KeyScreen: a font or the rows of %s are missing\n", KS_GUI);
        KeyScreen_Close();
        return -1;
    }

    Keybinds_Snapshot(&ks.snap);
    ks.have_snap = 1;
    /* A key or a button held from the press that opened it is spent. */
    memset(ks.prev_keys, 1, sizeof(ks.prev_keys));
    ks.wait_up = 1;
    ks.last_mx = ks.last_my = -1;
    return 0;
}

int KeyScreen_IsOpen(void) { return ks.open; }
int KeyScreen_Kept(void) { return ks.kept; }

void KeyScreen_Close(void) {
    if (!ks.open) return;
    if (ks.have_snap) Keybinds_Restore(&ks.snap);
    if (ks.question) MessageBox_Close();
    if (ks.rt) GUIRuntime_Destroy(ks.rt);
    GUIDialog_Free(&ks.dialog);
    if (ks.font_row)  Font_Free(ks.font_row);
    if (ks.font_bold) Font_Free(ks.font_bold);
    if (ks.font_head) Font_Free(ks.font_head);
    ks_free_art();
    memset(&ks, 0, sizeof(ks));
}
