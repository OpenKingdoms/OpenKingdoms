#ifndef TAK_COMMANDS_H
#define TAK_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

#define TAK_COMMAND_MAX_UNITS        256
#define TAK_COMMAND_BUFFER_MAX        64
#define TAK_COMMAND_WIRE_VERSION       1

typedef enum TAK_CommandType {
    TAK_CMD_NONE = 0,
    TAK_CMD_MOVE,
    TAK_CMD_ATTACK,
    TAK_CMD_BUILD,
    TAK_CMD_STOP,
    TAK_CMD_PATROL,
    TAK_CMD_GUARD,
    TAK_CMD_REPAIR,
    TAK_CMD_RECLAIM,
    TAK_CMD_CAPTURE,
    TAK_CMD_LOAD,
    TAK_CMD_UNLOAD,
    TAK_CMD_WAIT,
    TAK_CMD_SET_AGGRO,
    TAK_CMD_SET_WEAPON
} TAK_CommandType;

typedef struct TAK_GameCommand {
    uint8_t  type;
    uint8_t  player_id;
    uint16_t unit_count;
    uint32_t execute_tick;
    int32_t  target_x;
    int32_t  target_y;
    uint32_t target_unit_id;
    uint16_t build_type_id;
    uint16_t arg;
    uint32_t unit_ids[TAK_COMMAND_MAX_UNITS];
} TAK_GameCommand;

typedef struct TAK_CommandBuffer {
    uint32_t target_tick;
    uint16_t count;
    TAK_GameCommand commands[TAK_COMMAND_BUFFER_MAX];
} TAK_CommandBuffer;

void   TAK_CommandBuffer_Init(TAK_CommandBuffer *buf, uint32_t target_tick);
int    TAK_CommandBuffer_Push(TAK_CommandBuffer *buf, const TAK_GameCommand *cmd);
size_t TAK_CommandSerializedSize(const TAK_GameCommand *cmd);
int    TAK_CommandSerialize(const TAK_GameCommand *cmd,
                            uint8_t *out, size_t out_cap,
                            size_t *out_len);
int    TAK_CommandDeserialize(TAK_GameCommand *out,
                              const uint8_t *data, size_t len,
                              size_t *out_used);
int    TAK_CommandBufferSerialize(const TAK_CommandBuffer *buf,
                                  uint8_t *out, size_t out_cap,
                                  size_t *out_len);
int    TAK_CommandBufferDeserialize(TAK_CommandBuffer *out,
                                    const uint8_t *data, size_t len);

#endif /* TAK_COMMANDS_H */
