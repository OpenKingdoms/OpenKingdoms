/*
 * mod_install.c -- a registry mod's zip into the mod root, and back out.
 *
 * See tak_mod_install.h. web/mods.js is the browser's copy of these
 * rules, so a change here is a change there.
 */

#include "tak_mod_install.h"
#include "tak_modset.h"

#include "miniz.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MAX_ENTRIES     4096
#define MAX_UNPACKED    (256u << 20)
#define REL_MAX         400

static const char *const k_mods = "Mods/";
static const char *const k_presets = "TAKEnhanced/Presets/";

static void say(char *why, size_t cap, const char *fmt, const char *a, const char *b) {
    if (why && cap) snprintf(why, cap, fmt, a ? a : "", b ? b : "");
}

static int prefix_ci(const char *s, const char *pre) {
    for (; *pre; s++, pre++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    return 1;
}

int TAK_ModInstall_Rel(const char *name, char *out, size_t cap) {
    char p[REL_MAX];
    size_t n = strlen(name);
    if (n == 0 || n >= sizeof p) return -1;
    for (size_t i = 0; i <= n; i++) p[i] = name[i] == '\\' ? '/' : name[i];
    if (p[0] == '/' || strchr(p, ':')) return -1;
    for (const char *seg = p; *seg;) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (len == 0 && slash) return -1;
        if ((len == 1 && seg[0] == '.') || (len == 2 && seg[0] == '.' && seg[1] == '.')) return -1;
        for (size_t i = 0; i < len; i++)
            if ((unsigned char)seg[i] < 0x20) return -1;
        if (!slash) break;
        seg = slash + 1;
    }
    if (p[n - 1] == '/') return 0;
    const char *pre = prefix_ci(p, k_mods) ? k_mods : prefix_ci(p, k_presets) ? k_presets : NULL;
    if (!pre || !p[strlen(pre)]) return 0;
    if (snprintf(out, cap, "%s%s", pre, p + strlen(pre)) >= (int)cap) return -1;
    return 1;
}

/* ── files ─────────────────────────────────────────────────────────── */

static int make_dir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static void parent_dirs(const char *full) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", full);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        make_dir(buf);
        *s = '/';
    }
}

static int write_file(const char *full, const void *bytes, size_t len) {
    parent_dirs(full);
    FILE *fp = fopen(full, "wb");
    if (!fp) return -1;
    int ok = fwrite(bytes, 1, len, fp) == len;
    if (fclose(fp) != 0) ok = 0;
    return ok ? 0 : -1;
}

static char *read_all(const char *full, size_t *len) {
    FILE *fp = fopen(full, "rb");
    if (!fp) return NULL;
    char *buf = NULL;
    long n = -1;
    if (fseek(fp, 0, SEEK_END) == 0) n = ftell(fp);
    if (n >= 0 && n < (1 << 20) && fseek(fp, 0, SEEK_SET) == 0 && (buf = (char *)malloc((size_t)n + 1))) {
        if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); buf = NULL; }
        else { buf[n] = '\0'; if (len) *len = (size_t)n; }
    }
    fclose(fp);
    return buf;
}

/* Removes what a failed install wrote, and the folders it made empty. */
static void remove_rel(const char *root, const char *rel) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    remove(full);
    for (char *s = strrchr(full, '/'); s && s > full; s = strrchr(full, '/')) {
        *s = '\0';
        size_t rl = strlen(root);
        if (strlen(full) <= rl + 1 + 4) break;           /* root/Mods */
        if (strcmp(full + rl + 1, "TAKEnhanced/Presets") == 0) break;
#ifdef _WIN32
        if (_rmdir(full) != 0) break;
#else
        if (rmdir(full) != 0) break;
#endif
    }
}

typedef struct {
    char (*rel)[REL_MAX];
    int  n, cap;
} RelList;

static int list_add(RelList *l, const char *rel) {
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->rel[i], rel) == 0) return 0;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        void *grown = realloc(l->rel, (size_t)cap * sizeof *l->rel);
        if (!grown) return -1;
        l->rel = (char (*)[REL_MAX])grown;
        l->cap = cap;
    }
    snprintf(l->rel[l->n++], REL_MAX, "%s", rel);
    return 0;
}

static void receipt_path(const char *root, const char *id, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s%s%s", root, k_mods, id, TAK_MODINSTALL_RECEIPT);
}

/* file= lines of a receipt's text into l, and its version. */
static void read_receipt(const char *text, RelList *l, char *version, size_t vcap) {
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len && line[len - 1] == '\r') len--;
        char buf[REL_MAX + 16];
        if (len < sizeof buf) {
            memcpy(buf, line, len);
            buf[len] = '\0';
            char rel[REL_MAX];
            if (strncmp(buf, "file=", 5) == 0 && l && TAK_ModInstall_Rel(buf + 5, rel, sizeof rel) == 1)
                list_add(l, rel);
            if (strncmp(buf, "version=", 8) == 0 && version) snprintf(version, vcap, "%s", buf + 8);
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* ── install ───────────────────────────────────────────────────────── */

static void folder_id(const char *name, size_t n, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < cap; i++) {
        char c = (char)tolower((unsigned char)name[i]);
        out[j++] = isalnum((unsigned char)c) ? c : '-';
    }
    out[j] = '\0';
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* Where the manifest for e's mod set goes, from what the zip holds:
 * beside the preset whose id it is, or in the folder it names. */
static int manifest_rel(mz_zip_archive *z, const int *index, const RelList *keep,
                        const TAK_ModEntry *e, char *out, size_t cap) {
    for (int i = 0; i < keep->n; i++) {
        const char *rel = keep->rel[i];
        if (strncmp(rel, k_presets, strlen(k_presets)) != 0) continue;
        const char *file = rel + strlen(k_presets);
        size_t fl = strlen(file);
        if (strchr(file, '/') || fl < 6 || !ieq(file + fl - 5, ".json")) continue;
        size_t jl = 0;
        char *json = (char *)mz_zip_reader_extract_to_heap(z, (mz_uint)index[i], &jl, 0);
        if (!json) continue;
        TAK_ModSet set;
        int ok = TAK_ModSet_ParsePreset(json, jl, "Mods", &set) == 0 && ieq(set.id, e->modset);
        mz_free(json);
        if (!ok) continue;
        size_t stem = fl - (fl > 12 && ieq(file + fl - 12, ".preset.json") ? 12 : 5);
        snprintf(out, cap, "%s%.*s.mod.tdf", k_presets, (int)stem, file);
        return 0;
    }
    for (int i = 0; i < keep->n; i++) {
        const char *rel = keep->rel[i];
        if (strncmp(rel, k_mods, strlen(k_mods)) != 0) continue;
        const char *dir = rel + strlen(k_mods);
        const char *slash = strchr(dir, '/');
        if (!slash) continue;
        char id[64];
        folder_id(dir, (size_t)(slash - dir), id, sizeof id);
        if (strcmp(id, e->modset) != 0) continue;
        snprintf(out, cap, "%s%.*s/mod.tdf", k_mods, (int)(slash - dir), dir);
        return 0;
    }
    return -1;
}

int TAK_ModInstall_Zip(const char *root, const TAK_ModEntry *e,
                       const void *zip, size_t len, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    if (!root || !e || !zip) return -1;
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_mem(&z, zip, len, 0)) {
        say(why, cap, "The download of %s is not a zip.", e->name, NULL);
        return -1;
    }
    int rc = -1;
    RelList keep = { 0 }, wrote = { 0 };
    int *index = NULL;
    mz_uint n = mz_zip_reader_get_num_files(&z);
    mz_uint64 total = 0;
    if (n == 0 || n > MAX_ENTRIES || !(index = (int *)malloc(n * sizeof *index))) {
        say(why, cap, "The download of %s holds no files or too many.", e->name, NULL);
        goto out;
    }
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) goto bad_zip;
        char rel[REL_MAX];
        int k = TAK_ModInstall_Rel(st.m_filename, rel, sizeof rel);
        if (k < 0) {
            say(why, cap, "The download of %s holds a file that would land outside "
                "the game folder (%s). It was not installed.", e->name, st.m_filename);
            goto out;
        }
        if (k == 0 || mz_zip_reader_is_file_a_directory(&z, i)) continue;
        total += st.m_uncomp_size;
        if (total > MAX_UNPACKED) {
            say(why, cap, "The download of %s unpacks larger than a mod should.", e->name, NULL);
            goto out;
        }
        index[keep.n] = (int)i;
        if (list_add(&keep, rel) != 0) goto bad_zip;
    }
    char manifest[REL_MAX];
    if (keep.n == 0 || manifest_rel(&z, index, &keep, e, manifest, sizeof manifest) != 0) {
        say(why, cap, "The download of %s has no mod set %s in it.", e->name, e->modset);
        goto out;
    }
    for (int i = 0; i < keep.n; i++) {
        size_t sz = 0;
        void *bytes = mz_zip_reader_extract_to_heap(&z, (mz_uint)index[i], &sz, 0);
        if (!bytes) goto bad_zip;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", root, keep.rel[i]);
        int w = write_file(full, bytes, sz);
        mz_free(bytes);
        if (w != 0 || list_add(&wrote, keep.rel[i]) != 0) {
            say(why, cap, "Could not write %s into the game folder.", keep.rel[i], NULL);
            goto undo;
        }
    }
    {
        char text[512], full[1024];
        int tl = snprintf(text, sizeof text,
                          "[MOD]\n{\nname=%s;\nversion=%s;\nfingerprint=%016llx;\n}\n",
                          e->name, e->version, (unsigned long long)e->fingerprint);
        snprintf(full, sizeof full, "%s/%s", root, manifest);
        if (tl < 0 || write_file(full, text, (size_t)tl) != 0 || list_add(&wrote, manifest) != 0) {
            say(why, cap, "Could not write %s into the game folder.", manifest, NULL);
            goto undo;
        }
    }
    {
        size_t rcap = 256 + (size_t)wrote.n * (REL_MAX + 8);
        char *text = (char *)malloc(rcap);
        char full[1024];
        receipt_path(root, e->id, full, sizeof full);
        size_t tl = 0;
        if (text) {
            tl += (size_t)snprintf(text, rcap,
                                   "# Installed from the OpenKingdoms mod registry. Removing it deletes these files.\n"
                                   "id=%s\nversion=%s\n", e->id, e->version);
            for (int i = 0; i < wrote.n; i++)
                tl += (size_t)snprintf(text + tl, rcap - tl, "file=%s\n", wrote.rel[i]);
        }
        int w = text ? write_file(full, text, tl) : -1;
        free(text);
        if (w != 0) {
            say(why, cap, "Could not write the list of %s's files.", e->name, NULL);
            goto undo;
        }
    }
    rc = 0;
    goto out;
bad_zip:
    say(why, cap, "The download of %s is a damaged zip.", e->name, NULL);
undo:
    for (int i = wrote.n - 1; i >= 0; i--) remove_rel(root, wrote.rel[i]);
out:
    free(index);
    free(keep.rel);
    free(wrote.rel);
    mz_zip_reader_end(&z);
    return rc;
}

/* ── remove ────────────────────────────────────────────────────────── */

/* The other receipts' files, so a file two mods ship stays for the one
 * still installed. */
static void others(const char *root, const char *id, RelList *l) {
    char dir[1024], mine[256];
    snprintf(dir, sizeof dir, "%s/Mods", root);
    snprintf(mine, sizeof mine, "%s%s", id, TAK_MODINSTALL_RECEIPT);
    const size_t sl = strlen(TAK_MODINSTALL_RECEIPT);
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s/*%s", dir, TAK_MODINSTALL_RECEIPT);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *ent; (ent = readdir(d)) != NULL;) {
        const char *name = ent->d_name;
#endif
        size_t nl = strlen(name);
        if (nl > sl && strcmp(name + nl - sl, TAK_MODINSTALL_RECEIPT) == 0 && strcmp(name, mine) != 0) {
            char full[1400];
            snprintf(full, sizeof full, "%s/%s", dir, name);
            char *text = read_all(full, NULL);
            if (text) { read_receipt(text, l, NULL, 0); free(text); }
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
}

int TAK_ModInstall_Remove(const char *root, const char *id, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    char full[1024];
    receipt_path(root, id, full, sizeof full);
    char *text = read_all(full, NULL);
    if (!text) {
        say(why, cap, "%s was not installed from the registry.", id, NULL);
        return -1;
    }
    RelList mine = { 0 }, kept = { 0 };
    read_receipt(text, &mine, NULL, 0);
    free(text);
    others(root, id, &kept);
    for (int i = 0; i < mine.n; i++) {
        int shared = 0;
        for (int k = 0; k < kept.n && !shared; k++) shared = strcmp(kept.rel[k], mine.rel[i]) == 0;
        if (!shared) remove_rel(root, mine.rel[i]);
    }
    free(mine.rel);
    free(kept.rel);
    if (remove(full) != 0) {
        say(why, cap, "Could not remove the list of %s's files.", id, NULL);
        return -1;
    }
    return 0;
}

int TAK_ModInstall_Installed(const char *root, const char *id, char *version, size_t cap) {
    char full[1024];
    receipt_path(root, id, full, sizeof full);
    char *text = read_all(full, NULL);
    if (!text) return 0;
    if (version && cap) version[0] = '\0';
    read_receipt(text, NULL, version, cap);
    free(text);
    return 1;
}
