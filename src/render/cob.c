/*
 * cob.c — COB script bundle parser.
 *
 * Format spec lives in tak_cob.h. This file is only the loader;
 * the VM that *runs* the bytecode lands in M2 (cob_vm.c).
 *
 * Owns: bytecode buffer (deep-copied from the file), script name and
 * piece name string arrays (each strdup'd), the script offset table
 * (deep-copied). The raw .cob file buffer is freed at the end of
 * Cob_Load — nothing in CobScript points into it afterward.
 */

#include "tak_cob.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Header layout (offsets in bytes from start of file). */
#define COB_OFF_VERSION        0x00
#define COB_OFF_NUM_SCRIPTS    0x04
#define COB_OFF_NUM_PIECES     0x08
#define COB_OFF_CODE_LEN_WORDS 0x0c
#define COB_OFF_NUM_STATIC     0x10
#define COB_OFF_RESERVED       0x14
#define COB_OFF_SCRIPT_OFFS    0x18
#define COB_OFF_SCRIPT_NAMES   0x1c
#define COB_OFF_PIECE_NAMES    0x20
#define COB_OFF_CODE           0x24
#define COB_OFF_STRING_BLOB_A  0x28
#define COB_OFF_STRING_BLOB_B  0x2c
#define COB_HEADER_MIN_SIZE    0x30

#define COB_VERSION_EXPECTED   6

/* Read a uint32 LE at an absolute file offset, with bounds check.
 * Returns 0 on success, -1 if offset is out of range. */
static int read_u32(const uint8_t *buf, uint32_t buf_size,
                    uint32_t off, uint32_t *out) {
    if ((uint64_t)off + 4 > buf_size) return -1;
    *out = *(const uint32_t *)(buf + off);
    return 0;
}

/* Copy a null-terminated string starting at `off` into a fresh
 * tak_malloc'd buffer. Caller frees. Returns NULL if the string runs
 * past buf_size or exceeds 64 bytes (defensive cap; piece/script
 * names in shipped TAK are all ≤17 chars). */
static char *strdup_at(const uint8_t *buf, uint32_t buf_size, uint32_t off) {
    if (off >= buf_size) return NULL;
    uint32_t end = off;
    const uint32_t cap = 64;
    while (end < buf_size && end - off < cap && buf[end] != 0) end++;
    if (end >= buf_size || buf[end] != 0) return NULL;
    uint32_t len = end - off;
    char *s = (char *)tak_malloc(len + 1);
    if (!s) return NULL;
    if (len > 0) memcpy(s, buf + off, len);
    s[len] = '\0';
    return s;
}

int Cob_Load(CobScript **out, const char *vfs_path) {
    if (!out || !vfs_path) return -1;
    *out = NULL;

    void *raw = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(vfs_path, &raw, &size) != 0 || !raw) {
        fprintf(stderr, "Cob_Load: failed to read %s\n", vfs_path);
        return -1;
    }
    const uint8_t *buf = (const uint8_t *)raw;

    if (size < COB_HEADER_MIN_SIZE) {
        fprintf(stderr, "Cob_Load: %s too small (%u bytes)\n", vfs_path, size);
        tak_free(raw);
        return -2;
    }

    uint32_t version, num_scripts, num_pieces, code_len_words, num_static;
    uint32_t off_script_offs, off_script_names, off_piece_names, off_code;
    if (read_u32(buf, size, COB_OFF_VERSION, &version) != 0 ||
        read_u32(buf, size, COB_OFF_NUM_SCRIPTS, &num_scripts) != 0 ||
        read_u32(buf, size, COB_OFF_NUM_PIECES, &num_pieces) != 0 ||
        read_u32(buf, size, COB_OFF_CODE_LEN_WORDS, &code_len_words) != 0 ||
        read_u32(buf, size, COB_OFF_NUM_STATIC, &num_static) != 0 ||
        read_u32(buf, size, COB_OFF_SCRIPT_OFFS, &off_script_offs) != 0 ||
        read_u32(buf, size, COB_OFF_SCRIPT_NAMES, &off_script_names) != 0 ||
        read_u32(buf, size, COB_OFF_PIECE_NAMES, &off_piece_names) != 0 ||
        read_u32(buf, size, COB_OFF_CODE, &off_code) != 0) {
        fprintf(stderr, "Cob_Load: header read failed for %s\n", vfs_path);
        tak_free(raw);
        return -2;
    }

    if (version != COB_VERSION_EXPECTED) {
        fprintf(stderr, "Cob_Load: %s has version %u, expected %u\n",
                vfs_path, version, COB_VERSION_EXPECTED);
        tak_free(raw);
        return -2;
    }

    /* Sanity bounds. The defensive caps protect us against a
     * corrupted file claiming to have millions of pieces. Real shipped
     * TAK .cobs have at most ~50 pieces and ~80 scripts. */
    if (num_scripts > 256 || num_pieces > 256 || num_static > 1024) {
        fprintf(stderr, "Cob_Load: %s has implausible counts "
                "(scripts=%u, pieces=%u, statics=%u)\n",
                vfs_path, num_scripts, num_pieces, num_static);
        tak_free(raw);
        return -2;
    }

    /* Code region bounds. */
    if ((uint64_t)off_code + (uint64_t)code_len_words * 4 > size) {
        fprintf(stderr, "Cob_Load: %s code region runs past EOF "
                "(off=%u, words=%u, size=%u)\n",
                vfs_path, off_code, code_len_words, size);
        tak_free(raw);
        return -2;
    }
    /* Script offsets table bounds. */
    if ((uint64_t)off_script_offs + (uint64_t)num_scripts * 4 > size) {
        fprintf(stderr, "Cob_Load: %s script offsets table OOB\n", vfs_path);
        tak_free(raw);
        return -2;
    }
    /* Script & piece name pointer tables bounds. */
    if ((uint64_t)off_script_names + (uint64_t)num_scripts * 4 > size ||
        (uint64_t)off_piece_names + (uint64_t)num_pieces * 4 > size) {
        fprintf(stderr, "Cob_Load: %s name pointer table OOB\n", vfs_path);
        tak_free(raw);
        return -2;
    }

    /* Allocate and populate the script struct. We deep-copy everything
     * so the raw file buffer can be freed before returning. */
    CobScript *s = (CobScript *)tak_malloc(sizeof(CobScript));
    if (!s) { tak_free(raw); return -1; }
    memset(s, 0, sizeof(*s));
    s->version         = version;
    s->num_scripts     = (uint16_t)num_scripts;
    s->num_pieces      = (uint16_t)num_pieces;
    s->num_code_words  = code_len_words;
    s->num_static_vars = num_static;

    /* Bytecode — deep copy. */
    if (code_len_words > 0) {
        s->code = (uint32_t *)tak_malloc(code_len_words * sizeof(uint32_t));
        if (!s->code) goto fail;
        memcpy(s->code, buf + off_code, code_len_words * sizeof(uint32_t));
    }

    /* Script entry-point byte offsets — deep copy. */
    if (num_scripts > 0) {
        s->script_offsets = (uint32_t *)tak_malloc(num_scripts * sizeof(uint32_t));
        if (!s->script_offsets) goto fail;
        memcpy(s->script_offsets, buf + off_script_offs,
               num_scripts * sizeof(uint32_t));

        /* Validate every script offset falls within the code region.
         * Offsets are word indices, not byte offsets — see header doc.
         * (Some shipped scripts have offset=0 for "not implemented" —
         * accept that, the VM will treat it as a no-op return.) */
        for (uint32_t i = 0; i < num_scripts; i++) {
            if (s->script_offsets[i] >= code_len_words) {
                fprintf(stderr, "Cob_Load: %s script[%u] offset %u "
                        "exceeds code length %u words\n",
                        vfs_path, i, s->script_offsets[i], code_len_words);
                goto fail;
            }
        }
    }

    /* Script names — array of char* with each name strdup'd. */
    if (num_scripts > 0) {
        s->script_names = (char **)tak_malloc(num_scripts * sizeof(char *));
        if (!s->script_names) goto fail;
        memset(s->script_names, 0, num_scripts * sizeof(char *));
        const uint32_t *name_ptrs = (const uint32_t *)(buf + off_script_names);
        for (uint32_t i = 0; i < num_scripts; i++) {
            s->script_names[i] = strdup_at(buf, size, name_ptrs[i]);
            if (!s->script_names[i]) {
                fprintf(stderr, "Cob_Load: %s script_names[%u] @ 0x%x bad\n",
                        vfs_path, i, name_ptrs[i]);
                goto fail;
            }
        }
    }

    /* Piece names — same shape. */
    if (num_pieces > 0) {
        s->piece_names = (char **)tak_malloc(num_pieces * sizeof(char *));
        if (!s->piece_names) goto fail;
        memset(s->piece_names, 0, num_pieces * sizeof(char *));
        const uint32_t *name_ptrs = (const uint32_t *)(buf + off_piece_names);
        for (uint32_t i = 0; i < num_pieces; i++) {
            s->piece_names[i] = strdup_at(buf, size, name_ptrs[i]);
            if (!s->piece_names[i]) {
                fprintf(stderr, "Cob_Load: %s piece_names[%u] @ 0x%x bad\n",
                        vfs_path, i, name_ptrs[i]);
                goto fail;
            }
        }
    }

    /* TA:K v6 sound/command name table (header +0x28 = table start,
     * +0x2c = table end; entries are absolute name offsets). Unit COBs
     * ship an empty table (start == end); mission COBs reference these
     * names via PLAY-SOUND / MISSION-COMMAND inline indices. A bad
     * table is non-fatal — the opcodes degrade to index-only. */
    {
        uint32_t snd_a = 0, snd_b = 0;
        if (read_u32(buf, size, COB_OFF_STRING_BLOB_A, &snd_a) == 0 &&
            read_u32(buf, size, COB_OFF_STRING_BLOB_B, &snd_b) == 0 &&
            snd_b > snd_a && snd_a < size &&
            (uint64_t)snd_b <= size &&
            (snd_b - snd_a) % 4 == 0) {
            uint32_t n = (snd_b - snd_a) / 4;
            if (n <= 512) {
                s->sound_names = (char **)tak_malloc(n * sizeof(char *));
                if (s->sound_names) {
                    memset(s->sound_names, 0, n * sizeof(char *));
                    const uint32_t *name_ptrs = (const uint32_t *)(buf + snd_a);
                    uint32_t ok = 1;
                    for (uint32_t i = 0; i < n && ok; i++) {
                        s->sound_names[i] = strdup_at(buf, size, name_ptrs[i]);
                        if (!s->sound_names[i]) ok = 0;
                    }
                    if (ok) {
                        s->num_sound_names = (uint16_t)n;
                    } else {
                        for (uint32_t i = 0; i < n; i++) {
                            if (s->sound_names[i]) tak_free(s->sound_names[i]);
                        }
                        tak_free(s->sound_names);
                        s->sound_names = NULL;
                        s->num_sound_names = 0;
                    }
                }
            }
        }
    }

    tak_free(raw);
    *out = s;
    return 0;

fail:
    Cob_Free(s);
    tak_free(raw);
    return -2;
}

void Cob_Free(CobScript *script) {
    if (!script) return;
    if (script->code) tak_free(script->code);
    if (script->script_offsets) tak_free(script->script_offsets);
    if (script->script_names) {
        for (uint16_t i = 0; i < script->num_scripts; i++) {
            if (script->script_names[i]) tak_free(script->script_names[i]);
        }
        tak_free(script->script_names);
    }
    if (script->piece_names) {
        for (uint16_t i = 0; i < script->num_pieces; i++) {
            if (script->piece_names[i]) tak_free(script->piece_names[i]);
        }
        tak_free(script->piece_names);
    }
    if (script->sound_names) {
        for (uint16_t i = 0; i < script->num_sound_names; i++) {
            if (script->sound_names[i]) tak_free(script->sound_names[i]);
        }
        tak_free(script->sound_names);
    }
    tak_free(script);
}

int Cob_FindScript(const CobScript *script, const char *name) {
    if (!script || !name) return -1;
    for (uint16_t i = 0; i < script->num_scripts; i++) {
        if (tak_stricmp(script->script_names[i], name) == 0) return i;
    }
    return -1;
}

int Cob_FindPiece(const CobScript *script, const char *name) {
    if (!script || !name) return -1;
    for (uint16_t i = 0; i < script->num_pieces; i++) {
        if (tak_stricmp(script->piece_names[i], name) == 0) return i;
    }
    return -1;
}
