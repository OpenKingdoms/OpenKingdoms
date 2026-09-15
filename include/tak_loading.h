#ifndef TAK_LOADING_H
#define TAK_LOADING_H

#include "tak_platform.h"

/* Loading screen (GAMESTATE_GAME_LOADING).
 *
 * Shows loadscreen.gui (the parchment + banner) with a progress bar and
 * status text. The engine lane pumps progress via Loading_SetProgress
 * as it loads maps/units/etc.; when progress hits 1.0 and a full frame
 * has rendered, Loading_Tick returns GAMESTATE_IN_GAME.
 */

int  Loading_Init(TAK_Platform *platform);
int  Loading_Tick(TAK_Platform *platform, float frame_dt);
void Loading_Shutdown(void);

/* The save this load is coming out of. The loading screen builds the
 * world the normal way and then applies it, so a battle brought up
 * from a file lands on the same ground with the same settings the
 * player left it on. Ownership passes to the loading screen, which
 * closes it when the last phase is done. Cleared by Shutdown, so a
 * load that is abandoned does not leak the reader.
 *
 * A refusal that only shows up once the definitions are in memory
 * (a unit that changed under the save) is reported through
 * Loading_SaveRefusal, which is empty when nothing refused. */
struct TAK_SaveGame;
void Loading_SetPendingSave(struct TAK_SaveGame *sg);
const char *Loading_SaveRefusal(void);

/* 1 while a save is waiting to be applied. Whoever brings the world up
 * asks this, because a world being restored from a file must not also
 * be given a freshly spawned army, and the switch that stops that has
 * to be thrown after World_BeginLoad rather than before it:
 * World_BeginLoad clears it, which is what keeps a leaked flag from
 * ever reaching the next battle. */
int Loading_HasPendingSave(void);

/* 0.0 .. 1.0 — clamped on call. */
void Loading_SetProgress(float fraction);
/* Where the bar is, 0 to 1. */
float Loading_Progress(void);
void Loading_SetStatus(const char *status_line);  /* e.g. "Loading units..." */

/* The screen's live loadscreen.gui runtime, NULL outside Init/Shutdown.
 * Callers read the backdrop the dialog resolved through it. */
struct GUIRuntime *Loading_Runtime(void);

/* Test hook: the frame the arch's clip is on and how many it has.
 * Returns 0 when the screen has no clip. */
int Loading_DebugClip(int *frame, int *count);

#endif
