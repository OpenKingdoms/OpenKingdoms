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

/* Player colours are authored art, not a tint: each side's team-logo GAF
 * entry ships one frame per colour and the engine picks the frame with
 * the owner's colour byte. Ten of them, and cycling wraps at 9
 * (legacy:135368, legacy:135392). The same count backs the team-coloured
 * unit atlases, so this is the one index every lane shares. */
#define TAK_PLAYER_COLOR_COUNT     10

/* Name + the authored frame's dominant colour, used for the swatch when
 * the GAF is unavailable. Order is the authored frame order. */
typedef struct TakPlayerColor {
    const char *name;
    uint8_t     r, g, b;
} TakPlayerColor;

/* Colour `index`, wrapped into 0..TAK_PLAYER_COLOR_COUNT-1. Never NULL. */
const TakPlayerColor *BattleConfig_PlayerColor(int index);

/* A side is its SIDEn index in gamedata/sidedata.tdf (see tak_sides.h).
 * The four kingdoms come first, TAK_SIDE_COUNT of them, and Iron Plague's
 * data adds Creon as SIDE7, past three sides no player takes. */
typedef enum {
    TAK_SIDE_ARAMON = 0,
    TAK_SIDE_TAROS,
    TAK_SIDE_VERUNA,
    TAK_SIDE_ZHON,
    TAK_SIDE_COUNT,
    TAK_SIDE_CREON = 7
} TakSide;

typedef enum {
    TAK_SLOT_CLOSED = 0,   /* empty seat (not rendered in-game) */
    TAK_SLOT_HUMAN,
    TAK_SLOT_AI,
} TakSlotKind;

typedef struct PlayerSlot {
    TakSlotKind kind;
    int         side;      /* SIDEn index, TakSide */
    int         team;      /* 1..4; 0 = FFA */
    int         color;     /* authored colour index, 0..TAK_PLAYER_COLOR_COUNT-1 */
    int         ai_difficulty; /* 0=easy, 1=normal, 2=hard, 3=brutal */
    char        name[32];
} PlayerSlot;

typedef struct BattleConfig {
    /* Map selection */
    char map_name[96];         /* base name, e.g. "Vain Blessings" */

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
    int  crusades_balance;   /* Iron Plague unit balance toggle */

    /* The session seed. Every draw the simulation makes, script RAND
     * and the AI included, comes from it and from nothing local, so a
     * match that hands every machine the same seed plays the same
     * battle. The original seeded each machine from its own clock
     * (legacy:243075-243084) and never exchanged it. */
    uint32_t seed;
} BattleConfig;

/* Fill `cfg` with sensible skirmish defaults: 1 human (Aramon) + 1 AI
 * (Taros), line-of-sight on, 1500 units/side, no map selected yet. */
void BattleConfig_SetDefaults(BattleConfig *cfg);

/* Next colour a slot may take: the first one no other occupied slot is
 * using, scanning upward from `from` and wrapping (legacy:135368). */
int BattleConfig_NextFreeColor(const BattleConfig *cfg,
                               int slot_index, int from);

#endif /* TAK_BATTLE_CONFIG_H */
