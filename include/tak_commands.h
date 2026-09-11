#ifndef TAK_COMMANDS_H
#define TAK_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Player commands: the only thing that crosses the wire.
 *
 * Every action a player takes becomes one of these, is queued, and is
 * applied at a scheduled tick on every machine. Wire version 2 carries
 * no seat and no execution tick: the server stamps the seat, which is
 * also the rule that stops one player forging another's orders, and
 * the turn a command lands in decides the tick.
 *
 * Units are named by stable id, never by array slot, because slots are
 * reused within a session and mean nothing on another machine.
 */

#define TAK_COMMAND_MAX_UNITS        256
#define TAK_COMMAND_BUFFER_MAX        64
#define TAK_COMMAND_WIRE_VERSION       2

/* type + unit_count + target_x + target_y + target_unit_id
 * + build_type_id + arg. */
#define TAK_COMMAND_HEADER_BYTES      19
/* 'T' 'A' 'K' + version + sequence + count. */
#define TAK_COMMAND_BUFFER_HEADER_BYTES 10

typedef enum TAK_CommandType {
    TAK_CMD_NONE = 0,
    /* target_x, target_y: where to go. */
    TAK_CMD_MOVE,
    /* target_unit_id: what to hit. */
    TAK_CMD_ATTACK,
    /* build_type_id at target_x, target_y: place a building. */
    TAK_CMD_BUILD,
    TAK_CMD_STOP,
    TAK_CMD_PATROL,
    /* target_unit_id: who to follow. */
    TAK_CMD_GUARD,
    TAK_CMD_REPAIR,
    TAK_CMD_RECLAIM,
    TAK_CMD_CAPTURE,
    TAK_CMD_LOAD,
    TAK_CMD_UNLOAD,
    TAK_CMD_WAIT,
    /* arg: UNIT_AGGRO_*. */
    TAK_CMD_SET_AGGRO,
    /* arg: weapon slot 0..2. */
    TAK_CMD_SET_WEAPON,

    /* ── added with wire version 2 ──────────────────────────────── */

    /* unit_ids[0] is the factory, build_type_id the product. */
    TAK_CMD_FACTORY_ENQUEUE,
    /* Take the last queued product of build_type_id off the queue. */
    TAK_CMD_FACTORY_DEQUEUE,
    /* Drop what the factory is building now. */
    TAK_CMD_FACTORY_CANCEL,
    /* Where a factory's products gather: target_x, target_y. */
    TAK_CMD_RALLY,
    /* arg: 1 opens the gate, 0 closes it. */
    TAK_CMD_GATE,
    /* Shoot at ground nobody stands on: target_x, target_y. */
    TAK_CMD_ATTACK_GROUND,
    /* The special weapon at target_unit_id, or at target_x, target_y
     * when no unit is named. */
    TAK_CMD_SPECIAL_WEAPON,
    /* Sweep the map feature under target_x, target_y. */
    TAK_CMD_RECLAIM_FEATURE,
    /* Raise what lies at target_x, target_y. */
    TAK_CMD_RESURRECT_FEATURE,
    /* Hand the named units to the seat in arg (legacy:155766-155797). */
    TAK_CMD_GIVE_UNITS,
    /* arg: seat in the low byte, 1 to ally in the high byte. */
    TAK_CMD_ALLIANCE,
    /* The three sharing flags, arg laid out as TAK_CMD_ALLIANCE. */
    TAK_CMD_SHARE_VISION,
    TAK_CMD_SHARE_UNITS,
    TAK_CMD_SHARE_MANA,
    /* arg: the seat receiving, target_x: the amount in 16.16. */
    TAK_CMD_MANA_GIFT,
    TAK_CMD_RESIGN,
    /* arg: which code, build_type_id: its parameter. Refused unless
     * the room allows them. */
    TAK_CMD_POWER_CODE,
    /* The load cursor dragged over a box: unit_ids[0] is the transport
     * and the rest are the riders in the order it picks them up. arg
     * bit 0 keeps its earlier pickups (legacy:238654-238692). */
    TAK_CMD_LOAD_UNITS,

    TAK_CMD_COUNT
} TAK_CommandType;

typedef struct TAK_GameCommand {
    /* Stamped on arrival, never sent by a client. */
    uint8_t  seat;
    uint32_t tick;

    /* The wire payload. */
    uint8_t  type;
    uint16_t unit_count;
    int32_t  target_x;
    int32_t  target_y;
    uint32_t target_unit_id;    /* a stable id, 0 for none */
    uint16_t build_type_id;
    uint16_t arg;
    uint32_t unit_ids[TAK_COMMAND_MAX_UNITS];   /* stable ids */
} TAK_GameCommand;

typedef struct TAK_CommandBuffer {
    uint32_t sequence;          /* the sender's own counter */
    uint16_t count;
    TAK_GameCommand commands[TAK_COMMAND_BUFFER_MAX];
} TAK_CommandBuffer;

/* 0 for a type this build cannot execute. A command nobody can run
 * has to be refused, not skipped: lockstep has no room for a machine
 * that quietly does less than its peers. */
int         TAK_CommandTypeIsValid(unsigned type);
/* A short name for logs and test failures. Never NULL. */
const char *TAK_CommandTypeName(unsigned type);

/* 1 when the type carries a list of units, 0 for the seat-wide ones
 * (alliance, sharing, a mana gift, resigning, a power code). */
int         TAK_CommandTypeTakesUnits(unsigned type);

void   TAK_CommandBuffer_Init(TAK_CommandBuffer *buf, uint32_t sequence);
int    TAK_CommandBuffer_Push(TAK_CommandBuffer *buf, const TAK_GameCommand *cmd);
/* Write the seat and tick onto every command in the buffer, which is
 * what a receiver does with a turn bundle. */
void   TAK_CommandBuffer_Stamp(TAK_CommandBuffer *buf, uint8_t seat, uint32_t tick);

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
