/*
 * economy.c -- per-player mana pool, regen, and accounting.
 *
 * Single source of truth for "how much mana does player N have".
 * The HUD reads it for display, weapons + build queue spend through
 * Economy_TrySpend, lodestone capture/loss adjusts caps via
 * Economy_AdjustCaps. Numbers come from FBI's maxmana +
 * manarechargerate fields on the monarch (the legacy
 * loader does the same thing — see the legacy reference around
 * str_MaxMana / str_ManaRechargeRate).
 */

#include "tak_economy.h"
#include <string.h>

#define ECONOMY_TICK_HZ 60

void Economy_Init(EconomyState *eco) {
    if (!eco) return;
    memset(eco, 0, sizeof(*eco));
}

static PlayerEconomy *slot_for(EconomyState *eco, int player_id) {
    if (!eco) return NULL;
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return NULL;
    return &eco->players[player_id - 1];
}

static const PlayerEconomy *slot_for_const(const EconomyState *eco, int player_id) {
    if (!eco) return NULL;
    if (player_id < 1 || player_id > TAK_MAX_PLAYERS) return NULL;
    return &eco->players[player_id - 1];
}

void Economy_OnMonarchSpawn(EconomyState *eco, int player_id,
                             int32_t monarch_maxmana,
                             float   monarch_recharge_per_sec) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p) return;
    p->max_mana      += monarch_maxmana;
    p->regen_per_sec += monarch_recharge_per_sec;
    /* The manual: "Each Monarch has a built-in Mana pool that fills
     * automatically over time." Start at full so the player can act
     * immediately rather than waiting through a regen ramp. */
    p->mana = (float)p->max_mana;
}

int32_t Economy_GetMana(const EconomyState *eco, int player_id) {
    const PlayerEconomy *p = slot_for_const(eco, player_id);
    if (!p) return 0;
    return (int32_t)p->mana;
}
int32_t Economy_GetMaxMana(const EconomyState *eco, int player_id) {
    const PlayerEconomy *p = slot_for_const(eco, player_id);
    if (!p) return 0;
    return p->max_mana;
}
int32_t Economy_GetIncome(const EconomyState *eco, int player_id) {
    const PlayerEconomy *p = slot_for_const(eco, player_id);
    if (!p) return 0;
    return p->earned_last_sec;
}
int32_t Economy_GetRegenRate(const EconomyState *eco, int player_id) {
    const PlayerEconomy *p = slot_for_const(eco, player_id);
    if (!p) return 0;
    return (int32_t)(p->regen_per_sec + 0.5f);
}
int32_t Economy_GetSpend(const EconomyState *eco, int player_id) {
    const PlayerEconomy *p = slot_for_const(eco, player_id);
    if (!p) return 0;
    return p->spent_last_sec;
}

int Economy_TrySpend(EconomyState *eco, int player_id, int32_t amount) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p || amount <= 0) return 0;
    if (p->mana < (float)amount) return 0;
    p->mana        -= (float)amount;
    p->spent_accum += (float)amount;
    return 1;
}

float Economy_SpendAvailable(EconomyState *eco, int player_id, float amount) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p || amount <= 0.0f) return 0.0f;
    float paid = amount;
    if (paid > p->mana) paid = p->mana;
    if (paid <= 0.0f) return 0.0f;
    p->mana -= paid;
    p->spent_accum += paid;
    return paid;
}

void Economy_Earn(EconomyState *eco, int player_id, int32_t amount) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p || amount <= 0) return;
    p->mana += (float)amount;
    if (p->mana > (float)p->max_mana) p->mana = (float)p->max_mana;
    p->earned_accum += (float)amount;
}

void Economy_EarnF(EconomyState *eco, int player_id, float amount) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p || amount <= 0.0f) return;
    p->mana += amount;
    if (p->mana > (float)p->max_mana) p->mana = (float)p->max_mana;
    p->earned_accum += amount;
}

void Economy_EarnBounty(EconomyState *eco, int player_id, float amount) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p || amount <= 0.0f) return;
    /* Legacy bounty writes res[0] directly, past the storage cap
     * (legacy:227316 — unlike Resource_Add :8661). */
    p->mana += amount;
    p->earned_accum += amount;
}

void Economy_AdjustCaps(EconomyState *eco, int player_id,
                         int32_t delta_max,
                         float   delta_regen_per_sec) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p) return;
    if (delta_max > 0) {
        /* Legacy spawn/capture path adds mogriumstorage to current
         * mana as well as the special-limit/cap
         * (legacy:226990-226996). */
        p->mana += (float)delta_max;
        p->earned_accum += (float)delta_max;
    }
    p->max_mana      += delta_max;
    if (p->max_mana < 0) p->max_mana = 0;
    p->regen_per_sec += delta_regen_per_sec;
    if (p->regen_per_sec < 0) p->regen_per_sec = 0;
    if (p->mana > (float)p->max_mana) p->mana = (float)p->max_mana;
}

void Economy_Tick(EconomyState *eco) {
    if (!eco) return;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        PlayerEconomy *p = &eco->players[i];
        if (p->max_mana <= 0) continue;

        /* Per-tick regen: rate is per-second; 60Hz sim. earned_accum
         * captures the actual mana added (cap-clamped) as a float so
         * sub-tick fractions don't get lost to int truncation. */
        if (p->regen_per_sec > 0.0f) {
            float gained = p->regen_per_sec / (float)ECONOMY_TICK_HZ;
            float prev = p->mana;
            p->mana += gained;
            if (p->mana > (float)p->max_mana) p->mana = (float)p->max_mana;
            float effective = p->mana - prev;
            if (effective > 0.0f) p->earned_accum += effective;
        }

        /* Roll the per-second window. After every 60 ticks, copy
         * accumulators to *_last_sec and reset. The HUD reads
         * *_last_sec so the +/- indicators are stable instead of
         * flickering every frame. */
        p->ticks_since_window_reset++;
        if (p->ticks_since_window_reset >= ECONOMY_TICK_HZ) {
            p->earned_last_sec = (int32_t)p->earned_accum;
            p->spent_last_sec  = (int32_t)p->spent_accum;
            p->earned_accum    = 0.0f;
            p->spent_accum     = 0.0f;
            p->ticks_since_window_reset = 0;
        }
    }
}
