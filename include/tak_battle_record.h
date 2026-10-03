#ifndef TAK_BATTLE_RECORD_H
#define TAK_BATTLE_RECORD_H

/* What an end screen can tell beyond the original's tallies
 * (PlayerBattleStats): units and buildings finished by kind, damage,
 * spells, each kingdom's best unit, the key moments of the battle and
 * samples of every kingdom over time. Nothing in the simulation reads
 * it, so it stays out of the state hash. A save carries it in its own
 * section, which an older engine skips. */

#include "tak_battle_config.h"
#include <stdint.h>

struct GameWorld;
struct Unit;

/* A sample every 5 s of game time. When a long battle fills the
 * samples, every other one is kept and the gap doubles. */
#define BATTLE_SAMPLE_TICKS 300
#define BATTLE_MAX_SAMPLES  512
/* The kinds a kingdom finished, counted by def, and the key moments. */
#define BATTLE_MAX_KINDS     48
#define BATTLE_MAX_EVENTS    64

/* What each sample holds for a kingdom. */
enum {
    BATTLE_SERIES_ARMY,        /* finished mobile units on the field */
    BATTLE_SERIES_WORTH,       /* what they cost, in mana */
    BATTLE_SERIES_MANA,        /* the pool */
    BATTLE_SERIES_GATHERED,    /* mana gathered so far */
    BATTLE_SERIES_SPENT,       /* mana spent so far */
    BATTLE_SERIES_BUILT,       /* the original's units built so far */
    BATTLE_SERIES_KILLS,
    BATTLE_SERIES_LOSSES,
    BATTLE_SERIES_LODESTONES,  /* finished lodestones held */
    BATTLE_SERIES_COUNT
};

/* The key moments. `player` is whose moment it is and `other` the
 * kingdom on the other side of it, 0 for none. */
enum {
    BATTLE_EVENT_FIRST_BLOOD = 1,  /* player killed a unit of other's first */
    BATTLE_EVENT_MONARCH_SLAIN,    /* player's monarch, killed by other */
    BATTLE_EVENT_FELL,             /* player has nothing left */
    BATTLE_EVENT_YIELDED           /* player gave up */
};

typedef struct BattleEvent {
    int32_t tick;
    uint8_t kind;
    uint8_t player;
    uint8_t other;
    int16_t def;                   /* player's unit in it, -1 for none */
    int16_t other_def;             /* other's unit in it, -1 for none */
} BattleEvent;

typedef struct BattleKind {
    int16_t def;
    int16_t count;
} BattleKind;

typedef struct PlayerBattleRecord {
    int32_t units_trained;         /* mobile units finished */
    int32_t buildings_raised;      /* structures finished */
    int32_t damage_dealt;          /* hit points taken off other kingdoms */
    int32_t damage_taken;          /* hit points other kingdoms took off */
    int32_t spells_cast;           /* shots that cost mana */
    int32_t fell_tick;             /* when it had nothing left, 0 standing */
    /* The unit with the most kills, ties to the most experience. */
    uint32_t best_id;              /* its stable id, 0 for none */
    int16_t best_def;
    int16_t best_kills;
    int32_t best_xp;
    int32_t kind_count;
    BattleKind kinds[BATTLE_MAX_KINDS];
} PlayerBattleRecord;

typedef struct BattleRecord {
    int32_t every;                 /* ticks between samples, 0 before any */
    int32_t samples;               /* sample k was taken at k * every */
    uint32_t standing;             /* a bit per seat with units at the last look */
    int32_t event_count;
    BattleEvent events[BATTLE_MAX_EVENTS];
    PlayerBattleRecord players[TAK_MAX_PLAYERS + 1];
} BattleRecord;

/* Once a simulation tick, after everything else in it. */
void BattleRecord_Tick(struct GameWorld *w);
/* Hit points a unit of `shooter_player` took off one of
 * `victim_player`'s, what the victim had left at most. */
void BattleRecord_Hit(int shooter_player, int victim_player, int32_t hp);
/* A kill the player was credited with. `shooter` is the unit that made
 * it when it still stands, after its own tally went up. */
void BattleRecord_Kill(int killer_player, const struct Unit *shooter,
                       const struct Unit *victim);
/* A unit or building whose construction just finished. */
void BattleRecord_Finished(const struct Unit *u);
/* A shot that cost its player mana left. */
void BattleRecord_Cast(int player);

/* One series of a kingdom's samples, record.samples long, NULL for a
 * player or series out of range. The samples live beside the world
 * rather than in it, so a GameWorld stays small enough for the stack.
 * A battle with no samples yet ignores whatever they held before. */
int32_t *BattleRecord_SeriesRow(int player, int series);

/* What a sample of the kingdom would hold now, BATTLE_SERIES_COUNT
 * values, for a graph's last point at the current tick. */
void BattleRecord_SampleNow(const struct GameWorld *w, int player,
                            int32_t out[BATTLE_SERIES_COUNT]);
/* 1 for a finished lodestone's def: a structure that adds income and
 * builds nothing. */
int  BattleRecord_IsLodestone(int def);

#endif /* TAK_BATTLE_RECORD_H */
