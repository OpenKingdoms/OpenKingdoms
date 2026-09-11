/* Where the game writes the player's own files.
 *
 * Data free, so CI runs it. Everything happens under a scratch
 * directory beside the test binary. */

#include "tak_paths.h"
#include "tak_settings.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ASSERT(x) do { \
    if (!(x)) { \
        fprintf(stderr, "ASSERT failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define ASSERT_STR(exp, got) do { \
    const char *_e = (exp); \
    const char *_g = (got); \
    if (strcmp(_e, _g) != 0) { \
        fprintf(stderr, "ASSERT_STR failed at %s:%d:\n  expected \"%s\"\n  got      \"%s\"\n", \
                __FILE__, __LINE__, _e, _g); \
        return 1; \
    } \
} while (0)

static int is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

static int has_char(const char *s, char c) { return strchr(s, c) != NULL; }

/* A composed path must never mix separators. SDL hands back a
 * backslash path on Windows, so anything appended with a hardcoded
 * forward slash produces a path that is half one and half the
 * other, which is exactly what the option file path looks like today. */
static int mixed_separators(const char *s) {
    return has_char(s, '/') && has_char(s, '\\');
}

static int test_override_forward_slashes(void) {
    Paths_SetOverride("test_paths_scratch/fwd");
    ASSERT_STR("test_paths_scratch/fwd/saves/", Paths_SaveDir());
    ASSERT(!mixed_separators(Paths_SaveDir()));

    /* A trailing separator is not doubled. */
    Paths_SetOverride("test_paths_scratch/fwd/");
    ASSERT_STR("test_paths_scratch/fwd/saves/", Paths_SaveDir());
    return 0;
}

#ifdef _WIN32
static int test_override_backslashes(void) {
    Paths_SetOverride("test_paths_scratch\\bwd");
    ASSERT_STR("test_paths_scratch\\bwd\\saves\\", Paths_SaveDir());
    ASSERT(!mixed_separators(Paths_SaveDir()));

    char file[512];
    ASSERT(Paths_SaveFile("kingsmarch", file, sizeof(file)) == 0);
    ASSERT_STR("test_paths_scratch\\bwd\\saves\\kingsmarch.oksave", file);
    ASSERT(!mixed_separators(file));

    Settings_SetDirectory("test_paths_scratch\\bwd");
    ASSERT_STR("test_paths_scratch\\bwd\\options.cfg", Settings_FilePath());
    ASSERT(!mixed_separators(Settings_FilePath()));
    return 0;
}
#endif

static int test_save_dir_is_created(void) {
    Paths_SetOverride("test_paths_scratch/made/deeper");
    const char *dir = Paths_SaveDir();
    ASSERT(is_dir(dir));
    ASSERT(is_dir("test_paths_scratch/made"));
    return 0;
}

static int test_save_file(void) {
    Paths_SetOverride("test_paths_scratch/files");
    char file[512];

    ASSERT(Paths_SaveFile("autosave1", file, sizeof(file)) == 0);
    ASSERT_STR("test_paths_scratch/files/saves/autosave1.oksave", file);

    /* The display name lives inside the file, so a slug never needs a
     * separator, a drive or a parent reference. */
    ASSERT(Paths_SaveFile("../escape", file, sizeof(file)) == -1);
    ASSERT_STR("", file);
    ASSERT(Paths_SaveFile("sub/dir", file, sizeof(file)) == -1);
    ASSERT(Paths_SaveFile("sub\\dir", file, sizeof(file)) == -1);
    ASSERT(Paths_SaveFile("C:evil", file, sizeof(file)) == -1);
    ASSERT(Paths_SaveFile("", file, sizeof(file)) == -1);
    ASSERT(Paths_SaveFile(NULL, file, sizeof(file)) == -1);

    /* Too small a buffer reports failure rather than a truncated path. */
    char tiny[8];
    ASSERT(Paths_SaveFile("autosave1", tiny, sizeof(tiny)) == -1);
    ASSERT_STR("", tiny);
    return 0;
}

static int test_settings_override_still_works(void) {
    Settings_SetDirectory("test_paths_scratch/cfg");
    ASSERT_STR("test_paths_scratch/cfg/options.cfg", Settings_FilePath());

    /* Settings and saves resolve against the same override. */
    ASSERT_STR("test_paths_scratch/cfg/saves/", Paths_SaveDir());

    /* A write really lands there and reads back. */
    Settings_SetInt("DisplayDamageBars", 1);
    ASSERT(Paths_SaveDir() != NULL);   /* creates the tree */
    ASSERT(Settings_Save() == 0);
    struct stat st;
    ASSERT(stat("test_paths_scratch/cfg/options.cfg", &st) == 0);
    return 0;
}

static int test_platform_default_restored(void) {
    Settings_SetDirectory(NULL);
    const char *pref = Paths_PrefDir();
    ASSERT(pref != NULL);
    size_t n = strlen(pref);
    ASSERT(n > 0);
    ASSERT(pref[n - 1] == '/' || pref[n - 1] == '\\');
    ASSERT(!mixed_separators(pref));
    return 0;
}

int main(void) {
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "override_forward_slashes",     test_override_forward_slashes },
#ifdef _WIN32
        { "override_backslashes",         test_override_backslashes },
#endif
        { "save_dir_is_created",          test_save_dir_is_created },
        { "save_file",                    test_save_file },
        { "settings_override_still_works", test_settings_override_still_works },
        { "platform_default_restored",    test_platform_default_restored },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = cases[i].fn();
        printf("%-32s %s\n", cases[i].name, rc == 0 ? "ok" : "FAILED");
        failed += rc;
    }
    if (failed) {
        fprintf(stderr, "test_paths: %d case(s) failed\n", failed);
        return 1;
    }
    printf("test_paths: all cases passed\n");
    return 0;
}
