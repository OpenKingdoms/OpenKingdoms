#include "tak_commands.h"

#include "tak_bytes.h"

#include <string.h>

static const char *const g_cmd_names[TAK_CMD_COUNT] = {
    "none",
    "move", "attack", "build", "stop", "patrol", "guard", "repair",
    "reclaim", "capture", "load", "unload", "wait", "set-aggro",
    "set-weapon",
    "factory-enqueue", "factory-dequeue", "factory-cancel", "rally",
    "gate", "attack-ground", "special-weapon", "reclaim-feature",
    "resurrect-feature", "give-units", "alliance", "share-vision",
    "share-units", "share-mana", "mana-gift", "resign", "power-code",
    "load-units"
};

int TAK_CommandTypeIsValid(unsigned type) {
    return type > (unsigned)TAK_CMD_NONE && type < (unsigned)TAK_CMD_COUNT;
}

const char *TAK_CommandTypeName(unsigned type) {
    if (type >= (unsigned)TAK_CMD_COUNT) return "?";
    return g_cmd_names[type];
}

int TAK_CommandTypeTakesUnits(unsigned type) {
    switch (type) {
        case TAK_CMD_ALLIANCE:
        case TAK_CMD_SHARE_VISION:
        case TAK_CMD_SHARE_UNITS:
        case TAK_CMD_SHARE_MANA:
        case TAK_CMD_MANA_GIFT:
        case TAK_CMD_RESIGN:
        case TAK_CMD_POWER_CODE:
            return 0;
        default:
            return TAK_CommandTypeIsValid(type);
    }
}

void TAK_CommandBuffer_Init(TAK_CommandBuffer *buf, uint32_t sequence) {
    if (!buf) return;
    memset(buf, 0, sizeof(*buf));
    buf->sequence = sequence;
}

int TAK_CommandBuffer_Push(TAK_CommandBuffer *buf, const TAK_GameCommand *cmd) {
    if (!buf || !cmd) return -1;
    if (buf->count >= TAK_COMMAND_BUFFER_MAX) return -1;
    if (cmd->unit_count > TAK_COMMAND_MAX_UNITS) return -1;
    if (!TAK_CommandTypeIsValid(cmd->type)) return -1;
    buf->commands[buf->count++] = *cmd;
    return 0;
}

void TAK_CommandBuffer_Stamp(TAK_CommandBuffer *buf, uint8_t seat, uint32_t tick) {
    if (!buf) return;
    for (uint16_t i = 0; i < buf->count; i++) {
        buf->commands[i].seat = seat;
        buf->commands[i].tick = tick;
    }
}

size_t TAK_CommandSerializedSize(const TAK_GameCommand *cmd) {
    if (!cmd || cmd->unit_count > TAK_COMMAND_MAX_UNITS) return 0;
    if (!TAK_CommandTypeIsValid(cmd->type)) return 0;
    return (size_t)TAK_COMMAND_HEADER_BYTES + (size_t)cmd->unit_count * 4u;
}

/* The seat and the tick are absent on purpose: the server stamps one
 * and the turn decides the other. */
static void cmd_write(TAK_ByteWriter *w, const TAK_GameCommand *cmd) {
    TAK_BW_U8(w, cmd->type);
    TAK_BW_U16(w, cmd->unit_count);
    TAK_BW_I32(w, cmd->target_x);
    TAK_BW_I32(w, cmd->target_y);
    TAK_BW_U32(w, cmd->target_unit_id);
    TAK_BW_U16(w, cmd->build_type_id);
    TAK_BW_U16(w, cmd->arg);
    for (uint16_t i = 0; i < cmd->unit_count; i++) {
        TAK_BW_U32(w, cmd->unit_ids[i]);
    }
}

int TAK_CommandSerialize(const TAK_GameCommand *cmd,
                         uint8_t *out, size_t out_cap,
                         size_t *out_len) {
    size_t need = TAK_CommandSerializedSize(cmd);
    if (out_len) *out_len = need;
    if (need == 0 || !out || out_cap < need) return -1;
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, out_cap);
    cmd_write(&w, cmd);
    return TAK_BW_Ok(&w) ? 0 : -1;
}

static int cmd_read(TAK_ByteReader *r, TAK_GameCommand *out) {
    memset(out, 0, sizeof(*out));
    out->type = TAK_BR_U8(r);
    out->unit_count = TAK_BR_U16(r);
    out->target_x = TAK_BR_I32(r);
    out->target_y = TAK_BR_I32(r);
    out->target_unit_id = TAK_BR_U32(r);
    out->build_type_id = TAK_BR_U16(r);
    out->arg = TAK_BR_U16(r);
    if (!TAK_BR_Ok(r)) return -1;
    if (!TAK_CommandTypeIsValid(out->type)) return -1;
    if (out->unit_count > TAK_COMMAND_MAX_UNITS) return -1;
    for (uint16_t i = 0; i < out->unit_count; i++) {
        out->unit_ids[i] = TAK_BR_U32(r);
    }
    return TAK_BR_Ok(r) ? 0 : -1;
}

int TAK_CommandDeserialize(TAK_GameCommand *out,
                           const uint8_t *data, size_t len,
                           size_t *out_used) {
    if (out_used) *out_used = 0;
    if (!out || !data) return -1;
    TAK_ByteReader r;
    TAK_BR_Init(&r, data, len);
    if (cmd_read(&r, out) != 0) return -1;
    if (out_used) *out_used = r.pos;
    return 0;
}

int TAK_CommandBufferSerialize(const TAK_CommandBuffer *buf,
                               uint8_t *out, size_t out_cap,
                               size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!buf || buf->count > TAK_COMMAND_BUFFER_MAX) return -1;
    for (uint16_t i = 0; i < buf->count; i++) {
        if (TAK_CommandSerializedSize(&buf->commands[i]) == 0) return -1;
    }
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, out ? out_cap : 0);
    TAK_BW_U8(&w, 'T');
    TAK_BW_U8(&w, 'A');
    TAK_BW_U8(&w, 'K');
    TAK_BW_U8(&w, TAK_COMMAND_WIRE_VERSION);
    TAK_BW_U32(&w, buf->sequence);
    TAK_BW_U16(&w, buf->count);
    for (uint16_t i = 0; i < buf->count; i++) {
        cmd_write(&w, &buf->commands[i]);
    }
    if (out_len) *out_len = w.len;
    return TAK_BW_Ok(&w) ? 0 : -1;
}

int TAK_CommandBufferDeserialize(TAK_CommandBuffer *out,
                                 const uint8_t *data, size_t len) {
    if (!out || !data) return -1;
    TAK_ByteReader r;
    TAK_BR_Init(&r, data, len);
    uint8_t m0 = TAK_BR_U8(&r), m1 = TAK_BR_U8(&r), m2 = TAK_BR_U8(&r);
    uint8_t version = TAK_BR_U8(&r);
    uint32_t sequence = TAK_BR_U32(&r);
    uint16_t count = TAK_BR_U16(&r);
    if (!TAK_BR_Ok(&r)) return -1;
    if (m0 != 'T' || m1 != 'A' || m2 != 'K') return -1;
    if (version != TAK_COMMAND_WIRE_VERSION) return -1;
    if (count > TAK_COMMAND_BUFFER_MAX) return -1;
    TAK_CommandBuffer_Init(out, sequence);
    for (uint16_t i = 0; i < count; i++) {
        if (cmd_read(&r, &out->commands[i]) != 0) return -1;
        out->count = (uint16_t)(i + 1);
    }
    /* Trailing bytes mean the sender and this build disagree about the
     * format, which lockstep cannot paper over. */
    return TAK_BR_Done(&r) ? 0 : -1;
}
