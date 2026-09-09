/*
 * test_blit.c — Tests for Blit_RGBA_Opaque (used by the terrain renderer).
 *
 * The opaque variant is specifically contrasted with Blit_RGBA: it MUST
 * copy every source pixel regardless of value, because terrain tiles
 * legitimately contain black pixels (shadows, cave mouths). If these
 * are skipped the destination surface shows through — a classic bug.
 *
 * Also exercises clipping at all four edges, fully-offscreen blits, and
 * NULL / zero-dim safety.
 */

#include "test_framework.h"
#include "tak_blit.h"
#include <SDL.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* 32-bit surface with known bg fill. Uses RGBA32 to match UI_Offscreen. */
static SDL_Surface *make_surface(int w, int h, uint32_t fill) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                     SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    SDL_FillRect(s, NULL, fill);
    return s;
}

static uint32_t get_pixel(SDL_Surface *s, int x, int y) {
    uint8_t *p = (uint8_t *)s->pixels + y * s->pitch + x * 4;
    return *(uint32_t *)p;
}

/* ── Core behavior ────────────────────────────────────────────────────── */

TEST(copies_all_pixels_including_zero) {
    /* THE bug-catcher. If the impl skips px==0 the test fails because
     * the destination retains its pre-fill 0xAAAAAAAA where the source
     * is 0x00000000. For an opaque terrain blit, zeros must land. */
    SDL_Surface *dst = make_surface(8, 8, 0xAAAAAAAA);
    ASSERT(dst != NULL);
    uint32_t src[4 * 4] = {
        0xDEADBEEF, 0xDEADBEEF, 0x00000000, 0xFFFFFFFF,
        0xDEADBEEF, 0x00000000, 0xDEADBEEF, 0x00000000,
        0x00000000, 0xDEADBEEF, 0xFFFFFFFF, 0xDEADBEEF,
        0xFFFFFFFF, 0x00000000, 0xDEADBEEF, 0xDEADBEEF,
    };
    Blit_RGBA_Opaque(dst, 0, 0, src, 4, 4);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint32_t want = src[y * 4 + x];
            uint32_t got  = get_pixel(dst, x, y);
            if (want != got) {
                printf("FAIL at (%d,%d): want 0x%08x, got 0x%08x\n",
                        x, y, want, got);
                ASSERT_EQ_INT((int)want, (int)got);
            }
        }
    }
    SDL_FreeSurface(dst);
}

TEST(overwrites_destination_bg) {
    /* Fill bg with junk; blit solid color over a 4x4 region; verify
     * exactly that region is replaced and neighbors are untouched. */
    SDL_Surface *dst = make_surface(8, 8, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[4 * 4];
    for (int i = 0; i < 16; i++) src[i] = 0xDEADBEEF;

    Blit_RGBA_Opaque(dst, 2, 3, src, 4, 4);

    /* Pixels inside [2..5]x[3..6] are overwritten; everything else is bg. */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int in_blit = (x >= 2 && x <= 5 && y >= 3 && y <= 6);
            uint32_t want = in_blit ? 0xDEADBEEF : 0xCCCCCCCC;
            ASSERT_EQ_INT((int)want, (int)get_pixel(dst, x, y));
        }
    }
    SDL_FreeSurface(dst);
}

/* ── Clipping ─────────────────────────────────────────────────────────── */

TEST(clips_left_edge) {
    /* dst_x = -2 with a 4-wide source: src columns 0-1 fall offscreen,
     * columns 2-3 land at dst (0, 0) and (1, 0). */
    SDL_Surface *dst = make_surface(8, 8, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[4 * 2] = {
        0x01010101, 0x02020202, 0x03030303, 0x04040404,
        0x05050505, 0x06060606, 0x07070707, 0x08080808,
    };
    Blit_RGBA_Opaque(dst, -2, 0, src, 4, 2);

    /* dst col 0 should be src col 2 (0x03, 0x07) */
    ASSERT_EQ_INT((int)0x03030303, (int)get_pixel(dst, 0, 0));
    ASSERT_EQ_INT((int)0x07070707, (int)get_pixel(dst, 0, 1));
    /* dst col 1 should be src col 3 (0x04, 0x08) */
    ASSERT_EQ_INT((int)0x04040404, (int)get_pixel(dst, 1, 0));
    ASSERT_EQ_INT((int)0x08080808, (int)get_pixel(dst, 1, 1));
    /* Untouched */
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 2, 0));
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 7, 7));
    SDL_FreeSurface(dst);
}

TEST(clips_top_edge) {
    /* dst_y = -1 with a 2-tall source: src row 0 falls off, row 1 lands
     * at dst row 0. */
    SDL_Surface *dst = make_surface(8, 8, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[4 * 2] = {
        0x01010101, 0x02020202, 0x03030303, 0x04040404,
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
    };
    Blit_RGBA_Opaque(dst, 0, -1, src, 4, 2);

    ASSERT_EQ_INT((int)0x11111111, (int)get_pixel(dst, 0, 0));
    ASSERT_EQ_INT((int)0x22222222, (int)get_pixel(dst, 1, 0));
    ASSERT_EQ_INT((int)0x33333333, (int)get_pixel(dst, 2, 0));
    ASSERT_EQ_INT((int)0x44444444, (int)get_pixel(dst, 3, 0));
    /* Row 1 of dst should be untouched (bg). */
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 0, 1));
    SDL_FreeSurface(dst);
}

TEST(clips_right_edge) {
    /* Blit 4-wide source at dst_x = dst->w - 2 = 6: cols 0-1 land at
     * dst (6, 7), cols 2-3 clip. */
    SDL_Surface *dst = make_surface(8, 8, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[4] = { 0x01010101, 0x02020202, 0x03030303, 0x04040404 };
    Blit_RGBA_Opaque(dst, 6, 0, src, 4, 1);

    ASSERT_EQ_INT((int)0x01010101, (int)get_pixel(dst, 6, 0));
    ASSERT_EQ_INT((int)0x02020202, (int)get_pixel(dst, 7, 0));
    /* No spillover before the blit region */
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 5, 0));
    SDL_FreeSurface(dst);
}

TEST(clips_bottom_edge) {
    SDL_Surface *dst = make_surface(8, 8, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[2 * 4] = {
        0x11111111, 0x22222222,
        0x33333333, 0x44444444,
        0x55555555, 0x66666666,
        0x77777777, 0x88888888,
    };
    /* Start at dst_y=6: src rows 0-1 land at dst rows 6-7, rows 2-3 clip. */
    Blit_RGBA_Opaque(dst, 0, 6, src, 2, 4);

    ASSERT_EQ_INT((int)0x11111111, (int)get_pixel(dst, 0, 6));
    ASSERT_EQ_INT((int)0x22222222, (int)get_pixel(dst, 1, 6));
    ASSERT_EQ_INT((int)0x33333333, (int)get_pixel(dst, 0, 7));
    ASSERT_EQ_INT((int)0x44444444, (int)get_pixel(dst, 1, 7));
    SDL_FreeSurface(dst);
}

TEST(fully_offscreen_is_no_op) {
    SDL_Surface *dst = make_surface(8, 8, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[4] = { 0x11, 0x22, 0x33, 0x44 };

    Blit_RGBA_Opaque(dst, 10,  0, src, 2, 2);  /* right of dst  */
    Blit_RGBA_Opaque(dst, -5,  0, src, 2, 2);  /* left of dst   */
    Blit_RGBA_Opaque(dst,  0, 10, src, 2, 2);  /* below dst     */
    Blit_RGBA_Opaque(dst,  0, -5, src, 2, 2);  /* above dst     */

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, x, y));
    SDL_FreeSurface(dst);
}

/* ── Safety ───────────────────────────────────────────────────────────── */

TEST(null_dst_is_safe) {
    uint32_t src[1] = { 0x12345678 };
    Blit_RGBA_Opaque(NULL, 0, 0, src, 1, 1);
    ASSERT(1); /* didn't crash */
}

TEST(null_pixels_is_safe) {
    SDL_Surface *dst = make_surface(4, 4, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    Blit_RGBA_Opaque(dst, 0, 0, NULL, 1, 1);
    /* Background unchanged */
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 0, 0));
    SDL_FreeSurface(dst);
}

TEST(zero_width_is_safe) {
    SDL_Surface *dst = make_surface(4, 4, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[1] = { 0xDEADBEEF };
    Blit_RGBA_Opaque(dst, 0, 0, src, 0, 1);
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 0, 0));
    SDL_FreeSurface(dst);
}

TEST(zero_height_is_safe) {
    SDL_Surface *dst = make_surface(4, 4, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[1] = { 0xDEADBEEF };
    Blit_RGBA_Opaque(dst, 0, 0, src, 1, 0);
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 0, 0));
    SDL_FreeSurface(dst);
}

TEST(negative_dims_is_safe) {
    SDL_Surface *dst = make_surface(4, 4, 0xCCCCCCCC);
    ASSERT(dst != NULL);
    uint32_t src[1] = { 0xDEADBEEF };
    Blit_RGBA_Opaque(dst, 0, 0, src, -1, 1);
    Blit_RGBA_Opaque(dst, 0, 0, src, 1, -1);
    ASSERT_EQ_INT((int)0xCCCCCCCC, (int)get_pixel(dst, 0, 0));
    SDL_FreeSurface(dst);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    /* SDL_CreateRGBSurfaceWithFormat works without SDL_Init — the video
     * subsystem isn't needed for pure CPU surfaces. */

    TEST_SUITE("Blit_RGBA_Opaque core");
    RUN(copies_all_pixels_including_zero);
    RUN(overwrites_destination_bg);

    TEST_SUITE("Blit_RGBA_Opaque clipping");
    RUN(clips_left_edge);
    RUN(clips_top_edge);
    RUN(clips_right_edge);
    RUN(clips_bottom_edge);
    RUN(fully_offscreen_is_no_op);

    TEST_SUITE("Blit_RGBA_Opaque safety");
    RUN(null_dst_is_safe);
    RUN(null_pixels_is_safe);
    RUN(zero_width_is_safe);
    RUN(zero_height_is_safe);
    RUN(negative_dims_is_safe);

    TEST_REPORT();
}
