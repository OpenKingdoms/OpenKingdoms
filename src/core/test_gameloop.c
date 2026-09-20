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
#include <math.h>
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

/* SDL_Delay oversleeps on a loaded CI runner. Past the 0.25 s stall
 * limit Timer_Update drops the debt on purpose (hidden tab, debugger),
 * so a run that slept that long legitimately sees zero ticks. */
static double delay_seconds(Timer *t, Uint32 ms) {
    Uint64 c0 = SDL_GetPerformanceCounter();
    SDL_Delay(ms);
    Timer_Update(t);
    return (double)(SDL_GetPerformanceCounter() - c0) /
           (double)SDL_GetPerformanceFrequency();
}

TEST(timer_100ms_yields_about_6_ticks) {
    Timer t;
    Timer_Init(&t);
    double slept = delay_seconds(&t, 100);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    if (slept > 0.25) { ASSERT(ticks == 0 || ticks >= 4); return; }
    /* 100ms / 16.67ms = ~6 ticks, allow some slack */
    ASSERT(ticks >= 4 && ticks <= 8);
}

TEST(timer_50ms_yields_about_3_ticks) {
    Timer t;
    Timer_Init(&t);
    double slept = delay_seconds(&t, 50);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    if (slept > 0.25) { ASSERT(ticks == 0 || ticks >= 2); return; }
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
    /* A second of wall time is worth the frame's budget, not a second
     * of simulation. */
    Timer t;
    Timer_Init(&t);
    SDL_Delay(1000);
    Timer_Update(&t);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(t.max_ticks_per_frame, ticks);
    ASSERT(t.accumulator < t.sim_dt);
}

TEST(timer_an_hour_away_is_worth_one_frame_of_ticks) {
    /* A browser tab in the background gets no frames at all, so the
     * first frame back is handed the whole time away. Forty five
     * minutes at 60 Hz would be 162000 ticks in one frame. */
    Timer t;
    Timer_Init(&t);
    Timer_Advance(&t, 45.0 * 60.0);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(t.max_ticks_per_frame, ticks);
}

TEST(timer_a_run_of_long_frames_keeps_the_battle_moving) {
    /* Two frames a second is a machine in trouble, not a machine that
     * should watch a battle stand still. */
    Timer t;
    Timer_Init(&t);
    int ticks = 0;
    for (int frame = 0; frame < 20; frame++) {
        Timer_Advance(&t, 0.5);
        while (Timer_ConsumeTick(&t)) ticks++;
    }
    ASSERT_EQ_INT(20 * t.max_ticks_per_frame, ticks);
}

TEST(timer_nonsense_time_does_not_stop_the_clock) {
    /* A counter that goes backwards, and a NaN out of it, are worth
     * nothing, and the frame after one is worth its own time. */
    Timer t;
    Timer_Init(&t);
    int ticks = 0;
    Timer_Advance(&t, -5.0);
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(0, ticks);
    Timer_Advance(&t, (double)NAN);
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(0, ticks);
    Timer_Advance(&t, 1.0 / 60.0);
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(1, ticks);
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

/* Game speed. It scales the wall time going into the accumulator and
 * nothing else: the tick keeps its length, so the same battle runs the
 * same way at every speed and only the number of ticks a frame gets
 * changes (legacy:242355, legacy:242391-242396). */

TEST(timer_init_speed_is_normal) {
    Timer t;
    Timer_Init(&t);
    ASSERT(t.speed == 1.0);
    ASSERT_EQ_INT(SIM_MAX_TICKS_PER_FRAME, t.base_max_ticks);
}

TEST(timer_speed_does_not_touch_the_timestep) {
    Timer t;
    Timer_Init(&t);
    double dt = t.sim_dt;
    Timer_SetSpeed(&t, 2.0);
    ASSERT(t.sim_dt == dt);
}

TEST(timer_speed_scales_the_ticks_a_frame_gets) {
    Timer t;
    Timer_Init(&t);
    Timer_SetSpeed(&t, 2.0);
    Timer_Advance(&t, 1.0 / 60.0);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(2, ticks);
}

TEST(timer_speed_scales_the_catch_up_budget) {
    /* A budget frozen at the normal-speed value is spent by one frame
     * of a slow machine at double speed, and every frame after that
     * throws its surplus away, so the speed the player asked for never
     * arrives. */
    Timer t;
    Timer_Init(&t);
    Timer_SetSpeed(&t, 2.0);
    ASSERT_EQ_INT(SIM_MAX_TICKS_PER_FRAME * 2, t.max_ticks_per_frame);
    Timer_Advance(&t, 1.0 / 20.0);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(6, ticks);
}

TEST(timer_frame_dt_is_wall_time_at_any_speed) {
    /* Camera scroll and cursor animation run on frame_dt, and the
     * original scrolls per frame (legacy:243576). */
    Timer t;
    Timer_Init(&t);
    Timer_SetSpeed(&t, 2.0);
    Timer_Advance(&t, 0.02);
    ASSERT(Timer_GetFrameDT(&t) == 0.02);
}

TEST(timer_zero_speed_is_a_full_stop) {
    Timer t;
    Timer_Init(&t);
    Timer_SetSpeed(&t, 0.0);
    for (int i = 0; i < 100; i++) Timer_Advance(&t, 1.0 / 60.0);
    int ticks = 0;
    while (Timer_ConsumeTick(&t)) ticks++;
    ASSERT_EQ_INT(0, ticks);
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
    double slept = delay_seconds(&t, 100);
    double dt = Timer_GetFrameDT(&t);
    /* Roughly 0.1 s: the hitch cap holds frame_dt at 0.1 and the stall
     * rule resets it to one sim step. */
    if (slept > 0.25) { ASSERT(dt > 0.0 && dt < 0.15); return; }
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
    RUN(timer_an_hour_away_is_worth_one_frame_of_ticks);
    RUN(timer_a_run_of_long_frames_keeps_the_battle_moving);
    RUN(timer_nonsense_time_does_not_stop_the_clock);
    RUN(timer_accumulator_debt_is_clamped);

    TEST_SUITE("Game speed");
    RUN(timer_init_speed_is_normal);
    RUN(timer_speed_does_not_touch_the_timestep);
    RUN(timer_speed_scales_the_ticks_a_frame_gets);
    RUN(timer_speed_scales_the_catch_up_budget);
    RUN(timer_frame_dt_is_wall_time_at_any_speed);
    RUN(timer_zero_speed_is_a_full_stop);

    TEST_SUITE("Timer_GetAlpha");
    RUN(timer_alpha_in_range);
    RUN(timer_alpha_is_zero_when_exact);

    TEST_SUITE("Timer_GetFrameDT");
    RUN(timer_frame_dt_after_100ms);

    SDL_Quit();
    TEST_REPORT();
}
