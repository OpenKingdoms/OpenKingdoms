/*
 * test_io.c — Unit tests for the cross-platform tak_pread API
 */

#include "test_framework.h"
#include "tak_io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Test file: 512 bytes, pattern 0x00..0xFF repeated twice */
static const char *TEST_FILE = "test_io_tmp.bin";

static void create_test_file(void) {
    FILE *f = fopen(TEST_FILE, "wb");
    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;
    fwrite(data, 1, 256, f);
    fwrite(data, 1, 256, f);
    fclose(f);
}

static void cleanup_test_file(void) {
    remove(TEST_FILE);
}

/* ══════════════════════════════════════════════════════════════════
 *  File open / close
 * ══════════════════════════════════════════════════════════════════ */

TEST(open_valid_file) {
    create_test_file();
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);
    tak_file_close(f);
    cleanup_test_file();
}

TEST(open_nonexistent_returns_invalid) {
    tak_file_t f = tak_file_open("nonexistent_file_xyz_abc_123.bin");
    ASSERT(f == TAK_INVALID_FILE);
}

TEST(open_null_path_returns_invalid) {
    tak_file_t f = tak_file_open(NULL);
    ASSERT(f == TAK_INVALID_FILE);
}

TEST(close_invalid_handle_no_crash) {
    tak_file_close(TAK_INVALID_FILE);
    ASSERT(1); /* reaching here means no crash */
}

/* ══════════════════════════════════════════════════════════════════
 *  pread at various offsets
 * ══════════════════════════════════════════════════════════════════ */

TEST(pread_at_offset_zero) {
    create_test_file();
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);

    uint8_t buf[4];
    int64_t n = tak_pread(f, buf, 4, 0);
    ASSERT_EQ_INT(4, (int)n);
    ASSERT_EQ_INT(0x00, buf[0]);
    ASSERT_EQ_INT(0x01, buf[1]);
    ASSERT_EQ_INT(0x02, buf[2]);
    ASSERT_EQ_INT(0x03, buf[3]);

    tak_file_close(f);
    cleanup_test_file();
}

TEST(pread_at_middle_offset) {
    create_test_file();
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);

    uint8_t buf[4];
    int64_t n = tak_pread(f, buf, 4, 100);
    ASSERT_EQ_INT(4, (int)n);
    ASSERT_EQ_INT(100, buf[0]);
    ASSERT_EQ_INT(101, buf[1]);
    ASSERT_EQ_INT(102, buf[2]);
    ASSERT_EQ_INT(103, buf[3]);

    tak_file_close(f);
    cleanup_test_file();
}

TEST(pread_wraps_into_second_copy) {
    create_test_file();  /* 0x00..0xFF then 0x00..0xFF */
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);

    uint8_t buf[4];
    int64_t n = tak_pread(f, buf, 4, 256);  /* second copy starts here */
    ASSERT_EQ_INT(4, (int)n);
    ASSERT_EQ_INT(0x00, buf[0]);
    ASSERT_EQ_INT(0x01, buf[1]);
    ASSERT_EQ_INT(0x02, buf[2]);
    ASSERT_EQ_INT(0x03, buf[3]);

    tak_file_close(f);
    cleanup_test_file();
}

/* ══════════════════════════════════════════════════════════════════
 *  Independence: reads at different offsets don't affect each other
 * ══════════════════════════════════════════════════════════════════ */

TEST(pread_offsets_are_independent) {
    create_test_file();
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);

    uint8_t a[2], b[2], c[2];

    /* Read at 200, then 50, then 200 again */
    tak_pread(f, a, 2, 200);
    tak_pread(f, b, 2, 50);
    tak_pread(f, c, 2, 200);

    /* a and c should be identical (same offset, no position drift) */
    ASSERT_EQ_INT(a[0], c[0]);
    ASSERT_EQ_INT(a[1], c[1]);

    /* b should reflect offset 50 */
    ASSERT_EQ_INT(50, b[0]);
    ASSERT_EQ_INT(51, b[1]);

    tak_file_close(f);
    cleanup_test_file();
}

/* ══════════════════════════════════════════════════════════════════
 *  Edge cases
 * ══════════════════════════════════════════════════════════════════ */

TEST(pread_past_eof_returns_short) {
    create_test_file();  /* 512 bytes */
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);

    uint8_t buf[100];
    int64_t n = tak_pread(f, buf, 100, 500);
    /* Only 12 bytes remain at offset 500 in a 512-byte file */
    ASSERT_EQ_INT(12, (int)n);

    tak_file_close(f);
    cleanup_test_file();
}

TEST(pread_at_exact_eof_returns_zero) {
    create_test_file();  /* 512 bytes */
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);

    uint8_t buf[4];
    int64_t n = tak_pread(f, buf, 4, 512);
    /* At exact EOF, nothing to read */
    ASSERT(n == 0 || n == -1);  /* platform-dependent, both acceptable */

    tak_file_close(f);
    cleanup_test_file();
}

TEST(pread_invalid_handle_returns_error) {
    uint8_t buf[4];
    int64_t n = tak_pread(TAK_INVALID_FILE, buf, 4, 0);
    ASSERT_EQ_INT(-1, (int)n);
}

TEST(pread_null_buffer_returns_error) {
    create_test_file();
    tak_file_t f = tak_file_open(TEST_FILE);
    ASSERT(f != TAK_INVALID_FILE);

    int64_t n = tak_pread(f, NULL, 4, 0);
    ASSERT_EQ_INT(-1, (int)n);

    tak_file_close(f);
    cleanup_test_file();
}

/* ══════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════ */

int main(void) {
    TEST_SUITE("File open/close");
    RUN(open_valid_file);
    RUN(open_nonexistent_returns_invalid);
    RUN(open_null_path_returns_invalid);
    RUN(close_invalid_handle_no_crash);

    TEST_SUITE("pread at various offsets");
    RUN(pread_at_offset_zero);
    RUN(pread_at_middle_offset);
    RUN(pread_wraps_into_second_copy);

    TEST_SUITE("Offset independence");
    RUN(pread_offsets_are_independent);

    TEST_SUITE("Edge cases");
    RUN(pread_past_eof_returns_short);
    RUN(pread_at_exact_eof_returns_zero);
    RUN(pread_invalid_handle_returns_error);
    RUN(pread_null_buffer_returns_error);

    TEST_REPORT();
}
