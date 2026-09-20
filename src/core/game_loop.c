/*
 * game_loop.c -- Timer and game loop core
 *
 * Implements the Timer API from tak_gameloop.h.
 *
 */


#include "SDL2/SDL_timer.h"
#include "tak_gameloop.h"

#include <math.h>

/* Initialize the timer. Call once at startup. */
void Timer_Init(Timer *t) {
    t->frequency = SDL_GetPerformanceFrequency();
    t->sim_dt = 1.0 / SIM_TICKS_PER_SECOND;
    t->base_max_ticks = SIM_MAX_TICKS_PER_FRAME;
    t->max_ticks_per_frame = SIM_MAX_TICKS_PER_FRAME;
    t->speed = 1.0;
    t->accumulator = 0;
    t->alpha = 0;
    t->frame_dt = 0;
    t->last_counter = SDL_GetPerformanceCounter();
}

/* Set the game speed and resize the catch-up budget to match it. A
 * budget sized for normal speed is spent by one frame at double speed on
 * a machine that is already behind, and from there every frame throws
 * its surplus away: the readout says 2x and the battle does not run at
 * 2x. Scaling the budget keeps the discarded amount where it was, one
 * frame of wall time (legacy:242401-242416). */
void Timer_SetSpeed(Timer *t, double speed) {
    if (!(speed > 0.0)) speed = 0.0;   /* also catches NaN */
    t->speed = speed;
    int budget = (int)ceil((double)t->base_max_ticks * speed);
    if (budget < t->base_max_ticks) budget = t->base_max_ticks;
    t->max_ticks_per_frame = budget;
}

/* Fold one frame of wall time in. */
void Timer_Advance(Timer *t, double dt) {
    double max_sim_dt = t->max_ticks_per_frame * t->sim_dt;

    /* A frame is worth the hitch cap at most, and the accumulator the
     * frame's tick budget at most, so a browser tab back from an hour
     * in the background runs a frame of ticks and never replays the
     * hour (legacy:242401-242416). Backwards time and NaN are zero. */
    if (!(dt > 0.0)) dt = 0.0;
    if (dt > 0.1) dt = 0.1;   /* hitch cap */
    /* Wall time, never scaled. Camera scroll and cursor animation run on
     * this, and the original scrolls per frame too (legacy:243576), so
     * the view keeps its own pace at any game speed. */
    t->frame_dt = dt;

    t->accumulator += dt * t->speed;
    if (t->accumulator > max_sim_dt)
        t->accumulator = max_sim_dt;
}

/* Update the timer. Call once per frame, before consuming ticks. */
void Timer_Update(Timer *t) {
    Uint64 counter = SDL_GetPerformanceCounter();
    double dt = (double)(counter - t->last_counter) / (double)t->frequency;
    t->last_counter = counter;
    Timer_Advance(t, dt);
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
