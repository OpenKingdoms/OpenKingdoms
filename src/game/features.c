/*
 * features.c -- Global feature definition registry.
 *
 * Walks data/features/<subdir>/*.tdf at startup, parses every
 * [section] into a FeatureDef, and indexes them by load order so the
 * TNT feature_layer's uint16 IDs resolve to authored feature data
 * (sprite filename, category, footprint, etc.).
 *
 * Mirrors the legacy engine's feature loader (the legacy reference
 * ~126890 — `FileSystem_FindFiles("features", "tdf", ...)`).
 * Subdirectory order is alphabetical, file order alphabetical
 * within each, and section order is the order they appear in each
 * file. This deterministic ordering matches what the TNT was
 * authored against.
 */

#include "tak_features.h"
#include "tak_tdf.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <strings.h>
#  include <sys/stat.h>
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static FeatureDef *g_feats = NULL;
static int         g_feat_count = 0;
static int         g_feat_cap = 0;

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src || cap == 0) { if (cap) dst[0] = 0; return; }
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int feat_grow(int new_count) {
    if (new_count <= g_feat_cap) return 0;
    int new_cap = g_feat_cap ? g_feat_cap : 256;
    while (new_cap < new_count) new_cap *= 2;
    FeatureDef *p = (FeatureDef *)tak_malloc((size_t)new_cap * sizeof(FeatureDef));
    if (!p) return -1;
    if (g_feats) {
        memcpy(p, g_feats, (size_t)g_feat_count * sizeof(FeatureDef));
        tak_free(g_feats);
    }
    g_feats = p;
    g_feat_cap = new_cap;
    return 0;
}

static int parse_feature_tdf(const char *vfs_path) {
    /* Each TDF holds N [section] feature definitions. We walk every
     * top-level section in declared order. */
    TDFFile *t = TDF_Open(vfs_path);
    if (!t) return 0;
    if (TDF_Load(t) != 0) { TDF_Close(t); return 0; }
    int added = 0;
    const char *name = TDF_GetFirstSection(t);
    while (name) {
        if (TDF_PushSection(t, name) == 0) {
            if (feat_grow(g_feat_count + 1) == 0) {
                FeatureDef *f = &g_feats[g_feat_count];
                memset(f, 0, sizeof(*f));
                copy_str(f->name,     sizeof(f->name),     name);
                copy_str(f->world,    sizeof(f->world),
                         TDF_ReadString(t, "world",    ""));
                copy_str(f->category, sizeof(f->category),
                         TDF_ReadString(t, "category", ""));
                copy_str(f->filename, sizeof(f->filename),
                         TDF_ReadString(t, "filename", ""));
                copy_str(f->seqname,  sizeof(f->seqname),
                         TDF_ReadString(t, "seqname",  ""));
                f->footprint_x    = TDF_ReadInt(t, "footprintx",     1);
                f->footprint_z    = TDF_ReadInt(t, "footprintz",     1);
                f->height         = TDF_ReadInt(t, "height",         0);
                f->blocking       = TDF_ReadInt(t, "blocking",       0);
                f->reclaimable    = TDF_ReadInt(t, "reclaimable",    0);
                f->indestructible = TDF_ReadInt(t, "indestructible", 0);
                f->sacred_site    = TDF_ReadFloat(t, "sacredsite", 0.0f);
                f->damage         = TDF_ReadInt(t, "damage",         0);
                g_feat_count++;
                added++;
            }
            TDF_PopSection(t);
        }
        name = TDF_GetNextSection(t);
    }
    TDF_Close(t);
    return added;
}

static int str_cmp_qsort(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void scan_dir_files(const char *abs_dir,
                            const char *vfs_dir,
                            const char *ext,
                            char ***out_paths,
                            int *out_count)
{
    *out_paths = NULL;
    *out_count = 0;
#ifdef _WIN32
    char search[300];
    snprintf(search, sizeof(search), "%s\\*.%s", abs_dir, ext);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int cap = 32, n = 0;
    char **arr = (char **)tak_malloc(sizeof(char *) * cap);
    if (!arr) { FindClose(h); return; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n >= cap) {
            cap *= 2;
            arr = (char **)tak_realloc(arr, sizeof(char *) * cap);
            if (!arr) { FindClose(h); return; }
        }
        char *p = (char *)tak_malloc(strlen(vfs_dir) + 1 + strlen(fd.cFileName) + 1);
        if (!p) continue;
        sprintf(p, "%s/%s", vfs_dir, fd.cFileName);
        arr[n++] = p;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(arr, n, sizeof(char *), str_cmp_qsort);
    *out_paths = arr;
    *out_count = n;
#else
    DIR *d = opendir(abs_dir);
    if (!d) return;
    int cap = 32, n = 0;
    char **arr = (char **)tak_malloc(sizeof(char *) * cap);
    if (!arr) { closedir(d); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t fl = strlen(ent->d_name);
        size_t el = strlen(ext);
        if (fl <= el + 1) continue;
        if (strcasecmp(ent->d_name + fl - el, ext) != 0) continue;
        if (n >= cap) {
            cap *= 2;
            arr = (char **)tak_realloc(arr, sizeof(char *) * cap);
            if (!arr) { closedir(d); return; }
        }
        char *p = (char *)tak_malloc(strlen(vfs_dir) + 1 + fl + 1);
        if (!p) continue;
        sprintf(p, "%s/%s", vfs_dir, ent->d_name);
        arr[n++] = p;
    }
    closedir(d);
    qsort(arr, n, sizeof(char *), str_cmp_qsort);
    *out_paths = arr;
    *out_count = n;
#endif
}

static void scan_dir_subdirs(const char *abs_dir,
                              char ***out_subdirs,
                              int *out_count)
{
    *out_subdirs = NULL;
    *out_count = 0;
#ifdef _WIN32
    char search[300];
    snprintf(search, sizeof(search), "%s\\*", abs_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int cap = 16, n = 0;
    char **arr = (char **)tak_malloc(sizeof(char *) * cap);
    if (!arr) { FindClose(h); return; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (n >= cap) {
            cap *= 2;
            arr = (char **)tak_realloc(arr, sizeof(char *) * cap);
            if (!arr) { FindClose(h); return; }
        }
        size_t L = strlen(fd.cFileName);
        char *p = (char *)tak_malloc(L + 1);
        if (!p) continue;
        memcpy(p, fd.cFileName, L);
        p[L] = 0;
        arr[n++] = p;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(arr, n, sizeof(char *), str_cmp_qsort);
    *out_subdirs = arr;
    *out_count = n;
#else
    DIR *d = opendir(abs_dir);
    if (!d) return;
    int cap = 16, n = 0;
    char **arr = (char **)tak_malloc(sizeof(char *) * cap);
    if (!arr) { closedir(d); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", abs_dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (n >= cap) {
            cap *= 2;
            arr = (char **)tak_realloc(arr, sizeof(char *) * cap);
            if (!arr) { closedir(d); return; }
        }
        size_t L = strlen(ent->d_name);
        char *p = (char *)tak_malloc(L + 1);
        if (!p) continue;
        memcpy(p, ent->d_name, L);
        p[L] = 0;
        arr[n++] = p;
    }
    closedir(d);
    qsort(arr, n, sizeof(char *), str_cmp_qsort);
    *out_subdirs = arr;
    *out_count = n;
#endif
}

int Features_LoadAll(void) {
    Features_FreeAll();

    char root_abs[256];
    snprintf(root_abs, sizeof(root_abs), "%s/data/features", TAK_DATA_DIR);

    /* Walk subdirectories of data/features/ alphabetically. Within
     * each, walk *.tdf alphabetically. Within each TDF, walk every
     * top-level [section] in declared order. This matches the legacy
     * load order so the TNT feature_layer's IDs line up with our
     * registry indices. */
    char **subs = NULL;
    int n_subs = 0;
    scan_dir_subdirs(root_abs, &subs, &n_subs);
    for (int i = 0; i < n_subs; i++) {
        char abs_sub[300];
        char vfs_sub[160];
        snprintf(abs_sub, sizeof(abs_sub), "%s/%s", root_abs, subs[i]);
        snprintf(vfs_sub, sizeof(vfs_sub), "data/features/%s", subs[i]);

        char **files = NULL;
        int n_files = 0;
        scan_dir_files(abs_sub, vfs_sub, "tdf", &files, &n_files);
        for (int j = 0; j < n_files; j++) {
            int added = parse_feature_tdf(files[j]);
            (void)added;
            tak_free(files[j]);
        }
        tak_free(files);
        tak_free(subs[i]);
    }
    tak_free(subs);

    fprintf(stderr, "Features_LoadAll: %d feature defs registered\n",
            g_feat_count);
    return g_feat_count;
}

const FeatureDef *Features_GetByIndex(int idx) {
    if (idx < 0 || idx >= g_feat_count) return NULL;
    return &g_feats[idx];
}

int Features_GetCount(void) { return g_feat_count; }

int Features_FindByName(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_feat_count; i++) {
#ifdef _WIN32
        if (_stricmp(g_feats[i].name, name) == 0) return i;
#else
        if (strcasecmp(g_feats[i].name, name) == 0) return i;
#endif
    }
    return -1;
}

void Features_FreeAll(void) {
    if (g_feats) {
        tak_free(g_feats);
        g_feats = NULL;
    }
    g_feat_count = 0;
    g_feat_cap = 0;
}
