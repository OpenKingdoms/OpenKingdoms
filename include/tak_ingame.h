#ifndef TAK_INGAME_H
#define TAK_INGAME_H

#include "tak_platform.h"
#include "tak_gameloop.h"

/* ── In-game screen (GAMESTATE_IN_GAME) ──────────────────────────────
 *
 * Phase B scope: read the loaded GameWorld, clear the offscreen surface,
 * render terrain, return. ESC → exit to menu. No sim, no HUD, no units.
 *
 * Phase D will absorb: Simulation_Step inside a Timer_ConsumeTick loop,
 * alpha interpolation, unit rendering, HUD overlay, mouse-edge camera
 * scrolling. See PHASE_B_PLAN.md §6-§9 and PORTING_GUIDE §14 for the
 * "driving the battle frame" pattern this module grows into. */

/* Returns 0 on success, -1 if no loaded world is available (caller
 * should transition back to menu). */
int  InGame_Init(TAK_Platform *platform);

/* Renders one frame. Returns the next state: GAMESTATE_IN_GAME normally,
 * GAMESTATE_MENU on ESC. */
int  InGame_Tick(TAK_Platform *platform, Timer *timer);

/* Frees transient per-session resources. Does NOT free the GameWorld —
 * main.c's case owns that lifecycle via World_End(). */
void InGame_Shutdown(void);

/* Test/debug hook: advance the fixed-step simulation by `ticks` 60Hz
 * steps without rendering a frame. Runs the exact same per-tick systems
 * as InGame_Tick's Timer_ConsumeTick loop (AI, engines, economy, fog,
 * end-state rules). Integration tests use this to fast-forward a
 * skirmish without paying software-render cost per frame. */
void InGame_DebugRunSimTicks(int ticks);

/* One left click on the game world at a world position, with the shift
 * state. The tick calls this on release; tests drive the same dispatch
 * (pending HUD command, select, attack, Move) without a mouse. */
void InGame_WorldClick(int32_t world_x, int32_t world_y, int shift_held);
/* The cursor the world shows under a point with no command armed, and
 * the one an armed command shows there (a HUD_CMD_* or HUD_CUR_* id). */
int  InGame_HoverCursorAt(int32_t world_x, int32_t world_y);
int  InGame_CommandCursorAt(int mode, int32_t world_x, int32_t world_y);

/* One left-button drag box on the game world, corners in world
 * coordinates. The tick calls this on release and tests call it
 * directly. */
void InGame_WorldDrag(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int shift_held);

/* Test seam: a control group key, assign = ctrl held. Runs the same
 * dispatch the keyboard reaches. */
void InGame_DebugControlGroup(int digit, int assign);

/* Test seam: the Escape key, `down` being its current physical state.
 * SDL_GetKeyboardState cannot be driven from a test, so the edge
 * detection and the cancel body both run from here. */
void InGame_DebugEscape(int down);

#endif /* TAK_INGAME_H */
