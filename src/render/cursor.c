/*
 * cursor.c -- Custom cursor system for TAK-RE
 *
 * Loads the 20 cursor types from cursors.gaf using the cursors.pcx
 * palette, decodes each frame to RGBA, and creates SDL_Cursor objects
 * via SDL_CreateColorCursor. This gives zero-lag cursor rendering at
 * the OS compositor level (no input lag, works even during stutters).
 *
 * The original engine (the legacy reference lines 333094-333847) manages
 * cursors through a CursorMgr that software-blits a GAF sprite at the
 * mouse position. We achieve the same visual result with better UX by
 * using SDL's hardware cursor support.
 *
 * Animation: some cursors (e.g. hourglass) have multiple frames. A
 * per-frame countdown timer cycles through frames and calls
 * SDL_SetCursor to swap in the next frame's pre-built cursor.
 */

#include "tak_cursor.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_ui.h"
#include "tak_memory.h"

#include <stdio.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────── */

#define CURSOR_MAX_FRAMES   64  /* max frames per animation sequence */
#define CURSOR_ANIM_RATE     3  /* ticks between frame advances
                                   (original uses a per-frame countdown
                                   from TextureMgr; 3 gives ~10 fps
                                   cursor animation at 30 Hz tick rate,
                                   matching the original's feel) */

/* ── Cursor name table ───────────────────────────────────────────
 *
 * Maps CursorType enum values to the sequence names inside cursors.gaf.
 * Order MUST match the CursorType enum in tak_cursor.h.
 * Names from the legacy reference string table lines 4058-4471. */

static const char *s_cursor_names[CURSOR_COUNT] = {
    "cursornormal",       /* CURSOR_NORMAL     */
    "cursorhourglass",    /* CURSOR_HOURGLASS  */
    "cursorgrn",          /* CURSOR_GREEN      */
    "cursorred",          /* CURSOR_RED        */
    "cursorfindsite",     /* CURSOR_FINDSITE   */
    "cursorselect",       /* CURSOR_SELECT     */
    "cursormove",         /* CURSOR_MOVE       */
    "cursorunload",       /* CURSOR_UNLOAD     */
    "cursorload",         /* CURSOR_LOAD       */
    "cursorreclamate",    /* CURSOR_RECLAMATE  */
    "cursorrevive",       /* CURSOR_REVIVE     */
    "cursorteleport",     /* CURSOR_TELEPORT   */
    "cursorpickup",       /* CURSOR_PICKUP     */
    "cursorpatrol",       /* CURSOR_PATROL     */
    "cursorrepair",       /* CURSOR_REPAIR     */
    "cursordefend",       /* CURSOR_DEFEND     */
    "cursorcapture",      /* CURSOR_CAPTURE    */
    "cursortoofar",       /* CURSOR_TOOFAR     */
    "cursorairstrike",    /* CURSOR_AIRSTRIKE  */
    "cursorattack",       /* CURSOR_ATTACK     */
};

/* ── Per-type cursor data ────────────────────────────────────────── */

typedef struct CursorSequence {
    SDL_Cursor *frames[CURSOR_MAX_FRAMES];
    int         frame_count;
} CursorSequence;

static CursorSequence s_cursors[CURSOR_COUNT];
static CursorType     s_active_type  = CURSOR_NORMAL;
static int            s_active_frame = 0;
static int            s_anim_timer   = 0;
static int            s_initialized  = 0;

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Build an SDL_Cursor from RGBA pixels + hotspot. The surface is
   created, the cursor is built from it, and the surface is freed.
   Returns NULL on failure. */
static SDL_Cursor *build_sdl_cursor(const uint32_t *rgba, int w, int h,
                                     int hot_x, int hot_y) {
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
        0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;

    memcpy(surf->pixels, rgba, (size_t)w * h * 4);

    SDL_Cursor *cur = SDL_CreateColorCursor(surf, hot_x, hot_y);
    SDL_FreeSurface(surf);
    return cur;
}

/* Load all frames of a single cursor sequence from the GAF.
   Returns the number of frames loaded (0 on failure). */
static int load_cursor_sequence(GAFFile *gaf, const uint32_t *rgba_table,
                                 const char *seq_name, CursorSequence *out) {
    memset(out, 0, sizeof(*out));

    int entry_offset = GAF_FindSequence(gaf, seq_name);
    if (entry_offset < 0) {
        fprintf(stderr, "Cursor: sequence '%s' not found in cursors.gaf\n",
                seq_name);
        return 0;
    }

    /* Read num_frames from EntryHeader at the found offset */
    EntryHeader *entry = (EntryHeader *)(gaf->data + entry_offset);
    int num_frames = entry->num_frames;
    if (num_frames <= 0) return 0;
    if (num_frames > CURSOR_MAX_FRAMES) num_frames = CURSOR_MAX_FRAMES;

    for (int i = 0; i < num_frames; i++) {
        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(gaf, (uint32_t)entry_offset, i, &fh) != 0 || !fh) {
            fprintf(stderr, "Cursor: failed to get frame %d of '%s'\n",
                    i, seq_name);
            continue;
        }

        uint32_t *rgba = GAF_DecodeFrameRGBA(gaf, fh, rgba_table);
        if (!rgba) {
            fprintf(stderr, "Cursor: failed to decode frame %d of '%s'\n",
                    i, seq_name);
            continue;
        }

        /* Hotspot: the GAF frame header offset_x/offset_y define the
           anchor point. SDL_CreateColorCursor uses (hot_x, hot_y) as
           the pixel that aligns with the actual pointer position. */
        int hot_x = fh->offset_x;
        int hot_y = fh->offset_y;

        /* Clamp hotspot to valid range (SDL requires it within the surface) */
        if (hot_x < 0) hot_x = 0;
        if (hot_y < 0) hot_y = 0;
        if (hot_x >= fh->width)  hot_x = fh->width - 1;
        if (hot_y >= fh->height) hot_y = fh->height - 1;

        out->frames[out->frame_count] = build_sdl_cursor(
            rgba, fh->width, fh->height, hot_x, hot_y);
        tak_free(rgba);

        if (out->frames[out->frame_count]) {
            out->frame_count++;
        }
    }

    return out->frame_count;
}

/* ── Public API ──────────────────────────────────────────────────── */

int Cursor_Init(void) {
    if (s_initialized) return 0;

    /* Load cursors.gaf via VFS */
    GAFFile *gaf = NULL;
    if (GAF_Open(&gaf, "anims/cursors.gaf") != 0) {
        fprintf(stderr, "Cursor_Init: failed to open anims/cursors.gaf\n");
        return -1;
    }

    /* Load cursors.pcx palette and build RGBA lookup table */
    Palette pal;
    if (Palette_LoadPCX(&pal, "palettes/cursors.pcx") != 0) {
        fprintf(stderr, "Cursor_Init: failed to load palettes/cursors.pcx\n");
        GAF_Close(gaf);
        return -1;
    }

    SDL_PixelFormat *fmt = UI_RGBAFormat();
    if (!fmt) {
        fprintf(stderr, "Cursor_Init: UI not initialized (no pixel format)\n");
        GAF_Close(gaf);
        return -1;
    }

    uint32_t rgba_table[256];
    Palette_BuildRGBATable(&pal, fmt, rgba_table, 9);

    /* Load all 20 cursor types */
    int loaded = 0;
    for (int i = 0; i < CURSOR_COUNT; i++) {
        int n = load_cursor_sequence(gaf, rgba_table, s_cursor_names[i],
                                      &s_cursors[i]);
        if (n > 0) loaded++;
    }

    GAF_Close(gaf);

    fprintf(stderr, "Cursor_Init: loaded %d/%d cursor types\n",
            loaded, CURSOR_COUNT);

    if (loaded == 0) {
        fprintf(stderr, "Cursor_Init: no cursors loaded, falling back to OS cursor\n");
        return -1;
    }

    /* Activate the normal cursor */
    s_active_type  = CURSOR_NORMAL;
    s_active_frame = 0;
    s_anim_timer   = CURSOR_ANIM_RATE;
    s_initialized  = 1;

    if (s_cursors[CURSOR_NORMAL].frame_count > 0) {
        SDL_SetCursor(s_cursors[CURSOR_NORMAL].frames[0]);
    }

    return 0;
}

void Cursor_Shutdown(void) {
    for (int i = 0; i < CURSOR_COUNT; i++) {
        for (int f = 0; f < s_cursors[i].frame_count; f++) {
            if (s_cursors[i].frames[f]) {
                SDL_FreeCursor(s_cursors[i].frames[f]);
                s_cursors[i].frames[f] = NULL;
            }
        }
        s_cursors[i].frame_count = 0;
    }
    s_initialized = 0;
}

void Cursor_SetType(CursorType type) {
    if (!s_initialized) return;
    if (type < 0 || type >= CURSOR_COUNT) return;
    if (s_cursors[type].frame_count == 0) return;

    if (type != s_active_type) {
        s_active_type  = type;
        s_active_frame = 0;
        s_anim_timer   = CURSOR_ANIM_RATE;
        SDL_SetCursor(s_cursors[type].frames[0]);
    }
}

CursorType Cursor_GetType(void) {
    return s_active_type;
}

void Cursor_Tick(void) {
    if (!s_initialized) return;

    CursorSequence *seq = &s_cursors[s_active_type];
    if (seq->frame_count <= 1) return;  /* single-frame, no animation */

    s_anim_timer--;
    if (s_anim_timer <= 0) {
        s_anim_timer = CURSOR_ANIM_RATE;
        s_active_frame = (s_active_frame + 1) % seq->frame_count;
        SDL_SetCursor(seq->frames[s_active_frame]);
    }
}

void Cursor_SetVisible(int visible) {
    SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}
