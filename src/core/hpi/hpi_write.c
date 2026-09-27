/*
 * hpi_write.c -- an archive in the format hpi.c reads, see
 * tak_hpi_write.h.
 */
#include "tak_hpi_write.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"

#include "miniz.h"

#include <stdio.h>
#include <string.h>

#define CHUNK_BYTES 65536u
#define MAX_NODES   512

typedef struct Node {
    char     name[128];
    int      is_dir;
    int      parent;
    int      file;          /* index into the caller's files, for a file */
    uint32_t name_ptr;      /* into the name block */
    uint32_t start, stored; /* where its chunks lie, and their bytes */
} Node;

typedef struct Buf {
    uint8_t *p;
    size_t   n, cap;
} Buf;

static int buf_room(Buf *b, size_t more) {
    if (b->n + more <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->n + more) cap *= 2;
    uint8_t *p = (uint8_t *)tak_realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

static int buf_put(Buf *b, const void *data, size_t n) {
    if (buf_room(b, n) != 0) return -1;
    memcpy(b->p + b->n, data, n);
    b->n += n;
    return 0;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One SQSH chunk of zlib data, unencrypted, onto `out`. The checksum is
 * the sum of the stored bytes, as the reader checks it. */
static int put_chunk(Buf *out, const uint8_t *data, uint32_t size) {
    mz_ulong cap = mz_compressBound(size);
    uint8_t *z = (uint8_t *)tak_malloc(cap ? cap : 1);
    if (!z) return -1;
    if (mz_compress2(z, &cap, data, size, MZ_BEST_COMPRESSION) != MZ_OK) { tak_free(z); return -1; }
    uint32_t sum = 0;
    for (mz_ulong i = 0; i < cap; i++) sum += z[i];
    uint8_t hdr[19];
    put32(hdr, HPI_SQSH_MARKER);
    hdr[4] = 0x02;          /* chunk version */
    hdr[5] = 2;             /* zlib */
    hdr[6] = 0;             /* not encrypted */
    put32(hdr + 7, (uint32_t)cap);
    put32(hdr + 11, size);
    put32(hdr + 15, sum);
    int rc = buf_put(out, hdr, sizeof hdr) == 0 && buf_put(out, z, cap) == 0 ? 0 : -1;
    tak_free(z);
    return rc;
}

static int find_child(const Node *nodes, int n, int parent, const char *name, int dir) {
    for (int i = 0; i < n; i++)
        if (nodes[i].parent == parent && nodes[i].is_dir == dir && tak_stricmp(nodes[i].name, name) == 0)
            return i;
    return -1;
}

/* One folder's records: its sub folders as one run of HPIDir_V2, then
 * its files as one run of HPIFileEntry_V2, then each sub folder's own.
 * `self_at` is where this folder's record lies, filled in here. */
static int write_dir(Buf *dir, const Node *nodes, int n, int self, size_t self_at,
                     const HPIPackFile *files, uint32_t date) {
    int subs[MAX_NODES], fs[MAX_NODES], ns = 0, nf = 0;
    for (int i = 0; i < n; i++) {
        if (nodes[i].parent != self) continue;
        if (nodes[i].is_dir) subs[ns++] = i; else fs[nf++] = i;
    }
    uint32_t first_subdir = (uint32_t)dir->n;
    size_t sub_at[MAX_NODES];
    for (int k = 0; k < ns; k++) {
        uint8_t rec[20];
        memset(rec, 0, sizeof rec);
        put32(rec, nodes[subs[k]].name_ptr);
        sub_at[k] = dir->n;
        if (buf_put(dir, rec, sizeof rec) != 0) return -1;
    }
    uint32_t first_file = (uint32_t)dir->n;
    for (int k = 0; k < nf; k++) {
        const Node *f = &nodes[fs[k]];
        uint8_t rec[24];
        memset(rec, 0, sizeof rec);
        put32(rec, f->name_ptr);
        put32(rec + 4, f->start);
        put32(rec + 8, files[f->file].size);
        put32(rec + 12, f->stored);
        put32(rec + 16, date);
        if (buf_put(dir, rec, sizeof rec) != 0) return -1;
    }
    put32(dir->p + self_at + 4, first_subdir);
    put32(dir->p + self_at + 8, (uint32_t)ns);
    put32(dir->p + self_at + 12, first_file);
    put32(dir->p + self_at + 16, (uint32_t)nf);
    for (int k = 0; k < ns; k++)
        if (write_dir(dir, nodes, n, subs[k], sub_at[k], files, date) != 0) return -1;
    return 0;
}

int HPI_WritePack(const char *out_path, const HPIPackFile *files, int count,
                  uint32_t date, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    if (!out_path || !files || count <= 0) {
        if (err) snprintf(err, err_cap, "nothing to pack");
        return -1;
    }
    static Node nodes[MAX_NODES];
    Buf names = { 0 }, out = { 0 }, dir = { 0 };
    int rc = -1;
    const char *why = "out of memory";

    /* The tree, one node per folder and file, the root first. */
    memset(&nodes[0], 0, sizeof(Node));
    nodes[0].is_dir = 1;
    nodes[0].parent = -1;
    nodes[0].file = -1;
    int n = 1;
    for (int f = 0; f < count; f++) {
        char path[512];
        snprintf(path, sizeof path, "%s", files[f].path ? files[f].path : "");
        int parent = 0;
        char *part = path;
        for (;;) {
            char *slash = strpbrk(part, "/\\");
            if (slash) *slash = 0;
            if (part[0]) {
                int is_dir = slash != NULL;
                int at = find_child(nodes, n, parent, part, is_dir);
                if (at < 0) {
                    if (n >= MAX_NODES) { why = "too many files"; goto done; }
                    at = n++;
                    memset(&nodes[at], 0, sizeof(Node));
                    snprintf(nodes[at].name, sizeof nodes[at].name, "%s", part);
                    nodes[at].is_dir = is_dir;
                    nodes[at].parent = parent;
                    nodes[at].file = is_dir ? -1 : f;
                } else if (!is_dir) {
                    why = "a file is named twice";
                    goto done;
                }
                parent = at;
            }
            if (!slash) break;
            part = slash + 1;
        }
    }

    for (int i = 0; i < n; i++) {
        nodes[i].name_ptr = (uint32_t)names.n;
        if (buf_put(&names, nodes[i].name, strlen(nodes[i].name) + 1) != 0) goto done;
    }

    /* The header's room, then every file as its chunks from 0x20. */
    if (buf_room(&out, 0x20) != 0) goto done;
    memset(out.p, 0, 0x20);
    out.n = 0x20;
    for (int i = 0; i < n; i++) {
        if (nodes[i].is_dir) continue;
        const HPIPackFile *f = &files[nodes[i].file];
        nodes[i].start = (uint32_t)out.n;
        const uint8_t *p = (const uint8_t *)f->data;
        uint32_t left = f->size;
        do {
            uint32_t take = left < CHUNK_BYTES ? left : CHUNK_BYTES;
            if (put_chunk(&out, p, take) != 0) goto done;
            p += take;
            left -= take;
        } while (left > 0);
        nodes[i].stored = (uint32_t)out.n - nodes[i].start;
    }

    /* The directory, the root's record at offset 0. */
    {
        uint8_t root[20];
        memset(root, 0, sizeof root);
        put32(root, nodes[0].name_ptr);
        if (buf_put(&dir, root, sizeof root) != 0) goto done;
        if (write_dir(&dir, nodes, n, 0, 0, files, date) != 0) goto done;
    }

    /* The two blocks after the data, each one SQSH chunk. The header
     * gives each block's offset and its bytes on disk. */
    uint32_t dir_at = (uint32_t)out.n;
    if (put_chunk(&out, dir.p, (uint32_t)dir.n) != 0) goto done;
    uint32_t dir_stored = (uint32_t)out.n - dir_at;
    uint32_t name_at = (uint32_t)out.n;
    if (put_chunk(&out, names.p, (uint32_t)names.n) != 0) goto done;
    uint32_t name_stored = (uint32_t)out.n - name_at;
    put32(out.p, HPI_MAGIC);
    put32(out.p + 4, HPI_VERSION_V2);
    put32(out.p + 8, dir_at);
    put32(out.p + 12, dir_stored);
    put32(out.p + 16, name_at);
    put32(out.p + 20, name_stored);
    put32(out.p + 24, 0x20);
    put32(out.p + 28, 0);

    {
        FILE *fp = fopen(out_path, "wb");
        int ok = fp && fwrite(out.p, 1, out.n, fp) == out.n;
        if (fp) fclose(fp);
        if (!ok) { why = "the file could not be written"; goto done; }
    }
    rc = 0;

done:
    if (rc != 0 && err) snprintf(err, err_cap, "%s", why);
    tak_free(out.p);
    tak_free(dir.p);
    tak_free(names.p);
    return rc;
}
