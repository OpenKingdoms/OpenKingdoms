#include "tak_savefile.h"

#include "tak_build_stamp.h"
#include "tak_bytes.h"
#include "tak_memory.h"
#include "tak_sha256.h"

#include "miniz.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

/* Offsets into the 240 byte header. Written and read one field at a
 * time, never as a struct. */
#define OFF_MAGIC        0
#define OFF_CONTAINER    8
#define OFF_HEADER_BYTES 12
#define OFF_FILE_BYTES   16
#define OFF_SCHEMA       24
#define OFF_FLAGS        28
#define OFF_DIGEST       32
#define OFF_SECTIONS     64
#define OFF_SIM_TICK     68
#define OFF_SIM_HASH     72
#define OFF_RNG_AI       76
#define OFF_DETERMINISM  80
#define OFF_TICK_HZ      81
#define OFF_SAVE_KIND    82
#define OFF_FLOAT_FORM   83
#define OFF_STABLE_NEXT  84
#define OFF_SLOT_COUNT   88
#define OFF_SAVED_AT     92
#define OFF_BUILD        100
#define OFF_FINGERPRINT  164
#define OFF_RESERVED     196

static const uint8_t k_magic[TAK_SAVE_MAGIC_BYTES] = {
    0x4f, 0x4b, 0x53, 0x41, 0x56, 0x45, 0x1a, 0x0a   /* OKSAVE, 0x1a, 0x0a */
};

/* Deflate at level 1. Measured against a 359 KB state blob it keeps 94
 * percent of level 9's benefit for 9 percent of its cost, and the load
 * list never inflates anything because SUMM is written plain. */
#define SAVE_DEFLATE_LEVEL 1

/* A section that shrinks by less than this is stored plain, so a
 * reader that skips it does not pay for an inflate it never needed. */
#define SAVE_DEFLATE_MIN_GAIN 64

static void set_err(char *err, size_t cap, const char *fmt, ...);

/* ── growable byte buffer ─────────────────────────────────────────── */

typedef struct ByteBuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} ByteBuf;

static int buf_reserve(ByteBuf *b, size_t need) {
    if (b->cap >= need) return 0;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) return -1;
        cap *= 2;
    }
    uint8_t *p = (uint8_t *)tak_realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

static int buf_append(ByteBuf *b, const void *src, size_t n) {
    if (n == 0) return 0;
    if (buf_reserve(b, b->len + n) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int buf_append_zeros(ByteBuf *b, size_t n) {
    if (n == 0) return 0;
    if (buf_reserve(b, b->len + n) != 0) return -1;
    memset(b->data + b->len, 0, n);
    b->len += n;
    return 0;
}

/* ── header ───────────────────────────────────────────────────────── */

void Save_HeaderInit(TAK_SaveHeader *hdr) {
    if (!hdr) return;
    memset(hdr, 0, sizeof(*hdr));
    hdr->container_version = TAK_SAVE_CONTAINER_VERSION;
    hdr->tick_hz = 60;
    hdr->determinism_class = TAK_DETERMINISM_FLOAT;
    hdr->float_form = TAK_FLOAT_FORM_BINARY32;
    hdr->save_kind = TAK_SAVE_KIND_SKIRMISH;
    snprintf(hdr->engine_build, sizeof(hdr->engine_build), "%s", TAK_ENGINE_BUILD);
}

static void header_pack(const TAK_SaveHeader *h, uint8_t *p) {
    memset(p, 0, TAK_SAVE_HEADER_BYTES);
    memcpy(p + OFF_MAGIC, k_magic, sizeof(k_magic));
    tak_put_u32(p + OFF_CONTAINER, h->container_version);
    tak_put_u32(p + OFF_HEADER_BYTES, TAK_SAVE_HEADER_BYTES);
    tak_put_u64(p + OFF_FILE_BYTES, 0);        /* stamped when sealed */
    tak_put_u32(p + OFF_SCHEMA, h->schema_version);
    tak_put_u32(p + OFF_FLAGS, h->flags);
    /* digest stays zero until the file is sealed */
    tak_put_u32(p + OFF_SECTIONS, 0);          /* stamped when sealed */
    tak_put_u32(p + OFF_SIM_TICK, h->sim_tick);
    tak_put_u32(p + OFF_SIM_HASH, h->sim_state_hash);
    tak_put_u32(p + OFF_RNG_AI, h->rng_ai);
    tak_put_u8(p + OFF_DETERMINISM, h->determinism_class);
    tak_put_u8(p + OFF_TICK_HZ, h->tick_hz);
    tak_put_u8(p + OFF_SAVE_KIND, h->save_kind);
    tak_put_u8(p + OFF_FLOAT_FORM, h->float_form);
    tak_put_u32(p + OFF_STABLE_NEXT, h->unit_stable_id_next);
    tak_put_u32(p + OFF_SLOT_COUNT, h->unit_slot_count);
    tak_put_u64(p + OFF_SAVED_AT, h->saved_at_utc);
    memcpy(p + OFF_BUILD, h->engine_build, TAK_SAVE_BUILD_MAX);
    memcpy(p + OFF_FINGERPRINT, h->map_fingerprint, TAK_SHA256_BYTES);
}

static void header_unpack(TAK_SaveHeader *h, const uint8_t *p) {
    memset(h, 0, sizeof(*h));
    h->container_version = tak_get_u32(p + OFF_CONTAINER);
    h->file_bytes = tak_get_u64(p + OFF_FILE_BYTES);
    h->schema_version = tak_get_u32(p + OFF_SCHEMA);
    h->flags = tak_get_u32(p + OFF_FLAGS);
    memcpy(h->digest, p + OFF_DIGEST, TAK_SHA256_BYTES);
    h->section_count = tak_get_u32(p + OFF_SECTIONS);
    h->sim_tick = tak_get_u32(p + OFF_SIM_TICK);
    h->sim_state_hash = tak_get_u32(p + OFF_SIM_HASH);
    h->rng_ai = tak_get_u32(p + OFF_RNG_AI);
    h->determinism_class = tak_get_u8(p + OFF_DETERMINISM);
    h->tick_hz = tak_get_u8(p + OFF_TICK_HZ);
    h->save_kind = tak_get_u8(p + OFF_SAVE_KIND);
    h->float_form = tak_get_u8(p + OFF_FLOAT_FORM);
    h->unit_stable_id_next = tak_get_u32(p + OFF_STABLE_NEXT);
    h->unit_slot_count = tak_get_u32(p + OFF_SLOT_COUNT);
    h->saved_at_utc = tak_get_u64(p + OFF_SAVED_AT);
    memcpy(h->engine_build, p + OFF_BUILD, TAK_SAVE_BUILD_MAX);
    h->engine_build[TAK_SAVE_BUILD_MAX - 1] = '\0';
    memcpy(h->map_fingerprint, p + OFF_FINGERPRINT, TAK_SHA256_BYTES);
}

void Save_IdToText(uint32_t id, char out[5]) {
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)((id >> (8 * i)) & 0xffu);
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[4] = '\0';
}

/* ── digest ───────────────────────────────────────────────────────── */

/* SHA-256 over the whole file with the 32 digest bytes read as zero,
 * so the digest covers the header it sits in. */
static void digest_file(const uint8_t *bytes, size_t len,
                        uint8_t out[TAK_SHA256_BYTES]) {
    static const uint8_t zeros[TAK_SHA256_BYTES] = { 0 };
    TAK_Sha256 ctx;
    TAK_Sha256_Init(&ctx);
    TAK_Sha256_Update(&ctx, bytes, OFF_DIGEST);
    TAK_Sha256_Update(&ctx, zeros, TAK_SHA256_BYTES);
    TAK_Sha256_Update(&ctx, bytes + OFF_DIGEST + TAK_SHA256_BYTES,
                      len - OFF_DIGEST - TAK_SHA256_BYTES);
    TAK_Sha256_Final(&ctx, out);
}

/* ── writer ───────────────────────────────────────────────────────── */

struct TAK_SaveWriter {
    TAK_SaveHeader hdr;
    ByteBuf        body;        /* section headers and payloads */
    uint32_t       sections;
    int            failed;
    ByteBuf        sealed;      /* header plus body, once finished */
};

TAK_SaveWriter *Save_BeginWrite(const TAK_SaveHeader *hdr) {
    TAK_SaveWriter *w = (TAK_SaveWriter *)tak_malloc(sizeof(*w));
    if (!w) return NULL;
    memset(w, 0, sizeof(*w));
    if (hdr) {
        w->hdr = *hdr;
    } else {
        Save_HeaderInit(&w->hdr);
    }
    w->hdr.container_version = TAK_SAVE_CONTAINER_VERSION;
    return w;
}

int Save_AddSection(TAK_SaveWriter *w, uint32_t id, uint16_t version,
                    uint16_t flags, const void *payload, size_t len) {
    if (!w || w->failed) return -1;
    if (len > 0xffffffffu) { w->failed = 1; return -1; }
    if (len > 0 && !payload) { w->failed = 1; return -1; }

    const uint8_t *stored = (const uint8_t *)payload;
    size_t stored_len = len;
    uint8_t *packed = NULL;
    uint16_t out_flags = (uint16_t)(flags & ~(uint16_t)TAK_SECT_F_DEFLATED);

    /* SUMM is never deflated: the load list reads it with one short
     * read per file and must not pay for an inflate. */
    if (len > SAVE_DEFLATE_MIN_GAIN && id != TAK_SECT_SUMM) {
        mz_ulong bound = mz_compressBound((mz_ulong)len);
        packed = (uint8_t *)tak_malloc((size_t)bound);
        if (packed) {
            mz_ulong got = bound;
            if (mz_compress2(packed, &got, (const unsigned char *)payload,
                             (mz_ulong)len, SAVE_DEFLATE_LEVEL) == MZ_OK &&
                (size_t)got + SAVE_DEFLATE_MIN_GAIN < len) {
                stored = packed;
                stored_len = (size_t)got;
                out_flags |= TAK_SECT_F_DEFLATED;
                w->hdr.flags |= TAK_SAVE_F_DEFLATE;
            } else {
                tak_free(packed);
                packed = NULL;
            }
        }
    }

    uint8_t head[TAK_SAVE_SECTION_BYTES];
    tak_put_u32(head + 0, id);
    tak_put_u16(head + 4, version);
    tak_put_u16(head + 6, out_flags);
    tak_put_u32(head + 8, (uint32_t)stored_len);
    tak_put_u32(head + 12, (uint32_t)len);

    int rc = buf_append(&w->body, head, sizeof(head));
    if (rc == 0) rc = buf_append(&w->body, stored, stored_len);
    tak_free(packed);
    if (rc != 0) { w->failed = 1; return -1; }
    w->sections++;
    return 0;
}

int Save_AddRecords(TAK_SaveWriter *w, uint32_t id, uint16_t version,
                    uint16_t flags, uint32_t count, uint16_t fixed_bytes,
                    const void *records) {
    if (!w || w->failed) return -1;
    size_t body = (size_t)count * (size_t)fixed_bytes;
    ByteBuf payload = { 0 };
    uint8_t head[6];
    tak_put_u32(head + 0, count);
    tak_put_u16(head + 4, fixed_bytes);
    int rc = buf_append(&payload, head, sizeof(head));
    if (rc == 0 && body) rc = buf_append(&payload, records, body);
    if (rc != 0) {
        tak_free(payload.data);
        w->failed = 1;
        return -1;
    }
    rc = Save_AddSection(w, id, version,
                         (uint16_t)(flags | TAK_SECT_F_RECORDS),
                         payload.data, payload.len);
    tak_free(payload.data);
    return rc;
}

const uint8_t *Save_FinishToMemory(TAK_SaveWriter *w, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!w || w->failed) return NULL;
    if (w->sealed.len) {
        if (out_len) *out_len = w->sealed.len;
        return w->sealed.data;
    }
    if (buf_append_zeros(&w->sealed, TAK_SAVE_HEADER_BYTES) != 0) return NULL;
    if (buf_append(&w->sealed, w->body.data, w->body.len) != 0) return NULL;

    header_pack(&w->hdr, w->sealed.data);
    tak_put_u64(w->sealed.data + OFF_FILE_BYTES, (uint64_t)w->sealed.len);
    tak_put_u32(w->sealed.data + OFF_SECTIONS, w->sections);
    digest_file(w->sealed.data, w->sealed.len, w->sealed.data + OFF_DIGEST);

    if (out_len) *out_len = w->sealed.len;
    return w->sealed.data;
}

int Save_FinishToFile(TAK_SaveWriter *w, const char *path,
                      char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    size_t len = 0;
    const uint8_t *bytes = Save_FinishToMemory(w, &len);
    if (!bytes || !path) {
        set_err(err, err_cap, "The save could not be built.");
        return -1;
    }

    char tmp[1200];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        set_err(err, err_cap, "That save path is too long.");
        return -1;
    }

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        set_err(err, err_cap, "Could not open %s for writing.", tmp);
        return -1;
    }
    size_t wrote = fwrite(bytes, 1, len, fp);
    int closed = fclose(fp);
    if (wrote != len || closed != 0) {
        remove(tmp);
        set_err(err, err_cap, "Could not write %s.", tmp);
        return -1;
    }
    /* Replace in one step so a crash mid write leaves the previous
     * save intact. Plain rename refuses an existing destination on
     * Windows, which is what MoveFileEx is for. */
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        set_err(err, err_cap, "Could not replace %s.", path);
        return -1;
    }
#else
    if (rename(tmp, path) != 0) {
        remove(tmp);
        set_err(err, err_cap, "Could not replace %s.", path);
        return -1;
    }
#endif
    return 0;
}

void Save_EndWrite(TAK_SaveWriter *w) {
    if (!w) return;
    tak_free(w->body.data);
    tak_free(w->sealed.data);
    tak_free(w);
}

/* ── reader ───────────────────────────────────────────────────────── */

typedef struct SectionEntry {
    uint32_t id;
    uint16_t version;
    uint16_t flags;
    uint32_t stored_bytes;
    uint32_t plain_bytes;
    size_t   offset;        /* of the payload inside the file */
    uint8_t *plain;         /* inflated payload, cached */
} SectionEntry;

typedef struct KnownEntry {
    uint32_t id;
    uint16_t max_version;
} KnownEntry;

struct TAK_SaveReader {
    uint8_t       *bytes;
    size_t         len;
    TAK_SaveHeader hdr;
    SectionEntry  *sections;
    int            section_count;
    KnownEntry    *known;
    int            known_count;
    int            known_cap;
};

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static void reader_free(TAK_SaveReader *r) {
    if (!r) return;
    if (r->sections) {
        for (int i = 0; i < r->section_count; i++) tak_free(r->sections[i].plain);
        tak_free(r->sections);
    }
    tak_free(r->known);
    tak_free(r->bytes);
    tak_free(r);
}

TAK_SaveReader *Save_OpenMemory(const void *bytes, size_t len,
                                char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!bytes || len < TAK_SAVE_HEADER_BYTES) {
        set_err(err, err_cap,
                "This file is too short to be a saved game.");
        return NULL;
    }
    const uint8_t *p = (const uint8_t *)bytes;
    if (memcmp(p, k_magic, sizeof(k_magic)) != 0) {
        set_err(err, err_cap, "This is not a saved game.");
        return NULL;
    }
    uint32_t container = tak_get_u32(p + OFF_CONTAINER);
    if (container > TAK_SAVE_CONTAINER_VERSION) {
        set_err(err, err_cap,
                "This save was written by a newer version of the game "
                "(format %u, this build reads %u).",
                (unsigned)container, (unsigned)TAK_SAVE_CONTAINER_VERSION);
        return NULL;
    }
    if (tak_get_u32(p + OFF_HEADER_BYTES) != TAK_SAVE_HEADER_BYTES) {
        set_err(err, err_cap, "This save has an unreadable header.");
        return NULL;
    }
    uint64_t declared = tak_get_u64(p + OFF_FILE_BYTES);
    if (declared != (uint64_t)len) {
        set_err(err, err_cap,
                "This save is incomplete: it says %llu bytes and holds %llu.",
                (unsigned long long)declared, (unsigned long long)len);
        return NULL;
    }

    TAK_SaveReader *r = (TAK_SaveReader *)tak_malloc(sizeof(*r));
    if (!r) {
        set_err(err, err_cap, "Out of memory reading the save.");
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->bytes = (uint8_t *)tak_malloc(len);
    if (!r->bytes) {
        reader_free(r);
        set_err(err, err_cap, "Out of memory reading the save.");
        return NULL;
    }
    memcpy(r->bytes, bytes, len);
    r->len = len;
    header_unpack(&r->hdr, r->bytes);

    uint8_t want[TAK_SHA256_BYTES];
    digest_file(r->bytes, r->len, want);
    if (memcmp(want, r->hdr.digest, TAK_SHA256_BYTES) != 0) {
        reader_free(r);
        set_err(err, err_cap, "This save is damaged and cannot be loaded.");
        return NULL;
    }

    uint32_t count = r->hdr.section_count;
    if (count > 0) {
        /* Refuse a count that cannot possibly fit before allocating
         * for it. */
        if ((uint64_t)count * TAK_SAVE_SECTION_BYTES >
            (uint64_t)len - TAK_SAVE_HEADER_BYTES) {
            reader_free(r);
            set_err(err, err_cap, "This save is incomplete.");
            return NULL;
        }
        r->sections = (SectionEntry *)tak_malloc(sizeof(SectionEntry) * count);
        if (!r->sections) {
            reader_free(r);
            set_err(err, err_cap, "Out of memory reading the save.");
            return NULL;
        }
        memset(r->sections, 0, sizeof(SectionEntry) * count);
    }

    size_t off = TAK_SAVE_HEADER_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        if (off + TAK_SAVE_SECTION_BYTES > len) {
            reader_free(r);
            set_err(err, err_cap, "This save is incomplete.");
            return NULL;
        }
        SectionEntry *s = &r->sections[i];
        const uint8_t *h = r->bytes + off;
        s->id = tak_get_u32(h + 0);
        s->version = tak_get_u16(h + 4);
        s->flags = tak_get_u16(h + 6);
        s->stored_bytes = tak_get_u32(h + 8);
        s->plain_bytes = tak_get_u32(h + 12);
        off += TAK_SAVE_SECTION_BYTES;
        if ((uint64_t)off + s->stored_bytes > (uint64_t)len) {
            char code[5];
            Save_IdToText(s->id, code);
            reader_free(r);
            set_err(err, err_cap,
                    "This save is incomplete: section %s runs past the "
                    "end of the file.", code);
            return NULL;
        }
        s->offset = off;
        off += s->stored_bytes;
        r->section_count++;
    }
    return r;
}

TAK_SaveReader *Save_OpenFile(const char *path, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) {
        set_err(err, err_cap, "There is no saved game at %s.",
                path ? path : "(null)");
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        set_err(err, err_cap, "Could not read %s.", path);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        set_err(err, err_cap, "Could not read %s.", path);
        return NULL;
    }
    rewind(fp);
    uint8_t *bytes = (uint8_t *)tak_malloc((size_t)size ? (size_t)size : 1);
    if (!bytes) {
        fclose(fp);
        set_err(err, err_cap, "Out of memory reading %s.", path);
        return NULL;
    }
    size_t got = fread(bytes, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        tak_free(bytes);
        set_err(err, err_cap, "Could not read %s.", path);
        return NULL;
    }
    TAK_SaveReader *r = Save_OpenMemory(bytes, got, err, err_cap);
    tak_free(bytes);
    return r;
}

const TAK_SaveHeader *Save_ReaderHeader(const TAK_SaveReader *r) {
    return r ? &r->hdr : NULL;
}

int Save_SectionCount(const TAK_SaveReader *r) {
    return r ? r->section_count : 0;
}

uint32_t Save_SectionIdAt(const TAK_SaveReader *r, int index) {
    if (!r || index < 0 || index >= r->section_count) return 0;
    return r->sections[index].id;
}

int Save_DeclareKnown(TAK_SaveReader *r, uint32_t id, uint16_t max_version) {
    if (!r) return -1;
    if (r->known_count == r->known_cap) {
        int cap = r->known_cap ? r->known_cap * 2 : 16;
        KnownEntry *k = (KnownEntry *)tak_realloc(r->known, sizeof(KnownEntry) * (size_t)cap);
        if (!k) return -1;
        r->known = k;
        r->known_cap = cap;
    }
    r->known[r->known_count].id = id;
    r->known[r->known_count].max_version = max_version;
    r->known_count++;
    return 0;
}

static const KnownEntry *find_known(const TAK_SaveReader *r, uint32_t id) {
    for (int i = 0; i < r->known_count; i++) {
        if (r->known[i].id == id) return &r->known[i];
    }
    return NULL;
}

int Save_Validate(TAK_SaveReader *r, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!r) return -1;
    for (int i = 0; i < r->section_count; i++) {
        const SectionEntry *s = &r->sections[i];
        const KnownEntry *k = find_known(r, s->id);
        int understood = k && s->version <= k->max_version;
        if (understood) continue;
        /* Unknown, or from a newer writer. Optional means step over
         * it, so a newer writer's extra section is invisible to an
         * older reader. Required means refuse and name the code. */
        if ((s->flags & TAK_SECT_F_REQUIRED) == 0) continue;
        char code[5];
        Save_IdToText(s->id, code);
        if (k) {
            set_err(err, err_cap,
                    "This save needs a newer version of the game: section "
                    "%s is version %u and this build reads up to %u.",
                    code, (unsigned)s->version, (unsigned)k->max_version);
        } else {
            set_err(err, err_cap,
                    "This save needs a newer version of the game: it "
                    "carries a section this build does not know (%s).",
                    code);
        }
        return -1;
    }
    return 0;
}

static SectionEntry *find_section(TAK_SaveReader *r, uint32_t id) {
    for (int i = 0; i < r->section_count; i++) {
        if (r->sections[i].id == id) return &r->sections[i];
    }
    return NULL;
}

const void *Save_Section(TAK_SaveReader *r, uint32_t id,
                         uint16_t *out_version, size_t *out_len) {
    if (out_version) *out_version = 0;
    if (out_len) *out_len = 0;
    if (!r) return NULL;
    SectionEntry *s = find_section(r, id);
    if (!s) return NULL;
    if (out_version) *out_version = s->version;

    if ((s->flags & TAK_SECT_F_DEFLATED) == 0) {
        if (s->plain_bytes != s->stored_bytes) return NULL;
        if (out_len) *out_len = s->stored_bytes;
        return r->bytes + s->offset;
    }
    if (!s->plain) {
        if (s->plain_bytes == 0) return NULL;
        uint8_t *out = (uint8_t *)tak_malloc(s->plain_bytes);
        if (!out) return NULL;
        mz_ulong got = s->plain_bytes;
        if (mz_uncompress(out, &got, r->bytes + s->offset,
                          (mz_ulong)s->stored_bytes) != MZ_OK ||
            got != s->plain_bytes) {
            tak_free(out);
            return NULL;
        }
        s->plain = out;
    }
    if (out_len) *out_len = s->plain_bytes;
    return s->plain;
}

const void *Save_Records(TAK_SaveReader *r, uint32_t id,
                         uint16_t *out_version, uint32_t *out_count,
                         uint16_t *out_stored_bytes) {
    if (out_count) *out_count = 0;
    if (out_stored_bytes) *out_stored_bytes = 0;
    size_t len = 0;
    const uint8_t *p = (const uint8_t *)Save_Section(r, id, out_version, &len);
    if (!p || len < 6) return NULL;
    uint32_t count = tak_get_u32(p + 0);
    uint16_t width = tak_get_u16(p + 4);
    if ((uint64_t)count * (uint64_t)width + 6u != (uint64_t)len) return NULL;
    if (out_count) *out_count = count;
    if (out_stored_bytes) *out_stored_bytes = width;
    return p + 6;
}

void Save_Close(TAK_SaveReader *r) { reader_free(r); }

/* ── string table ─────────────────────────────────────────────────── */

struct TAK_StringTable {
    char **items;
    int    count;
    int    cap;
};

TAK_StringTable *StringTable_New(void) {
    TAK_StringTable *t = (TAK_StringTable *)tak_malloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    return t;
}

void StringTable_Free(TAK_StringTable *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) tak_free(t->items[i]);
    tak_free(t->items);
    tak_free(t);
}

int StringTable_Count(const TAK_StringTable *t) { return t ? t->count : 0; }

const char *StringTable_Get(const TAK_StringTable *t, int index) {
    if (!t || index < 0 || index >= t->count) return NULL;
    return t->items[index];
}

int StringTable_Intern(TAK_StringTable *t, const char *s) {
    if (!t || !s) return -1;
    size_t n = strlen(s);
    if (n > 0xffffu) return -1;
    for (int i = 0; i < t->count; i++) {
        if (strcmp(t->items[i], s) == 0) return i;
    }
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 32;
        char **items = (char **)tak_realloc(t->items, sizeof(char *) * (size_t)cap);
        if (!items) return -1;
        t->items = items;
        t->cap = cap;
    }
    char *copy = (char *)tak_malloc(n + 1);
    if (!copy) return -1;
    memcpy(copy, s, n + 1);
    t->items[t->count] = copy;
    return t->count++;
}

uint8_t *StringTable_Serialize(const TAK_StringTable *t, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!t) return NULL;
    ByteBuf b = { 0 };
    uint8_t head[4];
    tak_put_u32(head, (uint32_t)t->count);
    if (buf_append(&b, head, sizeof(head)) != 0) goto fail;
    for (int i = 0; i < t->count; i++) {
        size_t n = strlen(t->items[i]);
        uint8_t lenbuf[2];
        tak_put_u16(lenbuf, (uint16_t)n);
        if (buf_append(&b, lenbuf, sizeof(lenbuf)) != 0) goto fail;
        if (buf_append(&b, t->items[i], n) != 0) goto fail;
    }
    if (out_len) *out_len = b.len;
    return b.data;
fail:
    tak_free(b.data);
    return NULL;
}

TAK_StringTable *StringTable_Parse(const void *bytes, size_t len) {
    if (!bytes || len < 4) return NULL;
    const uint8_t *p = (const uint8_t *)bytes;
    uint32_t count = tak_get_u32(p);
    size_t off = 4;
    TAK_StringTable *t = StringTable_New();
    if (!t) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 2 > len) goto fail;
        uint16_t n = tak_get_u16(p + off);
        off += 2;
        if (off + n > len) goto fail;
        char *copy = (char *)tak_malloc((size_t)n + 1);
        if (!copy) goto fail;
        memcpy(copy, p + off, n);
        copy[n] = '\0';
        off += n;
        if (t->count == t->cap) {
            int cap = t->cap ? t->cap * 2 : 32;
            char **items = (char **)tak_realloc(t->items, sizeof(char *) * (size_t)cap);
            if (!items) { tak_free(copy); goto fail; }
            t->items = items;
            t->cap = cap;
        }
        t->items[t->count++] = copy;
    }
    if (off != len) goto fail;
    return t;
fail:
    StringTable_Free(t);
    return NULL;
}
