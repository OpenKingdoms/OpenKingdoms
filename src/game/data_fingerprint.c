/*
 * data_fingerprint.c -- what the simulation reads, hashed, see
 * tak_data_fingerprint.h.
 */

#include "tak_data_fingerprint.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_sha256.h"

#include <stdlib.h>
#include <string.h>

/* Where each group's files live, in the archive layout. The loose
 * layout is the same under a leading "data/", tried as well. A pattern
 * with no wildcard is one file. */
static const char *const k_units[] = {
    "units/*.fbi", "canbuild/*/*.tdf", "gamedata/sidedata.tdf",
    "gamedata/moveinfo.tdf", NULL
};
static const char *const k_weapons[] = {
    "gamedata/explosions/*.tdf", "weapons/*.tdf", NULL
};
static const char *const k_features[] = { "features/*/*.tdf", NULL };
static const char *const k_scripts[] = { "scripts/*.cob", NULL };
static const char *const k_ai[] = { "ai/*.txt", NULL };

static const char *const *const k_groups[TAK_DATA_GROUP_COUNT] = {
    k_units, k_weapons, k_features, k_scripts, k_ai
};
static const char *const k_group_names[TAK_DATA_GROUP_COUNT] = {
    "units", "weapons", "features", "scripts", "ai"
};

const char *TAK_DataFingerprint_GroupName(int group) {
    return (group >= 0 && group < TAK_DATA_GROUP_COUNT) ? k_group_names[group] : "";
}

/* One file of a group: the name it is hashed under, and the name the
 * VFS finds it by. */
typedef struct FpFile {
    char key[256];
    char path[256];
} FpFile;

typedef struct FpList {
    FpFile *f;
    int n, cap;
} FpList;

/* Case folded, forward slashes, and no leading "data/". */
static void fp_key(const char *path, char *out, size_t cap) {
    size_t j = 0;
    const char *p = path;
    if ((p[0] == 'd' || p[0] == 'D') && (p[1] == 'a' || p[1] == 'A') &&
        (p[2] == 't' || p[2] == 'T') && (p[3] == 'a' || p[3] == 'A') &&
        (p[4] == '/' || p[4] == '\\')) {
        p += 5;
    }
    for (; *p && j + 1 < cap; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[j++] = c;
    }
    out[j] = '\0';
}

static void fp_add(FpList *l, const char *path) {
    char key[256];
    fp_key(path, key, sizeof key);
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->f[i].key, key) == 0) return;   /* the other layout */
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        FpFile *nf = (FpFile *)tak_malloc((size_t)cap * sizeof(FpFile));
        if (!nf) return;
        if (l->f) {
            memcpy(nf, l->f, (size_t)l->n * sizeof(FpFile));
            tak_free(l->f);
        }
        l->f = nf;
        l->cap = cap;
    }
    FpFile *f = &l->f[l->n++];
    memcpy(f->key, key, sizeof f->key);
    strncpy(f->path, path, sizeof f->path - 1);
    f->path[sizeof f->path - 1] = '\0';
}

static void fp_collect(const char *pattern, FpList *l) {
    if (!strchr(pattern, '*')) {
        void *data = NULL;
        uint32_t size = 0;
        if (VFS_ReadFile(pattern, &data, &size) == 0) {
            VFS_FreeBuffer(data);
            fp_add(l, pattern);
        }
        return;
    }
    char **paths = NULL;
    int n = 0;
    if (VFS_ListFiles(pattern, &paths, &n) == 0) {
        for (int i = 0; i < n; i++) fp_add(l, paths[i]);
    }
    if (paths) {
        for (int i = 0; i < n; i++) tak_free(paths[i]);
        tak_free(paths);
    }
}

static int fp_cmp(const void *a, const void *b) {
    return strcmp(((const FpFile *)a)->key, ((const FpFile *)b)->key);
}

static void fp_list(int group, FpList *l) {
    memset(l, 0, sizeof *l);
    for (const char *const *p = k_groups[group]; *p; p++) {
        char loose[256];
        fp_collect(*p, l);
        snprintf(loose, sizeof loose, "data/%s", *p);
        fp_collect(loose, l);
    }
    if (l->n > 1) qsort(l->f, (size_t)l->n, sizeof(FpFile), fp_cmp);
}

static int fp_is_text(const char *key) {
    size_t n = strlen(key);
    return !(n >= 4 && strcmp(key + n - 4, ".cob") == 0);
}

/* A file into the running hash: its name, its length and its bytes,
 * text without its carriage returns. Writes the file's own hash when
 * asked. */
static void fp_file(TAK_Sha256 *group, const FpFile *f, uint8_t own[TAK_SHA256_BYTES]) {
    void *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(f->path, &data, &size) != 0) { data = NULL; size = 0; }
    const uint8_t *b = (const uint8_t *)data;
    uint8_t *clean = NULL;
    uint32_t len = size;
    if (b && fp_is_text(f->key)) {
        clean = (uint8_t *)tak_malloc(size ? size : 1);
        if (clean) {
            len = 0;
            for (uint32_t i = 0; i < size; i++)
                if (b[i] != '\r') clean[len++] = b[i];
            b = clean;
        }
    }
    uint8_t lenb[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16),
                        (uint8_t)(len >> 24) };
    TAK_Sha256_Update(group, f->key, strlen(f->key) + 1);
    TAK_Sha256_Update(group, lenb, sizeof lenb);
    if (b && len) TAK_Sha256_Update(group, b, len);
    if (own) TAK_Sha256_Hash(b ? b : (const uint8_t *)"", b ? len : 0, own);
    if (clean) tak_free(clean);
    if (data) VFS_FreeBuffer(data);
}

static uint64_t fp_first8(const uint8_t d[TAK_SHA256_BYTES]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | d[i];
    return v;
}

static int fp_run(TAK_DataFingerprint *out, FILE *report) {
    if (!out || !VFS_IsInitialized()) return -1;
    memset(out, 0, sizeof *out);
    TAK_Sha256 all;
    TAK_Sha256_Init(&all);
    for (int g = 0; g < TAK_DATA_GROUP_COUNT; g++) {
        FpList l;
        fp_list(g, &l);
        TAK_Sha256 gh;
        TAK_Sha256_Init(&gh);
        for (int i = 0; i < l.n; i++) {
            uint8_t own[TAK_SHA256_BYTES];
            fp_file(&gh, &l.f[i], report ? own : NULL);
            if (report) {
                char hex[65];
                TAK_Sha256_ToHex(own, hex);
                fprintf(report, "%-8s %-48s %.16s\n", k_group_names[g], l.f[i].key, hex);
            }
        }
        uint8_t d[TAK_SHA256_BYTES];
        TAK_Sha256_Final(&gh, d);
        out->group[g] = fp_first8(d);
        out->files[g] = l.n;
        TAK_Sha256_Update(&all, d, sizeof d);
        if (l.f) tak_free(l.f);
    }
    uint8_t d[TAK_SHA256_BYTES];
    TAK_Sha256_Final(&all, d);
    out->content = fp_first8(d);
    char schema[64];
    snprintf(schema, sizeof schema, "OpenKingdoms data schema %d", TAK_DATA_SCHEMA_VERSION);
    TAK_Sha256_Hash(schema, strlen(schema), d);
    out->schema = fp_first8(d);
    if (report) {
        for (int g = 0; g < TAK_DATA_GROUP_COUNT; g++)
            fprintf(report, "group %-8s %4d files %016llx\n", k_group_names[g],
                    out->files[g], (unsigned long long)out->group[g]);
        fprintf(report, "content %016llx schema %016llx\n",
                (unsigned long long)out->content, (unsigned long long)out->schema);
    }
    return 0;
}

int TAK_DataFingerprint_Compute(TAK_DataFingerprint *out) {
    return fp_run(out, NULL);
}

int TAK_DataFingerprint_Report(FILE *out) {
    TAK_DataFingerprint fp;
    return fp_run(&fp, out ? out : stdout);
}

const TAK_DataFingerprint *TAK_DataFingerprint_Get(void) {
    static TAK_DataFingerprint cached;
    static unsigned cached_gen;
    static int have;
    if (!VFS_IsInitialized()) return NULL;
    unsigned gen = VFS_Generation();
    if (!have || gen != cached_gen) {
        if (TAK_DataFingerprint_Compute(&cached) != 0) return NULL;
        cached_gen = gen;
        have = 1;
        fprintf(stderr, "Data fingerprint: content %016llx, %d unit files, "
                "%d scripts\n", (unsigned long long)cached.content,
                cached.files[TAK_DATA_GROUP_UNITS], cached.files[TAK_DATA_GROUP_SCRIPTS]);
    }
    return &cached;
}
