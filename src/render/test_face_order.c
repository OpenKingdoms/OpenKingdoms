/*
 * test_face_order.c -- the overlap order of a building's faces, with no
 * game data.
 *
 * Synthetic triangles on screen, each with a height per corner. A taller
 * sample at a pixel is the nearer one, so it has to be drawn later. The
 * GL renderers test depth per model instead, keyed as below.
 */

#include "test_framework.h"
#include "tak_unit.h"

#include <SDL.h>
#include <string.h>

/* One card per triangle, all covering the same screen area, each at
 * height `h` with a slight tilt across it. */
static void cards(const float *h, int n, float *xy, float *hv) {
    static const float p[6] = { 0.0f, 0.0f, 60.0f, 0.0f, 0.0f, 60.0f };
    for (int t = 0; t < n; t++) {
        memcpy(xy + 6 * t, p, sizeof(p));
        hv[3 * t + 0] = h[t];
        hv[3 * t + 1] = h[t] + 0.1f;
        hv[3 * t + 2] = h[t] + 0.05f;
    }
}

static int drawn_at(const int *order, int n, int tri) {
    for (int k = 0; k < n; k++)
        if (order[k] == tri) return k;
    return -1;
}

/* The Veruna lodestone: a back card, a logo card 0.3 under the plain card
 * and the plain card authored last. The near card goes on top. */
TEST(a_near_card_draws_over_one_a_fraction_behind) {
    const float h[3] = { 8.4f, 9.7f, 10.0f };
    float xy[18], hv[9];
    int order[3];
    cards(h, 3, xy, hv);
    ASSERT_EQ_INT(3, Units_DebugOverlapOrder(xy, hv, 3, order));
    ASSERT_EQ_INT(0, order[0]);
    ASSERT_EQ_INT(1, order[1]);
    ASSERT_EQ_INT(2, order[2]);
}

/* Two cards at one height: the first authored stays on top, as the
 * original's key test keeps the first drawn (legacy:265317). */
TEST(a_true_tie_keeps_the_first_authored_on_top) {
    const float h[2] = { 10.0f, 10.0f };
    float xy[12], hv[6];
    int order[2];
    cards(h, 2, xy, hv);
    ASSERT_EQ_INT(2, Units_DebugOverlapOrder(xy, hv, 2, order));
    ASSERT(drawn_at(order, 2, 0) > drawn_at(order, 2, 1));
}

/* A card tilted through a flat one, level at the middle and over
 * OV_LEVEL apart at the corners: the taller key goes on top, not the
 * first authored. */
TEST(a_crossing_falls_back_to_the_key_order) {
    static const float p[6] = { 0.0f, 0.0f, 60.0f, 0.0f, 0.0f, 60.0f };
    const float hv[6] = { 10.0f, 10.1f, 10.05f, 4.0f, 16.0f, 10.15f };
    float xy[12];
    int order[2];
    memcpy(xy, p, sizeof(p));
    memcpy(xy + 6, p, sizeof(p));
    ASSERT_EQ_INT(2, Units_DebugOverlapOrder(xy, hv, 2, order));
    ASSERT_EQ_INT(0, order[0]);
    ASSERT_EQ_INT(1, order[1]);
}

/* Per-model depth: a lodestone's cards, 0.2 apart, land on different steps
 * of a 24 bit buffer even in the last slice, and step apart alike in every
 * slice, so a near tie cannot flip with the model's place in the frame. */
TEST(cards_a_fifth_apart_part_alike_in_every_slice) {
    const int slots = Units_DepthSlots(24);
    const float lo0 = Units_DepthKey(0, slots, 10.0f, 0.0f);
    const float hi0 = Units_DepthKey(0, slots, 10.2f, 0.0f);
    const float lo = Units_DepthKey(slots - 1, slots, 10.0f, 0.0f);
    const float hi = Units_DepthKey(slots - 1, slots, 10.2f, 0.0f);
    ASSERT(hi > lo);
    /* The buffer stores 1 - z, so compare whole steps of 2^24 - 1. */
    const double steps = 16777215.0;
    ASSERT((long)((1.0 - (double)lo) * steps + 0.5) - (long)((1.0 - (double)hi) * steps + 0.5) >= 4);
    ASSERT(hi - lo == hi0 - lo0);
}

/* Models keep their draw order: anything of a later model is nearer
 * than anything of an earlier one, however tall. */
TEST(a_later_model_is_nearer_than_all_of_an_earlier_one) {
    const int slots = Units_DepthSlots(24);
    const float top_of_first = Units_DepthKey(0, slots, 5000.0f, 0.0f);
    const float foot_of_next = Units_DepthKey(1, slots, 0.0f, 0.0f);
    ASSERT(foot_of_next > top_of_first);
    ASSERT(Units_DepthKey(slots - 1, slots, 5000.0f, 0.0f) < 1.0f);
    /* The lowest point of the first model is still nearer than a clear. */
    ASSERT(Units_DepthKey(0, slots, 0.0f, 0.0f) > 0.0f);
}

/* A buffer under 16 bits is too coarse to use; 16 bits holds four
 * models between clears and 24 bits a thousand. */
TEST(depth_bits_decide_the_slices) {
    ASSERT_EQ_INT(0, Units_DepthSlots(8));
    ASSERT_EQ_INT(4, Units_DepthSlots(16));
    ASSERT_EQ_INT(1024, Units_DepthSlots(24));
    ASSERT_EQ_INT(1024, Units_DepthSlots(32));
}

/* Two see-through copies in one run: each writes its depth and blends
 * before the next writes depth, or the later one hides the earlier. */
TEST(a_see_through_copy_finishes_before_the_next) {
    ASSERT_EQ_INT(1, Units_DepthGroupSize(5, 1000, 1));
    ASSERT_EQ_INT(5, Units_DepthGroupSize(5, 1000, 0));
    ASSERT_EQ_INT(3, Units_DepthGroupSize(5, 3, 0));
    ASSERT_EQ_INT(1, Units_DepthGroupSize(5, 3, 1));
}

/* The 3D view shuts the GL helper down when a battle ends; the next
 * battle starts it again instead of keeping the sort. */
TEST(depth_comes_back_after_the_gl_helper_shuts_down) {
    ASSERT_EQ_INT(1, Units_DepthNeedsInit(0, 0, 0));
    ASSERT_EQ_INT(0, Units_DepthNeedsInit(1, 1, 1));
    ASSERT_EQ_INT(1, Units_DepthNeedsInit(1, 0, 0));
    ASSERT_EQ_INT(1, Units_DepthNeedsInit(1, 1, 0));
    ASSERT_EQ_INT(0, Units_DepthNeedsInit(-1, 0, 0));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Face order");
    RUN(a_near_card_draws_over_one_a_fraction_behind);
    RUN(a_true_tie_keeps_the_first_authored_on_top);
    RUN(a_crossing_falls_back_to_the_key_order);
    RUN(cards_a_fifth_apart_part_alike_in_every_slice);
    RUN(a_later_model_is_nearer_than_all_of_an_earlier_one);
    RUN(depth_bits_decide_the_slices);
    RUN(a_see_through_copy_finishes_before_the_next);
    RUN(depth_comes_back_after_the_gl_helper_shuts_down);
    TEST_REPORT();
}
