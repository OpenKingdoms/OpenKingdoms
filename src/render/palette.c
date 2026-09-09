#include "tak_palette.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdio.h>
#include <string.h>
#include "SDL.h"

// Load a .pal file (256 entries of R,G,B,pad = 1024 bytes) via VFS.
int Palette_Load(Palette *pal, const char *path) {
    uint32_t size = 0;
    void *data = NULL;
    if (VFS_ReadFile(path, &data, &size) != 0 || !data) {
        fprintf(stderr, "Error opening %s\n", path);
        return -1;
    }

    if (size != 1024) {
        tak_free(data);
        return -1;
    }

    memcpy(pal->entries, data, 1024);
    tak_free(data);
    return 0;
}


uint32_t Palette_IndexToRGBA(const Palette *pal, uint8_t index) {
    PaletteEntry entry = pal->entries[index];
    return ((uint32_t)entry.r << 24) | ((uint32_t)entry.g << 16) | ((uint32_t)entry.b << 8) | 0xFF;
}

// Load a 256-color palette from the end of a PCX file via VFS.
// Each GAF sprite has a matching .pcx file with its palette
// (e.g. singlemachine.gaf uses singlemachine.pcx).
int Palette_LoadPCX(Palette *pal, const char *path) {
    uint32_t size = 0;
    void *data = NULL;
    if (VFS_ReadFile(path, &data, &size) != 0 || !data) {
        fprintf(stderr, "Error opening %s\n", path);
        return -1;
    }

    uint8_t *buf = (uint8_t *)data;

    // Need at least 769 bytes: 1 marker byte + 256*3 RGB
    if (size < 769) {
        tak_free(data);
        return -1;
    }

    // Check for 0x0C palette marker at file_size - 769
    if (buf[size - 769] != 0x0C) {
        tak_free(data);
        return -1;
    }

    // Read 256 RGB triplets (3 bytes each, no padding)
    uint8_t *rgb = buf + size - 768;
    for (int i = 0; i < 256; i++) {
        pal->entries[i].r = rgb[i * 3];
        pal->entries[i].g = rgb[i * 3 + 1];
        pal->entries[i].b = rgb[i * 3 + 2];
        pal->entries[i].pad = 0;
    }

    tak_free(data);
    return 0;
}

void Palette_BuildRGBATable(const Palette *pal, SDL_PixelFormat *fmt, uint32_t *table_out, uint8_t transparent_index) {
    for (size_t i = 0; i < 256; i++) {
        if (i == transparent_index) {
            table_out[i] = SDL_MapRGBA(fmt, 0, 0, 0, 0);
        } else {
            const PaletteEntry entry = pal->entries[i];
            table_out[i] = SDL_MapRGBA(fmt, entry.r, entry.g, entry.b, 255);
        }

    }
}
