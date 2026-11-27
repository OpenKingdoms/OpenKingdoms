/*
 * credits.c -- Full-screen clip playback (GAMESTATE_CREDITS).
 *
 * Plays one clip out of the game folder, or two in a row, centred on
 * the 640x480 canvas, then goes to the state it was asked for. A key
 * that types a character or an Alt key ends it early, a click does not
 * (legacy:34816-34849).
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
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define CREDITS_PROBE EMSCRIPTEN_KEEPALIVE
#else
#define CREDITS_PROBE
#endif

#define CREDITS_CLIP "Movies/Credits.bik"

static struct {
    int          initialized;
    BinkPlayer  *player;
    int          return_state;
    int          prev_key_down;
    char         then[128];     /* the clip after this one, "" for none */
} cr;

/* What the next entry to the screen plays, and where it goes after. */
static char s_request[128] = CREDITS_CLIP;
static char s_then[128];
static int  s_request_state = GAMESTATE_MENU;

void Credits_Request(const char *rel_path, int next_state) {
    Credits_RequestPair(rel_path, NULL, next_state);
}

void Credits_RequestPair(const char *first, const char *then, int next_state) {
    snprintf(s_request, sizeof(s_request), "%s",
             (first && first[0]) ? first : CREDITS_CLIP);
    snprintf(s_then, sizeof(s_then), "%s", then ? then : "");
    s_request_state = next_state;
}

int Credits_ReturnState(void) { return cr.return_state; }

const char *Credits_RequestedClip(void) { return s_request; }
const char *Credits_RequestedThen(void) { return s_then; }
int Credits_RequestedState(void) { return s_request_state; }

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

/* Opens a clip and says so, the line the browser smoke waits for. */
static BinkPlayer *open_clip(const char *rel) {
    BinkPlayer *bp = BinkPlayer_OpenClip(rel);
    if (bp)
        fprintf(stderr, "Credits: playing %s %dx%d @ %.3fs/frame, %d frames\n",
                rel, BinkPlayer_GetWidth(bp), BinkPlayer_GetHeight(bp),
                BinkPlayer_GetFrameDuration(bp), BinkPlayer_GetFrameCount(bp));
    return bp;
}

int Credits_Init(TAK_Platform *platform) {
    (void)platform;
    memset(&cr, 0, sizeof(cr));
    cr.return_state = s_request_state;
    char rel[sizeof(s_request)];
    snprintf(rel, sizeof(rel), "%s", s_request);
    snprintf(cr.then, sizeof(cr.then), "%s", s_then);
    /* The request is spent: the door's own entry plays the credits. */
    snprintf(s_request, sizeof(s_request), "%s", CREDITS_CLIP);
    s_then[0] = '\0';
    s_request_state = GAMESTATE_MENU;

    cr.player = open_clip(rel);
    if (!cr.player && strcmp(rel, CREDITS_CLIP) == 0) {
        /* Some installs carry one credits file and not the other. */
        cr.player = open_clip("Movies/PostTakCredits.bik");
    }
    /* A first clip the install lacks leaves the second to play. */
    if (!cr.player && cr.then[0]) {
        snprintf(rel, sizeof(rel), "%s", cr.then);
        cr.then[0] = '\0';
        cr.player = open_clip(rel);
    }
    if (!cr.player) {
        fprintf(stderr, "Credits: could not open %s\n", rel);
        return -1;
    }
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

/* Frames of the reel's soundtrack the mixer has taken, -1 with no reel
 * up and 0 for a reel with no sound. A page reads it as
 * Module._Credits_SoundPlayed() to hear that a clip has sound. */
CREDITS_PROBE int Credits_SoundPlayed(void) {
    if (!cr.initialized || !cr.player) return -1;
    if (!BinkPlayer_HasAudio(cr.player)) return 0;
    int64_t n = BinkPlayer_AudioPlayed(cr.player);
    return n > 0x7fffffff ? 0x7fffffff : (int)n;
}

int Credits_Tick(TAK_Platform *platform, float frame_dt) {
    if (!cr.initialized) return GAMESTATE_MENU;
    if (!cr.player)      return cr.return_state;

    /* A key ends the pair, not just the clip on show: the original
     * skips a mission's clip when its shield was cut short
     * (legacy:241646-241649). */
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

    if (BinkPlayer_IsFinished(cr.player)) {
        if (!cr.then[0]) return cr.return_state;
        BinkPlayer_Close(cr.player);
        cr.player = open_clip(cr.then);
        cr.then[0] = '\0';
        if (!cr.player) return cr.return_state;
    }
    return GAMESTATE_CREDITS;
}
