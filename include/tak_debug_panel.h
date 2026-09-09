#ifndef TAK_DEBUG_PANEL_H
#define TAK_DEBUG_PANEL_H

/*
 * Debug overlay panel — draggable in-game inspector + tweaker.
 *
 * Scope: a self-contained immediate-mode UI that draws onto the
 * 640×480 UI canvas. Lives over the 3D scene (the panel's pixels
 * land on top of terrain + units because UI_Present composites the
 * canvas last each frame). Compiled in only when TAK_DEBUG=1.
 *
 * Designed to grow. M5 wires in TA_SCALE / TAN_TILT tuners and
 * spawn buttons; later milestones will add lighting toggles, AABB
 * overlays, texture-atlas viewer, etc.
 *
 * Usage from a screen module (e.g. ingame.c):
 *
 *   InGame_Init:     DebugPanel_Init(plat);
 *   InGame_Tick:     int consumed = DebugPanel_HandleInput(plat,
 *                                       wx, wy, left_button_down);
 *                    // ...skip other mouse logic if consumed...
 *                    DebugPanel_TickFPS(frame_dt);
 *                    DebugPanel_Draw();
 *   InGame_Shutdown: DebugPanel_Shutdown();
 *
 * Release builds (TAK_DEBUG undefined): all entry points are inline
 * no-ops. Calls remain in the source tree without #ifdefs at every
 * call site. */

#include "tak_platform.h"

#ifdef TAK_DEBUG

int  DebugPanel_Init(TAK_Platform *plat);
void DebugPanel_Shutdown(void);

/* Process mouse input. Returns 1 if the panel consumed the click
 * this frame (caller should skip its own mouse-driven actions). */
int  DebugPanel_HandleInput(TAK_Platform *plat,
                             int window_mouse_x, int window_mouse_y,
                             int left_button_down);

/* Smooths frame-time into a displayable FPS reading. Call once per
 * tick with the frame's delta-time. */
void DebugPanel_TickFPS(float frame_dt);

/* Draw the panel into the UI canvas (UI_Offscreen). Call after
 * Terrain/Units/Minimap have submitted their geometry but before
 * UI_Present. */
void DebugPanel_Draw(void);

#else  /* TAK_DEBUG */

static inline int  DebugPanel_Init(TAK_Platform *plat)      { (void)plat; return 0; }
static inline void DebugPanel_Shutdown(void)                {}
static inline int  DebugPanel_HandleInput(TAK_Platform *plat,
                                           int wx, int wy, int down) {
    (void)plat; (void)wx; (void)wy; (void)down; return 0;
}
static inline void DebugPanel_TickFPS(float dt)             { (void)dt; }
static inline void DebugPanel_Draw(void)                    {}

#endif /* TAK_DEBUG */

#endif /* TAK_DEBUG_PANEL_H */
