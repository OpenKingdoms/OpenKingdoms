#ifndef TAK_FONT_H
#define TAK_FONT_H

#include "tak_types.h"
#include <SDL.h>

/*
 * TAK font rendering.
 *
 * Fonts are GAFs with a single entry containing 256 frames — one per ASCII
 * code point. The matching .pcx supplies the palette; transparency index is
 * the GAF transparency (usually 9, same as sprites). A matching .tdf can
 * define a kerning/tracking table — not yet parsed.
 *
 * Load once; draw strings anywhere. No kerning yet, just per-character width.
 */

typedef struct Font Font;

/* Load a font from its GAF + PCX pair under the data dir (e.g.
 * "data/fonts/b_times new roman (100b)"). Returns NULL on failure. */
Font *Font_Load(const char *base_path, SDL_PixelFormat *rgba_format);

/* Free a font. */
void Font_Free(Font *f);

/* Draw a null-terminated ASCII string onto `dst` with its top-left at (x, y).
 * The top of tall glyphs lines up with `y`. */
void Font_DrawString(Font *f, SDL_Surface *dst, int x, int y, const char *s);

/* Measure a string's on-screen width in pixels (for centering / alignment). */
int Font_MeasureString(Font *f, const char *s);

/* Rough "line height" for the font — tallest glyph among printable ASCII. */
int Font_LineHeight(Font *f);

/* Where a string's ink lands relative to the y passed to Font_DrawString:
 * *out_top is the first painted row, *out_bottom one past the last.
 * Returns 0, or -1 when nothing would be painted. */
int Font_InkExtent(Font *f, const char *s, int *out_top, int *out_bottom);

#endif /* TAK_FONT_H */
