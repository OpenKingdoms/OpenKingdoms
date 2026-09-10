#ifndef TAK_GUI_H
#define TAK_GUI_H

#include "tak_types.h"
#include <SDL.h>

/*
 * TAK .gui file loader and generic widget model.
 *
 * A .gui file is a whitespace-separated token stream that describes a
 * single dialog (root window) containing N child widgets. Every widget
 * shares the same prolog (type code, rect, color, font ref, name,
 * frames, transitions, sounds, tooltip) — this loader parses all of
 * that into a plain-data GUIDialog that screens can render and inspect
 * by widget name.
 *
 * The format is self-describing:
 *   bare integer               → a single value
 *   N v1 v2 ... vN             → N-element array
 *   N c1 c2 ... cN             → N-character string (e.g. "8 MainMenu")
 *
 * Reference implementation in the legacy reference — search
 * `GUI_LoadDialogFromFile` (20+ callers). This port covers the subset
 * of widget types actually used by the menu dialogs; unknown widget
 * types are parsed defensively (skipped past) so we don't break on
 * types we haven't reverse-engineered yet.
 */

/* ── Widget type codes (the first integer of each widget block) ─────── */

typedef enum {
    GUI_WT_CONTAINER  = 1,    /* bare container — used as sub-dialog root  */
    GUI_WT_WINDOW     = 2,    /* root dialog / panel background */
    GUI_WT_BUTTON     = 4,    /* AnimButton — N-frame animated sprite */
    GUI_WT_CHECKBOX   = 5,    /* Checkbox with 5-frame on/off/hover sheet */
    GUI_WT_PANEL      = 9,    /* Static panel / groupbox */
    GUI_WT_SLIDER     = 12,   /* Horizontal slider (min, max, step, value) */
    GUI_WT_SCROLLBTN  = 14,   /* Scrollbar increment/decrement nub */
    GUI_WT_LISTBOX    = 15,   /* Scrollable list of strings */
    GUI_WT_STAGEBUTTON = 17,  /* StageBtn — 3-frame button like GUI_WT_BUTTON */
    GUI_WT_MULTISTATE = 18,   /* Click-to-cycle selector (teams, colors) */
    GUI_WT_LABEL      = 19,   /* Static text (single line) */
} GUIWidgetType;

/* ── A single frame reference in a multi-frame widget ───────────────── */

/* Example from the .gui: `1 17 singlemachine.gaf 14 SingleMachine0 0 18`
 * decodes to { gaf="singlemachine.gaf", sequence="SingleMachine0",
 *              frame_index=0, trans_index=18 } */
typedef struct GUIFrameRef {
    char gaf[64];          /* e.g. "mainscreen.gaf"            */
    char sequence[64];     /* e.g. "OptionsButton"              */
    int  frame_index;      /* which frame of the sequence      */
    int  transparency;     /* palette index treated as transparent */
} GUIFrameRef;

/* ── Widget ─────────────────────────────────────────────────────────── */

#define GUI_MAX_FRAMES 16  /* buttons have 3, checkboxes 5, multistate up to 12 */

typedef struct GUIWidget {
    GUIWidgetType type;
    SDL_Rect      rect;
    /* Authored visibility: the first flag after the rect. Legacy keeps it
     * at widget+0x14 and gates drawing on it (legacy:312621). */
    int           visible;
    uint32_t      color_rgba;    /* packed RRGGBBAA (often unused by us) */
    char          name[64];      /* e.g. "PlayComputer", "LineOfSight"  */
    char          font[64];      /* matching font .gaf (empty = none)   */
    char          display_text[128]; /* text rendered for labels/statics;
                                      * from the widget's transition block */
    char          tooltip[128];  /* hover help string (bottom strip)    */

    /* Frame sprites (buttons/checkboxes/multistate). Zero-initialized
     * if the widget has no frames (labels, panels). */
    int           num_frames;
    GUIFrameRef   frames[GUI_MAX_FRAMES];

    /* Optional payload per widget type */
    union {
        struct { int value; }                       checkbox;
        struct { int min, max, step, value; }       slider;
        struct { int state_count; int state; }      multistate;
        struct { char text[128]; int default_val; } label;
    } u;
} GUIWidget;

/* ── Dialog ─────────────────────────────────────────────────────────── */

typedef struct GUIDialog {
    char       path[128];        /* source .gui path (for debugging)   */
    GUIWidget  root;             /* the root window widget             */
    GUIWidget *children;         /* heap-allocated array               */
    int        num_children;
} GUIDialog;

/* Parse a .gui file under TAK_DATA_DIR (e.g. "data/guis/mainmenu.gui").
 * Returns 0 on success; caller must free with GUIDialog_Free. */
int GUIDialog_Load(GUIDialog *out, const char *path);

/* Parse a .gui file from an in-memory buffer (for tests). The buffer
 * is not retained after return. */
int GUIDialog_LoadFromBuffer(GUIDialog *out, const char *buffer, size_t len);

void GUIDialog_Free(GUIDialog *dialog);

/* Find the first child widget whose .name matches `name` (case-insensitive).
 * Returns NULL if not found. */
GUIWidget *GUIDialog_FindByName(GUIDialog *dialog, const char *name);

#endif /* TAK_GUI_H */
