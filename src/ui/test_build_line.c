/*
 * test_build_line.c: the spots a Shift drag with a building armed lays,
 * from the press, the release and the footprint. Headless and data free.
 */

#include "tak_build_line.h"
#include "tak_commands.h"
#include "test_framework.h"

#include <stdlib.h>

static BuildLineCell g_out[BUILD_LINE_MAX];

static int lay(int32_t ax, int32_t ay, int32_t bx, int32_t by, int fx, int fz) {
    return BuildLine_Cells(ax, ay, bx, by, fx, fz, g_out, BUILD_LINE_MAX);
}

TEST(a_drag_back_to_the_press_lays_one) {
    ASSERT_EQ_INT(1, lay(10, 10, 10, 10, 2, 2));
    ASSERT_EQ_INT(10, g_out[0].x);
    ASSERT_EQ_INT(10, g_out[0].y);
}

/* Walls are 2x2 (arawall.fbi): a ten cell drag lays six, end to end. */
TEST(walls_lie_end_to_end_along_the_drag) {
    ASSERT_EQ_INT(6, lay(10, 10, 20, 10, 2, 2));
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ_INT(10 + 2 * i, g_out[i].x);
        ASSERT_EQ_INT(10, g_out[i].y);
    }
}

TEST(the_line_starts_at_the_press_whichever_way_it_runs) {
    ASSERT_EQ_INT(6, lay(20, 10, 10, 10, 2, 2));
    ASSERT_EQ_INT(20, g_out[0].x);
    ASSERT_EQ_INT(10, g_out[5].x);
    ASSERT_EQ_INT(4, lay(10, 30, 10, 24, 2, 2));
    ASSERT_EQ_INT(30, g_out[0].y);
    ASSERT_EQ_INT(24, g_out[3].y);
}

/* The spot nearest the release ends the line: past half a footprint
 * the next one is laid. */
TEST(the_count_rounds_to_the_nearest_footprint) {
    ASSERT_EQ_INT(5, lay(10, 10, 10, 17, 2, 2));
    ASSERT_EQ_INT(18, g_out[4].y);
    ASSERT_EQ_INT(1, lay(0, 0, 1, 0, 4, 4));
    ASSERT_EQ_INT(2, lay(0, 0, 2, 0, 4, 4));
}

/* A footprint that is not square steps by its own side on each axis. */
TEST(each_axis_steps_by_its_own_side) {
    ASSERT_EQ_INT(4, lay(0, 0, 12, 0, 4, 2));
    ASSERT_EQ_INT(12, g_out[3].x);
    ASSERT_EQ_INT(4, lay(0, 0, 0, 6, 4, 2));
    ASSERT_EQ_INT(6, g_out[3].y);
    /* Four cells each way is one footprint across and two down. */
    ASSERT_EQ_INT(3, lay(0, 0, 4, 4, 4, 2));
    ASSERT_EQ_INT(4, g_out[2].x);
    ASSERT_EQ_INT(4, g_out[2].y);
}

TEST(a_diagonal_is_a_stair_of_touching_corners) {
    ASSERT_EQ_INT(6, lay(0, 0, 10, 10, 2, 2));
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ_INT(2 * i, g_out[i].x);
        ASSERT_EQ_INT(2 * i, g_out[i].y);
    }
    ASSERT_EQ_INT(6, lay(0, 0, 10, -3, 2, 2));
    ASSERT_EQ_INT(-3, g_out[5].y);
}

TEST(a_long_drag_stops_at_what_a_builder_holds) {
    ASSERT_EQ_INT(BUILD_LINE_MAX, lay(0, 0, 1000, 0, 2, 2));
    ASSERT_EQ_INT(2 * (BUILD_LINE_MAX - 1), g_out[BUILD_LINE_MAX - 1].x);
    BuildLineCell few[3];
    ASSERT_EQ_INT(3, BuildLine_Cells(0, 0, 1000, 0, 2, 2, few, 3));
    ASSERT_EQ_INT(0, BuildLine_Cells(0, 0, 10, 0, 2, 2, few, 0));
}

/* Over many drags and footprints no two spots overlap and none leaves
 * a gap: each step is one footprint on one axis and at most one on the
 * other. */
TEST(no_spot_overlaps_the_last_or_leaves_a_gap) {
    const int sides[][2] = { {1, 1}, {2, 2}, {3, 3}, {4, 2}, {2, 5}, {7, 3} };
    for (size_t s = 0; s < sizeof sides / sizeof sides[0]; s++) {
        int fx = sides[s][0], fz = sides[s][1];
        for (int bx = -23; bx <= 23; bx += 3) {
            for (int by = -19; by <= 19; by += 2) {
                int n = lay(5, 7, 5 + bx, 7 + by, fx, fz);
                ASSERT(n >= 1);
                ASSERT_EQ_INT(5, g_out[0].x);
                ASSERT_EQ_INT(7, g_out[0].y);
                for (int i = 1; i < n; i++) {
                    int sx = abs(g_out[i].x - g_out[i - 1].x);
                    int sy = abs(g_out[i].y - g_out[i - 1].y);
                    ASSERT((sx == fx && sy <= fz) || (sy == fz && sx <= fx));
                    for (int j = 0; j < i; j++)
                        ASSERT(abs(g_out[i].x - g_out[j].x) >= fx ||
                               abs(g_out[i].y - g_out[j].y) >= fz);
                }
            }
        }
    }
}

/* Shift queues every spot behind the last, first in first out, with no
 * facing bits, as the player sets none. */
TEST(each_spot_is_queued_at_the_default_facing) {
    ASSERT_EQ_INT(TAK_CMD_ARG_QUEUE, BuildLine_OrderArg(0, 0));
}

/* Ctrl+Shift is last in first out, as in the original: each spot goes
 * in front, so the release end is built first, never without end. */
TEST(ctrl_shift_puts_each_spot_in_front) {
    ASSERT_EQ_INT(TAK_CMD_ARG_FRONT, BuildLine_OrderArg(1, 0));
}

/* A summons without end lays its one spot as its click would: queued
 * with Shift, and with Ctrl dropping Shift (legacy:39177-39180). */
TEST(a_summons_sends_what_its_click_would) {
    ASSERT_EQ_INT(TAK_CMD_ARG_ENDLESS | TAK_CMD_ARG_QUEUE, BuildLine_OrderArg(0, 1));
    ASSERT_EQ_INT(TAK_CMD_ARG_ENDLESS, BuildLine_OrderArg(1, 1));
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    TEST_SUITE("The line of buildings a Shift drag lays");
    RUN(a_drag_back_to_the_press_lays_one);
    RUN(walls_lie_end_to_end_along_the_drag);
    RUN(the_line_starts_at_the_press_whichever_way_it_runs);
    RUN(the_count_rounds_to_the_nearest_footprint);
    RUN(each_axis_steps_by_its_own_side);
    RUN(a_diagonal_is_a_stair_of_touching_corners);
    RUN(a_long_drag_stops_at_what_a_builder_holds);
    RUN(no_spot_overlaps_the_last_or_leaves_a_gap);
    RUN(each_spot_is_queued_at_the_default_facing);
    RUN(ctrl_shift_puts_each_spot_in_front);
    RUN(a_summons_sends_what_its_click_would);
    TEST_REPORT();
}
