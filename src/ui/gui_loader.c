/*
 * gui_loader.c -- Parse TAK .gui files into a GUIDialog.
 *
 * Format (whitespace-separated tokens):
 *   bare int            → value
 *   N c1 c2 ... cN      → N-character string ("8 MainMenu")
 *   N v1 v2 ... vN      → N-element array (count + elements)
 *
 * Every widget is laid out as:
 *
 *   type flags                              │ e.g. "4 1"
 *   <type-specific header ints>             │ varies by type
 *   <extra 1>                               │ "1"
 *   rect  2 x y w h f f f f                 │
 *   color 3 r g b a                         │
 *   misc  1 0 0                             │
 *   font  "1 0" or "1 N <fontname>"         │
 *   name  N <name> action frame_count       │
 *   frames × frame_count                    │ "1 N <gaf> N <seq> frame trans"
 *                                           │ or "1 0 0 0 0" (empty frame)
 *   trans_count + (3*trans_count + 1) ints  │
 *   sounds × frame_count                    │ "1 0" or "1 N <wavname>"
 *   tooltip "2 0 N <text>"                  │
 *   trailing int (child count or 0)         │
 *
 * Unknown widget types are parsed best-effort; if the lexer desyncs we
 * stop on the root dialog rather than producing garbage. Reference:
 * the legacy reference — search `GUI_LoadDialogFromFile`.
 */

#include "tak_gui.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Tokenizer ───────────────────────────────────────────────────────── */

typedef struct {
    const char *p;
    const char *end;
    int         err;    /* sticky — once non-zero, reads become no-ops */
} Lex;

static void lex_init(Lex *l, const char *buf, size_t len) {
    l->p = buf;
    l->end = buf + len;
    l->err = 0;
}

static int lex_at_end(Lex *l) { return l->p >= l->end; }

static void lex_skip_ws(Lex *l) {
    while (l->p < l->end && isspace((unsigned char)*l->p)) l->p++;
}

static int lex_int(Lex *l) {
    if (l->err) return 0;
    lex_skip_ws(l);
    if (lex_at_end(l)) { l->err = 1; return 0; }
    char *end = NULL;
    long v = strtol(l->p, &end, 10);
    if (end == l->p) { l->err = 2; return 0; }
    l->p = end;
    return (int)v;
}

/* Read a length-prefixed string, already having read the length `n`. One
 * separator space precedes the n payload bytes. Copies into `out` with
 * null termination; silently truncates if out_cap is too small. */
static void lex_payload_chars(Lex *l, int n, char *out, size_t out_cap) {
    if (l->err || n < 0) { if (out && out_cap) out[0] = '\0'; return; }
    if (l->p < l->end && *l->p == ' ') l->p++;
    if (l->p + n > l->end) {
        l->err = 3;
        if (out && out_cap) out[0] = '\0';
        return;
    }
    size_t cp = (out_cap && (size_t)n < out_cap - 1) ? (size_t)n : (out_cap ? out_cap - 1 : 0);
    if (out && out_cap) {
        memcpy(out, l->p, cp);
        out[cp] = '\0';
    }
    l->p += n;
}

static void lex_string(Lex *l, char *out, size_t out_cap) {
    int n = lex_int(l);
    lex_payload_chars(l, n, out, out_cap);
}

/* "1 0" (no payload) or "1 N <text>". Writes payload into out if non-NULL. */
static void lex_optional_string(Lex *l, char *out, size_t out_cap) {
    if (out && out_cap) out[0] = '\0';
    (void)lex_int(l);                 /* leading 1 */
    int n = lex_int(l);
    if (n <= 0) return;
    lex_payload_chars(l, n, out, out_cap);
}

/* ── Widget body ─────────────────────────────────────────────────────── */

static void parse_widget_body(Lex *l, GUIWidget *w) {
    /* Rect: "2 x y w h f f f f" — first int is a count marker we ignore.
     * The "extra 1" that sits between flags and the rect on most widgets
     * is consumed in parse_type_header, so we arrive directly at the
     * rect count marker here. */
    (void)lex_int(l);
    w->rect.x = lex_int(l);
    w->rect.y = lex_int(l);
    w->rect.w = lex_int(l);
    w->rect.h = lex_int(l);
    /* First flag after the rect is the authored visibility. Legacy keeps
     * it at widget+0x14 and skips the blit when it is clear
     * (legacy:312621, legacy:45228). */
    w->visible = lex_int(l) ? 1 : 0;
    (void)lex_int(l); (void)lex_int(l); (void)lex_int(l);

    /* Color: "3 r g b a" */
    (void)lex_int(l);
    int r = lex_int(l), g = lex_int(l), b = lex_int(l), a = lex_int(l);
    w->color_rgba = ((uint32_t)r << 24) | ((uint32_t)g << 16) |
                    ((uint32_t)b << 8)  | (uint32_t)a;

    /* Cursor reference: "1 N1 [chars] N2 [chars]".
     * Most dialogs have "1 0 0" (empty cursor gaf + empty cursor name).
     * options.gui uses "1 11 cursors.gaf 12 cursornormal".
     * Both strings are optional — N==0 means absent. */
    (void)lex_int(l);                /* leading 1 */
    int cur_n1 = lex_int(l);
    if (cur_n1 > 0) {
        if (l->p < l->end && *l->p == ' ') l->p++;
        if (l->p + cur_n1 > l->end) { l->err = 6; return; }
        l->p += cur_n1;
    }
    int cur_n2 = lex_int(l);
    if (cur_n2 > 0) {
        if (l->p < l->end && *l->p == ' ') l->p++;
        if (l->p + cur_n2 > l->end) { l->err = 6; return; }
        l->p += cur_n2;
    }

    /* Optional font reference */
    lex_optional_string(l, w->font, sizeof(w->font));

    /* Name followed by two ints ("action" + a flag we don't use); the
     * real frame_count sits on the next line (typically matches the
     * second int, but not always — see checkbox where that pair is
     * "3 5" but frame_count is 5). */
    lex_string(l, w->name, sizeof(w->name));
    (void)lex_int(l);    /* action */
    (void)lex_int(l);    /* flag (often echoes frame_count) */
    int frame_count = lex_int(l);

    /* Frames: for each frame either "1 N <gaf> N <seq> frame trans"
     * or "1 0 0 0 0" (empty placeholder). */
    w->num_frames = 0;
    for (int i = 0; i < frame_count && !l->err; i++) {
        (void)lex_int(l);                    /* leading 1 */
        int gaf_len = lex_int(l);
        if (gaf_len <= 0) {
            /* Empty frame: three more zeros follow. */
            (void)lex_int(l); (void)lex_int(l); (void)lex_int(l);
            continue;
        }
        GUIFrameRef *fr = (w->num_frames < GUI_MAX_FRAMES)
                          ? &w->frames[w->num_frames++] : NULL;
        lex_payload_chars(l, gaf_len, fr ? fr->gaf : NULL,
                          fr ? sizeof(fr->gaf) : 0);
        lex_string(l, fr ? fr->sequence : NULL,
                   fr ? sizeof(fr->sequence) : 0);
        int fi = lex_int(l), tr = lex_int(l);
        if (fr) { fr->frame_index = fi; fr->transparency = tr; }
    }

    /* Transitions table. Each entry is three ints followed by an optional
     * string:  action flag N [N-char string].
     * The string on the first non-empty entry is the widget's *display
     * text* (what labels render to screen). An extra trailing int follows
     * the last entry. */
    int trans_count = lex_int(l);
    int captured_display = 0;
    for (int i = 0; i < trans_count && !l->err; i++) {
        (void)lex_int(l);                /* action */
        int align = lex_int(l);          /* flag: 0 centred, 1 left */
        int tlen = lex_int(l);
        if (tlen > 0) {
            if (l->p < l->end && *l->p == ' ') l->p++;
            if (l->p + tlen > l->end) { l->err = 5; break; }
            if (!captured_display && w) {
                size_t cp = (size_t)tlen < sizeof(w->display_text) - 1
                            ? (size_t)tlen : sizeof(w->display_text) - 1;
                memcpy(w->display_text, l->p, cp);
                w->display_text[cp] = '\0';
                w->text_align = align;
                captured_display = 1;
            }
            l->p += tlen;
        }
    }
    (void)lex_int(l);                    /* trailing int */

    /* Sound slots, one per frame. Each is either "1 0" or "1 N <wav>".
     * The first named one is the click sound (the end screens author
     * ok.wav and cancel.wav on their buttons). */
    for (int i = 0; i < frame_count && !l->err; i++) {
        char wav[64];
        lex_optional_string(l, wav, sizeof(wav));
        if (wav[0] && !w->sound[0]) {
            strncpy(w->sound, wav, sizeof(w->sound) - 1);
            w->sound[sizeof(w->sound) - 1] = '\0';
        }
    }

    /* Tooltip: "2 0 N <text>" (N may be 0) */
    (void)lex_int(l);                        /* 2 */
    (void)lex_int(l);                        /* 0 */
    lex_string(l, w->tooltip, sizeof(w->tooltip));

    /* Trailing child count (0 on leaves). We ignore it — the caller
     * simply keeps parsing widgets until the lexer reaches EOF. */
    (void)lex_int(l);
}

/* Consume the widget's pre-rect header. Every widget has a "flags" int
 * right after the type code; most widgets also have one or more extra
 * ints before the rect count marker. This function lands the lexer on
 * the rect marker ("2"), no matter the widget type. */
static void parse_type_header(Lex *l, int type) {
    int version = lex_int(l);  /* flags (usually 1), a version for some */
    switch (type) {
    case GUI_WT_WINDOW:
    case GUI_WT_CONTAINER:
        /* Root window / bare container has no extra — next token is the
         * rect count. */
        break;

    case GUI_WT_BUTTON:
    case GUI_WT_LABEL:
    case GUI_WT_PANEL:
    case GUI_WT_STAGEBUTTON:
        /* "<extra 1>" before the rect. */
        (void)lex_int(l);
        break;

    case GUI_WT_CHECKBOX:
        (void)lex_int(l);   /* state_count (usually 6) */
        (void)lex_int(l);   /* extra */
        break;

    case GUI_WT_MULTISTATE:
        (void)lex_int(l);   /* default_state */
        (void)lex_int(l);   /* -1 sentinel */
        (void)lex_int(l);   /* extra */
        break;

    case GUI_WT_SLIDER:
        /* "12 1 min max step value 0" + extra "1" — 6 ints after flags */
        (void)lex_int(l); (void)lex_int(l);   /* min, max */
        (void)lex_int(l); (void)lex_int(l);   /* step, value */
        (void)lex_int(l); (void)lex_int(l);   /* flag, extra */
        break;

    case GUI_WT_PROGRESS:
        /* "16 <version> value step max flag" then the shared extra int.
         * The original writes value, step and max after the version and
         * reads the fourth field only when the version is above 1
         * (legacy:328245, legacy:328267). Every shipped dialog is
         * version 2, so all four are present. Without this the lexer
         * desynced on the first progress bar and every widget after it
         * was dropped, which cost loadscreen.gui its stained glass. */
        (void)lex_int(l); (void)lex_int(l);   /* value, step */
        (void)lex_int(l); (void)lex_int(l);   /* max, flag   */
        (void)lex_int(l);                     /* extra       */
        break;

    case GUI_WT_SCROLLBTN:
        /* "14 1" then three extra ints (all "1") before the rect. */
        (void)lex_int(l); (void)lex_int(l); (void)lex_int(l);
        break;

    case GUI_WT_LISTBOX:
        /* "15 (flags) N r g b a" then "0 0 0 0" + extra 1 */
        (void)lex_int(l);   /* count */
        (void)lex_int(l); (void)lex_int(l);
        (void)lex_int(l); (void)lex_int(l);
        (void)lex_int(l); (void)lex_int(l);
        (void)lex_int(l); (void)lex_int(l);
        (void)lex_int(l);   /* extra */
        break;

    case GUI_WT_EDIT:
        /* Read the way legacy:321014-321059 reads it: a field when the
         * version is 3 or more, the maximum length when it is 2 or more,
         * the initial text, five values, a style, four flags and two
         * colours, then the shared extra. */
        if (version >= 3) (void)lex_int(l);
        if (version >= 2) (void)lex_int(l);
        lex_string(l, NULL, 0);
        for (int i = 0; i < 5; i++) (void)lex_int(l);
        (void)lex_int(l);                                /* style   */
        for (int i = 0; i < 4; i++) (void)lex_int(l);    /* flags   */
        for (int i = 0; i < 10; i++) (void)lex_int(l);   /* colours */
        (void)lex_int(l);                                /* extra   */
        break;

    default:
        /* Unknown type — assume one extra int and hope to resync. */
        (void)lex_int(l);
        break;
    }
}

static int parse_widget(Lex *l, GUIWidget *w) {
    memset(w, 0, sizeof(*w));
    int type = lex_int(l);
    if (l->err) return -1;
    w->type = (GUIWidgetType)type;
    parse_type_header(l, type);
    parse_widget_body(l, w);
    return l->err ? -1 : 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int GUIDialog_LoadFromBuffer(GUIDialog *out, const char *buffer, size_t len) {
    if (!out || !buffer) return -1;
    memset(out, 0, sizeof(*out));

    Lex lex;
    lex_init(&lex, buffer, len);

    if (parse_widget(&lex, &out->root) != 0) {
        fprintf(stderr, "GUIDialog_Load: failed to parse root widget\n");
        return -1;
    }

    size_t cap = 16;
    out->children = tak_malloc(cap * sizeof(GUIWidget));
    if (!out->children) return -1;
    out->num_children = 0;

    while (!lex.err) {
        lex_skip_ws(&lex);
        if (lex_at_end(&lex)) break;

        if (out->num_children >= (int)cap) {
            cap *= 2;
            GUIWidget *tmp = tak_realloc(out->children, cap * sizeof(GUIWidget));
            if (!tmp) return -1;
            out->children = tmp;
        }
        GUIWidget *w = &out->children[out->num_children];

        const char *before = lex.p;
        if (parse_widget(&lex, w) != 0) {
            long offset = (long)(before - buffer);
            long end_offset = (long)(lex.p - buffer);
            fprintf(stderr,
                    "GUIDialog_Load: widget #%d (started at offset %ld, "
                    "stopped at %ld, type=%d name='%s', next='%.40s'): err=%d\n",
                    out->num_children, offset, end_offset,
                    (int)w->type, w->name,
                    (lex.p < lex.end) ? lex.p : "<eof>", lex.err);
            break;
        }

        if (w->type < (int)GUI_WT_CONTAINER ||
            (w->type > (int)GUI_WT_LABEL && w->type != (int)GUI_WT_EDIT)) {
            fprintf(stderr,
                    "GUIDialog_Load: widget #%d has unknown type %d "
                    "(offset %ld, name='%s', next='%.40s') — stopping\n",
                    out->num_children, (int)w->type,
                    (long)(before - buffer), w->name,
                    (lex.p < lex.end) ? lex.p : "<eof>");
            break;
        }
        out->num_children++;
    }

    return 0;
}

int GUIDialog_Load(GUIDialog *out, const char *path) {
    uint32_t size = 0;
    uint8_t *buf = NULL;
    if (VFS_ReadFile(path, (void **)&buf, &size) != 0 || !buf) {
        fprintf(stderr, "GUIDialog_Load: cannot read %s\n", path);
        return -1;
    }
    int rc = GUIDialog_LoadFromBuffer(out, (const char *)buf, (size_t)size);
    if (rc == 0 && out) {
        strncpy(out->path, path, sizeof(out->path) - 1);
    }
    tak_free(buf);
    return rc;
}

void GUIDialog_Free(GUIDialog *dialog) {
    if (!dialog) return;
    if (dialog->children) tak_free(dialog->children);
    memset(dialog, 0, sizeof(*dialog));
}

/* The caption of the checkbox at `box`: the shown label to its left on
 * its line, or -1. */
static int option_caption(const GUIDialog *d, const GUIWidget *box) {
    int label = -1, best = 1 << 30;
    int mid = box->rect.y + box->rect.h / 2;
    for (int i = 0; i < d->num_children; i++) {
        const GUIWidget *w = &d->children[i];
        if (w->type != GUI_WT_LABEL || !w->visible) continue;
        if (w->rect.x + w->rect.w > box->rect.x + 4) continue;
        int off = w->rect.y + w->rect.h / 2 - mid;
        if (off < 0) off = -off;
        if (off <= 8 && off < best) { best = off; label = i; }
    }
    return label;
}

#define OPTION_ROWS_MAX 16

int GUIDialog_AddOptionRow(GUIDialog *d, const char *const *rows, int row_count,
                           const char *name, const char *caption, const char *tooltip) {
    if (!d || !rows || !name) return -1;
    /* The shown rule rows, top to bottom, each with its caption. */
    int box_at[OPTION_ROWS_MAX], cap_at[OPTION_ROWS_MAX], n = 0;
    for (int i = 0; i < d->num_children && n < OPTION_ROWS_MAX; i++) {
        const GUIWidget *w = &d->children[i];
        if (w->type != GUI_WT_CHECKBOX || !w->visible) continue;
        int listed = 0;
        for (int r = 0; r < row_count && !listed; r++)
            listed = tak_stricmp(w->name, rows[r]) == 0;
        if (!listed) continue;
        int k = n++;
        while (k > 0 && d->children[box_at[k - 1]].rect.y > w->rect.y) {
            box_at[k] = box_at[k - 1];
            k--;
        }
        box_at[k] = i;
    }
    if (n == 0) return -1;
    for (int k = 0; k < n; k++) cap_at[k] = option_caption(d, &d->children[box_at[k]]);
    const GUIWidget *last = &d->children[box_at[n - 1]];
    int first_y = d->children[box_at[0]].rect.y;
    int step = n > 1 ? (last->rect.y - first_y) / (n - 1) : last->rect.h + 9;
    int left = cap_at[n - 1] >= 0 ? d->children[cap_at[n - 1]].rect.x : last->rect.x;
    int right = last->rect.x + last->rect.w;
    /* The first thing shown under the rows in their column, where the
     * new row has to stop. A frame around the whole panel is not one. */
    int floor_y = 1 << 30;
    for (int i = 0; i < d->num_children; i++) {
        const GUIWidget *w = &d->children[i];
        if (!w->visible || w->rect.h > 3 * step) continue;
        if (w->rect.x >= right || w->rect.x + w->rect.w <= left) continue;
        if (w->rect.y < last->rect.y + last->rect.h) continue;
        if (w->rect.y < floor_y) floor_y = w->rect.y;
    }
    /* With no room under the last row, the rows close up evenly, the
     * first staying where it is (D-036). */
    if (last->rect.y + step + last->rect.h > floor_y - 2) {
        int room = floor_y - 2 - last->rect.h - first_y;
        int pitch = room / n;
        if (pitch < last->rect.h) return -1;
        for (int k = 0; k < n; k++) {
            GUIWidget *b = &d->children[box_at[k]];
            int dy = first_y + k * pitch - b->rect.y;
            b->rect.y += dy;
            if (cap_at[k] >= 0) d->children[cap_at[k]].rect.y += dy;
        }
        step = pitch;
    }
    GUIWidget box = d->children[box_at[n - 1]];
    GUIWidget cap;
    if (cap_at[n - 1] >= 0) cap = d->children[cap_at[n - 1]];
    GUIWidget *grown = (GUIWidget *)tak_realloc(d->children,
        (size_t)(d->num_children + 2) * sizeof(GUIWidget));
    if (!grown) return -1;
    d->children = grown;
    box.rect.y += step;
    snprintf(box.name, sizeof box.name, "%s", name);
    snprintf(box.tooltip, sizeof box.tooltip, "%s", tooltip ? tooltip : "");
    box.u.checkbox.value = 0;
    d->children[d->num_children++] = box;
    if (cap_at[n - 1] >= 0) {
        cap.rect.y += step;
        snprintf(cap.name, sizeof cap.name, "%sText", name);
        snprintf(cap.display_text, sizeof cap.display_text, "%s", caption ? caption : "");
        snprintf(cap.tooltip, sizeof cap.tooltip, "%s", tooltip ? tooltip : "");
        d->children[d->num_children++] = cap;
    }
    return 0;
}

GUIWidget *GUIDialog_FindByName(GUIDialog *dialog, const char *name) {
    if (!dialog || !name) return NULL;
    for (int i = 0; i < dialog->num_children; i++) {
        if (tak_stricmp(dialog->children[i].name, name) == 0)
            return &dialog->children[i];
    }
    return NULL;
}
