/*
 * gltf.c -- glTF 2.0 binary reader.
 *
 * A .glb is a twelve byte header and a run of chunks: one of JSON and
 * one of packed binary. The JSON describes nodes, meshes, accessors
 * and materials; the binary holds the vertex data and the texture
 * files. This reads all of that into the neutral model tak_gltf.h
 * describes, leaving the file's own units and axes alone.
 *
 * These files are art someone drops in a folder, not shipped data, so
 * every count and offset in one is treated as hostile: each is checked
 * against the chunk it points into before a byte is read, arithmetic
 * that could wrap is written so it cannot, and anything unexpected
 * ends the load rather than being worked around.
 */
#include "tak_gltf.h"
#include "tak_hpi.h"
#include "tak_jpg.h"
#include "tak_png.h"
#include "tak_memory.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* A model of a building is a few thousand vertices. These are the
 * walls that stop a malformed file asking for the world. */
#define GLB_MAX_BYTES     (64u * 1024u * 1024u)
#define JS_MAX_NODES      100000
#define JS_MAX_DEPTH      64
#define GLTF_MAX_VERTS    65535        /* the index type downstream */
#define GLTF_MAX_TRIS     200000
#define IMAGE_MAX_PIXELS  (4096 * 4096)

/* ── JSON ─────────────────────────────────────────────────────────── */

enum { JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_ARR, JS_OBJ };

typedef struct JsNode {
    uint8_t type;
    double  num;                  /* JS_NUM, and JS_BOOL as 0 or 1 */
    int     str_off, str_len;     /* JS_STR, into the text */
    int     key_off, key_len;     /* the member name, for an object's child */
    int     first_child, next_sib;
    int     count;                /* members or elements */
} JsNode;

typedef struct JsDoc {
    const char *text;
    size_t      len;
    JsNode     *nodes;
    int         count, cap;
} JsDoc;

typedef struct JsParser {
    JsDoc  *doc;
    size_t  pos;
    int     depth;
    int     ok;
} JsParser;

static int js_new(JsParser *p, uint8_t type) {
    JsDoc *d = p->doc;
    if (d->count >= JS_MAX_NODES) { p->ok = 0; return -1; }
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 64;
        if (cap > JS_MAX_NODES) cap = JS_MAX_NODES;
        JsNode *n = (JsNode *)tak_malloc(sizeof(JsNode) * (size_t)cap);
        if (!n) { p->ok = 0; return -1; }
        if (d->nodes) {
            memcpy(n, d->nodes, sizeof(JsNode) * (size_t)d->count);
            tak_free(d->nodes);
        }
        d->nodes = n;
        d->cap = cap;
    }
    JsNode *n = &d->nodes[d->count];
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->first_child = n->next_sib = -1;
    n->key_off = n->str_off = -1;
    return d->count++;
}

static void js_skip_ws(JsParser *p) {
    const char *t = p->doc->text;
    while (p->pos < p->doc->len) {
        char c = t[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

/* A JSON string, left as it lies in the text. An escape is stepped
 * over rather than expanded: names with escapes are not addressed by
 * this reader, and leaving them packed keeps the text the only copy. */
static int js_read_string(JsParser *p, int *out_off, int *out_len) {
    const char *t = p->doc->text;
    if (p->pos >= p->doc->len || t[p->pos] != '"') { p->ok = 0; return 0; }
    p->pos++;
    size_t start = p->pos;
    while (p->pos < p->doc->len) {
        char c = t[p->pos];
        if (c == '\\') {
            if (p->pos + 1 >= p->doc->len) { p->ok = 0; return 0; }
            p->pos += 2;
            continue;
        }
        if (c == '"') {
            size_t len = p->pos - start;
            if (len > (size_t)0x7fffffff) { p->ok = 0; return 0; }
            *out_off = (int)start;
            *out_len = (int)len;
            p->pos++;
            return 1;
        }
        p->pos++;
    }
    p->ok = 0;
    return 0;
}

/* A JSON number, read here rather than through the C library, whose
 * idea of a decimal point follows the machine's locale. A model would
 * come apart quietly on a machine that writes 1,5 for one and a half. */
static double js_number(const char *t, size_t len, size_t *at) {
    size_t i = *at;
    int neg = 0;
    if (i < len && (t[i] == '-' || t[i] == '+')) { neg = t[i] == '-'; i++; }
    size_t digits_at = i;
    double whole = 0.0;
    while (i < len && t[i] >= '0' && t[i] <= '9') {
        whole = whole * 10.0 + (t[i] - '0');
        i++;
    }
    if (i < len && t[i] == '.') {
        i++;
        double place = 0.1;
        while (i < len && t[i] >= '0' && t[i] <= '9') {
            whole += (t[i] - '0') * place;
            place *= 0.1;
            i++;
        }
    }
    if (i == digits_at) return 0.0;              /* no digits, not a number */
    if (i < len && (t[i] == 'e' || t[i] == 'E')) {
        size_t save = i;
        i++;
        int eneg = 0;
        if (i < len && (t[i] == '-' || t[i] == '+')) { eneg = t[i] == '-'; i++; }
        if (i < len && t[i] >= '0' && t[i] <= '9') {
            int exp = 0;
            while (i < len && t[i] >= '0' && t[i] <= '9') {
                if (exp < 10000) exp = exp * 10 + (t[i] - '0');
                i++;
            }
            if (exp > 308) exp = 308;
            double scale = 1.0;
            for (int k = 0; k < exp; k++) scale *= 10.0;
            whole = eneg ? whole / scale : whole * scale;
        } else {
            i = save;                            /* a stray e, not an exponent */
        }
    }
    *at = i;
    return neg ? -whole : whole;
}

static int js_parse_value(JsParser *p);

static int js_parse_object(JsParser *p) {
    int self = js_new(p, JS_OBJ);
    if (self < 0) return -1;
    p->pos++;                       /* { */
    int last = -1;
    for (;;) {
        js_skip_ws(p);
        if (p->pos >= p->doc->len) { p->ok = 0; return -1; }
        if (p->doc->text[p->pos] == '}') { p->pos++; break; }
        if (last >= 0) {
            if (p->doc->text[p->pos] != ',') { p->ok = 0; return -1; }
            p->pos++;
            js_skip_ws(p);
        }
        int koff = 0, klen = 0;
        if (!js_read_string(p, &koff, &klen)) return -1;
        js_skip_ws(p);
        if (p->pos >= p->doc->len || p->doc->text[p->pos] != ':') { p->ok = 0; return -1; }
        p->pos++;
        int child = js_parse_value(p);
        if (child < 0) return -1;
        JsNode *cn = &p->doc->nodes[child];
        cn->key_off = koff;
        cn->key_len = klen;
        if (last < 0) p->doc->nodes[self].first_child = child;
        else p->doc->nodes[last].next_sib = child;
        last = child;
        p->doc->nodes[self].count++;
    }
    return self;
}

static int js_parse_array(JsParser *p) {
    int self = js_new(p, JS_ARR);
    if (self < 0) return -1;
    p->pos++;                       /* [ */
    int last = -1;
    for (;;) {
        js_skip_ws(p);
        if (p->pos >= p->doc->len) { p->ok = 0; return -1; }
        if (p->doc->text[p->pos] == ']') { p->pos++; break; }
        if (last >= 0) {
            if (p->doc->text[p->pos] != ',') { p->ok = 0; return -1; }
            p->pos++;
        }
        int child = js_parse_value(p);
        if (child < 0) return -1;
        if (last < 0) p->doc->nodes[self].first_child = child;
        else p->doc->nodes[last].next_sib = child;
        last = child;
        p->doc->nodes[self].count++;
    }
    return self;
}

static int js_parse_value(JsParser *p) {
    if (++p->depth > JS_MAX_DEPTH) { p->ok = 0; p->depth--; return -1; }
    js_skip_ws(p);
    int r = -1;
    if (p->pos >= p->doc->len) { p->ok = 0; p->depth--; return -1; }
    const char *t = p->doc->text;
    char c = t[p->pos];
    if (c == '{') {
        r = js_parse_object(p);
    } else if (c == '[') {
        r = js_parse_array(p);
    } else if (c == '"') {
        r = js_new(p, JS_STR);
        if (r >= 0) {
            int off = 0, len = 0;
            if (js_read_string(p, &off, &len)) {
                p->doc->nodes[r].str_off = off;
                p->doc->nodes[r].str_len = len;
            } else r = -1;
        }
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *lit = c == 't' ? "true" : (c == 'f' ? "false" : "null");
        size_t n = strlen(lit);
        if (p->pos + n > p->doc->len || memcmp(t + p->pos, lit, n) != 0) {
            p->ok = 0;
        } else {
            r = js_new(p, c == 'n' ? JS_NULL : JS_BOOL);
            if (r >= 0) p->doc->nodes[r].num = (c == 't') ? 1.0 : 0.0;
            p->pos += n;
        }
    } else {
        size_t end = p->pos;
        double v = js_number(t, p->doc->len, &end);
        if (end == p->pos) {
            p->ok = 0;
        } else {
            p->pos = end;
            r = js_new(p, JS_NUM);
            if (r >= 0) p->doc->nodes[r].num = v;
        }
    }
    p->depth--;
    return p->ok ? r : -1;
}

/* Parses NUL terminated JSON text. Returns the root index or -1. */
static int js_parse(JsDoc *doc, const char *text, size_t len) {
    memset(doc, 0, sizeof(*doc));
    doc->text = text;
    doc->len = len;
    JsParser p;
    p.doc = doc;
    p.pos = 0;
    p.depth = 0;
    p.ok = 1;
    int root = js_parse_value(&p);
    if (root < 0 || !p.ok) return -1;
    return root;
}

static void js_free(JsDoc *doc) {
    if (doc->nodes) tak_free(doc->nodes);
    doc->nodes = NULL;
    doc->count = doc->cap = 0;
}

static int js_member(const JsDoc *d, int obj, const char *key) {
    if (obj < 0 || obj >= d->count || d->nodes[obj].type != JS_OBJ) return -1;
    size_t klen = strlen(key);
    for (int i = d->nodes[obj].first_child; i >= 0; i = d->nodes[i].next_sib) {
        const JsNode *n = &d->nodes[i];
        if (n->key_len == (int)klen && n->key_off >= 0 &&
            memcmp(d->text + n->key_off, key, klen) == 0)
            return i;
    }
    return -1;
}

static int js_at(const JsDoc *d, int arr, int index) {
    if (arr < 0 || arr >= d->count || d->nodes[arr].type != JS_ARR) return -1;
    if (index < 0 || index >= d->nodes[arr].count) return -1;
    int i = d->nodes[arr].first_child;
    while (index-- > 0 && i >= 0) i = d->nodes[i].next_sib;
    return i;
}

static int js_len(const JsDoc *d, int arr) {
    if (arr < 0 || arr >= d->count) return 0;
    return d->nodes[arr].count;
}

/* An integer member, or `def` when it is missing or not a whole
 * number that fits. Anything else is a malformed file. */
static int js_int(const JsDoc *d, int obj, const char *key, int def) {
    int m = js_member(d, obj, key);
    if (m < 0 || d->nodes[m].type != JS_NUM) return def;
    double v = d->nodes[m].num;
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return def;
    return (int)v;
}

static float js_float_at(const JsDoc *d, int arr, int i, float def) {
    int m = js_at(d, arr, i);
    if (m < 0 || d->nodes[m].type != JS_NUM) return def;
    double v = d->nodes[m].num;
    if (!(v > -1e30 && v < 1e30)) return def;
    return (float)v;
}

/* A JSON true or false. A number stands in for one, as some writers
 * put 1 and 0. */
static int js_bool(const JsDoc *d, int obj, const char *key, int def) {
    int m = js_member(d, obj, key);
    if (m < 0) return def;
    if (d->nodes[m].type == JS_BOOL || d->nodes[m].type == JS_NUM)
        return d->nodes[m].num != 0.0 ? 1 : 0;
    return def;
}

static float js_float(const JsDoc *d, int obj, const char *key, float def) {
    int m = js_member(d, obj, key);
    if (m < 0 || d->nodes[m].type != JS_NUM) return def;
    double v = d->nodes[m].num;
    if (!(v > -1e30 && v < 1e30)) return def;
    return (float)v;
}

static int js_str_eq_ci(const JsDoc *d, int n, const char *s) {
    if (n < 0 || d->nodes[n].type != JS_STR || d->nodes[n].str_off < 0) return 0;
    size_t len = strlen(s);
    if ((size_t)d->nodes[n].str_len != len) return 0;
    const char *a = d->text + d->nodes[n].str_off;
    for (size_t i = 0; i < len; i++) {
        char x = a[i], y = s[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

static void js_copy_str(const JsDoc *d, int n, char *out, size_t cap) {
    out[0] = '\0';
    if (n < 0 || d->nodes[n].type != JS_STR || d->nodes[n].str_off < 0) return;
    size_t len = (size_t)d->nodes[n].str_len;
    if (len > cap - 1) len = cap - 1;
    memcpy(out, d->text + d->nodes[n].str_off, len);
    out[len] = '\0';
}

/* ── the file ─────────────────────────────────────────────────────── */

typedef struct Ctx {
    int            with_images;
    JsDoc          doc;
    int            root;
    const uint8_t *bin;
    size_t         bin_size;
    int            accessors, buffer_views, meshes, materials, textures, images, nodes;
    GltfModel     *m;
} Ctx;

static int comp_bytes(int component_type) {
    switch (component_type) {
    case 5120: case 5121: return 1;   /* byte, unsigned byte */
    case 5122: case 5123: return 2;   /* short, unsigned short */
    case 5125: case 5126: return 4;   /* unsigned int, float */
    default: return 0;
    }
}

static int type_comps(const JsDoc *d, int str_node) {
    if (js_str_eq_ci(d, str_node, "SCALAR")) return 1;
    if (js_str_eq_ci(d, str_node, "VEC2")) return 2;
    if (js_str_eq_ci(d, str_node, "VEC3")) return 3;
    if (js_str_eq_ci(d, str_node, "VEC4")) return 4;
    return 0;
}

/* Where an accessor's elements live, with every offset checked
 * against the binary chunk before anything reads from it. */
typedef struct AccessorView {
    const uint8_t *base;
    size_t         stride;
    int            count;
    int            comps;
    int            component_type;
    int            normalized;
} AccessorView;

static int accessor_view(Ctx *c, int acc_index, AccessorView *out) {
    int acc = js_at(&c->doc, c->accessors, acc_index);
    if (acc < 0) return -1;
    int count = js_int(&c->doc, acc, "count", -1);
    int ctype = js_int(&c->doc, acc, "componentType", 0);
    int comps = type_comps(&c->doc, js_member(&c->doc, acc, "type"));
    int cb = comp_bytes(ctype);
    if (count <= 0 || comps <= 0 || cb <= 0) return -1;
    if (count > GLTF_MAX_TRIS * 3) return -1;
    if (js_member(&c->doc, acc, "sparse") >= 0) return -1;   /* not read */

    int bv_index = js_int(&c->doc, acc, "bufferView", -1);
    if (bv_index < 0) return -1;
    int bv = js_at(&c->doc, c->buffer_views, bv_index);
    if (bv < 0) return -1;
    if (js_int(&c->doc, bv, "buffer", 0) != 0) return -1;     /* glb has one */

    int acc_off = js_int(&c->doc, acc, "byteOffset", 0);
    int bv_off  = js_int(&c->doc, bv, "byteOffset", 0);
    int bv_len  = js_int(&c->doc, bv, "byteLength", -1);
    int stride  = js_int(&c->doc, bv, "byteStride", 0);
    if (acc_off < 0 || bv_off < 0 || bv_len <= 0 || stride < 0 || stride > 255) return -1;

    size_t elem = (size_t)comps * (size_t)cb;
    if (stride == 0) stride = (int)elem;
    if ((size_t)stride < elem) return -1;

    /* The view inside the chunk. */
    if ((size_t)bv_off > c->bin_size || (size_t)bv_len > c->bin_size - (size_t)bv_off)
        return -1;
    /* The accessor inside the view: offset, then the last element. */
    if ((size_t)acc_off > (size_t)bv_len) return -1;
    size_t avail = (size_t)bv_len - (size_t)acc_off;
    size_t span = (size_t)(count - 1) * (size_t)stride;
    if ((size_t)(count - 1) != 0 && span / (size_t)(count - 1) != (size_t)stride) return -1;
    if (span > avail || elem > avail - span) return -1;

    out->base = c->bin + (size_t)bv_off + (size_t)acc_off;
    out->stride = (size_t)stride;
    out->count = count;
    out->comps = comps;
    out->component_type = ctype;
    out->normalized = js_int(&c->doc, acc, "normalized", 0) ? 1 : 0;
    return 0;
}

static float read_component(const uint8_t *p, int ctype, int normalized) {
    switch (ctype) {
    case 5126: { float f; memcpy(&f, p, 4); return f; }
    case 5121: { uint8_t v = *p; return normalized ? (float)v / 255.0f : (float)v; }
    case 5123: { uint16_t v; memcpy(&v, p, 2); return normalized ? (float)v / 65535.0f : (float)v; }
    case 5120: { int8_t v; memcpy(&v, p, 1); return normalized ? (float)v / 127.0f : (float)v; }
    case 5122: { int16_t v; memcpy(&v, p, 2); return normalized ? (float)v / 32767.0f : (float)v; }
    case 5125: { uint32_t v; memcpy(&v, p, 4); return (float)v; }
    default: return 0.0f;
    }
}

/* Floats out of an accessor, `want` components each. */
static float *read_floats(Ctx *c, int acc_index, int want, int *out_count) {
    AccessorView v;
    if (accessor_view(c, acc_index, &v) != 0) return NULL;
    if (v.comps != want) return NULL;
    if (v.count > GLTF_MAX_VERTS) return NULL;
    float *out = (float *)tak_malloc(sizeof(float) * (size_t)want * (size_t)v.count);
    if (!out) return NULL;
    int cb = comp_bytes(v.component_type);
    for (int i = 0; i < v.count; i++) {
        const uint8_t *e = v.base + (size_t)i * v.stride;
        for (int k = 0; k < want; k++)
            out[i * want + k] = read_component(e + (size_t)k * (size_t)cb,
                                               v.component_type, v.normalized);
    }
    *out_count = v.count;
    return out;
}

/* Triangle indices out of an accessor. */
static uint32_t *read_indices(Ctx *c, int acc_index, int *out_count) {
    AccessorView v;
    if (accessor_view(c, acc_index, &v) != 0) return NULL;
    if (v.comps != 1) return NULL;
    if (v.component_type != 5121 && v.component_type != 5123 && v.component_type != 5125)
        return NULL;
    if (v.count % 3 != 0 || v.count / 3 > GLTF_MAX_TRIS) return NULL;
    uint32_t *out = (uint32_t *)tak_malloc(sizeof(uint32_t) * (size_t)v.count);
    if (!out) return NULL;
    for (int i = 0; i < v.count; i++) {
        const uint8_t *e = v.base + (size_t)i * v.stride;
        uint32_t idx = 0;
        if (v.component_type == 5121) idx = *e;
        else if (v.component_type == 5123) { uint16_t t; memcpy(&t, e, 2); idx = t; }
        else { memcpy(&idx, e, 4); }
        out[i] = idx;
    }
    *out_count = v.count;
    return out;
}

/* ── images ───────────────────────────────────────────────────────── */

/* Decodes image `index`, once, and hands back its slot in the model. */
static int image_slot(Ctx *c, int index, int *cache) {
    if (index < 0 || index >= js_len(&c->doc, c->images)) return -1;
    if (cache[index] >= -1) return cache[index];        /* -1 means tried and failed */

    cache[index] = -1;
    int img = js_at(&c->doc, c->images, index);
    if (img < 0) return -1;
    if (js_member(&c->doc, img, "uri") >= 0) {
        fprintf(stderr, "Gltf: image %d is a separate file, embed it\n", index);
        return -1;
    }
    int bv_index = js_int(&c->doc, img, "bufferView", -1);
    int bv = js_at(&c->doc, c->buffer_views, bv_index);
    if (bv < 0) return -1;
    if (js_int(&c->doc, bv, "buffer", 0) != 0) return -1;
    int off = js_int(&c->doc, bv, "byteOffset", 0);
    int len = js_int(&c->doc, bv, "byteLength", -1);
    if (off < 0 || len <= 0) return -1;
    if ((size_t)off > c->bin_size || (size_t)len > c->bin_size - (size_t)off) return -1;
    if (c->m->image_count >= GLTF_MAX_IMAGES) return -1;

    const uint8_t *bytes = c->bin + off;
    static const uint8_t png_magic[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    int is_png = (size_t)len >= 8 && memcmp(bytes, png_magic, 8) == 0;
    int is_jpg = (size_t)len >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8;
    if (!is_png && !is_jpg) {
        fprintf(stderr, "Gltf: image %d is neither PNG nor JPEG\n", index);
        return -1;
    }
    /* The slot is taken before the picture is decoded, so a reading
     * that skips the pictures numbers them the same as one that does
     * not. A picture that will not decode keeps its slot, empty. */
    int slot = c->m->image_count++;
    cache[index] = slot;
    if (!c->with_images) return slot;

    uint32_t *px = NULL;
    int w = 0, h = 0;
    if (is_png) {
        if (PNG_DecodeRGBA(bytes, (size_t)len, &px, &w, &h) != 0) px = NULL;
    } else {
        if (JPG_DecodeRGBA(bytes, (size_t)len, &px, &w, &h) != 0) px = NULL;
    }
    if (px && (w <= 0 || h <= 0 || (long long)w * h > IMAGE_MAX_PIXELS)) {
        tak_free(px);
        px = NULL;
    }
    if (!px) {
        fprintf(stderr, "Gltf: image %d would not decode, that part draws untextured\n", index);
        return slot;
    }
    c->m->images[slot].rgba = px;
    c->m->images[slot].w = w;
    c->m->images[slot].h = h;
    return slot;
}

/* ── materials ────────────────────────────────────────────────────── */

/* The image a texture reference points at, or -1. A picture laid by
 * a UV set other than the one the material was given is left out,
 * since a primitive carries one set. */
static int texture_image(Ctx *c, int tex_ref, int *cache, int uv_set) {
    if (tex_ref < 0) return -1;
    if (js_int(&c->doc, tex_ref, "texCoord", 0) != uv_set) {
        fprintf(stderr, "Gltf: a picture is laid by TEXCOORD_%d where its material "
                "uses TEXCOORD_%d, left out\n",
                js_int(&c->doc, tex_ref, "texCoord", 0), uv_set);
        return -1;
    }
    int tex = js_at(&c->doc, c->textures, js_int(&c->doc, tex_ref, "index", -1));
    if (tex < 0) return -1;
    return image_slot(c, js_int(&c->doc, tex, "source", -1), cache);
}

static void material_of(Ctx *c, int mat_index, int *cache, GltfSurface *out) {
    memset(out, 0, sizeof(*out));
    out->image = out->normal_image = out->mr_image = out->emissive_image = -1;
    out->base_color[0] = out->base_color[1] = out->base_color[2] = 1.0f;
    out->base_color[3] = 1.0f;
    out->metallic = 1.0f;
    out->roughness = 1.0f;
    out->normal_scale = 1.0f;

    int mat = js_at(&c->doc, c->materials, mat_index);
    if (mat < 0) return;
    if (js_str_eq_ci(&c->doc, js_member(&c->doc, mat, "name"), "teamcolor"))
        out->team_color = 1;
    out->double_sided = js_bool(&c->doc, mat, "doubleSided", 0) ? 1 : 0;

    int am = js_member(&c->doc, mat, "alphaMode");
    if (js_str_eq_ci(&c->doc, am, "BLEND")) out->blend = 1;
    else if (js_str_eq_ci(&c->doc, am, "MASK"))
        out->alpha_cutoff = js_float(&c->doc, mat, "alphaCutoff", 0.5f);

    /* The UV set is the one the first picture named asks for. */
    int pbr0 = js_member(&c->doc, mat, "pbrMetallicRoughness");
    int nt = js_member(&c->doc, mat, "normalTexture");
    int et0 = js_member(&c->doc, mat, "emissiveTexture");
    int bct0 = pbr0 >= 0 ? js_member(&c->doc, pbr0, "baseColorTexture") : -1;
    int mrt0 = pbr0 >= 0 ? js_member(&c->doc, pbr0, "metallicRoughnessTexture") : -1;
    int refs[4] = { bct0, nt, mrt0, et0 };
    int uv_set = 0;
    for (int k = 0; k < 4; k++) {
        if (refs[k] < 0) continue;
        uv_set = js_int(&c->doc, refs[k], "texCoord", 0);
        break;
    }
    if (uv_set < 0 || uv_set > 7) uv_set = 0;
    out->uv_set = (uint8_t)uv_set;

    if (nt >= 0) {
        out->normal_image = texture_image(c, nt, cache, uv_set);
        out->normal_scale = js_float(&c->doc, nt, "scale", 1.0f);
    }
    int ef = js_member(&c->doc, mat, "emissiveFactor");
    if (ef >= 0 && js_len(&c->doc, ef) >= 3) {
        for (int k = 0; k < 3; k++) {
            float v = js_float_at(&c->doc, ef, k, 0.0f);
            out->emissive[k] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    }
    int et = et0;
    if (et >= 0) {
        out->emissive_image = texture_image(c, et, cache, uv_set);
        /* A picture with no factor gives off nothing, by the spec. A
         * file that names the picture and leaves the factor at its
         * default means the picture as it is. */
        if (ef < 0) out->emissive[0] = out->emissive[1] = out->emissive[2] = 1.0f;
    }

    int pbr = js_member(&c->doc, mat, "pbrMetallicRoughness");
    if (pbr < 0) return;
    int bcf = js_member(&c->doc, pbr, "baseColorFactor");
    if (bcf >= 0 && js_len(&c->doc, bcf) >= 4) {
        for (int k = 0; k < 4; k++) {
            float v = js_float_at(&c->doc, bcf, k, 1.0f);
            out->base_color[k] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    }
    out->metallic  = js_float(&c->doc, pbr, "metallicFactor", 1.0f);
    out->roughness = js_float(&c->doc, pbr, "roughnessFactor", 1.0f);
    if (out->metallic < 0.0f) out->metallic = 0.0f;
    if (out->metallic > 1.0f) out->metallic = 1.0f;
    if (out->roughness < 0.04f) out->roughness = 0.04f;
    if (out->roughness > 1.0f) out->roughness = 1.0f;
    out->image    = texture_image(c, js_member(&c->doc, pbr, "baseColorTexture"), cache, uv_set);
    out->mr_image = texture_image(c, js_member(&c->doc, pbr, "metallicRoughnessTexture"), cache, uv_set);
    /* A material that says it blends but is opaque all through costs a
     * second pass for nothing. */
    if (out->blend && out->base_color[3] >= 0.999f && out->image < 0) out->blend = 0;
}

/* ── nodes ────────────────────────────────────────────────────────── */

/* A node's own rotation and scale as a row major 3x3, and its
 * translation. glTF gives either a matrix or a translation, rotation
 * and scale; both land here the same way. */
static void node_local(const JsDoc *d, int node, float rs[9], float t[3]) {
    rs[0] = rs[4] = rs[8] = 1.0f;
    rs[1] = rs[2] = rs[3] = rs[5] = rs[6] = rs[7] = 0.0f;
    t[0] = t[1] = t[2] = 0.0f;

    int m = js_member(d, node, "matrix");
    if (m >= 0 && js_len(d, m) >= 16) {
        /* Column major in the file: element (row, col) is at col*4+row. */
        for (int col = 0; col < 3; col++)
            for (int row = 0; row < 3; row++)
                rs[row * 3 + col] = js_float_at(d, m, col * 4 + row, row == col ? 1.0f : 0.0f);
        for (int k = 0; k < 3; k++) t[k] = js_float_at(d, m, 12 + k, 0.0f);
        return;
    }

    int tr = js_member(d, node, "translation");
    if (tr >= 0 && js_len(d, tr) >= 3)
        for (int k = 0; k < 3; k++) t[k] = js_float_at(d, tr, k, 0.0f);

    float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int ro = js_member(d, node, "rotation");
    if (ro >= 0 && js_len(d, ro) >= 4)
        for (int k = 0; k < 4; k++) q[k] = js_float_at(d, ro, k, k == 3 ? 1.0f : 0.0f);
    float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len > 1e-6f) { for (int k = 0; k < 4; k++) q[k] /= len; }
    else { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }

    float s[3] = { 1.0f, 1.0f, 1.0f };
    int sc = js_member(d, node, "scale");
    if (sc >= 0 && js_len(d, sc) >= 3)
        for (int k = 0; k < 3; k++) s[k] = js_float_at(d, sc, k, 1.0f);

    const float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[9];
    r[0] = 1.0f - 2.0f*(y*y + z*z); r[1] = 2.0f*(x*y - w*z);        r[2] = 2.0f*(x*z + w*y);
    r[3] = 2.0f*(x*y + w*z);        r[4] = 1.0f - 2.0f*(x*x + z*z); r[5] = 2.0f*(y*z - w*x);
    r[6] = 2.0f*(x*z - w*y);        r[7] = 2.0f*(y*z + w*x);        r[8] = 1.0f - 2.0f*(x*x + y*y);
    /* Scale is applied in the node's own axes, so it scales columns. */
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            rs[row * 3 + col] = r[row * 3 + col] * s[col];
}

static void mat3_mul(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] +
                             a[r * 3 + 1] * b[1 * 3 + c] +
                             a[r * 3 + 2] * b[2 * 3 + c];
}

static void mat3_apply(const float m[9], const float v[3], float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = m[r * 3 + 0] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
}

typedef struct NodeWalk {
    float accum[GLTF_MAX_NODES][9];   /* rotation and scale down the chain */
    float world[GLTF_MAX_NODES][3];   /* translation in model space */
    int   json_of[GLTF_MAX_NODES];    /* which JSON node each one is */
    uint8_t seen[GLTF_MAX_NODES];     /* a node reached twice is a cycle */
} NodeWalk;

/* Depth first, parents before children, which is the order the mesh
 * downstream requires. Returns 0, or -1 on a cycle or an overflow. */
static int walk_nodes(Ctx *c, NodeWalk *w, int json_index, int parent, int depth) {
    if (depth > GLTF_MAX_NODES) return -1;
    int node = js_at(&c->doc, c->nodes, json_index);
    if (node < 0) return -1;
    if (json_index < GLTF_MAX_NODES) {
        if (w->seen[json_index]) {
            fprintf(stderr, "Gltf: node %d is its own ancestor or is shared\n", json_index);
            return -1;
        }
        w->seen[json_index] = 1;
    }
    if (c->m->node_count >= GLTF_MAX_NODES) return -1;
    int self = c->m->node_count++;

    float rs[9], t[3];
    node_local(&c->doc, node, rs, t);

    if (parent < 0) {
        memcpy(w->accum[self], rs, sizeof(rs));
        for (int k = 0; k < 3; k++) w->world[self][k] = t[k];
    } else {
        mat3_mul(w->accum[parent], rs, w->accum[self]);
        float rotated[3];
        mat3_apply(w->accum[parent], t, rotated);
        for (int k = 0; k < 3; k++) w->world[self][k] = w->world[parent][k] + rotated[k];
    }
    w->json_of[self] = node;

    GltfNode *out = &c->m->nodes[self];
    js_copy_str(&c->doc, js_member(&c->doc, node, "name"), out->name, sizeof(out->name));
    if (!out->name[0]) snprintf(out->name, sizeof(out->name), "node%d", self);
    out->parent = parent;
    for (int k = 0; k < 3; k++)
        out->offset[k] = w->world[self][k] - (parent < 0 ? 0.0f : w->world[parent][k]);

    int children = js_member(&c->doc, node, "children");
    int n = js_len(&c->doc, children);
    for (int i = 0; i < n; i++) {
        int ch = js_at(&c->doc, children, i);
        if (ch < 0 || c->doc.nodes[ch].type != JS_NUM) return -1;
        if (walk_nodes(c, w, (int)c->doc.nodes[ch].num, self, depth + 1) != 0) return -1;
    }
    return 0;
}

/* ── primitives ───────────────────────────────────────────────────── */

static int add_prims_for_node(Ctx *c, const NodeWalk *w, int slot, int *img_cache,
                              GltfPrim *prims, int *prim_count) {
    int node = w->json_of[slot];
    int mesh_index = js_int(&c->doc, node, "mesh", -1);
    if (mesh_index < 0) return 0;
    int mesh = js_at(&c->doc, c->meshes, mesh_index);
    if (mesh < 0) return 0;
    int list = js_member(&c->doc, mesh, "primitives");
    int n = js_len(&c->doc, list);

    for (int i = 0; i < n; i++) {
        if (*prim_count >= GLTF_MAX_PRIMS) return -1;
        int prim = js_at(&c->doc, list, i);
        if (prim < 0) return -1;
        /* Mode 4 is triangles, and the default when it is not said. */
        if (js_int(&c->doc, prim, "mode", 4) != 4) continue;

        int attrs = js_member(&c->doc, prim, "attributes");
        int pos_acc = js_int(&c->doc, attrs, "POSITION", -1);
        if (pos_acc < 0) continue;

        int vert_count = 0;
        float *pos = read_floats(c, pos_acc, 3, &vert_count);
        if (!pos) return -1;

        float *nrm = NULL, *tan = NULL;
        int n_count = 0;
        int nrm_acc = js_int(&c->doc, attrs, "NORMAL", -1);
        if (nrm_acc >= 0) {
            nrm = read_floats(c, nrm_acc, 3, &n_count);
            if (nrm && n_count != vert_count) { tak_free(nrm); nrm = NULL; }
        }
        int tan_acc = js_int(&c->doc, attrs, "TANGENT", -1);
        if (tan_acc >= 0) {
            tan = read_floats(c, tan_acc, 4, &n_count);
            if (tan && n_count != vert_count) { tak_free(tan); tan = NULL; }
        }

        GltfSurface surface;
        material_of(c, js_int(&c->doc, prim, "material", -1), img_cache, &surface);

        float *uv = NULL;
        int uv_count = 0;
        char uv_key[16];
        snprintf(uv_key, sizeof(uv_key), "TEXCOORD_%d", (int)surface.uv_set);
        int uv_acc = js_int(&c->doc, attrs, uv_key, -1);
        if (uv_acc < 0 && surface.uv_set != 0) {
            fprintf(stderr, "Gltf: a primitive has no %s, its first set stands in\n", uv_key);
            uv_acc = js_int(&c->doc, attrs, "TEXCOORD_0", -1);
        }
        if (uv_acc >= 0) {
            uv = read_floats(c, uv_acc, 2, &uv_count);
            if (!uv || uv_count != vert_count) {
                if (uv) tak_free(uv);
                tak_free(pos);
                if (nrm) tak_free(nrm);
                if (tan) tak_free(tan);
                return -1;
            }
        } else {
            uv = (float *)tak_malloc(sizeof(float) * 2 * (size_t)vert_count);
            if (!uv) { tak_free(pos); if (nrm) tak_free(nrm); if (tan) tak_free(tan); return -1; }
            memset(uv, 0, sizeof(float) * 2 * (size_t)vert_count);
        }

        uint32_t *raw_idx = NULL;
        int idx_count = 0;
        int idx_acc = js_int(&c->doc, prim, "indices", -1);
        if (idx_acc >= 0) {
            raw_idx = read_indices(c, idx_acc, &idx_count);
            if (!raw_idx) { tak_free(pos); tak_free(uv); if (nrm) tak_free(nrm); if (tan) tak_free(tan); return -1; }
        } else {
            if (vert_count % 3 != 0) { tak_free(pos); tak_free(uv); if (nrm) tak_free(nrm); if (tan) tak_free(tan); return -1; }
            idx_count = vert_count;
            raw_idx = (uint32_t *)tak_malloc(sizeof(uint32_t) * (size_t)idx_count);
            if (!raw_idx) { tak_free(pos); tak_free(uv); if (nrm) tak_free(nrm); if (tan) tak_free(tan); return -1; }
            for (int k = 0; k < idx_count; k++) raw_idx[k] = (uint32_t)k;
        }

        uint16_t *idx = (uint16_t *)tak_malloc(sizeof(uint16_t) * (size_t)idx_count);
        if (!idx) { tak_free(pos); tak_free(uv); tak_free(raw_idx); if (nrm) tak_free(nrm); if (tan) tak_free(tan); return -1; }
        for (int k = 0; k < idx_count; k++) {
            if (raw_idx[k] >= (uint32_t)vert_count) {
                fprintf(stderr, "Gltf: an index points past the vertices\n");
                tak_free(pos); tak_free(uv); tak_free(raw_idx); tak_free(idx);
                if (nrm) tak_free(nrm);
                if (tan) tak_free(tan);
                return -1;
            }
            idx[k] = (uint16_t)raw_idx[k];
        }
        tak_free(raw_idx);

        /* Into the node's own frame: its rotation and scale go into the
         * vertices, its translation stays in the node offset. */
        for (int v = 0; v < vert_count; v++) {
            float in[3] = { pos[3*v], pos[3*v+1], pos[3*v+2] };
            float out[3];
            mat3_apply(w->accum[slot], in, out);
            pos[3*v] = out[0]; pos[3*v+1] = out[1]; pos[3*v+2] = out[2];
            if (nrm) {
                float n_in[3] = { nrm[3*v], nrm[3*v+1], nrm[3*v+2] };
                mat3_apply(w->accum[slot], n_in, out);
                float len = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
                if (len > 1e-8f) { out[0] /= len; out[1] /= len; out[2] /= len; }
                nrm[3*v] = out[0]; nrm[3*v+1] = out[1]; nrm[3*v+2] = out[2];
            }
            if (tan) {
                float t_in[3] = { tan[4*v], tan[4*v+1], tan[4*v+2] };
                mat3_apply(w->accum[slot], t_in, out);
                float len = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
                if (len > 1e-8f) { out[0] /= len; out[1] /= len; out[2] /= len; }
                tan[4*v] = out[0]; tan[4*v+1] = out[1]; tan[4*v+2] = out[2];
            }
        }

        GltfPrim *p = &prims[(*prim_count)++];
        memset(p, 0, sizeof(*p));
        p->node = slot;
        p->vert_count = vert_count;
        p->tri_count = idx_count / 3;
        p->pos = pos;
        p->nrm = nrm;
        p->tan = tan;
        p->uv = uv;
        p->idx = idx;
        p->surface = surface;
    }
    return 0;
}

/* ── the load ─────────────────────────────────────────────────────── */

static void free_prims(GltfPrim *prims, int count) {
    for (int i = 0; i < count; i++) {
        if (prims[i].pos) tak_free(prims[i].pos);
        if (prims[i].nrm) tak_free(prims[i].nrm);
        if (prims[i].tan) tak_free(prims[i].tan);
        if (prims[i].uv) tak_free(prims[i].uv);
        if (prims[i].idx) tak_free(prims[i].idx);
    }
}

void Gltf_Free(GltfModel *m) {
    if (!m) return;
    if (m->prims) {
        free_prims(m->prims, m->prim_count);
        tak_free(m->prims);
    }
    for (int i = 0; i < m->image_count; i++)
        if (m->images[i].rgba) tak_free(m->images[i].rgba);
    tak_free(m);
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int Gltf_LoadFromMemoryEx(GltfModel **out, const uint8_t *bytes, size_t size,
                          int with_images) {
    if (out) *out = NULL;
    if (!out || !bytes) return -1;
    if (size < 20 || size > GLB_MAX_BYTES) return -1;
    if (rd_u32(bytes) != 0x46546C67u) return -1;             /* "glTF" */
    if (rd_u32(bytes + 4) != 2u) {
        fprintf(stderr, "Gltf: version %u, this reads glTF 2\n", rd_u32(bytes + 4));
        return -1;
    }
    uint32_t total = rd_u32(bytes + 8);
    if (total < 12 || (size_t)total > size) return -1;

    const uint8_t *json = NULL, *bin = NULL;
    size_t json_len = 0, bin_len = 0;
    size_t at = 12;
    while (at + 8 <= (size_t)total) {
        uint32_t clen = rd_u32(bytes + at);
        uint32_t ctype = rd_u32(bytes + at + 4);
        at += 8;
        if ((size_t)clen > (size_t)total - at) return -1;
        if (ctype == 0x4E4F534Au && !json) { json = bytes + at; json_len = clen; }
        else if (ctype == 0x004E4942u && !bin) { bin = bytes + at; bin_len = clen; }
        size_t padded = ((size_t)clen + 3u) & ~(size_t)3u;
        if (padded > (size_t)total - at) break;
        at += padded;
    }
    if (!json || json_len == 0) return -1;

    /* The JSON chunk is padded with spaces rather than ended, and the
     * number reader wants a string it can stop at. */
    char *text = (char *)tak_malloc(json_len + 1);
    if (!text) return -1;
    memcpy(text, json, json_len);
    text[json_len] = '\0';

    GltfModel *m = (GltfModel *)tak_malloc(sizeof(GltfModel));
    if (!m) { tak_free(text); return -1; }
    memset(m, 0, sizeof(*m));
    m->scale_hint = 1.0f;

    Ctx c;
    memset(&c, 0, sizeof(c));
    c.with_images = with_images ? 1 : 0;
    c.m = m;
    c.bin = bin;
    c.bin_size = bin_len;

    int rc = -1;
    NodeWalk *w = NULL;
    GltfPrim *prims = NULL;
    int prim_count = 0;
    int img_cache[GLTF_MAX_IMAGES * 4];

    c.root = js_parse(&c.doc, text, json_len);
    if (c.root < 0 || c.doc.nodes[c.root].type != JS_OBJ) {
        fprintf(stderr, "Gltf: the JSON chunk does not parse\n");
        goto done;
    }

    c.accessors    = js_member(&c.doc, c.root, "accessors");
    c.buffer_views = js_member(&c.doc, c.root, "bufferViews");
    c.meshes       = js_member(&c.doc, c.root, "meshes");
    c.materials    = js_member(&c.doc, c.root, "materials");
    c.textures     = js_member(&c.doc, c.root, "textures");
    c.images       = js_member(&c.doc, c.root, "images");
    c.nodes        = js_member(&c.doc, c.root, "nodes");
    if (c.nodes < 0 || js_len(&c.doc, c.nodes) <= 0) {
        fprintf(stderr, "Gltf: the file has no nodes\n");
        goto done;
    }
    if (js_len(&c.doc, c.images) > GLTF_MAX_IMAGES * 4) goto done;

    {
        int extras = js_member(&c.doc, c.root, "extras");
        float s = js_float(&c.doc, extras, "tak_scale", 1.0f);
        if (s > 0.0f && s < 1e6f) m->scale_hint = s;
    }

    for (int i = 0; i < GLTF_MAX_IMAGES * 4; i++) img_cache[i] = -2;

    w = (NodeWalk *)tak_malloc(sizeof(NodeWalk));
    prims = (GltfPrim *)tak_malloc(sizeof(GltfPrim) * GLTF_MAX_PRIMS);
    if (!w || !prims) goto done;
    memset(w, 0, sizeof(*w));
    memset(prims, 0, sizeof(GltfPrim) * GLTF_MAX_PRIMS);

    /* The roots are the scene's if it names one, otherwise every node
     * nothing else claims as a child. */
    {
        int scene_index = js_int(&c.doc, c.root, "scene", -1);
        int scenes = js_member(&c.doc, c.root, "scenes");
        int roots = -1;
        if (scene_index >= 0) {
            int scene = js_at(&c.doc, scenes, scene_index);
            if (scene >= 0) roots = js_member(&c.doc, scene, "nodes");
        }
        if (roots >= 0) {
            int n = js_len(&c.doc, roots);
            for (int i = 0; i < n; i++) {
                int r = js_at(&c.doc, roots, i);
                if (r < 0 || c.doc.nodes[r].type != JS_NUM) goto done;
                if (walk_nodes(&c, w, (int)c.doc.nodes[r].num, -1, 0) != 0) goto done;
            }
        } else {
            int n = js_len(&c.doc, c.nodes);
            if (n > GLTF_MAX_NODES) goto done;
            uint8_t is_child[GLTF_MAX_NODES];
            memset(is_child, 0, sizeof(is_child));
            for (int i = 0; i < n; i++) {
                int node = js_at(&c.doc, c.nodes, i);
                int children = js_member(&c.doc, node, "children");
                int cn = js_len(&c.doc, children);
                for (int k = 0; k < cn; k++) {
                    int ch = js_at(&c.doc, children, k);
                    if (ch < 0 || c.doc.nodes[ch].type != JS_NUM) goto done;
                    int v = (int)c.doc.nodes[ch].num;
                    if (v >= 0 && v < GLTF_MAX_NODES) is_child[v] = 1;
                }
            }
            for (int i = 0; i < n; i++)
                if (!is_child[i] && walk_nodes(&c, w, i, -1, 0) != 0) goto done;
        }
    }
    if (m->node_count <= 0) goto done;

    for (int slot = 0; slot < m->node_count; slot++) {
        if (add_prims_for_node(&c, w, slot, img_cache, prims, &prim_count) != 0) {
            free_prims(prims, prim_count);
            prim_count = 0;
            goto done;
        }
    }
    if (prim_count <= 0) {
        fprintf(stderr, "Gltf: the file draws nothing\n");
        goto done;
    }

    m->prims = (GltfPrim *)tak_malloc(sizeof(GltfPrim) * (size_t)prim_count);
    if (!m->prims) { free_prims(prims, prim_count); goto done; }
    memcpy(m->prims, prims, sizeof(GltfPrim) * (size_t)prim_count);
    m->prim_count = prim_count;
    prim_count = 0;
    rc = 0;

done:
    if (prims) {
        if (prim_count > 0) free_prims(prims, prim_count);
        tak_free(prims);
    }
    if (w) tak_free(w);
    js_free(&c.doc);
    tak_free(text);
    if (rc != 0) { Gltf_Free(m); return -1; }
    *out = m;
    return 0;
}

int Gltf_LoadFromMemory(GltfModel **out, const uint8_t *bytes, size_t size) {
    return Gltf_LoadFromMemoryEx(out, bytes, size, GLTF_WITH_IMAGES);
}

int Gltf_LoadEx(GltfModel **out, const char *vfs_path, int with_images) {
    if (out) *out = NULL;
    if (!out || !vfs_path) return -1;
    void *bytes = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(vfs_path, &bytes, &size) != 0 || !bytes) return -1;
    int rc = Gltf_LoadFromMemoryEx(out, (const uint8_t *)bytes, (size_t)size, with_images);
    tak_free(bytes);
    return rc;
}

int Gltf_Load(GltfModel **out, const char *vfs_path) {
    return Gltf_LoadEx(out, vfs_path, GLTF_WITH_IMAGES);
}
