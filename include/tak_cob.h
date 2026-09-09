#ifndef TAK_COB_H
#define TAK_COB_H

#include "tak_types.h"

/* ── COB script bundle (Phase D M1) ────────────────────────────────
 *
 * One CobScript per UnitDef — parsed once at LS_LOAD_UNITS time from
 * data/scripts/<unitname>.cob. Holds the bytecode, named entry-point
 * table, piece-name table, and metadata. The VM (M2+) consumes this
 * read-only.
 *
 * Format (TAK COB v6, empirically verified by tools/probe_cob.c):
 *   header @ 0x00
 *     +0x00  uint32  version           (must be 6)
 *     +0x04  uint32  num_scripts
 *     +0x08  uint32  num_pieces
 *     +0x0c  uint32  code_length       (in 32-bit words)
 *     +0x10  uint32  num_static_vars
 *     +0x14  uint32  reserved (=0)
 *     +0x18  uint32  off_script_offsets
 *     +0x1c  uint32  off_script_name_ptrs
 *     +0x20  uint32  off_piece_name_ptrs
 *     +0x24  uint32  off_code           (typically 52)
 *     +0x28  uint32  off_string_blob ?  (== off_string_blob_end always seen)
 *     +0x2c  uint32  off_string_blob ?
 *   bytecode @ off_code, code_length × 4 bytes
 *   script_offsets[num_scripts]    @ off_script_offsets — WORD INDICES
 *                                    into the bytecode (PC values; the VM
 *                                    fetches code[pc] as the opcode).
 *   script_name_ptrs[num_scripts]  @ off_script_name_ptrs — absolute
 *                                    file offsets to null-terminated names
 *   piece_name_ptrs[num_pieces]    @ off_piece_name_ptrs — same
 *
 * IMPORTANT: script_offsets[] values are word indices, NOT byte offsets.
 * The decomp dispatch reads `code[pc * 4 + 4]` for operands, confirming
 * PC is a word index. Some shipped offsets are not 4-aligned as byte
 * values (e.g. 0x5cea = 23786 byte offset, not divisible by 4) which
 * would be invalid byte offsets but are valid word indices into a
 * uint32 code array. */

typedef struct CobScript {
    uint32_t   version;          /* always 6 */
    uint32_t   num_static_vars;
    uint32_t  *code;             /* owned, num_code_words long */
    uint32_t   num_code_words;
    char     **script_names;     /* [num_scripts] strdup'd */
    uint32_t  *script_offsets;   /* [num_scripts] word offsets into code */
    uint16_t   num_scripts;
    uint16_t   num_pieces;
    char     **piece_names;      /* [num_pieces] strdup'd */
    /* TA:K v6 sound/command name table: header +0x28 points at an
     * array of absolute name offsets, +0x2c at its end. PLAY-SOUND
     * (0x10072000), MISSION-COMMAND (0x10073000) and 0x10074000 carry
     * inline indices into this table (the legacy dispatch resolves
     * them through engine[3]+0x2c, legacy:306856). Unit COBs
     * usually have an empty table; mission COBs populate it. */
    char     **sound_names;      /* [num_sound_names] strdup'd */
    uint16_t   num_sound_names;
} CobScript;

/* Load a .cob via VFS. Returns 0 on success and writes *out;
 * -1 on I/O error; -2 on bad magic / corrupt header. Caller frees
 * with Cob_Free. */
int  Cob_Load(CobScript **out, const char *vfs_path);

/* Tear down. NULL-safe. */
void Cob_Free(CobScript *script);

/* Find a script entry by name (case-insensitive). Returns the script
 * index in [0, num_scripts) or -1 if not found. */
int  Cob_FindScript(const CobScript *script, const char *name);

/* Find a piece entry by name (case-insensitive). Returns the piece
 * index in [0, num_pieces) or -1 if not found. */
int  Cob_FindPiece(const CobScript *script, const char *name);

#endif /* TAK_COB_H */
