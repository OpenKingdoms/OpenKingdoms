/*
 * test_hud_layout.c -- the battle screen's regions at the Original
 * scale, pinned in numbers.
 *
 * No game data and no SDL. The 1280x600 case is measured off the
 * original running at that size: sidebar panel 128 px wide on the right
 * edge from y 248, minimap 128x128 at the top of the same column, bottom
 * strip 49 px tall from the left edge to the sidebar, and the first
 * unit panel's gauge frames at x 400. The play area is the screen less
 * 128 across and 48 down, its last row under the strip's first
 * (legacy:243513-243518).
 */

#include "test_framework.h"
#include "tak_hud_layout.h"

#include <stdio.h>

#define ASSERT_RECT(r, X, Y, W, H) do {          \
        ASSERT_EQ_INT((X), (r).x);               \
        ASSERT_EQ_INT((Y), (r).y);               \
        ASSERT_EQ_INT((W), (r).w);               \
        ASSERT_EQ_INT((H), (r).h);               \
    } while (0)

static HUD_Layout layout_at(int w, int h) {
    HUD_LayoutSource src;
    HUD_LayoutSourceDefault(&src);
    HUD_Layout lay;
    HUD_LayoutCompute(&src, w, h, &lay);
    return lay;
}

static HUD_Rect place_at(int w, int h, int x, int y, int rw, int rh) {
    HUD_LayoutSource src;
    HUD_LayoutSourceDefault(&src);
    HUD_Layout lay;
    HUD_LayoutCompute(&src, w, h, &lay);
    HUD_Rect r = { x, y, rw, rh };
    return HUD_LayoutPlace(&src, &lay, r);
}

TEST(at_640x480_the_dialog_stays_where_it_is_authored) {
    HUD_Layout l = layout_at(640, 480);
    ASSERT_RECT(l.play,    0,   0, 512, 432);
    ASSERT_RECT(l.sidebar, 512, 128, 128, 352);
    ASSERT_RECT(l.bottom,  0, 431, 512, 49);
    ASSERT_RECT(l.minimap, 512, 0, 128, 128);
    ASSERT_EQ_INT(0, l.info_dx);
    HUD_Rect hp = place_at(640, 480, 115, 461, 97, 2);
    ASSERT_RECT(hp, 115, 461, 97, 2);
}

TEST(at_1024x768_the_play_area_takes_the_extra_room) {
    HUD_Layout l = layout_at(1024, 768);
    ASSERT_RECT(l.play,    0,   0, 896, 720);
    ASSERT_RECT(l.sidebar, 896, 416, 128, 352);
    ASSERT_RECT(l.bottom,  0, 719, 896, 49);
    ASSERT_RECT(l.minimap, 896, 0, 128, 128);
    /* The unit panels centre in the strip: (896 - 453) / 2 = 221. */
    ASSERT_EQ_INT(162, l.info_dx);
}

TEST(at_1280x600_the_layout_matches_the_original) {
    HUD_Layout l = layout_at(1280, 600);
    ASSERT_RECT(l.play,    0,   0, 1152, 552);
    ASSERT_RECT(l.sidebar, 1152, 248, 128, 352);
    ASSERT_RECT(l.bottom,  0, 551, 1152, 49);
    ASSERT_RECT(l.minimap, 1152, 0, 128, 128);
    /* The first panel's gauge frame, authored at 110, 459. */
    HUD_Rect frame = place_at(1280, 600, 110, 459, 106, 6);
    ASSERT_RECT(frame, 400, 579, 106, 6);
    /* The second panel's, authored at 382, 458. */
    frame = place_at(1280, 600, 382, 458, 106, 6);
    ASSERT_RECT(frame, 672, 578, 106, 6);
    /* The end cap keeps the left edge, the crystal ball its corner. */
    HUD_Rect cap = place_at(1280, 600, 0, 432, 61, 41);
    ASSERT_RECT(cap, 0, 552, 61, 41);
    HUD_Rect ball = place_at(1280, 600, 561, 434, 36, 36);
    ASSERT_RECT(ball, 1201, 554, 36, 36);
    HUD_Rect move = place_at(1280, 600, 529, 153, 29, 29);
    ASSERT_RECT(move, 1169, 273, 29, 29);
}

TEST(at_1920x1080_the_hud_keeps_its_size) {
    HUD_Layout l = layout_at(1920, 1080);
    ASSERT_RECT(l.play,    0,    0, 1792, 1032);
    ASSERT_RECT(l.sidebar, 1792, 728, 128, 352);
    ASSERT_RECT(l.bottom,  0, 1031, 1792, 49);
    ASSERT_RECT(l.minimap, 1792, 0, 128, 128);
    ASSERT_EQ_INT(610, l.info_dx);
    /* The build button template is a size, not a place. */
    HUD_Rect cell = place_at(1920, 1080, 0, 0, 64, 48);
    ASSERT_RECT(cell, 0, 0, 64, 48);
}

TEST(original_draws_at_the_window_size_from_640x480_up) {
    int w = 0, h = 0;
    HUD_LayoutCanvasSize(HUD_SCALE_ORIGINAL, 1280, 600, &w, &h);
    ASSERT_EQ_INT(1280, w); ASSERT_EQ_INT(600, h);
    HUD_LayoutCanvasSize(HUD_SCALE_ORIGINAL, 640, 480, &w, &h);
    ASSERT_EQ_INT(640, w); ASSERT_EQ_INT(480, h);
    /* Below the original's smallest screen the dialog fits the window. */
    HUD_LayoutCanvasSize(HUD_SCALE_ORIGINAL, 1280, 400, &w, &h);
    ASSERT_EQ_INT(640, w); ASSERT_EQ_INT(480, h);
    HUD_LayoutCanvasSize(HUD_SCALE_FIT, 1920, 1080, &w, &h);
    ASSERT_EQ_INT(640, w); ASSERT_EQ_INT(480, h);
}

TEST(the_scale_setting_reads_both_ways) {
    ASSERT_EQ_INT(HUD_SCALE_FIT, HUD_ScaleModeFromName("fit"));
    ASSERT_EQ_INT(HUD_SCALE_FIT, HUD_ScaleModeFromName("Fit"));
    ASSERT_EQ_INT(HUD_SCALE_ORIGINAL, HUD_ScaleModeFromName("original"));
    ASSERT_EQ_INT(HUD_SCALE_ORIGINAL, HUD_ScaleModeFromName(""));
    ASSERT_EQ_INT(HUD_SCALE_ORIGINAL, HUD_ScaleModeFromName(NULL));
    ASSERT_EQ_STR("fit", HUD_ScaleModeName(HUD_SCALE_FIT));
    ASSERT_EQ_STR("original", HUD_ScaleModeName(HUD_SCALE_ORIGINAL));
}

/* A player who kept options before the Original scale existed keeps
 * the stretched battle they had. A new install starts on Original. */
TEST(an_existing_options_file_keeps_the_old_scale) {
    ASSERT_EQ_INT(HUD_SCALE_FIT, HUD_ScaleModeForSettings(NULL, 1));
    ASSERT_EQ_INT(HUD_SCALE_FIT, HUD_ScaleModeForSettings("", 1));
    ASSERT_EQ_INT(HUD_SCALE_ORIGINAL, HUD_ScaleModeForSettings(NULL, 0));
    ASSERT_EQ_INT(HUD_SCALE_ORIGINAL, HUD_ScaleModeForSettings("original", 1));
    ASSERT_EQ_INT(HUD_SCALE_FIT, HUD_ScaleModeForSettings("fit", 0));
}

int main(void) {
    TEST_SUITE("HUD layout");
    RUN(at_640x480_the_dialog_stays_where_it_is_authored);
    RUN(at_1024x768_the_play_area_takes_the_extra_room);
    RUN(at_1280x600_the_layout_matches_the_original);
    RUN(at_1920x1080_the_hud_keeps_its_size);
    RUN(original_draws_at_the_window_size_from_640x480_up);
    RUN(the_scale_setting_reads_both_ways);
    RUN(an_existing_options_file_keeps_the_old_scale);
    TEST_REPORT();
}
