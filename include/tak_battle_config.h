#ifndef TAK_BATTLE_CONFIG_H
#define TAK_BATTLE_CONFIG_H

#include <stdint.h>

/*
 * BattleConfig — the handoff struct between the battle-setup screen
 * (UI lane) and the game-loading / in-game code (engine lane).
 *
 * The skirmish lobby writes into this; game start reads from it.
 * Keeping this minimal and plain-data so both sides stay loosely coupled.
 */

/* Per-side unit cap (engine enforces per side rather than globally, so
 * 1v7 skirmishes aren't starved).
 *
 * Defaults are intentionally simple constants — modders can override
 * either at compile time (-DTAK_UNITS_PER_SIDE_MAX=4000) or at runtime
 * by editing the ini/tdf that eventually drives the slider bounds.
 * The slider itself reads these exact constants so modding one place
 * moves the UI in lockstep. */
#ifndef TAK_UNITS_PER_SIDE_MIN
#define TAK_UNITS_PER_SIDE_MIN     200
#endif
#ifndef TAK_UNITS_PER_SIDE_MAX
#define TAK_UNITS_PER_SIDE_MAX     2000
#endif
#ifndef TAK_UNITS_PER_SIDE_DEFAULT
#define TAK_UNITS_PER_SIDE_DEFAULT 2000
#endif
#ifndef TAK_UNITS_PER_SIDE_STEP
#define TAK_UNITS_PER_SIDE_STEP    100    /* slider click-step */
#endif

#define TAK_MAX_PLAYERS            8

typedef enum {
    TAK_SIDE_ARAMON = 0,
    TAK_SIDE_TAROS,
    TAK_SIDE_VERUNA,
    TAK_SIDE_ZHON,
    TAK_SIDE_COUNT
} TakSide;

typedef enum {
    TAK_SLOT_CLOSED = 0,   /* empty seat (not rendered in-game) */
    TAK_SLOT_HUMAN,
    TAK_SLOT_AI,
} TakSlotKind;

typedef struct PlayerSlot {
    TakSlotKind kind;
    int         side;      /* TakSide */
    int         team;      /* 1..4; 0 = FFA */
    int         color;     /* palette index, 0..11 */
    int         ai_difficulty; /* 0=easy, 1=normal, 2=hard, 3=brutal */
    char        name[32];
} PlayerSlot;

typedef struct BattleConfig {
    /* Map selection */
    char map_name[64];         /* base name, e.g. "Vain Blessings" */

    /* Players (fixed-size array; `kind == TAK_SLOT_CLOSED` = unused) */
    PlayerSlot players[TAK_MAX_PLAYERS];

    /* Per-side unit cap (applies to every non-closed slot). */
    int  units_per_side;

    /* Visibility / rules options — each maps 1:1 to a checkbox in the
     * skirmish lobby .gui. 0 = off, 1 = on. */
    int  line_of_sight;
    int  map_revealed;
    int  monarch_expendable;
    int  random_start_locations;
    int  power_codes;
    int  slow_game;          /* "degrade performance" — usually off */
} BattleConfig;

/* Fill `cfg` with sensible skirmish defaults: 1 human (Aramon) + 1 AI
 * (Taros), line-of-sight on, 1500 units/side, no map selected yet. */
void BattleConfig_SetDefaults(BattleConfig *cfg);

#endif /* TAK_BATTLE_CONFIG_H */
