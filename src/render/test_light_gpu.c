/*
 * test_light_gpu.c -- GPU shading and the shade copies an atlas keeps,
 * with a synthetic palette and no game files.
 */

#include "test_framework.h"
#include "tak_tex_atlas.h"
#include "tak_unit.h"

#include <SDL.h>
#include <string.h>

/* A palette of greys and a ramp whose row r brightens every index by r - 15. */
static void synthetic_ramp(uint32_t *rgba, uint8_t *shd) {
    for (int i = 0; i < 256; i++) rgba[i] = 0xFF000000u | (uint32_t)i * 0x010101u;
    for (int l = 0; l < TEX_SHADE_LEVELS; l++)
        for (int i = 0; i < 256; i++) {
            int k = i + (l - TEX_SHADE_IDENTITY) * 4;
            shd[l * 256 + i] = (uint8_t)(k < 0 ? 0 : k > 255 ? 255 : k);
        }
}

/* A mobile unit reaches row 16, a building 21 and a statue 23, so an atlas
 * keeps 2, 7 or 9 copies, and one nothing draws keeps 1. */
TEST(an_atlas_keeps_only_the_rows_its_meshes_reach) {
    ASSERT_EQ_INT(16, Units_LightTop(1, 0));
    ASSERT_EQ_INT(21, Units_LightTop(0, 0));
    ASSERT_EQ_INT(23, Units_LightTop(0, 1));
    ASSERT_EQ_INT(2, TexAtlas_PlanBands(Units_LightTop(1, 0), TEX_SHADE_BANDS));
    ASSERT_EQ_INT(7, TexAtlas_PlanBands(Units_LightTop(0, 0), TEX_SHADE_BANDS));
    ASSERT_EQ_INT(9, TexAtlas_PlanBands(Units_LightTop(0, 1), TEX_SHADE_BANDS));
    ASSERT_EQ_INT(1, TexAtlas_PlanBands(0, TEX_SHADE_BANDS));
    ASSERT_EQ_INT(4, TexAtlas_PlanBands(Units_LightTop(0, 1), 4));
}

/* The GPU draws row 15 + band, the band path the copy its v offset picks:
 * for every level a building reaches they are one row, kept or cut, with one
 * grey. A level past the copies kept takes the last. */
TEST(gpu_and_band_paths_draw_one_row_and_grey) {
    static uint32_t rgba[256];
    static uint8_t shd[TEX_SHADE_LEVELS * 256];
    synthetic_ramp(rgba, shd);
    TexShadeMap full, cut;
    TexAtlas_BuildShadeMap(rgba, shd, TEX_SHADE_BANDS, 1.0f / TEX_SHADE_BANDS, &full);
    cut = full;
    cut.bands = (uint8_t)TexAtlas_PlanBands(Units_LightTop(0, 0), TEX_SHADE_BANDS);
    cut.band_dv = 1.0f / (float)cut.bands;
    for (int l = 0; l <= Units_LightTop(0, 0); l++) {
        const int gpu_row = TEX_SHADE_IDENTITY + full.band[l];
        const int copy = (int)(TexAtlas_ShadeBandV(&cut, l) / cut.band_dv + 0.5f);
        ASSERT_EQ_INT(gpu_row, TEX_SHADE_IDENTITY + copy);
        ASSERT_EQ_INT(full.grey[l], cut.grey[l]);
    }
    const int top = Units_LightTop(0, 1);
    ASSERT_EQ_INT(cut.bands - 1, (int)(TexAtlas_ShadeBandV(&cut, top) / cut.band_dv + 0.5f));
}

/* GpuShading=0 keeps the copies; TAK_GPU_SHADING overrides the setting. */
TEST(gpu_shading_turns_off_by_setting_or_environment) {
    ASSERT_EQ_INT(1, TexAtlas_GpuShadeWanted(NULL, 1));
    ASSERT_EQ_INT(0, TexAtlas_GpuShadeWanted(NULL, 0));
    ASSERT_EQ_INT(0, TexAtlas_GpuShadeWanted("0", 1));
    ASSERT_EQ_INT(1, TexAtlas_GpuShadeWanted("1", 0));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Light on the GPU");
    RUN(an_atlas_keeps_only_the_rows_its_meshes_reach);
    RUN(gpu_and_band_paths_draw_one_row_and_grey);
    RUN(gpu_shading_turns_off_by_setting_or_environment);
    TEST_REPORT();
}
