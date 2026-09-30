/*
 * sdl2_platform.c -- SDL2 platform layer.
 *
 * Owns the window, the SDL_Renderer, and the canvas streaming texture.
 * The rest of the game composites into a fixed-size SDL_Surface (the UI
 * canvas, owned by render/ui.c) which we upload every frame and scale
 * into the window with aspect-preserving letterbox.
 */

#include "tak_platform.h"
#include "tak_click_map.h"
#include "tak_hud_layout.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

TAK_DisplayConfig TAK_DisplayConfig_Default(void) {
    TAK_DisplayConfig c;
    c.window_w        = 1280;
    c.window_h        = 720;
    c.canvas_w        = 640;
    c.canvas_h        = 480;
    c.fullscreen      = 0;
    c.vsync           = 1;
    /* Off by default: users expect content to grow continuously as they
     * drag the window edge. Pixel-perfect mode locks to integer steps,
     * which leaves large black bars at "in between" window sizes (e.g.
     * 1280×720 shows the canvas at 1× because 2× would overflow the
     * 720px height). Pass --pixel-perfect to opt in. */
    c.pixel_perfect   = 0;
    c.use_sw_renderer = 0;
    /* The 3D view needs the GL driver and only GL ES 2.0 level of it,
     * so GL is the default on Windows and Linux and V works without a
     * flag. macOS keeps SDL's pick, Metal, because GL is deprecated
     * there. --renderer overrides either way. */
#if defined(__EMSCRIPTEN__) || defined(__APPLE__)
    c.renderer_name   = NULL;
#else
    c.renderer_name   = "opengl";
#endif
    c.scale_mode      = HUD_SCALE_ORIGINAL;
    c.pixel_size      = 0;
    return c;
}

/* Nearest when a canvas pixel is a whole number of window pixels (the
 * Original battle, or pixel-perfect at an integer scale), linear for
 * the fractional stretch Fit gives. */
static void apply_canvas_filter(TAK_Platform *plat) {
    if (!plat->canvas_tex) return;
    int same = plat->canvas_w == plat->window_w && plat->canvas_h == plat->window_h;
    float s = plat->scale;
    int integer = plat->pixel_perfect && s >= 1.0f && s == (float)(int)s;
    SDL_SetTextureScaleMode(plat->canvas_tex,
        (same || integer) ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
}

static void recompute_layout(TAK_Platform *plat);

#ifdef __EMSCRIPTEN__
/* The page's size in game pixels at k screen pixels each. */
EM_JS(int, web_page_px, (int k, int height), {
    var dpr = window.devicePixelRatio || 1;
    var v = height ? window.innerHeight : window.innerWidth;
    return Math.floor(v * dpr / k);
});

EM_JS(double, web_device_pixel_ratio, (void), {
    return window.devicePixelRatio || 1;
});

/* Under Original the canvas is sized in CSS so each game pixel covers
 * exactly k screen pixels, and drawn without smoothing. Under Fit the
 * sheet's 16:9 box takes over again. */
EM_JS(void, web_canvas_css, (int original, int gw, int gh, int k), {
    var c = Module['canvas'];
    if (!c) return;
    var dpr = window.devicePixelRatio || 1;
    if (original) {
        c.classList.add('px');
        c.style.setProperty('width', (gw * k / dpr) + 'px', 'important');
        c.style.setProperty('height', (gh * k / dpr) + 'px', 'important');
    } else {
        c.classList.remove('px');
        c.style.removeProperty('width');
        c.style.removeProperty('height');
    }
});

/* The nearest whole number of screen pixels to a CSS pixel, so a game
 * pixel is about a CSS pixel on any display, the HUD keeps its size
 * and the buffer is never bigger than the screen. */
static int web_pixel_size(const TAK_Platform *plat) {
    if (plat->pixel_size > 0) return plat->pixel_size;
    int k = (int)floor(web_device_pixel_ratio() + 0.5);
    return k < 1 ? 1 : k;
}

/* What the canvas was last sized for, so a frame can tell the page
 * moved under it. SDL drops the resize the browser reports when the
 * box it reads back is the one it already had, which is what a box
 * sized in CSS pixels gives at a whole ratio. */
static int    g_web_gw, g_web_gh;
static double g_web_dpr;

/* The drawing buffer is the window as SDL sees it, and SDL scales the
 * pointer from the canvas's CSS box to that, so a buffer in screen
 * pixels gives a pointer in screen pixels too. */
static void web_size_canvas(TAK_Platform *plat) {
    if (!plat->window) return;
    if (plat->scale_mode == HUD_SCALE_ORIGINAL) {
        int k = web_pixel_size(plat);
        int gw = web_page_px(k, 0), gh = web_page_px(k, 1);
        if (gw < 1 || gh < 1) return;
        web_canvas_css(1, gw, gh, k);
        SDL_SetWindowSize(plat->window, gw, gh);
        g_web_gw = gw;
        g_web_gh = gh;
        g_web_dpr = web_device_pixel_ratio();
    } else {
        web_canvas_css(0, 0, 0, 1);
        double cw = 0, ch = 0;
        if (emscripten_get_element_css_size("#canvas", &cw, &ch) == EMSCRIPTEN_RESULT_SUCCESS &&
            cw >= 1 && ch >= 1)
            SDL_SetWindowSize(plat->window, (int)cw, (int)ch);
    }
    SDL_GetWindowSize(plat->window, &plat->window_w, &plat->window_h);
    recompute_layout(plat);
    apply_canvas_filter(plat);
}
#endif

/* Recompute scale + letterbox offsets from current window/canvas sizes.
 * Pixel-perfect mode picks the largest integer scale that fits; if no
 * integer scale fits (window smaller than canvas on either axis) or the
 * user disabled it, fall back to a fractional fit. */
static void recompute_layout(TAK_Platform *plat) {
    if (plat->canvas_w <= 0 || plat->canvas_h <= 0) return;

    float sx = (float)plat->window_w / (float)plat->canvas_w;
    float sy = (float)plat->window_h / (float)plat->canvas_h;
    float fit = sx < sy ? sx : sy;
    if (fit <= 0.f) fit = 1.f;

    float scale = fit;
    if (plat->pixel_perfect) {
        int isc = (int)fit;
        if (isc >= 1) scale = (float)isc;
    }

    int scaled_w = (int)(plat->canvas_w * scale + 0.5f);
    int scaled_h = (int)(plat->canvas_h * scale + 0.5f);

    plat->scale    = scale;
    plat->offset_x = (plat->window_w - scaled_w) / 2;
    plat->offset_y = (plat->window_h - scaled_h) / 2;
}

/* Counted, never zero, and never reused inside a run, so a texture
 * cache can tell one renderer from the next even when the allocator
 * hands out the same address twice. */
uint32_t TAK_Platform_NewRendererGen(void) {
    static uint32_t next = 1;
    return next++;
}

int TAK_Platform_Init(TAK_Platform *plat, const TAK_DisplayConfig *cfg) {
    if (!plat || !cfg) {
        fprintf(stderr, "TAK_Platform_Init: null args\n");
        return -1;
    }
    memset(plat, 0, sizeof(*plat));

#ifdef _WIN32
    /* Under Original a game pixel is a screen pixel, so a scaled display
     * must not stretch the window behind SDL's back. It can only be set
     * before the first window, so it follows the scale at start. */
    if (cfg->scale_mode == HUD_SCALE_ORIGINAL)
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    plat->started_scale = cfg->scale_mode;
#else
    plat->started_scale = -1;
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return -1;
    }

    Uint32 win_flags = SDL_WINDOW_RESIZABLE;
    if (cfg->fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    /* The GL render driver shares its context with the 3D view, which
     * needs a depth buffer on the window. SDL would recreate the window
     * for the driver anyway, so ask for it up front. */
    const char *driver = cfg->use_sw_renderer ? NULL : cfg->renderer_name;
    if (driver && strncmp(driver, "opengl", 6) == 0) {
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        win_flags |= SDL_WINDOW_OPENGL;
    }
#ifdef __EMSCRIPTEN__
    /* The browser's renderer is WebGL whatever it is called, and the
     * 3D view depth tests, so the context is asked for a depth buffer
     * here where no driver name reaches the branch above. */
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#endif

    plat->window = SDL_CreateWindow("Total Annihilation: Kingdoms",
                                     SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED,
                                     cfg->window_w, cfg->window_h,
                                     win_flags);
    if (!plat->window && (win_flags & SDL_WINDOW_OPENGL)) {
        /* No GL here. The game still runs, without the 3D view. */
        fprintf(stderr, "No OpenGL window (%s), starting without the 3D view\n",
                SDL_GetError());
        driver = NULL;
        win_flags &= ~(Uint32)SDL_WINDOW_OPENGL;
        plat->window = SDL_CreateWindow("Total Annihilation: Kingdoms",
                                         SDL_WINDOWPOS_CENTERED,
                                         SDL_WINDOWPOS_CENTERED,
                                         cfg->window_w, cfg->window_h,
                                         win_flags);
    }
    if (!plat->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    Uint32 rend_flags = cfg->use_sw_renderer
        ? SDL_RENDERER_SOFTWARE
        : SDL_RENDERER_ACCELERATED;
    if (cfg->vsync) rend_flags |= SDL_RENDERER_PRESENTVSYNC;
#ifdef __EMSCRIPTEN__
    /* The 3D view draws inside a render target SDL owns, and SDL only
     * allows one on a renderer created able to take it. */
    rend_flags |= SDL_RENDERER_TARGETTEXTURE;
#endif

    /* Naming a driver turns SDL's draw batching off unless it is asked
     * for by name too. The 3D view flushes the queue before its own
     * drawing, so batching stays on. */
    if (driver) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, driver);
        SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    }
    plat->renderer = SDL_CreateRenderer(plat->window, -1, rend_flags);
    if (!plat->renderer && driver) {
        /* No such driver here: let SDL choose, without the 3D view. */
        fprintf(stderr, "Renderer %s unavailable (%s), falling back\n",
                driver, SDL_GetError());
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");
        plat->renderer = SDL_CreateRenderer(plat->window, -1, rend_flags);
    }
    if (!plat->renderer) {
        /* Retry without vsync, then without hardware, before giving up. */
        rend_flags &= ~SDL_RENDERER_PRESENTVSYNC;
        plat->renderer = SDL_CreateRenderer(plat->window, -1, rend_flags);
    }
    if (!plat->renderer) {
        plat->renderer = SDL_CreateRenderer(plat->window, -1,
                                             SDL_RENDERER_SOFTWARE);
    }
    if (!plat->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(plat->window);
        SDL_Quit();
        return -1;
    }
    plat->renderer_gen = TAK_Platform_NewRendererGen();

    /* Log the backend we ended up with so it's visible in runs. */
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(plat->renderer, &info) == 0) {
        fprintf(stderr, "Renderer: %s (flags=0x%08x)\n",
                info.name ? info.name : "?", info.flags);
    }

    plat->canvas_tex = SDL_CreateTexture(plat->renderer,
                                          SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          cfg->canvas_w, cfg->canvas_h);
    if (!plat->canvas_tex) {
        fprintf(stderr, "SDL_CreateTexture(canvas) failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(plat->renderer);
        SDL_DestroyWindow(plat->window);
        SDL_Quit();
        return -1;
    }
    /* Linear filtering by default so the canvas looks clean at
     * fractional scales; switch to nearest when pixel_perfect is on and
     * the current scale is an exact integer (see recompute_layout). */
    SDL_SetTextureScaleMode(plat->canvas_tex,
        cfg->pixel_perfect ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    /* Alpha-compose the canvas on top of whatever the game drew to the
     * window this frame (e.g. terrain). UI screens that want an opaque
     * backdrop fill their canvas with alpha=255; screens that want
     * underlying draws to show through (in-game) fill with alpha=0. */
    SDL_SetTextureBlendMode(plat->canvas_tex, SDL_BLENDMODE_BLEND);

    plat->window_w        = cfg->window_w;
    plat->window_h        = cfg->window_h;
    plat->canvas_w        = cfg->canvas_w;
    plat->canvas_h        = cfg->canvas_h;
    plat->fullscreen      = cfg->fullscreen;
    plat->has_focus       = 1;
    plat->use_sw_renderer = cfg->use_sw_renderer;
    plat->pixel_perfect   = cfg->pixel_perfect;
    plat->vsync           = cfg->vsync;
    plat->scale_mode      = cfg->scale_mode;
    plat->pixel_size      = cfg->pixel_size;

    /* Pull the real window size — fullscreen-desktop or HiDPI may have
     * given us something other than what we requested. */
    SDL_GetWindowSize(plat->window, &plat->window_w, &plat->window_h);
    recompute_layout(plat);
#ifdef __EMSCRIPTEN__
    web_size_canvas(plat);
#endif
    apply_canvas_filter(plat);

    /* SDL turns text input on with the window on some platforms. Start
     * it off, so a screen that wants typed characters asks for them and
     * nothing else collects any. */
    SDL_StopTextInput();
    plat->text_in[0] = '\0';
    plat->text_in_len = 0;

    return 0;
}

void TAK_Platform_FrameBegin(TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;

    SDL_SetRenderDrawColor(plat->renderer, 0, 0, 0, 255);
    SDL_RenderClear(plat->renderer);
}

void TAK_Platform_Shutdown(TAK_Platform *plat) {
    if (!plat) return;
    if (plat->canvas_tex) SDL_DestroyTexture(plat->canvas_tex);
    if (plat->renderer)   SDL_DestroyRenderer(plat->renderer);
    if (plat->window)     SDL_DestroyWindow(plat->window);
    memset(plat, 0, sizeof(*plat));
    SDL_Quit();
}

int TAK_Platform_PumpEvents(TAK_Platform *plat) {
    if (!plat) return 0;
#ifdef __EMSCRIPTEN__
    /* The page's size and ratio, checked every frame under Original:
     * a resize, zoom, fullscreen or a move to another screen. */
    if (plat->scale_mode == HUD_SCALE_ORIGINAL && plat->window) {
        int k = web_pixel_size(plat);
        if (web_page_px(k, 0) != g_web_gw || web_page_px(k, 1) != g_web_gh ||
            web_device_pixel_ratio() != g_web_dpr)
            web_size_canvas(plat);
    }
#endif
    /* A frame's typing starts empty. Whatever no screen reads is gone
     * by the next pump, which is what keeps a key held through a mode
     * change from arriving somewhere it does not belong. */
    plat->text_in[0] = '\0';
    plat->text_in_len = 0;
    plat->pressed_enter = 0;
    plat->pressed_escape = 0;
    plat->pressed_backspace = 0;
    plat->pressed_mouse_left = 0;
    plat->wheel_dy = 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            return 0;

        case SDL_TEXTINPUT:
            /* SDL only sends these between SDL_StartTextInput and
             * SDL_StopTextInput, so nothing is collected while the chat
             * console is shut. */
            for (const char *p = ev.text.text; *p; p++) {
                unsigned char c = (unsigned char)*p;
                if (c < 0x20 || c > 0x7e) continue;
                if (plat->text_in_len >= (int)sizeof(plat->text_in) - 1) break;
                plat->text_in[plat->text_in_len++] = (char)c;
            }
            plat->text_in[plat->text_in_len] = '\0';
            break;

        case SDL_KEYDOWN:
            /* Alt+Enter toggles fullscreen. Escape is handled per-screen. */
            if (ev.key.keysym.sym == SDLK_RETURN &&
                (ev.key.keysym.mod & KMOD_ALT)) {
                TAK_Platform_ToggleFullscreen(plat);
                break;
            }
            if (ev.key.repeat) break;
            switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:  plat->pressed_enter = 1; break;
            case SDL_SCANCODE_ESCAPE:    plat->pressed_escape = 1; break;
            case SDL_SCANCODE_BACKSPACE: plat->pressed_backspace = 1; break;
            default: break;
            }
            break;

        case SDL_MOUSEWHEEL:
            plat->wheel_dy += ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                            ? -ev.wheel.y : ev.wheel.y;
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                plat->pressed_mouse_left = 1;
                plat->press_x = ev.button.x;
                plat->press_y = ev.button.y;
            }
            break;

        case SDL_WINDOWEVENT:
            switch (ev.window.event) {
            case SDL_WINDOWEVENT_FOCUS_GAINED: plat->has_focus = 1; break;
            case SDL_WINDOWEVENT_FOCUS_LOST:   plat->has_focus = 0; break;
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_RESIZED:
                plat->window_w = ev.window.data1;
                plat->window_h = ev.window.data2;
                recompute_layout(plat);
#ifdef __EMSCRIPTEN__
                /* SDL sizes the buffer to the CSS box on a page resize,
                 * which is a CSS pixel each. Original wants screen
                 * pixels, so it sizes it again. */
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED &&
                    plat->scale_mode == HUD_SCALE_ORIGINAL)
                    web_size_canvas(plat);
#endif
                apply_canvas_filter(plat);
                break;
            }
            break;
        }
    }
    return 1;
}

void TAK_Platform_ToggleFullscreen(TAK_Platform *plat) {
    if (!plat || !plat->window) return;
    plat->fullscreen = !plat->fullscreen;
    SDL_SetWindowFullscreen(plat->window,
        plat->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_GetWindowSize(plat->window, &plat->window_w, &plat->window_h);
    recompute_layout(plat);
#ifdef __EMSCRIPTEN__
    web_size_canvas(plat);
#endif
    apply_canvas_filter(plat);
}

void TAK_Platform_UpdateCanvas(TAK_Platform *plat, SDL_Surface *canvas) {
    if (!plat || !plat->canvas_tex || !canvas) return;
    if (canvas->w != plat->canvas_w || canvas->h != plat->canvas_h) {
        /* Defensive — something drifted. The UI canvas is expected to
         * stay a fixed size; if it doesn't, recreate the texture rather
         * than crash in SDL_UpdateTexture. */
        SDL_DestroyTexture(plat->canvas_tex);
        plat->canvas_tex = SDL_CreateTexture(plat->renderer,
                                              SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STREAMING,
                                              canvas->w, canvas->h);
        if (!plat->canvas_tex) return;
        SDL_SetTextureBlendMode(plat->canvas_tex, SDL_BLENDMODE_BLEND);
        plat->canvas_w = canvas->w;
        plat->canvas_h = canvas->h;
        recompute_layout(plat);
        apply_canvas_filter(plat);
    }
    SDL_LockSurface(canvas);
    SDL_UpdateTexture(plat->canvas_tex, NULL, canvas->pixels, canvas->pitch);
    SDL_UnlockSurface(canvas);
}

void TAK_Platform_SetCanvasSize(TAK_Platform *plat, int w, int h) {
    if (!plat || w <= 0 || h <= 0) return;
    if (plat->canvas_w == w && plat->canvas_h == h) return;
    if (plat->renderer) {
        SDL_Texture *t = SDL_CreateTexture(plat->renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!t) {
            fprintf(stderr, "Canvas %dx%d: %s\n", w, h, SDL_GetError());
            return;
        }
        if (plat->canvas_tex) SDL_DestroyTexture(plat->canvas_tex);
        plat->canvas_tex = t;
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    plat->canvas_w = w;
    plat->canvas_h = h;
    recompute_layout(plat);
    apply_canvas_filter(plat);
}

void TAK_Platform_SetScaleMode(TAK_Platform *plat, int scale_mode,
                               int pixel_size) {
    if (!plat) return;
    plat->scale_mode = scale_mode == HUD_SCALE_FIT ? HUD_SCALE_FIT
                                                   : HUD_SCALE_ORIGINAL;
    plat->pixel_size = pixel_size > 0 ? pixel_size : 0;
#ifdef __EMSCRIPTEN__
    web_size_canvas(plat);
#endif
    apply_canvas_filter(plat);
}

void TAK_Platform_BattleCanvasSize(const TAK_Platform *plat, int *w, int *h) {
    HUD_LayoutCanvasSize(plat ? (HUD_ScaleMode)plat->scale_mode : HUD_SCALE_FIT,
                         plat ? plat->window_w : 0, plat ? plat->window_h : 0,
                         w, h);
}

int TAK_Platform_PixelSize(const TAK_Platform *plat) {
#ifdef __EMSCRIPTEN__
    return plat ? web_pixel_size(plat) : 1;
#else
    (void)plat;
    return 1;
#endif
}

/* Kept in order, smallest width first, without repeats. */
static int add_resolution(TAK_Resolution *out, int n, int cap,
                          int w, int h, int k) {
    if (w < HUD_AUTHORED_W || h < HUD_AUTHORED_H || n >= cap) return n;
    for (int i = 0; i < n; i++)
        if (out[i].w == w && out[i].h == h) return n;
    int at = n;
    while (at > 0 && (out[at - 1].w > w || (out[at - 1].w == w && out[at - 1].h > h))) {
        out[at] = out[at - 1];
        at--;
    }
    out[at].w = w;
    out[at].h = h;
    out[at].pixel_size = k;
    return n + 1;
}

int TAK_Platform_Resolutions(const TAK_Platform *plat, TAK_Resolution *out,
                             int cap) {
    if (!plat || !out || cap <= 0) return 0;
    int n = 0;
#ifdef __EMSCRIPTEN__
    for (int k = 1; k <= 4; k++)
        n = add_resolution(out, n, cap, web_page_px(k, 0), web_page_px(k, 1), k);
#else
    /* Windowed, the modes that fit the screen size the window. In
     * fullscreen every mode is offered and a smaller one is a real mode
     * change, the way the original went bigger on a large screen. */
    if (plat->window) {
        int disp = SDL_GetWindowDisplayIndex(plat->window);
        if (disp < 0) disp = 0;
        SDL_Rect usable = { 0, 0, 0, 0 };
        int have_usable = !plat->fullscreen &&
                          SDL_GetDisplayUsableBounds(disp, &usable) == 0;
        int modes = SDL_GetNumDisplayModes(disp);
        for (int i = 0; i < modes; i++) {
            SDL_DisplayMode m;
            if (SDL_GetDisplayMode(disp, i, &m) != 0) continue;
            if (have_usable && (m.w > usable.w || m.h > usable.h)) continue;
            n = add_resolution(out, n, cap, m.w, m.h, 0);
        }
    }
    n = add_resolution(out, n, cap, plat->window_w, plat->window_h, 0);
#endif
    return n;
}

int TAK_Platform_SetWindowSize(TAK_Platform *plat, int w, int h) {
#ifdef __EMSCRIPTEN__
    (void)plat; (void)w; (void)h;
    return -1;
#else
    if (!plat || !plat->window || w <= 0 || h <= 0) return -1;
    int disp = SDL_GetWindowDisplayIndex(plat->window);
    if (disp < 0) disp = 0;
    if (plat->fullscreen) {
        SDL_DisplayMode desk, want = { 0 }, got;
        if (SDL_GetDesktopDisplayMode(disp, &desk) != 0) return -1;
        if (w == desk.w && h == desk.h) {
            if (SDL_SetWindowFullscreen(plat->window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
                return -1;
        } else {
            want.w = w;
            want.h = h;
            if (!SDL_GetClosestDisplayMode(disp, &want, &got) ||
                SDL_SetWindowDisplayMode(plat->window, &got) != 0 ||
                SDL_SetWindowFullscreen(plat->window, SDL_WINDOW_FULLSCREEN) != 0)
                return -1;
        }
    } else {
        SDL_SetWindowSize(plat->window, w, h);
        SDL_SetWindowPosition(plat->window, SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
    }
    SDL_GetWindowSize(plat->window, &plat->window_w, &plat->window_h);
    recompute_layout(plat);
    apply_canvas_filter(plat);
    return 0;
#endif
}

void TAK_Platform_Present(TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;
    if (plat->canvas_tex) {
        /* The UI canvas (640×480 native, hosting menu + HUD) is
         * stretched to fill the entire window so the in-game HUD
         * scales with the window. Letterboxing was causing the HUD
         * to render at native size with black bars at high-DPI
         * resolutions — not what we want for a modern remake.
         * Linear filtering keeps the upscale readable. */
        SDL_RenderCopy(plat->renderer, plat->canvas_tex, NULL, NULL);
    }
    SDL_RenderPresent(plat->renderer);
}

int TAK_Platform_MouseThisFrame(const TAK_Platform *plat, int *out_cx, int *out_cy) {
    int wx = 0, wy = 0;
    uint32_t held = SDL_GetMouseState(&wx, &wy) & SDL_BUTTON(SDL_BUTTON_LEFT);
    int down = held ? 1 : 0;
    if (plat && plat->pressed_mouse_left && !held) {
        /* Down and up inside one frame: the press is the event, and
         * where it landed is the position that matters. */
        down = 1;
        wx = plat->press_x;
        wy = plat->press_y;
    }
    int cx = -1, cy = -1;
    if (!TAK_Platform_MapMouseToCanvas(plat, wx, wy, &cx, &cy)) { cx = -1; cy = -1; }
    if (out_cx) *out_cx = cx;
    if (out_cy) *out_cy = cy;
    return down;
}

int TAK_Platform_MapMouseToCanvas(const TAK_Platform *plat,
                                   int wx, int wy,
                                   int *out_cx, int *out_cy) {
    if (!plat) return 0;
    /* The canvas is stretched over the whole window, no letterbox. The
     * arithmetic lives in click_map.c so the browser build checks it. */
    return ClickMap_WindowToCanvas(plat->window_w, plat->window_h,
                                   plat->canvas_w, plat->canvas_h,
                                   wx, wy, out_cx, out_cy);
}

SDL_Rect TAK_Platform_CanvasRectToWindow(const TAK_Platform *plat,
                                          SDL_Rect r) {
    SDL_Rect o = r;
    if (!plat) return o;
    /* Same transform TAK_Platform_Present uses for the canvas texture,
     * inverted from TAK_Platform_MapMouseToCanvas. */
    int out[4];
    ClickMap_CanvasRectToWindow(plat->window_w, plat->window_h,
                                plat->canvas_w, plat->canvas_h,
                                r.x, r.y, r.w, r.h, out);
    o.x = out[0]; o.y = out[1]; o.w = out[2]; o.h = out[3];
    return o;
}
