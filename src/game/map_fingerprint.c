/*
 * map_fingerprint.c -- what a map is, as 32 bytes.
 *
 * The definition and the canonical form live in tak_map_fingerprint.h.
 * Everything here is integer work on bytes: no floats, no locale calls,
 * no wall clock, so the same map hashes the same on every target.
 */

#include "tak_map_fingerprint.h"
#include "tak_maps.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_sha256.h"

#include <stdio.h>
#include <string.h>

/* ── A growable byte buffer ─────────────────────────────────────────── */

typedef struct FPBuf {
    char  *data;
    size_t len;
    size_t cap;
    int    failed;
} FPBuf;

static void fpbuf_free(FPBuf *b) {
    tak_free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void fpbuf_add(FPBuf *b, const char *bytes, size_t n) {
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 1024;
        while (want < b->len + n + 1) want *= 2;
        char *grown = (char *)tak_realloc(b->data, want);
        if (!grown) { b->failed = 1; return; }
        b->data = grown;
        b->cap = want;
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void fpbuf_str(FPBuf *b, const char *s) { fpbuf_add(b, s, strlen(s)); }

/* ── TDF canonicalisation ───────────────────────────────────────────── */

typedef struct FPNode {
    char *name;             /* lower cased */
    char *value;            /* NULL for a section */
    struct FPNode *first;   /* first child */
    struct FPNode *last;
    struct FPNode *next;    /* next sibling */
} FPNode;

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int name_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        a++; b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static char *dup_range(const char *s, size_t n, int lower) {
    char *out = (char *)tak_malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) out[i] = lower ? ascii_lower(s[i]) : s[i];
    out[n] = '\0';
    return out;
}

static void node_free(FPNode *n) {
    while (n) {
        FPNode *next = n->next;
        node_free(n->first);
        tak_free(n->name);
        tak_free(n->value);
        tak_free(n);
        n = next;
    }
}

static FPNode *node_new(char *name, char *value) {
    FPNode *n = (FPNode *)tak_calloc(1, sizeof(FPNode));
    if (!n) { tak_free(name); tak_free(value); return NULL; }
    n->name = name;
    n->value = value;
    return n;
}

static void node_append(FPNode *parent, FPNode *child) {
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

/* Line based, the same shape the engine's parser reads: a "[name]" line
 * opens a section, "{" enters it, "}" leaves it, and any other line with
 * an '=' is a key whose value runs to the first ';'. */
static FPNode *parse_tdf(const char *text, size_t len) {
    FPNode *root = node_new(dup_range("", 0, 0), NULL);
    if (!root) return NULL;

    FPNode *stack[64];
    int depth = 0;
    stack[0] = root;
    FPNode *pending = NULL;

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t end = i;
        if (i < len) i++;                     /* step past the newline */
        while (end > start && (text[end - 1] == '\r' || text[end - 1] == ' ' ||
                               text[end - 1] == '\t')) end--;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        if (start >= end) continue;                       /* blank */
        if (end - start >= 2 && text[start] == '/' && text[start + 1] == '/')
            continue;                                     /* comment */

        if (text[start] == '[') {
            size_t close = start + 1;
            while (close < end && text[close] != ']') close++;
            char *name = dup_range(text + start + 1, close - (start + 1), 1);
            FPNode *node = node_new(name, NULL);
            if (!node) { node_free(root); return NULL; }
            node_append(stack[depth], node);
            pending = node;
            continue;
        }
        if (text[start] == '{') {
            if (depth + 1 < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[depth + 1] = pending ? pending : stack[depth];
                depth++;
            }
            pending = NULL;
            continue;
        }
        if (text[start] == '}') {
            if (depth > 0) depth--;
            pending = NULL;
            continue;
        }

        size_t eq = start;
        while (eq < end && text[eq] != '=') eq++;
        if (eq >= end) continue;              /* not a key line */

        size_t key_end = eq;
        while (key_end > start && (text[key_end - 1] == ' ' ||
                                   text[key_end - 1] == '\t')) key_end--;
        size_t val_start = eq + 1;
        size_t val_end = val_start;
        while (val_end < end && text[val_end] != ';') val_end++;
        while (val_start < val_end && (text[val_start] == ' ' ||
                                       text[val_start] == '\t')) val_start++;
        while (val_end > val_start && (text[val_end - 1] == ' ' ||
                                       text[val_end - 1] == '\t')) val_end--;

        char *name = dup_range(text + start, key_end - start, 1);
        char *value = dup_range(text + val_start, val_end - val_start, 0);
        if (!name || !value) { tak_free(name); tak_free(value); node_free(root); return NULL; }
        /* Labels, not content: they name the map for a human. */
        if (name_cmp(name, "missionname") == 0 ||
            name_cmp(name, "missiondescription") == 0) {
            tak_free(name);
            tak_free(value);
            continue;
        }
        FPNode *node = node_new(name, value);
        if (!node) { node_free(root); return NULL; }
        node_append(stack[depth], node);
    }

    return root;
}

static void emit_section(FPBuf *out, FPNode *section, int depth) {
    /* Keys first, sorted by name, equal names keeping their file order. */
    int key_count = 0;
    for (FPNode *c = section->first; c; c = c->next)
        if (c->value) key_count++;

    if (key_count > 0) {
        FPNode **keys = (FPNode **)tak_malloc(sizeof(FPNode *) * (size_t)key_count);
        if (!keys) { out->failed = 1; return; }
        int n = 0;
        for (FPNode *c = section->first; c; c = c->next)
            if (c->value) keys[n++] = c;
        for (int i = 1; i < n; i++) {              /* stable insertion sort */
            FPNode *item = keys[i];
            int j = i - 1;
            while (j >= 0 && name_cmp(keys[j]->name, item->name) > 0) {
                keys[j + 1] = keys[j];
                j--;
            }
            keys[j + 1] = item;
        }
        for (int i = 0; i < n; i++) {
            fpbuf_str(out, keys[i]->name);
            fpbuf_str(out, "=");
            fpbuf_str(out, keys[i]->value);
            fpbuf_str(out, "\n");
        }
        tak_free(keys);
    }

    for (FPNode *c = section->first; c; c = c->next) {
        if (c->value) continue;
        fpbuf_str(out, "[");
        fpbuf_str(out, c->name);
        fpbuf_str(out, "]\n{\n");
        emit_section(out, c, depth + 1);
        fpbuf_str(out, "}\n");
    }
}

static int canonical_tdf(const void *text, size_t len, FPBuf *out) {
    memset(out, 0, sizeof(*out));
    if (!text && len) return -1;
    FPNode *root = parse_tdf((const char *)text, len);
    if (!root) return -1;
    emit_section(out, root, 0);
    node_free(root);
    if (out->failed) { fpbuf_free(out); return -1; }
    return 0;
}

int TAK_MapFingerprint_CanonicalTDF(const void *text, size_t len,
                                    char *out, size_t out_cap) {
    FPBuf buf;
    if (canonical_tdf(text, len, &buf) != 0) return -1;
    if (out && out_cap) {
        size_t n = (buf.len < out_cap - 1) ? buf.len : out_cap - 1;
        if (buf.data) memcpy(out, buf.data, n);
        out[n] = '\0';
    }
    int len_out = (int)buf.len;
    fpbuf_free(&buf);
    return len_out;
}

/* ── The fingerprint ────────────────────────────────────────────────── */

static void hash_part(TAK_Sha256 *ctx, const char *tag,
                      const void *bytes, size_t len, int present) {
    char header[64];
    if (!present) {
        snprintf(header, sizeof(header), "%s -\n", tag);
        TAK_Sha256_Update(ctx, header, strlen(header));
        return;
    }
    snprintf(header, sizeof(header), "%s %lu\n", tag, (unsigned long)len);
    TAK_Sha256_Update(ctx, header, strlen(header));
    TAK_Sha256_Update(ctx, bytes, len);
}

int TAK_MapFingerprint_FromFiles(const TAK_MapFiles *files,
                                 uint8_t out[TAK_MAP_FINGERPRINT_BYTES]) {
    if (!files || !out) return -1;
    if (!files->tnt || !files->ota) return -1;

    FPBuf ota_canon;
    if (canonical_tdf(files->ota, files->ota_size, &ota_canon) != 0) return -1;

    FPBuf tdf_canon;
    memset(&tdf_canon, 0, sizeof(tdf_canon));
    if (files->tdf && canonical_tdf(files->tdf, files->tdf_size, &tdf_canon) != 0) {
        fpbuf_free(&ota_canon);
        return -1;
    }

    TAK_Sha256 ctx;
    TAK_Sha256_Init(&ctx);
    TAK_Sha256_Update(&ctx, "OKMAP1\n", 7);
    hash_part(&ctx, "tnt", files->tnt, files->tnt_size, 1);
    hash_part(&ctx, "ota", ota_canon.data ? ota_canon.data : "", ota_canon.len, 1);
    hash_part(&ctx, "crt", files->crt, files->crt_size, files->crt != NULL);
    hash_part(&ctx, "tdf", tdf_canon.data ? tdf_canon.data : "", tdf_canon.len,
              files->tdf != NULL);
    TAK_Sha256_Final(&ctx, out);

    fpbuf_free(&ota_canon);
    fpbuf_free(&tdf_canon);
    return 0;
}

int TAK_MapFingerprint_FromName(const char *map_key,
                                uint8_t out[TAK_MAP_FINGERPRINT_BYTES]) {
    if (!map_key || !out) return -1;

    static const char *const exts[4] = { "tnt", "ota", "crt", "tdf" };
    void *data[4] = { NULL, NULL, NULL, NULL };
    uint32_t size[4] = { 0, 0, 0, 0 };

    for (int i = 0; i < 4; i++) {
        char path[512];
        if (TAK_Maps_FindFile(map_key, exts[i], path, sizeof(path)) != 0) continue;
        if (VFS_ReadFile(path, &data[i], &size[i]) != 0) {
            data[i] = NULL;
            size[i] = 0;
        }
    }

    TAK_MapFiles files;
    memset(&files, 0, sizeof(files));
    files.tnt = data[0]; files.tnt_size = size[0];
    files.ota = data[1]; files.ota_size = size[1];
    files.crt = data[2]; files.crt_size = size[2];
    files.tdf = data[3]; files.tdf_size = size[3];

    int rc = TAK_MapFingerprint_FromFiles(&files, out);
    for (int i = 0; i < 4; i++) VFS_FreeBuffer(data[i]);
    return rc;
}

void TAK_MapFingerprint_ToHex(const uint8_t fp[TAK_MAP_FINGERPRINT_BYTES],
                              char out[TAK_MAP_FINGERPRINT_HEX]) {
    TAK_Sha256_ToHex(fp, out);
}
