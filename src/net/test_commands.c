#include "tak_commands.h"

#include <stdio.h>
#include <string.h>

#define ASSERT(x) do { \
    if (!(x)) { \
        fprintf(stderr, "ASSERT failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ_INT(exp, got) do { \
    int _e = (exp); \
    int _g = (got); \
    if (_e != _g) { \
        fprintf(stderr, "ASSERT_EQ_INT failed at %s:%d: expected %d got %d\n", \
                __FILE__, __LINE__, _e, _g); \
        return 1; \
    } \
} while (0)

static int test_command_wire_bytes(void) {
    TAK_GameCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = TAK_CMD_MOVE;
    cmd.player_id = 2;
    cmd.unit_count = 2;
    cmd.execute_tick = 0x01020304u;
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
    ASSERT_EQ_INT(32, (int)len);
    const uint8_t expected[32] = {
        0x01, 0x02, 0x02, 0x00,
        0x04, 0x03, 0x02, 0x01,
        0xf0, 0xff, 0xff, 0xff,
        0x44, 0x33, 0x22, 0x11,
        0xdd, 0xcc, 0xbb, 0xaa,
        0x34, 0x12, 0x78, 0x56,
        0x02, 0x00, 0x00, 0x01,
        0x04, 0x00, 0x00, 0x03
    };
    ASSERT(memcmp(wire, expected, sizeof(expected)) == 0);

    TAK_GameCommand out;
    size_t used = 0;
    ASSERT_EQ_INT(0, TAK_CommandDeserialize(&out, wire, len, &used));
    ASSERT_EQ_INT((int)len, (int)used);
    ASSERT_EQ_INT(cmd.type, out.type);
    ASSERT_EQ_INT(cmd.player_id, out.player_id);
    ASSERT_EQ_INT(cmd.unit_count, out.unit_count);
    ASSERT_EQ_INT((int)cmd.execute_tick, (int)out.execute_tick);
    ASSERT_EQ_INT(cmd.target_x, out.target_x);
    ASSERT_EQ_INT(cmd.target_y, out.target_y);
    ASSERT_EQ_INT((int)cmd.target_unit_id, (int)out.target_unit_id);
    ASSERT_EQ_INT(cmd.build_type_id, out.build_type_id);
    ASSERT_EQ_INT(cmd.arg, out.arg);
    ASSERT_EQ_INT((int)cmd.unit_ids[0], (int)out.unit_ids[0]);
    ASSERT_EQ_INT((int)cmd.unit_ids[1], (int)out.unit_ids[1]);
    return 0;
}

static int test_buffer_roundtrip(void) {
    TAK_CommandBuffer buf;
    TAK_CommandBuffer_Init(&buf, 300);
    TAK_GameCommand a;
    memset(&a, 0, sizeof(a));
    a.type = TAK_CMD_PATROL;
    a.player_id = 1;
    a.execute_tick = 305;
    a.target_x = 128;
    a.target_y = 256;
    a.unit_count = 1;
    a.unit_ids[0] = 42;
    ASSERT_EQ_INT(0, TAK_CommandBuffer_Push(&buf, &a));

    TAK_GameCommand b = a;
    b.type = TAK_CMD_GUARD;
    b.target_unit_id = 99;
    b.unit_ids[0] = 43;
    ASSERT_EQ_INT(0, TAK_CommandBuffer_Push(&buf, &b));

    uint8_t wire[256];
    size_t len = 0;
    ASSERT_EQ_INT(0, TAK_CommandBufferSerialize(&buf, wire, sizeof(wire), &len));
    ASSERT(len > 8);
    ASSERT_EQ_INT('T', wire[0]);
    ASSERT_EQ_INT('A', wire[1]);
    ASSERT_EQ_INT('K', wire[2]);
    ASSERT_EQ_INT(TAK_COMMAND_WIRE_VERSION, wire[3]);

    TAK_CommandBuffer out;
    ASSERT_EQ_INT(0, TAK_CommandBufferDeserialize(&out, wire, len));
    ASSERT_EQ_INT(300, (int)out.target_tick);
    ASSERT_EQ_INT(2, out.count);
    ASSERT_EQ_INT(TAK_CMD_PATROL, out.commands[0].type);
    ASSERT_EQ_INT(TAK_CMD_GUARD, out.commands[1].type);
    ASSERT_EQ_INT(42, (int)out.commands[0].unit_ids[0]);
    ASSERT_EQ_INT(43, (int)out.commands[1].unit_ids[0]);
    ASSERT_EQ_INT(99, (int)out.commands[1].target_unit_id);
    return 0;
}

int main(void) {
    if (test_command_wire_bytes() != 0) return 1;
    if (test_buffer_roundtrip() != 0) return 1;
    puts("test_commands: ok");
    return 0;
}
