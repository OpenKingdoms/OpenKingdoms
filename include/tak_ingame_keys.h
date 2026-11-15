#ifndef TAK_INGAME_KEYS_H
#define TAK_INGAME_KEYS_H

#include <stdint.h>

/*
 * In-game key decoding that is worth testing on its own.
 *
 * The original does not test scancodes in the battle loop at all. A key
 * reaches a binding table built from Keys.TDF, the table names a
 * command, and the command runs (legacy:242938, legacy:122813-122845).
 * Until that layer exists, the bindings live here, in one place a test
 * can press.
 */

typedef enum InGameSpeedKey {
    IG_SPEED_KEY_NONE = 0,
    IG_SPEED_KEY_UP,
    IG_SPEED_KEY_DOWN
} InGameSpeedKey;

/* The game speed keys, from an SDL keyboard state array and last
 * frame's copy of it. The original binds them by character code, not by
 * key name: SYMBOL_2B and SYMBOL_3D raise the speed, SYMBOL_2D and
 * SYMBOL_5F lower it, which is '+', '=', '-' and '_'. On a US layout
 * that is the two keys right of the number row, and the keypad pair
 * produces the same characters. */
InGameSpeedKey InGame_SpeedKey(const uint8_t *keys, const uint8_t *prev);

/* Decode the speed keys and apply them. Nothing happens when the
 * control is unavailable, which GameSpeed_SetLevel enforces. */
void InGame_ApplySpeedKeys(const uint8_t *keys, const uint8_t *prev);

/* R or ] turns an armed building clockwise, Shift+R or [ back, and
 * only in the 3D view: the classic view has no camera to turn. The
 * original binds none of them (keys.tdf LOWER_R, SYMBOL_5B, 5D).
 * Returns the quarter turns to make, 1, -1, or 0 for none. */
int  InGame_TurnKey(const uint8_t *keys, const uint8_t *prev, int view3d);

/* Tab: the full screen radar for a player, the next view for a watcher.
 * Alt is left to the debug tools. */
int  InGame_WatchKey(const uint8_t *keys, const uint8_t *prev);

/* The view a watcher's Tab moves to: from the whole map to the first
 * player in the battle, from each player to the next, and from the last
 * back to the whole map. `open` has bit p set for each player p, 1 to 8,
 * in the battle. 0 is the whole map. */
int  InGame_WatchNextView(uint32_t open, int current);

/* The select keys, Ctrl with a letter, as Keys.TDF binds them. Shift
 * turns SelectUnits into SelectUnitsAdd and leaves the rest alone. */
typedef enum InGameSelectKind {
    IG_SELECT_NONE = 0,
    IG_SELECT_SAME_TYPE,   /* SelectAllUnitsSelectedType, Ctrl+Z */
    IG_SELECT_ALL,         /* SelectAllUnits, Ctrl+A */
    IG_SELECT_ON_SCREEN,   /* SelectUnitsOnScreen, Ctrl+U */
    IG_SELECT_CATEGORY     /* SelectUnits <category>, Ctrl+B and the rest */
} InGameSelectKind;

typedef struct InGameSelectKey {
    InGameSelectKind kind;
    const char *category;  /* the FBI category word for IG_SELECT_CATEGORY */
    int add;               /* 1 keeps the selection */
} InGameSelectKey;

InGameSelectKey InGame_SelectKey(const uint8_t *keys, const uint8_t *prev);

/* M held without Ctrl or Alt: a box drag leaves your monarch out. Ctrl+M
 * stays the select key. M is also Move, which arms on the press. */
int  InGame_BoxSkipsMonarchKey(const uint8_t *keys);

/* Keys.TDF's UnitCommand letters, read by scancode. Shift is the upper
 * case and means the same, and Ctrl or Alt makes a letter no order. */
typedef enum InGameOrderKey {
    IG_ORDER_NONE = 0,
    IG_ORDER_MOVE,
    IG_ORDER_ATTACK,
    IG_ORDER_GUARD,
    IG_ORDER_PATROL,
    IG_ORDER_LOAD,
    IG_ORDER_UNLOAD,
    IG_ORDER_HEAL,
    IG_ORDER_CLEAR,
    IG_ORDER_STOP,
    IG_ORDER_GATE
} InGameOrderKey;

/* The order key that went down this frame, or IG_ORDER_NONE. */
InGameOrderKey InGame_OrderKey(const uint8_t *keys, const uint8_t *prev);

/* Keys.TDF's other keys the engine has a feature for. None fires with Alt. */
typedef enum InGameFnKey {
    IG_FN_NONE = 0,
    IG_FN_PAUSE,       /* PAUSE: TogglePause */
    IG_FN_BRIEFING,    /* F2: Briefing */
    IG_FN_CLEAR_CHAT,  /* F12: ClearChat */
    IG_FN_SAVE,        /* Ctrl+S, with or without Shift: SaveGame */
    IG_FN_LOAD         /* Ctrl+L, with or without Shift: LoadGame */
} InGameFnKey;

InGameFnKey InGame_FnKey(const uint8_t *keys, const uint8_t *prev);

/* F1 alone opens the menu. Ctrl+F1 is a weapon key, and Shift+F1 does
 * nothing, as in the original. */
int  InGame_MenuKey(const uint8_t *keys, const uint8_t *prev);

/* Ctrl+F1 to F3: UnitCommand PrimaryWeapon, SecondaryWeapon and
 * SpecialWeapon. Returns the slot 0 to 2, or -1. */
int  InGame_WeaponKey(const uint8_t *keys, const uint8_t *prev);

/* F5 to F8 recall a camera bookmark and Ctrl+F5 to F8 store one. Returns
 * the slot 0 to 3 and sets *store, or returns -1. */
int  InGame_BookmarkKey(const uint8_t *keys, const uint8_t *prev, int *store);

/* Ctrl+D, with or without Shift: Keys.TDF's CTRL_D and CTRLSHIFT_D bind
 * SelfDestruct. */
int  InGame_SelfDestructKey(const uint8_t *keys, const uint8_t *prev);

/* K: Keys.TDF's LOWER_K and UPPER_K, UnitCommand ToggleCloak. */
int  InGame_CloakKey(const uint8_t *keys, const uint8_t *prev);

#endif /* TAK_INGAME_KEYS_H */
