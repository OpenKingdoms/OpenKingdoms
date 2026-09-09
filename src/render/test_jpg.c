/*
 * test_jpg.c -- M5 verification for JPG_DecodeRGBA.
 *
 * Decodes 5 shipped terrain chunks (from terrain.hpi) through the VFS
 * layer, asserts basic shape (512x512, RGBA pixel variety), and writes
 * each one out as a PPM so you can eyeball them in an image viewer.
 *
 * PPM output lands in `<cwd>/test_output/jpg/<chunk_id>.ppm`. Run the
 * test, then open one of those files to confirm the decoder actually
 * produced a sensible terrain tile rather than garbage.
 */

#include "test_framework.h"
#include "tak_jpg.h"
#include "tak_hpi.h"
#include "tak_memory.h"

#include <SDL.h>       /* for SDL_Init; we don't open a window    */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  define tak_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define tak_mkdir(p) mkdir(p, 0755)
#endif

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define PPM_OUT_DIR "test_output/jpg"

/* 5 known-good chunks lifted from the recon log (§7 of
 * PHASE_B2_TERRAIN.md). All confirmed as 512x512 baseline MJPEG. */
static const char *g_chunks[] = {
    "terrain/000a9eb4.jpg",
    "terrain/001000d8.jpg",
    "terrain/00203818.jpg",
    "terrain/002c5a76.jpg",
    "terrain/00604e2e.jpg",
};

static int g_vfs_ready = 0;

static void ensure_vfs(void) {
    if (g_vfs_ready) return;
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed — PPM tests will be skipped\n");
    }
    /* Best-effort mkdir; succeeds on first call, EEXIST after. */
    tak_mkdir("test_output");
    tak_mkdir(PPM_OUT_DIR);
    g_vfs_ready = 1;
}

/* ── shared decode-and-verify helper ───────────────────────────────── */

static void decode_chunk_and_assert(const char *vfs_path) {
    ensure_vfs();

    /* Read raw JPG bytes through VFS (never fopen for game data). */
    void *jpg_bytes = NULL;
    uint32_t jpg_size = 0;
    ASSERT_EQ_INT(0, VFS_ReadFile(vfs_path, &jpg_bytes, &jpg_size));
    ASSERT(jpg_size > 100);

    /* Decode. */
    uint32_t *pixels = NULL;
    int w = 0, h = 0;
    int rc = JPG_DecodeRGBA((const uint8_t *)jpg_bytes, jpg_size,
                             &pixels, &w, &h);
    tak_free(jpg_bytes);

    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(512, w);
    ASSERT_EQ_INT(512, h);
    ASSERT(pixels != NULL);

    /* Pixel-variety check: an all-zero or all-constant output means the
     * decoder ran but produced nothing useful. Count pixels that differ
     * from pixel[0]; a real terrain tile has tens of thousands. */
    uint32_t ref = pixels[0];
    int diff_count = 0;
    for (int i = 1; i < w * h; i++) {
        if (pixels[i] != ref) {
            diff_count++;
            if (diff_count > 5000) break;  /* early-exit; we have enough */
        }
    }
    ASSERT(diff_count > 5000);

    /* Write PPM for visual verification. Filename = last path
     * component, e.g. "terrain/000a9eb4.jpg" -> "000a9eb4.ppm". */
    const char *name = strrchr(vfs_path, '/');
    name = name ? name + 1 : vfs_path;
    char ppm_path[256];
    snprintf(ppm_path, sizeof(ppm_path), "%s/%.*s.ppm",
             PPM_OUT_DIR, (int)(strlen(name) - 4), name);
    FILE *f = fopen(ppm_path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) {
            /* RGBA32 layout: low byte is R, then G, B, A. Write just
             * R/G/B — PPM doesn't carry alpha. */
            uint32_t p = pixels[i];
            uint8_t rgb[3] = {
                (uint8_t)(p        & 0xFF),
                (uint8_t)((p >> 8) & 0xFF),
                (uint8_t)((p >> 16) & 0xFF),
            };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        printf("(wrote %s) ", ppm_path);
    }

    tak_free(pixels);
}

/* ── per-chunk tests (one PPM each) ─────────────────────────────────── */

TEST(decode_chunk_000a9eb4) { decode_chunk_and_assert(g_chunks[0]); }
TEST(decode_chunk_001000d8) { decode_chunk_and_assert(g_chunks[1]); }
TEST(decode_chunk_00203818) { decode_chunk_and_assert(g_chunks[2]); }
TEST(decode_chunk_002c5a76) { decode_chunk_and_assert(g_chunks[3]); }
TEST(decode_chunk_00604e2e) { decode_chunk_and_assert(g_chunks[4]); }

/* ── negative / edge cases ──────────────────────────────────────────── */

TEST(decode_rejects_null_input) {
    uint32_t *px = NULL;
    int w = 0, h = 0;
    int rc = JPG_DecodeRGBA(NULL, 0, &px, &w, &h);
    ASSERT(rc < 0);
    ASSERT_NULL(px);
}

TEST(decode_rejects_garbage_bytes) {
    /* Not a JPEG — first bytes don't form a valid SOI marker. The
     * decoder should fail gracefully (return < 0, not crash). */
    uint8_t garbage[128];
    memset(garbage, 0xAA, sizeof(garbage));
    uint32_t *px = NULL;
    int w = 0, h = 0;
    int rc = JPG_DecodeRGBA(garbage, sizeof(garbage), &px, &w, &h);
    ASSERT(rc < 0);
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    TEST_SUITE("JPG_DecodeRGBA (M5)");
    RUN(decode_chunk_000a9eb4);
    RUN(decode_chunk_001000d8);
    RUN(decode_chunk_00203818);
    RUN(decode_chunk_002c5a76);
    RUN(decode_chunk_00604e2e);

    TEST_SUITE("JPG_DecodeRGBA negative cases");
    RUN(decode_rejects_null_input);
    RUN(decode_rejects_garbage_bytes);

    if (g_vfs_ready) VFS_Shutdown();
    tak_mem_shutdown();

    TEST_REPORT();
}
