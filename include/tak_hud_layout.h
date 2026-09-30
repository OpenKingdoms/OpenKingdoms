/*
 * tak_hud_layout.h -- where the battle screen's regions go at a given
 * screen size.
 *
 * The in-game dialog is authored for 640x480. The Fit scale keeps it at
 * that size and stretches it over the window. The Original scale draws
 * one game pixel to one screen pixel at the window's own size and moves
 * the dialog the way the original does at a larger resolution: the
 * sidebar panel stands in the bottom right corner, the minimap stays at
 * the top of its column, and the bottom strip runs from the left edge to
 * the sidebar with its unit panels centred in it.
 *
 * Pure arithmetic with no SDL in it, so the browser build runs the same
 * check the desktop does (src/ui/test_hud_layout.c).
 */
#ifndef TAK_HUD_LAYOUT_H
#define TAK_HUD_LAYOUT_H

typedef struct { int x, y, w, h; } HUD_Rect;

/* Fit is zero so a platform set up by hand, as the tests do, keeps the
 * 640x480 canvas. TAK_DisplayConfig_Default picks Original. */
typedef enum {
    HUD_SCALE_FIT      = 0,   /* the 640x480 dialog stretched over the window */
    HUD_SCALE_ORIGINAL = 1,   /* one game pixel to one screen pixel */
} HUD_ScaleMode;

/* The dialog's own rects, as the .gui authors them at 640x480. */
typedef struct {
    HUD_Rect unit_menu;    /* the sidebar panel, UnitMenu */
    HUD_Rect bottom_bar;   /* the bottom strip, BottomBar */
    HUD_Rect info_group;   /* both unit panels, UnitInfoGroup */
    int      has_info_group;
} HUD_LayoutSource;

typedef struct {
    int      canvas_w, canvas_h;
    HUD_Rect play;         /* the world view */
    HUD_Rect minimap;
    HUD_Rect sidebar;
    HUD_Rect bottom;
    int      side_dx, side_dy;   /* moves everything in the sidebar */
    int      bottom_dy;          /* moves everything in the strip */
    int      info_dx;            /* moves the unit panels along the strip */
} HUD_Layout;

#define HUD_AUTHORED_W 640
#define HUD_AUTHORED_H 480

/* The rects every side's in-game dialog carries. */
void HUD_LayoutSourceDefault(HUD_LayoutSource *src);

/* The canvas the battle draws its dialog into: the window itself under
 * Original once it is at least 640x480, the smallest screen the
 * original offers, and 640x480 otherwise. */
void HUD_LayoutCanvasSize(HUD_ScaleMode mode, int win_w, int win_h,
                          int *out_w, int *out_h);

/* The regions at a canvas size. */
void HUD_LayoutCompute(const HUD_LayoutSource *src, int canvas_w, int canvas_h,
                       HUD_Layout *out);

/* Where one widget authored at `r` lands. */
HUD_Rect HUD_LayoutPlace(const HUD_LayoutSource *src, const HUD_Layout *lay,
                         HUD_Rect r);

/* The scale setting's words, "original" and "fit". Anything else reads
 * as the default, Original. */
HUD_ScaleMode HUD_ScaleModeFromName(const char *name);
const char   *HUD_ScaleModeName(HUD_ScaleMode mode);

/* The scale to start under: the saved one, else Fit for a player whose
 * options file predates the setting and Original for a new install. */
HUD_ScaleMode HUD_ScaleModeForSettings(const char *saved, int had_options_file);

#endif
