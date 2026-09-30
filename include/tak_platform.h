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
    /* The SDL render driver to ask for, "opengl" by default on the
     * desktop so the 3D view can share the context. NULL leaves the
     * choice to SDL. Ignored when use_sw_renderer is set. */
    const char *renderer_name;
    /* HUD_ScaleMode: Original draws a battle one game pixel to one
     * screen pixel at the window's size, Fit stretches the 640x480
     * dialog over the window. Original is the default. */
    int scale_mode;
    /* In a browser, screen pixels to a game pixel under Original: 0
     * picks the whole part of the page's devicePixelRatio. */
    int pixel_size;
} TAK_DisplayConfig;

/* Sensible defaults — modern resolution, bilinear when not integer-fit. */
TAK_DisplayConfig TAK_DisplayConfig_Default(void);

typedef struct TAK_Platform {
    SDL_Window    *window;
    SDL_Renderer  *renderer;      /* hardware or software per cfg          */
    /* Which renderer this is, counting from one. A destroyed renderer
     * frees an address the next one can land on, so a cached texture
     * cannot be matched to its renderer by pointer alone. Zero means
     * nobody claimed a generation, and a cache must then keep nothing. */
    uint32_t       renderer_gen;

    /* Keys pressed since the last pump, whether or not they are still
     * down. A press that went down and up between two frames is in
     * here and nowhere else. Cleared by the next pump. */
    uint8_t  pressed_enter;
    uint8_t  pressed_escape;
    uint8_t  pressed_backspace;
    /* The left mouse button went down since the last pump, and where,
     * in window pixels. A click that was over before the frame saw it
     * is in here and nowhere else. */
    uint8_t  pressed_mouse_left;
    int      press_x, press_y;
    /* Mouse wheel clicks since the last pump, up positive. */
    int      wheel_dy;
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

    /* Printable characters typed since the last pump, in order. The
     * chat console is the only reader: SDL delivers a composed
     * character as its own event, which is the only way to tell 'a'
     * from Shift+a or to type anything off a non US layout. Cleared at
     * the top of every pump, and empty unless something has called
     * SDL_StartTextInput. Filtered to 32 through 126, the glyphs the
     * fonts carry. */
    char           text_in[64];
    int            text_in_len;

    int            fullscreen;
    int            has_focus;
    int            use_sw_renderer;
    int            pixel_perfect;
    int            vsync;
    int            scale_mode;    /* HUD_ScaleMode                         */
    int            pixel_size;    /* browser only, 0 = from the page       */
    /* On Windows the scale the process started under, which fixed its
     * DPI awareness for the run. -1 elsewhere. */
    int            started_scale;
} TAK_Platform;

/* A size the battle can run at under Original. pixel_size is the screen
 * pixels to a game pixel in a browser, 0 on the desktop. */
typedef struct TAK_Resolution {
    int w, h;
    int pixel_size;
} TAK_Resolution;

/* Initialise the platform from a display config. Creates the window +
 * renderer + canvas texture. Returns 0 on success, -1 on failure. */
int  TAK_Platform_Init(TAK_Platform *plat, const TAK_DisplayConfig *cfg);

/* The next renderer generation, never zero. Whoever calls
 * SDL_CreateRenderer stores this in renderer_gen on the line after,
 * tests included, or every texture cache re-uploads on every frame. */
uint32_t TAK_Platform_NewRendererGen(void);

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
/* The left button as a screen should read it this frame: held, or
 * pressed since the last pump even if already let go. When it was
 * pressed and let go inside one frame the position is where the press
 * landed, not where the pointer is now. Returns whether it is down,
 * and fills the canvas position, -1 when off the canvas. */
int TAK_Platform_MouseThisFrame(const TAK_Platform *plat, int *out_cx, int *out_cy);

int  TAK_Platform_MapMouseToCanvas(const TAK_Platform *plat,
                                    int window_x, int window_y,
                                    int *out_canvas_x, int *out_canvas_y);

/* Map a canvas-space rect to window pixels, the exact inverse of
 * TAK_Platform_MapMouseToCanvas, so anything drawn straight to the
 * renderer lands on the canvas pixels it belongs to. Every HUD overlay,
 * the minimap slot and the world viewport clip go through this. */
SDL_Rect TAK_Platform_CanvasRectToWindow(const TAK_Platform *plat,
                                          SDL_Rect canvas_rect);

/* Resize the canvas texture. UI_SetCanvasSize calls this with the
 * compositing surface, so the two never disagree within a frame. */
void TAK_Platform_SetCanvasSize(TAK_Platform *plat, int w, int h);

/* Switch between the Original and Fit scales. In a browser this sizes
 * the page's canvas to match: under Original the drawing buffer is the
 * page's size in screen pixels over pixel_size, under Fit it is the
 * page's 16:9 box. */
void TAK_Platform_SetScaleMode(TAK_Platform *plat, int scale_mode,
                               int pixel_size);

/* The canvas a battle draws into: the window under Original once it is
 * 640x480 or more, 640x480 otherwise. */
void TAK_Platform_BattleCanvasSize(const TAK_Platform *plat, int *w, int *h);

/* Screen pixels to a game pixel as it stands: 1 on the desktop, and in
 * a browser the setting or the whole part of devicePixelRatio. */
int TAK_Platform_PixelSize(const TAK_Platform *plat);

/* The sizes Original can run at here, smallest first, at most cap. On
 * the desktop: the display's modes from 640x480 up that fit the screen,
 * plus the window as it is. In a browser: the page at each pixel size
 * that leaves at least 640x480. */
int TAK_Platform_Resolutions(const TAK_Platform *plat, TAK_Resolution *out,
                             int cap);

/* Set a windowed desktop window to w x h pixels, centred on its own
 * display. In fullscreen, the desktop's size keeps the desktop and any
 * other size changes the display mode. Returns -1 in a browser, where
 * the page decides the size, or when the mode will not take. */
int TAK_Platform_SetWindowSize(TAK_Platform *plat, int w, int h);

#endif /* TAK_PLATFORM_H */
