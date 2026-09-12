#ifndef TAK_COMMAND_EMIT_H
#define TAK_COMMAND_EMIT_H

#include "tak_commands.h"

/*
 * Turning what the local player did into a command.
 *
 * Input, the heads up display and the minimap all come through here.
 * Nothing in src/ui calls an order function any more, which the lint
 * in test_ui_lint keeps true: a click builds a command from the
 * selection, names its units by stable id, and hands it to the queue.
 * The order reaches the units when its tick comes round.
 */

/* The selection, filtered to units the local seat owns, as a command.
 * target_handle is a slot the click landed on, or -1. Returns 0 when
 * the command was queued and -1 when there was nothing to send. */
int TAK_Cmd_EmitSelection(uint8_t type,
                          int32_t world_x, int32_t world_y,
                          int target_handle,
                          uint16_t build_type_id, uint16_t arg);

/* One named unit, which is what a factory command carries. */
int TAK_Cmd_EmitUnit(uint8_t type, int handle,
                     int32_t world_x, int32_t world_y,
                     int target_handle,
                     uint16_t build_type_id, uint16_t arg);

/* The load cursor dragged over a box. With exactly one transport in
 * the selection it sends a pickup of every own unit drawn in the box
 * that it can carry, and returns how many. -1 when the selection does
 * not hold exactly one transport, so the drag is a box select. */
int TAK_Cmd_EmitLoadInRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           int queued);

/* A command about the seat rather than about units: alliance, the
 * sharing flags, a mana gift, resigning, a power code. */
int TAK_Cmd_EmitSeat(uint8_t type,
                     int32_t target_x, uint16_t build_type_id, uint16_t arg);

#endif /* TAK_COMMAND_EMIT_H */
