/*
 * modset.c -- the mod sets under a mod root, see tak_modset.h.
 *
 * Runs before the VFS is mounted, so it reads the disk itself.
 */

#include "tak_modset.h"
#include "tak_memory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

/* ── small helpers ─────────────────────────────────────────────────── */

static void copy_str(char *dst, size_t cap, const char *src, size_t n) {
    if (!cap) return;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && ieq(s + n - m, suffix);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static int is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && !(st.st_mode & S_IFDIR);
}

static char *read_text(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len < 0 || len > 1 << 20) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)tak_malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* The names in a folder, files or folders, sorted case folded. */
typedef struct NameList { char name[64][TAK_MODSET_PATH_MAX]; int n; } NameList;

static int name_cmp(const void *a, const void *b) {
    const char *x = (const char *)a, *y = (const char *)b;
    for (; *x && *y; x++, y++) {
        int d = tolower((unsigned char)*x) - tolower((unsigned char)*y);
        if (d) return d;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

static void list_names(const char *dir, int want_dirs, NameList *out) {
    out->n = 0;
#ifdef _WIN32
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        int d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d != want_dirs || out->n >= 64) continue;
        copy_str(out->name[out->n++], TAK_MODSET_PATH_MAX, fd.cFileName, strlen(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || out->n >= 64) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (is_dir(full) != want_dirs) continue;
        copy_str(out->name[out->n++], TAK_MODSET_PATH_MAX, e->d_name, strlen(e->d_name));
    }
    closedir(d);
#endif
    if (out->n > 1) qsort(out->name, (size_t)out->n, TAK_MODSET_PATH_MAX, name_cmp);
}

/* ── a TAK Enhanced preset ─────────────────────────────────────────── */

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

/* A JSON string at p (on its opening quote) into out. Returns the
 * character after the closing quote, or NULL. */
static const char *read_json_string(const char *p, const char *end, char *out, size_t cap) {
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t n = 0;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            char e = *p++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n] = '\0';
    return p < end ? p + 1 : NULL;
}

/* The value after "key": within [p, end), or NULL. */
static const char *find_key(const char *p, const char *end, const char *key) {
    size_t k = strlen(key);
    for (; p + k + 2 <= end; p++) {
        if (*p != '"' || strncmp(p + 1, key, k) != 0 || p[k + 1] != '"') continue;
        const char *q = skip_ws(p + k + 2, end);
        if (q < end && *q == ':') return skip_ws(q + 1, end);
    }
    return NULL;
}

/* The end of the object or array opening at p. */
static const char *match_close(const char *p, const char *end) {
    char open = *p, close = open == '{' ? '}' : ']';
    int depth = 0, in_str = 0;
    for (; p < end; p++) {
        if (in_str) {
            if (*p == '\\') p++;
            else if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') in_str = 1;
        else if (*p == open) depth++;
        else if (*p == close && --depth == 0) return p;
    }
    return NULL;
}

int TAK_ModSet_ParsePreset(const char *json, size_t len, const char *mods_dir,
                           TAK_ModSet *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof *out);
    const char *end = json + len;
    const char *v = find_key(json, end, "id");
    if (!v || !read_json_string(v, end, out->id, sizeof out->id) || !out->id[0]) return -1;
    v = find_key(json, end, "name");
    if (!v || !read_json_string(v, end, out->name, sizeof out->name)) copy_str(out->name, sizeof out->name, out->id, strlen(out->id));
    copy_str(out->kind, sizeof out->kind, "preset", 6);

    const char *mods = find_key(json, end, "mods");
    if (!mods || *mods != '{') return -1;
    const char *mods_end = match_close(mods, end);
    if (!mods_end) return -1;
    const char *en = find_key(mods, mods_end, "enabled");
    if (!en || strncmp(en, "true", 4) != 0) return -1;
    const char *arr = find_key(mods, mods_end, "selectedMods");
    if (!arr || *arr != '[') return -1;
    const char *arr_end = match_close(arr, mods_end + 1);
    if (!arr_end) return -1;
    const char *p = skip_ws(arr + 1, arr_end);
    while (p < arr_end && *p == '"' && out->count < TAK_MODSET_PATHS) {
        char file[128];
        p = read_json_string(p, arr_end, file, sizeof file);
        if (!p) break;
        if (file[0] && !strchr(file, '/') && !strchr(file, '\\') && !strstr(file, "..")) {
            snprintf(out->path[out->count], TAK_MODSET_PATH_MAX, "%s/%s",
                     mods_dir ? mods_dir : "Mods", file);
            out->count++;
        }
        p = skip_ws(p, arr_end);
        if (p < arr_end && *p == ',') p = skip_ws(p + 1, arr_end);
    }
    return out->count > 0 ? 0 : -1;
}

/* ── a folder of its own ───────────────────────────────────────────── */

/* Sixteen hex digits at most, 0 for anything else. */
static uint64_t parse_fingerprint(const char *p, size_t n) {
    uint64_t v = 0;
    int digits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) {
            if (c == ' ' || c == '\t') continue;
            return 0;
        }
        if (++digits > 16) return 0;
        v = (v << 4) | (uint64_t)d;
    }
    return v;
}

/* name=, version= and fingerprint=, any section. */
void TAK_ModSet_ReadManifest(const char *text, TAK_ModSet *m) {
    if (!text || !m) return;
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        while (*line == ' ' || *line == '\t') line++;
        char *dst = NULL;
        size_t cap = 0, kl = 0;
        if (strncmp(line, "name=", 5) == 0) { dst = m->name; cap = sizeof m->name; kl = 5; }
        else if (strncmp(line, "version=", 8) == 0) { dst = m->version; cap = sizeof m->version; kl = 8; }
        if (dst && !dst[0]) copy_str(dst, cap, line + kl, strcspn(line + kl, ";\r\n"));
        if (strncmp(line, "fingerprint=", 12) == 0 && !m->fingerprint)
            m->fingerprint = parse_fingerprint(line + 12, strcspn(line + 12, ";\r\n"));
        line = nl ? nl + 1 : NULL;
    }
}

static void read_mod_tdf(const char *path, TAK_ModSet *m) {
    size_t len = 0;
    char *t = read_text(path, &len);
    if (!t) return;
    TAK_ModSet_ReadManifest(t, m);
    tak_free(t);
}

/* A preset's manifest, beside it as <stem>.mod.tdf, over what the
 * preset itself says. */
static void read_preset_manifest(const char *dir, const char *file, TAK_ModSet *m) {
    size_t n = strlen(file);
    if (ends_with(file, ".preset.json")) n -= 12;
    else if (ends_with(file, ".json")) n -= 5;
    char path[TAK_MODSET_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/%.*s.mod.tdf", dir, (int)n, file);
    TAK_ModSet t;
    memset(&t, 0, sizeof t);
    read_mod_tdf(path, &t);
    if (t.name[0]) memcpy(m->name, t.name, sizeof m->name);
    if (t.version[0]) memcpy(m->version, t.version, sizeof m->version);
    if (t.fingerprint) m->fingerprint = t.fingerprint;
}

static void folder_id(const char *name, char *out, size_t cap) {
    size_t j = 0;
    for (const char *p = name; *p && j + 1 < cap; p++) {
        char c = (char)tolower((unsigned char)*p);
        out[j++] = isalnum((unsigned char)c) ? c : '-';
    }
    out[j] = '\0';
}

static int scan_folder_mod(const char *mods_dir, const char *name, TAK_ModSet *m) {
    char dir[TAK_MODSET_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", mods_dir, name);
    memset(m, 0, sizeof *m);
    folder_id(name, m->id, sizeof m->id);
    copy_str(m->kind, sizeof m->kind, "folder", 6);
    char tdf[TAK_MODSET_PATH_MAX + 16];
    snprintf(tdf, sizeof tdf, "%s/mod.tdf", dir);
    read_mod_tdf(tdf, m);
    if (!m->name[0]) copy_str(m->name, sizeof m->name, name, strlen(name));
    NameList files;
    list_names(dir, 0, &files);
    for (int i = 0; i < files.n && m->count < TAK_MODSET_PATHS - 1; i++) {
        if (!ends_with(files.name[i], ".hpi") && !ends_with(files.name[i], ".ufo")) continue;
        snprintf(m->path[m->count++], TAK_MODSET_PATH_MAX, "%s/%s", dir, files.name[i]);
    }
    /* Then the folder itself, loose, over its own archives. */
    snprintf(m->path[m->count++], TAK_MODSET_PATH_MAX, "%s", dir);
    return 0;
}

/* ── the scan ──────────────────────────────────────────────────────── */

int TAK_ModSet_Scan(const char *root, TAK_ModSet *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    memset(&out[n], 0, sizeof out[n]);
    copy_str(out[n].id, sizeof out[n].id, "vanilla", 7);
    copy_str(out[n].name, sizeof out[n].name, "Vanilla", 7);
    copy_str(out[n].kind, sizeof out[n].kind, "game", 4);
    n++;
    if (!root || !root[0]) return n;

    char mods_dir[TAK_MODSET_PATH_MAX];
    snprintf(mods_dir, sizeof mods_dir, "%s/Mods", root);

    char presets[TAK_MODSET_PATH_MAX];
    snprintf(presets, sizeof presets, "%s/TAKEnhanced/Presets", root);
    NameList files;
    list_names(presets, 0, &files);
    for (int i = 0; i < files.n && n < cap; i++) {
        if (!ends_with(files.name[i], ".json")) continue;
        char path[TAK_MODSET_PATH_MAX * 2];
        snprintf(path, sizeof path, "%s/%s", presets, files.name[i]);
        size_t len = 0;
        char *json = read_text(path, &len);
        if (!json) continue;
        TAK_ModSet m;
        int rc = TAK_ModSet_ParsePreset(json, len, mods_dir, &m);
        tak_free(json);
        if (rc != 0 || ieq(m.id, "vanilla") || TAK_ModSet_Find(out, n, m.id)) continue;
        read_preset_manifest(presets, files.name[i], &m);
        /* Keep what is there, and say what is not. */
        int kept = 0;
        for (int k = 0; k < m.count; k++) {
            if (!is_file(m.path[k])) { m.missing++; continue; }
            if (kept != k) memcpy(m.path[kept], m.path[k], TAK_MODSET_PATH_MAX);
            kept++;
        }
        m.count = kept;
        if (m.count > 0) out[n++] = m;
    }

    NameList dirs;
    list_names(mods_dir, 1, &dirs);
    for (int i = 0; i < dirs.n && n < cap; i++) {
        TAK_ModSet m;
        if (scan_folder_mod(mods_dir, dirs.name[i], &m) != 0) continue;
        if (TAK_ModSet_Find(out, n, m.id)) continue;
        out[n++] = m;
    }
    return n;
}

const TAK_ModSet *TAK_ModSet_Find(const TAK_ModSet *sets, int n, const char *id) {
    if (!sets || !id) return NULL;
    for (int i = 0; i < n; i++)
        if (ieq(sets[i].id, id)) return &sets[i];
    return NULL;
}

/* ── what the game is running ──────────────────────────────────────── */

static char g_active_id[64] = "vanilla";
static char g_active_name[128] = "Vanilla";
static char g_active_mod[96] = "Vanilla";
static char g_active_version[32] = "";
static uint64_t g_active_fingerprint;

void TAK_ModSet_SetActive(const TAK_ModSet *set) {
    if (!set) {
        copy_str(g_active_id, sizeof g_active_id, "vanilla", 7);
        copy_str(g_active_name, sizeof g_active_name, "Vanilla", 7);
        copy_str(g_active_mod, sizeof g_active_mod, "Vanilla", 7);
        g_active_version[0] = '\0';
        g_active_fingerprint = 0;
        return;
    }
    g_active_fingerprint = set->fingerprint;
    copy_str(g_active_id, sizeof g_active_id, set->id, strlen(set->id));
    copy_str(g_active_mod, sizeof g_active_mod, set->name, strlen(set->name));
    copy_str(g_active_version, sizeof g_active_version, set->version, strlen(set->version));
    TAK_ModSet_Label(set->name, set->version, g_active_name, sizeof g_active_name);
}

const char *TAK_ModSet_ActiveId(void) { return g_active_id; }
const char *TAK_ModSet_ActiveName(void) { return g_active_name; }
const char *TAK_ModSet_ActiveModName(void) { return g_active_mod; }
const char *TAK_ModSet_ActiveVersion(void) { return g_active_version; }
uint64_t TAK_ModSet_ActiveFingerprint(void) { return g_active_fingerprint; }
int TAK_ModSet_IsVanilla(void) { return ieq(g_active_id, "vanilla"); }

static char g_root[TAK_MODSET_PATH_MAX] = ".";

void TAK_ModSet_SetRoot(const char *root) {
    copy_str(g_root, sizeof g_root, root ? root : ".", root ? strlen(root) : 1);
}

const char *TAK_ModSet_Root(void) { return g_root; }

/* ── what is installed, for the lobby ──────────────────────────────── */

typedef struct Installed {
    char     id[64];
    char     name[96];
    char     version[32];
    uint64_t fingerprint;
} Installed;

static Installed g_installed[TAK_MODSET_MAX];
static int       g_installed_n;

void TAK_ModSet_SetInstalled(const TAK_ModSet *sets, int n) {
    g_installed_n = 0;
    for (int i = 0; sets && i < n && i < TAK_MODSET_MAX; i++) {
        Installed *x = &g_installed[g_installed_n++];
        memcpy(x->id, sets[i].id, sizeof x->id);
        memcpy(x->name, sets[i].name, sizeof x->name);
        memcpy(x->version, sets[i].version, sizeof x->version);
        x->fingerprint = sets[i].fingerprint;
    }
}

void TAK_ModSet_Label(const char *name, const char *version, char *out, size_t cap) {
    if (!out || !cap) return;
    if (!name || !name[0]) { out[0] = '\0'; return; }
    if (version && version[0]) snprintf(out, cap, "%s %s", name, version);
    else snprintf(out, cap, "%s", name);
}

void TAK_ModSet_JoinAdvice(const char *room_mod, const char *room_version,
                           uint64_t room_content, char *out, size_t cap) {
    if (!out || !cap) return;
    const char *rv = room_version ? room_version : "";
    char want[160];
    TAK_ModSet_Label(room_mod, rv, want, sizeof want);
    if (!want[0]) {
        snprintf(out, cap, "That game's data differs from yours (%s).", g_active_name);
        return;
    }
    /* Nothing scanned is still the game itself, and whatever is mounted. */
    Installed fallback[2];
    const Installed *list = g_installed;
    int n = g_installed_n;
    if (n == 0) {
        memset(fallback, 0, sizeof fallback);
        copy_str(fallback[0].id, sizeof fallback[0].id, "vanilla", 7);
        copy_str(fallback[0].name, sizeof fallback[0].name, "Vanilla", 7);
        copy_str(fallback[1].id, sizeof fallback[1].id, g_active_id, strlen(g_active_id));
        copy_str(fallback[1].name, sizeof fallback[1].name, g_active_mod, strlen(g_active_mod));
        copy_str(fallback[1].version, sizeof fallback[1].version,
                 g_active_version, strlen(g_active_version));
        list = fallback;
        n = TAK_ModSet_IsVanilla() ? 1 : 2;
    }
    /* The manifest's fingerprint says it for certain, the name and
     * version say it by what the author typed. */
    const Installed *same = NULL, *named = NULL;
    for (int i = 0; i < n && !same; i++)
        if (room_content && list[i].fingerprint == room_content) same = &list[i];
    for (int i = 0; i < n && !same; i++) {
        if (!ieq(list[i].name, room_mod)) continue;
        if (ieq(list[i].version, rv) && !(list[i].fingerprint && room_content))
            same = &list[i];
        else if (!named || ieq(list[i].id, g_active_id)) named = &list[i];
    }
    int vanilla = ieq(room_mod, "vanilla");
    if (same && ieq(same->id, g_active_id)) {
        if (vanilla)
            snprintf(out, cap, "That game plays Vanilla on game data other than yours.");
        else
            snprintf(out, cap, "That game plays %s, and your copy of it differs.", want);
    } else if (same && ieq(same->name, room_mod)) {
        snprintf(out, cap, "That game plays %s. Choose it as your mod set to join.", want);
    } else if (same) {
        char have[160];
        TAK_ModSet_Label(same->name, same->version, have, sizeof have);
        snprintf(out, cap, "That game plays %s, which is your %s. Choose it to join.", want, have);
    } else if (named && ieq(named->version, rv)) {
        snprintf(out, cap, "That game plays %s, and your copy of it differs.", want);
    } else if (named) {
        char have[160];
        TAK_ModSet_Label(named->name, named->version, have, sizeof have);
        snprintf(out, cap, "That game plays %s, and you have %s.", want, have);
    } else {
        snprintf(out, cap, "That game plays %s, which you do not have.", want);
    }
}
