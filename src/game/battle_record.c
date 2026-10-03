/*
 * battle_record.c -- what an end screen tells beyond the original's
 * tallies. The simulation calls in as things happen and once a tick,
 * and never reads any of it back, so nothing here can change a battle.
 */

#include "tak_battle_record.h"

#include "tak_unit.h"
#include "tak_world.h"

#include <limits.h>
#include <string.h>

/* Beside the world, not in it: 166 KB a GameWorld on the stack of a
 * test should not have to carry. */
static int32_t s_series[TAK_MAX_PLAYERS + 1][BATTLE_SERIES_COUNT][BATTLE_MAX_SAMPLES];

int32_t *BattleRecord_SeriesRow(int player, int series) {
    if (player < 0 || player > TAK_MAX_PLAYERS) return NULL;
    if (series < 0 || series >= BATTLE_SERIES_COUNT) return NULL;
    return s_series[player][series];
}

static int seat(int player) { return player >= 1 && player <= TAK_MAX_PLAYERS; }

static int32_t battle_tick(const GameWorld *w) {
    return w ? w->skirmish_elapsed_ticks + w->mission_elapsed_ticks : 0;
}

static int32_t add_capped(int32_t a, int32_t b) {
    if (b <= 0) return a;
    return a > INT32_MAX - b ? INT32_MAX : a + b;
}

static int32_t whole(double v) {
    if (!(v > 0.0)) return 0;
    return v >= (double)INT32_MAX ? INT32_MAX : (int32_t)v;
}

static BattleRecord *live_record(void) {
    GameWorld *w = World_Get();
    return w ? &w->record : NULL;
}

int BattleRecord_IsLodestone(int def) {
    const UnitDef *d = Units_GetDef(def);
    return d && d->bmcode == 0 && !d->commander &&
           !(d->cap_flags & UNIT_CAP_BUILDER) &&
           d->mogrium_income_per_sec > 0.0f;
}

static void add_event(BattleRecord *r, int32_t tick, int kind, int player,
                      int other, int def, int other_def) {
    if (r->event_count >= BATTLE_MAX_EVENTS) return;
    BattleEvent *e = &r->events[r->event_count++];
    memset(e, 0, sizeof(*e));
    e->tick = tick;
    e->kind = (uint8_t)kind;
    e->player = (uint8_t)player;
    e->other = (uint8_t)other;
    e->def = (int16_t)def;
    e->other_def = (int16_t)other_def;
}

static int has_event(const BattleRecord *r, int kind) {
    for (int i = 0; i < r->event_count; i++)
        if (r->events[i].kind == kind) return 1;
    return 0;
}

void BattleRecord_Hit(int shooter_player, int victim_player, int32_t hp) {
    BattleRecord *r = live_record();
    if (!r || hp <= 0 || !seat(shooter_player) || !seat(victim_player)) return;
    if (shooter_player == victim_player) return;
    PlayerBattleRecord *a = &r->players[shooter_player];
    PlayerBattleRecord *b = &r->players[victim_player];
    a->damage_dealt = add_capped(a->damage_dealt, hp);
    b->damage_taken = add_capped(b->damage_taken, hp);
}

void BattleRecord_Kill(int killer_player, const Unit *shooter, const Unit *victim) {
    GameWorld *w = World_Get();
    if (!w || !victim || !seat(killer_player)) return;
    BattleRecord *r = &w->record;
    int32_t t = battle_tick(w);
    int shooter_def = shooter ? (int)shooter->def_idx : -1;
    if (!has_event(r, BATTLE_EVENT_FIRST_BLOOD))
        add_event(r, t, BATTLE_EVENT_FIRST_BLOOD, killer_player,
                  victim->player_id, shooter_def, victim->def_idx);
    const UnitDef *vd = Units_GetDef(victim->def_idx);
    if (vd && vd->commander && seat(victim->player_id))
        add_event(r, t, BATTLE_EVENT_MONARCH_SLAIN, victim->player_id,
                  killer_player, victim->def_idx, shooter_def);
    if (!shooter) return;
    /* The champion: most kills, then most experience. */
    PlayerBattleRecord *p = &r->players[killer_player];
    int kills = shooter->kills > INT16_MAX ? INT16_MAX : (int)shooter->kills;
    if (p->best_id == shooter->stable_id || kills > p->best_kills ||
        (kills == p->best_kills && shooter->experience_pts > p->best_xp)) {
        p->best_id = shooter->stable_id;
        p->best_def = (int16_t)shooter->def_idx;
        p->best_kills = (int16_t)kills;
        p->best_xp = shooter->experience_pts;
    }
}

void BattleRecord_Finished(const Unit *u) {
    BattleRecord *r = live_record();
    if (!r || !u || !seat(u->player_id)) return;
    const UnitDef *d = Units_GetDef(u->def_idx);
    /* Walls are not counted, as the original never counts them built. */
    if (!d || d->is_feature) return;
    PlayerBattleRecord *p = &r->players[u->player_id];
    if (d->bmcode == 0) p->buildings_raised++;
    else p->units_trained++;
    for (int i = 0; i < p->kind_count; i++) {
        if (p->kinds[i].def != (int16_t)u->def_idx) continue;
        if (p->kinds[i].count < INT16_MAX) p->kinds[i].count++;
        return;
    }
    if (p->kind_count >= BATTLE_MAX_KINDS) return;
    p->kinds[p->kind_count].def = (int16_t)u->def_idx;
    p->kinds[p->kind_count].count = 1;
    p->kind_count++;
}

void BattleRecord_Cast(int player) {
    BattleRecord *r = live_record();
    if (!r || !seat(player)) return;
    r->players[player].spells_cast = add_capped(r->players[player].spells_cast, 1);
}

/* Every kingdom's sample in one walk over the units. */
static void sample_all(const GameWorld *w, int32_t out[TAK_MAX_PLAYERS + 1][BATTLE_SERIES_COUNT]) {
    memset(out, 0, sizeof(int32_t) * (TAK_MAX_PLAYERS + 1) * BATTLE_SERIES_COUNT);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; units && i < n; i++) {
        const Unit *u = &units[i];
        if (!seat(u->player_id) || u->under_construction) continue;
        if (u->alive != UNIT_ALIVE_ACTIVE && u->alive != UNIT_ALIVE_TRANSPORTED) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d) continue;
        int32_t *o = out[u->player_id];
        if (d->bmcode != 0) {
            o[BATTLE_SERIES_ARMY]++;
            o[BATTLE_SERIES_WORTH] = add_capped(o[BATTLE_SERIES_WORTH], d->build_cost);
        } else if (BattleRecord_IsLodestone(u->def_idx)) {
            o[BATTLE_SERIES_LODESTONES]++;
        }
    }
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        const PlayerEconomy *e = &w->economy.players[p - 1];
        int32_t *o = out[p];
        o[BATTLE_SERIES_MANA] = whole((double)e->mana);
        o[BATTLE_SERIES_GATHERED] = whole(e->earned_total);
        o[BATTLE_SERIES_SPENT] = whole(e->spent_total);
        o[BATTLE_SERIES_BUILT] = w->stats[p].units_built;
        o[BATTLE_SERIES_KILLS] = w->stats[p].kills;
        o[BATTLE_SERIES_LOSSES] = w->stats[p].losses;
    }
}

void BattleRecord_SampleNow(const GameWorld *w, int player,
                            int32_t out[BATTLE_SERIES_COUNT]) {
    memset(out, 0, sizeof(int32_t) * BATTLE_SERIES_COUNT);
    if (!w || !seat(player)) return;
    int32_t all[TAK_MAX_PLAYERS + 1][BATTLE_SERIES_COUNT];
    sample_all(w, all);
    memcpy(out, all[player], sizeof(int32_t) * BATTLE_SERIES_COUNT);
}

static void take_sample(const GameWorld *w, BattleRecord *r) {
    if (r->samples >= BATTLE_MAX_SAMPLES && r->every > INT32_MAX / 2) return;
    if (r->samples >= BATTLE_MAX_SAMPLES) {
        for (int p = 0; p <= TAK_MAX_PLAYERS; p++)
            for (int s = 0; s < BATTLE_SERIES_COUNT; s++)
                for (int k = 0; k < BATTLE_MAX_SAMPLES / 2; k++)
                    s_series[p][s][k] = s_series[p][s][2 * k];
        r->samples = BATTLE_MAX_SAMPLES / 2;
        r->every *= 2;
    }
    int32_t all[TAK_MAX_PLAYERS + 1][BATTLE_SERIES_COUNT];
    sample_all(w, all);
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++)
        for (int s = 0; s < BATTLE_SERIES_COUNT; s++)
            s_series[p][s][r->samples] = all[p][s];
    r->samples++;
}

/* A kingdom that stood and has nothing left falls, or yields when it
 * gave up. A dying unit still counts, as it does for the verdict. */
static void note_fallen(const GameWorld *w, BattleRecord *r, int32_t t) {
    uint32_t have = 0;
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; units && i < n; i++)
        if (units[i].alive != UNIT_ALIVE_DEAD && seat(units[i].player_id))
            have |= 1u << units[i].player_id;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (w->cfg.players[p - 1].kind == TAK_SLOT_CLOSED) continue;
        uint32_t bit = 1u << p;
        int stands = (have & bit) && !w->stats[p].eliminated && !w->resigned[p];
        if (stands) {
            r->standing |= bit;
        } else if (r->standing & bit) {
            r->standing &= ~bit;
            r->players[p].fell_tick = t;
            add_event(r, t, w->resigned[p] ? BATTLE_EVENT_YIELDED : BATTLE_EVENT_FELL,
                      p, 0, -1, -1);
        }
    }
}

void BattleRecord_Tick(GameWorld *w) {
    if (!w || !w->loaded) return;
    BattleRecord *r = &w->record;
    int32_t t = battle_tick(w);
    if (r->every <= 0) r->every = BATTLE_SAMPLE_TICKS;
    note_fallen(w, r, t);
    /* t >= samples * every, which a saved gap cannot overflow. */
    if (t / r->every >= r->samples) take_sample(w, r);
}
