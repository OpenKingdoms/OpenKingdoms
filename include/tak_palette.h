#ifndef TAK_PALETTE_H
#define TAK_PALETTE_H

#include "tak_types.h"
#include "SDL.h"

typedef struct {
    uint8_t r, g, b, pad;
} PaletteEntry;

typedef struct {
    PaletteEntry entries[256];
} Palette;

int Palette_Load(Palette *pal, const char *path);

int Palette_LoadPCX(Palette *pal, const char *path);

uint32_t Palette_IndexToRGBA(const Palette *pal, uint8_t index);

void Palette_BuildRGBATable(const Palette *pal, SDL_PixelFormat *fmt, uint32_t *table_out, uint8_t transparent_index);

/* Model textures: every index opaque from an RGBA32 table, and `magenta`
 * marks (128, 0, 128), the only texture key. */
void Palette_KeyTextures(const uint32_t *src, uint32_t *rgba, uint8_t *magenta);

/* A texel the art leaves clear, beside the 256 palette indices. */
#define PALETTE_TEXEL_CLEAR 0x100u

/* A texture texel's index, or PALETTE_TEXEL_CLEAR for magenta. */
uint16_t Palette_KeyTexel(uint8_t index, const uint8_t *magenta);

// Look up the correct palette PCX filename for a TAK GAF file.
// Returns e.g. "guipal.pcx" or NULL if the GAF has a matching .pcx next to it.
const char *Palette_LookupForGAF(const char *gaf_path);

// Alternative palette for faction GAFs (returns _textures if primary was _features).
const char *Palette_LookupForGAFAlt(const char *gaf_path);

#endif /* TAK_PALETTE_H */