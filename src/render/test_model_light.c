/* test_model_light.c -- model faces lit through a synthetic .shd ramp, the
 * texture key and the pictures a texture plays, with no game data. */

#include "test_framework.h"
#include "tak_palette.h"
#include "tak_tex_atlas.h"
#include "tak_gaf.h"
#include "tak_unit.h"

#include <SDL.h>
#include <stdint.h>
#include <string.h>

static uint32_t g_pal[256];
static uint8_t  g_shd[TEX_SHADE_LEVELS * 256];

/* Index i is grey i, opaque as the atlas keeps it. Row l takes index i to
 * i * (l + 1) / 16, held inside the palette. */
static void make_ramp(void) {
    for (int i = 0; i < 256; i++) {
        const uint32_t g = (uint32_t)i;
        g_pal[i] = 0xFF000000u | g | (g << 8) | (g << 16);
    }
    for (int l = 0; l < TEX_SHADE_LEVELS; l++)
        for (int i = 0; i < 256; i++) {
            int v = i * (l + 1) / 16;
            g_shd[l * 256 + i] = (uint8_t)(v > 255 ? 255 : v);
        }
}

/* A lit face samples the copy at its own row, and past the top copy a
 * level holds at it. */
TEST(a_lit_level_samples_its_own_row) {
    make_ramp();
    TexShadeMap m;
    TexAtlas_BuildShadeMap(g_pal, g_shd, TEX_SHADE_BANDS, 0.1f, &m);
    for (int l = TEX_SHADE_IDENTITY + 1; l <= TEX_SHADE_TOP; l++) {
        ASSERT_EQ_INT(l - TEX_SHADE_IDENTITY, m.band[l]);
        ASSERT_EQ_INT(255, m.grey[l]);
    }
    for (int l = TEX_SHADE_TOP + 1; l < TEX_SHADE_LEVELS; l++)
        ASSERT_EQ_INT(TEX_SHADE_BANDS - 1, m.band[l]);
    ASSERT(m.band_dv == 0.1f);
}

/* An entry and its frame table as a GAF lays them out, each picture's
 * hold in the second word. */
static uint8_t g_gaf_bytes[16 + 40 + 10 * 8];

static GAFFile make_gaf(const uint32_t *holds, int pics) {
    memset(g_gaf_bytes, 0, sizeof(g_gaf_bytes));
    g_gaf_bytes[16] = (uint8_t)pics;
    for (int k = 0; k < pics; k++)
        memcpy(g_gaf_bytes + 16 + 40 + 8 * k + 4, &holds[k], 4);
    GAFFile g = { g_gaf_bytes, (uint32_t)(16 + 40 + 8 * pics), 1 };
    return g;
}

/* Ten pictures of 10 frames each: a picture every 20 ticks and a 200 tick
 * loop, as in the original. */
TEST(a_ten_picture_texture_steps_every_ten_frames) {
    uint32_t holds[10];
    uint16_t hold[10];
    for (int k = 0; k < 10; k++) holds[k] = 10;
    GAFFile g = make_gaf(holds, 10);
    for (int k = 0; k < 10; k++) hold[k] = (uint16_t)GAF_FrameHold(&g, 16, k);
    ASSERT_EQ_INT(0, TexAtlas_IsTeamColour("textures/aradevic.gaf", 10));
    static const uint32_t tick[] = { 0, 19, 20, 39, 40, 199, 200, 219, 220, 1999 };
    static const int want[]      = { 0, 0,  1,  1,  2,  9,   0,   0,   1,   9 };
    for (int i = 0; i < 10; i++)
        ASSERT_EQ_INT(want[i], GAF_LoopPicture(hold, 10, tick[i] / 2u));
}

/* Untextured faces whose ramp writes the level into red: six on the root
 * facing +x, -z, -x, +z, up and down, and one on the child facing +x. */
enum { F_PX, F_NZ, F_NX, F_PZ, F_UP, F_DOWN, F_CHILD, F_COUNT };
static UnitMesh     g_mesh;
static UnitMeshFace g_faces[F_COUNT];
static float        g_pos[F_COUNT * 3 * 3];
static uint32_t     g_col[F_COUNT * 3];
static uint16_t     g_node_of[F_COUNT * 3];
static uint32_t     g_level_ramp[TEX_SHADE_LEVELS * 256];
static CobPiece     g_pieces[2];
static uint32_t     g_out[F_COUNT * 3];

static void make_mesh(void) {
    static const float n[F_COUNT][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 0, 1 },
                                         { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 } };
    memset(&g_mesh, 0, sizeof(g_mesh));
    memset(g_pieces, 0, sizeof(g_pieces));
    for (int l = 0; l < TEX_SHADE_LEVELS; l++)
        for (int i = 0; i < 256; i++) g_level_ramp[l * 256 + i] = 0xFF000000u | (uint32_t)l;
    for (int f = 0; f < F_COUNT; f++) {
        memcpy(g_faces[f].normal, n[f], sizeof(n[f]));
        g_faces[f].first_vert = (uint16_t)(f * 3);
        g_faces[f].vert_count = 3;
        g_faces[f].node = (uint16_t)(f == F_CHILD);
        g_faces[f].batch = 0;
        g_faces[f].flat_idx = 1;
        for (int k = 0; k < 3; k++) {
            g_col[f * 3 + k] = 0xFF000000u;
            g_node_of[f * 3 + k] = (uint16_t)(f == F_CHILD);
        }
    }
    g_mesh.positions = g_pos;
    g_mesh.colors = g_col;
    g_mesh.vert_node_idx = g_node_of;
    g_mesh.vert_count = F_COUNT * 3;
    g_mesh.faces = g_faces;
    g_mesh.face_count = F_COUNT;
    g_mesh.flat_ramp = g_level_ramp;
    g_mesh.batch_count = 1;
    g_mesh.node_count = 2;
    g_mesh.nodes[0].parent = -1;
    g_mesh.nodes[1].parent = 0;
}

/* The level each face of `def`'s model drew at, posed so. */
static void levels_of(const UnitDef *def, float heading, float pitch, float roll,
                      int out[F_COUNT]) {
    ASSERT_EQ_INT(0, Units_DebugLightMesh(&g_mesh, def, g_pieces, 2, heading, pitch, roll, g_out));
    for (int f = 0; f < F_COUNT; f++) out[f] = (int)(g_out[f * 3] & 0xFFu);
}

/* A building takes the light and a mobile unit 0.15 of it, a level or so
 * either side of 15, as in the original. */
TEST(a_building_is_lit_and_a_mobile_unit_a_little) {
    static UnitDef building, walker;
    walker.bmcode = 1;
    make_mesh();
    int lb[F_COUNT], lw[F_COUNT];
    levels_of(&building, 0.0f, 0.0f, 0.0f, lb);
    ASSERT(lb[F_UP] > TEX_SHADE_IDENTITY);
    ASSERT(lb[F_DOWN] < TEX_SHADE_IDENTITY);
    levels_of(&walker, 0.0f, 0.0f, 0.0f, lw);
    int differ = 0;
    for (int f = 0; f < F_COUNT; f++) {
        ASSERT(lw[f] >= TEX_SHADE_IDENTITY - 2 && lw[f] <= TEX_SHADE_IDENTITY + 2);
        if (lw[f] != TEX_SHADE_IDENTITY) differ = 1;
    }
    ASSERT(differ);
    ASSERT(lw[F_UP] >= lw[F_DOWN]);
}

/* Index 0 (ARACASTL's insignias) and 9 (the Blue lodestone ring) are art:
 * only magenta cuts a texture. */
TEST(only_magenta_keys_a_texture) {
    uint32_t src[256], rgba[256];
    uint8_t magenta[256];
    for (int i = 0; i < 256; i++) src[i] = 0xFF000000u | (uint32_t)i << 8;
    src[0] = 0u;              /* black, cleared as the table builder leaves it */
    src[5] = 0xFF800080u;     /* (128, 0, 128) */
    Palette_KeyTextures(src, rgba, magenta);
    ASSERT_EQ_INT(0xFF, (int)(rgba[0] >> 24));
    ASSERT_EQ_INT(0, (int)(rgba[0] & 0x00FFFFFFu));
    ASSERT_EQ_INT(1, magenta[5]);
    ASSERT_EQ_INT(0, magenta[0] + magenta[9] + magenta[200]);

    static const uint8_t art[4] = { 0, 5, 9, 200 };
    uint16_t tx[4];
    for (int k = 0; k < 4; k++) tx[k] = Palette_KeyTexel(art[k], magenta);
    ASSERT_EQ_INT(0, tx[0]);
    ASSERT_EQ_INT((int)PALETTE_TEXEL_CLEAR, tx[1]);
    ASSERT_EQ_INT(9, tx[2]);
    ASSERT_EQ_INT(200, tx[3]);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Model light");
    RUN(a_lit_level_samples_its_own_row);
    RUN(a_ten_picture_texture_steps_every_ten_frames);
    RUN(a_building_is_lit_and_a_mobile_unit_a_little);
    RUN(only_magenta_keys_a_texture);
    TEST_REPORT();
}
