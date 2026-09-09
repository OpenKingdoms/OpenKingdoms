/*
 * game_loop.c -- Timer and game loop core
 *
 * Implements the Timer API from tak_gameloop.h.
 *
 */


#include "SDL2/SDL_timer.h"
#include "tak_gameloop.h"

/* Initialize the timer. Call once at startup. */
void Timer_Init(Timer *t) {
    t->frequency = SDL_GetPerformanceFrequency();
    t->sim_dt = 1.0 / SIM_TICKS_PER_SECOND;
    t->max_ticks_per_frame = 4;
    t->accumulator = 0;
    t->alpha = 0;
    t->frame_dt = 0;
    t->last_counter = SDL_GetPerformanceCounter();
}

/* Update the timer. Call once per frame, before consuming ticks. */
void Timer_Update(Timer *t) {
    double max_sim_dt = t->max_ticks_per_frame * t->sim_dt;

    Uint64 counter = SDL_GetPerformanceCounter();
    double dt = (double)(counter - t->last_counter) / (double)t->frequency;
    t->last_counter = counter;

    /* rAF pause (hidden browser tab) / debugger: drop the debt, don't
     * fast-forward — a saturated accumulator locks max catch-up every
     * frame after refocus (WASM slowdown bug). */
    if (dt > 0.25) {
        t->frame_dt = t->sim_dt;
        t->accumulator = 0.0;
        return;
    }
    if (dt > 0.1) dt = 0.1;   /* hitch cap */
    t->frame_dt = dt;

    t->accumulator += dt;
    if (t->accumulator > max_sim_dt)
        t->accumulator = max_sim_dt;
}

/* Consume one simulation tick from the accumulator.
 * Returns 1 if a tick was consumed, 0 if no time remains. */
int Timer_ConsumeTick(Timer *t) {
    if (t->accumulator >= t->sim_dt) {
        t->accumulator -= t->sim_dt;
        return 1;
    }

    return 0;
}

/* Get the interpolation alpha (accumulator / sim_dt).
 * Use this to interpolate visual positions between sim states. */
double Timer_GetAlpha(const Timer *t) {
    return t->accumulator / t->sim_dt;
}

/* Get the raw frame delta time (for UI, audio, non-sim uses). */
double Timer_GetFrameDT(const Timer *t) {
    return t->frame_dt;
}
