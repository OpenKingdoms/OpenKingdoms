/*
 * test_hud_info.c -- what the selected-unit panel describes, picked from
 * the pointer and the selection. No game data and no world.
 */

#include "test_framework.h"
#include "tak_hud_info.h"

TEST(a_hovered_unit_comes_first) {
    int sel[1] = { 7 };
    HUDInfoPick p = HUD_PickInfo(3, 5, sel, 1);
    ASSERT_EQ_INT(HUD_INFO_UNIT, p.kind);
    ASSERT_EQ_INT(3, p.index);
}

TEST(a_hovered_feature_comes_before_the_selection) {
    int sel[1] = { 7 };
    HUDInfoPick p = HUD_PickInfo(-1, 5, sel, 1);
    ASSERT_EQ_INT(HUD_INFO_FEATURE, p.kind);
    ASSERT_EQ_INT(5, p.index);
}

TEST(several_selected_or_none_show_nothing) {
    int sel[2] = { 7, 8 };
    HUDInfoPick p = HUD_PickInfo(-1, -1, sel, 2);
    ASSERT_EQ_INT(HUD_INFO_NONE, p.kind);
    ASSERT_EQ_INT(-1, p.index);
    p = HUD_PickInfo(-1, -1, sel, 0);
    ASSERT_EQ_INT(HUD_INFO_NONE, p.kind);
    p = HUD_PickInfo(-1, -1, NULL, 1);
    ASSERT_EQ_INT(HUD_INFO_NONE, p.kind);
}

TEST(a_hover_still_shows_over_a_group) {
    int sel[2] = { 7, 8 };
    HUDInfoPick p = HUD_PickInfo(-1, 2, sel, 2);
    ASSERT_EQ_INT(HUD_INFO_FEATURE, p.kind);
    ASSERT_EQ_INT(2, p.index);
}

int main(void) {
    TEST_SUITE("HUD info");
    RUN(a_hovered_unit_comes_first);
    RUN(a_hovered_feature_comes_before_the_selection);
    RUN(several_selected_or_none_show_nothing);
    RUN(a_hover_still_shows_over_a_group);
    TEST_REPORT();
}
