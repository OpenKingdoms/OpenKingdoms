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

int main(void) {
    test_init_zero();
    test_monarch_seed();
    test_regen_one_second();
    test_cap_clamp();
    test_insufficient_spend();
    test_fractional_spend_available();
    test_lodestone_adjust();

    if (g_failures == 0) {
        printf("OK  test_economy: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL  test_economy: %d failure(s)\n", g_failures);
    return 1;
}
