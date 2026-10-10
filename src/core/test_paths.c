/* Where the game writes the player's own files.
 *
 * Data free, so CI runs it. Everything happens under a scratch
 * directory beside the test binary. */

#include "tak_paths.h"
#include "tak_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#  include <direct.h>
#endif

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

/* The store holds text as well as numbers, because a player name has
 * to survive a restart and the device token a rejoin is recognised by
 * will have to as well. The file is key=value lines either way, so a
 * value that happens to look like a number must still come back as a
 * number and one that does not must come back as its text. */
static int test_settings_hold_text_as_well_as_numbers(void) {
    Settings_SetDirectory("test_paths_scratch/text");
    remove(Settings_FilePath());

    Settings_SetStr("PlayerName", "42nd Regiment");
    Settings_SetInt("DisplayDamageBars", 1);
    ASSERT(Paths_SaveDir() != NULL);   /* creates the tree */
    ASSERT(Settings_Save() == 0);

    /* Forget both, then read the file back. */
    Settings_SetStr("PlayerName", "");
    Settings_SetInt("DisplayDamageBars", 0);
    ASSERT(Settings_Load() == 0);
    ASSERT_STR("42nd Regiment", Settings_GetStr("PlayerName", ""));
    ASSERT(Settings_GetInt("DisplayDamageBars", 0) == 1);

    /* A number asked for as text, and text asked for as a number, get
     * the default rather than a reinterpretation of the bytes. */
    ASSERT_STR("none", Settings_GetStr("DisplayDamageBars", "none"));
    ASSERT(Settings_GetInt("PlayerName", -7) == -7);

    /* One line per setting. A value carrying a newline would read back
     * as a second key, so it is refused rather than written. */
    Settings_SetStr("PlayerName", "one\ntwo");
    ASSERT_STR("42nd Regiment", Settings_GetStr("PlayerName", ""));

    Settings_SetDirectory(NULL);
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

/* Set or clear TAK_CONFIG_DIR through the C runtime, which is what
 * getenv reads. */
static void set_config_env(const char *value) {
#ifdef _WIN32
    _putenv_s("TAK_CONFIG_DIR", value ? value : "");
#else
    if (value) setenv("TAK_CONFIG_DIR", value, 1);
    else unsetenv("TAK_CONFIG_DIR");
#endif
}

static char s_env_was[1024];
static int  s_env_had;

static void save_config_env(void) {
    const char *v = getenv("TAK_CONFIG_DIR");
    s_env_had = v != NULL;
    snprintf(s_env_was, sizeof(s_env_was), "%s", v ? v : "");
}

static void restore_config_env(void) {
    set_config_env(s_env_had ? s_env_was : NULL);
    Settings_SetDirectory(NULL);
}

static int is_absolute(const char *p) {
    if (p[0] == '/' || p[0] == '\\') return 1;
    return p[0] && p[1] == ':';
}

/* TAK_CONFIG_DIR names the preference directory when nothing in the
 * process has overridden it, and the directory is made if missing. */
static int test_config_dir_from_environment(void) {
    save_config_env();
    set_config_env("test_paths_scratch/envprefs");
    Settings_SetDirectory(NULL);
    ASSERT_STR("test_paths_scratch/envprefs/", Paths_PrefDir());
    ASSERT(is_dir("test_paths_scratch/envprefs"));
    ASSERT_STR("test_paths_scratch/envprefs/options.cfg", Settings_FilePath());
    ASSERT_STR("test_paths_scratch/envprefs/saves/", Paths_SaveDir());

    /* An override set by the process still wins over the environment. */
    Settings_SetDirectory("test_paths_scratch/cfg");
    ASSERT_STR("test_paths_scratch/cfg/options.cfg", Settings_FilePath());
    restore_config_env();
    return 0;
}

/* A test binary never resolves the player's own preference directory,
 * not even with nothing in the environment, because that is the real
 * options.cfg a test would rewrite. It falls back to a folder under the
 * directory it runs in. */
static int test_test_build_never_reaches_player_prefs(void) {
    save_config_env();
    set_config_env(NULL);
    Settings_SetDirectory(NULL);
    const char *pref = Paths_PrefDir();
    ASSERT(!is_absolute(pref));
    ASSERT_STR("test_prefs/", pref);
    ASSERT(is_dir("test_prefs"));
    ASSERT_STR("test_prefs/options.cfg", Settings_FilePath());
    restore_config_env();
    return 0;
}

/* The game directory a shipped binary has to find at run time. */
static int scratch_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int scratch_touch(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fclose(f);
    return 0;
}

static int test_game_dir_search(void) {
    scratch_mkdir("test_paths_scratch");
    scratch_mkdir("test_paths_scratch/empty");
    scratch_mkdir("test_paths_scratch/install");
    /* A folder with no archives is not the game's folder. */
    ASSERT(scratch_touch("test_paths_scratch/empty/readme.txt") == 0);
    ASSERT(scratch_touch("test_paths_scratch/install/totala1.hpi") == 0);

    ASSERT(!Paths_IsGameDir(""));
    ASSERT(!Paths_IsGameDir("test_paths_scratch/nowhere"));
    ASSERT(!Paths_IsGameDir("test_paths_scratch/empty"));
    ASSERT(Paths_IsGameDir("test_paths_scratch/install"));

    /* The first candidate holding archives wins. */
    const char *cands[4] = { "", "test_paths_scratch/nowhere",
                             "test_paths_scratch/empty",
                             "test_paths_scratch/install" };
    char out[512];
    ASSERT(Paths_PickGameDir(cands, 4, out, sizeof out) == 0);
    ASSERT_STR("test_paths_scratch/install", out);

    /* Nothing found leaves no half answer behind. */
    ASSERT(Paths_PickGameDir(cands, 3, out, sizeof out) != 0);
    ASSERT_STR("", out);
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
        { "settings_hold_text",           test_settings_hold_text_as_well_as_numbers },
        { "platform_default_restored",    test_platform_default_restored },
        { "config_dir_from_environment",  test_config_dir_from_environment },
        { "test_build_keeps_off_prefs",   test_test_build_never_reaches_player_prefs },
        { "game_dir_search",              test_game_dir_search },
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
