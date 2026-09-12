/*
 * command_emit.c: what the local player did, as a command.
 *
 * The screens do not order units. They say what was clicked, this
 * turns it into a command naming units by stable id, and the queue
 * runs it on its tick. Single player uses the same road at zero
 * delay, so every session exercises the multiplayer path.
 */

#include "tak_command_emit.h"

#include "tak_command_queue.h"
#include "tak_unit.h"

#include <string.h>

/* One command is a kilobyte of unit ids, which is no size for a
 * stack frame on the input path. */
static TAK_GameCommand g_emit;

static void emit_begin(uint8_t type, int32_t world_x, int32_t world_y,
                       int target_handle,
                       uint16_t build_type_id, uint16_t arg) {
    memset(&g_emit, 0, sizeof(g_emit));
    g_emit.type = type;
    g_emit.target_x = world_x;
    g_emit.target_y = world_y;
    g_emit.target_unit_id = target_handle >= 0
                          ? Units_GetStableId(target_handle) : 0u;
    g_emit.build_type_id = build_type_id;
    g_emit.arg = arg;
}

static int emit_send(void) {
    return TAK_CmdQueue_Submit((uint8_t)Units_LocalPlayer(), &g_emit);
}

int TAK_Cmd_EmitSelection(uint8_t type,
                          int32_t world_x, int32_t world_y,
                          int target_handle,
                          uint16_t build_type_id, uint16_t arg) {
    emit_begin(type, world_x, world_y, target_handle, build_type_id, arg);
    int count = 0;
    const int *sel = Units_GetSelection(&count);
    int local = Units_LocalPlayer();
    for (int i = 0; i < count; i++) {
        if (g_emit.unit_count >= TAK_COMMAND_MAX_UNITS) break;
        if (g_units_get_player(sel[i]) != local) continue;
        uint32_t id = Units_GetStableId(sel[i]);
        if (!id) continue;
        g_emit.unit_ids[g_emit.unit_count++] = id;
    }
    if (g_emit.unit_count == 0) return -1;
    return emit_send();
}

int TAK_Cmd_EmitUnit(uint8_t type, int handle,
                     int32_t world_x, int32_t world_y,
                     int target_handle,
                     uint16_t build_type_id, uint16_t arg) {
    uint32_t id = handle >= 0 ? Units_GetStableId(handle) : 0u;
    if (!id) return -1;
    emit_begin(type, world_x, world_y, target_handle, build_type_id, arg);
    g_emit.unit_ids[0] = id;
    g_emit.unit_count = 1;
    return emit_send();
}

int TAK_Cmd_EmitLoadInRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           int queued) {
    static int riders[TAK_COMMAND_MAX_UNITS];
    int carrier = Units_SelectedTransport();
    if (carrier < 0) return -1;
    /* Which units the box holds is a question about where they are
     * drawn, so this machine answers it and the command names them. */
    int n = Units_LoadCandidatesInRect(x0, y0, x1, y1, carrier, riders,
                                       TAK_COMMAND_MAX_UNITS - 1);
    if (n <= 0) return 0;
    emit_begin(TAK_CMD_LOAD_UNITS, 0, 0, -1, 0, queued ? 1u : 0u);
    g_emit.unit_ids[g_emit.unit_count++] = Units_GetStableId(carrier);
    for (int i = 0; i < n; i++) {
        g_emit.unit_ids[g_emit.unit_count++] = Units_GetStableId(riders[i]);
    }
    return emit_send() == 0 ? n : 0;
}

int TAK_Cmd_EmitSeat(uint8_t type,
                     int32_t target_x, uint16_t build_type_id, uint16_t arg) {
    emit_begin(type, target_x, 0, -1, build_type_id, arg);
    return emit_send();
}
