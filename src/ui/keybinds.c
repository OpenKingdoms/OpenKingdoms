/*
 * keybinds.c -- the battle's keys as a table a player can change (D-057).
 *
 * No window, no world and no settings: a test can fill the table, press
 * a key and read a file with nothing else up. keybinds_store.c ties it
 * to the options and the player's folder.
 */

#include "tak_keybinds.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* Which actions can collide. The 3D camera shares R with the turn keys
 * and E with Resurrect: a building being placed, or a selection that
 * can raise, takes the key from the camera. */
enum {
    KX_BATTLE = 0,
    KX_CAMERA3D,
    KX_PLACING,
    KX_RAISE
};

#define ORDER  KG_ORDERS,   KX_BATTLE,   KF_SHIFT_FREE
#define SELECT KG_SELECT,   KX_BATTLE,   KF_SHIFT_FREE | KF_GUI_OK
#define EXACT  KX_BATTLE,   0
#define LOOSE  KX_BATTLE,   KF_SHIFT_FREE | KF_CTRL_FREE | KF_GUI_OK
#define CAM3D  KG_CAMERA,   KX_CAMERA3D, KF_SHIFT_FREE | KF_ALT_OK | KF_GUI_OK

static const KeyActionInfo k_info[KA_COUNT] = {
    [KA_ORDER_MOVE]    = { "Move",    "Move",    ORDER },
    [KA_ORDER_ATTACK]  = { "Attack",  "Attack",  ORDER },
    [KA_ORDER_GUARD]   = { "Guard",   "Guard",   ORDER },
    [KA_ORDER_PATROL]  = { "Patrol",  "Patrol",  ORDER },
    [KA_ORDER_LOAD]    = { "Load",    "Load",    ORDER },
    [KA_ORDER_UNLOAD]  = { "Unload",  "Unload",  ORDER },
    [KA_ORDER_HEAL]    = { "Heal",    "Heal",    ORDER },
    [KA_ORDER_CLEAR]   = { "Clear",   "Clear",   ORDER },
    [KA_ORDER_STOP]    = { "Stop",    "Stop",    ORDER },
    [KA_ORDER_GATE]    = { "Gate",    "Open or Close Gate", ORDER },
    [KA_ORDER_CLOAK]   = { "Cloak",   "Cloak",   ORDER },
    [KA_ORDER_RESURRECT] = { "Resurrect", "Resurrect",
                             KG_ORDERS, KX_RAISE, KF_SHIFT_FREE },

    [KA_SELECT_SAME_TYPE] = { "SelectSameType",  "Select Same Type",  SELECT },
    [KA_SELECT_ALL]       = { "SelectAll",       "Select All",        SELECT },
    [KA_SELECT_ON_SCREEN] = { "SelectOnScreen",  "Select On Screen",  SELECT },
    [KA_SELECT_BUILDER]   = { "SelectBuilders",  "Select Builders",   SELECT },
    [KA_SELECT_MELEE]     = { "SelectMelee",     "Select Melee",      SELECT },
    [KA_SELECT_FACTORY]   = { "SelectFactories", "Select Factories",  SELECT },
    [KA_SELECT_MAGIC]     = { "SelectMagic",     "Select Magic",      SELECT },
    [KA_SELECT_MONARCH]   = { "SelectMonarch",   "Select Monarch",    SELECT },
    /* Ctrl+Shift+N names a category no unit carries, so Shift is part
     * of this one's key. */
    [KA_SELECT_BOAT]      = { "SelectBoats",     "Select Boats",
                              KG_SELECT, KX_BATTLE, KF_GUI_OK },
    [KA_SELECT_BALLISTIC] = { "SelectBallistic", "Select Ballistic",  SELECT },
    [KA_SELECT_ATTACK]    = { "SelectAttackers", "Select Attackers",  SELECT },
    [KA_SELECT_FLY]       = { "SelectFlyers",    "Select Flyers",     SELECT },

    [KA_WEAPON_1] = { "WeaponPrimary",   "Primary Weapon",   KG_WEAPONS, EXACT },
    [KA_WEAPON_2] = { "WeaponSecondary", "Secondary Weapon", KG_WEAPONS, EXACT },
    [KA_WEAPON_3] = { "WeaponSpecial",   "Special Weapon",   KG_WEAPONS, EXACT },

    [KA_PAN_LEFT]  = { "PanLeft",  "Scroll Left",  KG_CAMERA, KX_BATTLE,
                       KF_SHIFT_FREE | KF_ALT_OK | KF_GUI_OK },
    [KA_PAN_RIGHT] = { "PanRight", "Scroll Right", KG_CAMERA, KX_BATTLE,
                       KF_SHIFT_FREE | KF_ALT_OK | KF_GUI_OK },
    [KA_PAN_UP]    = { "PanUp",    "Scroll Up",    KG_CAMERA, KX_BATTLE,
                       KF_SHIFT_FREE | KF_ALT_OK | KF_GUI_OK },
    [KA_PAN_DOWN]  = { "PanDown",  "Scroll Down",  KG_CAMERA, KX_BATTLE,
                       KF_SHIFT_FREE | KF_ALT_OK | KF_GUI_OK },
    [KA_BOOKMARK_1] = { "Bookmark1", "Go To Bookmark 1", KG_CAMERA, EXACT },
    [KA_BOOKMARK_2] = { "Bookmark2", "Go To Bookmark 2", KG_CAMERA, EXACT },
    [KA_BOOKMARK_3] = { "Bookmark3", "Go To Bookmark 3", KG_CAMERA, EXACT },
    [KA_BOOKMARK_4] = { "Bookmark4", "Go To Bookmark 4", KG_CAMERA, EXACT },
    [KA_BOOKMARK_STORE_1] = { "BookmarkStore1", "Set Bookmark 1", KG_CAMERA, EXACT },
    [KA_BOOKMARK_STORE_2] = { "BookmarkStore2", "Set Bookmark 2", KG_CAMERA, EXACT },
    [KA_BOOKMARK_STORE_3] = { "BookmarkStore3", "Set Bookmark 3", KG_CAMERA, EXACT },
    [KA_BOOKMARK_STORE_4] = { "BookmarkStore4", "Set Bookmark 4", KG_CAMERA, EXACT },
    [KA_CAM_TURN_LEFT]  = { "CameraTurnLeft",  "3D View: Orbit Left",  CAM3D },
    [KA_CAM_TURN_RIGHT] = { "CameraTurnRight", "3D View: Orbit Right", CAM3D },
    [KA_CAM_TILT_UP]    = { "CameraTiltUp",    "3D View: Tilt Up",     CAM3D },
    [KA_CAM_TILT_DOWN]  = { "CameraTiltDown",  "3D View: Tilt Down",   CAM3D },
    [KA_CAM_ZOOM_IN]    = { "CameraZoomIn",    "3D View: Zoom In",     CAM3D },
    [KA_CAM_ZOOM_OUT]   = { "CameraZoomOut",   "3D View: Zoom Out",    CAM3D },
    [KA_CAM_RESET]      = { "CameraReset",     "3D View: Classic Angle",
                            KG_CAMERA, KX_CAMERA3D,
                            KF_SHIFT_FREE | KF_CTRL_FREE | KF_ALT_OK | KF_GUI_OK },

    [KA_PAUSE]      = { "Pause",     "Pause",      KG_GAME, EXACT },
    [KA_MENU]       = { "Menu",      "Game Menu",  KG_GAME, EXACT },
    [KA_BRIEFING]   = { "Briefing",  "Briefing",   KG_GAME, EXACT },
    [KA_SAVE]       = { "SaveGame",  "Save Game",  KG_GAME, KX_BATTLE, KF_SHIFT_FREE },
    [KA_LOAD]       = { "LoadGame",  "Load Game",  KG_GAME, KX_BATTLE, KF_SHIFT_FREE },
    [KA_CLEAR_CHAT] = { "ClearChat", "Clear Chat", KG_GAME, EXACT },
    [KA_SPEED_UP]   = { "SpeedUp",   "Game Speed Up",   KG_GAME, LOOSE },
    [KA_SPEED_DOWN] = { "SpeedDown", "Game Speed Down", KG_GAME, LOOSE },
    [KA_SELF_DESTRUCT] = { "SelfDestruct", "Self Destruct", KG_GAME, KX_BATTLE,
                           KF_SHIFT_FREE | KF_GUI_OK },
    [KA_RADAR]      = { "Radar",      "Full Screen Radar",  KG_GAME, LOOSE },
    [KA_VIEW_3D]    = { "View3D",     "Switch 3D View",     KG_GAME, LOOSE },
    [KA_DAMAGE_BARS] = { "DamageBars", "Show Damage Bars",  KG_GAME, KX_BATTLE,
                         KF_SHIFT_FREE | KF_CTRL_FREE | KF_ALT_OK | KF_GUI_OK },

    [KA_TURN_BUILDING_CW]  = { "TurnBuilding",     "Turn Building",
                               KG_BUILDING, KX_PLACING, KF_GUI_OK },
    [KA_TURN_BUILDING_CCW] = { "TurnBuildingBack", "Turn Building Back",
                               KG_BUILDING, KX_PLACING, KF_GUI_OK },
};

static const char *const k_scheme_name[KEY_SCHEME_COUNT] = {
    "standard", "otak", "grid",
};

static const char *const k_scheme_label[KEY_SCHEME_COUNT] = {
    "Standard", "OTAK", "Grid",
};

/* A key alone, a key that takes Shift either way, Ctrl with a key, the
 * same taking Shift either way, and a key that minds neither. */
#define K(sc)   { SDL_SCANCODE_##sc, 0 }
#define KS(sc)  { SDL_SCANCODE_##sc, KM_ANY_SHIFT }
#define C(sc)   { SDL_SCANCODE_##sc, KM_CTRL }
#define CS(sc)  { SDL_SCANCODE_##sc, KM_CTRL | KM_ANY_SHIFT }
#define ANY(sc) { SDL_SCANCODE_##sc, KM_ANY_CTRL | KM_ANY_SHIFT }
#define NO_KEY  { 0, 0 }

typedef struct KeyPresetRow {
    KeyAction a;
    KeyChord  slot[KEYBIND_SLOTS];
} KeyPresetRow;

/* Keys.TDF as the engine binds it, and the engine's own beside it for
 * what the original lacks, such as V for the 3D view. */
static const KeyPresetRow k_otak[] = {
    { KA_ORDER_MOVE,   { KS(M), NO_KEY } },
    { KA_ORDER_ATTACK, { KS(A), NO_KEY } },
    { KA_ORDER_GUARD,  { KS(G), NO_KEY } },
    { KA_ORDER_PATROL, { KS(P), NO_KEY } },
    { KA_ORDER_LOAD,   { KS(L), NO_KEY } },
    { KA_ORDER_UNLOAD, { KS(U), NO_KEY } },
    { KA_ORDER_HEAL,   { KS(H), NO_KEY } },
    { KA_ORDER_CLEAR,  { KS(C), NO_KEY } },
    { KA_ORDER_STOP,   { KS(S), NO_KEY } },
    { KA_ORDER_GATE,   { KS(O), NO_KEY } },
    { KA_ORDER_CLOAK,  { KS(K), NO_KEY } },
    /* Keys.TDF leaves E empty, and E raises in the original (D-057). */
    { KA_ORDER_RESURRECT, { KS(E), NO_KEY } },

    { KA_SELECT_SAME_TYPE, { CS(Z), NO_KEY } },
    { KA_SELECT_ALL,       { CS(A), NO_KEY } },
    { KA_SELECT_ON_SCREEN, { CS(U), NO_KEY } },
    { KA_SELECT_BUILDER,   { CS(B), NO_KEY } },
    { KA_SELECT_MELEE,     { CS(E), NO_KEY } },
    { KA_SELECT_FACTORY,   { CS(F), NO_KEY } },
    { KA_SELECT_MAGIC,     { CS(G), NO_KEY } },
    { KA_SELECT_MONARCH,   { CS(M), NO_KEY } },
    { KA_SELECT_BOAT,      { C(N),  NO_KEY } },
    { KA_SELECT_BALLISTIC, { CS(R), NO_KEY } },
    { KA_SELECT_ATTACK,    { CS(W), NO_KEY } },
    { KA_SELECT_FLY,       { CS(Y), NO_KEY } },

    { KA_WEAPON_1, { C(F1), NO_KEY } },
    { KA_WEAPON_2, { C(F2), NO_KEY } },
    { KA_WEAPON_3, { C(F3), NO_KEY } },

    { KA_PAN_LEFT,  { ANY(LEFT),  NO_KEY } },
    { KA_PAN_RIGHT, { ANY(RIGHT), NO_KEY } },
    { KA_PAN_UP,    { ANY(UP),    NO_KEY } },
    { KA_PAN_DOWN,  { ANY(DOWN),  NO_KEY } },
    { KA_BOOKMARK_1, { K(F5), NO_KEY } },
    { KA_BOOKMARK_2, { K(F6), NO_KEY } },
    { KA_BOOKMARK_3, { K(F7), NO_KEY } },
    { KA_BOOKMARK_4, { K(F8), NO_KEY } },
    { KA_BOOKMARK_STORE_1, { C(F5), NO_KEY } },
    { KA_BOOKMARK_STORE_2, { C(F6), NO_KEY } },
    { KA_BOOKMARK_STORE_3, { C(F7), NO_KEY } },
    { KA_BOOKMARK_STORE_4, { C(F8), NO_KEY } },
    /* Without Ctrl, so Ctrl+Z selects and does not zoom as well. */
    { KA_CAM_TURN_LEFT,  { KS(Q), NO_KEY } },
    { KA_CAM_TURN_RIGHT, { KS(E), NO_KEY } },
    { KA_CAM_TILT_UP,    { KS(R), NO_KEY } },
    { KA_CAM_TILT_DOWN,  { KS(F), NO_KEY } },
    { KA_CAM_ZOOM_IN,    { KS(Z), NO_KEY } },
    { KA_CAM_ZOOM_OUT,   { KS(X), NO_KEY } },
    { KA_CAM_RESET,      { ANY(HOME), NO_KEY } },

    { KA_PAUSE,      { K(PAUSE), NO_KEY } },
    { KA_MENU,       { K(F1),    NO_KEY } },
    { KA_BRIEFING,   { K(F2),    NO_KEY } },
    { KA_SAVE,       { CS(S),    NO_KEY } },
    { KA_LOAD,       { CS(L),    NO_KEY } },
    { KA_CLEAR_CHAT, { K(F12),   NO_KEY } },
    { KA_SPEED_UP,   { ANY(EQUALS), ANY(KP_PLUS) } },
    { KA_SPEED_DOWN, { ANY(MINUS),  ANY(KP_MINUS) } },
    { KA_SELF_DESTRUCT,  { CS(D),          NO_KEY } },
    { KA_RADAR,          { ANY(TAB),       NO_KEY } },
    { KA_VIEW_3D,        { ANY(V),         NO_KEY } },
    { KA_DAMAGE_BARS,    { ANY(GRAVE),     NO_KEY } },

    /* The brackets turn with Shift held too, which a builder placing a
     * row holds. Without Ctrl on R, which selects. */
    { KA_TURN_BUILDING_CW,  { K(R), ANY(RIGHTBRACKET) } },
    { KA_TURN_BUILDING_CCW, { { SDL_SCANCODE_R, KM_SHIFT }, ANY(LEFTBRACKET) } },
};

/* What Standard changes: the letters pan beside the arrows, and Attack
 * and Stop are on B and N (D-050). */
static const KeyPresetRow k_standard[] = {
    { KA_PAN_LEFT,     { ANY(LEFT),  KS(A) } },
    { KA_PAN_RIGHT,    { ANY(RIGHT), KS(D) } },
    { KA_PAN_UP,       { ANY(UP),    KS(W) } },
    { KA_PAN_DOWN,     { ANY(DOWN),  KS(S) } },
    { KA_ORDER_ATTACK, { KS(B), NO_KEY } },
    { KA_ORDER_STOP,   { KS(N), NO_KEY } },
};

/* What Grid changes: the orders sit under the left hand, the top row in
 * the order a StarCraft II grid has them, and the 3D camera moves to
 * the keys above the arrows to leave the letters free.
 *   Q Move    W Stop   E Guard    R Patrol   T Attack
 *   A Heal    S Load   D Unload   F Clear    G Gate
 *   Z Cloak   X Resurrect   C turns a building   V view */
static const KeyPresetRow k_grid[] = {
    { KA_ORDER_MOVE,   { KS(Q), NO_KEY } },
    { KA_ORDER_STOP,   { KS(W), NO_KEY } },
    { KA_ORDER_GUARD,  { KS(E), NO_KEY } },
    { KA_ORDER_PATROL, { KS(R), NO_KEY } },
    { KA_ORDER_ATTACK, { KS(T), NO_KEY } },
    { KA_ORDER_HEAL,   { KS(A), NO_KEY } },
    { KA_ORDER_LOAD,   { KS(S), NO_KEY } },
    { KA_ORDER_UNLOAD, { KS(D), NO_KEY } },
    { KA_ORDER_CLEAR,  { KS(F), NO_KEY } },
    { KA_ORDER_GATE,   { KS(G), NO_KEY } },
    { KA_ORDER_CLOAK,  { KS(Z), NO_KEY } },
    { KA_ORDER_RESURRECT, { KS(X), NO_KEY } },
    { KA_CAM_TURN_LEFT,  { KS(DELETE),   NO_KEY } },
    { KA_CAM_TURN_RIGHT, { KS(PAGEDOWN), NO_KEY } },
    { KA_CAM_TILT_UP,    { KS(HOME),     NO_KEY } },
    { KA_CAM_TILT_DOWN,  { KS(END),      NO_KEY } },
    { KA_CAM_ZOOM_IN,    { KS(INSERT),   NO_KEY } },
    { KA_CAM_ZOOM_OUT,   { KS(PAGEUP),   NO_KEY } },
    { KA_CAM_RESET,      { ANY(BACKSPACE), NO_KEY } },
    { KA_TURN_BUILDING_CW,  { K(C), ANY(RIGHTBRACKET) } },
    { KA_TURN_BUILDING_CCW, { { SDL_SCANCODE_C, KM_SHIFT }, ANY(LEFTBRACKET) } },
};

#undef K
#undef KS
#undef C
#undef CS
#undef ANY
#undef NO_KEY

static struct {
    int       ready;
    KeyChord  preset[KEY_SCHEME_COUNT][KA_COUNT][KEYBIND_SLOTS];
    /* The player's own keys, for the actions marked changed. */
    KeyChord  custom[KA_COUNT][KEYBIND_SLOTS];
    uint8_t   changed[KA_COUNT];
    int       custom_on;
    KeyScheme scheme;
} kb;

static void kb_fill(KeyScheme s, const KeyPresetRow *rows, size_t n) {
    for (size_t i = 0; i < n; i++)
        memcpy(kb.preset[s][rows[i].a], rows[i].slot, sizeof(rows[i].slot));
}

#define KB_ROWS(t) (t), sizeof(t) / sizeof((t)[0])

static void kb_ready(void) {
    if (kb.ready) return;
    kb.ready = 1;
    kb.scheme = KEY_SCHEME_DEFAULT;   /* until Keybinds_Init reads the options */
    kb.custom_on = 1;
    for (int s = 0; s < KEY_SCHEME_COUNT; s++)
        kb_fill((KeyScheme)s, KB_ROWS(k_otak));
    kb_fill(KEY_SCHEME_STANDARD, KB_ROWS(k_standard));
    kb_fill(KEY_SCHEME_GRID, KB_ROWS(k_grid));
}

static int kb_action_ok(KeyAction a) { return a >= 0 && a < KA_COUNT; }
static int kb_slot_ok(int slot) { return slot >= 0 && slot < KEYBIND_SLOTS; }
static int kb_scheme_ok(KeyScheme s) { return s >= 0 && s < KEY_SCHEME_COUNT; }

/* The two keys of an action as they stand. */
static const KeyChord *kb_chords(KeyAction a) {
    kb_ready();
    return (kb.custom_on && kb.changed[a]) ? kb.custom[a] : kb.preset[kb.scheme][a];
}

const KeyActionInfo *Keybinds_Info(KeyAction a) {
    return kb_action_ok(a) ? &k_info[a] : NULL;
}

const char *Keybinds_SchemeName(KeyScheme s) {
    return k_scheme_name[kb_scheme_ok(s) ? s : KEY_SCHEME_DEFAULT];
}

const char *Keybinds_SchemeLabel(KeyScheme s) {
    return k_scheme_label[kb_scheme_ok(s) ? s : KEY_SCHEME_DEFAULT];
}

KeyScheme Keybinds_SchemeFromName(const char *name) {
    if (name)
        for (int s = 0; s < KEY_SCHEME_COUNT; s++)
            if (SDL_strcasecmp(name, k_scheme_name[s]) == 0) return (KeyScheme)s;
    return KEY_SCHEME_DEFAULT;
}

KeyScheme Keybinds_Scheme(void) {
    kb_ready();
    return kb.scheme;
}

void Keybinds_UseScheme(KeyScheme s) {
    kb_ready();
    if (kb_scheme_ok(s)) kb.scheme = s;
}

int Keybinds_CustomCount(void) {
    int n = 0;
    for (int a = 0; a < KA_COUNT; a++) n += kb.changed[a] != 0;
    return n;
}

int Keybinds_HasCustom(void) { return Keybinds_CustomCount() > 0; }

int Keybinds_IsCustom(KeyAction a) { return kb_action_ok(a) && kb.changed[a]; }

int Keybinds_CustomOn(void) {
    kb_ready();
    return kb.custom_on;
}

void Keybinds_SetCustomOn(int on) {
    kb_ready();
    kb.custom_on = on != 0;
}

KeyChord Keybinds_SchemeDefault(KeyScheme s, KeyAction a, int slot) {
    KeyChord none = { 0, 0 };
    kb_ready();
    if (!kb_scheme_ok(s) || !kb_action_ok(a) || !kb_slot_ok(slot)) return none;
    return kb.preset[s][a][slot];
}

KeyChord Keybinds_Get(KeyAction a, int slot) {
    KeyChord none = { 0, 0 };
    if (!kb_action_ok(a) || !kb_slot_ok(slot)) return none;
    return kb_chords(a)[slot];
}

/* Make an action the player's own, starting from the scheme's keys. */
static void kb_take(KeyAction a) {
    kb_ready();
    if (kb.changed[a]) return;
    memcpy(kb.custom[a], kb.preset[kb.scheme][a], sizeof(kb.custom[a]));
    kb.changed[a] = 1;
}

void Keybinds_Set(KeyAction a, int slot, KeyChord chord) {
    if (!kb_action_ok(a) || !kb_slot_ok(slot)) return;
    kb_take(a);
    kb.custom_on = 1;
    if (!chord.key) chord.mods = 0;
    kb.custom[a][slot] = chord;
    /* Field by field: a chord has padding. */
    int same = 1;
    for (int s = 0; s < KEYBIND_SLOTS; s++) {
        KeyChord have = kb.custom[a][s], was = kb.preset[kb.scheme][a][s];
        if (have.key != was.key || have.mods != was.mods) same = 0;
    }
    if (same) kb.changed[a] = 0;
}

void Keybinds_ResetAction(KeyAction a) {
    if (kb_action_ok(a)) kb.changed[a] = 0;
}

void Keybinds_ResetAll(void) {
    memset(kb.changed, 0, sizeof(kb.changed));
}

static int kb_contexts_meet(KeyAction a, KeyAction b) {
    int ca = k_info[a].context, cb = k_info[b].context;
    if (ca == cb) return 1;
    if (ca == KX_CAMERA3D) return cb != KX_PLACING && cb != KX_RAISE;
    if (cb == KX_CAMERA3D) return ca != KX_PLACING && ca != KX_RAISE;
    return 1;
}

/* Two chords one press can answer both of. */
static int kb_chords_meet(KeyChord x, KeyChord y) {
    if (!x.key || x.key != y.key) return 0;
    if (!((x.mods | y.mods) & KM_ANY_CTRL) && ((x.mods ^ y.mods) & KM_CTRL)) return 0;
    if (!((x.mods | y.mods) & KM_ANY_SHIFT) && ((x.mods ^ y.mods) & KM_SHIFT)) return 0;
    return 1;
}

KeyAction Keybinds_Conflict(KeyAction a, KeyChord chord, int *slot) {
    if (!kb_action_ok(a) || !chord.key) return KA_NONE;
    for (int b = 0; b < KA_COUNT; b++) {
        if (b == (int)a || !kb_contexts_meet(a, (KeyAction)b)) continue;
        const KeyChord *other = kb_chords((KeyAction)b);
        for (int s = 0; s < KEYBIND_SLOTS; s++) {
            if (!kb_chords_meet(chord, other[s])) continue;
            if (slot) *slot = s;
            return (KeyAction)b;
        }
    }
    return KA_NONE;
}

int Keybinds_HasConflict(KeyAction a) {
    if (!kb_action_ok(a)) return 0;
    const KeyChord *c = kb_chords(a);
    for (int s = 0; s < KEYBIND_SLOTS; s++)
        if (Keybinds_Conflict(a, c[s], NULL) != KA_NONE) return 1;
    return 0;
}

static int kb_ctrl(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
}

static int kb_shift(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
}

static int kb_alt(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
}

/* Cmd or the Windows key, which a browser on a Mac sends for its own
 * shortcuts. */
static int kb_gui(const uint8_t *keys) {
    return keys[SDL_SCANCODE_LGUI] || keys[SDL_SCANCODE_RGUI];
}

static int kb_mods_match(KeyChord c, const uint8_t *keys) {
    if (!(c.mods & KM_ANY_CTRL) && kb_ctrl(keys) != ((c.mods & KM_CTRL) != 0)) return 0;
    if (!(c.mods & KM_ANY_SHIFT) && kb_shift(keys) != ((c.mods & KM_SHIFT) != 0)) return 0;
    return 1;
}

/* The slot of `a` that is down with its modifiers, or -1. */
static int kb_down_slot(KeyAction a, const uint8_t *keys, const uint8_t *prev) {
    if (!kb_action_ok(a) || !keys) return -1;
    if (!(k_info[a].flags & KF_ALT_OK) && kb_alt(keys)) return -1;
    if (!(k_info[a].flags & KF_GUI_OK) && kb_gui(keys)) return -1;
    const KeyChord *c = kb_chords(a);
    for (int s = 0; s < KEYBIND_SLOTS; s++) {
        if (!c[s].key || c[s].key >= SDL_NUM_SCANCODES || !keys[c[s].key]) continue;
        if (prev && prev[c[s].key]) continue;
        if (kb_mods_match(c[s], keys)) return s;
    }
    return -1;
}

int Keybinds_Pressed(KeyAction a, const uint8_t *keys, const uint8_t *prev) {
    return prev && kb_down_slot(a, keys, prev) >= 0;
}

int Keybinds_Held(KeyAction a, const uint8_t *keys) {
    return kb_down_slot(a, keys, NULL) >= 0;
}

void Keybinds_ReleaseHeld(KeyAction a, uint8_t *keys) {
    if (!kb_action_ok(a) || !keys) return;
    if (!(k_info[a].flags & KF_ALT_OK) && kb_alt(keys)) return;
    if (!(k_info[a].flags & KF_GUI_OK) && kb_gui(keys)) return;
    const KeyChord *c = kb_chords(a);
    for (int s = 0; s < KEYBIND_SLOTS; s++)
        if (c[s].key && c[s].key < SDL_NUM_SCANCODES && kb_mods_match(c[s], keys))
            keys[c[s].key] = 0;
}

/* Escape and Enter belong to the dialogs and the chat line, the digits
 * to the control groups. */
static int kb_bindable(int sc) {
    if (sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI) return 0;
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return 0;
    if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_RETURN ||
        sc == SDL_SCANCODE_KP_ENTER) return 0;
    return SDL_GetScancodeName((SDL_Scancode)sc)[0] != '\0';
}

int Keybinds_Capture(KeyAction a, const uint8_t *keys, const uint8_t *prev,
                     KeyChord *out) {
    if (!kb_action_ok(a) || !keys || !prev || !out) return 0;
    kb_ready();
    for (int sc = SDL_SCANCODE_A; sc < SDL_NUM_SCANCODES; sc++) {
        if (!keys[sc] || prev[sc]) continue;
        if (sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI) continue;
        if (!kb_bindable(sc) || kb_alt(keys)) return -1;
        int flags = k_info[a].flags;
        if (kb_gui(keys) && !(flags & KF_GUI_OK)) return -1;
        /* The scheme's own key for the action comes back as the scheme
         * has it, so binding it again changes nothing. */
        const KeyChord *own = kb.preset[kb.scheme][a];
        for (int s = 0; s < KEYBIND_SLOTS; s++) {
            if (own[s].key != sc || !kb_mods_match(own[s], keys)) continue;
            *out = own[s];
            return 1;
        }
        uint8_t mods = 0;
        if (flags & KF_CTRL_FREE) mods |= KM_ANY_CTRL;
        else if (kb_ctrl(keys))   mods |= KM_CTRL;
        if (flags & KF_SHIFT_FREE) mods |= KM_ANY_SHIFT;
        else if (kb_shift(keys))   mods |= KM_SHIFT;
        out->key = (uint16_t)sc;
        out->mods = mods;
        return 1;
    }
    return 0;
}

char *Keybinds_ChordText(KeyChord chord, int for_file, char *buf, int cap) {
    if (!buf || cap <= 0) return buf;
    buf[0] = '\0';
    if (!chord.key) return buf;
    const char *ctrl = (chord.mods & KM_ANY_CTRL) ? (for_file ? "(Ctrl)+" : "")
                     : (chord.mods & KM_CTRL) ? "Ctrl+" : "";
    const char *shift = (chord.mods & KM_ANY_SHIFT) ? (for_file ? "(Shift)+" : "")
                      : (chord.mods & KM_SHIFT) ? "Shift+" : "";
    snprintf(buf, (size_t)cap, "%s%s%s", ctrl, shift,
             SDL_GetScancodeName((SDL_Scancode)chord.key));
    return buf;
}

static int kb_take_prefix(const char **text, const char *word) {
    size_t n = strlen(word);
    if (SDL_strncasecmp(*text, word, n) != 0) return 0;
    *text += n;
    return 1;
}

int Keybinds_ChordParse(const char *text, KeyChord *out) {
    if (!text || !out) return -1;
    char trimmed[64];
    while (*text == ' ' || *text == '\t') text++;
    snprintf(trimmed, sizeof(trimmed), "%s", text);
    size_t n = strlen(trimmed);
    while (n && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t' ||
                 trimmed[n - 1] == '\r' || trimmed[n - 1] == '\n'))
        trimmed[--n] = '\0';
    out->key = 0;
    out->mods = 0;
    if (!n) return 0;
    const char *p = trimmed;
    uint8_t mods = 0;
    /* A key's own name comes last, and none is a modifier's word with
     * a plus sign after it. */
    for (;;) {
        if (kb_take_prefix(&p, "(Ctrl)+"))       mods |= KM_ANY_CTRL;
        else if (kb_take_prefix(&p, "(Shift)+")) mods |= KM_ANY_SHIFT;
        else if (kb_take_prefix(&p, "Ctrl+"))    mods |= KM_CTRL;
        else if (kb_take_prefix(&p, "Shift+"))   mods |= KM_SHIFT;
        else break;
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(p);
    if (sc == SDL_SCANCODE_UNKNOWN || !kb_bindable((int)sc)) return -1;
    if (mods & KM_ANY_CTRL)  mods &= (uint8_t)~KM_CTRL;
    if (mods & KM_ANY_SHIFT) mods &= (uint8_t)~KM_SHIFT;
    out->key = (uint16_t)sc;
    out->mods = mods;
    return 0;
}

static KeyAction kb_action_by_id(const char *id) {
    for (int a = 0; a < KA_COUNT; a++)
        if (SDL_strcasecmp(id, k_info[a].id) == 0) return (KeyAction)a;
    return KA_NONE;
}

int Keybinds_LoadFile(const char *path) {
    kb_ready();
    Keybinds_ResetAll();
    FILE *fp = path ? fopen(path, "r") : NULL;
    if (!fp) return -1;
    char line[160];
    while (fgets(line, sizeof(line), fp)) {
        char *name = line;
        while (*name == ' ' || *name == '\t') name++;
        char *eq = strchr(name, '=');
        if (!eq || name[0] == '#') continue;
        *eq = '\0';
        char *end = eq;
        while (end > name && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        int slot = 0;
        char *dot = strrchr(name, '.');
        if (dot && SDL_strcasecmp(dot, ".alt") == 0) {
            *dot = '\0';
            slot = 1;
        }
        KeyAction a = kb_action_by_id(name);
        KeyChord c;
        /* A line this build cannot read is skipped, and the action
         * keeps the key it had. */
        if (a == KA_NONE || Keybinds_ChordParse(eq + 1, &c) != 0) continue;
        kb_take(a);
        kb.custom[a][slot] = c;
    }
    fclose(fp);
    return 0;
}

int Keybinds_SaveFile(const char *path) {
    FILE *fp = path ? fopen(path, "w") : NULL;
    if (!fp) return -1;
    fprintf(fp, "# Your own keys, over the scheme the options name.\n"
                "# Action=key, and Action.alt for a second key.\n"
                "# Ctrl+ and Shift+ go before a key. In brackets, (Shift)+, the\n"
                "# key answers with it held or not. Nothing after = is no key.\n");
    char text[64];
    for (int a = 0; a < KA_COUNT; a++) {
        if (!kb.changed[a]) continue;
        fprintf(fp, "%s=%s\n", k_info[a].id,
                Keybinds_ChordText(kb.custom[a][0], 1, text, (int)sizeof(text)));
        fprintf(fp, "%s.alt=%s\n", k_info[a].id,
                Keybinds_ChordText(kb.custom[a][1], 1, text, (int)sizeof(text)));
    }
    fclose(fp);
    return 0;
}

void Keybinds_Snapshot(KeybindsState *out) {
    if (!out) return;
    kb_ready();
    memcpy(out->custom, kb.custom, sizeof(out->custom));
    memcpy(out->changed, kb.changed, sizeof(out->changed));
    out->custom_on = (uint8_t)kb.custom_on;
    out->scheme = (uint8_t)kb.scheme;
}

void Keybinds_Restore(const KeybindsState *in) {
    if (!in || in->scheme >= KEY_SCHEME_COUNT) return;
    kb_ready();
    memcpy(kb.custom, in->custom, sizeof(kb.custom));
    memcpy(kb.changed, in->changed, sizeof(kb.changed));
    kb.custom_on = in->custom_on != 0;
    kb.scheme = (KeyScheme)in->scheme;
}
