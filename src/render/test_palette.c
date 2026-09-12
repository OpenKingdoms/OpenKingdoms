#include "test_framework.h"
#include "tak_palette.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include <string.h>

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

/* ── Palette_Load tests ──────────────────────────────────────────────── */

TEST(load_returns_zero_on_success) {
    ensure_vfs();
    Palette pal;
    int result = Palette_Load(&pal, GUIPAL_PATH);
    ASSERT_EQ_INT(0, result);
}

TEST(load_returns_negative_on_missing_file) {
    ensure_vfs();
    Palette pal;
    int result = Palette_Load(&pal, "nonexistent_file.pal");
    ASSERT(result < 0);
}

TEST(load_struct_is_1024_bytes) {
    ASSERT_EQ_INT(1024, (int)sizeof(Palette));
}

TEST(load_entry_is_4_bytes) {
    ASSERT_EQ_INT(4, (int)sizeof(PaletteEntry));
}

/* ── Known color verification ────────────────────────────────────────── */

TEST(index_0_is_black) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    ASSERT_EQ_INT(0x00, pal.entries[0].r);
    ASSERT_EQ_INT(0x00, pal.entries[0].g);
    ASSERT_EQ_INT(0x00, pal.entries[0].b);
}

TEST(index_1_is_dark_red) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    ASSERT_EQ_INT(0x80, pal.entries[1].r);
    ASSERT_EQ_INT(0x00, pal.entries[1].g);
    ASSERT_EQ_INT(0x00, pal.entries[1].b);
}

TEST(index_2_is_dark_green) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    ASSERT_EQ_INT(0x00, pal.entries[2].r);
    ASSERT_EQ_INT(0x80, pal.entries[2].g);
    ASSERT_EQ_INT(0x00, pal.entries[2].b);
}

TEST(padding_bytes_are_zero) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ_INT(0, pal.entries[i].pad);
    }
}

TEST(entries_are_not_all_zero) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    int nonzero = 0;
    for (int i = 0; i < 256; i++) {
        if (pal.entries[i].r || pal.entries[i].g || pal.entries[i].b)
            nonzero++;
    }
    ASSERT(nonzero > 200); /* most palette entries should have color */
}

/* ── Palette_IndexToRGBA tests ──────────────────────────────────────── */

TEST(index_to_rgba_black) {
    Palette pal;
    memset(&pal, 0, sizeof(pal));
    uint32_t rgba = Palette_IndexToRGBA(&pal, 0);
    /* R=0, G=0, B=0, A=0xFF */
    ASSERT_EQ_INT(0x000000FF, (int)rgba);
}

TEST(index_to_rgba_preserves_color) {
    Palette pal;
    memset(&pal, 0, sizeof(pal));
    pal.entries[42].r = 0xAB;
    pal.entries[42].g = 0xCD;
    pal.entries[42].b = 0xEF;
    uint32_t rgba = Palette_IndexToRGBA(&pal, 42);
    ASSERT_EQ_INT((int)0xABCDEFFF, (int)rgba);
}

TEST(index_to_rgba_alpha_is_opaque) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    /* Every entry should have alpha = 0xFF */
    for (int i = 0; i < 256; i++) {
        uint32_t rgba = Palette_IndexToRGBA(&pal, (uint8_t)i);
        ASSERT_EQ_INT(0xFF, (int)(rgba & 0xFF));
    }
}

TEST(index_to_rgba_loaded_guipal_index1) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);
    uint32_t rgba = Palette_IndexToRGBA(&pal, 1);
    /* Index 1 = dark red (0x80, 0x00, 0x00) -> 0x800000FF */
    ASSERT_EQ_INT((int)0x800000FF, (int)rgba);
}

/* ── Palette_BuildRGBATable tests ────────────────────────────────────── */

TEST(build_table_transparent_index_has_zero_alpha) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);

    /* Create a minimal 32-bit RGBA surface to get a pixel format */
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    ASSERT_NOT_NULL(surf);

    uint32_t table[256];
    Palette_BuildRGBATable(&pal, surf->format, table, 9);

    /* Index 9 should be fully transparent */
    uint8_t r, g, b, a;
    SDL_GetRGBA(table[9], surf->format, &r, &g, &b, &a);
    ASSERT_EQ_INT(0, a);

    /* Index 0 should be fully opaque */
    SDL_GetRGBA(table[0], surf->format, &r, &g, &b, &a);
    ASSERT_EQ_INT(255, a);

    SDL_FreeSurface(surf);
    SDL_Quit();
}

TEST(build_table_colors_match_palette) {
    ensure_vfs();
    Palette pal;
    Palette_Load(&pal, GUIPAL_PATH);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    ASSERT_NOT_NULL(surf);

    uint32_t table[256];
    Palette_BuildRGBATable(&pal, surf->format, table, 9);

    /* Spot-check several entries */
    uint8_t r, g, b, a;
    SDL_GetRGBA(table[1], surf->format, &r, &g, &b, &a);
    ASSERT_EQ_INT(0x80, r);
    ASSERT_EQ_INT(0x00, g);
    ASSERT_EQ_INT(0x00, b);
    ASSERT_EQ_INT(255, a);

    SDL_GetRGBA(table[2], surf->format, &r, &g, &b, &a);
    ASSERT_EQ_INT(0x00, r);
    ASSERT_EQ_INT(0x80, g);
    ASSERT_EQ_INT(0x00, b);

    SDL_FreeSurface(surf);
    SDL_Quit();
}

/* ── Palette_LoadPCX tests ────────────────────────────────────────── */

#define SINGLEMACHINE_PCX_PATH "data/anims/singlemachine.pcx"

TEST(load_pcx_returns_zero_on_success) {
    ensure_vfs();
    Palette pal;
    int result = Palette_LoadPCX(&pal, SINGLEMACHINE_PCX_PATH);
    ASSERT_EQ_INT(0, result);
}

TEST(load_pcx_returns_negative_on_missing_file) {
    ensure_vfs();
    Palette pal;
    int result = Palette_LoadPCX(&pal, "nonexistent.pcx");
    ASSERT(result < 0);
}

TEST(load_pcx_transparency_is_magenta) {
    ensure_vfs();
    /* singlemachine.pcx uses index 9 = magenta (255,0,255) as transparency */
    Palette pal;
    Palette_LoadPCX(&pal, SINGLEMACHINE_PCX_PATH);
    ASSERT_EQ_INT(255, pal.entries[9].r);
    ASSERT_EQ_INT(0, pal.entries[9].g);
    ASSERT_EQ_INT(255, pal.entries[9].b);
}

TEST(load_pcx_has_warm_tones) {
    ensure_vfs();
    /* singlemachine sprite palette should have brown/stone colors */
    Palette pal;
    Palette_LoadPCX(&pal, SINGLEMACHINE_PCX_PATH);
    /* Index 75 = warm beige (R=165, G=151, B=122) */
    ASSERT_EQ_INT(165, pal.entries[75].r);
    ASSERT_EQ_INT(151, pal.entries[75].g);
    ASSERT_EQ_INT(122, pal.entries[75].b);
}

TEST(load_pcx_differs_from_guipal) {
    ensure_vfs();
    /* The per-sprite PCX palette should NOT match guipal.pal */
    Palette pal_pcx, pal_pal;
    Palette_LoadPCX(&pal_pcx, SINGLEMACHINE_PCX_PATH);
    Palette_Load(&pal_pal, GUIPAL_PATH);
    /* Index 75: PCX has warm beige, PAL has dark blue -- very different */
    ASSERT(pal_pcx.entries[75].r != pal_pal.entries[75].r);
}

/* ── Palette_LookupForGAF tests ───────────────────────────────────── */

TEST(lookup_singlemachine_returns_null) {
    /* Self-palette GAF: lookup returns NULL (auto-detect finds matching .pcx) */
    const char *result = Palette_LookupForGAF("data/anims/singlemachine.gaf");
    ASSERT_NULL(result);
}

TEST(lookup_mainscreen_returns_null) {
    const char *result = Palette_LookupForGAF("data/anims/mainscreen.gaf");
    ASSERT_NULL(result);
}

TEST(lookup_commongui_returns_guipal) {
    const char *result = Palette_LookupForGAF("data/anims/commongui.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("guipal.pcx", result);
}

TEST(lookup_cursors_returns_cursors) {
    const char *result = Palette_LookupForGAF("data/anims/cursors.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("cursors.pcx", result);
}

TEST(lookup_smoke_returns_fx) {
    const char *result = Palette_LookupForGAF("data/anims/smoke.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("fx.pcx", result);
}

TEST(lookup_colorlogos_returns_colorlogos) {
    const char *result = Palette_LookupForGAF("data/anims/colorlogos.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("colorlogos.pcx", result);
}

TEST(lookup_verhut_returns_veruna_features) {
    const char *result = Palette_LookupForGAF("data/anims/verhut.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("veruna_features.pcx", result);
}

TEST(lookup_araknigh_returns_aramon_features) {
    const char *result = Palette_LookupForGAF("data/anims/araknigh.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("aramon_features.pcx", result);
}

TEST(lookup_unknown_returns_gameart) {
    const char *result = Palette_LookupForGAF("data/anims/somethingelse.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("gameart.pcx", result);
}

TEST(lookup_alt_verhut_returns_ver_textures) {
    const char *result = Palette_LookupForGAFAlt("data/anims/verhut.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("ver_textures.pcx", result);
}

TEST(lookup_alt_araknigh_returns_ara_textures) {
    const char *result = Palette_LookupForGAFAlt("data/anims/araknigh.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("ara_textures.pcx", result);
}

/* Creon's unit textures (textures/cre*.gaf, Iron Plague) decode with its
 * texture palette. Creon's sidedata names ara_textures.pal, and
 * cre_textures.pcx carries the same 256 colours. */
TEST(lookup_alt_creon_texture_returns_cre_textures) {
    const char *result = Palette_LookupForGAFAlt("textures/crebldg1.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("cre_textures.pcx", result);
}

TEST(lookup_alt_commongui_returns_null) {
    /* Non-faction GAF has no alt palette */
    const char *result = Palette_LookupForGAFAlt("data/anims/commongui.gaf");
    ASSERT_NULL(result);
}

TEST(lookup_font_returns_guipal) {
    const char *result = Palette_LookupForGAF("data/anims/font48.gaf");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("guipal.pcx", result);
}

TEST(lookup_case_insensitive) {
    const char *r1 = Palette_LookupForGAF("data/anims/SMOKE.GAF");
    const char *r2 = Palette_LookupForGAF("data/anims/Smoke.gaf");
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ_STR(r1, r2);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    TEST_SUITE("Palette_Load");
    RUN(load_returns_zero_on_success);
    RUN(load_returns_negative_on_missing_file);
    RUN(load_struct_is_1024_bytes);
    RUN(load_entry_is_4_bytes);

    TEST_SUITE("Known Color Verification");
    RUN(index_0_is_black);
    RUN(index_1_is_dark_red);
    RUN(index_2_is_dark_green);
    RUN(padding_bytes_are_zero);
    RUN(entries_are_not_all_zero);

    TEST_SUITE("Palette_IndexToRGBA");
    RUN(index_to_rgba_black);
    RUN(index_to_rgba_preserves_color);
    RUN(index_to_rgba_alpha_is_opaque);
    RUN(index_to_rgba_loaded_guipal_index1);

    TEST_SUITE("Palette_BuildRGBATable");
    RUN(build_table_transparent_index_has_zero_alpha);
    RUN(build_table_colors_match_palette);

    TEST_SUITE("Palette_LoadPCX");
    RUN(load_pcx_returns_zero_on_success);
    RUN(load_pcx_returns_negative_on_missing_file);
    RUN(load_pcx_transparency_is_magenta);
    RUN(load_pcx_has_warm_tones);
    RUN(load_pcx_differs_from_guipal);

    TEST_SUITE("Palette_LookupForGAF");
    RUN(lookup_singlemachine_returns_null);
    RUN(lookup_mainscreen_returns_null);
    RUN(lookup_commongui_returns_guipal);
    RUN(lookup_cursors_returns_cursors);
    RUN(lookup_smoke_returns_fx);
    RUN(lookup_colorlogos_returns_colorlogos);
    RUN(lookup_verhut_returns_veruna_features);
    RUN(lookup_araknigh_returns_aramon_features);
    RUN(lookup_unknown_returns_gameart);
    RUN(lookup_alt_verhut_returns_ver_textures);
    RUN(lookup_alt_araknigh_returns_ara_textures);
    RUN(lookup_alt_creon_texture_returns_cre_textures);
    RUN(lookup_alt_commongui_returns_null);
    RUN(lookup_font_returns_guipal);
    RUN(lookup_case_insensitive);

    if (vfs_ready) VFS_Shutdown();
    tak_mem_shutdown();

    TEST_REPORT();
}
