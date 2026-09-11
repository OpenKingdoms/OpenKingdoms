/*
 * font.c -- GAF-based ASCII font rendering.
 *
 * A TAK font GAF has exactly one entry whose 256 frames correspond to
 * ASCII code points (frame index == character value). Each frame is a
 * small paletted sprite with its own width/height/hotspot. We decode
 * all printable glyphs (32..126) up front into RGBA and draw strings
 * by blitting them in sequence.
 */

#include "tak_font.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_blit.h"
#include "tak_memory.h"
#include <stdio.h>
#include <string.h>

#define FONT_ASCII_FIRST 32
#define FONT_ASCII_LAST  126
#define FONT_ASCII_COUNT (FONT_ASCII_LAST - FONT_ASCII_FIRST + 1)

struct Font {
    GAFFile *gaf;
    uint32_t rgba_table[256];

    uint32_t *glyph_pixels[FONT_ASCII_COUNT];
    int glyph_w[FONT_ASCII_COUNT];
    int glyph_h[FONT_ASCII_COUNT];
    int glyph_oy[FONT_ASCII_COUNT];  /* hotspot Y — vertical baseline offset */

    int max_h;
    int max_oy;
};

Font *Font_Load(const char *base_path, SDL_PixelFormat *rgba_format) {
    char gaf_path[256], pcx_path[256];
    snprintf(gaf_path, sizeof(gaf_path), "%s.gaf", base_path);
    snprintf(pcx_path, sizeof(pcx_path), "%s.pcx", base_path);

    Font *f = (Font *)tak_malloc(sizeof(Font));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));

    if (GAF_Open(&f->gaf, gaf_path) != 0) {
        fprintf(stderr, "Font_Load: GAF open failed: %s\n", gaf_path);
        tak_free(f);
        return NULL;
    }

    Palette pal;
    if (Palette_LoadPCX(&pal, pcx_path) != 0) {
        fprintf(stderr, "Font_Load: PCX open failed: %s\n", pcx_path);
        /* Fall back to greyscale ramp so something still renders. */
        for (int i = 0; i < 256; i++) {
            pal.entries[i].r = pal.entries[i].g = pal.entries[i].b = (uint8_t)i;
            pal.entries[i].pad = 0;
        }
    }
    Palette_BuildRGBATable(&pal, rgba_format, f->rgba_table, 9);

    /* Font GAF has exactly one entry; its frames are indexed by ASCII. */
    if (f->gaf->num_entries < 1) {
        fprintf(stderr, "Font_Load: empty GAF %s\n", gaf_path);
        Font_Free(f);
        return NULL;
    }
    uint32_t entry_off = *(uint32_t *)(f->gaf->data + 12);

    for (int c = FONT_ASCII_FIRST; c <= FONT_ASCII_LAST; c++) {
        int i = c - FONT_ASCII_FIRST;
        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(f->gaf, entry_off, c, &fh) != 0 || !fh) continue;
        if (fh->width == 0 || fh->height == 0) continue;

        f->glyph_pixels[i] = GAF_DecodeFrameRGBA(f->gaf, fh, f->rgba_table);
        if (!f->glyph_pixels[i]) continue;
        f->glyph_w[i] = fh->width;
        f->glyph_h[i] = fh->height;
        f->glyph_oy[i] = fh->offset_y;

        if (fh->height > f->max_h)    f->max_h = fh->height;
        if (fh->offset_y > f->max_oy) f->max_oy = fh->offset_y;
    }

    return f;
}

void Font_Free(Font *f) {
    if (!f) return;
    for (int i = 0; i < FONT_ASCII_COUNT; i++) {
        if (f->glyph_pixels[i]) tak_free(f->glyph_pixels[i]);
    }
    if (f->gaf) GAF_Close(f->gaf);
    tak_free(f);
}

static int glyph_index(int c) {
    if (c < FONT_ASCII_FIRST || c > FONT_ASCII_LAST) return -1;
    return c - FONT_ASCII_FIRST;
}

int Font_MeasureString(Font *f, const char *s) {
    if (!f || !s) return 0;
    int w = 0, widest = 0;
    for (; *s; s++) {
        /* A newline starts a fresh line: report the widest one. */
        if (*s == '\n') { if (w > widest) widest = w; w = 0; continue; }
        int i = glyph_index((unsigned char)*s);
        if (i < 0) { w += 4; continue; }    /* unknown char → small gap */
        int gw = f->glyph_w[i];
        if (gw <= 0) { w += 4; continue; }  /* non-printable (e.g. space) */
        w += gw;
    }
    return w > widest ? w : widest;
}

int Font_LineHeight(Font *f) { return f ? f->max_h : 0; }

int Font_InkExtent(Font *f, const char *s, int *out_top, int *out_bottom) {
    if (!f || !s) return -1;
    int top = 0, bottom = 0, any = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int i = glyph_index(c);
        if (i < 0 || !f->glyph_pixels[i] || f->glyph_w[i] == 0 || c == ' ') continue;
        int t = f->max_oy - f->glyph_oy[i];
        int b = t + f->glyph_h[i];
        if (!any || t < top) top = t;
        if (!any || b > bottom) bottom = b;
        any = 1;
    }
    if (!any) return -1;
    if (out_top) *out_top = top;
    if (out_bottom) *out_bottom = bottom;
    return 0;
}

void Font_DrawString(Font *f, SDL_Surface *dst, int x, int y, const char *s) {
    if (!f || !dst || !s) return;
    int pen_x = x, pen_y = y;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') { pen_x = x; pen_y += f->max_h; continue; }
        int i = glyph_index(c);
        if (i < 0 || !f->glyph_pixels[i] || f->glyph_w[i] == 0) {
            pen_x += 4;  /* unknown */
            continue;
        }
        /* The space-character frame in this font still has non-empty
         * subframes (a baseline underline/shadow used for kerning/layout).
         * Don't render it — just advance by its nominal width. */
        if (c == ' ') {
            pen_x += f->glyph_w[i];
            continue;
        }
        int draw_y = pen_y + (f->max_oy - f->glyph_oy[i]);
        Blit_RGBA(dst, pen_x, draw_y,
                  f->glyph_pixels[i], f->glyph_w[i], f->glyph_h[i]);
        pen_x += f->glyph_w[i];
    }
}
