/*
 * test_shadow_pass.c -- which shadows are cast and in which pass, with no
 * game data.
 *
 * Synthetic unit and feature defs. The decisions are the ones the live
 * draw reads (Units_ShadowPass, Units_FeatureCastsShadow).
 */

#include "test_framework.h"
#include "tak_unit.h"
#include "tak_features.h"

#include <SDL.h>
#include <string.h>

/* A model feature casts unless its file says noshadow. */
TEST(a_model_feature_casts_unless_its_file_says_noshadow) {
    FeatureDef fd;
    memset(&fd, 0, sizeof(fd));
    strcpy(fd.object, "araarch_stone");
    ASSERT_EQ_INT(1, Units_FeatureCastsShadow(&fd));
    fd.no_shadow = 1;
    ASSERT_EQ_INT(0, Units_FeatureCastsShadow(&fd));
    /* A sprite feature's shadow is its GAF's, not a silhouette. */
    fd.no_shadow = 0;
    fd.object[0] = '\0';
    ASSERT_EQ_INT(0, Units_FeatureCastsShadow(&fd));
}

/* One draw in a frame: a unit's shadow or its body. */
typedef struct { int unit; int shadow; } Draw;

/* The frame's draws for units already in row order, back row first: the
 * ground pass lays its shadows before any body, and an in-row shadow goes
 * down just before its own body. Returns the count. */
static int frame_draws(const UnitDef *const *defs, int n, Draw *out) {
    int k = 0;
    for (int i = 0; i < n; i++)
        if (Units_ShadowPass(defs[i], 0) == UNITS_SHADOW_GROUND)
            out[k++] = (Draw){ i, 1 };
    for (int i = 0; i < n; i++) {
        if (Units_ShadowPass(defs[i], 0) == UNITS_SHADOW_IN_ROW)
            out[k++] = (Draw){ i, 1 };
        out[k++] = (Draw){ i, 0 };
    }
    return k;
}

static int draw_at(const Draw *d, int n, int unit, int shadow) {
    for (int k = 0; k < n; k++)
        if (d[k].unit == unit && d[k].shadow == shadow) return k;
    return -1;
}

/* A wall carries noshadow but casts as a feature, and two walls one row
 * apart on the diagonal lay the front wall's shadow over the back wall's
 * body. */
TEST(a_walls_shadow_goes_down_over_the_wall_behind) {
    UnitDef wall;
    memset(&wall, 0, sizeof(wall));
    wall.no_shadow = 1;
    wall.is_feature = 1;
    ASSERT_EQ_INT(UNITS_SHADOW_IN_ROW, Units_ShadowPass(&wall, 0));
    ASSERT_EQ_INT(UNITS_SHADOW_NONE, Units_ShadowPass(&wall, 1));

    const UnitDef *row[2] = { &wall, &wall };   /* back, then front */
    Draw d[8];
    const int n = frame_draws(row, 2, d);
    ASSERT(draw_at(d, n, 1, 1) > draw_at(d, n, 0, 0));
    ASSERT(draw_at(d, n, 1, 1) < draw_at(d, n, 1, 0));
}

/* Any other unit keeps its FBI's noshadow and the ground pass. */
TEST(other_units_keep_noshadow_and_the_ground_pass) {
    UnitDef u;
    memset(&u, 0, sizeof(u));
    ASSERT_EQ_INT(UNITS_SHADOW_GROUND, Units_ShadowPass(&u, 0));
    u.no_shadow = 1;
    ASSERT_EQ_INT(UNITS_SHADOW_NONE, Units_ShadowPass(&u, 0));
    u.no_shadow = 0;
    u.floater = 1;
    ASSERT_EQ_INT(UNITS_SHADOW_NONE, Units_ShadowPass(&u, 0));
    ASSERT_EQ_INT(UNITS_SHADOW_NONE, Units_ShadowPass(NULL, 0));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Shadow pass");
    RUN(a_model_feature_casts_unless_its_file_says_noshadow);
    RUN(a_walls_shadow_goes_down_over_the_wall_behind);
    RUN(other_units_keep_noshadow_and_the_ground_pass);
    TEST_REPORT();
}
