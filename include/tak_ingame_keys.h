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

/* Tab, while watching a match. Alt is left to the debug tools. */
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

#endif /* TAK_INGAME_KEYS_H */
