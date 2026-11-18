/* test_gpu.c -- the GPU seam's added blend and constant alpha, drawn by
 * SDL's software renderer onto a known background. */

#include "test_framework.h"
#include "tak_gpu.h"
#include "tak_memory.h"

#include <SDL.h>
#include <string.h>

static SDL_Surface *g_surf;

/* A 4x4 target filled with one colour, and a platform drawing into it. */
static int setup(TAK_Platform *p, uint8_t r, uint8_t g, uint8_t b) {
    memset(p, 0, sizeof(*p));
    g_surf = SDL_CreateRGBSurfaceWithFormat(0, 4, 4, 32, SDL_PIXELFORMAT_RGBA32);
    if (!g_surf) return -1;
    SDL_FillRect(g_surf, NULL, SDL_MapRGBA(g_surf->format, r, g, b, 255));
    p->renderer = SDL_CreateSoftwareRenderer(g_surf);
    return p->renderer ? 0 : -1;
}

static void teardown(TAK_Platform *p) {
    SDL_DestroyRenderer(p->renderer);
    SDL_FreeSurface(g_surf);
}

static void pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint32_t px = *(uint32_t *)((uint8_t *)g_surf->pixels + y * g_surf->pitch + x * 4);
    uint8_t a;
    SDL_GetRGBA(px, g_surf->format, r, g, b, &a);
}

/* One texel of RGBA32, R in the low byte, stamped over the whole target. */
static void stamp(TAK_Platform *p, uint32_t texel, GPU_Blend blend,
                  uint8_t mod_r, uint8_t mod_a) {
    GPU_Texture *t = GPU_UploadRGBA(p, &texel, 1, 1);
    ASSERT_NOT_NULL(t);
    GPU_SetTextureBlend(t, blend);
    GPU_SetTextureMod(t, mod_r, 255, 255, mod_a);
    GPU_DrawToWindow(p, t, NULL, NULL);
    SDL_RenderPresent(p->renderer);
    GPU_FreeTexture(p, t);
}

static int near(int got, int want) { return got >= want - 2 && got <= want + 2; }

TEST(add_brightens_by_the_texel_times_its_alpha) {
    TAK_Platform p;
    ASSERT_EQ_INT(0, setup(&p, 100, 100, 100));
    stamp(&p, 0x80000080u, GPU_BLEND_ADD, 255, 255);
    uint8_t r, g, b;
    pixel(2, 2, &r, &g, &b);
    ASSERT(near(r, 164) && near(g, 100) && near(b, 100));
    teardown(&p);
}

TEST(constant_alpha_scales_the_texel_alpha) {
    TAK_Platform p;
    ASSERT_EQ_INT(0, setup(&p, 0, 0, 0));
    stamp(&p, 0xFFFFFFFFu, GPU_BLEND_ALPHA, 255, 64);
    uint8_t r, g, b;
    pixel(3, 3, &r, &g, &b);
    ASSERT(near(r, 64) && near(g, 64) && near(b, 64));
    teardown(&p);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    tak_mem_init();
    TEST_SUITE("GPU blend modes");
    RUN(add_brightens_by_the_texel_times_its_alpha);
    RUN(constant_alpha_scales_the_texel_alpha);
    TEST_REPORT();
}
