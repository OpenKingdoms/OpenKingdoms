/*
 * savelist.c: the saved game directory, as the dialogs see it.
 *
 * The original's list box is a plain directory enumeration with the
 * extension stripped off each name (legacy:159073-159085), and the
 * right hand panel comes from a short Summary entry inside the archive
 * rather than from the battle itself (legacy:159131-159160). This
 * reads the .oksave header and its light sections for the same three
 * fields: side, map and game time.
 *
 * A file that refuses to open keeps its row. The container already
 * says why in words a player can act on, so the reason rides along on
 * the row instead of being dropped.
 */

#include "tak_savelist.h"

#include "tak_gameloop.h"
#include "tak_memory.h"
#include "tak_paths.h"
#include "tak_savegame.h"
#include "tak_sides.h"
#include "tak_util.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#define SAVE_EXT     ".oksave"
#define SAVE_EXT_LEN 7

static void set_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

void SaveList_FormatTime(uint32_t ticks, uint32_t hz, char *out, size_t cap) {
    if (!out || !cap) return;
    if (hz == 0) hz = SIM_TICKS_PER_SECOND;
    uint32_t secs = ticks / hz;
    snprintf(out, cap, "%02u:%02u:%02u",
             (unsigned)(secs / 3600u),
             (unsigned)((secs / 60u) % 60u),
             (unsigned)(secs % 60u));
}

TAK_SaveNameVerdict SaveList_CheckName(const char *name) {
    if (!name) return TAK_SAVENAME_EMPTY;
    /* Spaces alone are not a name. */
    const char *p = name;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return TAK_SAVENAME_EMPTY;

    size_t n = strlen(name);
    if (n + SAVE_EXT_LEN >= TAK_SAVE_SLUG_MAX) return TAK_SAVENAME_INVALID;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) return TAK_SAVENAME_INVALID;
        if (strchr("<>:\"/\\|?*", (int)c)) return TAK_SAVENAME_INVALID;
    }
    if (strstr(name, "..")) return TAK_SAVENAME_INVALID;
    /* A trailing space or dot is dropped by Windows, so the file the
     * player gets back is not the one they named. */
    if (name[n - 1] == ' ' || name[n - 1] == '.') return TAK_SAVENAME_INVALID;
    return TAK_SAVENAME_OK;
}

/* The slug of a directory entry, or 0 when it does not end in `ext`. */
static int slug_of(const char *filename, const char *ext, char *out, size_t cap) {
    size_t n = strlen(filename);
    size_t ext_len = strlen(ext);
    if (n <= ext_len) return 0;
    if (tak_stricmp(filename + n - ext_len, ext) != 0) return 0;
    size_t keep = n - ext_len;
    if (keep >= cap) keep = cap - 1;
    memcpy(out, filename, keep);
    out[keep] = '\0';
    return 1;
}

/* Read what the right hand panel shows. A refusal is not a failure
 * here: the row stays and carries the reason. */
static void fill_row(TAK_SaveEntry *e) {
    char err[256];
    err[0] = '\0';
    TAK_SaveGame *sg = Save_Read(e->path, err, sizeof(err));
    if (!sg) {
        e->readable = 0;
        set_text(e->refusal, sizeof(e->refusal),
                 err[0] ? err : "This save cannot be read.");
        set_text(e->game_time, sizeof(e->game_time), "--:--:--");
        return;
    }
    const TAK_SaveInfo *info = Save_Info(sg);
    e->readable = 1;
    set_text(e->map, sizeof(e->map), info->map_name);
    e->saved_at_utc = info->saved_at_utc;
    /* The header carries the rate the writer ran at, but the reader's
     * public info does not expose it, so this uses the rate this build
     * writes. See the note in tak_savelist.h's sibling report. */
    SaveList_FormatTime(info->sim_tick, SIM_TICKS_PER_SECOND,
                        e->game_time, sizeof(e->game_time));
    /* "Side" is the local player's kingdom, the way the original's
     * summary carries one Side line (legacy:164912-165010). Player one
     * is the local seat everywhere in this build. */
    Sides_DisplayName(info->cfg.players[0].side, e->side, sizeof(e->side));
    Save_ReadClose(sg);
}

static TAK_SaveEntry *grow(TAK_SaveEntry *list, int *cap, int want) {
    if (want <= *cap) return list;
    int next = *cap ? *cap * 2 : 16;
    while (next < want) next *= 2;
    TAK_SaveEntry *grown = (TAK_SaveEntry *)tak_realloc(
        list, (size_t)next * sizeof(TAK_SaveEntry));
    if (!grown) return NULL;
    *cap = next;
    return grown;
}

int SaveList_ScanExt(const char *ext, void (*fill)(TAK_SaveEntry *e),
                    int max, TAK_SaveEntry **out) {
    if (!out || !ext || !fill || max < 0) return -1;
    *out = NULL;

    const char *dir = Paths_SaveDir();
    TAK_SaveEntry *list = NULL;
    int count = 0, cap = 0;

#ifdef _WIN32
    char pattern[TAK_SAVE_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s*%s", dir, ext);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char slug[TAK_SAVE_SLUG_MAX];
        if (!slug_of(fd.cFileName, ext, slug, sizeof(slug))) continue;
        if (count >= max) break;
        TAK_SaveEntry *grown = grow(list, &cap, count + 1);
        if (!grown) break;
        list = grown;
        TAK_SaveEntry *e = &list[count++];
        memset(e, 0, sizeof(*e));
        set_text(e->slug, sizeof(e->slug), slug);
        snprintf(e->path, sizeof(e->path), "%s%s", dir, fd.cFileName);
        fill(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char slug[TAK_SAVE_SLUG_MAX];
        if (!slug_of(ent->d_name, ext, slug, sizeof(slug))) continue;
        if (count >= max) break;
        TAK_SaveEntry *grown = grow(list, &cap, count + 1);
        if (!grown) break;
        list = grown;
        TAK_SaveEntry *e = &list[count++];
        memset(e, 0, sizeof(*e));
        set_text(e->slug, sizeof(e->slug), slug);
        snprintf(e->path, sizeof(e->path), "%s%s", dir, ent->d_name);
        fill(e);
    }
    closedir(d);
#endif

    *out = list;
    return count;
}

int SaveList_Scan(TAK_SaveEntry **out) {
    return SaveList_ScanExt(SAVE_EXT, fill_row, INT_MAX, out);
}

void SaveList_Free(TAK_SaveEntry *list) {
    if (list) tak_free(list);
}
