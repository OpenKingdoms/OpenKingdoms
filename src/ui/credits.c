/*
 * credits.c -- Credits playback (GAMESTATE_CREDITS).
 *
 * Plays Movies/Credits.bik full-screen (centered on 640x480). Returns
 * to GAMESTATE_MENU when the clip ends, Escape is pressed, or any
 * mouse click occurs. Reuses BinkPlayer (the same decoder that drives
 * the main menu hover clips).
 */

#include "tak_credits.h"
#include "tak_gameloop.h"
#include "tak_bink.h"
#include "tak_blit.h"
#include "tak_ui.h"
#include "tak_memory.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

static struct {
    int          initialized;
    BinkPlayer  *player;
    double       video_timer;
    int          prev_mouse_down;
} cr;

static BinkPlayer *open_credits_bik(void) {
    /* Try lowercase and uppercase extensions. */
    char path[512];
    snprintf(path, sizeof(path), "%s/Movies/Credits.bik", TAK_GAME_DIR);
    BinkPlayer *bp = BinkPlayer_Open(path);
    if (bp) return bp;
    snprintf(path, sizeof(path), "%s/Movies/Credits.BIK", TAK_GAME_DIR);
    bp = BinkPlayer_Open(path);
    if (bp) return bp;
    /* Fall back to PostTakCredits.bik if the main credits file is
     * missing (some installs lack one or the other). */
    snprintf(path, sizeof(path), "%s/Movies/PostTakCredits.bik", TAK_GAME_DIR);
    return BinkPlayer_Open(path);
}

int Credits_Init(TAK_Platform *platform) {
    (void)platform;
    memset(&cr, 0, sizeof(cr));
    cr.player = open_credits_bik();
    if (!cr.player) {
        fprintf(stderr, "Credits: could not open Credits.bik\n");
        return -1;
    }
    fprintf(stderr, "Credits: playing %dx%d @ %.3fs/frame, %d frames\n",
            BinkPlayer_GetWidth(cr.player),
            BinkPlayer_GetHeight(cr.player),
            BinkPlayer_GetFrameDuration(cr.player),
            BinkPlayer_GetFrameCount(cr.player));
    cr.initialized = 1;
    return 0;
}

void Credits_Shutdown(void) {
    if (!cr.initialized) return;
    if (cr.player) BinkPlayer_Close(cr.player);
    memset(&cr, 0, sizeof(cr));
}

int Credits_Tick(TAK_Platform *platform, float frame_dt) {
    if (!cr.initialized) return GAMESTATE_MENU;
    if (!cr.player)      return GAMESTATE_MENU;

    /* Input: ESC or click → skip to end. */
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int mouse_down = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT);
    int mouse_click = (!mouse_down && cr.prev_mouse_down);
    cr.prev_mouse_down = mouse_down;

    if (keys[SDL_SCANCODE_ESCAPE] || mouse_click) {
        return GAMESTATE_MENU;
    }

    /* Advance video. */
    if (!BinkPlayer_IsFinished(cr.player)) {
        double fd = BinkPlayer_GetFrameDuration(cr.player);
        if (fd <= 0) fd = 1.0 / 30.0;
        cr.video_timer += frame_dt;
        while (cr.video_timer >= fd) {
            cr.video_timer -= fd;
            if (!BinkPlayer_NextFrame(cr.player)) break;
        }
    }

    /* Render — clear to black, draw frame centered on the 640x480 surface. */
    SDL_Surface *off = UI_Offscreen();
    SDL_Rect full = { 0, 0, 640, 480 };
    SDL_FillRect(off, &full, SDL_MapRGBA(off->format, 0, 0, 0, 255));

    const uint32_t *pixels = BinkPlayer_GetPixels(cr.player);
    int vw = BinkPlayer_GetWidth(cr.player);
    int vh = BinkPlayer_GetHeight(cr.player);
    if (pixels && vw > 0 && vh > 0) {
        int dx = (640 - vw) / 2;
        int dy = (480 - vh) / 2;
        Blit_RGBA(off, dx, dy, pixels, vw, vh);
    }

    UI_Present(platform);

    /* Natural end of video → back to menu. */
    if (BinkPlayer_IsFinished(cr.player)) return GAMESTATE_MENU;
    return GAMESTATE_CREDITS;
}
