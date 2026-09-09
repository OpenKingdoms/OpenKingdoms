#include "tak_hpi.h"
#include "tak_util.h"
#include "tak_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <fnmatch.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Platform directory scanning
 *
 *  scan_directory  -- flat scan: returns all files matching an extension
 *  walk_directory  -- recursive scan: returns all files matching a glob
 *
 *  Both return an allocated array of full paths. Caller frees each
 *  string and the array itself.
 * ═══════════════════════════════════════════════════════════════════ */

/* Dynamic string array used by scan/walk helpers */
typedef struct FileList {
    char **paths;
    size_t count;
    size_t capacity;
} FileList;

static int filelist_init(FileList *fl, size_t initial_cap) {
    fl->paths = (char **)tak_malloc(sizeof(char *) * initial_cap);
    if (!fl->paths) return -1;
    fl->count = 0;
    fl->capacity = initial_cap;
    return 0;
}

static int filelist_push(FileList *fl, const char *path) {
    if (fl->count == fl->capacity) {
        size_t new_cap = fl->capacity * 2;
        char **tmp = (char **)tak_realloc(fl->paths, sizeof(char *) * new_cap);
        if (!tmp) return -1;
        fl->paths = tmp;
        fl->capacity = new_cap;
    }
    fl->paths[fl->count] = tak_strdup(path);
    if (!fl->paths[fl->count]) return -1;
    fl->count++;
    return 0;
}

static void filelist_free(FileList *fl) {
    for (size_t i = 0; i < fl->count; i++) tak_free(fl->paths[i]);
    tak_free(fl->paths);
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

static int has_extension(const char *name, const char *ext) {
    size_t name_len = strlen(name);
    size_t ext_len = strlen(ext);
    if (name_len < ext_len) return 0;
    return tak_strnicmp(name + name_len - ext_len, ext, ext_len) == 0;
}

/* Scan a single directory (non-recursive) for files with a given extension.
 * Returns 0 on success, -1 on failure.
 * On success, *out_files is a malloc'd array of strdup'd full paths,
 * and *out_count is the number of entries. Caller frees both. */
static int scan_directory(const char *dir, const char *extension,
                          char ***out_files, int *out_count) {
    FileList fl;
    if (filelist_init(&fl, 16) != 0) return -1;

#ifdef _WIN32
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        tak_free(fl.paths);
        return -1;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_extension(fd.cFileName, extension)) continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
        if (filelist_push(&fl, full_path) != 0) {
            filelist_free(&fl);
            FindClose(h);
            return -1;
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { tak_free(fl.paths); return -1; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type == DT_DIR) continue;
        if (!has_extension(ent->d_name, extension)) continue;

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);
        if (filelist_push(&fl, full_path) != 0) {
            filelist_free(&fl);
            closedir(d);
            return -1;
        }
    }

    closedir(d);
#endif

    *out_files = fl.paths;
    *out_count = (int)fl.count;
    return 0;
}

/* Walk a directory tree recursively, collecting all non-directory files
 * matching a glob pattern. Patterns WITHOUT a '/' match against the
 * leaf filename (historic behavior); patterns WITH a '/' match against
 * the path relative to the walk root — this is what makes
 * VFS_ListFiles("data/canbuild/x/*.tdf") work for loose files (the old
 * leaf-only match silently found nothing for subdirectory globs).
 * Case-insensitive. Returns 0 on success, -1 on failure. */
static int walk_directory_impl(const char *dir, const char *rel_prefix,
                               const char *glob_pattern, FileList *fl) {
    const int pattern_has_dir = (strchr(glob_pattern, '/') != NULL);
#ifdef _WIN32
    char search[MAX_PATH];
    snprintf(search, sizeof(search), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0; /* empty or inaccessible */

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
        char rel_path[MAX_PATH];
        if (rel_prefix[0])
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, fd.cFileName);
        else
            snprintf(rel_path, sizeof(rel_path), "%s", fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (walk_directory_impl(full_path, rel_path, glob_pattern, fl) != 0) {
                FindClose(h);
                return -1;
            }
        } else {
            char *norm = normalize_path(pattern_has_dir ? rel_path : fd.cFileName);
            if (norm) {
                if (glob_path_match(glob_pattern, norm)) {
                    if (filelist_push(fl, full_path) != 0) {
                        tak_free(norm);
                        FindClose(h);
                        return -1;
                    }
                }
                tak_free(norm);
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);
        char rel_path[4096];
        if (rel_prefix[0])
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, ent->d_name);
        else
            snprintf(rel_path, sizeof(rel_path), "%s", ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (walk_directory_impl(full_path, rel_path, glob_pattern, fl) != 0) {
                closedir(d);
                return -1;
            }
        } else {
            char *norm = normalize_path(pattern_has_dir ? rel_path : ent->d_name);
            if (norm) {
                if (glob_path_match(glob_pattern, norm)) {
                    if (filelist_push(fl, full_path) != 0) {
                        tak_free(norm);
                        closedir(d);
                        return -1;
                    }
                }
                tak_free(norm);
            }
        }
    }

    closedir(d);
#endif

    return 0;
}

static int walk_directory(const char *dir, const char *glob_pattern, char ***out_files, int *out_count) {
    FileList fl;
    if (filelist_init(&fl, 32) != 0) return -1;

    if (walk_directory_impl(dir, "", glob_pattern, &fl) != 0) {
        filelist_free(&fl);
        return -1;
    }

    *out_files = fl.paths;
    *out_count = (int)fl.count;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  VFS state
 * ═══════════════════════════════════════════════════════════════════ */

static char **archive_paths = NULL;
static size_t total_archive_count = 0;
static HPIArchive **archives = NULL;
static char *local_dir = NULL;
static int vfs_initialized = 0;

static int path_cmp(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return tak_stricmp(sa, sb);
}

static int build_loose_path(char *buf, size_t buf_size,
                            const char *dir, const char *relative) {
    int n = snprintf(buf, buf_size, "%s/%s", dir, relative);
    return (n > 0 && (size_t)n < buf_size) ? 0 : -1;
}

#ifdef _WIN32
/* NTFS lookups already ignore case. */
#define vfs_fixup_case(path) ((void)(path))
#else
/* Case-insensitive path resolution for case-sensitive filesystems
 * (Linux, Emscripten MEMFS). Game data and code paths were authored on
 * Windows where case never mattered, so mixed-case references are
 * common. When the exact path misses, resolve each component against
 * the actual directory listing and rewrite the buffer in place —
 * case-insensitively equal names always have equal length, so the
 * rewrite never grows the string. */
static int vfs_fixup_case_r(char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1;
    *slash = '\0';
    int parent_ok = vfs_fixup_case_r(path);
    if (parent_ok != 0) { *slash = '/'; return -1; }
    DIR *d = opendir(path);
    *slash = '/';
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (tak_stricmp(e->d_name, slash + 1) == 0) {
            strcpy(slash + 1, e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}
static void vfs_fixup_case(char *path) { (void)vfs_fixup_case_r(path); }
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

int VFS_Init(const char *game_dir, const char *loose_dir) {
    if (vfs_initialized) {
        fprintf(stderr, "VFS_Init called multiple times without shutdown!\n");
        return -1;
    }

    if (!game_dir) return -1;

    if (loose_dir) {
        local_dir = tak_strdup(loose_dir);
        if (!local_dir) return -1;
    }

    /* Scan game_dir for .hpi files (flat, not recursive) */
    char **found_files = NULL;
    int found_count = 0;
    if (scan_directory(game_dir, ".hpi", &found_files, &found_count) != 0) {
        fprintf(stderr, "VFS_Init: cannot open game directory: %s\n", game_dir);
        tak_free(local_dir);
        local_dir = NULL;
        return -1;
    }

    if (found_count == 0) {
        tak_free(found_files);
        found_files = NULL;
        if (!local_dir) {
            fprintf(stderr, "VFS_Init: no .hpi archives found in %s\n", game_dir);
            return -1;
        }
    }

    archive_paths = found_files;
    total_archive_count = (size_t)found_count;

    if (total_archive_count > 0) {
        qsort(archive_paths, total_archive_count, sizeof(char *), path_cmp);

        archives = (HPIArchive **)tak_calloc(total_archive_count, sizeof(HPIArchive *));
        if (!archives) goto fail;

        for (size_t i = 0; i < total_archive_count; i++) {
            archives[i] = HPI_OpenArchive(archive_paths[i]);
            if (!archives[i]) {
                fprintf(stderr, "VFS_Init: failed to open archive: %s\n",
                        archive_paths[i]);
                goto fail_archives;
            }
        }
    }

    vfs_initialized = 1;
    return 0;

fail_archives:
    for (size_t i = 0; i < total_archive_count; i++) {
        if (archives[i]) HPI_CloseArchive(archives[i]);
    }
    tak_free(archives);
    archives = NULL;

fail:
    for (size_t i = 0; i < total_archive_count; i++) {
        tak_free(archive_paths[i]);
    }
    tak_free(archive_paths);
    archive_paths = NULL;
    total_archive_count = 0;
    tak_free(local_dir);
    local_dir = NULL;
    vfs_initialized = 0;
    return -1;
}

void VFS_Shutdown(void) {
    if (!vfs_initialized) return;

    for (size_t i = 0; i < total_archive_count; i++) {
        if (archives && archives[i]) HPI_CloseArchive(archives[i]);
        if (archive_paths) tak_free(archive_paths[i]);
    }

    tak_free(archives);
    tak_free(archive_paths);
    tak_free(local_dir);

    archives = NULL;
    archive_paths = NULL;
    local_dir = NULL;
    total_archive_count = 0;
    vfs_initialized = 0;
}

int VFS_GetArchiveCount(void) {
    return (int)total_archive_count;
}

/* The loose dev tree was unpacked with data.hpi's contents under data/;
 * the archives carry no such prefix. Callers write the loose form, so
 * archive lookups also try the path with "data/" stripped. */
static const char *strip_data_prefix(const char *path) {
    if (!path || strlen(path) < 6) return NULL;
    if ((path[0] == 'd' || path[0] == 'D') && (path[1] == 'a' || path[1] == 'A') &&
        (path[2] == 't' || path[2] == 'T') && (path[3] == 'a' || path[3] == 'A') &&
        (path[4] == '/' || path[4] == '\\'))
        return path + 5;
    return NULL;
}

int VFS_FileExists(const char *path) {
    if (!path) return -1;
    if (!vfs_initialized) return -1;

    for (size_t i = total_archive_count; i-- > 0;) {
        if (HPI_FileExists(archives[i], path)) return 0;
    }

    if (local_dir) {
        char full[4096];
        if (build_loose_path(full, sizeof(full), local_dir, path) == 0) {
            vfs_fixup_case(full);
            struct stat st;
            if (stat(full, &st) == 0 && !(st.st_mode & S_IFDIR)) {
                return 0;
            }
        }
    }

    /* Last: the archives with the loose tree's data/ prefix stripped, so
     * an archive-only session (the browser) still resolves. The loose
     * tree keeps priority for these paths on a dev machine. */
    const char *alt = strip_data_prefix(path);
    if (alt) {
        for (size_t i = total_archive_count; i-- > 0;) {
            if (HPI_FileExists(archives[i], alt)) return 0;
        }
    }

    return -1;
}

int VFS_ReadFile(const char *path, void **out_data, uint32_t *out_size) {
    if (!path || !out_data || !out_size) return -1;
    if (!vfs_initialized) return -1;

    for (size_t i = total_archive_count; i-- > 0;) {
        if (HPI_FileExists(archives[i], path)) {
            return HPI_ReadFile(archives[i], path, out_data, out_size);
        }
    }

    if (local_dir) {
        char full[4096];
        if (build_loose_path(full, sizeof(full), local_dir, path) == 0) {
            vfs_fixup_case(full);
            FILE *fp = fopen(full, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long len = ftell(fp);
                if (len < 0) { fclose(fp); return -1; }
                fseek(fp, 0, SEEK_SET);

                *out_size = (uint32_t)len;
                *out_data = tak_malloc(*out_size);
                if (!*out_data) { fclose(fp); return -1; }

                size_t nread = fread(*out_data, 1, *out_size, fp);
                fclose(fp);

                if (nread != *out_size) {
                    tak_free(*out_data);
                    *out_data = NULL;
                    *out_size = 0;
                    return -1;
                }
                return 0;
            }
        }
    }

    /* Last: archives with the loose tree's data/ prefix stripped (see
     * VFS_FileExists). */
    const char *alt = strip_data_prefix(path);
    if (alt) {
        for (size_t i = total_archive_count; i-- > 0;) {
            if (HPI_FileExists(archives[i], alt)) {
                return HPI_ReadFile(archives[i], alt, out_data, out_size);
            }
        }
    }

    return -1;
}

int VFS_ListFiles(const char *pattern, char ***out_paths, int *out_count) {
    if (!pattern || !out_paths || !out_count) return -1;
    if (!vfs_initialized) return -1;

    *out_paths = NULL;
    *out_count = 0;

    size_t capacity = 64;
    size_t count = 0;
    char **result = (char **)tak_malloc(sizeof(char *) * capacity);
    if (!result) return -1;

    /* Search archives (last-loaded first for priority) */
    for (size_t i = total_archive_count; i-- > 0;) {
        char **arc_paths = NULL;
        int arc_count = 0;
        if (HPI_ListFiles(archives[i], pattern, &arc_paths, &arc_count) != 0)
            continue;

        for (int j = 0; j < arc_count; j++) {
            int duplicate = 0;
            for (size_t k = 0; k < count; k++) {
                if (tak_stricmp(result[k], arc_paths[j]) == 0) {
                    duplicate = 1;
                    break;
                }
            }

            if (!duplicate) {
                if (count == capacity) {
                    size_t new_cap = capacity * 2;
                    char **tmp = (char **)tak_realloc(result, sizeof(char *) * new_cap);
                    if (!tmp) goto fail;
                    result = tmp;
                    capacity = new_cap;
                }
                result[count] = tak_strdup(arc_paths[j]);
                if (!result[count]) goto fail;
                count++;
            }

            tak_free(arc_paths[j]);
        }
        tak_free(arc_paths);
    }

    /* Search loose directory recursively */
    if (local_dir) {
        char *norm_pattern = normalize_path(pattern);
        if (norm_pattern) {
            char **loose_files = NULL;
            int loose_count = 0;
            if (walk_directory(local_dir, norm_pattern,
                               &loose_files, &loose_count) == 0) {
                for (int i = 0; i < loose_count; i++) {
                    /* Extract relative path from the full path for dedup */
                    const char *rel = loose_files[i];
                    size_t dir_len = strlen(local_dir);
                    if (strncmp(rel, local_dir, dir_len) == 0) {
                        rel += dir_len;
                        if (*rel == '/' || *rel == '\\') rel++;
                    }
                    char *norm_rel = normalize_path(rel);

                    int duplicate = 0;
                    if (norm_rel) {
                        for (size_t k = 0; k < count; k++) {
                            if (tak_stricmp(result[k], norm_rel) == 0) {
                                duplicate = 1;
                                break;
                            }
                        }
                    }

                    if (!duplicate && norm_rel) {
                        if (count == capacity) {
                            size_t new_cap = capacity * 2;
                            char **tmp = (char **)tak_realloc(result,
                                                sizeof(char *) * new_cap);
                            if (!tmp) {
                                tak_free(norm_rel);
                                for (int j = i; j < loose_count; j++)
                                    tak_free(loose_files[j]);
                                tak_free(loose_files);
                                tak_free(norm_pattern);
                                goto fail;
                            }
                            result = tmp;
                            capacity = new_cap;
                        }
                        result[count] = norm_rel;
                        count++;
                    } else {
                        tak_free(norm_rel);
                    }

                    tak_free(loose_files[i]);
                }
                tak_free(loose_files);
            }
            tak_free(norm_pattern);
        }
    }

    /* Last resort, when neither the archives nor the loose tree matched:
     * archives carry no data/ prefix (see strip_data_prefix), so list with
     * it stripped and hand the paths back in the form the caller asked
     * for. Only then, so a dev tree's loose set stays authoritative. */
    const char *alt_pattern = (count == 0) ? strip_data_prefix(pattern) : NULL;
    for (size_t i = alt_pattern ? total_archive_count : 0; i-- > 0;) {
        char **arc_paths = NULL;
        int arc_count = 0;
        if (HPI_ListFiles(archives[i], alt_pattern, &arc_paths, &arc_count) != 0)
            continue;

        for (int j = 0; j < arc_count; j++) {
            size_t n = strlen(arc_paths[j]) + 6;
            char *withp = (char *)tak_malloc(n);
            if (withp) {
                memcpy(withp, "data/", 5);
                memcpy(withp + 5, arc_paths[j], n - 5);
            }
            int duplicate = (withp == NULL);
            for (size_t k = 0; !duplicate && k < count; k++) {
                if (tak_stricmp(result[k], withp) == 0) duplicate = 1;
            }
            if (!duplicate) {
                if (count == capacity) {
                    size_t new_cap = capacity * 2;
                    char **tmp = (char **)tak_realloc(result, sizeof(char *) * new_cap);
                    if (!tmp) { tak_free(withp); goto fail; }
                    result = tmp;
                    capacity = new_cap;
                }
                result[count++] = withp;
            } else {
                tak_free(withp);
            }
            tak_free(arc_paths[j]);
        }
        tak_free(arc_paths);
    }

    if (count == 0) {
        tak_free(result);
        *out_paths = NULL;
        *out_count = 0;
        return 0;
    }

    *out_paths = result;
    *out_count = (int)count;
    return 0;

fail:
    for (size_t i = 0; i < count; i++) tak_free(result[i]);
    tak_free(result);
    *out_paths = NULL;
    *out_count = 0;
    return -1;
}

void VFS_FreeBuffer(void *data) {
    tak_free(data);
}
