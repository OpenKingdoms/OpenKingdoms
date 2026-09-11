/*
 * test_hpi_vfs.c -- Tests for the VFS layer over HPI archives + loose files.
 *
 * Uses real game data at TAK_GAME_DIR for integration tests.
 * Also tests loose file fallback by creating temporary files on disk.
 */

#include "test_framework.h"
#include "test_hpi_builder.h"
#include "tak_hpi.h"
#include "tak_util.h"
#include "tak_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>  /* _mkdir on Windows */
#else
#include <unistd.h>  /* rmdir, unlink */
#include <sys/stat.h>
#endif

/* Portable single-dir mkdir helper for tests. Ignores EEXIST failures. */
static int tak_test_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/* Portable rmdir shim: rmdir on POSIX, _rmdir on Windows. */
static int tak_test_rmdir(const char *path) {
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════ */

static int game_dir_exists(void) {
    struct stat st;
    return stat(TAK_GAME_DIR, &st) == 0;
}

/* Ensure VFS is shut down before each test that needs a fresh state */
static void ensure_clean_vfs(void) {
    VFS_Shutdown();
}

static const char *LOOSE_DIR = "test_vfs_loose_tmp";
static const char *LOOSE_FILE_RELATIVE = "testfile.txt";
static const char *LOOSE_FILE_CONTENT = "hello from loose dir";

static void create_loose_dir(void) {
    tak_test_mkdir(LOOSE_DIR);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LOOSE_DIR, LOOSE_FILE_RELATIVE);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(LOOSE_FILE_CONTENT, 1, strlen(LOOSE_FILE_CONTENT), fp);
        fclose(fp);
    }
}

static void remove_loose_dir(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LOOSE_DIR, LOOSE_FILE_RELATIVE);
    remove(path);
    tak_test_rmdir(LOOSE_DIR);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VFS_Init / VFS_Shutdown basics
 * ═══════════════════════════════════════════════════════════════════ */

TEST(init_null_game_dir_fails) {
    ensure_clean_vfs();
    ASSERT_EQ_INT(-1, VFS_Init(NULL, NULL));
}

TEST(init_nonexistent_dir_fails) {
    ensure_clean_vfs();
    ASSERT_EQ_INT(-1, VFS_Init("nonexistent_dir_xyz_999", NULL));
}

TEST(init_dir_with_no_hpi_files_fails) {
    ensure_clean_vfs();
    create_loose_dir();
    int r = VFS_Init(LOOSE_DIR, NULL);
    remove_loose_dir();
    ASSERT_EQ_INT(-1, r);
}

TEST(init_valid_game_dir_succeeds) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    VFS_Shutdown();
}

TEST(init_double_init_fails) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    int r = VFS_Init(TAK_GAME_DIR, NULL);
    VFS_Shutdown();
    ASSERT_EQ_INT(-1, r);
}

TEST(shutdown_then_reinit_succeeds) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    VFS_Shutdown();
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    VFS_Shutdown();
}

TEST(shutdown_without_init_does_not_crash) {
    ensure_clean_vfs();
    VFS_Shutdown();
}

/* The UI tests close a VFS a failed test left open; they need to ask. */
TEST(is_initialized_follows_init_and_shutdown) {
    ensure_clean_vfs();
    ASSERT_EQ_INT(0, VFS_IsInitialized());
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    ASSERT_EQ_INT(1, VFS_IsInitialized());
    VFS_Shutdown();
    ASSERT_EQ_INT(0, VFS_IsInitialized());
}

/* ═══════════════════════════════════════════════════════════════════
 *  VFS_GetArchiveCount
 * ═══════════════════════════════════════════════════════════════════ */

TEST(archive_count_is_zero_before_init) {
    ensure_clean_vfs();
    ASSERT_EQ_INT(0, VFS_GetArchiveCount());
}

TEST(archive_count_matches_hpi_files) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    int count = VFS_GetArchiveCount();
    VFS_Shutdown();
    ASSERT(count >= 10);  /* TAK has 16 HPIs */
    ASSERT(count <= 400); /* plus any .ufo and the Maps map packs */
}

TEST(archive_count_is_zero_after_shutdown) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    VFS_Shutdown();
    ASSERT_EQ_INT(0, VFS_GetArchiveCount());
}

/* ═══════════════════════════════════════════════════════════════════
 *  VFS_FileExists
 * ═══════════════════════════════════════════════════════════════════ */

TEST(file_exists_returns_0_for_known_file) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    int r = VFS_FileExists("gamedata/sidedata.tdf");
    VFS_Shutdown();
    ASSERT_EQ_INT(0, r);
}

TEST(file_exists_returns_neg1_for_missing_file) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    int r = VFS_FileExists("nonexistent/file.xyz");
    VFS_Shutdown();
    ASSERT_EQ_INT(-1, r);
}

TEST(file_exists_is_case_insensitive) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    int r = VFS_FileExists("GAMEDATA/SIDEDATA.TDF");
    VFS_Shutdown();
    ASSERT_EQ_INT(0, r);
}

TEST(file_exists_normalizes_backslashes) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    int r = VFS_FileExists("gamedata\\sidedata.tdf");
    VFS_Shutdown();
    ASSERT_EQ_INT(0, r);
}

TEST(file_exists_null_path_returns_neg1) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    int r = VFS_FileExists(NULL);
    VFS_Shutdown();
    ASSERT_EQ_INT(-1, r);
}

TEST(file_exists_without_init_returns_neg1) {
    ensure_clean_vfs();
    ASSERT_EQ_INT(-1, VFS_FileExists("gamedata/sidedata.tdf"));
}

/* ═══════════════════════════════════════════════════════════════════
 *  VFS_ReadFile
 * ═══════════════════════════════════════════════════════════════════ */

TEST(readfile_reads_known_file) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    void *data = NULL;
    uint32_t size = 0;
    int r = VFS_ReadFile("gamedata/sidedata.tdf", &data, &size);
    VFS_Shutdown();
    ASSERT_EQ_INT(0, r);
    ASSERT_NOT_NULL(data);
    ASSERT(size > 100);

    VFS_FreeBuffer(data);
}

TEST(readfile_returns_neg1_for_missing_file) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    void *data = NULL;
    uint32_t size = 0;
    int r = VFS_ReadFile("nonexistent/file.xyz", &data, &size);
    VFS_Shutdown();
    ASSERT_EQ_INT(-1, r);
}

TEST(readfile_null_path_returns_neg1) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    void *data = NULL;
    uint32_t size = 0;
    int r = VFS_ReadFile(NULL, &data, &size);
    VFS_Shutdown();
    ASSERT_EQ_INT(-1, r);
}

TEST(readfile_without_init_returns_neg1) {
    ensure_clean_vfs();
    void *data = NULL;
    uint32_t size = 0;
    ASSERT_EQ_INT(-1, VFS_ReadFile("gamedata/sidedata.tdf", &data, &size));
}

TEST(readfile_same_file_twice_gives_identical_data) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    void *data1 = NULL, *data2 = NULL;
    uint32_t size1 = 0, size2 = 0;
    VFS_ReadFile("gamedata/sidedata.tdf", &data1, &size1);
    VFS_ReadFile("gamedata/sidedata.tdf", &data2, &size2);
    VFS_Shutdown();

    ASSERT_EQ_INT((int)size1, (int)size2);
    ASSERT(memcmp(data1, data2, size1) == 0);

    VFS_FreeBuffer(data1);
    VFS_FreeBuffer(data2);
}

TEST(readfile_returns_nonzero_size) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    void *data = NULL;
    uint32_t size = 0;
    int r = VFS_ReadFile("gamedata/sidedata.tdf", &data, &size);
    VFS_Shutdown();
    ASSERT_EQ_INT(0, r);
    ASSERT(size > 0);
    VFS_FreeBuffer(data);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VFS_ListFiles
 * ═══════════════════════════════════════════════════════════════════ */

TEST(listfiles_finds_matching_files) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    char **paths = NULL;
    int count = 0;
    int r = VFS_ListFiles("gamedata/*", &paths, &count);
    VFS_Shutdown();

    ASSERT_EQ_INT(0, r);
    ASSERT(count > 0);

    for (int i = 0; i < count; i++) {
        ASSERT_NOT_NULL(paths[i]);
        ASSERT(tak_strnicmp(paths[i], "gamedata/", 9) == 0);
        tak_free(paths[i]);
    }
    tak_free(paths);
}

TEST(listfiles_no_matches_returns_0_count) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    char **paths = NULL;
    int count = 0;
    int r = VFS_ListFiles("zzz_nonexistent_pattern_zzz/*", &paths, &count);
    VFS_Shutdown();

    ASSERT_EQ_INT(0, r);
    ASSERT_EQ_INT(0, count);
    ASSERT_NULL(paths);
}

TEST(listfiles_deduplicates_across_archives) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    char **paths = NULL;
    int count = 0;
    int r = VFS_ListFiles("gamedata/*", &paths, &count);
    VFS_Shutdown();

    ASSERT_EQ_INT(0, r);

    /* Check no duplicates (case-insensitive) */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (tak_stricmp(paths[i], paths[j]) == 0) {
                printf("FAIL\n    duplicate: %s\n", paths[i]);
                for (int k = 0; k < count; k++) tak_free(paths[k]);
                tak_free(paths);
                _tf_fail_count++;
                _tf_current_failed = 1;
                return;
            }
        }
    }

    for (int i = 0; i < count; i++) tak_free(paths[i]);
    tak_free(paths);
}

TEST(listfiles_null_pattern_returns_neg1) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));

    char **paths = NULL;
    int count = 0;
    int r = VFS_ListFiles(NULL, &paths, &count);
    VFS_Shutdown();

    ASSERT_EQ_INT(-1, r);
}

TEST(listfiles_without_init_returns_neg1) {
    ensure_clean_vfs();
    char **paths = NULL;
    int count = 0;
    ASSERT_EQ_INT(-1, VFS_ListFiles("gamedata/*", &paths, &count));
}

/* ═══════════════════════════════════════════════════════════════════
 *  Loose file fallback (flat and recursive)
 * ═══════════════════════════════════════════════════════════════════ */

static void create_nested_loose_dir(void) {
    tak_test_mkdir(LOOSE_DIR);
    tak_test_mkdir("test_vfs_loose_tmp/subdir");
    tak_test_mkdir("test_vfs_loose_tmp/subdir/deep");

    FILE *fp;
    fp = fopen("test_vfs_loose_tmp/top.txt", "wb");
    if (fp) { fprintf(fp, "top"); fclose(fp); }
    fp = fopen("test_vfs_loose_tmp/subdir/mid.txt", "wb");
    if (fp) { fprintf(fp, "mid"); fclose(fp); }
    fp = fopen("test_vfs_loose_tmp/subdir/deep/bottom.txt", "wb");
    if (fp) { fprintf(fp, "bottom"); fclose(fp); }

    /* Also create the standard test file */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LOOSE_DIR, LOOSE_FILE_RELATIVE);
    fp = fopen(path, "wb");
    if (fp) { fwrite(LOOSE_FILE_CONTENT, 1, strlen(LOOSE_FILE_CONTENT), fp); fclose(fp); }
}

static void remove_nested_loose_dir(void) {
    remove("test_vfs_loose_tmp/subdir/deep/bottom.txt");
    tak_test_rmdir("test_vfs_loose_tmp/subdir/deep");
    remove("test_vfs_loose_tmp/subdir/mid.txt");
    tak_test_rmdir("test_vfs_loose_tmp/subdir");
    remove("test_vfs_loose_tmp/top.txt");
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LOOSE_DIR, LOOSE_FILE_RELATIVE);
    remove(path);
    tak_test_rmdir(LOOSE_DIR);
}

TEST(loose_file_exists) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    create_loose_dir();
    int r_init = VFS_Init(TAK_GAME_DIR, LOOSE_DIR);
    if (r_init != 0) { remove_loose_dir(); ASSERT_EQ_INT(0, r_init); return; }
    int r = VFS_FileExists(LOOSE_FILE_RELATIVE);
    VFS_Shutdown();
    remove_loose_dir();
    ASSERT_EQ_INT(0, r);
}

TEST(loose_file_read) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    create_loose_dir();
    int r_init = VFS_Init(TAK_GAME_DIR, LOOSE_DIR);
    if (r_init != 0) { remove_loose_dir(); ASSERT_EQ_INT(0, r_init); return; }

    void *data = NULL;
    uint32_t size = 0;
    int r = VFS_ReadFile(LOOSE_FILE_RELATIVE, &data, &size);
    VFS_Shutdown();
    remove_loose_dir();

    ASSERT_EQ_INT(0, r);
    ASSERT_NOT_NULL(data);
    ASSERT_EQ_INT((int)strlen(LOOSE_FILE_CONTENT), (int)size);
    ASSERT(memcmp(data, LOOSE_FILE_CONTENT, size) == 0);

    VFS_FreeBuffer(data);
}

TEST(archive_takes_priority_over_loose) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }

    tak_test_mkdir(LOOSE_DIR);
    tak_test_mkdir("test_vfs_loose_tmp/gamedata");
    char path[512];
    snprintf(path, sizeof(path), "%s/gamedata/sidedata.tdf", LOOSE_DIR);
    FILE *fp = fopen(path, "wb");
    if (fp) { fprintf(fp, "FAKE LOOSE DATA"); fclose(fp); }

    int r_init = VFS_Init(TAK_GAME_DIR, LOOSE_DIR);
    if (r_init != 0) {
        remove(path); tak_test_rmdir("test_vfs_loose_tmp/gamedata"); tak_test_rmdir(LOOSE_DIR);
        ASSERT_EQ_INT(0, r_init);
        return;
    }

    void *data = NULL;
    uint32_t size = 0;
    int r = VFS_ReadFile("gamedata/sidedata.tdf", &data, &size);
    VFS_Shutdown();

    remove(path);
    tak_test_rmdir("test_vfs_loose_tmp/gamedata");
    tak_test_rmdir(LOOSE_DIR);

    ASSERT_EQ_INT(0, r);
    ASSERT_NOT_NULL(data);
    /* The archive version should be larger than our 15-byte fake */
    ASSERT(size > 100);
    VFS_FreeBuffer(data);
}

TEST(loose_file_nested_exists) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    create_nested_loose_dir();

    int r_init = VFS_Init(TAK_GAME_DIR, LOOSE_DIR);
    if (r_init != 0) { remove_nested_loose_dir(); ASSERT_EQ_INT(0, r_init); return; }

    /* Files at various depths should be findable */
    ASSERT_EQ_INT(0, VFS_FileExists("top.txt"));
    ASSERT_EQ_INT(0, VFS_FileExists("subdir/mid.txt"));
    ASSERT_EQ_INT(0, VFS_FileExists("subdir/deep/bottom.txt"));

    VFS_Shutdown();
    remove_nested_loose_dir();
}

TEST(loose_file_nested_read) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    create_nested_loose_dir();

    int r_init = VFS_Init(TAK_GAME_DIR, LOOSE_DIR);
    if (r_init != 0) { remove_nested_loose_dir(); ASSERT_EQ_INT(0, r_init); return; }

    void *data = NULL;
    uint32_t size = 0;
    int r = VFS_ReadFile("subdir/deep/bottom.txt", &data, &size);
    VFS_Shutdown();
    remove_nested_loose_dir();

    ASSERT_EQ_INT(0, r);
    ASSERT_NOT_NULL(data);
    ASSERT_EQ_INT(6, (int)size); /* "bottom" */
    ASSERT(memcmp(data, "bottom", 6) == 0);
    VFS_FreeBuffer(data);
}

TEST(loose_listfiles_finds_nested_files) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    create_nested_loose_dir();

    int r_init = VFS_Init(TAK_GAME_DIR, LOOSE_DIR);
    if (r_init != 0) { remove_nested_loose_dir(); ASSERT_EQ_INT(0, r_init); return; }

    char **paths = NULL;
    int count = 0;
    int r = VFS_ListFiles("*.txt", &paths, &count);
    VFS_Shutdown();
    remove_nested_loose_dir();

    ASSERT_EQ_INT(0, r);
    /* Should find at least the loose .txt files */
    ASSERT(count >= 3);

    for (int i = 0; i < count; i++) tak_free(paths[i]);
    tak_free(paths);
}

/* ═══════════════════════════════════════════════════════════════════
 *  scan_directory (tested through VFS_Init archive counting)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(scan_finds_all_hpi_files) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    /* The GOG install has 16 .hpi, no .ufo and 181 .kmp map
     * packs in Maps. */
    ASSERT_EQ_INT(197, VFS_GetArchiveCount());
    VFS_Shutdown();
}

TEST(scan_returns_full_paths) {
    ensure_clean_vfs();
    if (!game_dir_exists()) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, NULL));
    /* After init, archives should be openable, which means scan_directory
       returned full paths (not just filenames). If it returned bare names,
       HPI_OpenArchive would have failed and VFS_Init would return -1. */
    ASSERT(VFS_GetArchiveCount() > 0);
    VFS_Shutdown();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Archive kinds and precedence
 *
 *  Fixtures are built on the fly by test_write_hpi so no game content
 *  is needed. The game folder holds <name>.hpi and <name>.ufo, and
 *  Maps/ holds the .kmp map packs.
 * ═══════════════════════════════════════════════════════════════════ */

static const char *ARC_DIR = "test_vfs_archives_tmp";
static const char *ARC_MAPS_DIR = "test_vfs_archives_tmp/Maps";

static void arc_dir_create(void) {
    tak_test_mkdir(ARC_DIR);
    tak_test_mkdir(ARC_MAPS_DIR);
}

static void arc_dir_remove(void) {
    remove("test_vfs_archives_tmp/alpha.hpi");
    remove("test_vfs_archives_tmp/zulu.hpi");
    remove("test_vfs_archives_tmp/extra.ufo");
    remove("test_vfs_archives_tmp/Maps/Custom Field.kmp");
    tak_test_rmdir(ARC_MAPS_DIR);
    tak_test_rmdir(ARC_DIR);
}

/* The original mounts the game folder's *.HPI, then its *.UFO
 * (legacy:121111, legacy:121133), so a unit pack shipped as .ufo is part
 * of the data set. */
TEST(ufo_archives_are_mounted) {
    ensure_clean_vfs();
    arc_dir_create();
    TestHPIEntry base[] = { { "gamedata/sidedata.tdf", "base sides", 1000 } };
    TestHPIEntry pack[] = { { "units/extra.fbi", "extra unit", 2000 } };
    int w = test_write_hpi("test_vfs_archives_tmp/alpha.hpi", base, 1);
    w |= test_write_hpi("test_vfs_archives_tmp/extra.ufo", pack, 1);
    if (w != 0) { arc_dir_remove(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(ARC_DIR, NULL);
    int count = VFS_GetArchiveCount();
    int found = VFS_FileExists("units/extra.fbi");
    VFS_Shutdown();
    arc_dir_remove();
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(2, count);
    ASSERT_EQ_INT(0, found);
}

/* A .kmp is an archive holding one map under kmap/ (legacy:167672 lists
 * Maps\*.kmp next to Maps\*.ota, legacy:168799 reads KMAP\*.ota). */
TEST(kmp_map_packs_are_mounted) {
    ensure_clean_vfs();
    arc_dir_create();
    TestHPIEntry base[] = { { "gamedata/sidedata.tdf", "base sides", 1000 } };
    TestHPIEntry kmp[] = {
        { "kmap/Custom Field.ota", "[GlobalHeader]", 3000 },
        { "kmap/Custom Field.tnt", "tnt bytes", 3000 },
    };
    int w = test_write_hpi("test_vfs_archives_tmp/alpha.hpi", base, 1);
    w |= test_write_hpi("test_vfs_archives_tmp/Maps/Custom Field.kmp", kmp, 2);
    if (w != 0) { arc_dir_remove(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(ARC_DIR, NULL);
    int exists = VFS_FileExists("kmap/Custom Field.ota");
    void *data = NULL;
    uint32_t size = 0;
    int read_rc = VFS_ReadFile("kmap/custom field.tnt", &data, &size);
    char **paths = NULL;
    int listed = 0;
    VFS_ListFiles("kmap/*.ota", &paths, &listed);
    if (paths) { for (int i = 0; i < listed; i++) tak_free(paths[i]); tak_free(paths); }
    VFS_FreeBuffer(data);
    VFS_Shutdown();
    arc_dir_remove();
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(0, exists);
    ASSERT_EQ_INT(0, read_rc);
    ASSERT_EQ_INT(9, (int)size);
    ASSERT_EQ_INT(1, listed);
}

/* A map pack only contributes its map. Anything else it carries stays
 * out of the data set, so dropping one in never rewrites the game. */
TEST(kmp_contents_outside_kmap_stay_invisible) {
    ensure_clean_vfs();
    arc_dir_create();
    TestHPIEntry base[] = { { "gamedata/sidedata.tdf", "base sides", 1000 } };
    TestHPIEntry kmp[] = {
        { "kmap/Custom Field.ota", "[GlobalHeader]", 3000 },
        { "units/sneaky.fbi", "not wanted", 9000 },
    };
    int w = test_write_hpi("test_vfs_archives_tmp/alpha.hpi", base, 1);
    w |= test_write_hpi("test_vfs_archives_tmp/Maps/Custom Field.kmp", kmp, 2);
    if (w != 0) { arc_dir_remove(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(ARC_DIR, NULL);
    int sneaky = VFS_FileExists("units/sneaky.fbi");
    VFS_Shutdown();
    arc_dir_remove();
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(-1, sneaky);
}

/* When two archives hold the same path the original takes the copy with
 * the newer entry date, whatever the archive is called (legacy:259289). */
TEST(newer_archive_entry_wins) {
    ensure_clean_vfs();
    arc_dir_create();
    TestHPIEntry newer[] = { { "gamedata/sidedata.tdf", "patched sides", 2000 } };
    TestHPIEntry older[] = { { "gamedata/sidedata.tdf", "original side", 1000 } };
    int w = test_write_hpi("test_vfs_archives_tmp/alpha.hpi", newer, 1);
    w |= test_write_hpi("test_vfs_archives_tmp/zulu.hpi", older, 1);
    if (w != 0) { arc_dir_remove(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(ARC_DIR, NULL);
    void *data = NULL;
    uint32_t size = 0;
    int read_rc = VFS_ReadFile("gamedata/sidedata.tdf", &data, &size);
    char got[32];
    int n = (size < sizeof(got) - 1) ? (int)size : (int)sizeof(got) - 1;
    if (data && read_rc == 0) { memcpy(got, data, (size_t)n); got[n] = '\0'; }
    else got[0] = '\0';
    VFS_FreeBuffer(data);
    VFS_Shutdown();
    arc_dir_remove();
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(0, read_rc);
    ASSERT_EQ_STR("patched sides", got);
}

/* Same date, so the tie goes to the archive mounted first, which is the
 * first by name among the .hpi files. */
TEST(same_date_keeps_the_first_archive) {
    ensure_clean_vfs();
    arc_dir_create();
    TestHPIEntry first[] = { { "gamedata/sidedata.tdf", "first copy..", 1000 } };
    TestHPIEntry second[] = { { "gamedata/sidedata.tdf", "second copy.", 1000 } };
    int w = test_write_hpi("test_vfs_archives_tmp/alpha.hpi", first, 1);
    w |= test_write_hpi("test_vfs_archives_tmp/zulu.hpi", second, 1);
    if (w != 0) { arc_dir_remove(); ASSERT_EQ_INT(0, w); return; }

    int rc = VFS_Init(ARC_DIR, NULL);
    void *data = NULL;
    uint32_t size = 0;
    int read_rc = VFS_ReadFile("gamedata/sidedata.tdf", &data, &size);
    char got[32];
    int n = (size < sizeof(got) - 1) ? (int)size : (int)sizeof(got) - 1;
    if (data && read_rc == 0) { memcpy(got, data, (size_t)n); got[n] = '\0'; }
    else got[0] = '\0';
    VFS_FreeBuffer(data);
    VFS_Shutdown();
    arc_dir_remove();
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(0, read_rc);
    ASSERT_EQ_STR("first copy..", got);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VFS_FreeBuffer
 * ═══════════════════════════════════════════════════════════════════ */

TEST(free_buffer_null_does_not_crash) {
    VFS_FreeBuffer(NULL);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    TEST_SUITE("VFS_Init / VFS_Shutdown");
    RUN(init_null_game_dir_fails);
    RUN(init_nonexistent_dir_fails);
    RUN(init_dir_with_no_hpi_files_fails);
    RUN(init_valid_game_dir_succeeds);
    RUN(init_double_init_fails);
    RUN(shutdown_then_reinit_succeeds);
    RUN(shutdown_without_init_does_not_crash);
    RUN(is_initialized_follows_init_and_shutdown);

    TEST_SUITE("VFS_GetArchiveCount");
    RUN(archive_count_is_zero_before_init);
    RUN(archive_count_matches_hpi_files);
    RUN(archive_count_is_zero_after_shutdown);

    TEST_SUITE("VFS_FileExists");
    RUN(file_exists_returns_0_for_known_file);
    RUN(file_exists_returns_neg1_for_missing_file);
    RUN(file_exists_is_case_insensitive);
    RUN(file_exists_normalizes_backslashes);
    RUN(file_exists_null_path_returns_neg1);
    RUN(file_exists_without_init_returns_neg1);

    TEST_SUITE("VFS_ReadFile");
    RUN(readfile_reads_known_file);
    RUN(readfile_returns_neg1_for_missing_file);
    RUN(readfile_null_path_returns_neg1);
    RUN(readfile_without_init_returns_neg1);
    RUN(readfile_same_file_twice_gives_identical_data);
    RUN(readfile_returns_nonzero_size);

    TEST_SUITE("VFS_ListFiles");
    RUN(listfiles_finds_matching_files);
    RUN(listfiles_no_matches_returns_0_count);
    RUN(listfiles_deduplicates_across_archives);
    RUN(listfiles_null_pattern_returns_neg1);
    RUN(listfiles_without_init_returns_neg1);

    TEST_SUITE("Loose file fallback");
    RUN(loose_file_exists);
    RUN(loose_file_read);
    RUN(archive_takes_priority_over_loose);

    TEST_SUITE("Recursive loose directory");
    RUN(loose_file_nested_exists);
    RUN(loose_file_nested_read);
    RUN(loose_listfiles_finds_nested_files);

    TEST_SUITE("scan_directory (via VFS_Init)");
    RUN(scan_finds_all_hpi_files);
    RUN(scan_returns_full_paths);

    TEST_SUITE("Archive kinds and precedence");
    RUN(ufo_archives_are_mounted);
    RUN(kmp_map_packs_are_mounted);
    RUN(kmp_contents_outside_kmap_stay_invisible);
    RUN(newer_archive_entry_wins);
    RUN(same_date_keeps_the_first_archive);

    TEST_SUITE("VFS_FreeBuffer");
    RUN(free_buffer_null_does_not_crash);

    TEST_REPORT();
}
