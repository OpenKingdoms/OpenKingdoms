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

/* Keys.TDF's CTRL_ and CTRLSHIFT_ select bindings. CTRL_T names TROOPS
 * and CTRLSHIFT_N names NAVAL, categories no unit carries, so they are
 * left out. CTRL_M also tracks the monarch, which is not here yet. */
static const struct {
    SDL_Scancode key;
    InGameSelectKind kind;
    const char *category;
} k_select_keys[] = {
    { SDL_SCANCODE_Z, IG_SELECT_SAME_TYPE, NULL },
    { SDL_SCANCODE_A, IG_SELECT_ALL, NULL },
    { SDL_SCANCODE_U, IG_SELECT_ON_SCREEN, NULL },
    { SDL_SCANCODE_B, IG_SELECT_CATEGORY, "BUILDER" },
    { SDL_SCANCODE_E, IG_SELECT_CATEGORY, "MELEE" },
    { SDL_SCANCODE_F, IG_SELECT_CATEGORY, "FACTORY" },
    { SDL_SCANCODE_G, IG_SELECT_CATEGORY, "MAGIC" },
    { SDL_SCANCODE_M, IG_SELECT_CATEGORY, "MONARCH" },
    { SDL_SCANCODE_N, IG_SELECT_CATEGORY, "BOAT" },
    { SDL_SCANCODE_R, IG_SELECT_CATEGORY, "BALLISTIC" },
    { SDL_SCANCODE_W, IG_SELECT_CATEGORY, "ATTACK" },
    { SDL_SCANCODE_Y, IG_SELECT_CATEGORY, "FLY" },
};

InGameSelectKey InGame_SelectKey(const uint8_t *keys, const uint8_t *prev) {
    InGameSelectKey none = { IG_SELECT_NONE, NULL, 0 };
    if (!keys || !prev) return none;
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return none;
    if (!keys[SDL_SCANCODE_LCTRL] && !keys[SDL_SCANCODE_RCTRL]) return none;
    int shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    for (size_t i = 0; i < sizeof(k_select_keys) / sizeof(k_select_keys[0]); i++) {
        if (!IG_KEY_PRESSED(k_select_keys[i].key)) continue;
        if (shift && k_select_keys[i].key == SDL_SCANCODE_N) return none;
        InGameSelectKey k = { k_select_keys[i].kind, k_select_keys[i].category,
                              shift && k_select_keys[i].kind == IG_SELECT_CATEGORY };
        return k;
    }
    return none;
}

int InGame_WatchNextView(uint32_t open, int current) {
    int from = (current >= 1 && current <= 8) ? current : 0;
    for (int p = from + 1; p <= 8; p++)
        if (open & (1u << p)) return p;
    return 0;
}

#undef IG_KEY_PRESSED
