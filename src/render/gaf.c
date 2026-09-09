/*
 * gaf.c -- GAF (Graphic Archive Format) sprite loader
 *
 * Loads and decodes TAK Kingdoms GAF sprite files. GAF files contain
 * named animation sequences, each with multiple frames of RLE-compressed
 * 8-bit paletted pixel data.
 *
 * NOTE: TAK's RLE scheme is DIFFERENT from TA's. The control byte uses
 * the low 2 bits as flags:
 *   bit0=1:         transparent skip,  count = byte >> 1
 *   bit0=0, bit1=0: literal pixels,    count = (byte >> 2) + 1
 *   bit0=0, bit1=1: repeat color,      count = (byte >> 2) + 1
 */

#include "tak_gaf.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/*
 * GAF_Open -- Load a GAF file via VFS and validate the header.
 *
 * Reads the entire file into memory, checks the version magic (0x00010100),
 * and populates a GAFFile handle. The raw file data stays in memory
 * so frame headers can be accessed as direct pointers into the buffer.
 */
int GAF_Open(GAFFile **out, const char *path) {
    uint32_t gaf_size = 0;
    uint8_t *gaf_buffer = NULL;
    if (VFS_ReadFile(path, (void**)&gaf_buffer, &gaf_size) != 0 || !gaf_buffer || gaf_size < sizeof(GAFHeader)) {
        fprintf(stderr, "Unable to read gaf file at: %s\n", path);
        if (gaf_buffer) tak_free(gaf_buffer);
        return -1;
    }
    uint32_t versions = *(uint32_t*)(gaf_buffer);
    if (versions != 0x00010100) {
        fprintf(stderr, "Invalid gaf file version at: %s\n", path);
        tak_free(gaf_buffer);
        return -1;
    }

    uint32_t num_entries = *(uint32_t*)(gaf_buffer + 4);

    GAFFile *gaf = (GAFFile*)tak_malloc(sizeof(GAFFile));
    if (!gaf) {
        fprintf(stderr, "Unable to allocate memory for gaf struct at: %s\n", path);
        tak_free(gaf_buffer);
        return -1;
    }

    gaf->num_entries = num_entries;
    gaf->data_size = gaf_size;
    gaf->data = gaf_buffer;

    *out = gaf;
    return 0;
}

/* GAF_Close -- Free the GAF file data and handle. */
void GAF_Close(GAFFile *gaf) {
    if (gaf) {
        if (gaf->data) tak_free(gaf->data);

        tak_free(gaf);
    }
}


/*

GAF Header (12 bytes at offset 0):
  Offset  Size  Field
  0x00    2     uint16 version     (always 0x0001)
  0x02    2     uint16 subversion  (always 0x0001)
  0x04    4     uint32 num_entries (number of animation sequences)
  0x08    4     uint32 reserved    (always 0)

  Validation: the first 4 bytes must be 00 01 01 00 in the file. That's version=1, subversion=1 as two uint16 LE values.
  The combined uint32 reads as 0x00010100.

  Entry Pointer Table (immediately after header at offset 0x0C):
  4 bytes per entry: uint32 absolute file offset to that entry's header

  So for num_entries sequences, you read num_entries * 4 bytes starting at offset 12.

  Entry Header (40 bytes at the pointed-to offset):
  Offset  Size  Field
  0x00    2     uint16  num_frames
  0x02    2     uint16  unknown1
  0x04    4     uint32  unknown2
  0x08    32    char[]  name (null-terminated, zero-padded)

  The name is what you match against — e.g. "MainBG", "SingleMachine0", "ExitButton".

  Frame Pointer Table (immediately after the entry header, num_frames * 8 bytes):
  Per frame:
    4 bytes: uint32 absolute offset to frame header
    4 bytes: uint32 unknown (often 0x02 or 0x0A)

  Frame Header (20 bytes at the pointed-to offset):
  Offset  Size  Field
  0x00    2     uint16  width
  0x02    2     uint16  height
  0x04    2     int16   offset_x (hotspot X)
  0x06    2     int16   offset_y (hotspot Y)
  0x08    1     uint8   transparency_index (usually 9)
  0x09    1     uint8   compressed (1 = RLE, 0 = raw)
  0x0A    2     uint16  subframes (0 for simple frames)
  0x0C    4     uint32  unknown
  0x10    4     uint32  pixel_data_offset (absolute file offset to RLE data)


*/

/*
 * GAF_FindSequence -- Find an animation sequence by name.
 *
 * Walks the entry pointer table at data+12 and compares each entry's
 * 32-byte name field (case-insensitive). Returns the absolute byte offset
 * to the matching EntryHeader, or -1 if not found.
 */
int GAF_FindSequence(GAFFile *gaf, const char *name) {
    if (!gaf) {
        return -1;
    }
    // Walk the entry pointer table at data + 12.
    uint32_t num_entries = gaf->num_entries;
    uint8_t *entry_pointer_table = gaf->data + 12;
    for(size_t i = 0; i < num_entries; i++) {
        // For each entry, read the uint32 offset, jump to data + offset,
        // read the 32-byte name starting at byte 8.
        uint32_t header_offset = *(uint32_t*)(entry_pointer_table + i * 4);
        EntryHeader *curr_entry_header = (EntryHeader*)(gaf->data + header_offset);
        if (tak_stricmp(curr_entry_header->name, name) == 0) {
            return header_offset;
        }
    }

    return -1;
}

/*
 * GAF_GetFrameInfo -- Get a pointer to a specific frame's header.
 *
 * Navigates from an entry offset to the frame pointer table (40 bytes
 * after the entry header), then follows the frame pointer to the 20-byte
 * FrameHeader. The returned pointer points directly into the loaded file
 * buffer — no allocation needed.
 */
int GAF_GetFrameInfo(GAFFile *gaf, uint32_t entry_offset, int frame_index, FrameHeader **out) {
    if (!gaf) {
        return -1;
    }
    EntryHeader *entry_header = (EntryHeader*)(gaf->data + entry_offset);
    if (frame_index >= entry_header->num_frames) {
        return -1;
    }

    /* Frame pointer table: 8 bytes per frame (4B offset + 4B unknown) */
    uint8_t *frame_ptr_table = (uint8_t*)(gaf->data + entry_offset + 40);
    uint32_t frame_header_offset = *(uint32_t*)(frame_ptr_table + frame_index * 8);
    *out = (FrameHeader*)(gaf->data + frame_header_offset);
    return 0;
}

/*
 * GAF_DecodeFrame -- Decode a frame's RLE data into palette indices.
 *
 * Allocates a width*height buffer, fills it with the transparency index,
 * then walks the RLE scanline data writing visible pixels. Each scanline
 * is prefixed with a uint16 byte count; a count of 0 means the entire
 * line is transparent.
 *
 * Returns a newly allocated buffer of palette indices. Caller must free
 * with tak_free().
 */
uint8_t *GAF_DecodeFrame(GAFFile *gaf, const FrameHeader *frame) {
    if (!gaf || !frame) return NULL;

    // Cast to size_t to avoid potential uint16 overflow (e.g. 256x256 = 65536)
    uint8_t *palette_index_grid = (uint8_t*)tak_malloc((size_t)frame->width * frame->height);
    if (!palette_index_grid) return NULL;

    // Pre-fill with transparency so skipped pixels are correct
    memset(palette_index_grid, frame->transparency_index, (size_t)frame->width * frame->height);

    // Bounds check: pixel_data_offset must be within the file
    if (frame->pixel_data_offset >= gaf->data_size) {
        tak_free(palette_index_grid);
        return NULL;
    }

    // Composite frame: pixel_data_offset points to an array of `subframes`
    // uint32 pointers to child FrameHeaders. Decode each sub-frame and
    // stamp it into the parent buffer aligned by hotspot:
    //   subframe pixel (ox_sub, oy_sub) lands on parent pixel (ox, oy),
    //   so subframe top-left = (ox - ox_sub, oy - oy_sub).
    // Font glyphs use this: glyph-plus-outline in one composite frame.
    if (frame->subframes > 0) {
        if (frame->pixel_data_offset + (uint32_t)frame->subframes * 4u
            > gaf->data_size) {
            return palette_index_grid;  // bad offset — return transparent frame
        }
        uint32_t *sub_ptrs = (uint32_t *)(gaf->data + frame->pixel_data_offset);
        for (int s = 0; s < frame->subframes; s++) {
            uint32_t sub_off = sub_ptrs[s];
            if (sub_off + sizeof(FrameHeader) > gaf->data_size) continue;
            FrameHeader *sub = (FrameHeader *)(gaf->data + sub_off);
            uint8_t *sub_px = GAF_DecodeFrame(gaf, sub);
            if (!sub_px) continue;

            int dx = frame->offset_x - sub->offset_x;
            int dy = frame->offset_y - sub->offset_y;
            for (int sy = 0; sy < sub->height; sy++) {
                int py = dy + sy;
                if (py < 0 || py >= frame->height) continue;
                for (int sx = 0; sx < sub->width; sx++) {
                    int px = dx + sx;
                    if (px < 0 || px >= frame->width) continue;
                    uint8_t v = sub_px[sy * sub->width + sx];
                    if (v == sub->transparency_index) continue;
                    palette_index_grid[py * frame->width + px] = v;
                }
            }
            tak_free(sub_px);
        }
        return palette_index_grid;
    }

    uint8_t *pixel_data_offset = gaf->data + frame->pixel_data_offset;
    size_t total_pixels = (size_t)frame->width * frame->height;

    if (frame->compressed == 0) {
        // Uncompressed: raw palette indices, width*height bytes
        size_t available = gaf->data_size - frame->pixel_data_offset;
        size_t to_copy = total_pixels;
        if (to_copy > available) to_copy = available;
        memcpy(palette_index_grid, pixel_data_offset, to_copy);
    } else {
        // RLE compressed: walk the data as a forward-only stream
        uint8_t *current_offset = pixel_data_offset;
        uint8_t *data_end = gaf->data + gaf->data_size;

        for (size_t row = 0; row < frame->height; row++) {
            // Bounds check before reading scanline header
            if (current_offset + 2 > data_end) break;

            // Each scanline starts with a uint16 byte count
            uint16_t rle_data_byte_count = *(uint16_t*)(current_offset);
            current_offset += 2;

            if (rle_data_byte_count == 0) {
                // Entire line is transparent — already filled, skip
                continue;
            }

            // Bounds check for line data
            if (current_offset + rle_data_byte_count > data_end) break;

            // Mark where this line's RLE data ends
            uint8_t *current_row_end = current_offset + rle_data_byte_count;

            uint16_t current_x_pixel = 0;
            // Process RLE control bytes until this line's data is consumed
            while (current_offset < current_row_end) {
                uint8_t curr_byte = *(current_offset);
                current_offset++;

                if (curr_byte & 1) {
                    // TRANSPARENT: skip count pixels (already filled)
                    current_x_pixel += curr_byte >> 1;
                } else if ((curr_byte & 3) == 0) {
                    // LITERAL: copy count pixel bytes from the stream
                    size_t read_count = (curr_byte >> 2) + 1;
                    // Clamp at the scanline end so a malformed stream
                    // can never write past the pixel buffer.
                    size_t max_write = current_x_pixel < frame->width
                                       ? (size_t)(frame->width - current_x_pixel) : 0;
                    size_t write_count = read_count < max_write ? read_count : max_write;
                    if (write_count > 0) {
                        memcpy(palette_index_grid + row * frame->width + current_x_pixel,
                               current_offset, write_count);
                    }
                    current_offset += read_count;
                    current_x_pixel += read_count;
                } else if ((curr_byte & 3) == 2) {
                    // REPEAT: fill count pixels with the next byte
                    size_t read_count = (curr_byte >> 2) + 1;
                    uint8_t color_byte = *current_offset;
                    current_offset++;
                    size_t max_write = current_x_pixel < frame->width
                                       ? (size_t)(frame->width - current_x_pixel) : 0;
                    size_t write_count = read_count < max_write ? read_count : max_write;
                    if (write_count > 0) {
                        memset(palette_index_grid + row * frame->width + current_x_pixel,
                               color_byte, write_count);
                    }
                    current_x_pixel += read_count;
                }
            }
        }
    }


    return palette_index_grid;
}

/*
 * GAF_DecodeFrameRGBA -- Decode a frame directly to 32-bit RGBA pixels.
 *
 * Calls GAF_DecodeFrame to get palette indices, then converts each index
 * to an RGBA pixel using the precomputed lookup table (from
 * Palette_BuildRGBATable). Caller must free with tak_free().
 */
uint32_t *GAF_DecodeFrameRGBA(GAFFile *gaf, const FrameHeader *frame, const uint32_t *rgba_table) {
    if (!gaf || !frame || !rgba_table) return NULL;

    uint8_t *palette_index_grid = GAF_DecodeFrame(gaf, frame);
    if (!palette_index_grid) return NULL;

    size_t total_pixels = (size_t)frame->width * frame->height;
    uint32_t *rgba_frame = (uint32_t*)tak_malloc(total_pixels * sizeof(uint32_t));

    if (!rgba_frame) {
        tak_free(palette_index_grid);
        return NULL;
    }

    // Convert each palette index to RGBA via the lookup table.
    // Two transparency mechanisms apply:
    //   (1) frame->transparency_index — set by the artist in the frame
    //       header, used by GAF_DecodeFrame's pre-fill for "skipped"
    //       RLE runs (and authored that way in uncompressed frames).
    //   (2) Magenta colorkey (RGB 128,0,128) — the engine's hardcoded
    //       16-bit colorkey for SoftRenderer_BlitOpaque. The 90s sprite
    //       convention: artists used pure magenta for transparent
    //       backdrops in uncompressed sprites because no real art uses
    //       that exact color. Verified via probe_zhn_pixels:
    //       thirsspear1 is 70% palette[0x05] = magenta in zon_textures.
    //       Without this, the Zhon monarch's spear renders a solid
    //       purple cross over the model.
    const uint8_t  key     = frame->transparency_index;
    const uint32_t magenta = 0xFF800080u;  /* SDL_PIXELFORMAT_RGBA32: A=FF B=80 G=00 R=80 */
    for (size_t i = 0; i < total_pixels; i++) {
        uint8_t  idx  = palette_index_grid[i];
        uint32_t rgba = rgba_table[idx];
        rgba_frame[i] = (idx == key || rgba == magenta) ? 0x00000000u : rgba;
    }

    tak_free(palette_index_grid);
    return rgba_frame;
}

/*
 * TAF_DecodeFrameRGBA -- Decode a TAF truecolor frame to 32-bit RGBA.
 *
 * TAF frames are raw uncompressed 16-bit pixels (width * height * 2 bytes).
 * Supports both ARGB 1555 (compressed=5) and ARGB 4444 (compressed=4).
 * 0x0000 = fully transparent pixel.
 * Caller must free with tak_free().
 */
uint32_t *TAF_DecodeFrameRGBA(GAFFile *gaf, const FrameHeader *frame) {
    if (!gaf || !frame) return NULL;

    size_t total_pixels = (size_t)frame->width * frame->height;
    size_t data_size = total_pixels * 2;

    if (frame->pixel_data_offset + data_size > gaf->data_size) return NULL;

    uint16_t *src = (uint16_t *)(gaf->data + frame->pixel_data_offset);
    uint32_t *rgba = (uint32_t *)tak_malloc(total_pixels * sizeof(uint32_t));
    if (!rgba) return NULL;

    if (frame->compressed == TAF_FORMAT_1555) {
        /* ARGB 1555: bit 15 = alpha, bits 14-10 = R, bits 9-5 = G, bits 4-0 = B */
        for (size_t i = 0; i < total_pixels; i++) {
            uint16_t px = src[i];
            if (px == 0) {
                rgba[i] = 0x00000000;
            } else {
                uint8_t a = (px & 0x8000) ? 255 : 0;
                uint8_t r = (uint8_t)(((px >> 10) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((px >> 5) & 0x1F) * 255 / 31);
                uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
                /* Pack as ABGR for SDL_PIXELFORMAT_RGBA32 on little-endian */
                rgba[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                          ((uint32_t)g << 8) | (uint32_t)r;
            }
        }
    } else if (frame->compressed == TAF_FORMAT_4444) {
        /* ARGB 4444: bits 15-12 = A, bits 11-8 = R, bits 7-4 = G, bits 3-0 = B */
        for (size_t i = 0; i < total_pixels; i++) {
            uint16_t px = src[i];
            if (px == 0) {
                rgba[i] = 0x00000000;
            } else {
                uint8_t a = (uint8_t)(((px >> 12) & 0xF) * 17);  /* 0xF * 17 = 255 */
                uint8_t r = (uint8_t)(((px >> 8) & 0xF) * 17);
                uint8_t g = (uint8_t)(((px >> 4) & 0xF) * 17);
                uint8_t b = (uint8_t)((px & 0xF) * 17);
                rgba[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                          ((uint32_t)g << 8) | (uint32_t)r;
            }
        }
    } else {
        /* Unknown format */
        tak_free(rgba);
        return NULL;
    }

    return rgba;
}
