/* The .oksave container: round trip, and every refusal driven by hand
 * corrupting one byte.
 *
 * Data free, so CI runs it. */

#include "tak_bytes.h"
#include "tak_memory.h"
#include "tak_savefile.h"
#include "tak_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(x) do { \
    if (!(x)) { \
        fprintf(stderr, "ASSERT failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ(exp, got) do { \
    long long _e = (long long)(exp); \
    long long _g = (long long)(got); \
    if (_e != _g) { \
        fprintf(stderr, "ASSERT_EQ failed at %s:%d: expected %lld got %lld\n", \
                __FILE__, __LINE__, _e, _g); \
        return 1; \
    } \
} while (0)

#define ASSERT_REFUSED(r, err) do { \
    if ((r) != NULL) { \
        fprintf(stderr, "expected a refusal at %s:%d but the file opened\n", \
                __FILE__, __LINE__); \
        Save_Close(r); \
        return 1; \
    } \
    if ((err)[0] == '\0') { \
        fprintf(stderr, "refused at %s:%d with no reason given\n", \
                __FILE__, __LINE__); \
        return 1; \
    } \
    printf("    refusal: %s\n", (err)); \
} while (0)

#define SECT_TEST TAK_SAVE_ID('T', 'E', 'S', 'T')
#define SECT_NEWR TAK_SAVE_ID('N', 'E', 'W', 'R')   /* newer, required */
#define SECT_NEWO TAK_SAVE_ID('N', 'E', 'W', 'O')   /* newer, optional */

static const char k_summary[] = "Kings March, 8 minutes, 2 players";

/* Header offsets the corruption cases reach into. Spelled out here
 * rather than shared with the writer, so a silent layout change in one
 * of them fails the test instead of hiding. */
#define H_MAGIC      0
#define H_CONTAINER  8
#define H_FILE_BYTES 16
#define H_DIGEST     32
#define H_SECTIONS   64

/* Recompute the digest after a deliberate edit, so a case can test the
 * section walk rather than tripping the damage check first. */
static void reseal(uint8_t *bytes, size_t len) {
    static const uint8_t zeros[TAK_SHA256_BYTES] = { 0 };
    TAK_Sha256 ctx;
    TAK_Sha256_Init(&ctx);
    TAK_Sha256_Update(&ctx, bytes, H_DIGEST);
    TAK_Sha256_Update(&ctx, zeros, TAK_SHA256_BYTES);
    TAK_Sha256_Update(&ctx, bytes + H_DIGEST + TAK_SHA256_BYTES,
                      len - H_DIGEST - TAK_SHA256_BYTES);
    TAK_Sha256_Final(&ctx, bytes + H_DIGEST);
}

static void fill_header(TAK_SaveHeader *h) {
    Save_HeaderInit(h);
    h->schema_version = 7;
    h->sim_tick = 0x00112233u;
    h->sim_state_hash = 0xdeadbeefu;
    h->rng_ai = 0x2a5f19c7u;
    h->unit_stable_id_next = 4321;
    h->unit_slot_count = 60;
    h->saved_at_utc = 1757548800ull;
    h->save_kind = TAK_SAVE_KIND_SKIRMISH;
    for (int i = 0; i < TAK_SHA256_BYTES; i++) h->map_fingerprint[i] = (uint8_t)(i * 3);
}

/* One save holding the summary and the string table, which is what
 * this container has to carry before anything else can use it. */
static uint8_t *build_save(size_t *out_len) {
    TAK_SaveHeader h;
    fill_header(&h);

    TAK_SaveWriter *w = Save_BeginWrite(&h);
    if (!w) return NULL;

    if (Save_AddSection(w, TAK_SECT_SUMM, 1, 0,
                        k_summary, sizeof(k_summary)) != 0) {
        Save_EndWrite(w);
        return NULL;
    }

    TAK_StringTable *t = StringTable_New();
    StringTable_Intern(t, "araking");
    StringTable_Intern(t, "vertrunner");
    StringTable_Intern(t, "araking");      /* the dedup */
    size_t strt_len = 0;
    uint8_t *strt = StringTable_Serialize(t, &strt_len);
    StringTable_Free(t);
    if (!strt) {
        Save_EndWrite(w);
        return NULL;
    }
    int rc = Save_AddSection(w, TAK_SECT_STRT, 1, TAK_SECT_F_REQUIRED,
                             strt, strt_len);
    tak_free(strt);
    if (rc != 0) {
        Save_EndWrite(w);
        return NULL;
    }

    size_t len = 0;
    const uint8_t *bytes = Save_FinishToMemory(w, &len);
    uint8_t *copy = NULL;
    if (bytes) {
        copy = (uint8_t *)tak_malloc(len);
        if (copy) memcpy(copy, bytes, len);
    }
    Save_EndWrite(w);
    if (out_len) *out_len = copy ? len : 0;
    return copy;
}

static int test_round_trip(void) {
    size_t len = 0;
    uint8_t *bytes = build_save(&len);
    ASSERT(bytes != NULL);
    ASSERT(len > TAK_SAVE_HEADER_BYTES);

    char err[TAK_SAVE_ERR_MAX];
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    if (!r) { fprintf(stderr, "refused a good save: %s\n", err); tak_free(bytes); return 1; }

    const TAK_SaveHeader *h = Save_ReaderHeader(r);
    ASSERT_EQ(TAK_SAVE_CONTAINER_VERSION, h->container_version);
    ASSERT_EQ(7, h->schema_version);
    ASSERT_EQ(0x00112233u, h->sim_tick);
    ASSERT_EQ(0xdeadbeefu, h->sim_state_hash);
    ASSERT_EQ(0x2a5f19c7u, h->rng_ai);
    ASSERT_EQ(4321, h->unit_stable_id_next);
    ASSERT_EQ(60, h->unit_slot_count);
    ASSERT_EQ(1757548800ull, h->saved_at_utc);
    ASSERT_EQ(60, h->tick_hz);
    ASSERT_EQ(TAK_DETERMINISM_FLOAT, h->determinism_class);
    ASSERT_EQ(TAK_FLOAT_FORM_BINARY32, h->float_form);
    ASSERT_EQ(len, h->file_bytes);
    ASSERT_EQ(2, h->section_count);
    ASSERT(h->engine_build[0] != '\0');
    for (int i = 0; i < TAK_SHA256_BYTES; i++) ASSERT_EQ(i * 3, h->map_fingerprint[i]);

    uint16_t version = 0;
    size_t plen = 0;
    const char *summ = (const char *)Save_Section(r, TAK_SECT_SUMM, &version, &plen);
    ASSERT(summ != NULL);
    ASSERT_EQ(1, version);
    ASSERT_EQ(sizeof(k_summary), plen);
    ASSERT(memcmp(summ, k_summary, sizeof(k_summary)) == 0);

    const void *strt = Save_Section(r, TAK_SECT_STRT, &version, &plen);
    ASSERT(strt != NULL);
    TAK_StringTable *t = StringTable_Parse(strt, plen);
    ASSERT(t != NULL);
    ASSERT_EQ(2, StringTable_Count(t));
    ASSERT(strcmp(StringTable_Get(t, 0), "araking") == 0);
    ASSERT(strcmp(StringTable_Get(t, 1), "vertrunner") == 0);
    StringTable_Free(t);

    /* A section the file does not carry is absent, not an error. */
    ASSERT(Save_Section(r, SECT_TEST, NULL, NULL) == NULL);

    Save_DeclareKnown(r, TAK_SECT_SUMM, 1);
    Save_DeclareKnown(r, TAK_SECT_STRT, 1);
    ASSERT_EQ(0, Save_Validate(r, err, sizeof(err)));

    Save_Close(r);
    tak_free(bytes);
    return 0;
}

static int test_string_table(void) {
    TAK_StringTable *t = StringTable_New();
    ASSERT(t != NULL);
    ASSERT_EQ(0, StringTable_Intern(t, ""));
    ASSERT_EQ(1, StringTable_Intern(t, "araking"));
    ASSERT_EQ(1, StringTable_Intern(t, "araking"));
    ASSERT_EQ(2, StringTable_Intern(t, "ARAKING"));   /* names are exact */
    ASSERT_EQ(3, StringTable_Count(t));

    size_t len = 0;
    uint8_t *bytes = StringTable_Serialize(t, &len);
    ASSERT(bytes != NULL);
    /* u32 count, then u16 len plus the bytes, with no terminator. */
    ASSERT_EQ(4 + 2 + 0 + 2 + 7 + 2 + 7, len);
    StringTable_Free(t);

    TAK_StringTable *back = StringTable_Parse(bytes, len);
    ASSERT(back != NULL);
    ASSERT_EQ(3, StringTable_Count(back));
    ASSERT(strcmp(StringTable_Get(back, 0), "") == 0);
    ASSERT(strcmp(StringTable_Get(back, 2), "ARAKING") == 0);
    ASSERT(StringTable_Get(back, 3) == NULL);
    StringTable_Free(back);

    /* A payload one byte short is malformed, not a shorter table. */
    ASSERT(StringTable_Parse(bytes, len - 1) == NULL);
    tak_free(bytes);
    return 0;
}

static int test_refuse_bad_magic(void) {
    size_t len = 0;
    uint8_t *bytes = build_save(&len);
    ASSERT(bytes != NULL);
    bytes[H_MAGIC + 2] ^= 0xffu;
    reseal(bytes, len);              /* intact, just not one of ours */
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT_REFUSED(r, err);
    tak_free(bytes);
    return 0;
}

static int test_refuse_newer_container(void) {
    size_t len = 0;
    uint8_t *bytes = build_save(&len);
    ASSERT(bytes != NULL);
    tak_put_u32(bytes + H_CONTAINER, TAK_SAVE_CONTAINER_VERSION + 1u);
    reseal(bytes, len);
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT_REFUSED(r, err);
    ASSERT(strstr(err, "newer") != NULL);
    tak_free(bytes);
    return 0;
}

static int test_refuse_truncated(void) {
    size_t len = 0;
    uint8_t *bytes = build_save(&len);
    ASSERT(bytes != NULL);

    /* The file is cut short: the header still says the full length. */
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveReader *r = Save_OpenMemory(bytes, len - 1, err, sizeof(err));
    ASSERT_REFUSED(r, err);

    /* A header shorter than the fixed header is refused too. */
    err[0] = '\0';
    r = Save_OpenMemory(bytes, 16, err, sizeof(err));
    ASSERT_REFUSED(r, err);

    /* A section header that claims more payload than the file holds.
     * Resealed, so this is the walk refusing rather than the digest. */
    size_t sect0 = TAK_SAVE_HEADER_BYTES;
    tak_put_u32(bytes + sect0 + 8, 0x0fffffffu);
    reseal(bytes, len);
    err[0] = '\0';
    r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT_REFUSED(r, err);
    ASSERT(strstr(err, "SUMM") != NULL);

    tak_free(bytes);
    return 0;
}

static int test_refuse_bad_digest(void) {
    size_t len = 0;
    uint8_t *bytes = build_save(&len);
    ASSERT(bytes != NULL);
    /* One byte of payload flipped, with the digest left alone. */
    bytes[len - 1] ^= 0x01u;
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT_REFUSED(r, err);
    ASSERT(strstr(err, "damaged") != NULL);
    tak_free(bytes);
    return 0;
}

static int test_refuse_lying_section_count(void) {
    size_t len = 0;
    uint8_t *bytes = build_save(&len);
    ASSERT(bytes != NULL);
    tak_put_u32(bytes + H_SECTIONS, 100000u);
    reseal(bytes, len);
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT_REFUSED(r, err);
    tak_free(bytes);
    return 0;
}

/* Rules 2 and 3 of the walk: an id this reader does not know is
 * stepped over when it is optional and refuses the file by name when
 * it is required. Rule 4 applies the same two answers to a known id
 * from a newer writer. */
static int test_unknown_sections(void) {
    TAK_SaveHeader h;
    fill_header(&h);
    TAK_SaveWriter *w = Save_BeginWrite(&h);
    ASSERT(w != NULL);
    ASSERT_EQ(0, Save_AddSection(w, TAK_SECT_SUMM, 1, 0, k_summary, sizeof(k_summary)));
    ASSERT_EQ(0, Save_AddSection(w, SECT_NEWO, 1, 0, "optional", 8));
    size_t len = 0;
    const uint8_t *sealed = Save_FinishToMemory(w, &len);
    ASSERT(sealed != NULL);
    uint8_t *bytes = (uint8_t *)tak_malloc(len);
    ASSERT(bytes != NULL);
    memcpy(bytes, sealed, len);
    Save_EndWrite(w);

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT(r != NULL);
    Save_DeclareKnown(r, TAK_SECT_SUMM, 1);
    ASSERT_EQ(0, Save_Validate(r, err, sizeof(err)));
    /* Stepped over, and the section after it still reads. */
    ASSERT(Save_Section(r, TAK_SECT_SUMM, NULL, NULL) != NULL);
    Save_Close(r);
    tak_free(bytes);

    /* The same section marked required. */
    w = Save_BeginWrite(&h);
    ASSERT(w != NULL);
    ASSERT_EQ(0, Save_AddSection(w, TAK_SECT_SUMM, 1, 0, k_summary, sizeof(k_summary)));
    ASSERT_EQ(0, Save_AddSection(w, SECT_NEWR, 1, TAK_SECT_F_REQUIRED, "needed", 6));
    sealed = Save_FinishToMemory(w, &len);
    ASSERT(sealed != NULL);
    bytes = (uint8_t *)tak_malloc(len);
    ASSERT(bytes != NULL);
    memcpy(bytes, sealed, len);
    Save_EndWrite(w);

    err[0] = '\0';
    r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT(r != NULL);
    Save_DeclareKnown(r, TAK_SECT_SUMM, 1);
    ASSERT_EQ(-1, Save_Validate(r, err, sizeof(err)));
    ASSERT(strstr(err, "NEWR") != NULL);
    printf("    refusal: %s\n", err);
    Save_Close(r);

    /* Rule 4: a known id at a version past what this reader handles
     * answers exactly as an unknown one would. */
    err[0] = '\0';
    r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT(r != NULL);
    Save_DeclareKnown(r, TAK_SECT_SUMM, 1);
    Save_DeclareKnown(r, SECT_NEWR, 0);
    ASSERT_EQ(-1, Save_Validate(r, err, sizeof(err)));
    ASSERT(strstr(err, "NEWR") != NULL);
    printf("    refusal: %s\n", err);
    Save_Close(r);
    tak_free(bytes);
    return 0;
}

/* Rule 6: a record array whose stored width differs from this
 * reader's. The reader takes the overlap and zeroes or steps over the
 * rest, which is what stops a field addition invalidating every save
 * already on disk. */
typedef struct TestRecordV1 { int32_t a; int32_t b; } TestRecordV1;
typedef struct TestRecordV2 { int32_t a; int32_t b; int32_t c; } TestRecordV2;

#define REC_V1_BYTES 8u
#define REC_V2_BYTES 12u

static uint8_t *records_save(uint16_t width, uint32_t count, size_t *out_len) {
    TAK_SaveHeader h;
    fill_header(&h);
    TAK_SaveWriter *w = Save_BeginWrite(&h);
    if (!w) return NULL;
    uint8_t *payload = (uint8_t *)tak_malloc((size_t)width * count);
    if (!payload) { Save_EndWrite(w); return NULL; }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *p = payload + (size_t)i * width;
        tak_put_i32(p + 0, (int32_t)(100 + i));
        tak_put_i32(p + 4, (int32_t)(200 + i));
        if (width >= REC_V2_BYTES) tak_put_i32(p + 8, (int32_t)(300 + i));
    }
    int rc = Save_AddRecords(w, SECT_TEST, 1, TAK_SECT_F_REQUIRED,
                             count, width, payload);
    tak_free(payload);
    if (rc != 0) { Save_EndWrite(w); return NULL; }
    size_t len = 0;
    const uint8_t *sealed = Save_FinishToMemory(w, &len);
    uint8_t *copy = NULL;
    if (sealed) {
        copy = (uint8_t *)tak_malloc(len);
        if (copy) memcpy(copy, sealed, len);
    }
    Save_EndWrite(w);
    if (out_len) *out_len = copy ? len : 0;
    return copy;
}

static int test_records_short_and_long(void) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };

    /* Stored narrower than this reader expects: read the prefix, zero
     * the tail. */
    size_t len = 0;
    uint8_t *bytes = records_save(REC_V1_BYTES, 3, &len);
    ASSERT(bytes != NULL);
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT(r != NULL);
    uint32_t count = 0;
    uint16_t stored = 0;
    const uint8_t *recs = (const uint8_t *)Save_Records(r, SECT_TEST, NULL,
                                                        &count, &stored);
    ASSERT(recs != NULL);
    ASSERT_EQ(3, count);
    ASSERT_EQ(REC_V1_BYTES, stored);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t field[REC_V2_BYTES];
        memset(field, 0, sizeof(field));
        uint16_t take = stored < REC_V2_BYTES ? stored : (uint16_t)REC_V2_BYTES;
        memcpy(field, recs + (size_t)i * stored, take);
        ASSERT_EQ(100 + i, tak_get_i32(field + 0));
        ASSERT_EQ(200 + i, tak_get_i32(field + 4));
        ASSERT_EQ(0, tak_get_i32(field + 8));   /* the zeroed tail */
    }
    Save_Close(r);
    tak_free(bytes);

    /* Stored wider: read our prefix, step over the remainder. */
    bytes = records_save(REC_V2_BYTES, 3, &len);
    ASSERT(bytes != NULL);
    r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT(r != NULL);
    recs = (const uint8_t *)Save_Records(r, SECT_TEST, NULL, &count, &stored);
    ASSERT(recs != NULL);
    ASSERT_EQ(3, count);
    ASSERT_EQ(REC_V2_BYTES, stored);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t field[REC_V1_BYTES];
        memcpy(field, recs + (size_t)i * stored, REC_V1_BYTES);
        ASSERT_EQ(100 + i, tak_get_i32(field + 0));
        ASSERT_EQ(200 + i, tak_get_i32(field + 4));
    }
    Save_Close(r);
    tak_free(bytes);

    return 0;
}

/* Compression is per section and SUMM is never compressed, so the load
 * list costs one short read per file. */
static int test_per_section_deflate(void) {
    TAK_SaveHeader h;
    fill_header(&h);
    enum { BIG = 8192 };
    uint8_t *big = (uint8_t *)tak_malloc(BIG);
    ASSERT(big != NULL);
    memset(big, 'a', BIG);

    TAK_SaveWriter *w = Save_BeginWrite(&h);
    ASSERT(w != NULL);
    ASSERT_EQ(0, Save_AddSection(w, SECT_TEST, 1, TAK_SECT_F_REQUIRED, big, BIG));
    size_t len = 0;
    const uint8_t *sealed = Save_FinishToMemory(w, &len);
    ASSERT(sealed != NULL);
    ASSERT(len < BIG / 4);                       /* it really deflated */
    uint8_t *bytes = (uint8_t *)tak_malloc(len);
    ASSERT(bytes != NULL);
    memcpy(bytes, sealed, len);
    Save_EndWrite(w);

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveReader *r = Save_OpenMemory(bytes, len, err, sizeof(err));
    ASSERT(r != NULL);
    size_t plen = 0;
    const uint8_t *back = (const uint8_t *)Save_Section(r, SECT_TEST, NULL, &plen);
    ASSERT(back != NULL);
    ASSERT_EQ(BIG, plen);
    ASSERT(memcmp(back, big, BIG) == 0);
    Save_Close(r);
    tak_free(bytes);

    /* The same payload under SUMM stays plain. */
    w = Save_BeginWrite(&h);
    ASSERT(w != NULL);
    ASSERT_EQ(0, Save_AddSection(w, TAK_SECT_SUMM, 1, 0, big, BIG));
    sealed = Save_FinishToMemory(w, &len);
    ASSERT(sealed != NULL);
    ASSERT_EQ(TAK_SAVE_HEADER_BYTES + TAK_SAVE_SECTION_BYTES + BIG, len);
    Save_EndWrite(w);
    tak_free(big);
    return 0;
}

/* The file is written to a temporary and renamed over, so an
 * interrupted write cannot leave a half file where the old one was. */
static int test_file_round_trip(void) {
    const char *path = "test_savefile_scratch.oksave";
    TAK_SaveHeader h;
    fill_header(&h);
    TAK_SaveWriter *w = Save_BeginWrite(&h);
    ASSERT(w != NULL);
    ASSERT_EQ(0, Save_AddSection(w, TAK_SECT_SUMM, 1, 0, k_summary, sizeof(k_summary)));
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    int rc = Save_FinishToFile(w, path, err, sizeof(err));
    if (rc != 0) fprintf(stderr, "write failed: %s\n", err);
    ASSERT_EQ(0, rc);
    Save_EndWrite(w);

    TAK_SaveReader *r = Save_OpenFile(path, err, sizeof(err));
    if (!r) fprintf(stderr, "read back failed: %s\n", err);
    ASSERT(r != NULL);
    size_t plen = 0;
    const char *summ = (const char *)Save_Section(r, TAK_SECT_SUMM, NULL, &plen);
    ASSERT(summ != NULL);
    ASSERT(memcmp(summ, k_summary, sizeof(k_summary)) == 0);
    Save_Close(r);

    /* Writing again over the same name replaces it. */
    h.sim_tick = 999;
    w = Save_BeginWrite(&h);
    ASSERT(w != NULL);
    ASSERT_EQ(0, Save_AddSection(w, TAK_SECT_SUMM, 1, 0, "later", 6));
    ASSERT_EQ(0, Save_FinishToFile(w, path, err, sizeof(err)));
    Save_EndWrite(w);
    r = Save_OpenFile(path, err, sizeof(err));
    ASSERT(r != NULL);
    ASSERT_EQ(999, Save_ReaderHeader(r)->sim_tick);
    Save_Close(r);

    /* No temporary left behind. */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "rb");
    ASSERT(fp == NULL);

    remove(path);

    err[0] = '\0';
    r = Save_OpenFile("no_such_file.oksave", err, sizeof(err));
    ASSERT_REFUSED(r, err);
    return 0;
}

int main(void) {
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "round_trip",              test_round_trip },
        { "string_table",            test_string_table },
        { "refuse_bad_magic",        test_refuse_bad_magic },
        { "refuse_newer_container",  test_refuse_newer_container },
        { "refuse_truncated",        test_refuse_truncated },
        { "refuse_bad_digest",       test_refuse_bad_digest },
        { "refuse_lying_count",      test_refuse_lying_section_count },
        { "unknown_sections",        test_unknown_sections },
        { "records_short_and_long",  test_records_short_and_long },
        { "per_section_deflate",     test_per_section_deflate },
        { "file_round_trip",         test_file_round_trip },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("%-28s ", cases[i].name);
        fflush(stdout);
        int rc = cases[i].fn();
        printf("%s\n", rc == 0 ? "ok" : "FAILED");
        failed += rc;
    }
    if (failed) {
        fprintf(stderr, "test_savefile: %d case(s) failed\n", failed);
        return 1;
    }
    printf("test_savefile: all cases passed\n");
    return 0;
}
