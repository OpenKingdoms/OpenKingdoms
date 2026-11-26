#ifndef TAK_KEYBINDS_H
#define TAK_KEYBINDS_H

#include <stdint.h>

/*
 * The battle's keys as a table a player can change (D-057).
 *
 * Every key the battle answers is an action with two slots. A scheme
 * fills the table: Standard is the keys of D-050, OTAK is Keys.TDF as the
 * engine binds it, and Grid packs the orders under the left hand. The
 * player's own keys lie over the scheme, an action at a time, and are
 * kept in "keys.cfg" beside the options. The decoders in ingame_keys.c
 * ask this table and test no scancode of their own.
 */

typedef enum KeyAction {
    KA_NONE = -1,

    KA_ORDER_MOVE = 0,
    KA_ORDER_ATTACK,
    KA_ORDER_GUARD,
    KA_ORDER_PATROL,
    KA_ORDER_LOAD,
    KA_ORDER_UNLOAD,
    KA_ORDER_HEAL,
    KA_ORDER_CLEAR,
    KA_ORDER_STOP,
    KA_ORDER_GATE,
    KA_ORDER_CLOAK,
    KA_ORDER_RESURRECT,

    KA_SELECT_SAME_TYPE,
    KA_SELECT_ALL,
    KA_SELECT_ON_SCREEN,
    KA_SELECT_BUILDER,
    KA_SELECT_MELEE,
    KA_SELECT_FACTORY,
    KA_SELECT_MAGIC,
    KA_SELECT_MONARCH,
    KA_SELECT_BOAT,
    KA_SELECT_BALLISTIC,
    KA_SELECT_ATTACK,
    KA_SELECT_FLY,

    KA_WEAPON_1,
    KA_WEAPON_2,
    KA_WEAPON_3,

    KA_PAN_LEFT,
    KA_PAN_RIGHT,
    KA_PAN_UP,
    KA_PAN_DOWN,
    KA_BOOKMARK_1,
    KA_BOOKMARK_2,
    KA_BOOKMARK_3,
    KA_BOOKMARK_4,
    KA_BOOKMARK_STORE_1,
    KA_BOOKMARK_STORE_2,
    KA_BOOKMARK_STORE_3,
    KA_BOOKMARK_STORE_4,
    KA_CAM_TURN_LEFT,
    KA_CAM_TURN_RIGHT,
    KA_CAM_TILT_UP,
    KA_CAM_TILT_DOWN,
    KA_CAM_ZOOM_IN,
    KA_CAM_ZOOM_OUT,
    KA_CAM_RESET,

    KA_PAUSE,
    KA_MENU,
    KA_BRIEFING,
    KA_SAVE,
    KA_LOAD,
    KA_CLEAR_CHAT,
    KA_SPEED_UP,
    KA_SPEED_DOWN,
    KA_SELF_DESTRUCT,
    KA_RADAR,
    KA_VIEW_3D,
    KA_DAMAGE_BARS,

    KA_TURN_BUILDING_CW,
    KA_TURN_BUILDING_CCW,

    KA_COUNT
} KeyAction;

#define KEYBIND_SLOTS 2

/* A key with what Ctrl and Shift must be. KM_CTRL and KM_SHIFT say held,
 * their absence says up, and the ANY bits say either. Alt never binds:
 * it is the debug modifier. key 0 is an empty slot. */
enum {
    KM_CTRL      = 1,
    KM_SHIFT     = 2,
    KM_ANY_CTRL  = 4,
    KM_ANY_SHIFT = 8
};

typedef struct KeyChord {
    uint16_t key;   /* SDL_Scancode */
    uint8_t  mods;  /* KM_ bits */
} KeyChord;

typedef enum KeyGroup {
    KG_ORDERS = 0,
    KG_SELECT,
    KG_WEAPONS,
    KG_CAMERA,
    KG_GAME,
    KG_BUILDING,
    KG_COUNT
} KeyGroup;

/* What an action is, apart from its keys. */
enum {
    KF_SHIFT_FREE = 1,   /* a key the player picks answers with Shift or without */
    KF_CTRL_FREE  = 2,   /* the same for Ctrl */
    KF_ALT_OK     = 4,   /* answers with Alt held, where the rest do not */
    KF_GUI_OK     = 8    /* answers with Cmd or the Windows key held */
};

typedef struct KeyActionInfo {
    const char *id;      /* the name in keys.cfg, never changed once shipped */
    const char *label;   /* the name a screen shows */
    uint8_t group;       /* KeyGroup */
    uint8_t context;     /* which actions it can collide with */
    uint8_t flags;       /* KF_ bits */
} KeyActionInfo;

const KeyActionInfo *Keybinds_Info(KeyAction a);

typedef enum KeyScheme {
    KEY_SCHEME_STANDARD = 0,
    KEY_SCHEME_OTAK,
    KEY_SCHEME_GRID,
    KEY_SCHEME_COUNT
} KeyScheme;

/* The scheme a player gets with nothing chosen, and the one the table
 * holds until Keybinds_Init reads the options. */
#define KEY_SCHEME_DEFAULT KEY_SCHEME_STANDARD

/* "standard", "otak" or "grid", the word in the options file, the name
 * a screen shows, and the scheme a word names, the default for one it
 * does not know. */
const char *Keybinds_SchemeName(KeyScheme s);
const char *Keybinds_SchemeLabel(KeyScheme s);
KeyScheme   Keybinds_SchemeFromName(const char *name);

/* The scheme in use. Changing it leaves the player's own keys lying
 * over the new one: a screen that changes it says so and offers
 * Keybinds_ResetAll. */
KeyScheme Keybinds_Scheme(void);
void      Keybinds_UseScheme(KeyScheme s);

/* The player's own keys: whether there are any, how many actions, and
 * whether one action is among them. They can be switched off without
 * being lost, which leaves the scheme alone to answer. */
int  Keybinds_HasCustom(void);
int  Keybinds_CustomCount(void);
int  Keybinds_IsCustom(KeyAction a);
int  Keybinds_CustomOn(void);
void Keybinds_SetCustomOn(int on);

/* A scheme's own key, whatever the player has laid over it. */
KeyChord Keybinds_SchemeDefault(KeyScheme s, KeyAction a, int slot);

/* The key in use. */
KeyChord Keybinds_Get(KeyAction a, int slot);

/* Change a slot, which makes the action the player's own and switches
 * their keys on. An empty chord clears the slot. An action set back to
 * exactly the scheme's keys is the scheme's again. */
void Keybinds_Set(KeyAction a, int slot, KeyChord chord);

/* Give one action, or all of them, back to the scheme. */
void Keybinds_ResetAction(KeyAction a);
void Keybinds_ResetAll(void);

/* The other action that already answers `chord` where `a` would, or
 * KA_NONE. Sets *slot to the slot it sits in. */
KeyAction Keybinds_Conflict(KeyAction a, KeyChord chord, int *slot);

/* 1 when a key of `a` as it stands collides. A change of scheme under
 * the player's own keys can leave one that does. */
int Keybinds_HasConflict(KeyAction a);

/* 1 on the frame one of the action's keys goes down with its modifiers
 * as bound, and 1 while one is down. */
int Keybinds_Pressed(KeyAction a, const uint8_t *keys, const uint8_t *prev);
int Keybinds_Held(KeyAction a, const uint8_t *keys);

/* Lift every key of an action in a copy of the keyboard. */
void Keybinds_Release(KeyAction a, uint8_t *keys);

/* The key a player pressed this frame to bind to `a`: 1 and the chord,
 * 0 for no key yet, -1 for a key that cannot be bound (Escape, Enter,
 * the digits, the modifiers, anything with Alt). */
int Keybinds_Capture(KeyAction a, const uint8_t *keys, const uint8_t *prev,
                     KeyChord *out);

/* A chord as text. The file form writes an either way modifier in
 * brackets, "(Shift)+]", and reads back. The shown form leaves those
 * out. An empty slot is "". Returns buf. */
char *Keybinds_ChordText(KeyChord chord, int for_file, char *buf, int cap);
int   Keybinds_ChordParse(const char *text, KeyChord *out);

/* The player's own keys to and from a file of Action=key lines, with
 * Action.alt for the second slot. Only the actions they changed are in
 * it. A slot the file leaves out of an action it names keeps the key of
 * the scheme in use, so choose the scheme before loading. Loading
 * forgets the keys held before. Returns 0 on success. */
int Keybinds_LoadFile(const char *path);
int Keybinds_SaveFile(const char *path);

/* Everything a dialog can change, to put back on Cancel. */
typedef struct KeybindsState {
    KeyChord custom[KA_COUNT][KEYBIND_SLOTS];
    uint8_t  changed[KA_COUNT];
    uint8_t  custom_on;
    uint8_t  scheme;
} KeybindsState;

void Keybinds_Snapshot(KeybindsState *out);
void Keybinds_Restore(const KeybindsState *in);

/* keybinds_store.c: the scheme the options name and the player's file,
 * read at start, and both written back. Keybinds_Store sets the options
 * and leaves Settings_Save to the caller. */
#define TAK_SETTING_KEY_SCHEME  "KeyScheme"
#define TAK_SETTING_KEYS_CUSTOM "KeysCustom"
void Keybinds_Init(void);
int  Keybinds_Store(void);
const char *Keybinds_FilePath(void);

#endif /* TAK_KEYBINDS_H */
