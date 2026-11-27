/*
 * ingame_keys.c -- in-game key bindings that stand on their own.
 *
 * Separate from ingame.c so a test can press a key with no window and
 * no world, and separate from the debug decoder because these ship.
 */

#include "tak_ingame_keys.h"
#include "tak_game_speed.h"
#include "tak_keybinds.h"

#include <SDL.h>
#include <string.h>

/* Every key here is asked of the table in keybinds.c, which holds the
 * modifiers each one takes. Alt is the debug modifier, and the table
 * lets it through to nothing a player orders with. */

InGameSpeedKey InGame_SpeedKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return IG_SPEED_KEY_NONE;
    if (Keybinds_Pressed(KA_SPEED_UP, keys, prev)) return IG_SPEED_KEY_UP;
    if (Keybinds_Pressed(KA_SPEED_DOWN, keys, prev)) return IG_SPEED_KEY_DOWN;
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
    if (Keybinds_Pressed(KA_TURN_BUILDING_CW, keys, prev)) return 1;
    if (Keybinds_Pressed(KA_TURN_BUILDING_CCW, keys, prev)) return -1;
    return 0;
}

int InGame_WatchKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    return Keybinds_Pressed(KA_RADAR, keys, prev);
}

int InGame_ViewKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    return Keybinds_Pressed(KA_VIEW_3D, keys, prev);
}

int InGame_DamageBarsKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    return Keybinds_Pressed(KA_DAMAGE_BARS, keys, prev);
}

void InGame_PanKeys(const uint8_t *keys, int *dx, int *dy) {
    int x = 0, y = 0;
    if (keys) {
        if (Keybinds_Held(KA_PAN_LEFT, keys))  x -= 1;
        if (Keybinds_Held(KA_PAN_RIGHT, keys)) x += 1;
        if (Keybinds_Held(KA_PAN_UP, keys))    y -= 1;
        if (Keybinds_Held(KA_PAN_DOWN, keys))  y += 1;
    }
    if (dx) *dx = x;
    if (dy) *dy = y;
}

/* Keys.TDF's CTRL_ and CTRLSHIFT_ select bindings. CTRL_T names TROOPS
 * and CTRLSHIFT_N names NAVAL, categories no unit carries, so they are
 * left out. CTRL_M also tracks the monarch, which is not here yet. */
static const struct {
    KeyAction action;
    InGameSelectKind kind;
    const char *category;
} k_select_keys[] = {
    { KA_SELECT_SAME_TYPE, IG_SELECT_SAME_TYPE, NULL },
    { KA_SELECT_ALL,       IG_SELECT_ALL, NULL },
    { KA_SELECT_ON_SCREEN, IG_SELECT_ON_SCREEN, NULL },
    { KA_SELECT_BUILDER,   IG_SELECT_CATEGORY, "BUILDER" },
    { KA_SELECT_MELEE,     IG_SELECT_CATEGORY, "MELEE" },
    { KA_SELECT_FACTORY,   IG_SELECT_CATEGORY, "FACTORY" },
    { KA_SELECT_MAGIC,     IG_SELECT_CATEGORY, "MAGIC" },
    { KA_SELECT_MONARCH,   IG_SELECT_CATEGORY, "MONARCH" },
    { KA_SELECT_BOAT,      IG_SELECT_CATEGORY, "BOAT" },
    { KA_SELECT_BALLISTIC, IG_SELECT_CATEGORY, "BALLISTIC" },
    { KA_SELECT_ATTACK,    IG_SELECT_CATEGORY, "ATTACK" },
    { KA_SELECT_FLY,       IG_SELECT_CATEGORY, "FLY" },
};

InGameSelectKey InGame_SelectKey(const uint8_t *keys, const uint8_t *prev) {
    InGameSelectKey none = { IG_SELECT_NONE, NULL, 0 };
    if (!keys || !prev) return none;
    int shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    for (size_t i = 0; i < sizeof(k_select_keys) / sizeof(k_select_keys[0]); i++) {
        if (!Keybinds_Pressed(k_select_keys[i].action, keys, prev)) continue;
        InGameSelectKey k = { k_select_keys[i].kind, k_select_keys[i].category,
                              shift && k_select_keys[i].kind == IG_SELECT_CATEGORY };
        return k;
    }
    return none;
}

int InGame_BoxSkipsMonarchKey(const uint8_t *keys) {
    if (!keys) return 0;
    /* Cmd or the Windows key does not stop the box, as it stops Move. */
    uint8_t held[SDL_NUM_SCANCODES];
    memcpy(held, keys, sizeof(held));
    held[SDL_SCANCODE_LGUI] = held[SDL_SCANCODE_RGUI] = 0;
    return Keybinds_Held(KA_ORDER_MOVE, held);
}

int InGame_CloakKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    return Keybinds_Pressed(KA_ORDER_CLOAK, keys, prev);
}

/* Keys.TDF's LOWER_ and UPPER_ letters that name a UnitCommand. The
 * cloak has its own decoder above. */
static const struct {
    KeyAction action;
    InGameOrderKey order;
} k_order_keys[] = {
    { KA_ORDER_MOVE,   IG_ORDER_MOVE },
    { KA_ORDER_ATTACK, IG_ORDER_ATTACK },
    { KA_ORDER_GUARD,  IG_ORDER_GUARD },
    { KA_ORDER_PATROL, IG_ORDER_PATROL },
    { KA_ORDER_LOAD,   IG_ORDER_LOAD },
    { KA_ORDER_UNLOAD, IG_ORDER_UNLOAD },
    { KA_ORDER_HEAL,   IG_ORDER_HEAL },
    { KA_ORDER_CLEAR,  IG_ORDER_CLEAR },
    { KA_ORDER_STOP,   IG_ORDER_STOP },
    { KA_ORDER_GATE,   IG_ORDER_GATE },
    { KA_ORDER_RESURRECT, IG_ORDER_RESURRECT },
};

InGameOrderKey InGame_OrderKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return IG_ORDER_NONE;
    for (size_t i = 0; i < sizeof(k_order_keys) / sizeof(k_order_keys[0]); i++)
        if (Keybinds_Pressed(k_order_keys[i].action, keys, prev))
            return k_order_keys[i].order;
    return IG_ORDER_NONE;
}

int InGame_MenuKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    return Keybinds_Pressed(KA_MENU, keys, prev);
}

InGameFnKey InGame_FnKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return IG_FN_NONE;
    if (Keybinds_Pressed(KA_SAVE, keys, prev)) return IG_FN_SAVE;
    if (Keybinds_Pressed(KA_LOAD, keys, prev)) return IG_FN_LOAD;
    if (Keybinds_Pressed(KA_PAUSE, keys, prev)) return IG_FN_PAUSE;
    if (Keybinds_Pressed(KA_BRIEFING, keys, prev)) return IG_FN_BRIEFING;
    if (Keybinds_Pressed(KA_CLEAR_CHAT, keys, prev)) return IG_FN_CLEAR_CHAT;
    return IG_FN_NONE;
}

int InGame_WeaponKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return -1;
    for (int i = 0; i < 3; i++)
        if (Keybinds_Pressed((KeyAction)(KA_WEAPON_1 + i), keys, prev)) return i;
    return -1;
}

int InGame_BookmarkKey(const uint8_t *keys, const uint8_t *prev, int *store) {
    if (!keys || !prev) return -1;
    for (int i = 0; i < 4; i++) {
        int put = Keybinds_Pressed((KeyAction)(KA_BOOKMARK_STORE_1 + i), keys, prev);
        if (!put && !Keybinds_Pressed((KeyAction)(KA_BOOKMARK_1 + i), keys, prev))
            continue;
        if (store) *store = put;
        return i;
    }
    return -1;
}

int InGame_SelfDestructKey(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return 0;
    return Keybinds_Pressed(KA_SELF_DESTRUCT, keys, prev);
}

int InGame_WatchNextView(uint32_t open, int current) {
    int from = (current >= 1 && current <= 8) ? current : 0;
    for (int p = from + 1; p <= 8; p++)
        if (open & (1u << p)) return p;
    return 0;
}
