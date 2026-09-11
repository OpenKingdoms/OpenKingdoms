/*
 * maps.c -- where maps live and what the chooser may offer.
 *
 * Three sources hold skirmish maps: the maps folder inside an archive,
 * the same folder extracted loose, and the map packs in the Maps
 * folder, whose files sit under kmap. Campaign maps live in the missions
 * folder and never reach the skirmish list (legacy:167671 scans
 * Maps\*.ota and Maps\*.kmp and nothing else).
 */

#include "tak_maps.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>

/* List patterns per source. The archives spell the maps folder
 * "Maps/<name>.ota" or "maps/<name>.ota" and the loose dev tree keeps
 * it one level down as "maps/Maps/<name>.ota". */
static const char *const scan_patterns[] = {
    "kmap/*.ota",
    "maps/*.ota",
    "maps/Maps/*.ota",
};
static const int scan_sources[] = {
    TAK_MAP_SOURCE_PACK,
    TAK_MAP_SOURCE_MAPS,
    TAK_MAP_SOURCE_MAPS,
};

/* Folders a map's files may sit in, in the order the original prefers:
 * the map pack, then the maps folder, then the missions folder. */
static const char *const lookup_dirs[] = {
    "kmap/",
    "maps/Maps/",
    "maps/",
    "missions/missions/",
};

static void map_stem(const char *path, char *out, size_t cap) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot) n = (size_t)(dot - base);
    if (n > cap - 1) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

static int entry_cmp(const void *a, const void *b) {
    const TAK_MapEntry *ea = (const TAK_MapEntry *)a;
    const TAK_MapEntry *eb = (const TAK_MapEntry *)b;
    return tak_stricmp(ea->key, eb->key);
}

int TAK_Maps_Scan(TAK_MapEntry **out_entries, int *out_count) {
    if (!out_entries || !out_count) return -1;
    *out_entries = NULL;
    *out_count = 0;

    int cap = 64;
    int count = 0;
    TAK_MapEntry *list = (TAK_MapEntry *)tak_malloc(sizeof(TAK_MapEntry) * (size_t)cap);
    if (!list) return -1;

    for (size_t s = 0; s < sizeof(scan_patterns) / sizeof(scan_patterns[0]); s++) {
        char **paths = NULL;
        int n = 0;
        if (VFS_ListFiles(scan_patterns[s], &paths, &n) != 0) continue;
        for (int i = 0; i < n; i++) {
            char stem[sizeof(list[0].key)];
            map_stem(paths[i], stem, sizeof(stem));
            tak_free(paths[i]);
            if (!stem[0]) continue;

            int seen = 0;
            for (int k = 0; k < count; k++) {
                if (tak_stricmp(list[k].key, stem) == 0) { seen = 1; break; }
            }
            if (seen) continue;

            if (count == cap) {
                int grown = cap * 2;
                TAK_MapEntry *tmp = (TAK_MapEntry *)tak_realloc(
                    list, sizeof(TAK_MapEntry) * (size_t)grown);
                if (!tmp) {
                    for (int k = i + 1; k < n; k++) tak_free(paths[k]);
                    tak_free(list);
                    tak_free(paths);
                    return -1;
                }
                list = tmp;
                cap = grown;
            }
            strncpy(list[count].key, stem, sizeof(list[count].key) - 1);
            list[count].key[sizeof(list[count].key) - 1] = '\0';
            list[count].source = scan_sources[s];
            count++;
        }
        tak_free(paths);
    }

    if (count > 1) qsort(list, (size_t)count, sizeof(TAK_MapEntry), entry_cmp);
    *out_entries = list;
    *out_count = count;
    return 0;
}

void TAK_Maps_Free(TAK_MapEntry *entries) {
    tak_free(entries);
}

int TAK_Maps_FindFile(const char *key, const char *ext,
                      char *out, size_t out_cap) {
    if (!key || !ext || !out || out_cap == 0) return -1;

    char upper[16];
    size_t e = 0;
    for (; ext[e] && e < sizeof(upper) - 1; e++) {
        char c = ext[e];
        upper[e] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    upper[e] = '\0';

    for (size_t d = 0; d < sizeof(lookup_dirs) / sizeof(lookup_dirs[0]); d++) {
        snprintf(out, out_cap, "%s%s.%s", lookup_dirs[d], key, ext);
        if (VFS_FileExists(out) == 0) return 0;
        /* Shipped maps mix .tnt and .TNT, and a case-sensitive loose
         * tree does not fold that for us. */
        snprintf(out, out_cap, "%s%s.%s", lookup_dirs[d], key, upper);
        if (VFS_FileExists(out) == 0) return 0;
    }

    snprintf(out, out_cap, "maps/Maps/%s.%s", key, ext);
    return -1;
}
