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
    GAMESTATE_SELECT_GAME     = 8, // Which game to join (selectgame.gui)
    GAMESTATE_QUIT            = -1,
} GameStateEnum;

/* ── Fixed-step timer ───────────────────────────────────────────────── */

#define SIM_TICKS_PER_SECOND 60

/* The frame budget at normal speed. A frame that wants more than this
 * many ticks gets this many and the surplus is discarded, not banked
 * (legacy:242401-242416), which is also what bounds the catch-up when
 * a browser tab comes back from an hour in the background. */
#define SIM_MAX_TICKS_PER_FRAME 4

typedef struct Timer {
    uint64_t frequency;          /* platform timer frequency (ticks/sec)       */
    uint64_t last_counter;       /* counter value at end of previous frame     */
    double   frame_dt;           /* seconds of WALL time since last frame      */
    double   accumulator;        /* accumulated unprocessed sim time           */
    double   sim_dt;             /* fixed sim timestep: 1.0 / SIM_TICKS_PER_SECOND */
    double   alpha;              /* interpolation factor for rendering: 0.0-1.0 */
    /* Game speed: seconds of battle per second of wall time. 1.0 is
     * normal, 0.0 a full stop, 2.0 the top of the original's range. It
     * scales the time going into the accumulator and nothing else, so a
     * tick keeps its length and keeps its content at every speed. */
    double   speed;
    int      base_max_ticks;     /* the budget at normal speed                 */
    int      max_ticks_per_frame;/* that budget scaled by speed                */
} Timer;

/* Initialize the timer. Call once at startup. */
void Timer_Init(Timer *t);

/* Update the timer. Call once per frame, before consuming ticks. */
void Timer_Update(Timer *t);

/* The clock-free half of Timer_Update: fold `dt` seconds of wall time
 * in. Timer_Update is this plus a read of the platform counter, and a
 * test drives this one so a case is not at the mercy of SDL_Delay.
 * However long `dt` is, the frame is worth the tick budget at most. */
void Timer_Advance(Timer *t, double dt);

/* Set the game speed. The catch-up budget scales with it, otherwise a
 * frame at double speed spends a budget sized for normal speed and the
 * surplus is thrown away every frame: the readout says 2x and the
 * battle does not run at 2x. The budget never falls below the normal
 * one. Out of range values clamp to 0. */
void Timer_SetSpeed(Timer *t, double speed);

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
