#include "tak_commands.h"

#include "tak_bytes.h"

#include <string.h>

void TAK_CommandBuffer_Init(TAK_CommandBuffer *buf, uint32_t target_tick) {
    if (!buf) return;
    memset(buf, 0, sizeof(*buf));
    buf->target_tick = target_tick;
}

int TAK_CommandBuffer_Push(TAK_CommandBuffer *buf, const TAK_GameCommand *cmd) {
    if (!buf || !cmd) return -1;
    if (buf->count >= TAK_COMMAND_BUFFER_MAX) return -1;
    if (cmd->unit_count > TAK_COMMAND_MAX_UNITS) return -1;
    buf->commands[buf->count++] = *cmd;
    return 0;
}

size_t TAK_CommandSerializedSize(const TAK_GameCommand *cmd) {
    if (!cmd || cmd->unit_count > TAK_COMMAND_MAX_UNITS) return 0;
    return 1u + 1u + 2u + 4u + 4u + 4u + 4u + 2u + 2u +
           (size_t)cmd->unit_count * 4u;
}

int TAK_CommandSerialize(const TAK_GameCommand *cmd,
                         uint8_t *out, size_t out_cap,
                         size_t *out_len) {
    size_t need = TAK_CommandSerializedSize(cmd);
    if (out_len) *out_len = need;
    if (need == 0 || !out || out_cap < need) return -1;
    uint8_t *p = out;
    *p++ = cmd->type;
    *p++ = cmd->player_id;
    tak_put_u16(p, cmd->unit_count); p += 2;
    tak_put_u32(p, cmd->execute_tick); p += 4;
    tak_put_u32(p, (uint32_t)cmd->target_x); p += 4;
    tak_put_u32(p, (uint32_t)cmd->target_y); p += 4;
    tak_put_u32(p, cmd->target_unit_id); p += 4;
    tak_put_u16(p, cmd->build_type_id); p += 2;
    tak_put_u16(p, cmd->arg); p += 2;
    for (uint16_t i = 0; i < cmd->unit_count; i++) {
        tak_put_u32(p, cmd->unit_ids[i]);
        p += 4;
    }
    return 0;
}

int TAK_CommandDeserialize(TAK_GameCommand *out,
                           const uint8_t *data, size_t len,
                           size_t *out_used) {
    if (out_used) *out_used = 0;
    if (!out || !data || len < 24u) return -1;
    memset(out, 0, sizeof(*out));
    const uint8_t *p = data;
    out->type = *p++;
    out->player_id = *p++;
    out->unit_count = tak_get_u16(p); p += 2;
    if (out->unit_count > TAK_COMMAND_MAX_UNITS) return -1;
    size_t need = 24u + (size_t)out->unit_count * 4u;
    if (len < need) return -1;
    out->execute_tick = tak_get_u32(p); p += 4;
    out->target_x = (int32_t)tak_get_u32(p); p += 4;
    out->target_y = (int32_t)tak_get_u32(p); p += 4;
    out->target_unit_id = tak_get_u32(p); p += 4;
    out->build_type_id = tak_get_u16(p); p += 2;
    out->arg = tak_get_u16(p); p += 2;
    for (uint16_t i = 0; i < out->unit_count; i++) {
        out->unit_ids[i] = tak_get_u32(p);
        p += 4;
    }
    if (out_used) *out_used = need;
    return 0;
}

int TAK_CommandBufferSerialize(const TAK_CommandBuffer *buf,
                               uint8_t *out, size_t out_cap,
                               size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!buf || !out || buf->count > TAK_COMMAND_BUFFER_MAX) return -1;
    size_t need = 8u;
    for (uint16_t i = 0; i < buf->count; i++) {
        size_t cmd_len = TAK_CommandSerializedSize(&buf->commands[i]);
        if (cmd_len == 0) return -1;
        need += cmd_len;
    }
    if (out_len) *out_len = need;
    if (out_cap < need) return -1;
    out[0] = 'T'; out[1] = 'A'; out[2] = 'K';
    out[3] = TAK_COMMAND_WIRE_VERSION;
    tak_put_u32(out + 4, buf->target_tick);
    size_t off = 8u;
    for (uint16_t i = 0; i < buf->count; i++) {
        size_t cmd_len = 0;
        if (TAK_CommandSerialize(&buf->commands[i],
                                 out + off, out_cap - off, &cmd_len) != 0) {
            return -1;
        }
        off += cmd_len;
    }
    return 0;
}

int TAK_CommandBufferDeserialize(TAK_CommandBuffer *out,
                                 const uint8_t *data, size_t len) {
    if (!out || !data || len < 8u) return -1;
    if (data[0] != 'T' || data[1] != 'A' || data[2] != 'K') return -1;
    if (data[3] != TAK_COMMAND_WIRE_VERSION) return -1;
    TAK_CommandBuffer_Init(out, tak_get_u32(data + 4));
    size_t off = 8u;
    while (off < len) {
        TAK_GameCommand cmd;
        size_t used = 0;
        if (TAK_CommandDeserialize(&cmd, data + off, len - off, &used) != 0) {
            return -1;
        }
        if (TAK_CommandBuffer_Push(out, &cmd) != 0) return -1;
        off += used;
    }
    return off == len ? 0 : -1;
}
