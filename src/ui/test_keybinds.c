/* The key table: the built in schemes, a player's own keys, their file.
 *
 * Standard has to answer every key as the fixed decoders did before
 * the table, which the key cases in test_orders press. The cases here
 * press the keys with a rule of their own. Headless and data free.
 */

#include "tak_keybinds.h"
#include "tak_ingame_keys.h"
#include "tak_game_speed.h"
#include "tak_paths.h"
#include "tak_settings.h"
#include "test_framework.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* The decoders link the speed control, which nothing here turns. */
void GameSpeed_Increase(void) {}
void GameSpeed_Decrease(void) {}

static uint8_t keys[SDL_NUM_SCANCODES];
static uint8_t prev[SDL_NUM_SCANCODES];

enum { CTRL = 1, SHIFT = 2, ALT = 4, GUI = 8 };

static void hold(SDL_Scancode sc, int mods) {
    memset(keys, 0, sizeof(keys));
    memset(prev, 0, sizeof(prev));
    keys[sc] = 1;
    if (mods & CTRL)  keys[SDL_SCANCODE_LCTRL] = 1;
    if (mods & SHIFT) keys[SDL_SCANCODE_RSHIFT] = 1;
    if (mods & ALT)   keys[SDL_SCANCODE_LALT] = 1;
    if (mods & GUI)   keys[SDL_SCANCODE_LGUI] = 1;
}

static int pressed(KeyAction a, SDL_Scancode sc, int mods) {
    hold(sc, mods);
    return Keybinds_Pressed(a, keys, prev);
}

/* The OTAK scheme with no keys of the player's own. */
static void fresh(void) {
    Keybinds_ResetAll();
    Keybinds_SetCustomOn(1);
    Keybinds_UseScheme(KEY_SCHEME_OTAK);
}

static KeyChord chord(SDL_Scancode sc, uint8_t mods) {
    KeyChord c = { (uint16_t)sc, mods };
    return c;
}

static int key_of(KeyAction a, int slot) { return Keybinds_Get(a, slot).key; }

TEST(every_action_has_a_name_and_the_names_differ) {
    for (int a = 0; a < KA_COUNT; a++) {
        const KeyActionInfo *i = Keybinds_Info((KeyAction)a);
        ASSERT_NOT_NULL(i);
        ASSERT_NOT_NULL(i->id);
        ASSERT_NOT_NULL(i->label);
        ASSERT(i->id[0] && i->label[0]);
        ASSERT(strchr(i->id, '=') == NULL && strchr(i->id, '.') == NULL);
        ASSERT(i->group < KG_COUNT);
        for (int b = 0; b < a; b++)
            ASSERT(SDL_strcasecmp(i->id, Keybinds_Info((KeyAction)b)->id) != 0);
    }
    ASSERT(Keybinds_Info(KA_NONE) == NULL);
    ASSERT(Keybinds_Info(KA_COUNT) == NULL);
    for (int s = 0; s < KEY_SCHEME_COUNT; s++)
        ASSERT_EQ_INT(s, (int)Keybinds_SchemeFromName(Keybinds_SchemeName((KeyScheme)s)));
    ASSERT_EQ_INT(KEY_SCHEME_DEFAULT, (int)Keybinds_SchemeFromName("no such scheme"));
    ASSERT_EQ_INT(KEY_SCHEME_DEFAULT, (int)Keybinds_SchemeFromName(NULL));
    ASSERT_EQ_INT(KEY_SCHEME_STANDARD, KEY_SCHEME_DEFAULT);
}

TEST(a_scheme_gives_every_action_a_key_and_no_key_twice) {
    for (int s = 0; s < KEY_SCHEME_COUNT; s++) {
        fresh();
        Keybinds_UseScheme((KeyScheme)s);
        for (int a = 0; a < KA_COUNT; a++) {
            ASSERT(key_of((KeyAction)a, 0) != 0);
            for (int slot = 0; slot < KEYBIND_SLOTS; slot++) {
                KeyChord c = Keybinds_Get((KeyAction)a, slot);
                KeyAction other = Keybinds_Conflict((KeyAction)a, c, NULL);
                if (other != KA_NONE)
                    printf("  %s: %s meets %s\n", Keybinds_SchemeName((KeyScheme)s),
                           Keybinds_Info((KeyAction)a)->id, Keybinds_Info(other)->id);
                ASSERT_EQ_INT(KA_NONE, (int)other);
            }
            ASSERT_EQ_INT(0, Keybinds_HasConflict((KeyAction)a));
        }
    }
}

TEST(the_otak_scheme_keeps_each_keys_modifiers) {
    fresh();
    /* An order letter takes Shift and nothing else. */
    ASSERT(pressed(KA_ORDER_ATTACK, SDL_SCANCODE_A, 0));
    ASSERT(pressed(KA_ORDER_ATTACK, SDL_SCANCODE_A, SHIFT));
    ASSERT(!pressed(KA_ORDER_ATTACK, SDL_SCANCODE_A, CTRL));
    ASSERT(!pressed(KA_ORDER_ATTACK, SDL_SCANCODE_A, ALT));
    ASSERT(!pressed(KA_ORDER_ATTACK, SDL_SCANCODE_A, GUI));
    /* A select key needs Ctrl, and lets Cmd through as it always did. */
    ASSERT(pressed(KA_SELECT_ALL, SDL_SCANCODE_A, CTRL));
    ASSERT(pressed(KA_SELECT_ALL, SDL_SCANCODE_A, CTRL | SHIFT | GUI));
    ASSERT(!pressed(KA_SELECT_ALL, SDL_SCANCODE_A, 0));
    ASSERT(!pressed(KA_SELECT_ALL, SDL_SCANCODE_A, CTRL | ALT));
    ASSERT(pressed(KA_SELECT_BOAT, SDL_SCANCODE_N, CTRL));
    ASSERT(!pressed(KA_SELECT_BOAT, SDL_SCANCODE_N, CTRL | SHIFT));
    /* F1 alone is the menu and Ctrl+F1 alone a weapon. */
    ASSERT(pressed(KA_MENU, SDL_SCANCODE_F1, 0));
    ASSERT(!pressed(KA_MENU, SDL_SCANCODE_F1, SHIFT));
    ASSERT(!pressed(KA_MENU, SDL_SCANCODE_F1, CTRL));
    ASSERT(pressed(KA_WEAPON_1, SDL_SCANCODE_F1, CTRL));
    ASSERT(!pressed(KA_WEAPON_1, SDL_SCANCODE_F1, CTRL | SHIFT));
    ASSERT(pressed(KA_SAVE, SDL_SCANCODE_S, CTRL | SHIFT));
    ASSERT(!pressed(KA_ORDER_STOP, SDL_SCANCODE_S, CTRL));
    /* The speed keys answer to both of their keys with Shift or Ctrl. */
    ASSERT(pressed(KA_SPEED_UP, SDL_SCANCODE_EQUALS, SHIFT));
    ASSERT(pressed(KA_SPEED_UP, SDL_SCANCODE_KP_PLUS, CTRL));
    ASSERT(!pressed(KA_SPEED_UP, SDL_SCANCODE_EQUALS, ALT));
    /* A builder placing a row holds Shift, and the brackets still turn. */
    ASSERT(pressed(KA_TURN_BUILDING_CW, SDL_SCANCODE_R, 0));
    ASSERT(!pressed(KA_TURN_BUILDING_CW, SDL_SCANCODE_R, SHIFT));
    ASSERT(pressed(KA_TURN_BUILDING_CCW, SDL_SCANCODE_R, SHIFT));
    ASSERT(pressed(KA_TURN_BUILDING_CW, SDL_SCANCODE_RIGHTBRACKET, SHIFT));
    ASSERT(pressed(KA_TURN_BUILDING_CCW, SDL_SCANCODE_LEFTBRACKET, 0));
    /* A key held from the frame before is no press. */
    hold(SDL_SCANCODE_A, 0);
    prev[SDL_SCANCODE_A] = 1;
    ASSERT(!Keybinds_Pressed(KA_ORDER_ATTACK, keys, prev));
    ASSERT(Keybinds_Held(KA_ORDER_ATTACK, keys));
}

TEST(standard_pans_on_the_letters_and_moves_attack_and_stop) {
    fresh();
    int dx = 0, dy = 0;
    hold(SDL_SCANCODE_A, 0);
    InGame_PanKeys(keys, &dx, &dy);
    ASSERT_EQ_INT(0, dx);
    ASSERT_EQ_INT(IG_ORDER_ATTACK, (int)InGame_OrderKey(keys, prev));

    Keybinds_UseScheme(KEY_SCHEME_STANDARD);
    InGame_PanKeys(keys, &dx, &dy);
    ASSERT_EQ_INT(-1, dx);
    ASSERT_EQ_INT(0, dy);
    ASSERT_EQ_INT(IG_ORDER_NONE, (int)InGame_OrderKey(keys, prev));
    hold(SDL_SCANCODE_S, SHIFT);
    InGame_PanKeys(keys, &dx, &dy);
    ASSERT_EQ_INT(1, dy);
    /* With Ctrl the letter is a select key and not the camera. */
    hold(SDL_SCANCODE_A, CTRL);
    InGame_PanKeys(keys, &dx, &dy);
    ASSERT_EQ_INT(0, dx);
    ASSERT_EQ_INT(IG_SELECT_ALL, (int)InGame_SelectKey(keys, prev).kind);
    hold(SDL_SCANCODE_LEFT, CTRL);
    InGame_PanKeys(keys, &dx, &dy);
    ASSERT_EQ_INT(-1, dx);
    hold(SDL_SCANCODE_B, 0);
    ASSERT_EQ_INT(IG_ORDER_ATTACK, (int)InGame_OrderKey(keys, prev));
    hold(SDL_SCANCODE_N, 0);
    ASSERT_EQ_INT(IG_ORDER_STOP, (int)InGame_OrderKey(keys, prev));
}

TEST(grid_puts_the_orders_under_the_left_hand) {
    fresh();
    Keybinds_UseScheme(KEY_SCHEME_GRID);
    static const struct { SDL_Scancode sc; InGameOrderKey order; } grid[] = {
        { SDL_SCANCODE_Q, IG_ORDER_MOVE },   { SDL_SCANCODE_W, IG_ORDER_STOP },
        { SDL_SCANCODE_E, IG_ORDER_GUARD },  { SDL_SCANCODE_R, IG_ORDER_PATROL },
        { SDL_SCANCODE_T, IG_ORDER_ATTACK }, { SDL_SCANCODE_A, IG_ORDER_HEAL },
        { SDL_SCANCODE_S, IG_ORDER_LOAD },   { SDL_SCANCODE_D, IG_ORDER_UNLOAD },
        { SDL_SCANCODE_F, IG_ORDER_CLEAR },  { SDL_SCANCODE_G, IG_ORDER_GATE },
    };
    for (size_t i = 0; i < sizeof(grid) / sizeof(grid[0]); i++) {
        hold(grid[i].sc, 0);
        ASSERT_EQ_INT(grid[i].order, (int)InGame_OrderKey(keys, prev));
    }
    hold(SDL_SCANCODE_Z, 0);
    ASSERT_EQ_INT(1, InGame_CloakKey(keys, prev));
    hold(SDL_SCANCODE_X, 0);
    ASSERT_EQ_INT(IG_ORDER_RESURRECT, (int)InGame_OrderKey(keys, prev));
    /* R is Patrol, so a building turns on C and the brackets. */
    hold(SDL_SCANCODE_R, 0);
    ASSERT_EQ_INT(0, InGame_TurnKey(keys, prev, 1));
    hold(SDL_SCANCODE_C, 0);
    ASSERT_EQ_INT(1, InGame_TurnKey(keys, prev, 1));
    hold(SDL_SCANCODE_C, SHIFT);
    ASSERT_EQ_INT(-1, InGame_TurnKey(keys, prev, 1));
    hold(SDL_SCANCODE_X, 0);
    ASSERT_EQ_INT(0, InGame_TurnKey(keys, prev, 1));
    /* The 3D camera leaves the letters. */
    hold(SDL_SCANCODE_Q, 0);
    ASSERT(!Keybinds_Held(KA_CAM_TURN_LEFT, keys));
    hold(SDL_SCANCODE_DELETE, 0);
    ASSERT(Keybinds_Held(KA_CAM_TURN_LEFT, keys));
    /* The select keys stay where they were. */
    hold(SDL_SCANCODE_A, CTRL);
    ASSERT_EQ_INT(IG_SELECT_ALL, (int)InGame_SelectKey(keys, prev).kind);
}

TEST(the_players_keys_lie_over_whichever_scheme_is_in_use) {
    fresh();
    ASSERT_EQ_INT(0, Keybinds_HasCustom());
    Keybinds_Set(KA_ORDER_GUARD, 1, chord(SDL_SCANCODE_J, KM_ANY_SHIFT));
    ASSERT_EQ_INT(1, Keybinds_CustomCount());
    ASSERT_EQ_INT(1, Keybinds_IsCustom(KA_ORDER_GUARD));
    ASSERT_EQ_INT(0, Keybinds_IsCustom(KA_ORDER_ATTACK));
    ASSERT_EQ_INT(KEY_SCHEME_OTAK, (int)Keybinds_Scheme());
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_J, 0));
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_G, 0));

    /* A new scheme changes what the player left alone and nothing else. */
    Keybinds_UseScheme(KEY_SCHEME_GRID);
    ASSERT_EQ_INT(1, Keybinds_CustomCount());
    ASSERT_EQ_INT(SDL_SCANCODE_T, key_of(KA_ORDER_ATTACK, 0));
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_J, 0));
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_G, 0));
    ASSERT(!pressed(KA_ORDER_GUARD, SDL_SCANCODE_E, 0));
    /* Their G now meets the scheme's Gate, and the table says so. */
    ASSERT_EQ_INT(1, Keybinds_HasConflict(KA_ORDER_GUARD));
    ASSERT_EQ_INT(1, Keybinds_HasConflict(KA_ORDER_GATE));
    ASSERT_EQ_INT(0, Keybinds_HasConflict(KA_ORDER_MOVE));
    /* The scheme's own key is still there to read. */
    ASSERT_EQ_INT(SDL_SCANCODE_E, Keybinds_SchemeDefault(KEY_SCHEME_GRID,
                                                        KA_ORDER_GUARD, 0).key);

    /* Switched off they are kept, and the scheme answers alone. */
    Keybinds_SetCustomOn(0);
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_E, 0));
    ASSERT(!pressed(KA_ORDER_GUARD, SDL_SCANCODE_J, 0));
    ASSERT_EQ_INT(1, Keybinds_HasCustom());
    Keybinds_SetCustomOn(1);
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_J, 0));

    /* The reset the screen offers after a change of scheme. */
    Keybinds_ResetAll();
    ASSERT_EQ_INT(0, Keybinds_HasCustom());
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_E, 0));
    ASSERT_EQ_INT(0, Keybinds_HasConflict(KA_ORDER_GATE));
}

TEST(a_conflict_names_the_action_that_holds_the_key) {
    fresh();
    int slot = -1;
    KeyChord s = chord(SDL_SCANCODE_S, KM_ANY_SHIFT);
    ASSERT_EQ_INT(KA_ORDER_STOP, (int)Keybinds_Conflict(KA_ORDER_ATTACK, s, &slot));
    ASSERT_EQ_INT(0, slot);
    ASSERT_EQ_INT(KA_NONE, (int)Keybinds_Conflict(KA_ORDER_STOP, s, NULL));
    /* Ctrl+S is the save key and S alone is not. */
    ASSERT_EQ_INT(KA_SAVE, (int)Keybinds_Conflict(KA_ORDER_ATTACK,
                                                 chord(SDL_SCANCODE_S, KM_CTRL), NULL));
    /* A key that minds no modifier meets both. */
    ASSERT(Keybinds_Conflict(KA_RADAR, chord(SDL_SCANCODE_S, KM_ANY_CTRL | KM_ANY_SHIFT),
                             NULL) != KA_NONE);
    ASSERT_EQ_INT(KA_SPEED_UP, (int)Keybinds_Conflict(
                      KA_ORDER_ATTACK, chord(SDL_SCANCODE_KP_PLUS, 0), &slot));
    ASSERT_EQ_INT(1, slot);
    /* The camera's R and the turn key's R live together. */
    ASSERT_EQ_INT(KA_NONE, (int)Keybinds_Conflict(KA_TURN_BUILDING_CW,
                                                 chord(SDL_SCANCODE_F, 0), NULL));
    ASSERT_EQ_INT(KA_CAM_TILT_DOWN, (int)Keybinds_Conflict(KA_ORDER_ATTACK,
                                                          chord(SDL_SCANCODE_F, 0), NULL));
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp) fclose(fp);
    return fp != NULL;
}

TEST(the_players_keys_survive_their_file_and_the_options) {
    Settings_SetDirectory("test_keybinds_scratch");
    ASSERT(Paths_SaveDir() != NULL);   /* creates the tree */
    remove(Keybinds_FilePath());
    fresh();

    /* No file and no option: the default scheme and nothing over it. */
    Keybinds_UseScheme(KEY_SCHEME_GRID);
    Keybinds_Init();
    ASSERT_EQ_INT(KEY_SCHEME_DEFAULT, (int)Keybinds_Scheme());
    ASSERT_EQ_INT(0, Keybinds_HasCustom());
    ASSERT_EQ_INT(0, Keybinds_Store());
    ASSERT(!file_exists(Keybinds_FilePath()));

    Keybinds_UseScheme(KEY_SCHEME_STANDARD);
    Keybinds_Set(KA_ORDER_ATTACK, 0, chord(SDL_SCANCODE_J, KM_ANY_SHIFT));
    Keybinds_Set(KA_BOOKMARK_1, 1, chord(SDL_SCANCODE_KP_PLUS, KM_CTRL | KM_SHIFT));
    Keybinds_Set(KA_ORDER_CLOAK, 0, chord(SDL_SCANCODE_UNKNOWN, 0));
    ASSERT_EQ_INT(0, Keybinds_Store());
    ASSERT_EQ_STR("standard", Settings_GetStr(TAK_SETTING_KEY_SCHEME, ""));
    ASSERT_EQ_INT(1, Settings_GetInt(TAK_SETTING_KEYS_CUSTOM, 0));

    fresh();
    Keybinds_Init();
    ASSERT_EQ_INT(KEY_SCHEME_STANDARD, (int)Keybinds_Scheme());
    ASSERT_EQ_INT(3, Keybinds_CustomCount());
    ASSERT(pressed(KA_ORDER_ATTACK, SDL_SCANCODE_J, 0));
    ASSERT(!pressed(KA_ORDER_ATTACK, SDL_SCANCODE_B, 0));
    ASSERT(pressed(KA_BOOKMARK_1, SDL_SCANCODE_F5, 0));
    ASSERT(pressed(KA_BOOKMARK_1, SDL_SCANCODE_KP_PLUS, CTRL | SHIFT));
    ASSERT_EQ_INT(0, key_of(KA_ORDER_CLOAK, 0));
    ASSERT(pressed(KA_ORDER_STOP, SDL_SCANCODE_N, 0));

    /* Switched off, they stay in the file and out of the battle. */
    Keybinds_SetCustomOn(0);
    ASSERT_EQ_INT(0, Keybinds_Store());
    fresh();
    Keybinds_Init();
    ASSERT_EQ_INT(0, Keybinds_CustomOn());
    ASSERT_EQ_INT(3, Keybinds_CustomCount());
    ASSERT(pressed(KA_ORDER_ATTACK, SDL_SCANCODE_B, 0));

    /* A hand written file: what it names is taken, what it cannot read
     * is skipped, and a slot it leaves out keeps the scheme's key. */
    FILE *fp = fopen(Keybinds_FilePath(), "w");
    ASSERT_NOT_NULL(fp);
    fprintf(fp, "# mine\nattack = Ctrl+Q\nStop.alt=Space\nNoSuchAction=A\n"
                "Guard=NoSuchKey\nMove=\n");
    fclose(fp);
    Settings_SetStr(TAK_SETTING_KEY_SCHEME, "otak");
    Settings_SetInt(TAK_SETTING_KEYS_CUSTOM, 1);
    fresh();
    Keybinds_Init();
    ASSERT_EQ_INT(3, Keybinds_CustomCount());
    ASSERT(pressed(KA_ORDER_ATTACK, SDL_SCANCODE_Q, CTRL));
    ASSERT(pressed(KA_ORDER_STOP, SDL_SCANCODE_SPACE, 0));
    ASSERT(pressed(KA_ORDER_STOP, SDL_SCANCODE_S, 0));
    ASSERT(pressed(KA_ORDER_GUARD, SDL_SCANCODE_G, 0));
    ASSERT_EQ_INT(0, Keybinds_IsCustom(KA_ORDER_GUARD));
    ASSERT_EQ_INT(0, key_of(KA_ORDER_MOVE, 0));

    /* With nothing left of the player's own the file goes. */
    Keybinds_ResetAll();
    ASSERT_EQ_INT(0, Keybinds_Store());
    ASSERT(!file_exists(Keybinds_FilePath()));
    Settings_SetDirectory(NULL);
}

/* E raises in the original and orbits the 3D camera here. A selection
 * that can raise takes the key from the camera, so the two share it. */
TEST(resurrect_is_on_e_and_shares_it_with_the_camera) {
    for (int s = KEY_SCHEME_STANDARD; s <= KEY_SCHEME_OTAK; s++) {
        fresh();
        Keybinds_UseScheme((KeyScheme)s);
        hold(SDL_SCANCODE_E, 0);
        ASSERT_EQ_INT(IG_ORDER_RESURRECT, (int)InGame_OrderKey(keys, prev));
        ASSERT(Keybinds_Held(KA_CAM_TURN_RIGHT, keys));
        hold(SDL_SCANCODE_E, CTRL);
        ASSERT_EQ_INT(IG_ORDER_NONE, (int)InGame_OrderKey(keys, prev));
        ASSERT_EQ_INT(IG_SELECT_CATEGORY, (int)InGame_SelectKey(keys, prev).kind);
        ASSERT_EQ_INT(0, Keybinds_HasConflict(KA_ORDER_RESURRECT));
        ASSERT_EQ_INT(0, Keybinds_HasConflict(KA_CAM_TURN_RIGHT));
        /* Another order on E does collide with it. */
        ASSERT_EQ_INT(KA_ORDER_RESURRECT, (int)Keybinds_Conflict(
                          KA_ORDER_GUARD, chord(SDL_SCANCODE_E, KM_ANY_SHIFT), NULL));
        /* What the battle does with the camera's keys for such a selection. */
        hold(SDL_SCANCODE_E, 0);
        Keybinds_ReleaseHeld(KA_ORDER_RESURRECT, keys);
        ASSERT(!Keybinds_Held(KA_CAM_TURN_RIGHT, keys));
    }
}

/* Lifting an action's key for another leaves it where the modifiers do
 * not make the action. */
TEST(a_key_is_lifted_only_where_its_action_would_answer) {
    fresh();
    KeyChord ctrl_f = { SDL_SCANCODE_F, KM_CTRL };
    Keybinds_Set(KA_TURN_BUILDING_CW, 0, ctrl_f);
    hold(SDL_SCANCODE_F, 0);
    Keybinds_ReleaseHeld(KA_TURN_BUILDING_CW, keys);
    ASSERT(Keybinds_Held(KA_CAM_TILT_DOWN, keys));
    hold(SDL_SCANCODE_F, CTRL);
    Keybinds_ReleaseHeld(KA_TURN_BUILDING_CW, keys);
    ASSERT_EQ_INT(0, keys[SDL_SCANCODE_F]);
    /* Alt makes no turn, so the bracket stays down. */
    hold(SDL_SCANCODE_RIGHTBRACKET, ALT);
    Keybinds_ReleaseHeld(KA_TURN_BUILDING_CW, keys);
    ASSERT_EQ_INT(1, keys[SDL_SCANCODE_RIGHTBRACKET]);
    Keybinds_ResetAll();
}

/* A scheme's own key pressed into its own action is the scheme's key,
 * and binds nothing new. */
TEST(a_schemes_own_key_captures_as_the_scheme_has_it) {
    for (int s = 0; s < KEY_SCHEME_COUNT; s++) {
        fresh();
        Keybinds_UseScheme((KeyScheme)s);
        for (int a = 0; a < KA_COUNT; a++)
            for (int slot = 0; slot < KEYBIND_SLOTS; slot++) {
                KeyChord want = Keybinds_Get((KeyAction)a, slot), got;
                if (!want.key) continue;
                hold((SDL_Scancode)want.key, ((want.mods & KM_CTRL) ? CTRL : 0) |
                                             ((want.mods & KM_SHIFT) ? SHIFT : 0));
                ASSERT_EQ_INT(1, Keybinds_Capture((KeyAction)a, keys, prev, &got));
                ASSERT_EQ_INT(want.key, got.key);
                ASSERT_EQ_INT(want.mods, got.mods);
                ASSERT_EQ_INT(KA_NONE, (int)Keybinds_Conflict((KeyAction)a, got, NULL));
                Keybinds_Set((KeyAction)a, slot, got);
                ASSERT_EQ_INT(0, Keybinds_IsCustom((KeyAction)a));
            }
    }
    /* A new key for a turn takes Ctrl as pressed, so Ctrl+R stays a
     * select key. */
    fresh();
    KeyChord c;
    hold(SDL_SCANCODE_J, 0);
    ASSERT_EQ_INT(1, Keybinds_Capture(KA_TURN_BUILDING_CW, keys, prev, &c));
    ASSERT_EQ_INT(0, c.mods);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    TEST_SUITE("The key table and its schemes");
    RUN(every_action_has_a_name_and_the_names_differ);
    RUN(a_scheme_gives_every_action_a_key_and_no_key_twice);
    RUN(the_otak_scheme_keeps_each_keys_modifiers);
    RUN(standard_pans_on_the_letters_and_moves_attack_and_stop);
    RUN(grid_puts_the_orders_under_the_left_hand);
    RUN(the_players_keys_lie_over_whichever_scheme_is_in_use);
    RUN(a_conflict_names_the_action_that_holds_the_key);
    RUN(the_players_keys_survive_their_file_and_the_options);
    RUN(a_schemes_own_key_captures_as_the_scheme_has_it);
    RUN(resurrect_is_on_e_and_shares_it_with_the_camera);
    RUN(a_key_is_lifted_only_where_its_action_would_answer);
    TEST_REPORT();
}
