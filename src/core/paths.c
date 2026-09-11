#include "tak_paths.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  define tak_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define tak_mkdir(p) mkdir(p, 0755)
#endif

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif

#define PATHS_DIR_MAX  1024
#define PATHS_SUB_MAX  1100

static char s_pref[PATHS_DIR_MAX];
static char s_save[PATHS_SUB_MAX];

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* The separator this directory already uses. Falls back to a forward
 * slash, which every platform including Windows accepts. */
static char dir_sep(const char *dir) {
    size_t n = dir ? strlen(dir) : 0;
    while (n > 0) {
        char c = dir[--n];
        if (is_sep(c)) return c;
    }
    return '/';
}

static void ensure_trailing_sep(char *dir, size_t cap) {
    size_t n = strlen(dir);
    if (n == 0 || n + 2 > cap) return;
    if (is_sep(dir[n - 1])) return;
    dir[n] = dir_sep(dir);
    dir[n + 1] = '\0';
}

/* Create every component of `path`, ignoring failures on components
 * that already exist. An override can name a directory that is not
 * there yet, and a save is not worth losing to a missing parent. */
static void make_dirs(const char *path) {
    char buf[PATHS_SUB_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 0; i < n; i++) {
        if (!is_sep(buf[i]) || i == 0) continue;
        /* Skip the root of a Windows drive or a UNC share. */
        if (i == 2 && buf[1] == ':') continue;
        if (is_sep(buf[i - 1])) continue;
        char saved = buf[i];
        buf[i] = '\0';
        tak_mkdir(buf);
        buf[i] = saved;
    }
    tak_mkdir(buf);
}

const char *Paths_PrefDir(void) {
    if (s_pref[0]) return s_pref;
    char *pref = SDL_GetPrefPath("OpenKingdoms", "OpenKingdoms");
    if (pref) {
        snprintf(s_pref, sizeof(s_pref), "%s", pref);
        SDL_free(pref);
    } else {
        snprintf(s_pref, sizeof(s_pref), "./");
    }
    ensure_trailing_sep(s_pref, sizeof(s_pref));
    return s_pref;
}

const char *Paths_SaveDir(void) {
    if (s_save[0]) return s_save;
    const char *pref = Paths_PrefDir();
    char sep = dir_sep(pref);
    snprintf(s_save, sizeof(s_save), "%ssaves%c", pref, sep);
    make_dirs(s_save);
    return s_save;
}

int Paths_SaveFile(const char *slug, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!slug || !slug[0]) return -1;
    for (const char *p = slug; *p; p++) {
        if (is_sep(*p) || *p == ':') return -1;
    }
    if (strstr(slug, "..")) return -1;
    int n = snprintf(out, cap, "%s%s.oksave", Paths_SaveDir(), slug);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

void Paths_SetOverride(const char *dir) {
    if (dir && dir[0]) {
        snprintf(s_pref, sizeof(s_pref), "%s", dir);
        ensure_trailing_sep(s_pref, sizeof(s_pref));
    } else {
        s_pref[0] = '\0';
    }
    s_save[0] = '\0';
}

#ifdef __EMSCRIPTEN__
/* EM_JS is C calling out to the page, so nothing has to be added to
 * the exported runtime methods. The page installs Module.syncPrefs
 * when origin private storage is available and leaves it undefined
 * otherwise, which is the private window case. */
EM_JS(void, paths_sync_prefs, (void), {
    if (typeof Module !== 'undefined' && Module.syncPrefs) Module.syncPrefs();
});
#endif

void Paths_NotifyPrefWritten(void) {
#ifdef __EMSCRIPTEN__
    paths_sync_prefs();
#endif
}
