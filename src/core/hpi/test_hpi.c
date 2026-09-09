/*
 * test_hpi.c — Unit tests for HPI SQSH chunk decompression
 *
 * Tests the hpi_decompress_chunk() function using:
 *   - The ARMFLAK.TDF worked example from Hpi-fmt.txt (LZ77, encrypted)
 *   - Synthetic zlib-compressed chunks
 *   - Error handling (bad marker, bad checksum, truncated, bad method)
 *   - Non-encrypted chunks
 *
 * See PORTING_GUIDE.md §2.8 "Testing strategy" for the test plan.
 */

#include "test_framework.h"
#include "tak_hpi.h"
#include "tak_io.h"
#include "tak_memory.h"
#include "miniz.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

/* Declare the function under test (defined in hpi_decompress.c) */
extern int hpi_decompress_chunk(
    const uint8_t *chunk_data,
    uint32_t chunk_total_size,
    uint8_t *out_buf,
    uint32_t out_buf_size
);

/* ══════════════════════════════════════════════════════════════════════
 *  Test vector: ARMFLAK.TDF from Hpi-fmt.txt
 *
 *  19-byte HPIChunk header + 107 bytes encrypted LZ77 payload = 126 bytes.
 *  Decompresses to 257 bytes of TDF text starting with "[MENUENTRY1]\r\n\t{"
 * ══════════════════════════════════════════════════════════════════════ */

static const uint8_t armflak_chunk_raw[] = {
    /* HPIChunk header (19 bytes) */
    0x53,0x51,0x53,0x48,             /* marker = "SQSH" */
    0x02,                             /* version = 2 */
    0x01,                             /* compression_method = 1 (LZ77) */
    0x01,                             /* encrypted = 1 */
    0x6B,0x00,0x00,0x00,             /* compressed_size = 107 */
    0x01,0x01,0x00,0x00,             /* decompressed_size = 257 */
    0xFE,0x36,0x00,0x00,             /* checksum = 0x36FE */

    /* Encrypted LZ77 payload (107 bytes) */
    0x20,0x5B,0x51,0x49,0x4E,0x55,0x3C,0x0E,
    0x64,0x64,0x94,0x5D,0x49,0x5D,0x11,0x14,
    0x29,0x7B,0xD5,0x26,0x18,0x55,0x6E,0x75,
    0x64,0x54,0x34,0x41,0x79,0x6C,0x9C,0x71,
    0x81,0x83,0x8B,0x3B,0x59,0x49,0xCB,0x4D,
    0x43,0xD1,0x42,0x54,0x9A,0xA5,0xA8,0xAA,
    0xAF,0xB0,0xB8,0x64,0x61,0xAC,0x6D,0xB0,
    0xB1,0x82,0x72,0x34,0x79,0xB8,0xB0,0xBD,
    0xDD,0xA3,0x82,0x81,0xE8,0x86,0xAC,0x89,
    0x98,0x92,0xC2,0xCF,0x98,0xEB,0x9D,0xE0,
    0x56,0xBF,0xA2,0x6F,0xAB,0x5F,0xA8,0x96,
    0xB5,0xC3,0x9F,0xB8,0xEB,0xB9,0xBE,0x7D,
    0x4F,0xC7,0x5F,0xCE,0x2F,0xD1,0x4C,0xD1,
    0xD0,0xD2,0x90
};

/* Expected first 16 bytes of decompressed output */
static const char armflak_expected_prefix[] = "[MENUENTRY1]\r\n\t{";

/* Full 257-byte expected decompressed output (from Hpi-fmt.txt hex dump) */
static const uint8_t armflak_expected_full[257] = {
    0x5B,0x4D,0x45,0x4E,0x55,0x45,0x4E,0x54,0x52,0x59,0x31,0x5D,0x0D,0x0A,0x09,0x7B,
    0x0D,0x0A,0x09,0x55,0x4E,0x49,0x54,0x4D,0x45,0x4E,0x55,0x3D,0x41,0x52,0x4D,0x41,
    0x43,0x4B,0x3B,0x0D,0x0A,0x09,0x4D,0x45,0x4E,0x55,0x3D,0x33,0x3B,0x0D,0x0A,0x09,
    0x42,0x55,0x54,0x54,0x4F,0x4E,0x3D,0x33,0x3B,0x0D,0x0A,0x09,0x55,0x4E,0x49,0x54,
    0x4E,0x41,0x4D,0x45,0x3D,0x41,0x52,0x4D,0x46,0x4C,0x41,0x4B,0x3B,0x0D,0x0A,0x09,
    0x7D,0x0D,0x0A,0x0D,0x0A,0x5B,0x4D,0x45,0x4E,0x55,0x45,0x4E,0x54,0x52,0x59,0x32,
    0x5D,0x0D,0x0A,0x09,0x7B,0x0D,0x0A,0x09,0x55,0x4E,0x49,0x54,0x4D,0x45,0x4E,0x55,
    0x3D,0x41,0x52,0x4D,0x41,0x43,0x56,0x3B,0x0D,0x0A,0x09,0x4D,0x45,0x4E,0x55,0x3D,
    0x33,0x3B,0x0D,0x0A,0x09,0x42,0x55,0x54,0x54,0x4F,0x4E,0x3D,0x33,0x3B,0x0D,0x0A,
    0x09,0x55,0x4E,0x49,0x54,0x4E,0x41,0x4D,0x45,0x3D,0x41,0x52,0x4D,0x46,0x4C,0x41,
    0x4B,0x3B,0x0D,0x0A,0x09,0x7D,0x0D,0x0A,0x0D,0x0A,0x5B,0x4D,0x45,0x4E,0x55,0x45,
    0x4E,0x54,0x52,0x59,0x33,0x5D,0x0D,0x0A,0x09,0x7B,0x0D,0x0A,0x09,0x55,0x4E,0x49,
    0x54,0x4D,0x45,0x4E,0x55,0x3D,0x41,0x52,0x4D,0x41,0x43,0x41,0x3B,0x0D,0x0A,0x09,
    0x4D,0x45,0x4E,0x55,0x3D,0x33,0x3B,0x0D,0x0A,0x09,0x42,0x55,0x54,0x54,0x4F,0x4E,
    0x3D,0x33,0x3B,0x0D,0x0A,0x09,0x55,0x4E,0x49,0x54,0x4E,0x41,0x4D,0x45,0x3D,0x41,
    0x52,0x4D,0x46,0x4C,0x41,0x4B,0x3B,0x0D,0x0A,0x09,0x7D,0x0D,0x0A,0x0D,0x0A,0x0D,
    0x0A
};

/* ══════════════════════════════════════════════════════════════════════
 *  Helper: build a synthetic SQSH chunk with zlib compression
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Builds a valid HPIChunk + compressed payload in `out_buf`.
 * Returns total size (header + payload), or -1 on error.
 */
static int build_zlib_chunk(
    const uint8_t *plaintext, uint32_t plain_len,
    int encrypted,
    uint8_t *out_buf, uint32_t out_buf_size
) {
    /* Compress the plaintext with miniz */
    mz_ulong comp_bound = mz_compressBound(plain_len);
    uint8_t *comp_tmp = (uint8_t *)tak_malloc(comp_bound);
    if (!comp_tmp) return -1;

    mz_ulong comp_len = comp_bound;
    int ret = mz_compress(comp_tmp, &comp_len, plaintext, plain_len);
    if (ret != MZ_OK) { tak_free(comp_tmp); return -1; }

    uint32_t total = (uint32_t)(sizeof(HPIChunk) + comp_len);
    if (total > out_buf_size) { tak_free(comp_tmp); return -1; }

    /* Optionally encrypt: reverse of decrypt (data[i] = (data[i] - i) ^ i)
       Encrypt: data[i] = (data[i] ^ i) + i */
    if (encrypted) {
        for (mz_ulong i = 0; i < comp_len; i++) {
            comp_tmp[i] = (comp_tmp[i] ^ (uint8_t)i) + (uint8_t)i;
        }
    }

    /* Compute checksum on the (possibly encrypted) bytes —
       the HPI format checksums the on-disk bytes, not the decrypted ones */
    uint32_t checksum = 0;
    for (mz_ulong i = 0; i < comp_len; i++) {
        checksum += comp_tmp[i];
    }

    /* Build the HPIChunk header */
    HPIChunk hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.marker = HPI_SQSH_MARKER;
    hdr.version = 0x02;
    hdr.compression_method = 2;  /* zlib */
    hdr.encrypted = (uint8_t)encrypted;
    hdr.compressed_size = (uint32_t)comp_len;
    hdr.decompressed_size = plain_len;
    hdr.checksum = checksum;

    memcpy(out_buf, &hdr, sizeof(HPIChunk));
    memcpy(out_buf + sizeof(HPIChunk), comp_tmp, comp_len);

    tak_free(comp_tmp);
    return (int)total;
}


/* ══════════════════════════════════════════════════════════════════════
 *  Test 1: LZ77 round-trip with known ARMFLAK.TDF data
 * ══════════════════════════════════════════════════════════════════════ */

TEST(lz77_armflak_tdf_returns_257) {
    uint8_t out[512];
    int result = hpi_decompress_chunk(
        armflak_chunk_raw, sizeof(armflak_chunk_raw), out, sizeof(out));
    ASSERT_EQ_INT(257, result);
}

TEST(lz77_armflak_tdf_prefix_matches) {
    uint8_t out[512];
    int result = hpi_decompress_chunk(
        armflak_chunk_raw, sizeof(armflak_chunk_raw), out, sizeof(out));
    ASSERT(result >= 16);
    ASSERT(memcmp(out, armflak_expected_prefix, 16) == 0);
}

TEST(lz77_armflak_tdf_full_output_matches) {
    uint8_t out[512];
    int result = hpi_decompress_chunk(
        armflak_chunk_raw, sizeof(armflak_chunk_raw), out, sizeof(out));
    ASSERT_EQ_INT(257, result);
    ASSERT(memcmp(out, armflak_expected_full, 257) == 0);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 2: Error handling — bad marker
 * ══════════════════════════════════════════════════════════════════════ */

TEST(bad_marker_returns_error) {
    uint8_t corrupted[sizeof(armflak_chunk_raw)];
    memcpy(corrupted, armflak_chunk_raw, sizeof(armflak_chunk_raw));
    corrupted[0] = 0xFF;  /* corrupt the marker */

    uint8_t out[512];
    int result = hpi_decompress_chunk(
        corrupted, sizeof(corrupted), out, sizeof(out));
    ASSERT_EQ_INT(-1, result);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 3: Error handling — bad checksum
 * ══════════════════════════════════════════════════════════════════════ */

TEST(bad_checksum_returns_error) {
    uint8_t corrupted[sizeof(armflak_chunk_raw)];
    memcpy(corrupted, armflak_chunk_raw, sizeof(armflak_chunk_raw));
    /* Flip one byte in the payload (after the 19-byte header) */
    corrupted[19 + 50] ^= 0xFF;

    uint8_t out[512];
    int result = hpi_decompress_chunk(
        corrupted, sizeof(corrupted), out, sizeof(out));
    ASSERT_EQ_INT(-1, result);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 4: Error handling — truncated chunk
 * ══════════════════════════════════════════════════════════════════════ */

TEST(truncated_chunk_returns_error) {
    uint8_t out[512];
    /* Pass chunk_total_size smaller than header + compressed_size */
    int result = hpi_decompress_chunk(
        armflak_chunk_raw, sizeof(HPIChunk) + 10, out, sizeof(out));
    ASSERT_EQ_INT(-1, result);
}

TEST(too_small_for_header_returns_error) {
    uint8_t out[512];
    /* Pass chunk_total_size smaller than even the header */
    int result = hpi_decompress_chunk(
        armflak_chunk_raw, 5, out, sizeof(out));
    ASSERT_EQ_INT(-1, result);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 5: zlib path — synthetic round-trip
 * ══════════════════════════════════════════════════════════════════════ */

TEST(zlib_synthetic_round_trip) {
    const char *plaintext = "Hello from TA:Kingdoms! This is a zlib test string for HPI decompression.";
    uint32_t plain_len = (uint32_t)strlen(plaintext);

    uint8_t chunk_buf[1024];
    int chunk_size = build_zlib_chunk(
        (const uint8_t *)plaintext, plain_len, 0, chunk_buf, sizeof(chunk_buf));
    ASSERT(chunk_size > 0);

    uint8_t out[512];
    int result = hpi_decompress_chunk(chunk_buf, (uint32_t)chunk_size, out, sizeof(out));
    ASSERT_EQ_INT((int)plain_len, result);
    ASSERT(memcmp(out, plaintext, plain_len) == 0);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 6: Non-encrypted chunk (encrypted == 0)
 * ══════════════════════════════════════════════════════════════════════ */

TEST(non_encrypted_zlib_chunk) {
    const char *plaintext = "[UNITINFO]\r\n{\r\n\tUnitName=TestUnit;\r\n}\r\n";
    uint32_t plain_len = (uint32_t)strlen(plaintext);

    uint8_t chunk_buf[1024];
    int chunk_size = build_zlib_chunk(
        (const uint8_t *)plaintext, plain_len, 0, chunk_buf, sizeof(chunk_buf));
    ASSERT(chunk_size > 0);

    uint8_t out[512];
    int result = hpi_decompress_chunk(chunk_buf, (uint32_t)chunk_size, out, sizeof(out));
    ASSERT_EQ_INT((int)plain_len, result);
    ASSERT(memcmp(out, plaintext, plain_len) == 0);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test: Encrypted zlib chunk
 * ══════════════════════════════════════════════════════════════════════ */

TEST(encrypted_zlib_chunk) {
    const char *plaintext = "Encrypted zlib test data for HPI archives -- Aramon vs Veruna!";
    uint32_t plain_len = (uint32_t)strlen(plaintext);

    uint8_t chunk_buf[1024];
    int chunk_size = build_zlib_chunk(
        (const uint8_t *)plaintext, plain_len, 1, chunk_buf, sizeof(chunk_buf));
    ASSERT(chunk_size > 0);

    uint8_t out[512];
    int result = hpi_decompress_chunk(chunk_buf, (uint32_t)chunk_size, out, sizeof(out));
    ASSERT_EQ_INT((int)plain_len, result);
    ASSERT(memcmp(out, plaintext, plain_len) == 0);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test: Unknown compression method returns error
 * ══════════════════════════════════════════════════════════════════════ */

TEST(unknown_compression_method_returns_error) {
    uint8_t chunk_buf[1024];
    /* Build a valid zlib chunk, then patch the compression method to 99 */
    const char *plaintext = "test";
    int chunk_size = build_zlib_chunk(
        (const uint8_t *)plaintext, 4, 0, chunk_buf, sizeof(chunk_buf));
    ASSERT(chunk_size > 0);

    /* Patch compression_method field (offset 5 in HPIChunk) */
    chunk_buf[5] = 99;

    uint8_t out[512];
    int result = hpi_decompress_chunk(chunk_buf, (uint32_t)chunk_size, out, sizeof(out));
    ASSERT_EQ_INT(-1, result);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test: Output buffer too small returns error
 * ══════════════════════════════════════════════════════════════════════ */

TEST(output_buffer_too_small_returns_error) {
    uint8_t out[4];  /* way too small for 257 bytes */
    int result = hpi_decompress_chunk(
        armflak_chunk_raw, sizeof(armflak_chunk_raw), out, sizeof(out));
    ASSERT_EQ_INT(-1, result);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Synthetic archive helpers for public-API tests
 *
 *  HPI_OpenArchive / HPI_CloseArchive touch the filesystem and need a
 *  real HPI fixture, so they're not exercised here. The in-memory API
 *  (HPI_FileExists, HPI_GetEntryCount, HPI_GetVersion, HPI_ListFiles)
 *  only needs records + a count, so we build a mirror of the internal
 *  HPIArchive struct and hand it to the public API as an opaque handle.
 *
 *  NOTE: The mirror MUST stay in sync with the definition in hpi.c.
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct HPIArchiveInternal {
    tak_file_t handle;
    HPIVersion version;
    HPIFileRecord *records;
    unsigned int record_count;
    char *decrypt_key;
} HPIArchiveInternal;

/* compare_record is defined in hpi.c (non-static) so we can sort the
   synthetic records with the same comparator HPI_FileExists uses. */
extern int compare_record(const void *a, const void *b);

static const char *FAKE_PATHS[] = {
    "units/aramon/acolyte.fbi",
    "units/aramon/knight.fbi",
    "units/aramon/king.fbi",
    "units/veruna/monk.fbi",
    "units/veruna/amphibian.fbi",
    "units/zhon/beast.fbi",
    "weapons/arrow.tdf",
    "weapons/fireball.tdf",
    "anims/buildpic/araacolyte.jpg",
    "gamedata/sidedata.tdf",
};
#define FAKE_PATHS_COUNT ((int)(sizeof(FAKE_PATHS) / sizeof(FAKE_PATHS[0])))

static void build_fake_archive(HPIArchiveInternal *out, const char **paths, int n) {
    out->handle = TAK_INVALID_FILE;
    out->version.marker = HPI_MAGIC;
    out->version.version = HPI_VERSION_V2;
    out->decrypt_key = NULL;
    out->record_count = (unsigned int)n;
    if (n <= 0) {
        out->records = NULL;
        return;
    }
    out->records = (HPIFileRecord *)tak_calloc((size_t)n, sizeof(HPIFileRecord));
    for (int i = 0; i < n; i++) {
        out->records[i].path = tak_strdup(paths[i]);
        out->records[i].data_offset = 0;
        out->records[i].decompressed_size = 0;
        out->records[i].compressed_size = 0;
        out->records[i].compression = 0;
    }
    qsort(out->records, (size_t)n, sizeof(HPIFileRecord), compare_record);
}

static void free_fake_archive(HPIArchiveInternal *a) {
    if (!a || !a->records) return;
    for (unsigned int i = 0; i < a->record_count; i++) {
        tak_free(a->records[i].path);
    }
    tak_free(a->records);
    a->records = NULL;
    a->record_count = 0;
}

static void free_path_list(char **paths, int count) {
    if (!paths) return;
    for (int i = 0; i < count; i++) tak_free(paths[i]);
    tak_free(paths);
}

/* ══════════════════════════════════════════════════════════════════════
 *  HPI_GetEntryCount
 * ══════════════════════════════════════════════════════════════════════ */

TEST(entry_count_returns_number_of_records) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    unsigned int count = HPI_GetEntryCount((HPIArchive *)&a);
    free_fake_archive(&a);
    ASSERT_EQ_INT(FAKE_PATHS_COUNT, (int)count);
}

TEST(entry_count_null_archive_returns_zero) {
    ASSERT_EQ_INT(0, (int)HPI_GetEntryCount(NULL));
}

TEST(entry_count_empty_archive_returns_zero) {
    HPIArchiveInternal a;
    build_fake_archive(&a, NULL, 0);
    unsigned int count = HPI_GetEntryCount((HPIArchive *)&a);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, (int)count);
}

/* ══════════════════════════════════════════════════════════════════════
 *  HPI_GetVersion
 * ══════════════════════════════════════════════════════════════════════ */

TEST(get_version_returns_v2_for_v2_archive) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    uint32_t v = HPI_GetVersion((HPIArchive *)&a);
    free_fake_archive(&a);
    ASSERT_EQ_INT((int)HPI_VERSION_V2, (int)v);
}

TEST(get_version_null_returns_uint32_max) {
    uint32_t v = HPI_GetVersion(NULL);
    ASSERT(v == 0xFFFFFFFFu);
}

/* ══════════════════════════════════════════════════════════════════════
 *  HPI_FileExists
 * ══════════════════════════════════════════════════════════════════════ */

TEST(file_exists_returns_1_for_existing_file) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    int r = HPI_FileExists((HPIArchive *)&a, "units/aramon/knight.fbi");
    free_fake_archive(&a);
    ASSERT_EQ_INT(1, r);
}

TEST(file_exists_returns_0_for_missing_file) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    int r = HPI_FileExists((HPIArchive *)&a, "units/aramon/dragon.fbi");
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
}

TEST(file_exists_is_case_insensitive) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    int r = HPI_FileExists((HPIArchive *)&a, "UNITS/ARAMON/KNIGHT.FBI");
    free_fake_archive(&a);
    ASSERT_EQ_INT(1, r);
}

TEST(file_exists_normalizes_backslashes) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    int r = HPI_FileExists((HPIArchive *)&a, "units\\aramon\\knight.fbi");
    free_fake_archive(&a);
    ASSERT_EQ_INT(1, r);
}

TEST(file_exists_null_archive_returns_0) {
    ASSERT_EQ_INT(0, HPI_FileExists(NULL, "units/aramon/knight.fbi"));
}

TEST(file_exists_null_path_returns_0) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    int r = HPI_FileExists((HPIArchive *)&a, NULL);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
}

/* ══════════════════════════════════════════════════════════════════════
 *  HPI_ListFiles
 *
 *  Tests encode the *intended* contract: returns 0 on success, fills
 *  out_paths with strdup'd matches, fills out_count with the match
 *  count. Pattern matching is case-insensitive and uses * / ? wildcards.
 * ══════════════════════════════════════════════════════════════════════ */

TEST(list_null_archive_returns_error) {
    char **paths = NULL;
    int count = 0;
    int r = HPI_ListFiles(NULL, "units/aramon/*", &paths, &count);
    ASSERT_EQ_INT(-1, r);
}

TEST(list_null_pattern_returns_error) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = NULL;
    int count = 0;
    int r = HPI_ListFiles((HPIArchive *)&a, NULL, &paths, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(-1, r);
}

TEST(list_null_out_paths_returns_error) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    int count = 0;
    int r = HPI_ListFiles((HPIArchive *)&a, "units/aramon/*", NULL, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(-1, r);
}

TEST(list_null_out_count_returns_error) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = NULL;
    int r = HPI_ListFiles((HPIArchive *)&a, "units/aramon/*", &paths, NULL);
    free_fake_archive(&a);
    ASSERT_EQ_INT(-1, r);
}

TEST(list_no_match_returns_zero_count_and_null_paths) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = (char **)0xdeadbeef;   /* sentinel; impl should clear to NULL */
    int count = -1;
    int r = HPI_ListFiles((HPIArchive *)&a, "nonexistent/*.xyz", &paths, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
    ASSERT_EQ_INT(0, count);
    ASSERT_NULL(paths);
}

TEST(list_subdirectory_glob_matches_three_aramon_units) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = NULL;
    int count = 0;
    int r = HPI_ListFiles((HPIArchive *)&a, "units/aramon/*", &paths, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
    ASSERT_EQ_INT(3, count);
    free_path_list(paths, count);
}

TEST(list_subdirectory_glob_matches_two_veruna_units) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = NULL;
    int count = 0;
    int r = HPI_ListFiles((HPIArchive *)&a, "units/veruna/*", &paths, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
    ASSERT_EQ_INT(2, count);
    free_path_list(paths, count);
}

TEST(list_weapons_glob_matches_two_tdfs) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = NULL;
    int count = 0;
    int r = HPI_ListFiles((HPIArchive *)&a, "weapons/*", &paths, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
    ASSERT_EQ_INT(2, count);
    free_path_list(paths, count);
}

TEST(list_result_paths_are_independently_allocated) {
    /* After free_fake_archive destroys the archive's own record paths,
       the returned paths must still be readable (they should be strdup'd copies). */
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = NULL;
    int count = 0;
    int r = HPI_ListFiles((HPIArchive *)&a, "units/aramon/*", &paths, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
    ASSERT(count > 0);
    ASSERT_NOT_NULL(paths);
    for (int i = 0; i < count; i++) {
        ASSERT_NOT_NULL(paths[i]);
        ASSERT(strlen(paths[i]) > 0);
    }
    free_path_list(paths, count);
}

TEST(list_case_insensitive_pattern_matches) {
    HPIArchiveInternal a;
    build_fake_archive(&a, FAKE_PATHS, FAKE_PATHS_COUNT);
    char **paths = NULL;
    int count = 0;
    int r = HPI_ListFiles((HPIArchive *)&a, "UNITS/ARAMON/*", &paths, &count);
    free_fake_archive(&a);
    ASSERT_EQ_INT(0, r);
    ASSERT_EQ_INT(3, count);
    free_path_list(paths, count);
}

/* ══════════════════════════════════════════════════════════════════════
 *  HPI_ReadFile (requires real game data)
 * ══════════════════════════════════════════════════════════════════════ */

static int has_game_data(void) {
    tak_file_t f = tak_file_open(TAK_GAME_DIR "/data.hpi");
    if (f == TAK_INVALID_FILE) return 0;
    tak_file_close(f);
    return 1;
}

TEST(readfile_open_close_only) {
    if (!has_game_data()) { printf("  [SKIP - no game data]\n"); return; }
    HPIArchive *archive = HPI_OpenArchive(TAK_GAME_DIR "/data.hpi");
    ASSERT_NOT_NULL(archive);
    HPI_CloseArchive(archive);
}

TEST(readfile_returns_valid_tdf_data) {
    if (!has_game_data()) { printf("  [SKIP - no game data]\n"); return; }
    HPIArchive *archive = HPI_OpenArchive(TAK_GAME_DIR "/data.hpi");
    ASSERT_NOT_NULL(archive);

    void *data = NULL;
    uint32_t size = 0;
    int r = HPI_ReadFile(archive, "gamedata/sidedata.tdf", &data, &size);
    ASSERT_EQ_INT(0, r);
    ASSERT_NOT_NULL(data);
    ASSERT(size > 0);

    /* The sidedata.tdf file should contain TDF text with known strings.
       Null-terminate it temporarily so we can use strstr. */
    char *text = (char *)tak_malloc(size + 1);
    memcpy(text, data, size);
    text[size] = '\0';
    int found = (strstr(text, "Aramon") != NULL || strstr(text, "aramon") != NULL);
    tak_free(text);
    ASSERT(found);

    HPI_FreeBuffer(data);
    HPI_CloseArchive(archive);
}

TEST(readfile_size_matches_record) {
    if (!has_game_data()) { printf("  [SKIP - no game data]\n"); return; }
    HPIArchive *archive = HPI_OpenArchive(TAK_GAME_DIR "/data.hpi");
    ASSERT_NOT_NULL(archive);

    void *data = NULL;
    uint32_t size = 0;
    int r = HPI_ReadFile(archive, "gamedata/sidedata.tdf", &data, &size);
    ASSERT_EQ_INT(0, r);
    ASSERT(size > 100);  /* sidedata.tdf is non-trivial */

    HPI_FreeBuffer(data);
    HPI_CloseArchive(archive);
}

TEST(readfile_same_file_twice_identical) {
    if (!has_game_data()) { printf("  [SKIP - no game data]\n"); return; }
    HPIArchive *archive = HPI_OpenArchive(TAK_GAME_DIR "/data.hpi");
    ASSERT_NOT_NULL(archive);

    void *data1 = NULL, *data2 = NULL;
    uint32_t size1 = 0, size2 = 0;
    HPI_ReadFile(archive, "gamedata/sidedata.tdf", &data1, &size1);
    HPI_ReadFile(archive, "gamedata/sidedata.tdf", &data2, &size2);

    ASSERT_EQ_INT((int)size1, (int)size2);
    ASSERT(memcmp(data1, data2, size1) == 0);

    HPI_FreeBuffer(data1);
    HPI_FreeBuffer(data2);
    HPI_CloseArchive(archive);
}

TEST(readfile_nonexistent_path_returns_error) {
    if (!has_game_data()) { printf("  [SKIP - no game data]\n"); return; }
    HPIArchive *archive = HPI_OpenArchive(TAK_GAME_DIR "/data.hpi");
    ASSERT_NOT_NULL(archive);

    void *data = NULL;
    uint32_t size = 0;
    int r = HPI_ReadFile(archive, "nonexistent/file.xyz", &data, &size);
    ASSERT_EQ_INT(-1, r);

    HPI_CloseArchive(archive);
}

TEST(readfile_null_archive_returns_error) {
    void *data = NULL;
    uint32_t size = 0;
    int r = HPI_ReadFile(NULL, "gamedata/sidedata.tdf", &data, &size);
    ASSERT_EQ_INT(-1, r);
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    setbuf(stdout, NULL);
    TEST_SUITE("LZ77 decompression (ARMFLAK.TDF)");
    RUN(lz77_armflak_tdf_returns_257);
    RUN(lz77_armflak_tdf_prefix_matches);
    RUN(lz77_armflak_tdf_full_output_matches);

    TEST_SUITE("Error handling");
    RUN(bad_marker_returns_error);
    RUN(bad_checksum_returns_error);
    RUN(truncated_chunk_returns_error);
    RUN(too_small_for_header_returns_error);
    RUN(output_buffer_too_small_returns_error);
    RUN(unknown_compression_method_returns_error);

    TEST_SUITE("zlib decompression");
    RUN(zlib_synthetic_round_trip);
    RUN(non_encrypted_zlib_chunk);
    RUN(encrypted_zlib_chunk);

    TEST_SUITE("HPI_GetEntryCount");
    RUN(entry_count_returns_number_of_records);
    RUN(entry_count_null_archive_returns_zero);
    RUN(entry_count_empty_archive_returns_zero);

    TEST_SUITE("HPI_GetVersion");
    RUN(get_version_returns_v2_for_v2_archive);
    RUN(get_version_null_returns_uint32_max);

    TEST_SUITE("HPI_FileExists");
    RUN(file_exists_returns_1_for_existing_file);
    RUN(file_exists_returns_0_for_missing_file);
    RUN(file_exists_is_case_insensitive);
    RUN(file_exists_normalizes_backslashes);
    RUN(file_exists_null_archive_returns_0);
    RUN(file_exists_null_path_returns_0);

    TEST_SUITE("HPI_ListFiles");
    RUN(list_null_archive_returns_error);
    RUN(list_null_pattern_returns_error);
    RUN(list_null_out_paths_returns_error);
    RUN(list_null_out_count_returns_error);
    RUN(list_no_match_returns_zero_count_and_null_paths);
    RUN(list_subdirectory_glob_matches_three_aramon_units);
    RUN(list_subdirectory_glob_matches_two_veruna_units);
    RUN(list_weapons_glob_matches_two_tdfs);
    RUN(list_result_paths_are_independently_allocated);
    RUN(list_case_insensitive_pattern_matches);

    TEST_SUITE("HPI_ReadFile");
    RUN(readfile_open_close_only);
    RUN(readfile_returns_valid_tdf_data);
    RUN(readfile_size_matches_record);
    RUN(readfile_same_file_twice_identical);
    RUN(readfile_nonexistent_path_returns_error);
    RUN(readfile_null_archive_returns_error);

    TEST_REPORT();
}
