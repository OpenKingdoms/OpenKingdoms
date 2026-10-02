/*
 * ingame_keys.c -- in-game key bindings that stand on their own.
 *
 * Separate from ingame.c so a test can press a key with no window and
 * no world, and separate from the debug decoder because these ship.
 */

#include "tak_ingame_keys.h"
#include "tak_game_speed.h"

#include <SDL.h>

#define IG_KEY_PRESSED(sc) (keys[sc] && !prev[sc])

InGameSpeedKey InGame_SpeedKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return IG_SPEED_KEY_NONE;
    /* Alt is the debug modifier, and nothing bound here answers it. */
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT])
        return IG_SPEED_KEY_NONE;
    if (IG_KEY_PRESSED(SDL_SCANCODE_EQUALS) || IG_KEY_PRESSED(SDL_SCANCODE_KP_PLUS))
        return IG_SPEED_KEY_UP;
    if (IG_KEY_PRESSED(SDL_SCANCODE_MINUS) || IG_KEY_PRESSED(SDL_SCANCODE_KP_MINUS))
        return IG_SPEED_KEY_DOWN;
    return IG_SPEED_KEY_NONE;
}

void InGame_ApplySpeedKeys(const uint8_t *keys, const uint8_t *prev) {
    switch (InGame_SpeedKey(keys, prev)) {
    case IG_SPEED_KEY_UP:   GameSpeed_Increase(); break;
    case IG_SPEED_KEY_DOWN: GameSpeed_Decrease(); break;
    case IG_SPEED_KEY_NONE:
    default: break;
    }
}

int InGame_TurnKey(const uint8_t *keys, const uint8_t *prev, int view3d) {
    if (!keys || !prev || !view3d) return 0;
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return 0;
    int shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    if (IG_KEY_PRESSED(SDL_SCANCODE_R)) return shift ? -1 : 1;
    if (IG_KEY_PRESSED(SDL_SCANCODE_RIGHTBRACKET)) return 1;
    if (IG_KEY_PRESSED(SDL_SCANCODE_LEFTBRACKET)) return -1;
    return 0;
}

int InGame_WatchKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return 0;
    return IG_KEY_PRESSED(SDL_SCANCODE_TAB) ? 1 : 0;
}

int InGame_WatchNextView(uint32_t open, int current) {
    int from = (current >= 1 && current <= 8) ? current : 0;
    for (int p = from + 1; p <= 8; p++)
        if (open & (1u << p)) return p;
    return 0;
}

#undef IG_KEY_PRESSED
