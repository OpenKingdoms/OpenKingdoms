/*
 * test_view_shake.c -- the view shake on its own.
 */

#include "tak_view_shake.h"
#include <stdio.h>

static int g_failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

/* A shake is a step each frame inside a box that shrinks to nothing
 * over its length, centred on nought (legacy:120568-120594). */
static void test_a_shake_runs_down_inside_its_box(void) {
    ViewShake_Reset();
    int32_t dx = 9, dy = 9;
    ViewShake_Step(&dx, &dy);
    EXPECT(dx == 0 && dy == 0);
    EXPECT(!ViewShake_Active());

    ViewShake_Start(40, 100);
    EXPECT(ViewShake_Active());
    int moved = 0;
    long sum_x = 0;
    for (int f = 0; f < 100; f++) {
        int box = 40 * (100 - f) / 100;
        ViewShake_Step(&dx, &dy);
        EXPECT(dx >= -box / 2 - 1 && dx <= box / 2 + 1);
        EXPECT(dy >= -box / 2 - 1 && dy <= box / 2 + 1);
        if (dx || dy) moved++;
        sum_x += dx;
    }
    EXPECT(moved > 50);
    /* Centred: a hundred steps in a box of forty do not walk off. */
    EXPECT(sum_x > -400 && sum_x < 400);
    EXPECT(!ViewShake_Active());
    ViewShake_Step(&dx, &dy);
    EXPECT(dx == 0 && dy == 0);
}

/* A second shake on one that is running adds its magnitude and
 * averages the lengths (legacy:120454-120470). Nothing starts one of
 * no size or no length. */
static void test_shakes_pile_up_the_way_the_original_piles_them(void) {
    ViewShake_Reset();
    ViewShake_Start(0, 60);
    ViewShake_Start(3, 0);
    EXPECT(!ViewShake_Active());
    ViewShake_Start(10, 100);
    int32_t dx, dy;
    for (int f = 0; f < 40; f++) ViewShake_Step(&dx, &dy);
    ViewShake_Start(10, 20);
    /* Sixty frames from here: (100 + 20) / 2. */
    int frames = 0;
    while (ViewShake_Active() && frames < 1000) { ViewShake_Step(&dx, &dy); frames++; }
    EXPECT(frames == 60);
}

/* The same shake is the same steps, and it draws from no generator but
 * its own. */
static void test_a_shake_repeats(void) {
    int32_t a[20][2], b[20][2];
    ViewShake_Reset();
    ViewShake_Start(30, 20);
    for (int f = 0; f < 20; f++) ViewShake_Step(&a[f][0], &a[f][1]);
    ViewShake_Reset();
    ViewShake_Start(30, 20);
    for (int f = 0; f < 20; f++) ViewShake_Step(&b[f][0], &b[f][1]);
    for (int f = 0; f < 20; f++) EXPECT(a[f][0] == b[f][0] && a[f][1] == b[f][1]);
}

int main(void) {
    test_a_shake_runs_down_inside_its_box();
    test_shakes_pile_up_the_way_the_original_piles_them();
    test_a_shake_repeats();
    if (g_failures == 0) {
        printf("OK  test_view_shake: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL  test_view_shake: %d failure(s)\n", g_failures);
    return 1;
}
