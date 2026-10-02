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
#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <stdint.h>
#include <string.h>

/* Units a command may reach in one go. The wire caps a command at
 * TAK_COMMAND_MAX_UNITS, and this holds exactly that many. */
static int g_exec_handles[TAK_COMMAND_MAX_UNITS];
/* Where each kept unit stood in the command, for per-unit data. */
static int g_exec_slot[TAK_COMMAND_MAX_UNITS];

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
        g_exec_slot[n] = i;
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

/* ── the console codes ────────────────────────────────────────────
 *
 * The original ran these on the machine that typed them and let the
 * others find out, which lockstep cannot allow, so each is a command
 * every machine applies. What each changes is the original's handler,
 * found through its command table (legacy:38938-38946). */

/* A share setting's 16.16, from 0 to 1 as the original insists. */
static int exec_unit_fraction(int32_t v, float *out) {
    if (v < 0 || v > 0x10000) return 0;
    *out = (float)v / 65536.0f;
    return 1;
}

static int exec_console_code(const TAK_GameCommand *cmd, GameWorld *w) {
    int seat = (int)cmd->seat;
    unsigned code = cmd->arg & 0xffu;
    unsigned param = cmd->arg >> 8;
    WorldConsole *c = &w->console;
    /* Refused unless the room turned them on, so a client that sends
     * one anyway changes nothing anywhere. */
    if (TAK_ConsoleCodeNeedsRoom(code) && !w->cfg.power_codes) return 0;
    switch (code) {
        case TAK_CODE_ATM: {
            /* The pool to its cap, the difference counted as earned. */
            const PlayerEconomy *e = &w->economy.players[seat - 1];
            float room = (float)e->max_mana - e->mana;
            if (room > 0.0f) Economy_EarnF(&w->economy, seat, room);
            return 1;
        }
        case TAK_CODE_RADAR:
            c->radar[seat] = !c->radar[seat];
            return 1;
        case TAK_CODE_VIEW:
            /* Only a seat that plays can be looked through, and the
             * seat's own number goes back to its own view. */
            if (!exec_seat_valid(param)) return 0;
            if (w->cfg.players[param - 1].kind == TAK_SLOT_CLOSED) return 0;
            c->view[seat] = (uint8_t)(param == (unsigned)seat ? 0 : param);
            return 1;
        case TAK_CODE_LOS:
            if (param == TAK_CODE_LOS_OFF) w->cfg.line_of_sight = 0;
            else if (param == TAK_CODE_LOS_ON) w->cfg.line_of_sight = 1;
            else w->cfg.line_of_sight = !w->cfg.line_of_sight;
            Fog_Refresh(w);
            return 1;
        case TAK_CODE_MAPPING:
            w->cfg.map_revealed = !w->cfg.map_revealed;
            Fog_Refresh(w);
            return 1;
        case TAK_CODE_NOW_I_SEE:
            w->cfg.line_of_sight = 0;
            w->cfg.map_revealed = 1;
            Fog_Refresh(w);
            return 1;
        case TAK_CODE_DOUBLE_SHOT:
            c->double_shot = !c->double_shot;
            if (c->double_shot) c->half_shot = 0;
            return 1;
        case TAK_CODE_HALF_SHOT:
            c->half_shot = !c->half_shot;
            if (c->half_shot) c->double_shot = 0;
            return 1;
        case TAK_CODE_MANA_ME:
        case TAK_CODE_NO_MANA: {
            /* The original fills or empties the typist's selection,
             * which arrives here as the command's units. */
            int n = exec_owned_units(cmd);
            for (int i = 0; i < n; i++)
                Units_FillOwnMana(g_exec_handles[i], code == TAK_CODE_MANA_ME);
            return 1;
        }
        case TAK_CODE_I_WIN:
            /* Every seat the typist is at war with loses its army. */
            for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
                if (p == seat || !Units_PlayersAreEnemies(seat, p)) continue;
                Units_KillAllOf(p);
                w->stats[p].eliminated = 1;
            }
            /* The battle ends on the spot, not on the unit count, and
             * the verdict reads the call this same tick. */
            if (!c->called[seat]) c->called[seat] = 1;
            return 1;
        case TAK_CODE_I_LOSE:
            Units_KillAllOf(seat);
            w->stats[seat].eliminated = 1;
            if (!c->called[seat]) c->called[seat] = -1;
            return 1;
        case TAK_CODE_KILL:
            Units_KillAllOf(0);
            return 1;
        case TAK_CODE_SHARE_LIMIT:
            return exec_unit_fraction(cmd->target_x, &c->share_limit[seat]);
        case TAK_CODE_SHARE_PCT:
            return exec_unit_fraction(cmd->target_x, &c->share_pct[seat]);
        default:
            return 0;
    }
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
             * however the sender's slider rounded it. Any amount above
             * 0 goes, fractions too. */
            if (cmd->target_x <= 0) return 0;
            /* What the giver holds and the receiver has room for goes,
             * the rest stays with the giver (legacy:206055-206087). */
            return Economy_Transfer(&w->economy, seat, other,
                                    (float)cmd->target_x / 65536.0f) > 0.0f;
        }
        case TAK_CMD_RESIGN:
            if (w->resigned[seat]) return 0;
            w->resigned[seat] = 1;
            return 1;
        case TAK_CMD_POWER_CODE:
            return exec_console_code(cmd, w);
        default:
            return 0;
    }
}

/* Who plays a seat. The match makes this from the relay's own entry in
 * a turn, so every machine hands the seat over on the same tick and the
 * computer thinks for it up to that tick and not after. */
static int exec_seat_control(const TAK_GameCommand *cmd, GameWorld *w) {
    int seat = (int)cmd->seat;
    PlayerSlot *slot = &w->cfg.players[seat - 1];
    switch (cmd->arg) {
        case TAK_SEAT_TO_COMPUTER:
            if (slot->kind != TAK_SLOT_HUMAN || w->resigned[seat]) return 0;
            slot->kind = TAK_SLOT_AI;
            return 1;
        case TAK_SEAT_TO_HUMAN:
            if (slot->kind != TAK_SLOT_AI) return 0;
            slot->kind = TAK_SLOT_HUMAN;
            return 1;
        case TAK_SEAT_ARMY_REMOVED:
            /* The original took a departed player's army off the map. */
            Units_KillAllOf(seat);
            w->stats[seat].eliminated = 1;
            w->resigned[seat] = 1;
            return 1;
        case TAK_SEAT_RESIGNED:
            if (w->resigned[seat]) return 0;
            w->resigned[seat] = 1;
            return 1;
        default:
            return 0;
    }
}

/* ── the unit commands ────────────────────────────────────────────── */

/* A Shift order goes behind what the unit holds, a Ctrl one replaces
 * the order in hand and keeps the rest. 0 for neither. */
static int g_exec_mode;

static int exec_mode(const TAK_GameCommand *cmd) {
    if (cmd->arg & TAK_CMD_ARG_QUEUE) return 1;
    if (cmd->arg & TAK_CMD_ARG_KEEP) return UNIT_ORDER_KEEP;
    return 0;
}

static int exec_leg(int handle, int kind, const TAK_GameCommand *cmd,
                    int target) {
    UnitMoveLeg leg;
    memset(&leg, 0, sizeof leg);
    leg.kind = (uint8_t)kind;
    leg.x = cmd->target_x;
    leg.y = cmd->target_y;
    leg.target = target >= 0 ? Units_GetStableId(target) : 0u;
    if (kind == UNIT_LEG_BUILD) {
        leg.def = (int16_t)cmd->build_type_id;
        leg.facing = (uint8_t)(cmd->arg & 3u);
    }
    return Units_OrderLeg(handle, &leg, g_exec_mode);
}

/* How many a factory command names, TAK_FACTORY_ALL standing for the
 * original's ten million. */
static int exec_factory_count(const TAK_GameCommand *cmd) {
    unsigned n = cmd->arg & TAK_FACTORY_COUNT_MASK;
    if (n == TAK_FACTORY_ALL) return (int)UNIT_PROD_ENDLESS;
    return n ? (int)n : 1;
}

static int exec_unit_command(const TAK_GameCommand *cmd, int count) {
    int target = exec_target(cmd);
    int applied = 0;
    int mode = exec_mode(cmd);
    g_exec_mode = mode;

    switch (cmd->type) {
        case TAK_CMD_MOVE:
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_MOVE, cmd, -1)
                    : Units_OrderMove(g_exec_handles[i],
                                      cmd->target_x, cmd->target_y);
            break;
        case TAK_CMD_MOVE_FORMATION: {
            /* The group is the seat's and the number the sender gave the
             * move, so the commands of one big move keep one pace. The
             * pace itself is worked out each tick from those walking. */
            const GameWorld *w = World_Get();
            UnitMoveLeg leg;
            memset(&leg, 0, sizeof leg);
            leg.group = ((uint32_t)cmd->seat << 24) | (cmd->target_unit_id & 0xFFFFFFu);
            leg.paced = (cmd->arg & TAK_FORMATION_GROUP_PACE) ? 1 : 0;
            leg.face = (cmd->arg & TAK_FORMATION_FACE) ? 1 : 0;
            leg.heading = leg.face ? cmd->build_type_id : 0;
            int queued = (cmd->arg & TAK_FORMATION_QUEUE) != 0;
            int64_t max_x = w && w->map_pixels_w > 0 ? w->map_pixels_w - 1 : INT32_MAX;
            int64_t max_y = w && w->map_pixels_h > 0 ? w->map_pixels_h - 1 : INT32_MAX;
            for (int i = 0; i < count; i++) {
                int k = g_exec_slot[i];
                /* Wide enough that no command can wrap it, and on the map. */
                int64_t x = (int64_t)cmd->target_x + cmd->unit_dx[k];
                int64_t y = (int64_t)cmd->target_y + cmd->unit_dy[k];
                leg.x = (int32_t)(x < 0 ? 0 : x > max_x ? max_x : x);
                leg.y = (int32_t)(y < 0 ? 0 : y > max_y ? max_y : y);
                applied += Units_OrderMoveLeg(g_exec_handles[i], &leg, queued);
            }
            break;
        }
        case TAK_CMD_PATROL:
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_PATROL, cmd, -1)
                    : Units_OrderPatrol(g_exec_handles[i],
                                        cmd->target_x, cmd->target_y);
            break;
        case TAK_CMD_ATTACK:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_ATTACK, cmd, target)
                    : Units_OrderAttack(g_exec_handles[i], target);
            break;
        case TAK_CMD_ATTACK_GROUND:
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_ATTACK_GROUND, cmd, -1)
                    : Units_OrderAttackGround(g_exec_handles[i],
                                              cmd->target_x, cmd->target_y);
            break;
        case TAK_CMD_GUARD:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_GUARD, cmd, target)
                    : Units_OrderGuard(g_exec_handles[i], target);
            break;
        case TAK_CMD_REPAIR:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_REPAIR, cmd, target)
                    : Units_OrderRepair(g_exec_handles[i], target);
            break;
        case TAK_CMD_RECLAIM:
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_RECLAIM, cmd, target)
                    : Units_OrderReclaim(g_exec_handles[i], target);
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
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_UNLOAD, cmd, -1)
                    : Units_OrderUnload(g_exec_handles[i],
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
                if (mode) {
                    applied += exec_leg(h, UNIT_LEG_SPECIAL, cmd, target);
                    continue;
                }
                Units_OrderSetWeaponSlot(h, 2);
                applied += target >= 0
                         ? Units_OrderAttack(h, target)
                         : Units_OrderMove(h, cmd->target_x, cmd->target_y);
            }
            break;
        case TAK_CMD_RECLAIM_FEATURE:
            /* The sweep resolves on the map cell first and on what
             * stands there second, once per unit ordered
             * (legacy:187131-187199). Deciding here, on the tick, is
             * what keeps the choice the same on every machine. */
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_SWEEP, cmd, target)
                    : Units_OrderReclaimFeature(g_exec_handles[i],
                                                cmd->target_x,
                                                cmd->target_y,
                                                target);
            break;
        case TAK_CMD_RESURRECT_FEATURE:
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_RAISE, cmd, -1)
                    : Units_OrderResurrectFeature(g_exec_handles[i],
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
                int took = mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_BUILD, cmd, -1)
                    : Units_BeginBuildingForUnitFacing(g_exec_handles[i],
                                                       (int)cmd->build_type_id,
                                                       cmd->target_x,
                                                       cmd->target_y,
                                                       cmd->arg & 3u) >= 0;
                if (took) {
                    applied++;
                    break;
                }
            }
            break;
        case TAK_CMD_FACTORY_ENQUEUE: {
            int n = exec_factory_count(cmd);
            int unfinished = (cmd->arg & TAK_FACTORY_UNFINISHED) != 0;
            for (int i = 0; i < count; i++)
                applied += Units_FactoryAdd(g_exec_handles[i],
                                            (int)cmd->build_type_id, n,
                                            unfinished) == 0;
            break;
        }
        case TAK_CMD_FACTORY_DEQUEUE: {
            int n = exec_factory_count(cmd);
            for (int i = 0; i < count; i++)
                applied += Units_FactoryRemove(g_exec_handles[i],
                                               (int)cmd->build_type_id, n) == 0;
            break;
        }
        case TAK_CMD_FACTORY_CANCEL:
            for (int i = 0; i < count; i++)
                applied += Units_FactoryCancelCurrent(g_exec_handles[i]) == 0;
            break;
        case TAK_CMD_RALLY:
            for (int i = 0; i < count; i++) {
                if (mode) {
                    applied += exec_leg(g_exec_handles[i], UNIT_LEG_MOVE, cmd, -1);
                    continue;
                }
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
            /* The attack, for the units that carry cancapture. The
             * mind control shot does the taking (legacy:247761). */
            if (target < 0) break;
            for (int i = 0; i < count; i++)
                applied += mode
                    ? exec_leg(g_exec_handles[i], UNIT_LEG_CAPTURE, cmd, target)
                    : Units_OrderCapture(g_exec_handles[i], target);
            break;
        case TAK_CMD_WAIT:
            /* Declared on the wire, not yet simulated. Refusing it
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
    /* A seat that resigned still loses its army when its player goes. */
    if (cmd->type == TAK_CMD_SEAT_CONTROL) return exec_seat_control(cmd, w);
    /* A seat that resigned issues nothing further. */
    if (w->resigned[cmd->seat]) return 0;

    if (!TAK_CommandTypeTakesUnits(cmd->type)) {
        return exec_seat_command(cmd, w);
    }
    int count = exec_owned_units(cmd);
    if (count <= 0) return 0;
    return exec_unit_command(cmd, count);
}
