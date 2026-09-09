#ifndef TAK_GUI_RENDER_H
#define TAK_GUI_RENDER_H

#include "tak_gui.h"
#include "tak_font.h"
#include "tak_platform.h"

/*
 * Generic renderer for a GUIDialog. Takes a parsed .gui dialog plus a
 * small runtime-state struct and handles hover detection, click routing,
 * button/label/checkbox rendering, and tooltip display. Screens layer
 * game-specific behavior on top via the name-keyed widget lookup.
 */

typedef struct GUIRuntime GUIRuntime;

/* Initialize: decode every frame sprite referenced by the dialog's
 * widgets, load fonts, ready for per-frame Update/Render. */
GUIRuntime *GUIRuntime_Create(GUIDialog *dialog);
void        GUIRuntime_Destroy(GUIRuntime *rt);

/* Per-frame: update hover state and (if a button was clicked this frame)
 * write its widget name into `out_clicked`. Returns 1 if a click occurred.
 * `mx, my` are mouse coords in dialog space (0..640, 0..480). */
int  GUIRuntime_Update(GUIRuntime *rt, int mx, int my, int mouse_down,
                       char *out_clicked, size_t out_clicked_cap);

/* Same as Update but also returns the clicked widget's child index
 * (into the dialog's children array) via out_index when the return
 * value is 1. Use this when multiple widgets share a name (e.g. the
 * 8 "PlayerName" rows in the skirmish lobby). out_index is set to -1
 * when no click occurred. */
int  GUIRuntime_UpdateEx(GUIRuntime *rt, int mx, int my, int mouse_down,
                         char *out_clicked, size_t out_clicked_cap,
                         int *out_index);

/* Render the dialog (background + all children) into the shared UI
 * offscreen surface. Override individual widget states via
 * GUIRuntime_GetWidget to override frame index, disable, etc. */
void GUIRuntime_Render(GUIRuntime *rt);

/* Hover state helpers */
int  GUIRuntime_HoveredIndex(const GUIRuntime *rt);
const GUIWidget *GUIRuntime_HoveredWidget(const GUIRuntime *rt);

/* Widget accessors — returns the nth child widget from the underlying
 * dialog. Useful for screen-specific overlays (tooltip lookup, etc.). */
const GUIWidget *GUIRuntime_WidgetByName(GUIRuntime *rt, const char *name);
int              GUIRuntime_NumWidgets(const GUIRuntime *rt);

/* Set a per-widget override frame (e.g. "display the checkbox's 'on'
 * frame regardless of hover"). frame_index of -1 clears the override. */
void GUIRuntime_SetFrameOverride(GUIRuntime *rt, const char *name, int frame_index);

/* Shift every rendered widget + hit-test by (dx, dy). Used when a
 * dialog is embedded inside another (e.g. Options tabs): its child
 * widgets use their own parent-relative coordinate system, so the
 * caller sets an offset equal to the outer dialog's root-rect origin. */
void GUIRuntime_SetOffset(GUIRuntime *rt, int dx, int dy);

/* Suppress the root-widget background draw. The in-game HUD dialog's
 * root references gui.gaf:DefaultPanel (256x256) which would otherwise
 * cover the world view in the top-left corner. Menu screens want the
 * root drawn; the in-game HUD doesn't. */
void GUIRuntime_HideRoot(GUIRuntime *rt);

/* Override the display text of a widget (LABELs and any text-bearing
 * widget). Pass NULL or "" to suppress the label entirely. Used by the
 * in-game HUD to clear placeholder strings ("UnitText", "ActionText",
 * "HelpText", "+0000", "-0000") that would otherwise render as literal
 * placeholder labels until the engine binds dynamic content. */
void GUIRuntime_SetWidgetText(GUIRuntime *rt, const char *name, const char *text);

/* Show/hide a widget by name. A hidden widget is skipped during render
 * AND ignored by hover/hit-test. Used by the in-game HUD to hide
 * action buttons the selected unit lacks the cap for (e.g. a building
 * shouldn't display MOVE/PATROL/STOP). */
void GUIRuntime_SetWidgetVisible(GUIRuntime *rt, const char *name, int visible);
/* 1 when the named widget is currently hidden (0 if absent). */
int  GUIRuntime_WidgetHidden(const GUIRuntime *rt, const char *name);

#endif /* TAK_GUI_RENDER_H */
