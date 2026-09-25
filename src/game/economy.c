/*
 * economy.c -- per-player mana pool, regen, and accounting.
 *
 * Single source of truth for "how much mana does player N have".
 * The HUD reads it for display, weapons + build queue spend through
 * Economy_TrySpend. The cap and the rate are the sum of the player's
 * finished units' mogriumstorage and mogriumincome, set every tick by
 * Units_RecomputeEconomy through Economy_SetPool.
 */

#include "tak_economy.h"
#include <string.h>

#define ECONOMY_TICK_HZ 60

void Economy_Init(EconomyState *eco) {
    if (!eco) return;
    memset(eco, 0, sizeof(*eco));
    /* Nobody is short of anything until a tick says so. */
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) eco->players[i].share = 1.0f;
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
                             int32_t storage, float income_per_sec) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p) return;
    p->max_mana      += storage;
    p->regen_per_sec += income_per_sec;
    p->mana          += (float)storage;
    if (p->mana > (float)p->max_mana) p->mana = (float)p->max_mana;
}

void Economy_SetPool(EconomyState *eco, int player_id,
                     int32_t storage, float income_per_sec) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p) return;
    storage += p->bonus_storage;
    income_per_sec += p->bonus_income;
    p->max_mana = storage > 0 ? storage : 1;
    p->regen_per_sec = income_per_sec > 0.0f ? income_per_sec : 0.0f;
}

void Economy_AdjustCaps(EconomyState *eco, int player_id,
                        int32_t delta_storage, float delta_income_per_sec) {
    PlayerEconomy *p = slot_for(eco, player_id);
    if (!p) return;
    p->bonus_storage += delta_storage;
    p->bonus_income += delta_income_per_sec;
    p->max_mana += delta_storage;
    if (p->max_mana < 1) p->max_mana = 1;
    p->regen_per_sec += delta_income_per_sec;
    if (p->regen_per_sec < 0.0f) p->regen_per_sec = 0.0f;
    if (delta_storage > 0) p->mana += (float)delta_storage;
    if (p->mana > (float)p->max_mana) p->mana = (float)p->max_mana;
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
float Economy_GetShare(const EconomyState *eco, int player_id) {
    const PlayerEconomy *p = slot_for_const(eco, player_id);
    if (!p) return 0.0f;
    return p->share;
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
    /* The whole of what was asked counts as demand, before the share
     * trims it, or the next tick's share would be worked out from the
     * starved figure and the treasury would never climb back out
     * (legacy:39479-39482). */
    p->demand_accum += amount;
    float paid = amount * p->share;
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

float Economy_Transfer(EconomyState *eco, int from_player, int to_player,
                       float amount) {
    PlayerEconomy *a = slot_for(eco, from_player);
    PlayerEconomy *b = slot_for(eco, to_player);
    if (!a || !b || a == b) return 0.0f;
    if (amount > a->mana) amount = a->mana;
    if (!(amount > 0.0f)) return 0.0f;
    float room = (float)b->max_mana - b->mana;
    if (amount > room) amount = room;
    if (!(amount > 0.0f)) return 0.0f;
    a->mana -= amount;
    b->mana += amount;
    return amount;
}

#define SHARE_FILL    0.5f    /* legacy data at 0x617048 */
#define SHARE_RATE    0.01f   /* legacy data at 0x61704c */
#define SHARE_FRAMES  30.0f   /* the original's frames a second */

void Economy_ShareMana(EconomyState *eco,
                       const uint8_t share[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1]) {
    if (!eco || !share) return;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        PlayerEconomy *e = slot_for(eco, p);
        if (!e || e->max_mana <= 1) continue;
        float fill = e->mana / (float)e->max_mana;
        if (!(fill > SHARE_FILL)) continue;
        int with = 0;
        for (int q = 1; q <= TAK_MAX_PLAYERS; q++)
            if (q != p && share[p][q]) with++;
        if (with == 0) continue;
        float each = (fill - SHARE_FILL) * SHARE_RATE * (float)e->max_mana / (float)with
                   * (SHARE_FRAMES / (float)ECONOMY_TICK_HZ);
        for (int q = 1; q <= TAK_MAX_PLAYERS; q++)
            if (q != p && share[p][q]) Economy_Transfer(eco, p, q, each);
    }
}

void Economy_Tick(EconomyState *eco) {
    if (!eco) return;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        PlayerEconomy *p = &eco->players[i];
        if (p->max_mana <= 0) {
            p->share = 1.0f;
            p->demand_accum = 0.0f;
            continue;
        }

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

        /* What the treasury can cover of what was asked of it, income
         * counted in first. Everyone who asks next tick is trimmed by
         * this one figure, so two factories at an empty pool both
         * creep along at the rate the income buys instead of the
         * first in the list taking the lot (legacy:235971-235977).
         * The demand is the tick's, so it is cleared once read. */
        if (p->demand_accum > 0.0f && p->mana < p->demand_accum) {
            p->share = p->mana > 0.0f ? p->mana / p->demand_accum : 0.0f;
        } else {
            p->share = 1.0f;
        }
        p->demand_accum = 0.0f;

        /* The end of the original's frame: nothing stays over the cap
         * or under nothing (legacy:8774-8784). */
        if (p->mana > (float)p->max_mana) p->mana = (float)p->max_mana;
        if (p->mana < 0.0f) p->mana = 0.0f;

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
