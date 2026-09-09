#ifndef TAK_GAMELOOP_H
#define TAK_GAMELOOP_H

#include <stdint.h>

/* ── Game state machine ─────────────────────────────────────────────── */

typedef enum GameStateEnum {
    GAMESTATE_MENU            = 0,
    GAMESTATE_BATTLE_SETUP    = 1, // Skirmish lobby
    GAMESTATE_GAME_LOADING    = 2, // Map/assets load, progress bar
    GAMESTATE_IN_GAME         = 3,
    GAMESTATE_CREDITS         = 4, // Movies/Credits.bik
    GAMESTATE_OPTIONS         = 5,
    GAMESTATE_CAMPAIGN        = 6, // Story / Book of Deeds (bod.gui)
    GAMESTATE_MULTIPLAYER     = 7, // Multiplayer battle lobby (battlemenumulti.gui)
    GAMESTATE_QUIT            = -1,
} GameStateEnum;

/* ── Fixed-step timer ───────────────────────────────────────────────── */

#define SIM_TICKS_PER_SECOND 60

typedef struct Timer {
    uint64_t frequency;          /* platform timer frequency (ticks/sec)       */
    uint64_t last_counter;       /* counter value at end of previous frame     */
    double   frame_dt;           /* seconds elapsed since last frame           */
    double   accumulator;        /* accumulated unprocessed sim time           */
    double   sim_dt;             /* fixed sim timestep: 1.0 / SIM_TICKS_PER_SECOND */
    double   alpha;              /* interpolation factor for rendering: 0.0-1.0 */
    int      max_ticks_per_frame;/* spiral-of-death clamp (default: 4)        */
} Timer;

/* Initialize the timer. Call once at startup. */
void Timer_Init(Timer *t);

/* Update the timer. Call once per frame, before consuming ticks. */
void Timer_Update(Timer *t);

/* Consume one simulation tick from the accumulator.
 * Returns 1 if a tick was consumed, 0 if no time remains. */
int Timer_ConsumeTick(Timer *t);

/* Get the interpolation alpha (accumulator / sim_dt).
 * Use this to interpolate visual positions between sim states. */
double Timer_GetAlpha(const Timer *t);

/* Get the raw frame delta time (for UI, audio, non-sim uses). */
double Timer_GetFrameDT(const Timer *t);

void GameLoop_InGameTick(void);

#endif /* TAK_GAMELOOP_H */
