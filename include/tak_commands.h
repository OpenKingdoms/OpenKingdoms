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
/* The largest command on the wire: a formation of every unit, an id
 * and an offset each. */
#define TAK_COMMAND_MAX_BYTES     (TAK_COMMAND_HEADER_BYTES + TAK_COMMAND_MAX_UNITS * 8u)

/* TAK_CMD_MOVE_FORMATION's arg bits. */
#define TAK_FORMATION_QUEUE      0x1u   /* behind the orders in hand */
#define TAK_FORMATION_GROUP_PACE 0x2u   /* at the slowest unit's pace */
#define TAK_FORMATION_FACE       0x4u   /* turn to a heading on arrival */
/* Units in one formation command. A match paces what a seat sends to
 * what its relay takes a turn, so a big move goes as several. */
#define TAK_FORMATION_CHUNK      128

/* In arg of a unit order: go behind the orders the unit holds rather
 * than replace them, as Shift does. MOVE, ATTACK, ATTACK_GROUND, PATROL,
 * GUARD, REPAIR, RECLAIM, RECLAIM_FEATURE, RESURRECT_FEATURE, CAPTURE,
 * UNLOAD, SPECIAL_WEAPON, RALLY and BUILD take it. */
#define TAK_CMD_ARG_QUEUE        0x8000u
/* In arg of the same orders: replace the order in hand and keep the ones
 * queued behind it, the manual's Ctrl-click. */
#define TAK_CMD_ARG_KEEP         0x4000u
/* FACTORY_ENQUEUE and FACTORY_DEQUEUE: how many in the low bits, 0
 * meaning one. TAK_FACTORY_ALL makes an enqueue run without end and a
 * dequeue take every one (the original's Ctrl click). */
#define TAK_FACTORY_COUNT_MASK   0x3FFFu
#define TAK_FACTORY_ALL          0x3FFFu
/* FACTORY_ENQUEUE: a factory still being built takes the order and
 * starts on it once finished. A remaster option, never the original's. */
#define TAK_FACTORY_UNFINISHED   0x4000u

typedef enum TAK_CommandType {
    TAK_CMD_NONE = 0,
    /* target_x, target_y: where to go. */
    TAK_CMD_MOVE,
    /* target_unit_id: what to hit. */
    TAK_CMD_ATTACK,
    /* build_type_id at target_x, target_y: place a building, turned by
     * arg's low two bits (quarter turns clockwise). */
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

    /* unit_ids[0] is the factory, build_type_id the product, arg as
     * TAK_FACTORY_* says. */
    TAK_CMD_FACTORY_ENQUEUE,
    /* Take the last queued products of build_type_id off the queue. */
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
    /* A typed console command that changes the battle. arg's low byte
     * is a TAK_CODE_*, its high byte the code's small parameter. The
     * power codes are refused unless the room allows them. */
    TAK_CMD_POWER_CODE,
    /* The load cursor dragged over a box: unit_ids[0] is the transport
     * and the rest are the riders in the order it picks them up. arg
     * bit 0 keeps its earlier pickups (legacy:238654-238692). */
    TAK_CMD_LOAD_UNITS,
    /* Each unit to a point of its own, target_x + unit_dx[i] and
     * target_y + unit_dy[i], as one order. arg holds TAK_FORMATION_*
     * bits, and with TAK_FORMATION_FACE build_type_id is the heading
     * to turn to on arrival, in 65536ths of a turn. target_unit_id is
     * the sender's number for the move, the same on every command a big
     * move is sent as, so they keep one pace. A build that does not
     * know the type refuses the command, and the lobby's build id keeps
     * such builds apart. */
    TAK_CMD_MOVE_FORMATION,

    TAK_CMD_COUNT
} TAK_CommandType;

/* The codes a TAK_CMD_POWER_CODE carries, the original's console table
 * (legacy:38938-38946 registers it). What each one changes is in
 * command_exec.c. */
typedef enum TAK_ConsoleCode {
    TAK_CODE_NONE = 0,
    TAK_CODE_ATM,           /* the seat's pool to its cap */
    TAK_CODE_RADAR,         /* the seat's minimap shows every unit */
    TAK_CODE_VIEW,          /* arg high byte: the seat to look through */
    TAK_CODE_LOS,           /* arg high byte: a TAK_CODE_LOS_* */
    TAK_CODE_MAPPING,
    TAK_CODE_DOUBLE_SHOT,
    TAK_CODE_HALF_SHOT,
    TAK_CODE_NOW_I_SEE,
    TAK_CODE_MANA_ME,       /* the named units' own mana to full */
    TAK_CODE_NO_MANA,       /* and to empty */
    TAK_CODE_I_WIN,
    TAK_CODE_I_LOSE,
    TAK_CODE_KILL,
    /* The two sharing settings, open to every room. target_x holds the
     * value in 16.16, from 0 to 1. */
    TAK_CODE_SHARE_LIMIT,
    TAK_CODE_SHARE_PCT,
    TAK_CODE_COUNT
} TAK_ConsoleCode;

#define TAK_CODE_LOS_TOGGLE 0u
#define TAK_CODE_LOS_OFF    1u
#define TAK_CODE_LOS_ON     2u

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
    /* TAK_CMD_MOVE_FORMATION only: each unit's point, from target. */
    int16_t  unit_dx[TAK_COMMAND_MAX_UNITS];
    int16_t  unit_dy[TAK_COMMAND_MAX_UNITS];
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
/* 1 for a code only a room that allows power codes runs. */
int         TAK_ConsoleCodeNeedsRoom(unsigned code);

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
