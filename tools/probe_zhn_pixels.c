/*
 * probe_zhn_pixels.c — Dump palette-index histograms for the Zhon
 * monarch's textures so we can figure out *which* indices are producing
 * the purple cross artifact in our 3D unit renderer.
 *
 * For each entry in textures/zonmonst11.gaf whose name starts with
 * "thirs" (the THIRSty hunter / Zhon monarch's textures: thirsspear,
 * thirsshield, thirsface, thirswing, etc.), this:
 *
 *   1. Decodes frame 0 to raw 8-bit palette indices via GAF_DecodeFrame.
 *   2. Builds a 256-bucket histogram.
 *   3. Loads zon_textures.pcx and prints palette[idx]=RGB for every
 *      non-zero bucket. Flags "purpley" indices (R>100, B>100, G<60).
 *   4. Echoes frame->transparency_index for the entry.
 *
 * We expect to learn: do the purple pixels live in one tight band
 * (like 0x80..0x8F) that the engine special-cased, or are they scattered?
 */

#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int is_purpley(uint8_t r, uint8_t g, uint8_t b) {
    return (r > 100 && b > 100 && g < 60);
}

int main(void) {
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    /* Load zon_textures.pcx and build an RGBA lookup. */
    Palette pal;
    if (Palette_LoadPCX(&pal, "data/palettes/zon_textures.pcx") != 0) {
        fprintf(stderr, "failed to load zon_textures.pcx\n");
        return 1;
    }
    /* Use the same RGBA format ui.c uses (SDL_PIXELFORMAT_RGBA32). */
    SDL_PixelFormat *fmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    uint32_t rgba_table[256];
    Palette_BuildRGBATable(&pal, fmt, rgba_table, 0);

    /* Print full palette so we can eyeball which index = which color. */
    printf("### zon_textures.pcx — full palette\n");
    for (int i = 0; i < 256; i++) {
        uint32_t c = rgba_table[i];
        uint8_t r = (uint8_t)(c & 0xFF);
        uint8_t g = (uint8_t)((c >> 8) & 0xFF);
        uint8_t b = (uint8_t)((c >> 16) & 0xFF);
        const char *flag = is_purpley(r, g, b) ? "  <-- PURPLEY" : "";
        if (i % 16 == 0) printf("\n  0x%02X: ", i);
        printf("%02X%02X%02X ", r, g, b);
        (void)flag;  /* shown below per-index in histogram */
    }
    printf("\n\n");

    /* Open the GAF. */
    GAFFile *gaf = NULL;
    if (GAF_Open(&gaf, "textures/zonmonst11.gaf") != 0) {
        fprintf(stderr, "failed to open zonmonst11.gaf\n");
        return 1;
    }

    printf("### zonmonst11.gaf — per-entry palette-index histograms (thirs* only)\n\n");

    for (uint32_t i = 0; i < gaf->num_entries; i++) {
        uint32_t entry_off = *(const uint32_t *)(gaf->data + 12 + i * 4);
        if (entry_off + 40 > gaf->data_size) continue;
        const char *name = (const char *)(gaf->data + entry_off + 8);
        if (!starts_with_ci(name, "thirs")) continue;

        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(gaf, entry_off, 0, &fh) != 0 || !fh) continue;

        uint8_t *grid = GAF_DecodeFrame(gaf, fh);
        if (!grid) continue;

        size_t total = (size_t)fh->width * fh->height;
        uint32_t hist[256] = {0};
        for (size_t p = 0; p < total; p++) hist[grid[p]]++;

        printf("--- %-32s  %dx%d  trans_idx=0x%02X  compressed=%d\n",
               name, fh->width, fh->height,
               fh->transparency_index, fh->compressed);

        /* Sort indices by count (descending) for readable output. */
        int order[256];
        for (int k = 0; k < 256; k++) order[k] = k;
        for (int a = 0; a < 256; a++) {
            for (int b = a + 1; b < 256; b++) {
                if (hist[order[b]] > hist[order[a]]) {
                    int t = order[a]; order[a] = order[b]; order[b] = t;
                }
            }
        }
        int shown = 0;
        for (int k = 0; k < 256 && shown < 20; k++) {
            int idx = order[k];
            if (hist[idx] == 0) break;
            uint32_t c = rgba_table[idx];
            uint8_t r = (uint8_t)(c & 0xFF);
            uint8_t g = (uint8_t)((c >> 8) & 0xFF);
            uint8_t bl = (uint8_t)((c >> 16) & 0xFF);
            const char *flag = is_purpley(r, g, bl) ? "  <-- PURPLEY" : "";
            const char *trans = (idx == fh->transparency_index) ? "  (trans)" : "";
            printf("    idx=0x%02X  count=%6u  rgb=(%3u,%3u,%3u)%s%s\n",
                   idx, hist[idx], r, g, bl, flag, trans);
            shown++;
        }
        printf("\n");
        tak_free(grid);
    }

    GAF_Close(gaf);
    SDL_FreeFormat(fmt);
    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
