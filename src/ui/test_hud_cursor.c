/*
 * test_hud_cursor.c -- the battle view's cursor picked from what lies
 * under the pointer. No game data and no world.
 */

#include "test_framework.h"
#include "tak_hud_cursor.h"
#include "tak_hud.h"

#include <string.h>

static HUDCursorFacts walkers(void) {
    HUDCursorFacts f;
    memset(&f, 0, sizeof f);
    f.own_selected = 1;
    f.own_walker = 1;
    return f;
}

static HUDCursorFacts over_unit(HUDCursorFacts f, int enemy) {
    f.unit = 1;
    f.selectable = 1;
    f.enemy = enemy;
    return f;
}

/* Ground is a move for a selection that can move, and the pointer for
 * none or for one that cannot (legacy:186603). */
TEST(ground_is_a_move_for_a_selection_that_can_move) {
    HUDCursorFacts f = walkers();
    ASSERT_EQ_INT(HUD_CMD_MOVE, HUD_PickCursor(&f));
    f.own_walker = 0;
    ASSERT_EQ_INT(HUD_CUR_NORMAL, HUD_PickCursor(&f));
    memset(&f, 0, sizeof f);
    ASSERT_EQ_INT(HUD_CUR_NORMAL, HUD_PickCursor(&f));
    ASSERT_EQ_INT(HUD_CUR_NORMAL, HUD_PickCursor(NULL));
}

/* An enemy is attacked when a weapon in hand can take it, too far when
 * the armed ones all cannot, and the ground with nobody armed. */
TEST(an_enemy_shows_attack_too_far_or_its_ground) {
    HUDCursorFacts f = over_unit(walkers(), 1);
    f.weapons = 2;
    f.weapons_take = 1;
    ASSERT_EQ_INT(HUD_CMD_ATTACK, HUD_PickCursor(&f));
    f.weapons_take = 0;
    ASSERT_EQ_INT(HUD_CUR_TOOFAR, HUD_PickCursor(&f));
    f.weapons = 0;
    ASSERT_EQ_INT(HUD_CMD_MOVE, HUD_PickCursor(&f));
    /* With nothing of yours selected an enemy is only looked at. */
    f = over_unit((HUDCursorFacts){0}, 1);
    ASSERT_EQ_INT(HUD_CUR_SELECT, HUD_PickCursor(&f));
}

/* A frame is never selected, so with nobody to help it the cursor is
 * its ground's (legacy:186553). */
TEST(a_frame_is_its_ground) {
    HUDCursorFacts f = over_unit(walkers(), 0);
    f.selectable = 0;
    ASSERT_EQ_INT(HUD_CMD_MOVE, HUD_PickCursor(&f));
    f.mend = 1;
    ASSERT_EQ_INT(HUD_CMD_HEAL, HUD_PickCursor(&f));
}

/* A map point gives the field cursor while units of yours are selected,
 * and the plain pointer while a press there only looks (D-037). */
TEST(a_map_point_shows_the_order_a_press_there_gives) {
    HUDCursorFacts f = walkers();
    ASSERT_EQ_INT(HUD_CMD_MOVE, HUD_PickMapCursor(&f));
    f = over_unit(walkers(), 1);
    f.weapons = f.weapons_take = 1;
    ASSERT_EQ_INT(HUD_CMD_ATTACK, HUD_PickMapCursor(&f));
    f = over_unit(walkers(), 0);
    ASSERT_EQ_INT(HUD_CUR_SELECT, HUD_PickMapCursor(&f));
    f.armed = HUD_CMD_GUARD;
    ASSERT_EQ_INT(HUD_CMD_GUARD, HUD_PickMapCursor(&f));
    /* Nothing of yours selected: a press looks, armed or not. */
    f = over_unit((HUDCursorFacts){0}, 1);
    ASSERT_EQ_INT(HUD_CUR_NORMAL, HUD_PickMapCursor(&f));
    f.armed = HUD_CMD_ATTACK;
    ASSERT_EQ_INT(HUD_CUR_NORMAL, HUD_PickMapCursor(&f));
    ASSERT_EQ_INT(HUD_CUR_NORMAL, HUD_PickMapCursor(NULL));
}

/* An armed order shows its own cursor, but revive over a body the sweep
 * raises, too far over an enemy no weapon takes, and the pointer over a
 * unit the transport cannot carry (legacy:186387-186402). */
TEST(an_armed_order_shows_its_cursor_with_three_exceptions) {
    HUDCursorFacts f = walkers();
    f.armed = HUD_CMD_CLEAR;
    ASSERT_EQ_INT(HUD_CMD_CLEAR, HUD_PickCursor(&f));
    f.raise = 1;
    ASSERT_EQ_INT(HUD_CUR_REVIVE, HUD_PickCursor(&f));
    f = over_unit(walkers(), 1);
    f.armed = HUD_CMD_ATTACK;
    f.weapons = 2;
    f.weapons_take = 1;
    ASSERT_EQ_INT(HUD_CMD_ATTACK, HUD_PickCursor(&f));
    f.weapons_take = 0;
    ASSERT_EQ_INT(HUD_CUR_TOOFAR, HUD_PickCursor(&f));
    f = over_unit(walkers(), 0);
    f.armed = HUD_CMD_LOAD;
    ASSERT_EQ_INT(HUD_CMD_LOAD, HUD_PickCursor(&f));
    f.load_refused = 1;
    ASSERT_EQ_INT(HUD_CUR_NORMAL, HUD_PickCursor(&f));
    /* Over the ground Load keeps its cursor. */
    f = walkers();
    f.armed = HUD_CMD_LOAD;
    f.load_refused = 1;
    ASSERT_EQ_INT(HUD_CMD_LOAD, HUD_PickCursor(&f));
}

int main(void) {
    TEST_SUITE("HUD cursor");
    RUN(ground_is_a_move_for_a_selection_that_can_move);
    RUN(an_enemy_shows_attack_too_far_or_its_ground);
    RUN(a_frame_is_its_ground);
    RUN(a_map_point_shows_the_order_a_press_there_gives);
    RUN(an_armed_order_shows_its_cursor_with_three_exceptions);
    TEST_REPORT();
}
