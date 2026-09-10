#ifndef TAK_PLATFORM_H
#define TAK_PLATFORM_H

#include <SDL.h>
#include <stdint.h>

/* Display/window configuration. Callers assemble this (from defaults,
 * CLI args, and eventually a config file) and hand it to TAK_Platform_Init.
 *
 * canvas_w/h are the fixed virtual UI canvas size — all GUI widget rects
 * in the original game's .gui files are absolute pixel coords against
 * 640×480, so we keep that as the compositing space and letterbox it
 * into whatever window the user picks. Don't change canvas_w/h unless
 * you know you're retargeting the UI. */
typedef struct TAK_DisplayConfig {
    int window_w;          /* default: 1280                         */
    int window_h;          /* default: 720                          */
    int canvas_w;          /* default: 640 (virtual UI canvas)      */
    int canvas_h;          /* default: 480                          */
    int fullscreen;        /* 0 = windowed, 1 = fullscreen-desktop  */
    int vsync;             /* 1 = enable, 0 = disable               */
    int pixel_perfect;     /* 1 = integer scale when it fits,
                            * 0 = bilinear at any size              */
    int use_sw_renderer;   /* 1 = force SDL_RENDERER_SOFTWARE       */
} TAK_DisplayConfig;

/* Sensible defaults — modern resolution, bilinear when not integer-fit. */
TAK_DisplayConfig TAK_DisplayConfig_Default(void);

typedef struct TAK_Platform {
    SDL_Window    *window;
    SDL_Renderer  *renderer;      /* hardware or software per cfg          */
    SDL_Texture   *canvas_tex;    /* streaming, canvas_w × canvas_h, RGBA  */

    int            window_w;      /* current window size (updates on resize) */
    int            window_h;
    int            canvas_w;      /* virtual UI canvas (fixed)             */
    int            canvas_h;

    /* Letterbox transform window -> canvas, recomputed on every resize.
     * scale maps canvas units to window units (scale >= 1 when the
     * window fits the canvas; < 1 when window is smaller). offset_x/y
     * is the top-left corner of the scaled canvas within the window. */
    float          scale;
    int            offset_x;
    int            offset_y;

    int            fullscreen;
    int            has_focus;
    int            use_sw_renderer;
    int            pixel_perfect;
    int            vsync;
} TAK_Platform;

/* Initialise the platform from a display config. Creates the window +
 * renderer + canvas texture. Returns 0 on success, -1 on failure. */
int  TAK_Platform_Init(TAK_Platform *plat, const TAK_DisplayConfig *cfg);

void TAK_Platform_FrameBegin(TAK_Platform *plat);

/* Destroy the window, renderer, and canvas texture; quits SDL. */
void TAK_Platform_Shutdown(TAK_Platform *plat);

/* Pump the SDL event queue. Returns 1 to keep running, 0 on quit.
 * Handles resize events internally — updates window_w/h and the
 * scale/offset transform. */
int  TAK_Platform_PumpEvents(TAK_Platform *plat);

/* Toggle fullscreen (Alt+Enter hook). */
void TAK_Platform_ToggleFullscreen(TAK_Platform *plat);

/* Upload the current UI canvas contents into the canvas texture. Called
 * by UI_Present after each screen finishes compositing. canvas must be
 * a canvas_w × canvas_h SDL_PIXELFORMAT_RGBA32 surface. */
void TAK_Platform_UpdateCanvas(TAK_Platform *plat, SDL_Surface *canvas);

/* Clear the window, draw the canvas texture letterboxed to the current
 * transform, and present. Called once at the end of each frame by main. */
void TAK_Platform_Present(TAK_Platform *plat);

/* Map a window-space mouse coord (e.g. from SDL_GetMouseState) back to
 * canvas coords. If the point lies in the letterbox bars, returns 0 and
 * leaves *out_cx/cy untouched; otherwise returns 1 and writes clamped
 * canvas coords. UI code should use this before doing any hit-testing. */
int  TAK_Platform_MapMouseToCanvas(const TAK_Platform *plat,
                                    int window_x, int window_y,
                                    int *out_canvas_x, int *out_canvas_y);

/* Map a canvas-space rect to window pixels, the exact inverse of
 * TAK_Platform_MapMouseToCanvas, so anything drawn straight to the
 * renderer lands on the canvas pixels it belongs to. Every HUD overlay,
 * the minimap slot and the world viewport clip go through this. */
SDL_Rect TAK_Platform_CanvasRectToWindow(const TAK_Platform *plat,
                                          SDL_Rect canvas_rect);

#endif /* TAK_PLATFORM_H */
