#ifndef TAK_SAVEFILE_H
#define TAK_SAVEFILE_H

#include <stddef.h>
#include <stdint.h>

#include "tak_sha256.h"

/* The .oksave container.
 *
 * A save is a magic, a fixed size header, a digest and a list of typed
 * skippable sections. Nothing in it is a struct handed to a write
 * call, so a file written by the 32 bit Windows build or the wasm32
 * browser build opens unchanged on a 64 bit macOS or Linux build.
 *
 * What the container knows about is bytes. It has no idea what a unit
 * or a map is. The state serialisers hand it finished payloads and ask
 * for them back by four character code. */

#define TAK_SAVE_MAGIC_BYTES        8
#define TAK_SAVE_CONTAINER_VERSION  1u
#define TAK_SAVE_HEADER_BYTES       240u
#define TAK_SAVE_SECTION_BYTES      16u
#define TAK_SAVE_BUILD_MAX          64
#define TAK_SAVE_EXTENSION          ".oksave"

/* Header flags. */
#define TAK_SAVE_F_DEFLATE   0x1u   /* some section may be deflated */
#define TAK_SAVE_F_CAMPAIGN  0x2u

/* Section flags. */
#define TAK_SECT_F_REQUIRED  0x1u   /* an unknown one refuses the file */
#define TAK_SECT_F_DEFLATED  0x2u
#define TAK_SECT_F_RECORDS   0x4u   /* payload is a record array */

/* determinism_class. A save does not cross the line: when the fixed
 * point conversion lands the class becomes 1, and a class 0 file is
 * refused by name rather than reinterpreted. */
#define TAK_DETERMINISM_FLOAT  0
#define TAK_DETERMINISM_FIXED  1

/* float_form. */
#define TAK_FLOAT_FORM_BINARY32  0
#define TAK_FLOAT_FORM_FIXED     1

/* save_kind. */
#define TAK_SAVE_KIND_SKIRMISH        1
#define TAK_SAVE_KIND_CAMPAIGN_BATTLE 2
#define TAK_SAVE_KIND_CAMPAIGN_BETWEEN 3

#define TAK_SAVE_ID(a, b, c, d) \
    (((uint32_t)(uint8_t)(a)) | ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

#define TAK_SECT_SUMM  TAK_SAVE_ID('S', 'U', 'M', 'M')
#define TAK_SECT_STRT  TAK_SAVE_ID('S', 'T', 'R', 'T')

/* Room for the longest refusal plus the names it carries. */
#define TAK_SAVE_ERR_MAX 256

/* The header, in memory. Never written or read as a struct. */
typedef struct TAK_SaveHeader {
    uint32_t container_version;
    uint32_t schema_version;
    uint32_t flags;
    uint32_t sim_tick;
    uint32_t sim_state_hash;
    uint32_t rng_ai;
    uint8_t  determinism_class;
    uint8_t  tick_hz;
    uint8_t  save_kind;
    uint8_t  float_form;
    uint32_t unit_stable_id_next;
    uint32_t unit_slot_count;
    uint64_t saved_at_utc;
    char     engine_build[TAK_SAVE_BUILD_MAX];
    uint8_t  map_fingerprint[TAK_SHA256_BYTES];
    /* Filled by the reader, ignored by the writer. */
    uint32_t section_count;
    uint64_t file_bytes;
    uint8_t  digest[TAK_SHA256_BYTES];
} TAK_SaveHeader;

/* Fill in the fields this build owns: container version, tick rate,
 * determinism class, float form and the build stamp. The caller sets
 * the rest. */
void Save_HeaderInit(TAK_SaveHeader *hdr);

/* ── Writing ──────────────────────────────────────────────────────── */

typedef struct TAK_SaveWriter TAK_SaveWriter;

/* Start a save. The header is copied. NULL on allocation failure. */
TAK_SaveWriter *Save_BeginWrite(const TAK_SaveHeader *hdr);

/* Append one section, in file order. `flags` carries TAK_SECT_F_*
 * without TAK_SECT_F_DEFLATED: the writer decides that per section and
 * keeps the plain form when deflating does not pay. Returns 0, or -1
 * on allocation failure or a payload over 4 GiB. */
int Save_AddSection(TAK_SaveWriter *w, uint32_t id, uint16_t version,
                    uint16_t flags, const void *payload, size_t len);

/* Same, for a record array. Writes `u32 count` then `u16 fixed_bytes`
 * ahead of `count * fixed_bytes` bytes of records and sets the record
 * flag. */
int Save_AddRecords(TAK_SaveWriter *w, uint32_t id, uint16_t version,
                    uint16_t flags, uint32_t count, uint16_t fixed_bytes,
                    const void *records);

/* Seal the file: stamp the length, take the digest over the whole of
 * it with the digest field read as zero, and hand back the bytes. The
 * writer keeps ownership, so call this once and copy what you need. */
const uint8_t *Save_FinishToMemory(TAK_SaveWriter *w, size_t *out_len);

/* Seal and write. The bytes land in <path>.tmp which is then renamed
 * over <path>, so a crash mid write leaves the previous save intact.
 * Returns 0, or -1 with a reason in `err`. */
int Save_FinishToFile(TAK_SaveWriter *w, const char *path,
                      char *err, size_t err_cap);

void Save_EndWrite(TAK_SaveWriter *w);

/* ── Reading ──────────────────────────────────────────────────────── */

typedef struct TAK_SaveReader TAK_SaveReader;

/* Open and validate: magic, container version, declared length against
 * the real one, digest, then the section walk. Returns NULL with a
 * plain reason in `err` on any refusal. The bytes are copied, so the
 * caller's buffer does not have to outlive the reader. */
TAK_SaveReader *Save_OpenMemory(const void *bytes, size_t len,
                                char *err, size_t err_cap);
TAK_SaveReader *Save_OpenFile(const char *path, char *err, size_t err_cap);

const TAK_SaveHeader *Save_ReaderHeader(const TAK_SaveReader *r);

/* How many sections the file carries, and the id of one of them. */
int      Save_SectionCount(const TAK_SaveReader *r);
uint32_t Save_SectionIdAt(const TAK_SaveReader *r, int index);

/* Payload of one section, inflated on first ask and cached. NULL when
 * the file has no such section. */
const void *Save_Section(TAK_SaveReader *r, uint32_t id,
                         uint16_t *out_version, size_t *out_len);

/* A record array section. `out_stored_bytes` is the per record width
 * the file used, which may differ from this reader's constant: read
 * the stored prefix and zero the tail when it is shorter, read our
 * prefix and step over the rest when it is longer. That tolerance is
 * what keeps an old save loading after a field is appended, and it
 * puts an obligation on the writer. Within one section version fields
 * are append only and zero must be a safe default for every appended
 * field. A field whose zero is not safe forces a version bump. */
const void *Save_Records(TAK_SaveReader *r, uint32_t id,
                         uint16_t *out_version, uint32_t *out_count,
                         uint16_t *out_stored_bytes);

/* Tell the reader which ids it understands and up to which version.
 * Call before Save_Validate. An unknown required section, or a known
 * one from a newer writer, is a refusal that names the four character
 * code. */
int  Save_DeclareKnown(TAK_SaveReader *r, uint32_t id, uint16_t max_version);

/* Apply the walk rules against the declared set. 0 when the file is
 * loadable, -1 with a reason in `err`. */
int  Save_Validate(TAK_SaveReader *r, char *err, size_t err_cap);

void Save_Close(TAK_SaveReader *r);

/* ── The shared string table ──────────────────────────────────────── */

/* Every name in a save is an index into one table, so a unit
 * definition name is stored once however many units use it. */

typedef struct TAK_StringTable TAK_StringTable;

TAK_StringTable *StringTable_New(void);
void             StringTable_Free(TAK_StringTable *t);

/* Index of `s`, adding it if new. Repeats return the same index.
 * Returns -1 on allocation failure or a string over 65535 bytes. */
int  StringTable_Intern(TAK_StringTable *t, const char *s);
int  StringTable_Count(const TAK_StringTable *t);
const char *StringTable_Get(const TAK_StringTable *t, int index);

/* Serialise to the STRT payload: u32 count, then u16 len and len bytes
 * per entry with no terminator. Caller frees with tak_free. */
uint8_t *StringTable_Serialize(const TAK_StringTable *t, size_t *out_len);

/* Parse a STRT payload. NULL when the payload is malformed. */
TAK_StringTable *StringTable_Parse(const void *bytes, size_t len);

/* Write a four character code into `out[5]` for a message. */
void Save_IdToText(uint32_t id, char out[5]);

#endif /* TAK_SAVEFILE_H */
