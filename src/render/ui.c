/*
 * ui.c -- shared UI helpers (offscreen compositing surface + GAF loaders).
 *
 * The functions here were originally file-local in main_menu.c. Extracted
 * so that battle-setup, options, credits, in-game HUD, etc. all hit the
 * same code path instead of copy-pasting boilerplate.
 */

#include "tak_ui.h"
#include "tak_palette.h"
#include "tak_memory.h"
#include <stdio.h>

/* Single 640x480 RGBA32 offscreen surface, shared by every screen.
 * Created at program start (UI_Init), destroyed at exit (UI_Shutdown).
 * Screens read/write it directly via UI_Offscreen(). */
static SDL_Surface *g_offscreen = NULL;

int UI_Init(void) {
    if (g_offscreen) return 0;  /* already initialized */
    g_offscreen = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32,
                                                  SDL_PIXELFORMAT_RGBA32);
    if (!g_offscreen) {
        fprintf(stderr, "UI_Init: failed to create offscreen surface: %s\n",
                SDL_GetError());
        return -1;
    }
    return 0;
}

void UI_Shutdown(void) {
    if (g_offscreen) {
        SDL_FreeSurface(g_offscreen);
        g_offscreen = NULL;
    }
}

SDL_Surface *UI_Offscreen(void) { return g_offscreen; }

SDL_PixelFormat *UI_RGBAFormat(void) {
    return g_offscreen ? g_offscreen->format : NULL;
}

void UI_Present(TAK_Platform *platform) {
    /* Hand the composited canvas over to the platform so it can be
     * uploaded into the streaming texture. The actual window present
     * happens in TAK_Platform_Present at end of frame — this call is
     * cheap and idempotent, so screens can call it from their Tick
     * without worrying about double-presenting. */
    if (!g_offscreen || !platform) return;
    TAK_Platform_UpdateCanvas(platform, g_offscreen);
}

/* ── GAF + palette helpers ───────────────────────────────────────────── */

int UI_LoadGAFWithPalette(const char *gaf_path, const char *pcx_path,
                          GAFFile **out_gaf, uint32_t *rgba_table) {
    if (GAF_Open(out_gaf, gaf_path) != 0) {
        fprintf(stderr, "UI_LoadGAFWithPalette: failed to open %s\n", gaf_path);
        return -1;
    }

    Palette pal;
    int pal_ok = (Palette_LoadPCX(&pal, pcx_path) == 0);
    if (!pal_ok) {
        /* Sibling .pcx missing — fall back to the central palette
         * lookup (e.g. gui.gaf has no sibling, the legacy engine
         * binds it to palettes/guipal.pcx). */
        const char *resolved = Palette_LookupForGAF(gaf_path);
        if (resolved) {
            char fallback[160];
            snprintf(fallback, sizeof(fallback), "data/palettes/%s", resolved);
            pal_ok = (Palette_LoadPCX(&pal, fallback) == 0);
            if (pal_ok) {
                fprintf(stderr, "UI_LoadGAFWithPalette: %s -> %s (lookup)\n",
                        gaf_path, fallback);
            }
        }
    }
    if (!pal_ok) {
        fprintf(stderr, "UI_LoadGAFWithPalette: failed to load palette %s "
                        "(using greyscale ramp)\n", pcx_path);
        for (int i = 0; i < 256; i++) {
            pal.entries[i].r = pal.entries[i].g = pal.entries[i].b = (uint8_t)i;
            pal.entries[i].pad = 0;
        }
    }

    /* Transparency index 9 matches the GAF convention across TAK assets. */
    Palette_BuildRGBATable(&pal, UI_RGBAFormat(), rgba_table, 9);
    return 0;
}

uint32_t *UI_DecodeFrame(GAFFile *gaf, int entry_offset, int frame_index,
                         const uint32_t *rgba_table,
                         int *out_w, int *out_h) {
    FrameHeader *frame = NULL;
    if (GAF_GetFrameInfo(gaf, entry_offset, frame_index, &frame) != 0) return NULL;
    if (out_w) *out_w = frame->width;
    if (out_h) *out_h = frame->height;
    return GAF_DecodeFrameRGBA(gaf, frame, rgba_table);
}

uint32_t *UI_DecodeEntryIdle(GAFFile *gaf, int entry_idx,
                             const uint32_t *rgba_table,
                             int *out_w, int *out_h,
                             int *out_ox, int *out_oy) {
    /* The entry pointer table lives at GAF header offset 12, 4 bytes per
     * entry. Jump to the Nth entry and decode its frame 0. */
    uint32_t entry_off = *(uint32_t *)(gaf->data + 12 + entry_idx * 4);
    FrameHeader *frame = NULL;
    if (GAF_GetFrameInfo(gaf, entry_off, 0, &frame) != 0) return NULL;
    if (out_w)  *out_w  = frame->width;
    if (out_h)  *out_h  = frame->height;
    if (out_ox) *out_ox = frame->offset_x;
    if (out_oy) *out_oy = frame->offset_y;
    return GAF_DecodeFrameRGBA(gaf, frame, rgba_table);
}
