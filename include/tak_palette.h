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

// Look up the correct palette PCX filename for a TAK GAF file.
// Returns e.g. "guipal.pcx" or NULL if the GAF has a matching .pcx next to it.
const char *Palette_LookupForGAF(const char *gaf_path);

// Alternative palette for faction GAFs (returns _textures if primary was _features).
const char *Palette_LookupForGAFAlt(const char *gaf_path);

#endif /* TAK_PALETTE_H */