#ifndef TAK_GAF_H
#define TAK_GAF_H

#include "tak_types.h"

typedef struct GAFHeader {
    uint16_t version;
    uint16_t subversion;
    uint32_t num_entries;
    uint32_t reserved;
} GAFHeader;

typedef struct EntryHeader {
    uint16_t num_frames;
    uint16_t unknown1;
    uint32_t unknown2;
    char name[32];

} EntryHeader;

typedef struct FrameHeader {
    uint16_t width;
    uint16_t height;
    int16_t offset_x;
    int16_t offset_y;
    uint8_t transparency_index;
    uint8_t compressed;
    int16_t subframes;
    uint32_t unknown;
    uint32_t pixel_data_offset;

} FrameHeader;

typedef struct GAFFile {
    uint8_t *data;
    uint32_t data_size;
    uint32_t num_entries;
} GAFFile;


int GAF_Open(GAFFile **out, const char *path);

void GAF_Close(GAFFile *gaf);

int GAF_FindSequence(GAFFile *gaf, const char *name);

int GAF_GetFrameInfo(GAFFile *gaf, uint32_t entry_offset, int frame_index, FrameHeader **out);

uint8_t *GAF_DecodeFrame(GAFFile *gaf, const FrameHeader *frame);

uint32_t *GAF_DecodeFrameRGBA(GAFFile *gaf, const FrameHeader *frame, const uint32_t *rgba_table);

/*
 * TAF (Truecolor Animation Format) support.
 *
 * TAF files use the same container as GAF (same header, entry table,
 * frame headers) but store 16-bit color pixels instead of 8-bit palette
 * indices. No palette needed.
 *
 * Frame header compressed field:
 *   4 = ARGB 4444 (4 bits each for A, R, G, B)
 *   5 = ARGB 1555 (1 bit alpha, 5 bits each for R, G, B)
 *
 * Pixel data is raw uncompressed: width * height * 2 bytes of uint16 values.
 * 0x0000 = fully transparent pixel.
 */

#define TAF_FORMAT_4444 4
#define TAF_FORMAT_1555 5

/* Decode a TAF frame directly to 32-bit RGBA. No palette needed.
 * Handles both ARGB 1555 and ARGB 4444 pixel formats automatically
 * based on the frame's compressed field. Caller must free with tak_free(). */
uint32_t *TAF_DecodeFrameRGBA(GAFFile *gaf, const FrameHeader *frame);

#endif /* TAK_GAF_H */