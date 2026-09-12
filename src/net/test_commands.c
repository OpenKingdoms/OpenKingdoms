/*
 * test_commands.c: the command codec, wire version 2.
 *
 * A client sends neither the seat nor the execution tick, so both are
 * absent from the bytes and stamped on arrival.
 */

#include "test_framework.h"
#include "tak_commands.h"
#include "tak_bytes.h"

#include <stdio.h>
#include <string.h>

static TAK_CommandBuffer g_buf_a;
static TAK_CommandBuffer g_buf_b;

TEST(a_command_lays_out_nineteen_bytes_and_its_units) {
    TAK_GameCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.seat = 2;               /* not on the wire */
    cmd.tick = 0x01020304u;     /* not on the wire */
    cmd.type = TAK_CMD_MOVE;
    cmd.unit_count = 2;
    cmd.target_x = -16;
    cmd.target_y = 0x11223344;
    cmd.target_unit_id = 0xaabbccddu;
    cmd.build_type_id = 0x1234u;
    cmd.arg = 0x5678u;
    cmd.unit_ids[0] = 0x01000002u;
    cmd.unit_ids[1] = 0x03000004u;

    uint8_t wire[64];
    size_t len = 0;
    ASSERT_EQ_INT(0, TAK_CommandSerialize(&cmd, wire, sizeof(wire), &len));
    ASSERT_EQ_INT(TAK_COMMAND_HEADER_BYTES + 8, (int)len);
    const uint8_t expected[TAK_COMMAND_HEADER_BYTES + 8] = {
        0x01,                                /* type */
        0x02, 0x00,                          /* unit_count */
        0xf0, 0xff, 0xff, 0xff,              /* target_x */
        0x44, 0x33, 0x22, 0x11,              /* target_y */
        0xdd, 0xcc, 0xbb, 0xaa,              /* target_unit_id */
        0x34, 0x12,                          /* build_type_id */
        0x78, 0x56,                          /* arg */
        0x02, 0x00, 0x00, 0x01,
        0x04, 0x00, 0x00, 0x03
    };
    ASSERT(memcmp(wire, expected, sizeof(expected)) == 0);

    TAK_GameCommand out;
    size_t used = 0;
    ASSERT_EQ_INT(0, TAK_CommandDeserialize(&out, wire, len, &used));
    ASSERT_EQ_INT((int)len, (int)used);
    ASSERT_EQ_INT(cmd.type, out.type);
    ASSERT_EQ_INT(cmd.unit_count, out.unit_count);
    ASSERT_EQ_INT(cmd.target_x, out.target_x);
    ASSERT_EQ_INT(cmd.target_y, out.target_y);
    ASSERT_EQ_INT((int)cmd.target_unit_id, (int)out.target_unit_id);
    ASSERT_EQ_INT(cmd.build_type_id, out.build_type_id);
    ASSERT_EQ_INT(cmd.arg, out.arg);
    ASSERT_EQ_INT((int)cmd.unit_ids[0], (int)out.unit_ids[0]);
    ASSERT_EQ_INT((int)cmd.unit_ids[1], (int)out.unit_ids[1]);
    /* What the client never sent comes back cleared. */
    ASSERT_EQ_INT(0, (int)out.seat);
    ASSERT_EQ_INT(0, (int)out.tick);
}

TEST(the_buffer_header_says_version_two) {
    TAK_CommandBuffer_Init(&g_buf_a, 0x0a0b0c0du);
    TAK_GameCommand a;
    memset(&a, 0, sizeof(a));
    a.type = TAK_CMD_PATROL;
    a.target_x = 128;
    a.target_y = 256;
    a.unit_count = 1;
    a.unit_ids[0] = 42;
    ASSERT_EQ_INT(0, TAK_CommandBuffer_Push(&g_buf_a, &a));

    TAK_GameCommand b = a;
    b.type = TAK_CMD_GUARD;
    b.target_unit_id = 99;
    b.unit_ids[0] = 43;
    ASSERT_EQ_INT(0, TAK_CommandBuffer_Push(&g_buf_a, &b));

    uint8_t wire[256];
    size_t len = 0;
    ASSERT_EQ_INT(0, TAK_CommandBufferSerialize(&g_buf_a, wire, sizeof(wire), &len));
    ASSERT_EQ_INT('T', wire[0]);
    ASSERT_EQ_INT('A', wire[1]);
    ASSERT_EQ_INT('K', wire[2]);
    ASSERT_EQ_INT(2, wire[3]);
    ASSERT_EQ_INT(2, TAK_COMMAND_WIRE_VERSION);
    ASSERT_EQ_INT(0x0a0b0c0d, (int)tak_get_u32(wire + 4));
    ASSERT_EQ_INT(2, (int)tak_get_u16(wire + 8));
    ASSERT_EQ_INT(TAK_COMMAND_BUFFER_HEADER_BYTES +
                  2 * (TAK_COMMAND_HEADER_BYTES + 4), (int)len);

    ASSERT_EQ_INT(0, TAK_CommandBufferDeserialize(&g_buf_b, wire, len));
    ASSERT_EQ_INT(0x0a0b0c0d, (int)g_buf_b.sequence);
    ASSERT_EQ_INT(2, g_buf_b.count);
    ASSERT_EQ_INT(TAK_CMD_PATROL, g_buf_b.commands[0].type);
    ASSERT_EQ_INT(TAK_CMD_GUARD, g_buf_b.commands[1].type);
    ASSERT_EQ_INT(42, (int)g_buf_b.commands[0].unit_ids[0]);
    ASSERT_EQ_INT(43, (int)g_buf_b.commands[1].unit_ids[0]);
    ASSERT_EQ_INT(99, (int)g_buf_b.commands[1].target_unit_id);

    /* The receiver stamps what the sender left out. */
    TAK_CommandBuffer_Stamp(&g_buf_b, 3, 1200);
    ASSERT_EQ_INT(3, (int)g_buf_b.commands[0].seat);
    ASSERT_EQ_INT(1200, (int)g_buf_b.commands[1].tick);
}

/* Every type version 2 adds survives a round trip, so no new type is
 * declared without a codec that carries it. */
TEST(every_command_type_round_trips) {
    for (unsigned t = TAK_CMD_MOVE; t < TAK_CMD_COUNT; t++) {
        ASSERT(TAK_CommandTypeIsValid(t));
        ASSERT(TAK_CommandTypeName(t)[0] != '?');
        TAK_GameCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = (uint8_t)t;
        cmd.target_x = (int32_t)(t * 7u) - 100;
        cmd.target_y = (int32_t)(t * 13u);
        cmd.target_unit_id = t + 900u;
        cmd.build_type_id = (uint16_t)(t + 5u);
        cmd.arg = (uint16_t)(t * 3u);
        if (TAK_CommandTypeTakesUnits(t)) {
            cmd.unit_count = 3;
            for (int i = 0; i < 3; i++) cmd.unit_ids[i] = t * 100u + (unsigned)i;
        }
        uint8_t wire[128];
        size_t len = 0;
        ASSERT_EQ_INT(0, TAK_CommandSerialize(&cmd, wire, sizeof(wire), &len));
        TAK_GameCommand out;
        size_t used = 0;
        ASSERT_EQ_INT(0, TAK_CommandDeserialize(&out, wire, len, &used));
        ASSERT_EQ_INT((int)len, (int)used);
        ASSERT_EQ_INT((int)t, (int)out.type);
        ASSERT_EQ_INT(cmd.target_x, out.target_x);
        ASSERT_EQ_INT(cmd.target_y, out.target_y);
        ASSERT_EQ_INT((int)cmd.target_unit_id, (int)out.target_unit_id);
        ASSERT_EQ_INT(cmd.build_type_id, out.build_type_id);
        ASSERT_EQ_INT(cmd.arg, out.arg);
        ASSERT_EQ_INT(cmd.unit_count, out.unit_count);
        for (int i = 0; i < cmd.unit_count; i++) {
            ASSERT_EQ_INT((int)cmd.unit_ids[i], (int)out.unit_ids[i]);
        }
    }
}

/* The version 2 types the plan named, each by the name the codec
 * reports, so a renumbering that drops one is caught here. */
TEST(version_two_carries_the_types_the_lobby_needs) {
    ASSERT_EQ_STR("build", TAK_CommandTypeName(TAK_CMD_BUILD));
    ASSERT_EQ_STR("factory-enqueue", TAK_CommandTypeName(TAK_CMD_FACTORY_ENQUEUE));
    ASSERT_EQ_STR("factory-dequeue", TAK_CommandTypeName(TAK_CMD_FACTORY_DEQUEUE));
    ASSERT_EQ_STR("factory-cancel", TAK_CommandTypeName(TAK_CMD_FACTORY_CANCEL));
    ASSERT_EQ_STR("rally", TAK_CommandTypeName(TAK_CMD_RALLY));
    ASSERT_EQ_STR("gate", TAK_CommandTypeName(TAK_CMD_GATE));
    ASSERT_EQ_STR("attack-ground", TAK_CommandTypeName(TAK_CMD_ATTACK_GROUND));
    ASSERT_EQ_STR("special-weapon", TAK_CommandTypeName(TAK_CMD_SPECIAL_WEAPON));
    ASSERT_EQ_STR("reclaim-feature", TAK_CommandTypeName(TAK_CMD_RECLAIM_FEATURE));
    ASSERT_EQ_STR("resurrect-feature", TAK_CommandTypeName(TAK_CMD_RESURRECT_FEATURE));
    ASSERT_EQ_STR("give-units", TAK_CommandTypeName(TAK_CMD_GIVE_UNITS));
    ASSERT_EQ_STR("alliance", TAK_CommandTypeName(TAK_CMD_ALLIANCE));
    ASSERT_EQ_STR("share-vision", TAK_CommandTypeName(TAK_CMD_SHARE_VISION));
    ASSERT_EQ_STR("share-units", TAK_CommandTypeName(TAK_CMD_SHARE_UNITS));
    ASSERT_EQ_STR("share-mana", TAK_CommandTypeName(TAK_CMD_SHARE_MANA));
    ASSERT_EQ_STR("mana-gift", TAK_CommandTypeName(TAK_CMD_MANA_GIFT));
    ASSERT_EQ_STR("resign", TAK_CommandTypeName(TAK_CMD_RESIGN));
    ASSERT_EQ_STR("power-code", TAK_CommandTypeName(TAK_CMD_POWER_CODE));
    ASSERT_EQ_STR("load-units", TAK_CommandTypeName(TAK_CMD_LOAD_UNITS));
    ASSERT_EQ_INT(1, TAK_CommandTypeTakesUnits(TAK_CMD_LOAD_UNITS));
    /* The seat-wide ones carry no unit list. */
    ASSERT_EQ_INT(0, TAK_CommandTypeTakesUnits(TAK_CMD_RESIGN));
    ASSERT_EQ_INT(0, TAK_CommandTypeTakesUnits(TAK_CMD_ALLIANCE));
    ASSERT_EQ_INT(0, TAK_CommandTypeTakesUnits(TAK_CMD_MANA_GIFT));
    ASSERT_EQ_INT(1, TAK_CommandTypeTakesUnits(TAK_CMD_GIVE_UNITS));
}

/* Bad input is refused, never guessed at. */
TEST(the_codec_refuses_what_it_cannot_run) {
    TAK_GameCommand out;
    size_t used = 0;

    uint8_t truncated[TAK_COMMAND_HEADER_BYTES - 1] = { TAK_CMD_MOVE };
    ASSERT_EQ_INT(-1, TAK_CommandDeserialize(&out, truncated,
                                             sizeof(truncated), &used));

    /* A type this build has never heard of. */
    uint8_t unknown[TAK_COMMAND_HEADER_BYTES];
    memset(unknown, 0, sizeof(unknown));
    unknown[0] = (uint8_t)TAK_CMD_COUNT;
    ASSERT_EQ_INT(-1, TAK_CommandDeserialize(&out, unknown,
                                             sizeof(unknown), &used));
    unknown[0] = TAK_CMD_NONE;
    ASSERT_EQ_INT(-1, TAK_CommandDeserialize(&out, unknown,
                                             sizeof(unknown), &used));

    /* A unit count larger than the list can hold. */
    uint8_t huge[TAK_COMMAND_HEADER_BYTES];
    memset(huge, 0, sizeof(huge));
    huge[0] = TAK_CMD_MOVE;
    tak_put_u16(huge + 1, TAK_COMMAND_MAX_UNITS + 1);
    ASSERT_EQ_INT(-1, TAK_CommandDeserialize(&out, huge, sizeof(huge), &used));

    /* A unit count the payload does not back up. */
    uint8_t lying[TAK_COMMAND_HEADER_BYTES + 4];
    memset(lying, 0, sizeof(lying));
    lying[0] = TAK_CMD_MOVE;
    tak_put_u16(lying + 1, 8);
    ASSERT_EQ_INT(-1, TAK_CommandDeserialize(&out, lying, sizeof(lying), &used));

    /* A buffer that claims more commands than it carries, and one with
     * bytes to spare after the last. */
    uint8_t buf[TAK_COMMAND_BUFFER_HEADER_BYTES + TAK_COMMAND_HEADER_BYTES + 1];
    memset(buf, 0, sizeof(buf));
    buf[0] = 'T'; buf[1] = 'A'; buf[2] = 'K'; buf[3] = TAK_COMMAND_WIRE_VERSION;
    tak_put_u16(buf + 8, 2);
    buf[TAK_COMMAND_BUFFER_HEADER_BYTES] = TAK_CMD_STOP;
    ASSERT_EQ_INT(-1, TAK_CommandBufferDeserialize(&g_buf_b, buf, sizeof(buf) - 1));
    tak_put_u16(buf + 8, 1);
    ASSERT_EQ_INT(-1, TAK_CommandBufferDeserialize(&g_buf_b, buf, sizeof(buf)));
    ASSERT_EQ_INT(0, TAK_CommandBufferDeserialize(&g_buf_b, buf, sizeof(buf) - 1));
    ASSERT_EQ_INT(1, g_buf_b.count);

    /* Wire version 1 is gone. */
    buf[3] = 1;
    ASSERT_EQ_INT(-1, TAK_CommandBufferDeserialize(&g_buf_b, buf, sizeof(buf) - 1));
    buf[3] = TAK_COMMAND_WIRE_VERSION;
    buf[0] = 'X';
    ASSERT_EQ_INT(-1, TAK_CommandBufferDeserialize(&g_buf_b, buf, sizeof(buf) - 1));

    /* A command nobody can run never reaches a buffer. */
    TAK_GameCommand bad;
    memset(&bad, 0, sizeof(bad));
    bad.type = TAK_CMD_NONE;
    TAK_CommandBuffer_Init(&g_buf_a, 1);
    ASSERT_EQ_INT(-1, TAK_CommandBuffer_Push(&g_buf_a, &bad));
    bad.type = TAK_CMD_MOVE;
    bad.unit_count = TAK_COMMAND_MAX_UNITS + 1;
    ASSERT_EQ_INT(-1, TAK_CommandBuffer_Push(&g_buf_a, &bad));
}

/* Random bytes must never be read as a command, and must never be
 * read past their end. A cheap stand-in for the fuzz target that runs
 * against the same entry points in CI. */
TEST(random_bytes_are_never_mistaken_for_a_command) {
    uint32_t s = 12345u;
    uint8_t junk[200];
    for (int round = 0; round < 20000; round++) {
        size_t n = (size_t)(s % sizeof(junk));
        for (size_t i = 0; i < n; i++) {
            s = s * 1103515245u + 12345u;
            junk[i] = (uint8_t)(s >> 16);
        }
        TAK_GameCommand cmd;
        size_t used = 0;
        if (TAK_CommandDeserialize(&cmd, junk, n, &used) == 0) {
            ASSERT(used <= n);
            ASSERT(TAK_CommandTypeIsValid(cmd.type));
            ASSERT(cmd.unit_count <= TAK_COMMAND_MAX_UNITS);
        }
        (void)TAK_CommandBufferDeserialize(&g_buf_b, junk, n);
        s = s * 1103515245u + 12345u;
    }
}

int main(void) {
    TEST_SUITE("Command codec, wire version 2");
    RUN(a_command_lays_out_nineteen_bytes_and_its_units);
    RUN(the_buffer_header_says_version_two);
    RUN(every_command_type_round_trips);
    RUN(version_two_carries_the_types_the_lobby_needs);
    RUN(the_codec_refuses_what_it_cannot_run);
    RUN(random_bytes_are_never_mistaken_for_a_command);
    TEST_REPORT();
}
