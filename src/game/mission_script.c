/*
 * mission_script.c -- a campaign mission's order lists and map script.
 *
 * See tak_mission_script.h for what the two are. Everything here is
 * simulation state: it is in the hash and in the save.
 */

#include "tak_mission_script.h"
#include "tak_mission.h"
#include "tak_bytes.h"
#include "tak_cob.h"
#include "tak_cob_vm.h"
#include "tak_memory.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>

#define MS_CELL          16     /* a script square, in pixels */
#define MS_TICKS_PER_SEC 60
#define MS_MAX_UNITS     2048
#define MS_PROGRAMS      1024
#define MS_STEPS         16384
#define MS_IDENTS        256
#define MS_IDENT_LEN     32
/* Orders that take no time are worked through in one tick, this many
 * at most so a list of nothing else cannot hold the tick. */
#define MS_STEPS_PER_TICK 16
/* How near a patrol point counts as reached, in pixels. */
#define MS_PATROL_REACH  64

#define MS_CREATED_BIT 0x80000000u   /* legacy:178229 */

enum {
    MS_STEP_NONE = 0,
    MS_STEP_WAIT,           /* a seconds, b squares to wake at */
    MS_STEP_WAIT_ATTACK,    /* ref ident, or -1 for the unit itself */
    MS_STEP_MOVE,           /* a, b squares */
    MS_STEP_PATROL,         /* a, b squares */
    MS_STEP_ATTACK_TYPE,    /* ref unit type */
    MS_STEP_ATTACK_IDENT,   /* ref ident */
    MS_STEP_ATTACK_PLACE,   /* a, b squares */
    MS_STEP_BUILD,          /* ref unit type, a count, b, c squares */
    MS_STEP_BOARD,          /* ref ident */
    MS_STEP_ORDERS,         /* a level */
    MS_STEP_SELF_DESTRUCT,
    MS_STEP_UNLOAD,         /* a, b squares */
    MS_STEP_SKIP            /* cloak, selectable, speed, unknown */
};

typedef struct MsStep {
    uint8_t type;
    uint8_t pad;
    int16_t ref;
    int16_t a, b, c;
} MsStep;

typedef struct MsProgram {
    int16_t  unit;          /* -1 when the slot is free */
    uint8_t  started;       /* the order at pc has been given */
    uint8_t  pad;
    uint32_t stable_id;
    uint16_t first, count, pc;
    int16_t  watch;         /* wait-for-attack: whose health is marked */
    int32_t  wait_left;
    int32_t  mark;
} MsProgram;

typedef struct MsTrigger {
    uint8_t used;
    uint8_t round;
    int16_t x, z, x2, z2;
    int32_t radius;
} MsTrigger;

static struct {
    int        active;
    int        local_player;
    int        unit_limit;
    int        verdict;
    int        done_orders;
    int        shake_magnitude, shake_frames;
    uint32_t   tick;

    CobScript *script;
    int        script_owned;
    CobEngine  engine;
    int        engine_up;

    MsTrigger  trigger[TAK_MS_TRIGGERS];
    uint32_t   unit_id[MS_MAX_UNITS];
    uint32_t   unit_bits[MS_MAX_UNITS];

    MsProgram  program[MS_PROGRAMS];
    int16_t    program_of[MS_MAX_UNITS];   /* slot + 1, 0 for none */
    MsStep     step[MS_STEPS];
    int        steps_used;

    char       ident[MS_IDENTS][MS_IDENT_LEN];
    int16_t    ident_unit[MS_IDENTS];
    uint32_t   ident_id[MS_IDENTS];
    int        ident_count;
} ms;

static MissionScript_SoundFn g_ms_sound;
static MissionScript_SpawnFn g_ms_spawn;
static MissionScript_ManaFn  g_ms_mana;

/* ── units ────────────────────────────────────────────────────────── */

static const Unit *ms_unit(int handle) {
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (!units || handle < 0 || handle >= n) return NULL;
    if (units[handle].alive != UNIT_ALIVE_ACTIVE) return NULL;
    return &units[handle];
}

static const Unit *ms_unit_of_script(int32_t id) {
    return id > 0 ? ms_unit((int)id - 1) : NULL;
}

/* max + min / 4, the original's distance (legacy:254574). */
static int32_t ms_distance(int32_t dx, int32_t dz) {
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    return dz < dx ? (dz >> 2) + dx : (dx >> 2) + dz;
}

static int16_t ms_clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* ── idents ───────────────────────────────────────────────────────── */

static int ms_ident_index(const char *name, int create) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < ms.ident_count; i++) {
        if (tak_stricmp(ms.ident[i], name) == 0) return i;
    }
    if (!create || ms.ident_count >= MS_IDENTS) return -1;
    int i = ms.ident_count++;
    memset(ms.ident[i], 0, MS_IDENT_LEN);
    strncpy(ms.ident[i], name, MS_IDENT_LEN - 1);
    ms.ident_unit[i] = -1;
    ms.ident_id[i] = 0;
    return i;
}

static int ms_ident_unit(int index) {
    if (index < 0 || index >= ms.ident_count) return -1;
    int h = ms.ident_unit[index];
    const Unit *u = ms_unit(h);
    if (!u || u->stable_id != ms.ident_id[index]) return -1;
    return h;
}

void MissionOrders_NameUnit(const char *ident, int handle) {
    const Unit *u = ms_unit(handle);
    int i = ms_ident_index(ident, 1);
    if (i < 0 || !u) return;
    ms.ident_unit[i] = (int16_t)handle;
    ms.ident_id[i] = u->stable_id;
}

/* ── order lists ──────────────────────────────────────────────────── */

static void ms_program_free(MsProgram *p) {
    if (p->unit >= 0 && p->unit < MS_MAX_UNITS) ms.program_of[p->unit] = 0;
    p->unit = -1;
}

/* Close the holes finished lists leave in the step pool. A list keeps
 * all its steps, a patrol round goes back over them. */
static void ms_steps_compact(void) {
    static MsStep packed[MS_STEPS];
    int used = 0;
    for (int i = 0; i < MS_PROGRAMS; i++) {
        MsProgram *p = &ms.program[i];
        if (p->unit < 0) continue;
        memcpy(&packed[used], &ms.step[p->first],
               (size_t)p->count * sizeof(MsStep));
        p->first = (uint16_t)used;
        used += p->count;
    }
    memset(ms.step, 0, sizeof(ms.step));
    memcpy(ms.step, packed, (size_t)used * sizeof(MsStep));
    ms.steps_used = used;
}

static void ms_step_from(const MissionCommand *c, MsStep *s) {
    memset(s, 0, sizeof(*s));
    s->ref = -1;
    s->a = ms_clamp16(c->a);
    s->b = ms_clamp16(c->b);
    s->c = ms_clamp16(c->c);
    switch (c->type) {
    case MISSION_CMD_WAIT:   s->type = MS_STEP_WAIT; break;
    case MISSION_CMD_WAIT_ANIMATION:
        s->type = MS_STEP_WAIT_ATTACK;
        s->ref = ms_clamp16(ms_ident_index(c->text, 1));
        break;
    case MISSION_CMD_MOVE:   s->type = MS_STEP_MOVE; break;
    case MISSION_CMD_PATROL: s->type = MS_STEP_PATROL; break;
    case MISSION_CMD_UNLOAD: s->type = MS_STEP_UNLOAD; break;
    case MISSION_CMD_ATTACK:
        if (!c->text[0]) { s->type = MS_STEP_ATTACK_PLACE; break; }
        /* A unit type when there is one of that name, and an Ident
         * when there is not (legacy:228381-228398). */
        {
            int def = Units_FindDefByName(c->text);
            if (def >= 0) {
                s->type = MS_STEP_ATTACK_TYPE;
                s->ref = ms_clamp16(def);
            } else {
                s->type = MS_STEP_ATTACK_IDENT;
                s->ref = ms_clamp16(ms_ident_index(c->text, 1));
            }
        }
        break;
    case MISSION_CMD_BUILD: {
        int def = Units_FindDefByName(c->text);
        s->type = def >= 0 ? MS_STEP_BUILD : MS_STEP_SKIP;
        s->ref = ms_clamp16(def);
        break;
    }
    case MISSION_CMD_BOARD:
        s->type = MS_STEP_BOARD;
        s->ref = ms_clamp16(ms_ident_index(c->text, 1));
        break;
    case MISSION_CMD_ORDERS:        s->type = MS_STEP_ORDERS; break;
    case MISSION_CMD_SELF_DESTRUCT: s->type = MS_STEP_SELF_DESTRUCT; break;
    default:                        s->type = MS_STEP_SKIP; break;
    }
}

int MissionOrders_Give(int handle, const char *text) {
    const Unit *u = ms_unit(handle);
    if (!ms.active || !u || handle >= MS_MAX_UNITS) return 0;
    if (ms.program_of[handle]) {
        ms_program_free(&ms.program[ms.program_of[handle] - 1]);
    }

    MissionCommand *cmds = NULL;
    int n = 0;
    if (Mission_ParseInitialMission(text, &cmds, &n) != 0 || n <= 0) {
        Mission_FreeCommands(cmds);
        return 0;
    }
    if (n > 0xffff) n = 0xffff;
    if (ms.steps_used + n > MS_STEPS) ms_steps_compact();
    int slot = -1;
    for (int i = 0; i < MS_PROGRAMS && slot < 0; i++) {
        if (ms.program[i].unit < 0) slot = i;
    }
    if (slot < 0 || ms.steps_used + n > MS_STEPS) {
        fprintf(stderr, "Mission: no room for the orders of unit %d\n", handle);
        Mission_FreeCommands(cmds);
        return 0;
    }
    MsProgram *p = &ms.program[slot];
    memset(p, 0, sizeof(*p));
    p->unit = (int16_t)handle;
    p->stable_id = u->stable_id;
    p->first = (uint16_t)ms.steps_used;
    p->count = (uint16_t)n;
    p->watch = -1;
    for (int i = 0; i < n; i++) {
        ms_step_from(&cmds[i], &ms.step[ms.steps_used + i]);
    }
    ms.steps_used += n;
    ms.program_of[handle] = (int16_t)(slot + 1);
    Mission_FreeCommands(cmds);
    return n;
}

int MissionOrders_Running(int handle) {
    if (handle < 0 || handle >= MS_MAX_UNITS) return 0;
    return ms.program_of[handle] != 0;
}

int MissionOrders_DebugDone(void) { return ms.done_orders; }

static int ms_idle(const Unit *u) {
    return u->cmd_kind == UNIT_CMD_NONE;
}

static int ms_enemy_within(const Unit *u, int32_t reach) {
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; i < n; i++) {
        const Unit *o = &units[i];
        if (o->alive != UNIT_ALIVE_ACTIVE) continue;
        if (!Units_PlayersAreEnemies(u->player_id, o->player_id)) continue;
        if (ms_distance(o->world_x - u->world_x,
                        o->world_y - u->world_y) <= reach) return 1;
    }
    return 0;
}

/* The nearest unit of a type this one may attack, or -1
 * (legacy:8963-9031, without the jitter the original adds to the
 * distance). */
static int ms_nearest_of_type(int handle, const Unit *u, int def) {
    int n = 0, best = -1;
    int32_t best_d = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; i < n; i++) {
        const Unit *o = &units[i];
        if (o->alive != UNIT_ALIVE_ACTIVE || (int)o->def_idx != def) continue;
        if (o->player_id == u->player_id) continue;
        if (!Units_CanAttackTarget(handle, i)) continue;
        int32_t d = ms_distance(o->world_x - u->world_x,
                                o->world_y - u->world_y);
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

/* Where the run of patrol orders that holds `at` begins and ends. */
static void ms_patrol_run(const MsProgram *p, int at, int *from, int *to) {
    const MsStep *s = &ms.step[p->first];
    int a = at, b = at;
    while (a > 0 && s[a - 1].type == MS_STEP_PATROL) a--;
    while (b + 1 < p->count && s[b + 1].type == MS_STEP_PATROL) b++;
    *from = a;
    *to = b;
}

/* 1 when the order at pc is finished and the list moves on. */
static int ms_step_run(MsProgram *p, int handle, const Unit *u) {
    MsStep *s = &ms.step[p->first + p->pc];
    switch (s->type) {
    case MS_STEP_WAIT:
        /* So many seconds, or sooner when an enemy comes within the
         * distance given (legacy:8920-8958). */
        if (!p->started) {
            p->started = 1;
            p->wait_left = (int32_t)s->a * MS_TICKS_PER_SEC;
        }
        if (--p->wait_left <= 0) return 1;
        if (s->b > 0 && ((ms.tick + (uint32_t)handle) % 8u) == 0u &&
            ms_enemy_within(u, (int32_t)s->b * MS_CELL)) return 1;
        return 0;

    case MS_STEP_WAIT_ATTACK: {
        /* Until the named unit is hurt, or this one when none is named
         * (legacy:228706 passes the unit itself as the target). */
        int who = s->ref >= 0 ? ms_ident_unit(s->ref) : handle;
        const Unit *w = ms_unit(who);
        if (!w) return 1;
        if (!p->started || p->watch != who) {
            p->started = 1;
            p->watch = (int16_t)who;
            p->mark = w->health;
            return 0;
        }
        if (w->health < p->mark) return 1;
        p->mark = w->health;
        return 0;
    }

    case MS_STEP_MOVE:
    case MS_STEP_UNLOAD:
    case MS_STEP_ATTACK_PLACE:
        if (!p->started) {
            int took;
            if (s->type == MS_STEP_UNLOAD) {
                took = Units_OrderUnload(handle, s->a * MS_CELL, s->b * MS_CELL);
            } else {
                took = Units_OrderMove(handle, s->a * MS_CELL, s->b * MS_CELL);
            }
            if (!took) return 1;
            p->started = 1;
            return 0;
        }
        return ms_idle(u);

    case MS_STEP_PATROL: {
        int from, to;
        ms_patrol_run(p, p->pc, &from, &to);
        if (from == to) {
            /* One point: the unit's own patrol order does the rest. */
            Units_OrderPatrol(handle, s->a * MS_CELL, s->b * MS_CELL);
            ms_program_free(p);
            return 0;
        }
        /* Several: the round is walked here and never ends. */
        int32_t px = s->a * MS_CELL, py = s->b * MS_CELL;
        if (!p->started) {
            p->started = 1;
            Units_OrderPatrol(handle, px, py);
            return 0;
        }
        if (ms_distance(u->world_x - px, u->world_y - py) <= MS_PATROL_REACH) {
            p->pc = (uint16_t)(p->pc >= to ? from : p->pc + 1);
            p->started = 0;
        }
        return 0;
    }

    case MS_STEP_ATTACK_TYPE:
        /* Every unit of the type in turn until none is left. */
        if (p->started && !ms_idle(u)) return 0;
        {
            int t = ms_nearest_of_type(handle, u, s->ref);
            if (t < 0) return 1;
            if (!Units_OrderAttack(handle, t)) return 1;
            p->started = 1;
            return 0;
        }

    case MS_STEP_ATTACK_IDENT: {
        int t = ms_ident_unit(s->ref);
        if (t < 0) return 1;
        if (p->started && !ms_idle(u)) return 0;
        if (!Units_OrderAttack(handle, t)) return 1;
        p->started = 1;
        return 0;
    }

    case MS_STEP_BUILD: {
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (d && d->max_velocity <= 0.0f) {
            /* A factory: so many of the thing (legacy:228486). */
            int count = s->a > 0 ? s->a : 1;
            for (int i = 0; i < count; i++) Units_FactoryEnqueue(handle, s->ref);
            return 1;
        }
        if (!p->started) {
            if (Units_BeginBuildingForUnit(handle, s->ref, s->b * MS_CELL,
                                           s->c * MS_CELL) < 0) return 1;
            p->started = 1;
            return 0;
        }
        return ms_idle(u);
    }

    case MS_STEP_BOARD: {
        int carrier = ms_ident_unit(s->ref);
        if (carrier >= 0) Units_OrderLoadList(carrier, &handle, 1, 1);
        return 1;
    }

    case MS_STEP_ORDERS:
        if (s->a >= 0 && s->a <= 2) Units_OrderSetAggro(handle, s->a);
        return 1;

    case MS_STEP_SELF_DESTRUCT:
        Units_DebugKillHandle(handle);
        ms_program_free(p);
        return 0;

    default:
        return 1;
    }
}

static void ms_orders_tick(void) {
    for (int i = 0; i < MS_PROGRAMS; i++) {
        MsProgram *p = &ms.program[i];
        if (p->unit < 0) continue;
        int handle = p->unit;
        const Unit *u = ms_unit(handle);
        if (!u || u->stable_id != p->stable_id) { ms_program_free(p); continue; }
        for (int k = 0; k < MS_STEPS_PER_TICK; k++) {
            if (!ms_step_run(p, handle, u) || p->unit < 0) break;
            ms.done_orders++;
            p->pc++;
            p->started = 0;
            if (p->pc >= p->count) { ms_program_free(p); break; }
        }
    }
}

/* ── the map script's host ────────────────────────────────────────── */

static int ms_text_is(const char *text, const char *word, const char **rest) {
    size_t n = strlen(word);
    if (tak_strnicmp(text, word, n) != 0) return 0;
    if (text[n] != '\0' && text[n] != ' ' && text[n] != '\t') return 0;
    const char *r = text + n;
    while (*r == ' ' || *r == '\t') r++;
    if (rest) *rest = r;
    return 1;
}

/* The original has ten seats, and a mission's neutral townsfolk sit in
 * the tenth (legacy:178615). */
static int ms_player_of_script(int32_t p) {
    return (p >= 0 && p < 10) ? (int)p + 1 : -1;
}

static int32_t ms_cmd_create(const char *name, int n_args, const int32_t *a) {
    /* (x, z) for the local player, (player, x, z) for a named one
     * (legacy:178374-178385). */
    int player, x, z;
    if (n_args == 2)      { player = ms.local_player; x = a[0]; z = a[1]; }
    else if (n_args == 3) { player = ms_player_of_script(a[0]); x = a[1]; z = a[2]; }
    else return 0;
    char unitname[32];
    size_t len = 0;
    while (name[len] && name[len] != ' ' && name[len] != '\t' &&
           len + 1 < sizeof(unitname)) { unitname[len] = name[len]; len++; }
    unitname[len] = '\0';
    int def = Units_FindDefByName(unitname);
    if (def < 0 || player < 1) return 0;
    int handle = Units_Spawn(def, player, (player - 1) % 12,
                             x * MS_CELL, z * MS_CELL);
    if (handle < 0) return 0;
    if (g_ms_spawn) g_ms_spawn(handle);
    return handle + 1;
}

static int32_t ms_cmd_set_trigger(int n_args, const int32_t *a) {
    /* (id, x, z, radius) is a circle and (id, x, z, far x, far z) a box
     * (legacy:178631-178652). An id in use is left as it is. */
    if (n_args != 4 && n_args != 5) return 0;
    if (a[0] < 0 || a[0] >= TAK_MS_TRIGGERS) return 0;
    MsTrigger *t = &ms.trigger[a[0]];
    if (t->used) return 0;
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->x = ms_clamp16(a[1]);
    t->z = ms_clamp16(a[2]);
    if (n_args == 4) { t->round = 1; t->radius = a[3]; }
    else { t->x2 = ms_clamp16(a[3]); t->z2 = ms_clamp16(a[4]); }
    return a[0];
}

static int32_t ms_cmd_remove_trigger(int n_args, const int32_t *a) {
    /* Every unit may hit the id again once it is set anew
     * (legacy:178483-178505). */
    if (n_args < 1 || a[0] < 0 || a[0] >= TAK_MS_TRIGGERS) return 0;
    if (!ms.trigger[a[0]].used) return 0;
    ms.trigger[a[0]].used = 0;
    for (int i = 0; i < MS_MAX_UNITS; i++) ms.unit_bits[i] &= ~(1u << a[0]);
    return 0;
}

static int32_t ms_cmd_set_attribute(const char *what, int n_args,
                                    const int32_t *a) {
    /* legacy:178530-178600. */
    if (n_args != 2 || !ms_unit_of_script(a[0])) return 0;
    int handle = (int)a[0] - 1;
    if (tak_stricmp(what, "HealthPercentage") == 0) {
        Units_SetHealthPercent(handle, (int)a[1]);
    } else if (tak_stricmp(what, "ArmorPercentage") == 0) {
        Units_SetArmorPercent(handle, (int)a[1]);
    } else if (tak_stricmp(what, "AttackPercentage") == 0) {
        Units_SetAttackPercent(handle, (int)a[1]);
    } else if (tak_stricmp(what, "ManaPercentage") == 0) {
        float cur = 0.0f, max = 0.0f;
        if (Units_GetMana(handle, &cur, &max)) {
            Units_DebugSetMana(handle, max * (float)a[1] * 0.01f);
        }
    }
    return 0;
}

static int32_t ms_mission_command(void *user, const char *text,
                                  int n_args, const int32_t *a) {
    const char *rest = NULL;
    (void)user;
    if (ms_text_is(text, "create", &rest)) return ms_cmd_create(rest, n_args, a);
    if (ms_text_is(text, "setmission", &rest)) {
        if (n_args == 1 && ms_unit_of_script(a[0])) {
            MissionOrders_Give((int)a[0] - 1, rest);
        }
        return 0;
    }
    if (ms_text_is(text, "settrigger", NULL)) return ms_cmd_set_trigger(n_args, a);
    if (ms_text_is(text, "removetrigger", NULL)) return ms_cmd_remove_trigger(n_args, a);
    if (ms_text_is(text, "getutype", &rest)) {
        /* Nought is "no such type" (legacy:178512). */
        return Units_FindDefByName(rest) + 1;
    }
    if (ms_text_is(text, "capture", NULL)) {
        /* (unit, player): the unit changes hands and the script gets
         * the new one's number (legacy:178601-178628). */
        if (n_args != 2 || !ms_unit_of_script(a[0])) return 0;
        int player = ms_player_of_script(a[1]);
        if (player < 1) return 0;
        return Units_Capture((int)a[0] - 1, player) + 1;
    }
    if (ms_text_is(text, "screenshake", NULL)) {
        /* (magnitude, milliseconds) (legacy:178524). */
        if (n_args == 2) {
            ms.shake_magnitude = (int)a[0];
            ms.shake_frames = (int)a[1] * MS_TICKS_PER_SEC / 1000;
        }
        return 0;
    }
    if (ms_text_is(text, "setattribute", &rest)) {
        return ms_cmd_set_attribute(rest, n_args, a);
    }
    if (ms_text_is(text, "kill", NULL)) {
        if (n_args >= 1 && ms_unit_of_script(a[0])) {
            Units_DebugKillHandle((int)a[0] - 1);
        }
        return 0;
    }
    if (ms_text_is(text, "attack", NULL)) {
        if (n_args == 2 && ms_unit_of_script(a[0]) && ms_unit_of_script(a[1])) {
            Units_OrderAttack((int)a[0] - 1, (int)a[1] - 1);
        }
        return 0;
    }
    return 0;
}

static int32_t ms_player_units(int player) {
    int n = 0, count = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; i < n; i++) {
        if (units[i].alive == UNIT_ALIVE_ACTIVE &&
            units[i].player_id == player) count++;
    }
    return count;
}

/* The map script's GET ports (legacy:178722-178822). */
static int32_t ms_get(void *user, int port, int n_args, const int32_t *args) {
    (void)user;
    int32_t a = n_args > 1 ? args[1] : 0;
    const Unit *u;
    switch (port) {
    case 1:  return (int32_t)(ms.tick / MS_TICKS_PER_SEC);
    case 5:  return ms_player_units(ms_player_of_script(a));
    case 7:  return ms.unit_limit;
    case 30: u = ms_unit_of_script(a); return u ? (int32_t)u->def_idx + 1 : 0;
    case 31: u = ms_unit_of_script(a); return u ? (int32_t)u->player_id - 1 : 10;
    case 35: u = ms_unit_of_script(a); return u ? u->world_x / MS_CELL : 0;
    case 36: u = ms_unit_of_script(a); return u ? u->world_y / MS_CELL : 0;
    case 40: {
        int player = ms_player_of_script(a);
        return (g_ms_mana && player > 0) ? g_ms_mana(player) : 0;
    }
    default: return 0;
    }
}

/* Port 2 calls the mission: 1 won, anything else lost (legacy:178706). */
static void ms_set(void *user, int port, int32_t value) {
    (void)user;
    if (port == 2 && ms.verdict == 0) ms.verdict = value ? 1 : -1;
}

static int32_t ms_play_sound(void *user, const char *name, int32_t arg) {
    (void)user;
    if (g_ms_sound && name) g_ms_sound(name, (int)arg);
    return 0;
}

static int ms_in_trigger(const MsTrigger *t, const Unit *u) {
    int32_t x = u->world_x / MS_CELL, z = u->world_y / MS_CELL;
    if (t->round) return ms_distance(x - t->x, z - t->z) <= t->radius;
    return x >= t->x && x <= t->x2 && z >= t->z && z <= t->z2;
}

static void ms_call(const char *name, const int32_t *args, int n_args) {
    if (!ms.engine_up) return;
    Cob_StartThreadByName(&ms.engine, name, args, n_args);
}

/* Units that have gone, units that have come, and who stands where
 * (legacy:178218-178250, legacy:177932). */
static void ms_events(void) {
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n > MS_MAX_UNITS) n = MS_MAX_UNITS;
    for (int i = 0; i < MS_MAX_UNITS; i++) {
        const Unit *u = (i < n && units[i].alive == UNIT_ALIVE_ACTIVE)
                            ? &units[i] : NULL;
        if (ms.unit_id[i] && (!u || u->stable_id != ms.unit_id[i])) {
            int32_t args[1] = { i + 1 };
            ms.unit_id[i] = 0;
            ms.unit_bits[i] = 0;
            ms_call("UnitDestroyed", args, 1);
        }
        if (!u || Units_IsUnderConstruction(i)) continue;
        if (!ms.unit_id[i]) {
            int32_t args[2] = { i + 1, (int32_t)u->player_id - 1 };
            ms.unit_id[i] = u->stable_id;
            ms.unit_bits[i] = MS_CREATED_BIT;
            ms_call("UnitCreated", args, 2);
        }
        for (int t = 0; t < TAK_MS_TRIGGERS; t++) {
            if (!ms.trigger[t].used || (ms.unit_bits[i] & (1u << t))) continue;
            if (!ms_in_trigger(&ms.trigger[t], u)) continue;
            int32_t args[3] = { t, i + 1, (int32_t)u->player_id - 1 };
            ms.unit_bits[i] |= 1u << t;
            ms_call("TriggerHit", args, 3);
        }
    }
}

/* ── lifetime ─────────────────────────────────────────────────────── */

static void ms_reset(void) {
    memset(&ms, 0, sizeof(ms));
    for (int i = 0; i < MS_PROGRAMS; i++) ms.program[i].unit = -1;
}

static int ms_engine_start(CobScript *script, int owned, int start_index) {
    ms.script = script;
    ms.script_owned = owned;
    if (Cob_EngineInit(&ms.engine, script, 0, NULL) != 0) return -1;
    ms.engine_up = 1;
    Cob_EngineSetHost(&ms.engine, NULL, NULL, ms_get);
    Cob_EngineSetHostSetter(&ms.engine, ms_set);
    Cob_EngineSetHostPlaySound(&ms.engine, ms_play_sound);
    Cob_EngineSetHostRand(&ms.engine, World_ScriptRand);
    Cob_EngineSetHostMissionCommand(&ms.engine, ms_mission_command);
    int32_t args[1] = { start_index };
    ms_call("Start", args, 1);
    return 0;
}

int MissionScript_Begin(const char *stem, int local_player, int start_index) {
    MissionScript_End();
    ms.active = 1;
    ms.local_player = local_player;
    if (!stem || !stem[0]) return 0;
    char path[256];
    snprintf(path, sizeof(path), "missions/missions/%s.cob", stem);
    CobScript *script = NULL;
    if (Cob_Load(&script, path) != 0 || !script) return 0;
    if (ms_engine_start(script, 1, start_index) != 0) {
        Cob_Free(script);
        ms.script = NULL;
        return -1;
    }
    fprintf(stderr, "Mission: map script %s, %u scripts\n", path,
            (unsigned)script->num_scripts);
    return 0;
}

int MissionScript_BeginWith(CobScript *script, int local_player,
                            int start_index) {
    MissionScript_End();
    ms.active = 1;
    ms.local_player = local_player;
    return script ? ms_engine_start(script, 0, start_index) : 0;
}

void MissionScript_End(void) {
    if (ms.engine_up) Cob_EngineFree(&ms.engine);
    if (ms.script && ms.script_owned) Cob_Free(ms.script);
    ms_reset();
}

int MissionScript_Active(void) { return ms.active; }
int MissionScript_HasScript(void) { return ms.engine_up; }
int MissionScript_Verdict(void) { return ms.verdict; }
void MissionScript_SetUnitLimit(int limit) { ms.unit_limit = limit; }

void MissionScript_SetSoundHook(MissionScript_SoundFn fn) { g_ms_sound = fn; }
void MissionScript_SetSpawnHook(MissionScript_SpawnFn fn) { g_ms_spawn = fn; }
void MissionScript_SetManaHook(MissionScript_ManaFn fn) { g_ms_mana = fn; }

int MissionScript_TakeShake(int *magnitude, int *frames) {
    if (ms.shake_frames <= 0 || ms.shake_magnitude <= 0) return 0;
    if (magnitude) *magnitude = ms.shake_magnitude;
    if (frames) *frames = ms.shake_frames;
    ms.shake_magnitude = 0;
    ms.shake_frames = 0;
    return 1;
}

void MissionScript_Tick(void) {
    if (!ms.active) return;
    ms.tick++;
    if (ms.engine_up) {
        ms_events();
        Cob_RunAllThreads(&ms.engine);
    }
    ms_orders_tick();
}

int MissionScript_DebugTrigger(int id, int *x, int *z, int *x2, int *z2,
                               int *radius) {
    if (id < 0 || id >= TAK_MS_TRIGGERS || !ms.trigger[id].used) return 0;
    const MsTrigger *t = &ms.trigger[id];
    if (x) *x = t->x;
    if (z) *z = t->z;
    if (x2) *x2 = t->x2;
    if (z2) *z2 = t->z2;
    if (radius) *radius = t->round ? t->radius : 0;
    return 1;
}

int MissionScript_DebugStatic(int index, int32_t *out) {
    if (!ms.engine_up || !ms.script || index < 0 ||
        (uint32_t)index >= ms.script->num_static_vars) return 0;
    if (out) *out = ms.engine.static_vars[index];
    return 1;
}

/* ── the save and the hash ────────────────────────────────────────── */

#define MS_SAVE_MAGIC 0x3143534du   /* "MSC1" */

typedef struct MsOut { uint8_t *p; unsigned len; } MsOut;

static void out_u8(MsOut *o, uint8_t v)   { if (o->p) o->p[o->len] = v; o->len += 1; }
static void out_u16(MsOut *o, uint16_t v) { if (o->p) tak_put_u16(o->p + o->len, v); o->len += 2; }
static void out_u32(MsOut *o, uint32_t v) { if (o->p) tak_put_u32(o->p + o->len, v); o->len += 4; }
static void out_i16(MsOut *o, int16_t v)  { out_u16(o, (uint16_t)v); }
static void out_i32(MsOut *o, int32_t v)  { out_u32(o, (uint32_t)v); }

static int ms_tracked_units(void) {
    int n = 0;
    for (int i = 0; i < MS_MAX_UNITS; i++) if (ms.unit_id[i]) n = i + 1;
    return n;
}

static void ms_write(MsOut *o) {
    out_u32(o, MS_SAVE_MAGIC);
    out_i32(o, ms.verdict);
    out_u32(o, ms.tick);
    out_i32(o, ms.done_orders);
    out_i32(o, ms.shake_magnitude);
    out_i32(o, ms.shake_frames);
    for (int i = 0; i < TAK_MS_TRIGGERS; i++) {
        const MsTrigger *t = &ms.trigger[i];
        out_u8(o, t->used);
        out_u8(o, t->round);
        out_i16(o, t->x);  out_i16(o, t->z);
        out_i16(o, t->x2); out_i16(o, t->z2);
        out_i32(o, t->radius);
    }
    out_u16(o, (uint16_t)ms.ident_count);
    for (int i = 0; i < ms.ident_count; i++) {
        for (int k = 0; k < MS_IDENT_LEN; k++) out_u8(o, (uint8_t)ms.ident[i][k]);
        out_i16(o, ms.ident_unit[i]);
        out_u32(o, ms.ident_id[i]);
    }
    int tracked = ms_tracked_units();
    out_u16(o, (uint16_t)tracked);
    for (int i = 0; i < tracked; i++) {
        out_u32(o, ms.unit_id[i]);
        out_u32(o, ms.unit_bits[i]);
    }
    uint16_t live = 0;
    for (int i = 0; i < MS_PROGRAMS; i++) if (ms.program[i].unit >= 0) live++;
    out_u16(o, live);
    for (int i = 0; i < MS_PROGRAMS; i++) {
        const MsProgram *p = &ms.program[i];
        if (p->unit < 0) continue;
        out_i16(o, p->unit);
        out_u8(o, p->started);
        out_u32(o, p->stable_id);
        out_u16(o, p->count);
        out_u16(o, p->pc);
        out_i16(o, p->watch);
        out_i32(o, p->wait_left);
        out_i32(o, p->mark);
        for (int k = 0; k < p->count; k++) {
            const MsStep *s = &ms.step[p->first + k];
            out_u8(o, s->type);
            out_i16(o, s->ref);
            out_i16(o, s->a); out_i16(o, s->b); out_i16(o, s->c);
        }
    }
    out_u8(o, (uint8_t)(ms.engine_up ? 1 : 0));
    if (!ms.engine_up) return;
    uint32_t statics = ms.script ? ms.script->num_static_vars : 0u;
    if (!ms.engine.static_vars) statics = 0;
    out_u32(o, statics);
    for (uint32_t k = 0; k < statics; k++) out_i32(o, ms.engine.static_vars[k]);
    for (int t = 0; t < COB_THREADS_PER_UNIT; t++) {
        const CobThread *th = &ms.engine.threads[t];
        out_u32(o, th->pc);
        out_i32(o, th->sleep_remaining);
        out_u32(o, th->signal_mask);
        out_i32(o, th->return_value);
        out_i16(o, th->wait_piece);
        out_u8(o, (uint8_t)th->wait_child);
        out_u8(o, th->wait_axis);
        out_u8(o, th->wait_kind);
        out_u8(o, th->sp);
        out_u8(o, th->alive);
        out_u8(o, th->has_return_value);
        for (int k = 0; k < COB_THREAD_STACK_DEPTH; k++) out_i32(o, th->stack[k]);
    }
}

unsigned MissionScript_SaveSize(void) {
    MsOut o = { NULL, 0 };
    if (!ms.active) return 0;
    ms_write(&o);
    return o.len;
}

void MissionScript_SaveState(unsigned char *out) {
    MsOut o = { out, 0 };
    if (!ms.active || !out) return;
    ms_write(&o);
}

typedef struct MsIn { const uint8_t *p; unsigned len, at; int bad; } MsIn;

static const uint8_t *in_take(MsIn *in, unsigned n) {
    static const uint8_t zero[8] = { 0 };
    if (in->bad || in->at + n > in->len) { in->bad = 1; return zero; }
    const uint8_t *r = in->p + in->at;
    in->at += n;
    return r;
}
static uint8_t  in_u8(MsIn *in)  { return in_take(in, 1)[0]; }
static uint16_t in_u16(MsIn *in) { return tak_get_u16(in_take(in, 2)); }
static uint32_t in_u32(MsIn *in) { return tak_get_u32(in_take(in, 4)); }
static int16_t  in_i16(MsIn *in) { return (int16_t)in_u16(in); }
static int32_t  in_i32(MsIn *in) { return (int32_t)in_u32(in); }

/* Puts a running mission back. MissionScript_Begin has been called for
 * the same mission first, so the script is loaded and its engine is up,
 * and everything it had begun is replaced by what was saved. */
int MissionScript_LoadState(const unsigned char *data, unsigned len) {
    MsIn in = { data, len, 0, 0 };
    if (!ms.active || !data) return -1;
    if (in_u32(&in) != MS_SAVE_MAGIC) return -1;
    ms.verdict = in_i32(&in);
    ms.tick = in_u32(&in);
    ms.done_orders = in_i32(&in);
    ms.shake_magnitude = in_i32(&in);
    ms.shake_frames = in_i32(&in);
    for (int i = 0; i < TAK_MS_TRIGGERS; i++) {
        MsTrigger *t = &ms.trigger[i];
        t->used = in_u8(&in);
        t->round = in_u8(&in);
        t->x = in_i16(&in);  t->z = in_i16(&in);
        t->x2 = in_i16(&in); t->z2 = in_i16(&in);
        t->radius = in_i32(&in);
    }
    int idents = in_u16(&in);
    if (idents > MS_IDENTS) return -1;
    ms.ident_count = idents;
    for (int i = 0; i < idents; i++) {
        for (int k = 0; k < MS_IDENT_LEN; k++) ms.ident[i][k] = (char)in_u8(&in);
        ms.ident[i][MS_IDENT_LEN - 1] = 0;
        ms.ident_unit[i] = in_i16(&in);
        ms.ident_id[i] = in_u32(&in);
    }
    int tracked = in_u16(&in);
    if (tracked > MS_MAX_UNITS) return -1;
    memset(ms.unit_id, 0, sizeof(ms.unit_id));
    memset(ms.unit_bits, 0, sizeof(ms.unit_bits));
    for (int i = 0; i < tracked; i++) {
        ms.unit_id[i] = in_u32(&in);
        ms.unit_bits[i] = in_u32(&in);
    }
    for (int i = 0; i < MS_PROGRAMS; i++) ms.program[i].unit = -1;
    memset(ms.program_of, 0, sizeof(ms.program_of));
    ms.steps_used = 0;
    int live = in_u16(&in);
    if (live > MS_PROGRAMS) return -1;
    for (int i = 0; i < live && !in.bad; i++) {
        MsProgram *p = &ms.program[i];
        memset(p, 0, sizeof(*p));
        p->unit = in_i16(&in);
        p->started = in_u8(&in);
        p->stable_id = in_u32(&in);
        p->count = in_u16(&in);
        p->pc = in_u16(&in);
        p->watch = in_i16(&in);
        p->wait_left = in_i32(&in);
        p->mark = in_i32(&in);
        if (p->unit < 0 || p->unit >= MS_MAX_UNITS || p->pc >= p->count ||
            ms.steps_used + p->count > MS_STEPS) {
            p->unit = -1;
            return -1;
        }
        p->first = (uint16_t)ms.steps_used;
        for (int k = 0; k < p->count; k++) {
            MsStep *s = &ms.step[ms.steps_used++];
            memset(s, 0, sizeof(*s));
            s->type = in_u8(&in);
            s->ref = in_i16(&in);
            s->a = in_i16(&in); s->b = in_i16(&in); s->c = in_i16(&in);
        }
        ms.program_of[p->unit] = (int16_t)(i + 1);
    }
    int had_engine = in_u8(&in);
    if (in.bad) return -1;
    if (!had_engine) return 0;
    uint32_t statics = in_u32(&in);
    uint32_t mine = (ms.engine_up && ms.script && ms.engine.static_vars)
                        ? ms.script->num_static_vars : 0u;
    /* The mission's script is not the one the save was made with. */
    if (!ms.engine_up || statics != mine) return -1;
    for (uint32_t k = 0; k < statics; k++) ms.engine.static_vars[k] = in_i32(&in);
    int alive = 0;
    for (int t = 0; t < COB_THREADS_PER_UNIT; t++) {
        CobThread *th = &ms.engine.threads[t];
        th->pc = in_u32(&in);
        th->sleep_remaining = in_i32(&in);
        th->signal_mask = in_u32(&in);
        th->return_value = in_i32(&in);
        th->wait_piece = in_i16(&in);
        th->wait_child = (int8_t)in_u8(&in);
        th->wait_axis = in_u8(&in);
        th->wait_kind = in_u8(&in);
        th->sp = in_u8(&in);
        th->alive = in_u8(&in);
        th->has_return_value = in_u8(&in);
        for (int k = 0; k < COB_THREAD_STACK_DEPTH; k++) th->stack[k] = in_i32(&in);
        if (th->sp > COB_THREAD_STACK_DEPTH) return -1;
        if (th->alive && th->pc >= ms.script->num_code_words) return -1;
        alive += th->alive ? 1 : 0;
    }
    if (in.bad) return -1;
    ms.engine.active_thread_count = (uint16_t)alive;
    return 0;
}

/* The save's bytes are the state, so they are what is hashed. */
uint32_t TAK_SimHash_Mission(uint32_t h) {
    static uint8_t buf[1 << 19];
    MsOut o = { NULL, 0 };
    if (!ms.active) return h;
    ms_write(&o);
    if (o.len > sizeof(buf)) return TAK_HashU32(h, o.len);
    o.p = buf;
    o.len = 0;
    ms_write(&o);
    unsigned whole = o.len & ~3u;
    for (unsigned i = 0; i < whole; i += 4) h = TAK_HashU32(h, tak_get_u32(buf + i));
    for (unsigned i = whole; i < o.len; i++) h = TAK_HashU32(h, buf[i]);
    return h;
}
