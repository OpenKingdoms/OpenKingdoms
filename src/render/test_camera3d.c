/*
 * test_camera3d.c -- the free camera's arithmetic, pinned in numbers.
 *
 * No game data and no SDL, so it runs on every platform we ship, the
 * browser under node included. The expected values were worked out
 * separately from the code, by hand and in a scripting language, so
 * the camera is checked against something other than itself.
 */

#include "test_framework.h"
#include "tak_camera3d.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

/* A target at (1000, 2000), looking north from the south, tilted to
 * the classic angle, 800 pixels back, 45 degrees tall, 16:9 wide. */
static Camera3D fixture(void) {
    Camera3D c;
    c.target_x = 1000.0f; c.target_y = 0.0f; c.target_z = 2000.0f;
    c.yaw = 0.0f;
    c.pitch = (float)atan(2.0);
    c.dist = 800.0f;
    c.fov_y = 45.0f * 3.14159265f / 180.0f;
    c.aspect = 16.0f / 9.0f;
    c.near_z = 8.0f;
    c.far_z = 20000.0f;
    return c;
}

static float flat_ground(void *ctx, float x, float z) {
    (void)ctx; (void)x; (void)z;
    return 0.0f;
}

/* A 200 pixel step east of x = 1050. */
static float stepped_ground(void *ctx, float x, float z) {
    (void)ctx; (void)z;
    return x >= 1050.0f ? 200.0f : 0.0f;
}

/* A 300 pixel ridge south of z = 2200. */
static float ridge_ground(void *ctx, float x, float z) {
    (void)ctx; (void)x;
    return z > 2200.0f ? 300.0f : 0.0f;
}

static float cliff_ground(void *ctx, float x, float z) {
    (void)ctx; (void)x;
    return z > 2200.0f ? 1000.0f : 0.0f;
}

TEST(the_eye_sits_back_and_up_from_the_target) {
    Camera3D c = fixture();
    float eye[3];
    Camera3D_Eye(&c, eye);
    ASSERT(NEAR(eye[0], 1000.0, 1e-3));
    ASSERT(NEAR(eye[1], 715.5418, 1e-3));
    ASSERT(NEAR(eye[2], 2357.7709, 1e-3));
}

TEST(the_view_matrix_puts_the_target_straight_ahead) {
    Camera3D c = fixture();
    float v[16], out[4];
    Camera3D_ViewMatrix(&c, v);
    const float target[3] = { 1000.0f, 0.0f, 2000.0f };
    Mat4_TransformPoint(v, target, out);
    ASSERT(NEAR(out[0], 0.0, 1e-3));
    ASSERT(NEAR(out[1], 0.0, 1e-3));
    ASSERT(NEAR(out[2], -800.0, 1e-3));
    /* East of the target is to the right, a higher point is up and a
     * little nearer. */
    const float east[3] = { 1100.0f, 0.0f, 2000.0f };
    Mat4_TransformPoint(v, east, out);
    ASSERT(NEAR(out[0], 100.0, 1e-3));
    ASSERT(NEAR(out[1], 0.0, 1e-3));
    ASSERT(NEAR(out[2], -800.0, 1e-3));
    const float high[3] = { 1000.0f, 50.0f, 2000.0f };
    Mat4_TransformPoint(v, high, out);
    ASSERT(NEAR(out[0], 0.0, 1e-3));
    ASSERT(NEAR(out[1], 22.3607, 1e-3));
    ASSERT(NEAR(out[2], -755.2786, 1e-3));
}

TEST(the_projection_is_the_stated_perspective) {
    Camera3D c = fixture();
    float p[16];
    Camera3D_ProjMatrix(&c, p);
    ASSERT(NEAR(p[0],  1.3579951, 1e-5));
    ASSERT(NEAR(p[5],  2.4142136, 1e-5));
    ASSERT(NEAR(p[10], -1.0008003, 1e-5));
    ASSERT(NEAR(p[11], -1.0, 1e-6));
    ASSERT(NEAR(p[14], -16.006403, 1e-4));
    ASSERT(NEAR(p[15], 0.0, 1e-6));
    /* The product places the target at the centre of the screen. */
    float vp[16], out[4];
    Camera3D_ViewProj(&c, vp);
    const float target[3] = { 1000.0f, 0.0f, 2000.0f };
    Mat4_TransformPoint(vp, target, out);
    ASSERT(NEAR(out[0] / out[3], 0.0, 1e-5));
    ASSERT(NEAR(out[1] / out[3], 0.0, 1e-5));
    ASSERT(out[3] > 0.0f);
}

TEST(the_pointer_ray_under_a_pixel_is_pinned) {
    Camera3D c = fixture();
    float o[3], d[3];
    /* The middle of a 1600 by 900 viewport looks straight at the target. */
    Camera3D_PointerRay(&c, 1600, 900, 799.5f, 449.5f, o, d);
    ASSERT(NEAR(o[1], 715.5418, 1e-3));
    ASSERT(NEAR(d[0], 0.0, 1e-5));
    ASSERT(NEAR(d[1], -0.8944272, 1e-5));
    ASSERT(NEAR(d[2], -0.4472136, 1e-5));
    /* The right edge, halfway down. */
    Camera3D_PointerRay(&c, 1600, 900, 1599.5f, 449.5f, o, d);
    ASSERT(NEAR(d[0], 0.5929577, 1e-5));
    ASSERT(NEAR(d[1], -0.7202229, 1e-5));
    ASSERT(NEAR(d[2], -0.3601114, 1e-5));
    /* Up the screen looks further north. */
    Camera3D_PointerRay(&c, 1600, 900, 799.5f, 100.0f, o, d);
    ASSERT(NEAR(d[0], 0.0, 1e-5));
    ASSERT(NEAR(d[1], -0.7144930, 1e-5));
    ASSERT(NEAR(d[2], -0.6996426, 1e-5));
}

TEST(the_ray_lands_on_the_ground_that_is_there) {
    Camera3D c = fixture();
    float o[3], d[3], x = 0.0f, z = 0.0f;
    Camera3D_PointerRay(&c, 1600, 900, 799.5f, 449.5f, o, d);
    ASSERT_EQ_INT(1, Camera3D_RayHitGround(o, d, 20000.0f, flat_ground, NULL, &x, &z));
    ASSERT(NEAR(x, 1000.0, 0.1));
    ASSERT(NEAR(z, 2000.0, 0.1));
    Camera3D_PointerRay(&c, 1600, 900, 1599.5f, 449.5f, o, d);
    ASSERT_EQ_INT(1, Camera3D_RayHitGround(o, d, 20000.0f, flat_ground, NULL, &x, &z));
    ASSERT(NEAR(x, 1589.1037, 0.1));
    ASSERT(NEAR(z, 2000.0, 0.1));
    /* The same ray meets a raised step sooner, on its top. */
    ASSERT_EQ_INT(1, Camera3D_RayHitGround(o, d, 20000.0f, stepped_ground, NULL, &x, &z));
    ASSERT(NEAR(x, 1424.4442, 0.1));
    ASSERT(NEAR(z, 2100.0, 0.1));
    Camera3D_PointerRay(&c, 1600, 900, 799.5f, 100.0f, o, d);
    ASSERT_EQ_INT(1, Camera3D_RayHitGround(o, d, 20000.0f, flat_ground, NULL, &x, &z));
    ASSERT(NEAR(x, 1000.0, 0.1));
    ASSERT(NEAR(z, 1657.1013, 0.1));
    /* A ray that never comes down says so. */
    float up[3] = { 0.0f, 1.0f, 0.0f };
    ASSERT_EQ_INT(0, Camera3D_RayHitGround(o, up, 20000.0f, flat_ground, NULL, &x, &z));
}

TEST(the_classic_preset_spans_the_play_area) {
    /* Filled the way an MSVC Debug build fills the stack, so a field the
     * preset forgets shows up on every platform. */
    Camera3D c;
    memset(&c, 0xCC, sizeof c);
    Camera3D_ClassicPreset(&c, 3000.0f, 4000.0f, 1600, 900);
    ASSERT(NEAR(c.target_y, 0.0, 1e-6));
    ASSERT(NEAR(c.yaw, 0.0, 1e-6));
    ASSERT(NEAR(c.pitch, 1.1071487, 1e-5));
    ASSERT(NEAR(c.dist, 1086.3961, 1e-3));
    /* The pixel at the right edge, level with the target, lands half
     * the play area's width east of it and no further south. */
    float o[3], d[3], x = 0.0f, z = 0.0f;
    Camera3D_PointerRay(&c, 1600, 900, 1599.5f, 449.5f, o, d);
    ASSERT_EQ_INT(1, Camera3D_RayHitGround(o, d, 20000.0f, flat_ground, NULL, &x, &z));
    ASSERT(NEAR(x, 3800.0, 0.5));
    ASSERT(NEAR(z, 4000.0, 0.5));
}

TEST(the_clamp_keeps_the_eye_over_the_ground) {
    Camera3D c = fixture();
    c.pitch = 20.0f * 3.14159265f / 180.0f;
    /* A ridge under the eye tilts the camera up to clear it by twenty. */
    Camera3D_Clamp(&c, 8000.0f, 8000.0f, ridge_ground, NULL);
    ASSERT(NEAR(c.pitch, 0.4115168, 1e-5));
    /* A cliff it cannot clear tilts it to the limit. */
    c.pitch = 20.0f * 3.14159265f / 180.0f;
    Camera3D_Clamp(&c, 8000.0f, 8000.0f, cliff_ground, NULL);
    ASSERT(NEAR(c.pitch, 1.5533430, 1e-5));
    /* Off the map comes back to its edge, and the distance is bounded. */
    c.target_x = -50.0f; c.target_z = 9000.0f; c.dist = 1.0f;
    Camera3D_Clamp(&c, 8000.0f, 8000.0f, flat_ground, NULL);
    ASSERT(NEAR(c.target_x, 0.0, 1e-6));
    ASSERT(NEAR(c.target_z, 8000.0, 1e-6));
    ASSERT(NEAR(c.dist, 48.0, 1e-6));
}

TEST(a_pan_follows_the_cameras_own_directions) {
    Camera3D c = fixture();
    Camera3D_Pan(&c, 100.0f, 0.0f);
    ASSERT(NEAR(c.target_x, 1100.0, 1e-3));
    ASSERT(NEAR(c.target_z, 2000.0, 1e-3));
    Camera3D_Pan(&c, 0.0f, 100.0f);
    ASSERT(NEAR(c.target_z, 1900.0, 1e-3));
    /* Turned to look west, right on the screen is north on the map. */
    c = fixture();
    c.yaw = 3.14159265f * 0.5f;
    Camera3D_Pan(&c, 100.0f, 0.0f);
    ASSERT(NEAR(c.target_x, 1000.0, 1e-3));
    ASSERT(NEAR(c.target_z, 1900.0, 1e-3));
    Camera3D_Pan(&c, 0.0f, 100.0f);
    ASSERT(NEAR(c.target_x, 900.0, 1e-3));
}

TEST(orbit_and_zoom_change_only_their_own_terms) {
    Camera3D c = fixture();
    Camera3D_Orbit(&c, 0.25f, -0.1f);
    ASSERT(NEAR(c.yaw, 0.25, 1e-6));
    ASSERT(NEAR(c.pitch, atan(2.0) - 0.1, 1e-6));
    Camera3D_Zoom(&c, 0.5f);
    ASSERT(NEAR(c.dist, 400.0, 1e-6));
    ASSERT(NEAR(c.target_x, 1000.0, 1e-6));
}

TEST(the_frustum_keeps_what_the_camera_can_see) {
    Camera3D c = fixture();
    float vp[16], planes[6][4];
    Camera3D_ViewProj(&c, vp);
    Camera3D_FrustumPlanes(vp, planes);
    const float target[3] = { 1000.0f, 0.0f, 2000.0f };
    ASSERT_EQ_INT(1, Camera3D_SphereInFrustum(planes, target, 1.0f));
    /* Far to the east is out, but a sphere wide enough to reach the
     * edge of the view is in. */
    const float east[3] = { 4000.0f, 0.0f, 2000.0f };
    ASSERT_EQ_INT(0, Camera3D_SphereInFrustum(planes, east, 10.0f));
    ASSERT_EQ_INT(1, Camera3D_SphereInFrustum(planes, east, 2500.0f));
    /* Behind the eye is out however big. */
    const float behind[3] = { 1000.0f, 800.0f, 3000.0f };
    ASSERT_EQ_INT(0, Camera3D_SphereInFrustum(planes, behind, 100.0f));
}

int main(void) {
    printf("test_camera3d\n");
    RUN(the_frustum_keeps_what_the_camera_can_see);
    RUN(the_eye_sits_back_and_up_from_the_target);
    RUN(the_view_matrix_puts_the_target_straight_ahead);
    RUN(the_projection_is_the_stated_perspective);
    RUN(the_pointer_ray_under_a_pixel_is_pinned);
    RUN(the_ray_lands_on_the_ground_that_is_there);
    RUN(the_classic_preset_spans_the_play_area);
    RUN(the_clamp_keeps_the_eye_over_the_ground);
    RUN(a_pan_follows_the_cameras_own_directions);
    RUN(orbit_and_zoom_change_only_their_own_terms);
    TEST_REPORT();
}
