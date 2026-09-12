/*
 * command_exec.c: where a player command becomes a change in the
 * simulation.
 *
 * One ownership check lives here and nowhere else. Every unit a
 * command names has to belong to the seat the server stamped on it,
 * so the order primitives below the check take no owner and no
 * selection, and a client cannot order an army it does not own.
 */

#include "tak_command_exec.h"

#include "tak_battle_config.h"
#include "tak_economy.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <string.h>

/* Units a command may reach in one go. The wire caps a command at
 * TAK_COMMAND_MAX_UNITS, and this holds exactly that many. */
static int g_exec_handles[TAK_COMMAND_MAX_UNITS];

static int exec_seat_valid(unsigned seat) {
    return seat >= 1u && seat <= (unsigned)TAK_MAX_PLAYERS;
}

/* ── the one ownership check ──────────────────────────────────────
 *
 * Resolve the command's stable ids into slots and drop every unit the
 * stamped seat does not own. Nothing further down this file looks at
 * a seat again. */
static int exec_owned_units(const TAK_GameCommand *cmd) {
    int n = 0;
    for (uint16_t i = 0; i < cmd->unit_count && n < TAK_COMMAND_MAX_UNITS; i++) {
        int h = Units_FindByStableId(cmd->unit_ids[i]);
        if (h < 0) continue;
        if (g_units_get_player(h) != (int)cmd->seat) continue;
        g_exec_handles[n++] = h;
    }
    return n;
}

/* A target the command points at, which the seat need not own: you
 * attack an enemy and you heal a friend. -1 when it names none or the
 * unit has gone. */
static int exec_target(const TAK_GameCommand *cmd) {
    return cmd->target_unit_id ? Units_FindByStableId(cmd->target_unit_id) : -1;
}

/* arg as the diplomacy commands pack it: the other seat in the low
 * byte, the new setting in the high byte. */
static int exec_other_seat(const TAK_GameCommand *cmd) {
    unsigned other = cmd->arg & 0xffu;
    return exec_seat_valid(other) ? (int)other : -1;
}

static int exec_flag(const TAK_GameCommand *cmd) {
    return (cmd->arg >> 8) != 0;
}

/* ── the seat-wide commands ───────────────────────────────────────── */

static int exec_seat_command(const TAK_GameCommand *cmd, GameWorld *w) {
    int seat = (int)cmd->seat;
    switch (cmd->type) {
        case TAK_CMD_ALLIANCE: {
            int other = exec_other_seat(cmd);
            if (other < 0 || other == seat) return 0;
            w->allied[seat][other] = (uint8_t)exec_flag(cmd);
            return 1;
        }
        case TAK_CMD_SHARE_VISION: {
            int other = exec_other_seat(cmd);
            if (other < 0 || other == seat) return 0;
            w->share_vision[seat][other] = (uint8_t)exec_flag(cmd);
            return 1;
        }
        case TAK_CMD_SHARE_UNITS: {
            int other = exec_other_seat(cmd);
            if (other < 0 || other == seat) return 0;
            w->share_units[seat][other] = (uint8_t)exec_flag(cmd);
            return 1;
        }
        case TAK_CMD_SHARE_MANA: {
            int other = exec_other_seat(cmd);
            if (other < 0 || other == seat) return 0;
            w->share_mana[seat][other] = (uint8_t)exec_flag(cmd);
            return 1;
        }
        case TAK_CMD_MANA_GIFT: {
            int other = exec_other_seat(cmd);
            if (other < 0 || other == seat) return 0;
            /* target_x is 16.16, so a gift is exact on every machine
             * however the sender's slider rounded it. */
            int32_t whole = cmd->target_x >> 16;
            if (whole <= 0) return 0;
            if (!Economy_TrySpend(&w->economy, seat, whole)) return 0;
            Economy_Earn(&w->economy, other, whole);
            return 1;
        }
        case TAK_CMD_RESIGN:
            if (w->resigned[seat]) return 0;
            w->resigned[seat] = 1;
            return 1;
        case TAK_CMD_POWER_CODE:
            /* Refused unless the room turned them on, so a client that
             * sends one anyway changes nothing anywhere. */
            if (!w->cfg.power_codes) return 0;
            /* Only the mana code so far: arg names it, build_type_id
             * carries the amount. */
            if (cmd->arg != 1) return 0;
            Economy_Earn(&w->economy, seat, (int32_t)cmd->build_type_id);
            return 1;
        default:
            return 0;
    }
}

/* ── the unit commands ────────────────────────────────────────────── */

static int exec_unit_command(const TAK_GameCommand *cmd, int count) {
    int target = exec_target(cmd);
    int applied = 0;

    switch (cmd->type) {
        case TAK_CMD_MOVE:
            for (int i = 0; i < count; i++)
                applied += Units_OrderMove(g_exec_handles[i],
                                           cmd->target_x, cmd->target_y);
            break;
        case TAK_CMD_PATROL:
            for (int i = 0; i < count; i++)
                applied += Units_OrderPatrol(g_exec_handles[i],
                                             cmd->target_x, cmd->target_y);
            break;
        case TAK_CMD_ATTACK:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += Units_OrderAttack(g_exec_handles[i], target);
            break;
        case TAK_CMD_ATTACK_GROUND:
            for (int i = 0; i < count; i++)
                applied += Units_OrderAttackGround(g_exec_handles[i],
                                                   cmd->target_x, cmd->target_y);
            break;
        case TAK_CMD_GUARD:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += Units_OrderGuard(g_exec_handles[i], target);
            break;
        case TAK_CMD_REPAIR:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += Units_OrderRepair(g_exec_handles[i], target);
            break;
        case TAK_CMD_RECLAIM:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += Units_OrderReclaim(g_exec_handles[i], target);
            break;
        case TAK_CMD_LOAD:
            /* One transport among the units picks the target up, and
             * arg bit 0 keeps its earlier pickups (legacy:181670-181671). */
            if (target < 0) break;
            applied = Units_OrderLoadGroup(g_exec_handles, count, target,
                                           cmd->arg & 1u);
            break;
        case TAK_CMD_UNLOAD:
            for (int i = 0; i < count; i++)
                applied += Units_OrderUnload(g_exec_handles[i],
                                             cmd->target_x, cmd->target_y);
            break;
        case TAK_CMD_STOP:
            for (int i = 0; i < count; i++)
                applied += Units_OrderStop(g_exec_handles[i]);
            break;
        case TAK_CMD_SET_AGGRO:
            for (int i = 0; i < count; i++)
                applied += Units_OrderSetAggro(g_exec_handles[i], (int)cmd->arg);
            break;
        case TAK_CMD_SET_WEAPON:
            for (int i = 0; i < count; i++)
                applied += Units_OrderSetWeaponSlot(g_exec_handles[i],
                                                    (int)cmd->arg);
            break;
        case TAK_CMD_SPECIAL_WEAPON:
            /* Slot 2 is the special one. With a unit under the click the
             * shot follows on the next combat tick, and with none the
             * units walk to the spot, as the cursor always has. */
            for (int i = 0; i < count; i++) {
                int h = g_exec_handles[i];
                Units_OrderSetWeaponSlot(h, 2);
                applied += target >= 0
                         ? Units_OrderAttack(h, target)
                         : Units_OrderMove(h, cmd->target_x, cmd->target_y);
            }
            break;
        case TAK_CMD_RECLAIM_FEATURE:
            /* The sweep resolves on the map cell first. Only when no unit
             * took the cell does a unit under the click become the target
             * (legacy:187127-187207). Deciding here, on the tick, is what
             * keeps the choice the same on every machine. */
            for (int i = 0; i < count; i++)
                applied += Units_OrderReclaimFeature(g_exec_handles[i],
                                                     cmd->target_x,
                                                     cmd->target_y);
            if (applied == 0 && target >= 0) {
                for (int i = 0; i < count; i++)
                    applied += Units_OrderReclaim(g_exec_handles[i], target);
            }
            break;
        case TAK_CMD_RESURRECT_FEATURE:
            for (int i = 0; i < count; i++)
                applied += Units_OrderResurrectFeature(g_exec_handles[i],
                                                       cmd->target_x,
                                                       cmd->target_y);
            break;
        case TAK_CMD_GATE:
            for (int i = 0; i < count; i++) {
                if (Units_GateState(g_exec_handles[i]) < 0) continue;
                Units_SetGateOpen(g_exec_handles[i], cmd->arg != 0);
                applied++;
            }
            break;
        case TAK_CMD_BUILD:
            /* The first builder that can take the site starts it, as a
             * click on the ghost does. */
            for (int i = 0; i < count; i++) {
                if (Units_BeginBuildingForUnit(g_exec_handles[i],
                                               (int)cmd->build_type_id,
                                               cmd->target_x,
                                               cmd->target_y) >= 0) {
                    applied++;
                    break;
                }
            }
            break;
        case TAK_CMD_FACTORY_ENQUEUE:
            for (int i = 0; i < count; i++)
                applied += Units_FactoryEnqueue(g_exec_handles[i],
                                                (int)cmd->build_type_id) == 0;
            break;
        case TAK_CMD_FACTORY_DEQUEUE:
            for (int i = 0; i < count; i++)
                applied += Units_FactoryDequeueDef(g_exec_handles[i],
                                                   (int)cmd->build_type_id) == 0;
            break;
        case TAK_CMD_FACTORY_CANCEL:
            for (int i = 0; i < count; i++)
                applied += Units_FactoryCancelCurrent(g_exec_handles[i]) == 0;
            break;
        case TAK_CMD_RALLY:
            for (int i = 0; i < count; i++) {
                Units_FactorySetRally(g_exec_handles[i],
                                      cmd->target_x, cmd->target_y);
                applied++;
            }
            break;
        case TAK_CMD_GIVE_UNITS: {
            /* The original's own player action (legacy:155766-155797).
             * arg names the seat receiving. */
            int other = exec_other_seat(cmd);
            if (other < 0 || other == (int)cmd->seat) break;
            int color = Units_PlayerColorIndex(other);
            for (int i = 0; i < count; i++) {
                Units_SetOwner(g_exec_handles[i], other, color);
                applied++;
            }
            break;
        }
        case TAK_CMD_LOAD_UNITS: {
            /* unit_ids[0] is the transport. When the check dropped it the
             * riders have no one to board. */
            int carrier = Units_FindByStableId(cmd->unit_ids[0]);
            if (count < 2 || g_exec_handles[0] != carrier) break;
            applied = Units_OrderLoadList(carrier, g_exec_handles + 1,
                                          count - 1, cmd->arg & 1u);
            break;
        }
        case TAK_CMD_CAPTURE:
        case TAK_CMD_WAIT:
            /* Declared on the wire, not yet simulated. Refusing them
             * everywhere alike is what keeps the machines in step. */
            break;
        default:
            break;
    }
    return applied;
}

int TAK_CommandExec_Apply(const TAK_GameCommand *cmd) {
    if (!cmd) return 0;
    if (!TAK_CommandTypeIsValid(cmd->type)) return 0;
    if (!exec_seat_valid(cmd->seat)) return 0;
    if (cmd->unit_count > TAK_COMMAND_MAX_UNITS) return 0;

    GameWorld *w = World_Get();
    if (!w) return 0;
    /* A seat that resigned issues nothing further. */
    if (w->resigned[cmd->seat]) return 0;

    if (!TAK_CommandTypeTakesUnits(cmd->type)) {
        return exec_seat_command(cmd, w);
    }
    int count = exec_owned_units(cmd);
    if (count <= 0) return 0;
    return exec_unit_command(cmd, count);
}
