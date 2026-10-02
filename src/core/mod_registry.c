/*
 * mod_registry.c -- the mod registry's JSON, read and checked.
 *
 * A small strict reader for one known shape rather than a general JSON
 * library: objects, arrays, strings, integers and the three literals.
 * The rules match web/mods.js, which validates the same file in the page.
 */

#include "tak_mod_registry.h"
#include "tak_sha256.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
    int         bad;
} Cur;

static void skip_ws(Cur *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int eat(Cur *c, char ch) {
    skip_ws(c);
    if (c->p < c->end && *c->p == ch) { c->p++; return 1; }
    return 0;
}

static int hexval(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

static int put_utf8(char *out, size_t cap, size_t *n, unsigned cp) {
    char b[4];
    size_t k;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F)); k = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); k = 4;
    }
    if (*n + k >= cap) return -1;
    memcpy(out + *n, b, k);
    *n += k;
    return 0;
}

static int read_hex4(Cur *c, unsigned *v) {
    if (c->end - c->p < 4) return -1;
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(c->p[i]);
        if (h < 0) return -1;
        *v = (*v << 4) | (unsigned)h;
    }
    c->p += 4;
    return 0;
}

/* A string into out. Returns 0, -1 on bad JSON, -2 when it does not
 * fit, which the caller reports as too long rather than as bad JSON. */
static int read_string(Cur *c, char *out, size_t cap) {
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"') return -1;
    c->p++;
    size_t n = 0;
    int over = 0;
    while (c->p < c->end && *c->p != '"') {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch < 0x20) return -1;
        if (ch != '\\') {
            if (n + 1 < cap) out[n++] = (char)ch; else over = 1;
            continue;
        }
        if (c->p >= c->end) return -1;
        char e = *c->p++;
        unsigned cp = 0;
        switch (e) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (read_hex4(c, &cp) != 0) return -1;
            if (cp >= 0xD800 && cp < 0xDC00) {
                unsigned lo = 0;
                if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u') return -1;
                c->p += 2;
                if (read_hex4(c, &lo) != 0 || lo < 0xDC00 || lo > 0xDFFF) return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                return -1;
            }
            break;
        default: return -1;
        }
        if (!over && out && put_utf8(out, cap, &n, cp) != 0) over = 1;
    }
    if (c->p >= c->end) return -1;
    c->p++;
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return over ? -2 : 0;
}

/* A non-negative integer. -1 for anything else, a fraction included. */
static int read_uint(Cur *c, uint64_t *v) {
    skip_ws(c);
    const char *s = c->p;
    *v = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        if (*v > (UINT64_MAX - 9) / 10) return -1;
        *v = *v * 10 + (uint64_t)(*c->p++ - '0');
    }
    if (c->p == s) return -1;
    if (c->p < c->end && (*c->p == '.' || *c->p == 'e' || *c->p == 'E')) return -1;
    return 0;
}

static int skip_value(Cur *c, int depth);

static int skip_container(Cur *c, char open, char close, int depth) {
    if (!eat(c, open)) return -1;
    if (eat(c, close)) return 0;
    for (;;) {
        if (open == '{') {
            char key[2];
            if (read_string(c, key, sizeof key) == -1 || !eat(c, ':')) return -1;
        }
        if (skip_value(c, depth + 1) != 0) return -1;
        if (eat(c, ',')) continue;
        return eat(c, close) ? 0 : -1;
    }
}

static int skip_value(Cur *c, int depth) {
    if (depth > 16) return -1;
    skip_ws(c);
    if (c->p >= c->end) return -1;
    char ch = *c->p;
    if (ch == '{') return skip_container(c, '{', '}', depth);
    if (ch == '[') return skip_container(c, '[', ']', depth);
    if (ch == '"') { char k[2]; return read_string(c, k, sizeof k) == -1 ? -1 : 0; }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        if (ch == '-') c->p++;
        while (c->p < c->end && strchr("0123456789.eE+-", *c->p)) c->p++;
        return 0;
    }
    static const char *const lits[] = { "true", "false", "null" };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(lits[i]);
        if ((size_t)(c->end - c->p) >= n && memcmp(c->p, lits[i], n) == 0) { c->p += n; return 0; }
    }
    return -1;
}

/* ── one entry ─────────────────────────────────────────────────────── */

static int is_id(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n > max) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (ch == '-' && i > 0);
        if (!ok) return 0;
    }
    return 1;
}

static int is_plain(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return 0;
    return 1;
}

static int is_https(const char *s) {
    if (strncmp(s, "https://", 8) != 0 || !s[8] || s[8] == '/') return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p <= 0x20 || *p == 0x7F || *p == '"' || *p == '<' || *p == '>' || *p == '\\') return 0;
    return 1;
}

typedef struct {
    TAK_ModEntry e;
    int  has_size, has_sha, has_fp;
    char sha_text[80];
    char fp_text[40];
    const char *why;
} Draft;

static void read_field(Cur *c, const char *key, Draft *d) {
    struct { const char *key; char *dst; size_t cap; } strs[] = {
        { "id", d->e.id, sizeof d->e.id },
        { "name", d->e.name, sizeof d->e.name },
        { "version", d->e.version, sizeof d->e.version },
        { "author", d->e.author, sizeof d->e.author },
        { "page", d->e.page, sizeof d->e.page },
        { "modset", d->e.modset, sizeof d->e.modset },
        { "url", d->e.url, sizeof d->e.url },
        { "manual", d->e.manual, sizeof d->e.manual },
        { "sha256", d->sha_text, sizeof d->sha_text },
        { "fingerprint", d->fp_text, sizeof d->fp_text },
    };
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        if (strcmp(key, strs[i].key) != 0) continue;
        int rc = read_string(c, strs[i].dst, strs[i].cap);
        if (rc == -1) { c->bad = 1; return; }
        if (rc == -2 && !d->why) d->why = "a field is too long";
        if (strcmp(key, "sha256") == 0) d->has_sha = 1;
        if (strcmp(key, "fingerprint") == 0) d->has_fp = 1;
        return;
    }
    if (strcmp(key, "size") == 0) {
        skip_ws(c);
        const char *at = c->p;
        if (read_uint(c, &d->e.size) != 0) {
            if (!d->why) d->why = "size is not a whole number of bytes";
            c->p = at;
            if (skip_value(c, 1) != 0) c->bad = 1;
        }
        d->has_size = 1;
        return;
    }
    if (skip_value(c, 1) != 0) c->bad = 1;
}

static int parse_hex(const char *s, uint8_t *out, size_t bytes) {
    if (strlen(s) != bytes * 2) return -1;
    for (size_t i = 0; i < bytes; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static const char *finish(Draft *d, const TAK_ModRegistry *r) {
    TAK_ModEntry *e = &d->e;
    if (d->why) return d->why;
    if (!is_id(e->id, sizeof e->id - 1)) return "id must be lower case letters, digits and dashes";
    if (TAK_ModRegistry_Find(r, e->id)) return "an id is listed twice";
    if (!e->name[0] || !is_plain(e->name)) return "name is missing";
    if (!e->version[0] || !is_plain(e->version)) return "version is missing";
    if (!e->author[0] || !is_plain(e->author)) return "author is missing";
    if (!is_https(e->page)) return "page must be an https address";
    if (e->url[0] && e->manual[0]) return "an entry is one click or manual, not both";
    if (e->manual[0]) return is_plain(e->manual) ? NULL : "manual has control characters";
    if (!e->url[0]) return "an entry needs a url or manual";
    if (!is_https(e->url)) return "url must be an https address";
    if (!d->has_size || e->size == 0) return "size is missing";
    if (e->size > TAK_MODREG_SIZE_MAX) return "size is over the limit the relay streams";
    if (!d->has_sha || parse_hex(d->sha_text, e->sha256, 32) != 0) return "sha256 must be 64 hex digits";
    uint8_t fp[8];
    if (!d->has_fp || parse_hex(d->fp_text, fp, 8) != 0) return "fingerprint must be 16 hex digits";
    e->fingerprint = 0;
    for (int i = 0; i < 8; i++) e->fingerprint = e->fingerprint << 8 | fp[i];
    if (e->fingerprint == 0) return "fingerprint must not be zero";
    if (!is_id(e->modset, sizeof e->modset - 1)) return "modset must be a mod set id";
    return NULL;
}

int TAK_ModRegistry_Parse(const char *json, size_t len, TAK_ModRegistry *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!json) return -1;
    Cur c = { json, json + len, 0 };
    int saw_version = 0, saw_mods = 0;
    if (!eat(&c, '{')) return -1;
    if (!eat(&c, '}')) for (;;) {
        char key[32];
        if (read_string(&c, key, sizeof key) == -1 || !eat(&c, ':')) return -1;
        if (strcmp(key, "registry") == 0) {
            uint64_t v = 0;
            if (read_uint(&c, &v) != 0 || v != 1) return -1;
            saw_version = 1;
        } else if (strcmp(key, "mods") == 0) {
            if (!eat(&c, '[')) return -1;
            saw_mods = 1;
            if (!eat(&c, ']')) for (;;) {
                Draft d;
                memset(&d, 0, sizeof d);
                if (!eat(&c, '{')) return -1;
                if (!eat(&c, '}')) for (;;) {
                    char fk[32];
                    int rc = read_string(&c, fk, sizeof fk);
                    if (rc == -1 || !eat(&c, ':')) return -1;
                    if (rc == -2) { if (skip_value(&c, 1) != 0) return -1; }
                    else read_field(&c, fk, &d);
                    if (c.bad) return -1;
                    if (eat(&c, ',')) continue;
                    if (!eat(&c, '}')) return -1;
                    break;
                }
                const char *why = finish(&d, out);
                if (!why && out->count >= TAK_MODREG_MAX) why = "the registry lists too many mods";
                if (why) {
                    if (!out->errors++)
                        snprintf(out->first_error, sizeof out->first_error, "%s%s%s",
                                 d.e.id[0] ? d.e.id : "an entry", ": ", why);
                } else {
                    out->mod[out->count++] = d.e;
                }
                if (eat(&c, ',')) continue;
                if (!eat(&c, ']')) return -1;
                break;
            }
        } else if (skip_value(&c, 1) != 0) {
            return -1;
        }
        if (eat(&c, ',')) continue;
        if (!eat(&c, '}')) return -1;
        break;
    }
    skip_ws(&c);
    if (c.p != c.end || !saw_version || !saw_mods) return -1;
    return out->count;
}

const TAK_ModEntry *TAK_ModRegistry_Find(const TAK_ModRegistry *r, const char *id) {
    if (!r || !id) return NULL;
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->mod[i].id, id) == 0) return &r->mod[i];
    return NULL;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

const TAK_ModEntry *TAK_ModRegistry_ForRoom(const TAK_ModRegistry *r,
                                            const char *mod, const char *version,
                                            uint64_t content) {
    if (!r) return NULL;
    for (int i = 0; content && i < r->count; i++)
        if (r->mod[i].fingerprint == content) return &r->mod[i];
    if (!mod || !mod[0]) return NULL;
    for (int i = 0; i < r->count; i++)
        if (ieq(r->mod[i].name, mod) && ieq(r->mod[i].version, version ? version : ""))
            return &r->mod[i];
    return NULL;
}

int TAK_ModEntry_OneClick(const TAK_ModEntry *e) {
    return e && e->url[0] && !e->manual[0];
}

int TAK_ModEntry_Check(const TAK_ModEntry *e, const void *bytes, size_t len,
                       uint64_t room_content, char *why, size_t cap) {
    char label[64];
    if (why && cap) why[0] = '\0';
    if (!e) return -1;
    snprintf(label, sizeof label, "%s %s", e->name, e->version);
    if (room_content && e->fingerprint != room_content) {
        if (why) snprintf(why, cap, "The registry's %s plays other data than that game, "
                          "so it would not let you join.", label);
        return -1;
    }
    if ((uint64_t)len != e->size) {
        if (why) snprintf(why, cap, "The download of %s is %llu bytes and the registry says %llu. "
                          "It was not installed.", label, (unsigned long long)len,
                          (unsigned long long)e->size);
        return -1;
    }
    uint8_t got[TAK_SHA256_BYTES];
    TAK_Sha256_Hash(bytes, len, got);
    if (memcmp(got, e->sha256, sizeof got) != 0) {
        if (why) snprintf(why, cap, "The download of %s does not match the registry's fingerprint, "
                          "so it was changed or damaged on the way. It was not installed.", label);
        return -1;
    }
    return 0;
}
