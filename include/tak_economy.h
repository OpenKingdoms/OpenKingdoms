#ifndef TAK_ECONOMY_H
#define TAK_ECONOMY_H

#include "tak_battle_config.h"
#include <stdint.h>

/* ── Per-player mana economy ─────────────────────────────────────
 *
 * TAK's "mana" is the single resource. Each player has:
 *   - current mana (float for fractional regen each tick)
 *   - max pool   (monarch maxmana + lodestone bonuses)
 *   - regen rate (monarch manarechargerate + lodestone bonuses)
 *
 * Rates in FBI are *per-second*; the legacy reference multiplies
 * by 1/30 (TAK ran a 30Hz sim). We run a 60Hz sim, so per-tick
 * regen is rate / 60.0.
 *
 * Mana is *spent* by units that fire mana-cost weapons (manapershot)
 * and by build queues. Spending lives in unit/build code; this
 * module just owns the bookkeeping. */

typedef struct PlayerEconomy {
    float    mana;            /* current mana (float for sub-tick regen) */
    int32_t  max_mana;        /* derived: monarch + lodestone caps      */
    float    regen_per_sec;   /* derived: monarch + lodestone rates     */
    int32_t  spent_last_sec;  /* sliding window for "-N" indicator      */
    int32_t  earned_last_sec; /* sliding window for "+N" indicator      */

    /* Sliding-window accumulators — drained into the *_last_sec
     * fields once per second so HUD shows stable rates. earned_accum
     * is float because per-tick gains are fractional (e.g. 20/sec at
     * 60Hz = 0.333/tick) and casting each delta to int loses
     * everything below 1. */
    float    earned_accum;
    float    spent_accum;
    int32_t  ticks_since_window_reset;
} PlayerEconomy;

typedef struct EconomyState {
    PlayerEconomy players[TAK_MAX_PLAYERS];   /* indexed [0..TAK_MAX_PLAYERS-1] (player_id - 1) */
    int          active_count;                /* slots with kind != CLOSED */
} EconomyState;

/* Fresh state — every player gets a 0-mana, 0-cap pool. Pools fill
 * in once their monarch spawns and Economy_OnMonarchSpawn is called. */
void Economy_Init(EconomyState *eco);

/* Called from Units_Spawn when a unit with maxmana > 0 (typically the
 * monarch) is created for `player_id`. Sets initial pool to monarch's
 * maxmana and starts regen at monarch's manarechargerate. Adds to
 * existing values if the player already has bonuses (e.g. lodestones
 * captured first by some future scripted scenario). */
void Economy_OnMonarchSpawn(EconomyState *eco, int player_id,
                             int32_t monarch_maxmana,
                             float   monarch_recharge_per_sec);

/* Accessors used by HUD + AI. player_id is 1-based (matches Unit.player_id). */
int32_t Economy_GetMana   (const EconomyState *eco, int player_id);
int32_t Economy_GetMaxMana(const EconomyState *eco, int player_id);
/* Earned-last-second; goes to 0 when the pool is at cap (regen is
 * being clamped). For the HUD's "+N" display you usually want
 * GetRegenRate instead — that shows production capacity even at cap,
 * matching the legacy display. */
int32_t Economy_GetIncome (const EconomyState *eco, int player_id);  /* +N/sec actual */
int32_t Economy_GetRegenRate(const EconomyState *eco, int player_id);/* +N/sec capacity */
int32_t Economy_GetSpend  (const EconomyState *eco, int player_id);  /* -N/sec */

/* Try to spend `amount`. Returns 1 on success (mana decremented),
 * 0 if insufficient. Used by weapon firing + build queue. */
int  Economy_TrySpend(EconomyState *eco, int player_id, int32_t amount);

/* Spend up to `amount` and return the actual amount paid. Used by
 * construction, where legacy TAK scales progress down instead of
 * requiring a whole integer cost chunk to be available. */
float Economy_SpendAvailable(EconomyState *eco, int player_id, float amount);

/* Add mana (e.g. lodestone capture). Caps at max_mana. */
void Economy_Earn   (EconomyState *eco, int player_id, int32_t amount);

/* Fractional earn (nanoframe decay refund — legacy refunds mana
 * continuously and proportionally as an abandoned frame decays,
 * legacy:39510-39524). Caps at max_mana. */
void Economy_EarnF  (EconomyState *eco, int player_id, float amount);

/* Kill bounty (FBI mogriumbounty). The legacy award writes the
 * resource directly and BYPASSES the max-mana clamp
 * (legacy:227316-227319 vs Resource_Add :8661). */
void Economy_EarnBounty(EconomyState *eco, int player_id, float amount);

/* Bump max + regen — used when a lodestone is captured. Negative
 * deltas allowed for losing a lodestone. */
void Economy_AdjustCaps(EconomyState *eco, int player_id,
                         int32_t delta_max,
                         float   delta_regen_per_sec);

/* One-tick advance at 60Hz. Regenerates mana, drains the per-second
 * sliding window. Call from the game's per-frame tick before HUD draw. */
void Economy_Tick(EconomyState *eco);

#endif /* TAK_ECONOMY_H */
