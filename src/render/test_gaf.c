/*
 * test_gaf.c -- Tests for the GAF sprite loader.
 *
 * Uses real game data via VFS for integration tests against
 * mainscreen.gaf and singlemachine.gaf (main menu assets).
 */

#include "test_framework.h"
#include "tak_gaf.h"
#include "tak_hpi.h"
#include "tak_palette.h"
#include "tak_memory.h"
#include <string.h>
#include <SDL.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define GUIPAL_PATH "data/palettes/guipal.pal"

static int vfs_ready = 0;

static void ensure_vfs(void) {
    if (!vfs_ready) {
        tak_mem_init();
        VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR);
        vfs_ready = 1;
    }
}

/* ── GAF_Open tests ──────────────────────────────────────────────── */

TEST(open_mainscreen_succeeds) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    int result = GAF_Open(&gaf, "data/anims/mainscreen.gaf");
    ASSERT_EQ_INT(0, result);
    ASSERT_NOT_NULL(gaf);
    ASSERT(gaf->num_entries > 0);
    GAF_Close(gaf);
}

TEST(open_singlemachine_succeeds) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    int result = GAF_Open(&gaf, "data/anims/singlemachine.gaf");
    ASSERT_EQ_INT(0, result);
    ASSERT_NOT_NULL(gaf);
    ASSERT_EQ_INT(8, (int)gaf->num_entries);
    GAF_Close(gaf);
}

TEST(open_nonexistent_fails) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    int result = GAF_Open(&gaf, "data/anims/nonexistent.gaf");
    ASSERT(result != 0);
}

/* ── GAF_FindSequence tests ──────────────────────────────────────── */

TEST(find_mainbg_sequence) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/mainscreen.gaf");
    ASSERT_NOT_NULL(gaf);

    int offset = GAF_FindSequence(gaf, "MainBG");
    ASSERT(offset >= 0);

    GAF_Close(gaf);
}

TEST(find_exitbutton_sequence) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/mainscreen.gaf");
    ASSERT_NOT_NULL(gaf);

    int offset = GAF_FindSequence(gaf, "ExitButton");
    ASSERT(offset >= 0);

    GAF_Close(gaf);
}

TEST(find_singlemachine0_sequence) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/singlemachine.gaf");
    ASSERT_NOT_NULL(gaf);

    int offset = GAF_FindSequence(gaf, "SingleMachine0");
    ASSERT(offset >= 0);

    GAF_Close(gaf);
}

TEST(find_nonexistent_sequence_returns_negative) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/mainscreen.gaf");
    ASSERT_NOT_NULL(gaf);

    int offset = GAF_FindSequence(gaf, "DoesNotExist");
    ASSERT(offset < 0);

    GAF_Close(gaf);
}

/* ── GAF_GetFrameInfo tests ──────────────────────────────────────── */

TEST(get_mainbg_frame0_dimensions) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/mainscreen.gaf");

    int entry_off = GAF_FindSequence(gaf, "MainBG");
    ASSERT(entry_off >= 0);

    FrameHeader *frame = NULL;
    int result = GAF_GetFrameInfo(gaf, entry_off, 0, &frame);
    ASSERT_EQ_INT(0, result);
    ASSERT_NOT_NULL(frame);

    /* MainBG is the 640x480 background image */
    ASSERT_EQ_INT(640, (int)frame->width);
    ASSERT_EQ_INT(480, (int)frame->height);
    ASSERT_EQ_INT(9, (int)frame->transparency_index);
    ASSERT_EQ_INT(1, (int)frame->compressed);

    GAF_Close(gaf);
}

TEST(get_singlemachine0_frame0_dimensions) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/singlemachine.gaf");

    int entry_off = GAF_FindSequence(gaf, "SingleMachine0");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);
    ASSERT_NOT_NULL(frame);

    /* SingleMachine0 is the skirmish mage character */
    ASSERT_EQ_INT(143, (int)frame->width);
    ASSERT_EQ_INT(182, (int)frame->height);
    ASSERT_EQ_INT(9, (int)frame->transparency_index);

    GAF_Close(gaf);
}

TEST(get_frame_out_of_range_fails) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/mainscreen.gaf");

    int entry_off = GAF_FindSequence(gaf, "MainBG");
    FrameHeader *frame = NULL;
    int result = GAF_GetFrameInfo(gaf, entry_off, 999, &frame);
    ASSERT(result != 0);

    GAF_Close(gaf);
}

/* ── GAF_DecodeFrame tests ───────────────────────────────────────── */

TEST(decode_singlemachine0_produces_correct_size) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/singlemachine.gaf");

    int entry_off = GAF_FindSequence(gaf, "SingleMachine0");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint8_t *pixels = GAF_DecodeFrame(gaf, frame);
    ASSERT_NOT_NULL(pixels);

    /* Buffer should be width * height bytes */
    /* Spot check: transparent pixels at corners (character is centered) */
    ASSERT_EQ_INT(9, (int)pixels[0]); /* top-left should be transparent */
    ASSERT_EQ_INT(9, (int)pixels[frame->width - 1]); /* top-right */

    tak_free(pixels);
    GAF_Close(gaf);
}

TEST(decode_mainbg_has_no_fully_transparent_first_row) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/mainscreen.gaf");

    int entry_off = GAF_FindSequence(gaf, "MainBG");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint8_t *pixels = GAF_DecodeFrame(gaf, frame);
    ASSERT_NOT_NULL(pixels);

    /* MainBG is a full background — first row should have non-transparent pixels */
    int has_color = 0;
    for (int x = 0; x < frame->width; x++) {
        if (pixels[x] != frame->transparency_index) {
            has_color = 1;
            break;
        }
    }
    ASSERT(has_color);

    tak_free(pixels);
    GAF_Close(gaf);
}

TEST(decode_singlemachine0_has_visible_pixels_in_middle) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/singlemachine.gaf");

    int entry_off = GAF_FindSequence(gaf, "SingleMachine0");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint8_t *pixels = GAF_DecodeFrame(gaf, frame);
    ASSERT_NOT_NULL(pixels);

    /* The character sprite should have visible pixels somewhere in the center rows */
    int mid_row = frame->height / 2;
    int has_visible = 0;
    for (int x = 0; x < frame->width; x++) {
        if (pixels[mid_row * frame->width + x] != frame->transparency_index) {
            has_visible = 1;
            break;
        }
    }
    ASSERT(has_visible);

    tak_free(pixels);
    GAF_Close(gaf);
}

/* ── GAF_DecodeFrameRGBA tests ───────────────────────────────────── */

TEST(decode_rgba_produces_valid_pixels) {
    ensure_vfs();
    SDL_Init(SDL_INIT_VIDEO);

    /* Load palette and build RGBA lookup table */
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA32);
    ASSERT_NOT_NULL(tmp);
    uint32_t rgba_table[256];
    Palette_BuildRGBATable(&pal, tmp->format, rgba_table, 9);

    /* Decode a frame to RGBA */
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/singlemachine.gaf");
    int entry_off = GAF_FindSequence(gaf, "SingleMachine0");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint32_t *rgba = GAF_DecodeFrameRGBA(gaf, frame, rgba_table);
    ASSERT_NOT_NULL(rgba);

    /* Top-left corner should be transparent (alpha = 0) */
    uint8_t r, g, b, a;
    SDL_GetRGBA(rgba[0], tmp->format, &r, &g, &b, &a);
    ASSERT_EQ_INT(0, a);

    /* Middle of sprite should have opaque pixels */
    int mid = (frame->height / 2) * frame->width + (frame->width / 2);
    SDL_GetRGBA(rgba[mid], tmp->format, &r, &g, &b, &a);
    ASSERT_EQ_INT(255, a);

    tak_free(rgba);
    GAF_Close(gaf);
    SDL_FreeSurface(tmp);
    SDL_Quit();
}

/* ── TAF tests ───────────────────────────────────────────────────── */

TEST(taf_open_1555_succeeds) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    int result = GAF_Open(&gaf, "data/anims/cannblg_1555.taf");
    ASSERT_EQ_INT(0, result);
    ASSERT_NOT_NULL(gaf);
    ASSERT(gaf->num_entries > 0);
    GAF_Close(gaf);
}

TEST(taf_open_4444_succeeds) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    int result = GAF_Open(&gaf, "data/anims/cannblg_4444.taf");
    ASSERT_EQ_INT(0, result);
    ASSERT_NOT_NULL(gaf);
    GAF_Close(gaf);
}

TEST(taf_1555_frame_dimensions) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/cannblg_1555.taf");
    int entry_off = GAF_FindSequence(gaf, "cannblg");
    ASSERT(entry_off >= 0);

    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ_INT(13, (int)frame->width);
    ASSERT_EQ_INT(12, (int)frame->height);
    ASSERT_EQ_INT(TAF_FORMAT_1555, (int)frame->compressed);

    GAF_Close(gaf);
}

TEST(taf_4444_compressed_field) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/cannblg_4444.taf");
    int entry_off = GAF_FindSequence(gaf, "cannblg");

    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ_INT(TAF_FORMAT_4444, (int)frame->compressed);

    GAF_Close(gaf);
}

TEST(taf_1555_decode_produces_pixels) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/cannblg_1555.taf");
    int entry_off = GAF_FindSequence(gaf, "cannblg");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint32_t *rgba = TAF_DecodeFrameRGBA(gaf, frame);
    ASSERT_NOT_NULL(rgba);

    /* Should have some non-zero pixels (it's a cannonball sprite) */
    int has_color = 0;
    int total = frame->width * frame->height;
    for (int i = 0; i < total; i++) {
        if (rgba[i] != 0) { has_color = 1; break; }
    }
    ASSERT(has_color);

    tak_free(rgba);
    GAF_Close(gaf);
}

TEST(taf_4444_decode_produces_pixels) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/cannblg_4444.taf");
    int entry_off = GAF_FindSequence(gaf, "cannblg");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint32_t *rgba = TAF_DecodeFrameRGBA(gaf, frame);
    ASSERT_NOT_NULL(rgba);

    int has_color = 0;
    int total = frame->width * frame->height;
    for (int i = 0; i < total; i++) {
        if (rgba[i] != 0) { has_color = 1; break; }
    }
    ASSERT(has_color);

    tak_free(rgba);
    GAF_Close(gaf);
}

TEST(taf_transparent_pixels_have_zero_alpha) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/cannblg_1555.taf");
    int entry_off = GAF_FindSequence(gaf, "cannblg");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint32_t *rgba = TAF_DecodeFrameRGBA(gaf, frame);
    ASSERT_NOT_NULL(rgba);

    /* Top-left corner of cannonball sprite should be transparent */
    uint8_t a = (rgba[0] >> 24) & 0xFF;
    ASSERT_EQ_INT(0, a);

    tak_free(rgba);
    GAF_Close(gaf);
}

TEST(taf_colored_pixels_have_nonzero_alpha) {
    ensure_vfs();
    GAFFile *gaf = NULL;
    GAF_Open(&gaf, "data/anims/cannblg_1555.taf");
    int entry_off = GAF_FindSequence(gaf, "cannblg");
    FrameHeader *frame = NULL;
    GAF_GetFrameInfo(gaf, entry_off, 0, &frame);

    uint32_t *rgba = TAF_DecodeFrameRGBA(gaf, frame);
    ASSERT_NOT_NULL(rgba);

    /* Center pixel should be opaque (it's a cannonball) */
    int mid = (frame->height / 2) * frame->width + (frame->width / 2);
    uint8_t a = (rgba[mid] >> 24) & 0xFF;
    ASSERT(a > 0);

    tak_free(rgba);
    GAF_Close(gaf);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    TEST_SUITE("GAF_Open");
    RUN(open_mainscreen_succeeds);
    RUN(open_singlemachine_succeeds);
    RUN(open_nonexistent_fails);

    TEST_SUITE("GAF_FindSequence");
    RUN(find_mainbg_sequence);
    RUN(find_exitbutton_sequence);
    RUN(find_singlemachine0_sequence);
    RUN(find_nonexistent_sequence_returns_negative);

    TEST_SUITE("GAF_GetFrameInfo");
    RUN(get_mainbg_frame0_dimensions);
    RUN(get_singlemachine0_frame0_dimensions);
    RUN(get_frame_out_of_range_fails);

    TEST_SUITE("GAF_DecodeFrame");
    RUN(decode_singlemachine0_produces_correct_size);
    RUN(decode_mainbg_has_no_fully_transparent_first_row);
    RUN(decode_singlemachine0_has_visible_pixels_in_middle);

    TEST_SUITE("GAF_DecodeFrameRGBA");
    RUN(decode_rgba_produces_valid_pixels);

    TEST_SUITE("TAF_DecodeFrameRGBA");
    RUN(taf_open_1555_succeeds);
    RUN(taf_open_4444_succeeds);
    RUN(taf_1555_frame_dimensions);
    RUN(taf_4444_compressed_field);
    RUN(taf_1555_decode_produces_pixels);
    RUN(taf_4444_decode_produces_pixels);
    RUN(taf_transparent_pixels_have_zero_alpha);
    RUN(taf_colored_pixels_have_nonzero_alpha);

    if (vfs_ready) VFS_Shutdown();
    tak_mem_shutdown();

    TEST_REPORT();
}
