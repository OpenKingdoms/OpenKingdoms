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
    ASSERT_EQ_INT(11, (int)TAK_BW_Len(&w));
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
    TAK_BW_Str(&w, "kingdoms", 16);
    ASSERT(TAK_BW_Ok(&w));

    TAK_ByteReader r;
    TAK_BR_Init(&r, buf, TAK_BW_Len(&w));
    ASSERT_EQ_INT(7, (int)TAK_BR_U8(&r));
    ASSERT_EQ_INT(65535, (int)TAK_BR_U16(&r));
    ASSERT_EQ_INT(0x01020304, (int)TAK_BR_U32(&r));
    ASSERT_EQ_INT(-2000000000, TAK_BR_I32(&r));
    ASSERT(TAK_BR_U64(&r) == 0x0123456789abcdefull);
    char name[16];
    TAK_BR_Str(&r, name, sizeof(name));
    ASSERT_EQ_STR("kingdoms", name);
    ASSERT(TAK_BR_Done(&r));
}

TEST(the_raw_accessors_agree_with_the_cursors) {
    uint8_t b[8];
    tak_put_u32(b, 0xa1b2c3d4u);
    ASSERT_EQ_INT(0xd4, b[0]);
    ASSERT_EQ_INT(0xa1, b[3]);
    ASSERT_EQ_INT((int)0xa1b2c3d4u, (int)tak_get_u32(b));
    tak_put_u64(b, 0x1122334455667788ull);
    ASSERT(tak_get_u64(b) == 0x1122334455667788ull);
    tak_put_u16(b, 0xbeefu);
    ASSERT_EQ_INT(0xbeef, (int)tak_get_u16(b));
}

/* A writer over a short buffer stops storing and says so, and writes
 * nothing past its end. */
TEST(a_full_writer_refuses_and_stays_refused) {
    uint8_t buf[6];
    memset(buf, 0x55, sizeof(buf));
    TAK_ByteWriter w;
    TAK_BW_Init(&w, buf, 4);
    TAK_BW_U32(&w, 0xffffffffu);
    ASSERT(TAK_BW_Ok(&w));
    TAK_BW_U32(&w, 0x11223344u);
    ASSERT(!TAK_BW_Ok(&w));
    ASSERT_EQ_INT(4, (int)TAK_BW_Len(&w));
    /* The refused field left the buffer and what lies past it alone. */
    ASSERT_EQ_INT((int)0xffffffffu, (int)tak_get_u32(buf));
    ASSERT_EQ_INT(0x55, buf[4]);
    /* Sticky: a field that would fit is refused too once one was not. */
    TAK_BW_U8(&w, 1);
    ASSERT(!TAK_BW_Ok(&w));
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

/* A text field is fixed width. Too long a name is cut to fit with its
 * terminator, and a hostile sender that fills the field with no
 * terminator still reads back terminated. */
TEST(a_text_field_never_runs_past_its_width) {
    uint8_t buf[32];
    TAK_ByteWriter w;
    TAK_BW_Init(&w, buf, sizeof(buf));
    TAK_BW_Str(&w, "a rather long display name", 16);
    ASSERT(TAK_BW_Ok(&w));
    ASSERT_EQ_INT(16, (int)TAK_BW_Len(&w));

    TAK_ByteReader r;
    TAK_BR_Init(&r, buf, 16);
    char out[16];
    TAK_BR_Str(&r, out, sizeof(out));
    ASSERT_EQ_STR("a rather long d", out);
    ASSERT(TAK_BR_Done(&r));

    uint8_t hostile[16];
    memset(hostile, 'x', sizeof(hostile));
    TAK_BR_Init(&r, hostile, sizeof(hostile));
    TAK_BR_Str(&r, out, sizeof(out));
    ASSERT_EQ_INT(15, (int)strlen(out));
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
    RUN(the_raw_accessors_agree_with_the_cursors);
    RUN(a_full_writer_refuses_and_stays_refused);
    RUN(a_short_reader_refuses_and_stays_refused);
    RUN(a_reader_with_bytes_to_spare_is_not_done);
    RUN(a_text_field_never_runs_past_its_width);
    RUN(a_huge_length_cannot_wrap_the_cursor);
    TEST_REPORT();
}
