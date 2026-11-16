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

/* Cmd or the Windows key, which a browser on a Mac sends for its own
 * shortcuts. */
static int ig_gui(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LGUI] || keys[SDL_SCANCODE_RGUI];
}

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

int InGame_BoxSkipsMonarchKey(const uint8_t *keys) {
    if (!keys) return 0;
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return 0;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) return 0;
    return keys[SDL_SCANCODE_M] ? 1 : 0;
}

int InGame_CloakKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return 0;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL] || ig_gui(keys)) return 0;
    return IG_KEY_PRESSED(SDL_SCANCODE_K) ? 1 : 0;
}

/* Keys.TDF's LOWER_ and UPPER_ letters that name a UnitCommand. K is
 * the cloak and has its own decoder above. */
static const struct {
    SDL_Scancode key;
    InGameOrderKey order;
} k_order_keys[] = {
    { SDL_SCANCODE_M, IG_ORDER_MOVE },
    { SDL_SCANCODE_A, IG_ORDER_ATTACK },
    { SDL_SCANCODE_G, IG_ORDER_GUARD },
    { SDL_SCANCODE_P, IG_ORDER_PATROL },
    { SDL_SCANCODE_L, IG_ORDER_LOAD },
    { SDL_SCANCODE_U, IG_ORDER_UNLOAD },
    { SDL_SCANCODE_H, IG_ORDER_HEAL },
    { SDL_SCANCODE_C, IG_ORDER_CLEAR },
    { SDL_SCANCODE_S, IG_ORDER_STOP },
    { SDL_SCANCODE_O, IG_ORDER_GATE },
};

InGameOrderKey InGame_OrderKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return IG_ORDER_NONE;
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return IG_ORDER_NONE;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) return IG_ORDER_NONE;
    if (ig_gui(keys)) return IG_ORDER_NONE;
    for (size_t i = 0; i < sizeof(k_order_keys) / sizeof(k_order_keys[0]); i++)
        if (IG_KEY_PRESSED(k_order_keys[i].key)) return k_order_keys[i].order;
    return IG_ORDER_NONE;
}

static int ig_alt(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
}

static int ig_ctrl(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
}

static int ig_shift(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
}

int InGame_MenuKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev || ig_alt(keys) || ig_ctrl(keys) || ig_shift(keys) || ig_gui(keys))
        return 0;
    return IG_KEY_PRESSED(SDL_SCANCODE_F1) ? 1 : 0;
}

InGameFnKey InGame_FnKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev || ig_alt(keys) || ig_gui(keys)) return IG_FN_NONE;
    if (ig_ctrl(keys)) {
        if (IG_KEY_PRESSED(SDL_SCANCODE_S)) return IG_FN_SAVE;
        if (IG_KEY_PRESSED(SDL_SCANCODE_L)) return IG_FN_LOAD;
        return IG_FN_NONE;
    }
    /* Shift with these does nothing in the original. */
    if (ig_shift(keys)) return IG_FN_NONE;
    if (IG_KEY_PRESSED(SDL_SCANCODE_PAUSE)) return IG_FN_PAUSE;
    if (IG_KEY_PRESSED(SDL_SCANCODE_F2)) return IG_FN_BRIEFING;
    if (IG_KEY_PRESSED(SDL_SCANCODE_F12)) return IG_FN_CLEAR_CHAT;
    return IG_FN_NONE;
}

int InGame_WeaponKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev || ig_alt(keys) || ig_gui(keys) || !ig_ctrl(keys)) return -1;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) return -1;
    for (int i = 0; i < 3; i++)
        if (IG_KEY_PRESSED(SDL_SCANCODE_F1 + i)) return i;
    return -1;
}

int InGame_BookmarkKey(const uint8_t *keys, const uint8_t *prev, int *store) {
    if (!keys || !prev || ig_alt(keys) || ig_gui(keys)) return -1;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) return -1;
    for (int i = 0; i < 4; i++) {
        if (!IG_KEY_PRESSED(SDL_SCANCODE_F5 + i)) continue;
        if (store) *store = ig_ctrl(keys) ? 1 : 0;
        return i;
    }
    return -1;
}

int InGame_SelfDestructKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return 0;
    if (!keys[SDL_SCANCODE_LCTRL] && !keys[SDL_SCANCODE_RCTRL]) return 0;
    return IG_KEY_PRESSED(SDL_SCANCODE_D) ? 1 : 0;
}

int InGame_WatchNextView(uint32_t open, int current) {
    int from = (current >= 1 && current <= 8) ? current : 0;
    for (int p = from + 1; p <= 8; p++)
        if (open & (1u << p)) return p;
    return 0;
}

#undef IG_KEY_PRESSED
