/*
 * map_scenario.c -- a scripted multiplayer map's .crt: the units each
 * start begins with and the rules read once a game second.
 *
 * See tak_map_scenario.h. The parsed file is the map's and is read again
 * on every load. What the rules change is simulation state: it is in the
 * hash and in the save.
 */

#include "tak_map_scenario.h"
#include "tak_battle_config.h"
#include "tak_bytes.h"
#include "tak_economy.h"
#include "tak_hpi.h"
#include "tak_maps.h"
#include "tak_memory.h"
#include "tak_occupancy.h"
#include "tak_sides.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_tdf.h"
#include "tak_unit.h"
#include "tak_util.h"
#include "tak_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_TICKS_PER_SEC 60
#define SC_CELL          16
#define SC_SECTIONS      9      /* every player, then the eight starts */
#define SC_PARAM         64
#define SC_PARAMS        5
#define SC_TEXT          (SC_PARAM * 2 + 8)
#define SC_MAX_COUNT     4096   /* a count past this is a broken file */
#define SC_TIMERS        64
#define SC_TALLIES       128
#define SC_SIDE_COUNTER  's'    /* legacy:173060 */

/* Record sizes in the file (legacy:172631-172850). */
#define SC_MOD_BYTES     0x110
#define SC_PLACE_BYTES   0x238
#define SC_ENTRY_BYTES   0x144
#define SC_AREA_BYTES    0x110

/* Conditions (legacy:171181-171415) and actions (legacy:171416-172122),
 * numbered as the file numbers them. */
enum {
    SC_C_START = 0, SC_C_TIME_PAST, SC_C_TIME_BEFORE, SC_C_TIMER_ABOVE,
    SC_C_TIMER_BELOW, SC_C_KILLS_ABOVE, SC_C_KILLS_BELOW, SC_C_KILLS_MOST,
    SC_C_KILLS_LEAST, SC_C_LOSSES_ABOVE, SC_C_LOSSES_BELOW, SC_C_LOSSES_MOST,
    SC_C_LOSSES_LEAST, SC_C_AREA_MOST, SC_C_AREA_LEAST, SC_C_AREA_ABOVE,
    SC_C_AREA_BELOW, SC_C_COUNTER_ABOVE, SC_C_COUNTER_BELOW, SC_C_ALWAYS,
    SC_C_NONE, SC_C_ENEMIES_BELOW, SC_C_ENEMIES_ABOVE, SC_C_CHANCE,
    SC_C_MANA_BELOW, SC_C_MANA_ABOVE, SC_C_SIDE_IS
};
enum {
    SC_A_TIMER_DOWN = 0, SC_A_TIMER_UP, SC_A_COUNTER_SET, SC_A_COUNTER_ADD,
    SC_A_COUNTER_SUB, SC_A_WIN, SC_A_LOSE, SC_A_CREATE, SC_A_KILL,
    SC_A_CAPTURE, SC_A_HEAL, SC_A_HARM, SC_A_CLOCK, SC_A_MESSAGE,
    SC_A_ONCE, SC_A_MOVE, SC_A_HOLD_INCOME, SC_A_MANA_SET, SC_A_MANA_ADD,
    SC_A_MANA_SUB, SC_A_RESUME_INCOME, SC_A_ALLIES_WIN, SC_A_ALLIES_LOSE,
    SC_A_ENEMIES_WIN, SC_A_ENEMIES_LOSE, SC_A_COUNTER_MESSAGE, SC_A_MONARCH
};

typedef struct ScCond {
    int32_t type, a, b, area;
    int16_t def;            /* -1 for any unit */
    uint8_t ch;             /* a counter's letter */
} ScCond;

typedef struct ScAct {
    int32_t type, a, b, area, area2, to;
    int16_t def;
    uint8_t ch;
    char    text[SC_TEXT];
    char    tail[SC_PARAM]; /* after a counter message's number */
} ScAct;

typedef struct ScRule {
    int32_t section, first_cond, n_cond, first_act, n_act;
} ScRule;

typedef struct ScArea { int32_t x1, z1, x2, z2; char name[SC_PARAM]; } ScArea;

typedef struct ScPlace {
    int16_t def;
    uint8_t start;
    int32_t x, z, health, armor, attack, vet;
} ScPlace;

typedef struct ScMod { int16_t def; int32_t armor, attack, vet; } ScMod;

typedef struct ScTimer { int32_t id; uint8_t up; int32_t value; } ScTimer;
typedef struct ScTally { int16_t def; int32_t count; } ScTally;

typedef struct ScSeat {
    uint8_t  in;            /* plays this scenario */
    uint8_t  start;         /* its start, 0 based in StartPos order */
    uint8_t  out;           /* beaten by the rules (legacy:206573) */
    int32_t  hold;          /* above 0 stops its income (legacy:235961) */
    uint8_t *on;            /* per rule, cleared by a run-once action */
    int      n_timers;
    ScTimer  timer[SC_TIMERS];
    uint8_t  counter_on[256];
    int32_t  counter[256];
} ScSeat;

static struct {
    int      active;
    uint32_t tick, second;
    uint8_t  starting;      /* the first look, at game start */
    uint8_t  clock;
    uint32_t mods_next;     /* the first stable id not yet given its mod */
    ScSeat   seat[TAK_MAX_PLAYERS + 1];
    int      n_kills[TAK_MAX_PLAYERS + 1], n_losses[TAK_MAX_PLAYERS + 1];
    ScTally  kills[TAK_MAX_PLAYERS + 1][SC_TALLIES];
    ScTally  losses[TAK_MAX_PLAYERS + 1][SC_TALLIES];

    ScRule  *rules;  int n_rules;
    ScCond  *conds;  int n_conds;
    ScAct   *acts;   int n_acts;
    ScArea  *areas;  int n_areas;
    ScPlace *places; int n_places;
    ScMod   *mods;   int n_mods;
} sc;

static MapScenario_SpawnFn   g_sc_spawn;
static MapScenario_MessageFn g_sc_message;

/* ── reading the file ─────────────────────────────────────────────── */

typedef struct ScIn { const uint8_t *p; size_t len, at; int bad; } ScIn;

static const uint8_t *sc_take(ScIn *in, size_t n) {
    if (in->bad || in->len - in->at < n) { in->bad = 1; return NULL; }
    const uint8_t *p = in->p + in->at;
    in->at += n;
    return p;
}

static int32_t sc_i32(ScIn *in) {
    const uint8_t *p = sc_take(in, 4);
    return p ? (int32_t)tak_get_u32(p) : 0;
}

static int32_t sc_count(ScIn *in) {
    int32_t n = sc_i32(in);
    if (n < 0 || n > SC_MAX_COUNT) in->bad = 1;
    return in->bad ? 0 : n;
}

/* A fixed text field, cut at its first NUL. */
static void sc_text(const uint8_t *p, size_t cap, char *out, size_t out_cap) {
    size_t n = 0;
    while (n < cap && n + 1 < out_cap && p[n]) { out[n] = (char)p[n]; n++; }
    out[n] = '\0';
}

/* The C library's atoi, as the original reads every number. */
static int32_t sc_atoi(const char *s) { return (int32_t)atoi(s); }

/* A name no unit has, or none, is any unit (legacy:229614). */
static int16_t sc_def(const char *name) {
    int d = name[0] ? Units_FindDefByName(name) : -1;
    return (int16_t)(d >= 0 && d < 0x7fff ? d : -1);
}

/* An area by name, the whole name, any case. -1 when none has it
 * (legacy:172232-172296). Anywhere is the first. */
static int32_t sc_area(const char *name) {
    for (int i = 0; i < sc.n_areas; i++)
        if (tak_stricmp(sc.areas[i].name, name) == 0) return i;
    return -1;
}

/* "Player N" is the Nth start, "All Players" every one (legacy:170976). */
static int32_t sc_to(const char *p0) {
    size_t n = strlen(p0);
    if (tak_strnicmp(p0, "All Players", n < 12 ? 11 : n) == 0) return -1;
    return n > 7 ? sc_atoi(p0 + 7) - 1 : -1;
}

static void sc_free(void) {
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) tak_free(sc.seat[p].on);
    tak_free(sc.rules);
    tak_free(sc.conds);
    tak_free(sc.acts);
    tak_free(sc.areas);
    tak_free(sc.places);
    tak_free(sc.mods);
    memset(&sc, 0, sizeof(sc));
}

/* The compiled form of a condition (legacy:170797-170886). */
static void sc_compile_cond(ScCond *c, int32_t type, char p[SC_PARAMS][SC_PARAM]) {
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->def = -1;
    c->area = -1;
    switch (type) {
    case SC_C_TIME_PAST: case SC_C_TIME_BEFORE: case SC_C_ENEMIES_BELOW:
    case SC_C_ENEMIES_ABOVE: case SC_C_CHANCE: case SC_C_MANA_BELOW:
    case SC_C_MANA_ABOVE: case SC_C_SIDE_IS:
        c->a = sc_atoi(p[0]);
        break;
    case SC_C_TIMER_ABOVE: case SC_C_TIMER_BELOW:
        c->a = sc_atoi(p[0]);
        c->b = sc_atoi(p[1]);
        break;
    case SC_C_KILLS_ABOVE: case SC_C_KILLS_BELOW:
    case SC_C_LOSSES_ABOVE: case SC_C_LOSSES_BELOW:
        c->a = sc_atoi(p[0]);
        c->def = sc_def(p[1]);
        break;
    case SC_C_KILLS_MOST: case SC_C_KILLS_LEAST:
    case SC_C_LOSSES_MOST: case SC_C_LOSSES_LEAST:
        c->def = sc_def(p[0]);
        break;
    case SC_C_AREA_MOST: case SC_C_AREA_LEAST:
        c->def = sc_def(p[0]);
        c->area = sc_area(p[1]);
        break;
    case SC_C_AREA_ABOVE: case SC_C_AREA_BELOW:
        c->a = sc_atoi(p[0]);
        c->def = sc_def(p[1]);
        c->area = sc_area(p[2]);
        break;
    case SC_C_COUNTER_ABOVE: case SC_C_COUNTER_BELOW:
        c->ch = (uint8_t)p[0][0];
        c->a = sc_atoi(p[1]);
        break;
    default:
        break;
    }
}

/* The compiled form of an action (legacy:170888-171149). */
static void sc_compile_act(ScAct *a, int32_t type, char p[SC_PARAMS][SC_PARAM]) {
    memset(a, 0, sizeof(*a));
    a->type = type;
    a->def = -1;
    a->area = a->area2 = -1;
    switch (type) {
    case SC_A_TIMER_DOWN: case SC_A_TIMER_UP:
        a->a = sc_atoi(p[0]);
        a->b = sc_atoi(p[1]);
        break;
    case SC_A_COUNTER_SET:
        a->ch = (uint8_t)p[0][0];
        a->a = sc_atoi(p[1]);
        break;
    case SC_A_COUNTER_ADD: case SC_A_COUNTER_SUB:
        a->a = sc_atoi(p[0]);
        a->ch = (uint8_t)p[1][0];
        break;
    case SC_A_CREATE: case SC_A_KILL: case SC_A_CAPTURE:
        a->def = sc_def(p[0]);
        a->area = sc_area(p[1]);
        break;
    case SC_A_HEAL: case SC_A_HARM:
        a->def = sc_def(p[0]);
        a->a = sc_atoi(p[1]);
        a->area = sc_area(p[2]);
        break;
    case SC_A_MESSAGE:
        a->to = sc_to(p[0]);
        snprintf(a->text, sizeof a->text, "%s", p[1]);
        break;
    case SC_A_MOVE:
        a->def = sc_def(p[0]);
        a->area = sc_area(p[1]);
        a->area2 = sc_area(p[2]);
        break;
    case SC_A_HOLD_INCOME: case SC_A_MANA_SET: case SC_A_MANA_ADD:
    case SC_A_MANA_SUB:
        a->a = sc_atoi(p[0]);
        break;
    case SC_A_COUNTER_MESSAGE:
        a->to = sc_to(p[0]);
        a->ch = (uint8_t)p[2][0];
        snprintf(a->text, sizeof a->text, "%s", p[1]);
        snprintf(a->tail, sizeof a->tail, "%s", p[3]);
        break;
    default:
        break;
    }
}

/* Two passes over the file: count, then fill. The layout is the
 * original's reader (legacy:172631-172850, legacy:172851-173084). */
static int sc_parse(const uint8_t *data, size_t len, int fill) {
    ScIn in = { data, len, 0, 0 };
    const uint8_t *v = sc_take(&in, 4);
    if (!v || tak_get_u32(v) != 0x3f800000u) return -1;   /* 1.0f */

    int n_mods = sc_count(&in);
    for (int i = 0; i < n_mods; i++) {
        const uint8_t *r = sc_take(&in, SC_MOD_BYTES);
        if (!r || !fill) continue;
        char name[SC_PARAM];
        sc_text(r, 0x100, name, sizeof name);
        ScMod *m = &sc.mods[i];
        m->def = sc_def(name);
        m->armor  = (int32_t)tak_get_u32(r + 0x104);
        m->attack = (int32_t)tak_get_u32(r + 0x108);
        m->vet    = (int32_t)tak_get_u32(r + 0x10c);
    }
    int n_places = sc_count(&in);
    for (int i = 0; i < n_places; i++) {
        const uint8_t *r = sc_take(&in, SC_PLACE_BYTES);
        if (!r || !fill) continue;
        char name[SC_PARAM];
        sc_text(r, 0x100, name, sizeof name);
        ScPlace *pl = &sc.places[i];
        pl->def    = sc_def(name);
        pl->x      = (int16_t)tak_get_u16(r + 0x200);
        pl->z      = (int16_t)tak_get_u16(r + 0x208);
        pl->start  = (uint8_t)tak_get_u32(r + 0x20c);
        pl->health = (int32_t)tak_get_u32(r + 0x210);
        pl->armor  = (int32_t)tak_get_u32(r + 0x214);
        pl->attack = (int32_t)tak_get_u32(r + 0x218);
        pl->vet    = (int32_t)tak_get_u32(r + 0x220);
    }

    /* The rules come before the areas they name, so the first pass
     * finds where the areas are and the second reads them first. */
    size_t rules_at = in.at;
    int n_sections = sc_count(&in);
    int n_rules = 0, n_conds = 0, n_acts = 0;
    for (int s = 0; s < n_sections; s++) {
        int nr = sc_count(&in);
        for (int r = 0; r < nr; r++) {
            int nc = sc_count(&in);
            if (!sc_take(&in, (size_t)nc * SC_ENTRY_BYTES)) break;
            int na = sc_count(&in);
            if (!sc_take(&in, (size_t)na * SC_ENTRY_BYTES)) break;
            n_rules++;
            n_conds += nc;
            n_acts += na;
        }
    }
    int n_areas = sc_count(&in);
    size_t areas_at = in.at;
    if (!sc_take(&in, (size_t)n_areas * SC_AREA_BYTES) || in.bad) return -1;
    if (in.at != len) return -1;

    if (!fill) {
        sc.n_mods = n_mods;
        sc.n_places = n_places;
        sc.n_rules = n_rules;
        sc.n_conds = n_conds;
        sc.n_acts = n_acts;
        sc.n_areas = n_areas + 1;
        return 0;
    }

    /* Anywhere, the map and far past it (legacy:172994-173020). */
    GameWorld *w = World_Get();
    int32_t mw = w ? w->map_pixels_w / SC_CELL : 0;
    int32_t mh = w ? w->map_pixels_h / SC_CELL : 0;
    ScArea *any = &sc.areas[0];
    any->x1 = -10000; any->z1 = -10000;
    any->x2 = mw + 10000; any->z2 = mh + 10000;
    snprintf(any->name, sizeof any->name, "Anywhere");
    for (int i = 0; i < n_areas; i++) {
        const uint8_t *r = data + areas_at + (size_t)i * SC_AREA_BYTES;
        ScArea *a = &sc.areas[i + 1];
        sc_text(r, 0x100, a->name, sizeof a->name);
        a->x1 = (int32_t)tak_get_u32(r + 0x100);
        a->z1 = (int32_t)tak_get_u32(r + 0x104);
        a->x2 = (int32_t)tak_get_u32(r + 0x108);
        a->z2 = (int32_t)tak_get_u32(r + 0x10c);
    }

    in.at = rules_at;
    n_sections = sc_count(&in);
    int ri = 0, ci = 0, ai = 0;
    char p[SC_PARAMS][SC_PARAM];
    for (int s = 0; s < n_sections; s++) {
        int nr = sc_count(&in);
        for (int r = 0; r < nr; r++) {
            ScRule *rule = &sc.rules[ri++];
            rule->section = s;
            rule->n_cond = sc_count(&in);
            rule->first_cond = ci;
            for (int k = 0; k < rule->n_cond; k++) {
                const uint8_t *e = sc_take(&in, SC_ENTRY_BYTES);
                for (int q = 0; q < SC_PARAMS; q++)
                    sc_text(e + 4 + q * SC_PARAM, SC_PARAM, p[q], SC_PARAM);
                sc_compile_cond(&sc.conds[ci++], (int32_t)tak_get_u32(e), p);
            }
            rule->n_act = sc_count(&in);
            rule->first_act = ai;
            for (int k = 0; k < rule->n_act; k++) {
                const uint8_t *e = sc_take(&in, SC_ENTRY_BYTES);
                for (int q = 0; q < SC_PARAMS; q++)
                    sc_text(e + 4 + q * SC_PARAM, SC_PARAM, p[q], SC_PARAM);
                sc_compile_act(&sc.acts[ai++], (int32_t)tak_get_u32(e), p);
            }
        }
    }
    return in.bad ? -1 : 0;
}

static int sc_load(const uint8_t *data, size_t len) {
    sc_free();
    if (sc_parse(data, len, 0) != 0) { sc_free(); return -1; }
    sc.mods   = (ScMod *)tak_calloc((size_t)sc.n_mods + 1, sizeof(ScMod));
    sc.places = (ScPlace *)tak_calloc((size_t)sc.n_places + 1, sizeof(ScPlace));
    sc.rules  = (ScRule *)tak_calloc((size_t)sc.n_rules + 1, sizeof(ScRule));
    sc.conds  = (ScCond *)tak_calloc((size_t)sc.n_conds + 1, sizeof(ScCond));
    sc.acts   = (ScAct *)tak_calloc((size_t)sc.n_acts + 1, sizeof(ScAct));
    sc.areas  = (ScArea *)tak_calloc((size_t)sc.n_areas, sizeof(ScArea));
    int ok = sc.mods && sc.places && sc.rules && sc.conds && sc.acts && sc.areas;
    for (int p = 1; ok && p <= TAK_MAX_PLAYERS; p++) {
        sc.seat[p].on = (uint8_t *)tak_calloc((size_t)sc.n_rules + 1, 1);
        if (!sc.seat[p].on) ok = 0;
    }
    if (!ok || sc_parse(data, len, 1) != 0) { sc_free(); return -1; }
    return 0;
}

/* ── the seats and their units ────────────────────────────────────── */

static int sc_seat_ok(int p) { return p >= 1 && p <= TAK_MAX_PLAYERS; }

static int sc_units_of(int p) {
    int n = 0, c = 0;
    const Unit *u = Units_GetActive(&n);
    for (int i = 0; u && i < n; i++)
        if (u[i].player_id == p && u[i].alive != UNIT_ALIVE_DEAD) c++;
    return c;
}

/* A player still in the battle: a seat that has units, or has never had
 * any (legacy:171229-171232). */
static int sc_active(int p) {
    GameWorld *w = World_Get();
    if (!w || !sc_seat_ok(p) || w->cfg.players[p - 1].kind == TAK_SLOT_CLOSED) return 0;
    if (w->stats[p].eliminated) return 0;
    return sc_units_of(p) > 0 || w->stats[p].units_built == 0;
}

static int sc_in_area(const Unit *u, int32_t area) {
    if (area < 0 || area >= sc.n_areas) return 0;
    const ScArea *a = &sc.areas[area];
    int32_t cx = u->world_x >> 4, cz = u->world_y >> 4;
    return a->x1 <= cx && cx <= a->x2 && a->z1 <= cz && cz <= a->z2;
}

/* A standing, finished unit of p's of that kind in the area
 * (legacy:172298-172331). */
static int sc_counts(const Unit *u, int p, int16_t def, int32_t area) {
    return u->player_id == p && u->alive == UNIT_ALIVE_ACTIVE &&
           !u->under_construction && (def < 0 || u->def_idx == def) &&
           sc_in_area(u, area);
}

static int32_t sc_count_in(int p, int16_t def, int32_t area) {
    if (area < 0) return 0;
    int n = 0;
    int32_t c = 0;
    const Unit *u = Units_GetActive(&n);
    for (int i = 0; u && i < n; i++) c += sc_counts(&u[i], p, def, area);
    return c;
}

/* A tally by kind. Any unit reads the first kind counted, as the
 * original's list walk does (legacy:172195-172230). */
static int32_t sc_tally(const ScTally *t, int n, int16_t def) {
    for (int i = 0; i < n; i++)
        if (def < 0 || t[i].def == def) return t[i].count;
    return 0;
}

static void sc_tally_add(ScTally *t, int *n, int def) {
    for (int i = 0; i < *n; i++)
        if (t[i].def == def) { t[i].count++; return; }
    if (*n >= SC_TALLIES) return;
    t[*n].def = (int16_t)def;
    t[*n].count = 1;
    (*n)++;
}

void MapScenario_NoteKill(int killer, int def_idx) {
    if (!sc.active || !sc_seat_ok(killer) || def_idx < 0) return;
    sc_tally_add(sc.kills[killer], &sc.n_kills[killer], def_idx);
}

void MapScenario_NoteLoss(int owner, int def_idx) {
    if (!sc.active || !sc_seat_ok(owner) || def_idx < 0) return;
    sc_tally_add(sc.losses[owner], &sc.n_losses[owner], def_idx);
}

/* Percent scales the script gives: nought or less is the unit as
 * authored, as the original takes a scale of 0 (legacy:245324). */
static int sc_pct(int32_t v) { return v > 0 ? (int)v : 100; }

static const ScMod *sc_mod_of(int def) {
    for (int i = 0; i < sc.n_mods; i++)
        if (sc.mods[i].def == def) return &sc.mods[i];
    return NULL;
}

static void sc_apply_mod(int h, int def) {
    const ScMod *m = sc_mod_of(def);
    if (!m) return;
    Units_SetArmorPercent(h, sc_pct(m->armor));
    Units_SetAttackPercent(h, sc_pct(m->attack));
    Units_SetVeteranLevel(h, m->vet);
}

/* The script's mods for every unit made since the last look
 * (legacy:173340-173372). */
static void sc_mods_catch_up(void) {
    if (sc.n_mods > 0) {
        int n = 0;
        const Unit *u = Units_GetActive(&n);
        for (int i = 0; u && i < n; i++)
            if (u[i].alive == UNIT_ALIVE_ACTIVE && u[i].stable_id >= sc.mods_next)
                sc_apply_mod(i, u[i].def_idx);
    }
    sc.mods_next = Units_NextStableId();
}

/* A unit made for p with its footprint starting at the cell, as the
 * original places one (legacy:172792-172794). */
static int sc_make(int p, int def, int32_t cx, int32_t cz) {
    const UnitDef *d = Units_GetDef(def);
    if (!d) return -1;
    int h = Units_Spawn(def, p, Units_PlayerColorIndex(p),
                        cx * SC_CELL + d->footprint_x * 8,
                        cz * SC_CELL + d->footprint_z * 8);
    if (h < 0) return -1;
    if (g_sc_spawn) g_sc_spawn(h);
    sc_apply_mod(h, def);
    return h;
}

/* The first cell of the area no unit holds, row by row, or its middle
 * when every one is held (legacy:172456-172544). */
static int sc_make_in(int p, int def, int32_t area) {
    GameWorld *w = World_Get();
    if (!w || area < 0 || area >= sc.n_areas || def < 0) return -1;
    const ScArea *a = &sc.areas[area];
    int32_t mw = w->map_pixels_w / SC_CELL, mh = w->map_pixels_h / SC_CELL;
    int32_t x1 = a->x1 < 0 ? 0 : a->x1, z1 = a->z1 < 0 ? 0 : a->z1;
    int32_t x2 = a->x2 >= mw ? mw - 1 : a->x2, z2 = a->z2 >= mh ? mh - 1 : a->z2;
    for (int32_t z = z1; z <= z2; z++)
        for (int32_t x = x1; x <= x2; x++)
            if (Occ_QueryTile(w, x, z, 0, 0) == 0) return sc_make(p, def, x, z);
    return sc_make(p, def, x1 + (x2 - x1) / 2, z1 + (z2 - z1) / 2);
}

/* ── verdicts ─────────────────────────────────────────────────────── */

/* A win stands as the seat's call (legacy:206542-206553). */
static void sc_win(int p) {
    GameWorld *w = World_Get();
    if (!w || !sc_seat_ok(p)) return;
    if (!w->console.called[p]) w->console.called[p] = 1;
}

/* A loss under the script takes the seat's army and its turn at the
 * rules (legacy:206558-206576, legacy:227541-227580). */
static void sc_lose(int p) {
    GameWorld *w = World_Get();
    if (!w || !sc_seat_ok(p)) return;
    if (!w->console.called[p]) w->console.called[p] = -1;
    sc.seat[p].out = 1;
    Units_EliminatePlayer(p, -1);
}

static void sc_say(int to_start, int from, const char *text) {
    if (!g_sc_message) return;
    for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
        if (!sc.seat[q].in || !sc_active(q)) continue;
        if (to_start >= 0 && sc.seat[q].start != to_start) continue;
        g_sc_message(q, from, text);
    }
}

/* ── the rules ────────────────────────────────────────────────────── */

static ScTimer *sc_timer(ScSeat *s, int32_t id) {
    for (int i = 0; i < s->n_timers; i++)
        if (s->timer[i].id == id) return &s->timer[i];
    return NULL;
}

static float sc_mana(int p) {
    GameWorld *w = World_Get();
    return w ? w->economy.players[p - 1].mana : 0.0f;
}

/* Every other player still in, held against p's figure: 1 when each
 * of them is below (most) or above (least) it (legacy:171224-171357). */
static int sc_beats_all(int p, int most, int32_t mine,
                        int32_t (*theirs)(int q, const ScCond *c),
                        const ScCond *c) {
    for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
        if (q == p || !sc_active(q)) continue;
        int32_t t = theirs(q, c);
        if (most ? !(t < mine) : !(mine < t)) return 0;
    }
    return 1;
}

static int32_t sc_kills_of(int q, const ScCond *c) {
    return sc_tally(sc.kills[q], sc.n_kills[q], c->def);
}
static int32_t sc_losses_of(int q, const ScCond *c) {
    return sc_tally(sc.losses[q], sc.n_losses[q], c->def);
}
static int32_t sc_area_of(int q, const ScCond *c) {
    return sc_count_in(q, c->def, c->area);
}

static int sc_enemies_in(int p) {
    int n = 0;
    for (int q = 1; q <= TAK_MAX_PLAYERS; q++)
        if (q != p && sc_active(q) && Units_PlayersAreEnemies(p, q)) n++;
    return n;
}

static int sc_holds(int p, const ScCond *c) {
    ScSeat *s = &sc.seat[p];
    GameWorld *w = World_Get();
    switch (c->type) {
    case SC_C_START:        return sc.starting;
    case SC_C_TIME_PAST:    return (uint32_t)c->a < sc.second;
    case SC_C_TIME_BEFORE:  return sc.second < (uint32_t)c->a;
    case SC_C_TIMER_ABOVE: { ScTimer *t = sc_timer(s, c->a); return t && c->b < t->value; }
    case SC_C_TIMER_BELOW: { ScTimer *t = sc_timer(s, c->a); return t && t->value < c->b; }
    case SC_C_KILLS_ABOVE:  return c->a < sc_kills_of(p, c);
    case SC_C_KILLS_BELOW:  return sc_kills_of(p, c) < c->a;
    case SC_C_KILLS_MOST:   return sc_beats_all(p, 1, sc_kills_of(p, c), sc_kills_of, c);
    case SC_C_KILLS_LEAST:  return sc_beats_all(p, 0, sc_kills_of(p, c), sc_kills_of, c);
    case SC_C_LOSSES_ABOVE: return c->a < sc_losses_of(p, c);
    case SC_C_LOSSES_BELOW: return sc_losses_of(p, c) < c->a;
    case SC_C_LOSSES_MOST:  return sc_beats_all(p, 1, sc_losses_of(p, c), sc_losses_of, c);
    case SC_C_LOSSES_LEAST: return sc_beats_all(p, 0, sc_losses_of(p, c), sc_losses_of, c);
    case SC_C_AREA_MOST:    return sc_beats_all(p, 1, sc_area_of(p, c), sc_area_of, c);
    case SC_C_AREA_LEAST:   return sc_beats_all(p, 0, sc_area_of(p, c), sc_area_of, c);
    case SC_C_AREA_ABOVE:   return c->a < sc_area_of(p, c);
    case SC_C_AREA_BELOW:   return sc_area_of(p, c) < c->a;
    case SC_C_COUNTER_ABOVE: return s->counter_on[c->ch] && c->a < s->counter[c->ch];
    case SC_C_COUNTER_BELOW: return s->counter_on[c->ch] && s->counter[c->ch] < c->a;
    case SC_C_ALWAYS:       return 1;
    case SC_C_ENEMIES_BELOW: return sc_enemies_in(p) < c->a;
    case SC_C_ENEMIES_ABOVE: return c->a < sc_enemies_in(p);
    /* rand() over 32768 times 100, the sim's generator for the C
     * library's (legacy:171386-171389). */
    case SC_C_CHANCE:       return (int64_t)World_Rand(32768) * 25 < (int64_t)c->a * 8192;
    case SC_C_MANA_BELOW:   return sc_mana(p) < (float)c->a;
    case SC_C_MANA_ABOVE:   return (float)c->a < sc_mana(p);
    case SC_C_SIDE_IS:      return w && w->cfg.players[p - 1].side == c->a;
    default:                return 0;
    }
}

/* Only a unit that walks is sent (legacy:171742-171760). */
static int sc_walks(const Unit *u) {
    const UnitDef *d = Units_GetDef(u->def_idx);
    return d && d->max_velocity > 0.0f;
}

/* What p's units of the kind in the area take: 0 kill, 1 heal, 2 harm,
 * 3 walk to the other area (legacy:171554-171678, legacy:171731-171805). */
static void sc_each_in(int p, const ScAct *a, int what) {
    if (a->area < 0) return;
    int32_t tx = 0, tz = 0;
    if (what == 3) {
        if (a->area2 < 0 || a->area2 >= sc.n_areas) return;
        const ScArea *b = &sc.areas[a->area2];
        tx = (b->x1 + (b->x2 - b->x1) / 2) * SC_CELL;
        tz = (b->z1 + (b->z2 - b->z1) / 2) * SC_CELL;
    }
    for (int i = 0;; i++) {
        int n = 0;
        const Unit *all = Units_GetActive(&n);
        if (!all || i >= n) break;
        const Unit *u = &all[i];
        if (u->player_id != p || u->alive != UNIT_ALIVE_ACTIVE) continue;
        if (a->def >= 0 && u->def_idx != a->def) continue;
        if (!sc_in_area(u, a->area)) continue;
        if (what == 0) Units_ScriptKill(i);
        else if (what == 1) Units_ScriptHeal(i, a->a);
        else if (what == 2) Units_ScriptHarm(i, a->a);
        else if (!u->under_construction && sc_walks(u)) Units_OrderMove(i, tx, tz);
    }
}

static void sc_capture(int p, const ScAct *a) {
    if (a->area < 0) return;
    for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
        if (q == p || !sc_active(q)) continue;
        for (int i = 0;; i++) {
            int n = 0;
            const Unit *all = Units_GetActive(&n);
            if (!all || i >= n) break;
            const Unit *u = &all[i];
            if (u->player_id != q || u->alive != UNIT_ALIVE_ACTIVE) continue;
            if (a->def >= 0 && u->def_idx != a->def) continue;
            if (!sc_in_area(u, a->area)) continue;
            int def = u->def_idx;
            int h = Units_Capture(i, p);
            if (h >= 0) sc_apply_mod(h, def);
        }
    }
}

/* p's side's monarch on p's start (legacy:172088-172119). */
static void sc_monarch(int p) {
    GameWorld *w = World_Get();
    if (!w) return;
    const TakSideInfo *side = Sides_Get(w->cfg.players[p - 1].side);
    int def = side && side->commander[0] ? Units_FindDefByName(side->commander) : -1;
    for (int i = 0; def >= 0 && i < w->num_start_positions; i++) {
        if (w->start_positions[i].player != p) continue;
        int h = Units_Spawn(def, p, Units_PlayerColorIndex(p),
                            w->start_positions[i].x * SC_CELL,
                            w->start_positions[i].z * SC_CELL);
        if (h >= 0 && g_sc_spawn) g_sc_spawn(h);
        if (h >= 0) sc_apply_mod(h, def);
        return;
    }
}

/* Every other player still in, allied to p (allies) or not: a verdict
 * each (legacy:171848-172030). */
static void sc_verdict_others(int p, int allies, int win) {
    for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
        if (q == p || !sc_active(q)) continue;
        if ((Units_PlayersAreEnemies(p, q) != 0) == allies) continue;
        if (win) sc_win(q);
        else sc_lose(q);
    }
}

static void sc_act(int p, const ScAct *a, int rule) {
    ScSeat *s = &sc.seat[p];
    GameWorld *w = World_Get();
    PlayerEconomy *e = w ? &w->economy.players[p - 1] : NULL;
    switch (a->type) {
    case SC_A_TIMER_DOWN:
    case SC_A_TIMER_UP: {
        ScTimer *t = sc_timer(s, a->a);
        if (!t && s->n_timers < SC_TIMERS) {
            t = &s->timer[s->n_timers++];
            t->id = a->a;
        }
        if (t) {
            t->up = (uint8_t)(a->type == SC_A_TIMER_UP);
            t->value = a->b;
        }
        break;
    }
    case SC_A_COUNTER_SET:
        s->counter_on[a->ch] = 1;
        s->counter[a->ch] = a->a;
        break;
    case SC_A_COUNTER_ADD:
        if (s->counter_on[a->ch]) s->counter[a->ch] += a->a;
        break;
    case SC_A_COUNTER_SUB:
        if (s->counter_on[a->ch]) s->counter[a->ch] -= a->a;
        break;
    case SC_A_WIN:        sc_win(p); break;
    case SC_A_LOSE:       sc_lose(p); break;
    case SC_A_CREATE:     sc_make_in(p, a->def, a->area); break;
    case SC_A_KILL:       sc_each_in(p, a, 0); break;
    case SC_A_CAPTURE:    sc_capture(p, a); break;
    case SC_A_HEAL:       sc_each_in(p, a, 1); break;
    case SC_A_HARM:       sc_each_in(p, a, 2); break;
    case SC_A_CLOCK:      sc.clock = 1; break;
    case SC_A_MESSAGE:    sc_say(a->to, p, a->text); break;
    case SC_A_ONCE:       s->on[rule] = 0; break;
    case SC_A_MOVE:       sc_each_in(p, a, 3); break;
    case SC_A_HOLD_INCOME: s->hold = a->a; break;
    case SC_A_RESUME_INCOME: s->hold = 0; break;
    /* Straight into the pool, the end of the tick holds it to the cap
     * (legacy:171806-171847, legacy:8774). */
    case SC_A_MANA_SET:   if (e) e->mana = (float)a->a; break;
    case SC_A_MANA_ADD:
        if (e) {
            e->mana += (float)a->a;
            e->earned_total += (double)a->a;
        }
        break;
    case SC_A_MANA_SUB:   if (e) e->mana -= (float)a->a; break;
    case SC_A_ALLIES_WIN:   sc_verdict_others(p, 1, 1); sc_win(p); break;
    case SC_A_ALLIES_LOSE:  sc_verdict_others(p, 1, 0); sc_lose(p); break;
    case SC_A_ENEMIES_WIN:  sc_verdict_others(p, 0, 1); break;
    case SC_A_ENEMIES_LOSE: sc_verdict_others(p, 0, 0); break;
    case SC_A_COUNTER_MESSAGE:
        if (s->counter_on[a->ch]) {
            char line[SC_TEXT + SC_PARAM + 16];
            snprintf(line, sizeof line, "%s %d%s", a->text, (int)s->counter[a->ch], a->tail);
            sc_say(a->to, p, line);
        }
        break;
    case SC_A_MONARCH:    sc_monarch(p); break;
    default: break;
    }
}

/* A rule runs its actions when every condition holds, the first that
 * fails ending the look (legacy:171151-171179). */
static void sc_rule(int p, int r) {
    const ScRule *rule = &sc.rules[r];
    if (!sc.seat[p].on[r]) return;
    for (int k = 0; k < rule->n_cond; k++)
        if (!sc_holds(p, &sc.conds[rule->first_cond + k])) return;
    for (int k = 0; k < rule->n_act; k++)
        sc_act(p, &sc.acts[rule->first_act + k], r);
}

/* A seat's rules are every player's, then its own start's. */
static int sc_rule_is_for(int p, int r) {
    int s = sc.rules[r].section;
    return s == 0 || s == (int)sc.seat[p].start + 1;
}

/* Once a game second, or at game start: the timers step, then each
 * seat still in reads all of its rules, a loss on the way included
 * (legacy:173086-173338). */
static void sc_look(void) {
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        ScSeat *s = &sc.seat[p];
        for (int i = 0; s->in && i < s->n_timers; i++)
            s->timer[i].value += s->timer[i].up ? 1 : -1;
    }
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (!sc.seat[p].in || sc.seat[p].out) continue;
        for (int r = 0; r < sc.n_rules; r++)
            if (sc_rule_is_for(p, r)) sc_rule(p, r);
    }
}

/* Income held at nothing while the script holds it (legacy:235961). */
static void sc_hold_income(void) {
    GameWorld *w = World_Get();
    for (int p = 1; w && p <= TAK_MAX_PLAYERS; p++)
        if (sc.seat[p].in && sc.seat[p].hold > 0)
            w->economy.players[p - 1].regen_per_sec = 0.0f;
}

/* ── lifetime ─────────────────────────────────────────────────────── */

/* The map's script runs only when its header says it has one
 * (legacy:169012, legacy:177664). */
int MapScenario_MapHasScript(const char *map_key) {
    char path[256];
    if (!map_key || !map_key[0]) return 0;
    if (TAK_Maps_FindFile(map_key, "ota", path, sizeof path) != 0) return 0;
    TDFFile *tdf = TDF_Open(path);
    int has = 0;
    if (tdf && TDF_Load(tdf) == 0 && TDF_PushSection(tdf, "GlobalHeader") == 0)
        has = TDF_ReadInt(tdf, "hasscenario", 0) != 0;
    if (tdf) TDF_Close(tdf);
    return has;
}

int MapScenario_Begin(const char *map_key) {
    MapScenario_End();
    if (!MapScenario_MapHasScript(map_key)) return 0;
    char path[256];
    if (TAK_Maps_FindFile(map_key, "crt", path, sizeof path) != 0) return 0;
    void *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &data, &size) != 0 || !data || size == 0) {
        if (data) VFS_FreeBuffer(data);
        return 0;
    }
    int rc = sc_load((const uint8_t *)data, size);
    VFS_FreeBuffer(data);
    if (rc != 0) {
        fprintf(stderr, "Scenario: %s does not read\n", path);
        return -1;
    }
    GameWorld *w = World_Get();
    for (int p = 1; w && p <= TAK_MAX_PLAYERS; p++) {
        sc.seat[p].in = w->cfg.players[p - 1].kind != TAK_SLOT_CLOSED;
        memset(sc.seat[p].on, 1, (size_t)sc.n_rules);
    }
    sc.active = 1;
    fprintf(stderr, "Scenario: %s, %d rules, %d start units, %d areas\n", path,
            sc.n_rules, sc.n_places, sc.n_areas - 1);
    return 1;
}

void MapScenario_End(void) { sc_free(); }

int MapScenario_Active(void) { return sc.active; }
int MapScenario_ClockShown(void) { return sc.active && sc.clock; }

void MapScenario_SetSpawnHook(MapScenario_SpawnFn fn) { g_sc_spawn = fn; }
void MapScenario_SetMessageHook(MapScenario_MessageFn fn) { g_sc_message = fn; }

void MapScenario_Start(void) {
    GameWorld *w = World_Get();
    if (!sc.active || !w) return;
    int dealt[TAK_MAX_PLAYERS];
    BattleConfig_AssignStarts(&w->cfg, w->num_start_positions, dealt);
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        ScSeat *s = &sc.seat[p];
        if (!s->in) continue;
        s->start = (uint8_t)(dealt[p - 1] >= 0 ? dealt[p - 1] : 0);
        s->counter_on[SC_SIDE_COUNTER] = 1;
        s->counter[SC_SIDE_COUNTER] = w->cfg.players[p - 1].side;
    }
    sc.mods_next = Units_NextStableId();
    /* The start's units take its health, scales and rank after the
     * map's mod for their kind (legacy:173226-173262). */
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (!sc.seat[p].in) continue;
        for (int i = 0; i < sc.n_places; i++) {
            const ScPlace *pl = &sc.places[i];
            if (pl->start != sc.seat[p].start || pl->def < 0) continue;
            int h = sc_make(p, pl->def, pl->x, pl->z);
            if (h < 0) continue;
            /* The start's scales multiply the mod's (legacy:173251-173252). */
            const ScMod *m = sc_mod_of(pl->def);
            int64_t armor = m ? sc_pct(m->armor) : 100;
            int64_t attack = m ? sc_pct(m->attack) : 100;
            Units_ScaleHealth(h, sc_pct(pl->health));
            Units_SetArmorPercent(h, sc_pct((int32_t)(armor * pl->armor / 100)));
            Units_SetAttackPercent(h, sc_pct((int32_t)(attack * pl->attack / 100)));
            Units_SetVeteranLevel(h, pl->vet);
        }
    }
    sc.starting = 1;
    sc_look();
    sc.starting = 0;
    sc.mods_next = Units_NextStableId();
}

void MapScenario_Tick(void) {
    if (!sc.active) return;
    sc_mods_catch_up();
    sc.tick++;
    uint32_t second = sc.tick / SC_TICKS_PER_SEC;
    if (second > sc.second) {
        sc.second = second;
        sc_look();
    }
    sc.mods_next = Units_NextStableId();
    sc_hold_income();
}

int MapScenario_DebugTimer(int player, int id, int32_t *out) {
    if (!sc.active || !sc_seat_ok(player)) return -1;
    ScTimer *t = sc_timer(&sc.seat[player], id);
    if (!t) return -1;
    if (out) *out = t->value;
    return 0;
}

int MapScenario_DebugCounter(int player, int id, int32_t *out) {
    if (!sc.active || !sc_seat_ok(player) || id < 0 || id > 255) return -1;
    if (!sc.seat[player].counter_on[id]) return -1;
    if (out) *out = sc.seat[player].counter[id];
    return 0;
}

/* ── the save and the hash ────────────────────────────────────────── */

#define SC_SAVE_MAGIC 0x314e4353u   /* "SCN1" */

typedef struct ScOut { uint8_t *p; unsigned len; } ScOut;

static void out_u8(ScOut *o, uint8_t v)   { if (o->p) o->p[o->len] = v; o->len += 1; }
static void out_u16(ScOut *o, uint16_t v) { if (o->p) tak_put_u16(o->p + o->len, v); o->len += 2; }
static void out_u32(ScOut *o, uint32_t v) { if (o->p) tak_put_u32(o->p + o->len, v); o->len += 4; }
static void out_i32(ScOut *o, int32_t v)  { out_u32(o, (uint32_t)v); }

static void sc_write(ScOut *o) {
    out_u32(o, SC_SAVE_MAGIC);
    /* What the map gave, so a save on another script is refused. */
    out_u32(o, (uint32_t)sc.n_rules);
    out_u32(o, (uint32_t)sc.n_places);
    out_u32(o, sc.tick);
    out_u32(o, sc.second);
    out_u8(o, sc.clock);
    out_u32(o, sc.mods_next);
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        const ScSeat *s = &sc.seat[p];
        out_u8(o, s->in);
        out_u8(o, s->start);
        out_u8(o, s->out);
        out_i32(o, s->hold);
        for (int r = 0; r < sc.n_rules; r++) out_u8(o, s->on[r]);
        out_u8(o, (uint8_t)s->n_timers);
        for (int i = 0; i < s->n_timers; i++) {
            out_i32(o, s->timer[i].id);
            out_u8(o, s->timer[i].up);
            out_i32(o, s->timer[i].value);
        }
        uint16_t counters = 0;
        for (int c = 0; c < 256; c++) counters += s->counter_on[c];
        out_u16(o, counters);
        for (int c = 0; c < 256; c++) {
            if (!s->counter_on[c]) continue;
            out_u8(o, (uint8_t)c);
            out_i32(o, s->counter[c]);
        }
        out_u8(o, (uint8_t)sc.n_kills[p]);
        for (int i = 0; i < sc.n_kills[p]; i++) {
            out_u16(o, (uint16_t)sc.kills[p][i].def);
            out_i32(o, sc.kills[p][i].count);
        }
        out_u8(o, (uint8_t)sc.n_losses[p]);
        for (int i = 0; i < sc.n_losses[p]; i++) {
            out_u16(o, (uint16_t)sc.losses[p][i].def);
            out_i32(o, sc.losses[p][i].count);
        }
    }
}

unsigned MapScenario_SaveSize(void) {
    ScOut o = { NULL, 0 };
    if (!sc.active) return 0;
    sc_write(&o);
    return o.len;
}

void MapScenario_SaveState(unsigned char *out) {
    ScOut o = { out, 0 };
    if (!sc.active || !out) return;
    sc_write(&o);
}

typedef struct ScRd { const uint8_t *p; unsigned len, at; int bad; } ScRd;

static const uint8_t *rd_take(ScRd *r, unsigned n) {
    if (r->bad || r->len - r->at < n) { r->bad = 1; return NULL; }
    const uint8_t *p = r->p + r->at;
    r->at += n;
    return p;
}
static uint8_t  rd_u8(ScRd *r)  { const uint8_t *p = rd_take(r, 1); return p ? *p : 0; }
static uint16_t rd_u16(ScRd *r) { const uint8_t *p = rd_take(r, 2); return p ? tak_get_u16(p) : 0; }
static uint32_t rd_u32(ScRd *r) { const uint8_t *p = rd_take(r, 4); return p ? tak_get_u32(p) : 0; }

int MapScenario_LoadState(const unsigned char *in, unsigned len) {
    if (!sc.active || !in) return -1;
    ScRd r = { in, len, 0, 0 };
    if (rd_u32(&r) != SC_SAVE_MAGIC) return -1;
    if (rd_u32(&r) != (uint32_t)sc.n_rules || rd_u32(&r) != (uint32_t)sc.n_places)
        return -1;
    /* Read whole into a copy first, so a short or foreign blob leaves
     * the scenario as the loading screen started it. */
    static ScSeat seat[TAK_MAX_PLAYERS + 1];
    static ScTally kills[TAK_MAX_PLAYERS + 1][SC_TALLIES];
    static ScTally losses[TAK_MAX_PLAYERS + 1][SC_TALLIES];
    const uint8_t *flags[TAK_MAX_PLAYERS + 1] = { 0 };
    int n_kills[TAK_MAX_PLAYERS + 1] = { 0 }, n_losses[TAK_MAX_PLAYERS + 1] = { 0 };
    uint32_t tick = rd_u32(&r), second = rd_u32(&r);
    uint8_t clock = rd_u8(&r);
    uint32_t mods_next = rd_u32(&r);
    for (int p = 1; p <= TAK_MAX_PLAYERS && !r.bad; p++) {
        ScSeat *s = &seat[p];
        memset(s, 0, sizeof(*s));
        s->in = rd_u8(&r);
        s->start = rd_u8(&r);
        s->out = rd_u8(&r);
        s->hold = (int32_t)rd_u32(&r);
        flags[p] = rd_take(&r, (unsigned)sc.n_rules);
        s->n_timers = rd_u8(&r);
        if (s->n_timers > SC_TIMERS) r.bad = 1;
        for (int i = 0; i < s->n_timers && !r.bad; i++) {
            s->timer[i].id = (int32_t)rd_u32(&r);
            s->timer[i].up = rd_u8(&r);
            s->timer[i].value = (int32_t)rd_u32(&r);
        }
        unsigned counters = rd_u16(&r);
        if (counters > 256) r.bad = 1;
        for (unsigned i = 0; i < counters && !r.bad; i++) {
            uint8_t c = rd_u8(&r);
            s->counter_on[c] = 1;
            s->counter[c] = (int32_t)rd_u32(&r);
        }
        n_kills[p] = rd_u8(&r);
        if (n_kills[p] > SC_TALLIES) r.bad = 1;
        for (int i = 0; i < n_kills[p] && !r.bad; i++) {
            kills[p][i].def = (int16_t)rd_u16(&r);
            kills[p][i].count = (int32_t)rd_u32(&r);
        }
        n_losses[p] = rd_u8(&r);
        if (n_losses[p] > SC_TALLIES) r.bad = 1;
        for (int i = 0; i < n_losses[p] && !r.bad; i++) {
            losses[p][i].def = (int16_t)rd_u16(&r);
            losses[p][i].count = (int32_t)rd_u32(&r);
        }
    }
    if (r.bad || r.at != len) return -1;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        uint8_t *on = sc.seat[p].on;
        sc.seat[p] = seat[p];
        sc.seat[p].on = on;
        if (on && flags[p]) memcpy(on, flags[p], (size_t)sc.n_rules);
        memcpy(sc.kills[p], kills[p], sizeof(kills[p]));
        memcpy(sc.losses[p], losses[p], sizeof(losses[p]));
        sc.n_kills[p] = n_kills[p];
        sc.n_losses[p] = n_losses[p];
    }
    sc.tick = tick;
    sc.second = second;
    sc.clock = clock;
    sc.mods_next = mods_next;
    return 0;
}

uint32_t TAK_SimHash_Scenario(uint32_t h) {
    if (!sc.active) return h;
    unsigned n = MapScenario_SaveSize();
    uint8_t stack[4096];
    uint8_t *buf = n <= sizeof stack ? stack : (uint8_t *)tak_malloc(n);
    if (!buf) return TAK_HashU32(h, n);
    MapScenario_SaveState(buf);
    h = TAK_HashBytes(h, buf, n);
    if (buf != stack) tak_free(buf);
    return h;
}
