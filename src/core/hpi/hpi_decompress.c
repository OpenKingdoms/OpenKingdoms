/*
 * hpi_decompress.c — LZ77 + zlib decompression for HPI archives
 *
 * Uses miniz (vendored in third_party/) for zlib-compatible inflate.
 * This file pulls in the miniz implementation so the rest of the project
 * only needs to include miniz.h for declarations.
 */

#include "miniz.h"

#include "tak_types.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Forward declarations for static helpers */
static int lz77_decompress(const uint8_t *in, int in_size, uint8_t *out, int out_size);
static int zlib_decompress(const uint8_t *in, int in_size, uint8_t *out, int out_size);

// Decompress a single SQSH chunk. Reads the HPIChunk header from `chunk_data`,
// handles decryption if flagged, then decompresses into `out_buf`.
// Returns number of decompressed bytes, or -1 on error.
int hpi_decompress_chunk(
    const uint8_t *chunk_data,
    uint32_t chunk_total_size,
    uint8_t *out_buf,
    uint32_t out_buf_size
) {
    if (chunk_total_size < sizeof(HPIChunk)) return -1;

	HPIChunk chunk;
	memcpy(&chunk, chunk_data, sizeof(HPIChunk));

	if (chunk.marker != HPI_SQSH_MARKER) return -1;

	if (chunk.compressed_size + sizeof(HPIChunk) > chunk_total_size) return -1;
	if (chunk.decompressed_size > out_buf_size) return -1;

	uint8_t *comp_buf = (uint8_t*)tak_malloc(chunk.compressed_size);
	memcpy(comp_buf, chunk_data + sizeof(HPIChunk), chunk.compressed_size);

	// Compute and verify checksum - the checksum is the sum of all compressed bytes 
	// ... before decryption
	uint32_t checksum = 0;
	for (int i = 0; i < chunk.compressed_size; i++) {
		checksum += comp_buf[i];
		if (chunk.encrypted) {
			comp_buf[i] = (comp_buf[i] - i) ^ i;
		}
	}

	if (checksum != chunk.checksum) {
		tak_free(comp_buf);
		return -1;
	}

	int result;
	switch (chunk.compression_method) {
		// TODO
		case 1:
			result = lz77_decompress(comp_buf, chunk.compressed_size, out_buf, out_buf_size);
			break;
		case 2:
			result = zlib_decompress(comp_buf, chunk.compressed_size, out_buf, out_buf_size);
			break;
		default:
			result = -1;
			break;
	}

	tak_free(comp_buf);
	return result;
}

// LZ77 decompression (port directly from HPIDump.c LZ77Decompress, lines 203-260).
// Input: already-decrypted compressed data. Output: decompressed bytes.
static int lz77_decompress(const uint8_t *in, int in_size, uint8_t *out, int out_size)
{
    uint8_t window[4096];     // 4096-byte sliding window (circular buffer)
    int     win_pos  = 1;     // Current write position in window. Starts at 1, NOT 0.
                              //   (position 0 is reserved as the "end" sentinel)
    int     in_pos   = 0;     // Read cursor into `in`
    int     out_pos  = 0;     // Write cursor into `out`
    int     tag_bit  = 1;     // Current bit mask within the tag byte (starts at bit 0)
    int     tag_byte;         // The current tag byte

    // Read the first tag byte
    tag_byte = in[in_pos++];

    for (;;) {
        // --- Bounds check (not in original, but add for safety) ---
        if (in_pos >= in_size) break;
        if (out_pos >= out_size) break;

        if ((tag_bit & tag_byte) == 0) {
            // ============================================================
            // BIT IS 0 → LITERAL BYTE
            // ============================================================
            // Copy one byte from input directly to output AND to the window.

            out[out_pos++] = in[in_pos];
            window[win_pos] = in[in_pos];
            win_pos = (win_pos + 1) & 0xFFF;   // wrap at 4096
            in_pos++;
        }
        else {
            // ============================================================
            // BIT IS 1 → BACK-REFERENCE (offset + length)
            // ============================================================
            // Read 2 bytes as a little-endian uint16:
            //   - Upper 12 bits = offset into the sliding window
            //   - Lower 4 bits  = length - 2  (so actual length = value + 2)

            // Read the 16-bit value (little-endian):
            uint16_t pair = (uint16_t)in[in_pos] | ((uint16_t)in[in_pos + 1] << 8);
            in_pos += 2;

            int ref_pos = pair >> 4;           // window offset (12 bits)
            int length  = (pair & 0x0F) + 2;   // match length (2..17)

            // If ref_pos == 0, this is the END-OF-STREAM sentinel. Return.
            if (ref_pos == 0) {
                return out_pos;
            }

            // Copy `length` bytes from window[ref_pos..] to output,
            // also appending each byte to the window at win_pos.
            for (int i = 0; i < length; i++) {
                uint8_t byte = window[ref_pos];
                out[out_pos++] = byte;
                window[win_pos] = byte;
                ref_pos = (ref_pos + 1) & 0xFFF;   // wrap at 4096
                win_pos = (win_pos + 1) & 0xFFF;
            }
        }

        // Advance to the next bit in the tag byte
        tag_bit <<= 1;                // shift left (multiply by 2)

        if (tag_bit & 0x100) {
            // All 8 bits consumed → load the next tag byte
            tag_bit = 1;
            tag_byte = in[in_pos++];
        }
    }

    return out_pos;
}

// zlib decompression via miniz.
static int zlib_decompress(
    const uint8_t *in,
    int in_size,
    uint8_t *out,
    int out_size
) {
	mz_stream stream;
	memset(&stream, 0, sizeof(stream));
	stream.next_in = in;
	stream.avail_in = (mz_uint32)in_size;
	stream.next_out = out;
	stream.avail_out = (mz_uint32)out_size;

	int ret = mz_inflateInit(&stream);
	if (ret != MZ_OK) return -1;

	ret = mz_inflate(&stream, MZ_FINISH);
	if (ret != MZ_STREAM_END) {
		mz_inflateEnd(&stream);
		return -1;
	}

	mz_inflateEnd(&stream);
	return (int)stream.total_out;
}