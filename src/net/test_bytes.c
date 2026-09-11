/*
 * test_bytes.c: the bounded byte cursors every wire message is built
 * and parsed through.
 */

#include "test_framework.h"
#include "tak_bytes.h"

#include <stdio.h>

TEST(a_writer_lays_the_fields_out_little_endian) {
    uint8_t buf[32];
    TAK_ByteWriter w;
    TAK_BW_Init(&w, buf, sizeof(buf));
    TAK_BW_U8(&w, 0xa1u);
    TAK_BW_U16(&w, 0x1234u);
    TAK_BW_U32(&w, 0xdeadbeefu);
    TAK_BW_I32(&w, -16);
    ASSERT(TAK_BW_Ok(&w));
    ASSERT_EQ_INT(11, (int)w.len);
    const uint8_t want[11] = {
        0xa1,
        0x34, 0x12,
        0xef, 0xbe, 0xad, 0xde,
        0xf0, 0xff, 0xff, 0xff
    };
    ASSERT(memcmp(buf, want, sizeof(want)) == 0);
}

TEST(a_reader_gives_back_what_the_writer_put_in) {
    uint8_t buf[64];
    TAK_ByteWriter w;
    TAK_BW_Init(&w, buf, sizeof(buf));
    TAK_BW_U8(&w, 7);
    TAK_BW_U16(&w, 65535u);
    TAK_BW_U32(&w, 0x01020304u);
    TAK_BW_I32(&w, -2000000000);
    TAK_BW_U64(&w, 0x0123456789abcdefull);
    TAK_BW_Str8(&w, "kingdoms");
    ASSERT(TAK_BW_Ok(&w));

    TAK_ByteReader r;
    TAK_BR_Init(&r, buf, w.len);
    ASSERT_EQ_INT(7, (int)TAK_BR_U8(&r));
    ASSERT_EQ_INT(65535, (int)TAK_BR_U16(&r));
    ASSERT_EQ_INT(0x01020304, (int)TAK_BR_U32(&r));
    ASSERT_EQ_INT(-2000000000, TAK_BR_I32(&r));
    ASSERT(TAK_BR_U64(&r) == 0x0123456789abcdefull);
    char name[16];
    TAK_BR_Str8(&r, name, sizeof(name));
    ASSERT_EQ_STR("kingdoms", name);
    ASSERT(TAK_BR_Done(&r));
}

/* A writer over a short buffer stops storing, keeps counting, and says
 * so, which is how a caller sizes a message in one pass. */
TEST(a_full_writer_refuses_and_still_counts) {
    uint8_t buf[4];
    TAK_ByteWriter w;
    TAK_BW_Init(&w, buf, sizeof(buf));
    TAK_BW_U32(&w, 0xffffffffu);
    ASSERT(TAK_BW_Ok(&w));
    TAK_BW_U32(&w, 0x11223344u);
    ASSERT(!TAK_BW_Ok(&w));
    ASSERT_EQ_INT(8, (int)w.len);
    /* The refused field left the buffer alone. */
    ASSERT_EQ_INT(0xffffffff, (int)TAK_GetU32(buf));

    TAK_ByteWriter sizing;
    TAK_BW_Init(&sizing, NULL, 0);
    TAK_BW_U32(&sizing, 1);
    TAK_BW_U32(&sizing, 2);
    ASSERT_EQ_INT(8, (int)sizing.len);
}

/* A truncated message must not read past its end, and every read after
 * the first refusal stays refused. */
TEST(a_short_reader_refuses_and_stays_refused) {
    const uint8_t data[3] = { 1, 2, 3 };
    TAK_ByteReader r;
    TAK_BR_Init(&r, data, sizeof(data));
    ASSERT_EQ_INT(1, (int)TAK_BR_U8(&r));
    ASSERT(TAK_BR_Ok(&r));
    ASSERT_EQ_INT(2, (int)TAK_BR_Remaining(&r));
    ASSERT_EQ_INT(0, (int)TAK_BR_U32(&r));
    ASSERT(!TAK_BR_Ok(&r));
    ASSERT_EQ_INT(0, (int)TAK_BR_Remaining(&r));
    /* Still two bytes physically there, and still refused. */
    ASSERT_EQ_INT(0, (int)TAK_BR_U8(&r));
    ASSERT(!TAK_BR_Ok(&r));
    ASSERT(!TAK_BR_Done(&r));
}

/* Leftover bytes are as wrong as missing ones. */
TEST(a_reader_with_bytes_to_spare_is_not_done) {
    const uint8_t data[4] = { 9, 9, 9, 9 };
    TAK_ByteReader r;
    TAK_BR_Init(&r, data, sizeof(data));
    ASSERT_EQ_INT(9, (int)TAK_BR_U8(&r));
    ASSERT(TAK_BR_Ok(&r));
    ASSERT(!TAK_BR_Done(&r));
}

/* A length byte a hostile sender made too large must not overrun the
 * destination buffer. */
TEST(a_string_too_long_for_the_field_is_refused) {
    uint8_t buf[64];
    TAK_ByteWriter w;
    TAK_BW_Init(&w, buf, sizeof(buf));
    TAK_BW_Str8(&w, "a rather long display name");
    ASSERT(TAK_BW_Ok(&w));

    TAK_ByteReader r;
    TAK_BR_Init(&r, buf, w.len);
    char small[8];
    memset(small, 'x', sizeof(small));
    TAK_BR_Str8(&r, small, sizeof(small));
    ASSERT(!TAK_BR_Ok(&r));
    ASSERT_EQ_STR("", small);

    /* A length that runs past the data is refused too. */
    const uint8_t lying[3] = { 200, 'h', 'i' };
    TAK_ByteReader r2;
    TAK_BR_Init(&r2, lying, sizeof(lying));
    char out[256];
    memset(out, 'x', sizeof(out));
    TAK_BR_Str8(&r2, out, sizeof(out));
    ASSERT(!TAK_BR_Ok(&r2));
    ASSERT_EQ_STR("", out);
}

/* A read whose size overflows the cursor arithmetic must be refused
 * rather than wrapping into a pointer inside the buffer. */
TEST(a_huge_length_cannot_wrap_the_cursor) {
    const uint8_t data[8] = { 0 };
    TAK_ByteReader r;
    TAK_BR_Init(&r, data, sizeof(data));
    ASSERT_NULL(TAK_BR_Take(&r, (size_t)-1));
    ASSERT(!TAK_BR_Ok(&r));
}

int main(void) {
    TEST_SUITE("Bounded byte cursors");
    RUN(a_writer_lays_the_fields_out_little_endian);
    RUN(a_reader_gives_back_what_the_writer_put_in);
    RUN(a_full_writer_refuses_and_still_counts);
    RUN(a_short_reader_refuses_and_stays_refused);
    RUN(a_reader_with_bytes_to_spare_is_not_done);
    RUN(a_string_too_long_for_the_field_is_refused);
    RUN(a_huge_length_cannot_wrap_the_cursor);
    TEST_REPORT();
}
