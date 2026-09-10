#ifndef TAK_INGAME_H
#define TAK_INGAME_H

#include "tak_platform.h"
#include "tak_gameloop.h"

/* â”€â”€ In-game screen (GAMESTATE_IN_GAME) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * Phase B scope: read the loaded GameWorld, clear the offscreen surface,
 * render terrain, return. ESC â†’ exit to menu. No sim, no HUD, no units.
 *
 * Phase D will absorb: Simulation_Step inside a Timer_ConsumeTick loop,
 * alpha interpolation, unit rendering, HUD overlay, mouse-edge camera
 * scrolling. See PHASE_B_PLAN.md Â§6-Â§9 and PORTING_GUIDE Â§14 for the
 * "driving the battle frame" pattern this module grows into. */

/* Returns 0 on success, -1 if no loaded world is available (caller
 * should transition back to menu). */
int  InGame_Init(TAK_Platform *platform);

/* Renders one frame. Returns the next state: GAMESTATE_IN_GAME normally,
 * GAMESTATE_MENU on ESC. */
int  InGame_Tick(TAK_Platform *platform, Timer *timer);

/* Frees transient per-session resources. Does NOT free the GameWorld â€”
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

/* One left-button drag box on the game world, corners in world
 * coordinates. The tick calls this on release and tests call it
 * directly. */
void InGame_WorldDrag(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int shift_held);

/* Test seam: a control group key, assign = ctrl held. Runs the same
 * dispatch the keyboard reaches. */
void InGame_DebugControlGroup(int digit, int assign);

#endif /* TAK_INGAME_H */
