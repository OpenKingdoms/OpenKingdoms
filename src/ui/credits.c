/*
 * credits.c -- Full-screen clip playback (GAMESTATE_CREDITS).
 *
 * Plays one clip out of the game folder, centred on the 640x480 canvas,
 * then goes to the state it was asked for. A key that types a character
 * or an Alt key ends it early, a click does not (legacy:34816-34849).
 * The music is paused while it plays (legacy:140736).
 */

#include "tak_credits.h"
#include "tak_gameloop.h"
#include "tak_bink.h"
#include "tak_blit.h"
#include "tak_ui.h"
#include "tak_music.h"
#include "tak_memory.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define CREDITS_CLIP "Movies/Credits.bik"

static struct {
    int          initialized;
    BinkPlayer  *player;
    int          return_state;
    int          prev_key_down;
} cr;

/* What the next entry to the screen plays, and where it goes after. */
static char s_request[128] = CREDITS_CLIP;
static int  s_request_state = GAMESTATE_MENU;

void Credits_Request(const char *rel_path, int next_state) {
    snprintf(s_request, sizeof(s_request), "%s",
             (rel_path && rel_path[0]) ? rel_path : CREDITS_CLIP);
    s_request_state = next_state;
}

int Credits_ReturnState(void) { return cr.return_state; }

/* A key the original's player would have seen as a character or an
 * Alt press: letters, digits, the editing keys, the keypad, Alt itself
 * and F10, which Windows reports as a system key. */
static int dismissing_key_down(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    for (int sc = SDL_SCANCODE_A; sc <= SDL_SCANCODE_SLASH; sc++)
        if (keys[sc]) return 1;
    for (int sc = SDL_SCANCODE_KP_DIVIDE; sc <= SDL_SCANCODE_KP_PERIOD; sc++)
        if (keys[sc]) return 1;
    return keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT] ||
           keys[SDL_SCANCODE_F10];
}

int Credits_Init(TAK_Platform *platform) {
    (void)platform;
    memset(&cr, 0, sizeof(cr));
    cr.return_state = s_request_state;
    char rel[sizeof(s_request)];
    snprintf(rel, sizeof(rel), "%s", s_request);
    /* The request is spent: the door's own entry plays the credits. */
    snprintf(s_request, sizeof(s_request), "%s", CREDITS_CLIP);
    s_request_state = GAMESTATE_MENU;

    cr.player = BinkPlayer_OpenClip(rel);
    if (!cr.player && strcmp(rel, CREDITS_CLIP) == 0) {
        /* Some installs carry one credits file and not the other. */
        cr.player = BinkPlayer_OpenClip("Movies/PostTakCredits.bik");
    }
    if (!cr.player) {
        fprintf(stderr, "Credits: could not open %s\n", rel);
        return -1;
    }
    fprintf(stderr, "Credits: playing %s %dx%d @ %.3fs/frame, %d frames\n",
            rel,
            BinkPlayer_GetWidth(cr.player),
            BinkPlayer_GetHeight(cr.player),
            BinkPlayer_GetFrameDuration(cr.player),
            BinkPlayer_GetFrameCount(cr.player));
    /* A key still held from the screen before does not count. */
    cr.prev_key_down = dismissing_key_down();
    TAK_Music_Pause(1);
    cr.initialized = 1;
    return 0;
}

void Credits_Shutdown(void) {
    if (!cr.initialized) return;
    if (cr.player) BinkPlayer_Close(cr.player);
    TAK_Music_Pause(0);
    memset(&cr, 0, sizeof(cr));
}

int Credits_Tick(TAK_Platform *platform, float frame_dt) {
    if (!cr.initialized) return GAMESTATE_MENU;
    if (!cr.player)      return cr.return_state;

    int key_down = dismissing_key_down();
    int key_edge = key_down && !cr.prev_key_down;
    cr.prev_key_down = key_down;
    if (key_edge) return cr.return_state;

    BinkPlayer_Advance(cr.player, frame_dt);

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

    if (BinkPlayer_IsFinished(cr.player)) return cr.return_state;
    return GAMESTATE_CREDITS;
}
