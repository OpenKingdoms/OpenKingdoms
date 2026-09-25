/*
 * test_economy.c -- unit tests for the per-player mana economy.
 *
 * Run with: build/src/Debug/test_economy.exe
 *
 * Validates:
 *   - Initial state is zero across all slots
 *   - Spawn hook seeds mana to max
 *   - Per-tick regen at 60Hz matches the per-second rate over 60 ticks
 *   - Cap clamping
 *   - Insufficient-mana spend rejection
 */

#include "tak_economy.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void test_init_zero(void) {
    EconomyState eco;
    Economy_Init(&eco);
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        EXPECT(Economy_GetMana(&eco, p)    == 0);
        EXPECT(Economy_GetMaxMana(&eco, p) == 0);
        EXPECT(Economy_GetIncome(&eco, p)  == 0);
        EXPECT(Economy_GetSpend(&eco, p)   == 0);
    }
}

static void test_monarch_seed(void) {
    EconomyState eco;
    Economy_Init(&eco);
    /* Aramon Elsin: maxmana=1000, manarechargerate=20/sec. */
    Economy_OnMonarchSpawn(&eco, 1, 1000, 20.0f);
    EXPECT(Economy_GetMana(&eco, 1)    == 1000);  /* starts full */
    EXPECT(Economy_GetMaxMana(&eco, 1) == 1000);
}

static void test_regen_one_second(void) {
    EconomyState eco;
    Economy_Init(&eco);
    /* Spend 500 then regen for exactly 1 second (60 ticks at 60Hz)
     * with 20/sec rate — should add ~20 mana. */
    Economy_OnMonarchSpawn(&eco, 1, 1000, 20.0f);
    EXPECT(Economy_TrySpend(&eco, 1, 500) == 1);
    EXPECT(Economy_GetMana(&eco, 1) == 500);
    for (int i = 0; i < 60; i++) Economy_Tick(&eco);
    int m = Economy_GetMana(&eco, 1);
    /* Allow ±1 for float floor truncation. */
    EXPECT(m >= 519 && m <= 521);
    /* After 60 ticks the per-second window rolls; income should be ~20. */
    EXPECT(Economy_GetIncome(&eco, 1) >= 19);
    EXPECT(Economy_GetIncome(&eco, 1) <= 21);
    EXPECT(Economy_GetSpend(&eco, 1)  == 500);
}

static void test_cap_clamp(void) {
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 100, 1000.0f);  /* huge regen */
    /* Already at cap; ticking shouldn't push past it. */
    for (int i = 0; i < 600; i++) Economy_Tick(&eco);
    EXPECT(Economy_GetMana(&eco, 1) == 100);
}

static void test_insufficient_spend(void) {
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 100, 0.0f);
    EXPECT(Economy_TrySpend(&eco, 1, 50)  == 1);
    EXPECT(Economy_TrySpend(&eco, 1, 100) == 0);  /* would overdraft */
    EXPECT(Economy_GetMana(&eco, 1)       == 50); /* unchanged */
}

static void test_fractional_spend_available(void) {
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 10, 0.0f);
    float paid = Economy_SpendAvailable(&eco, 1, 2.5f);
    EXPECT(fabsf(paid - 2.5f) < 0.001f);
    EXPECT(Economy_GetMana(&eco, 1) == 7);
    paid = Economy_SpendAvailable(&eco, 1, 20.0f);
    EXPECT(fabsf(paid - 7.5f) < 0.001f);
    EXPECT(Economy_GetMana(&eco, 1) == 0);
    paid = Economy_SpendAvailable(&eco, 1, 1.0f);
    EXPECT(fabsf(paid) < 0.001f);
}

/* Issue #252. A barracks was reported building nothing at all beside a
 * second one, at an empty pool with income still coming in. The
 * treasury used to pay whoever asked first, so the first consumer took
 * the whole trickle every tick and the second never saw a mana. The
 * original hands the same share to everyone who asks
 * (legacy:235971-235977), so both creep along. */
static void test_two_consumers_share_a_dry_pool(void) {
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 5200, 12.0f);
    eco.players[0].mana = 0.0f;

    /* Each asks for far more than the income covers, the way two
     * barracks each training a swordsman do. */
    const float want = 5.0f;
    float first = 0.0f, second = 0.0f;
    for (int t = 0; t < 600; t++) {
        first  += Economy_SpendAvailable(&eco, 1, want);
        second += Economy_SpendAvailable(&eco, 1, want);
        Economy_Tick(&eco);
    }
    printf("(first %.2f, second %.2f, share %.4f)\n",
           first, second, (double)Economy_GetShare(&eco, 1));

    /* Both are fed, and fed alike. */
    EXPECT(second > 0.0f);
    EXPECT(fabsf(first - second) < 0.05f * first);
    /* Together they spend the income and no more: ten seconds at
     * twelve a second. */
    EXPECT(fabsf((first + second) - 120.0f) < 2.0f);
    EXPECT(Economy_GetShare(&eco, 1) < 1.0f);
}

/* A pool that covers everything asked of it pays in full: the share
 * only bites when the treasury is short. */
static void test_a_full_pool_pays_in_full(void) {
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 5200, 12.0f);
    float first = 0.0f, second = 0.0f;
    for (int t = 0; t < 60; t++) {
        first  += Economy_SpendAvailable(&eco, 1, 1.0f);
        second += Economy_SpendAvailable(&eco, 1, 1.0f);
        Economy_Tick(&eco);
    }
    EXPECT(fabsf(first - 60.0f) < 0.001f);
    EXPECT(fabsf(second - 60.0f) < 0.001f);
    EXPECT(fabsf(Economy_GetShare(&eco, 1) - 1.0f) < 0.001f);
}

static void test_lodestone_adjust(void) {
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 1000, 20.0f);
    /* Capture an Aramon lodestone: +1000 cap, +10/sec regen. */
    Economy_AdjustCaps(&eco, 1, 1000, 10.0f);
    EXPECT(Economy_GetMaxMana(&eco, 1) == 2000);
    /* Legacy adds mogriumstorage to current mana immediately. */
    EXPECT(Economy_GetMana(&eco, 1) == 2000);
    /* Lose the lodestone: -1000 cap, -10/sec regen. Mana must clamp. */
    Economy_AdjustCaps(&eco, 1, -1500, -25.0f);  /* take more than we have */
    EXPECT(Economy_GetMaxMana(&eco, 1) == 500);  /* no negative */
    EXPECT(Economy_GetMana(&eco, 1)    == 500);  /* clamped down */
}

/* A gift moves what the giver holds and the receiver has room for, and
 * the giver keeps the rest (legacy:206055-206087). */
static void test_transfer_moves_what_fits(void) {
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 5000, 0.0f);
    Economy_OnMonarchSpawn(&eco, 2, 1000, 0.0f);
    eco.players[1].mana = 900.0f;
    float moved = Economy_Transfer(&eco, 1, 2, 500.0f);
    EXPECT(fabsf(moved - 100.0f) < 0.001f);
    EXPECT(Economy_GetMana(&eco, 1) == 4900);
    EXPECT(Economy_GetMana(&eco, 2) == 1000);
    eco.players[0].mana = 30.0f;
    eco.players[1].mana = 0.0f;
    moved = Economy_Transfer(&eco, 1, 2, 500.0f);
    EXPECT(fabsf(moved - 30.0f) < 0.001f);
    EXPECT(Economy_GetMana(&eco, 1) == 0);
    EXPECT(Economy_GetMana(&eco, 2) == 30);
}

/* A full pool sharing with one ally passes it 25 a frame of the
 * original's, 12.5 a tick of ours, and nothing with the share off or
 * at half full (legacy:206694-206725). */
static void test_allies_share_what_is_over_half(void) {
    static uint8_t share[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1];
    memset(share, 0, sizeof(share));
    EconomyState eco;
    Economy_Init(&eco);
    Economy_OnMonarchSpawn(&eco, 1, 5000, 0.0f);
    Economy_OnMonarchSpawn(&eco, 2, 5000, 0.0f);
    eco.players[1].mana = 0.0f;
    Economy_ShareMana(&eco, (const uint8_t (*)[TAK_MAX_PLAYERS + 1])share);
    EXPECT(Economy_GetMana(&eco, 2) == 0);
    share[1][2] = 1;
    Economy_ShareMana(&eco, (const uint8_t (*)[TAK_MAX_PLAYERS + 1])share);
    printf("  (shared %.2f in a tick)\n", (double)eco.players[1].mana);
    EXPECT(fabsf(eco.players[1].mana - 12.5f) < 0.001f);
    EXPECT(fabsf(eco.players[0].mana - 4987.5f) < 0.001f);
    eco.players[0].mana = 2500.0f;
    eco.players[1].mana = 0.0f;
    Economy_ShareMana(&eco, (const uint8_t (*)[TAK_MAX_PLAYERS + 1])share);
    EXPECT(eco.players[1].mana == 0.0f);
}

int main(void) {
    test_init_zero();
    test_monarch_seed();
    test_transfer_moves_what_fits();
    test_allies_share_what_is_over_half();
    test_regen_one_second();
    test_cap_clamp();
    test_insufficient_spend();
    test_fractional_spend_available();
    test_two_consumers_share_a_dry_pool();
    test_a_full_pool_pays_in_full();
    test_lodestone_adjust();

    if (g_failures == 0) {
        printf("OK  test_economy: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL  test_economy: %d failure(s)\n", g_failures);
    return 1;
}
