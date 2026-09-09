/*
 * test_gameloop.c -- Unit tests for the game loop Timer.
 *
 * These tests validate your Timer implementation. They will fail
 * until you implement the functions in game_loop.c.
 *
 * Build: cmake --build build --target test_gameloop
 * Run:   build/src/Debug/test_gameloop.exe (Windows)
 *        build/src/test_gameloop (Mac/Linux)
 */

#include <SDL.h>
#include "test_framework.h"
#include "tak_gameloop.h"

/* ── Timer_Init ─────────────────────────────────────────────────── */

TEST(timer_init_sets_frequency) {
    Timer t;
    Timer_Init(&t);
    ASSERT(t.frequency > 0);
}

TEST(timer_init_sets_sim_dt) {
    Timer t;
    Timer_Init(&t);
    /* sim_dt should be ~1/60 = 0.01667 */
    ASSERT(t.sim_dt > 0.016 && t.sim_dt < 0.017);
}

TEST(timer_init_sets_max_ticks) {
    Timer t;
    Timer_Init(&t);
    ASSERT_EQ_INT(4, t.max_ticks_per_frame);
}

TEST(timer_init_accumulator_is_zero) {
    Timer t;
    Timer_Init(&t);
    ASSERT(t.accumulator == 0.0);
}

/* ── Timer_Update + Timer_ConsumeTick ───────────────────────────── */

TEST(timer_100ms_yields_about_6_ticks) {
    Timer t;
    Timer_Init(&t);
    SDL_Delay(100);
    Timer_Update(&t);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    /* 100ms / 16.67ms = ~6 ticks, allow some slack */
    ASSERT(ticks >= 4 && ticks <= 8);
}

TEST(timer_50ms_yields_about_3_ticks) {
    Timer t;
    Timer_Init(&t);
    SDL_Delay(50);
    Timer_Update(&t);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT(ticks >= 2 && ticks <= 5);
}

TEST(timer_no_delay_yields_zero_or_one_ticks) {
    Timer t;
    Timer_Init(&t);
    Timer_Update(&t);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT(ticks <= 1);
}

/* ── Spiral-of-death clamp ──────────────────────────────────────── */

TEST(timer_spiral_of_death_clamp) {
    /* >250ms gap = tab-away/debugger pause: debt DROPPED, not replayed
     * (the WASM refocus-slowdown fix). */
    Timer t;
    Timer_Init(&t);
    SDL_Delay(1000);
    Timer_Update(&t);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(0, ticks);
    ASSERT(t.accumulator == 0.0);
    ASSERT(t.frame_dt == t.sim_dt);
}

TEST(timer_accumulator_debt_is_clamped) {
    Timer t;
    Timer_Init(&t);
    t.accumulator = 100.0;  /* sim debt from menu/loading must not leak */
    Timer_Update(&t);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(t.max_ticks_per_frame, ticks);
}

/* ── Timer_GetAlpha ─────────────────────────────────────────────── */

TEST(timer_alpha_in_range) {
    Timer t;
    Timer_Init(&t);
    SDL_Delay(20);  /* slightly more than one tick */
    Timer_Update(&t);
    while (Timer_ConsumeTick(&t)) {}
    double alpha = Timer_GetAlpha(&t);
    ASSERT(alpha >= 0.0 && alpha < 1.0);
}

TEST(timer_alpha_is_zero_when_exact) {
    Timer t;
    Timer_Init(&t);
    /* After consuming all ticks with no remainder, alpha should be near 0 */
    SDL_Delay(100);
    Timer_Update(&t);
    while (Timer_ConsumeTick(&t)) {}
    double alpha = Timer_GetAlpha(&t);
    /* Alpha is the fractional remainder -- should be small but >= 0 */
    ASSERT(alpha >= 0.0 && alpha < 1.0);
}

/* ── Timer_GetFrameDT ───────────────────────────────────────────── */

TEST(timer_frame_dt_after_100ms) {
    Timer t;
    Timer_Init(&t);
    SDL_Delay(100);
    Timer_Update(&t);
    double dt = Timer_GetFrameDT(&t);
    /* Should be roughly 0.1 seconds, but clamped to max_ticks * sim_dt
       if spiral-of-death kicks in. 4 * 0.01667 = 0.0667.
       Since 100ms < 4 ticks worth (66.7ms)... wait, 100ms > 66.7ms,
       so the accumulator is clamped. frame_dt should be clamped to ~0.0667. */
    ASSERT(dt > 0.05 && dt < 0.15);
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    SDL_Init(SDL_INIT_TIMER);

    TEST_SUITE("Timer_Init");
    RUN(timer_init_sets_frequency);
    RUN(timer_init_sets_sim_dt);
    RUN(timer_init_sets_max_ticks);
    RUN(timer_init_accumulator_is_zero);

    TEST_SUITE("Timer_Update + Timer_ConsumeTick");
    RUN(timer_100ms_yields_about_6_ticks);
    RUN(timer_50ms_yields_about_3_ticks);
    RUN(timer_no_delay_yields_zero_or_one_ticks);

    TEST_SUITE("Spiral-of-death clamp");
    RUN(timer_spiral_of_death_clamp);
    RUN(timer_accumulator_debt_is_clamped);

    TEST_SUITE("Timer_GetAlpha");
    RUN(timer_alpha_in_range);
    RUN(timer_alpha_is_zero_when_exact);

    TEST_SUITE("Timer_GetFrameDT");
    RUN(timer_frame_dt_after_100ms);

    SDL_Quit();
    TEST_REPORT();
}
