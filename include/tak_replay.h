#ifndef TAK_REPLAY_H
#define TAK_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "tak_battle_config.h"
#include "tak_commands.h"
#include "tak_data_fingerprint.h"

/*
 * A replay file: the battle's header and every command it applied, in
 * the order it applied them.
 *
 * Lockstep already makes a battle a pure function of its seed, its
 * configuration and its commands, so a replay is nothing more than
 * those. The file is a fixed header followed by a stream of records,
 * written as the battle runs and read back one record at a time, so
 * neither side ever holds the whole log.
 *
 * Records are a tag byte and a tick delta as a variable length integer,
 * then the payload:
 *   command     the seat, the wire length, the wire bytes
 *   checkpoint  the simulation hash after that tick's orders, and the
 *               checksum of every record byte before this one
 *   end         the checksum of every record byte before this one
 *
 * The header is rewritten when the recording closes, marked finished
 * and carrying the last tick and the command count. A header not
 * marked finished is a battle that never closed, a browser tab shut
 * mid game, and plays up to its last checkpoint. A finished header
 * whose stream stops short of its end record was cut, and is refused.
 */

#define TAK_REPLAY_EXT          ".okreplay"
#define TAK_REPLAY_VERSION      1
#define TAK_REPLAY_HEADER_BYTES 576
/* A recording stops taking commands here. Hours of a busy match fit in
 * a few megabytes, so this is a guard, not a limit anyone meets. */
#define TAK_REPLAY_MAX_BYTES    (16u * 1024u * 1024u)
/* Ticks between two simulation hash checkpoints: the protocol's own
 * cadence, so a match computes one hash for both. No record lies more
 * than this past the last checkpoint, which the reader enforces. */
#define TAK_REPLAY_HASH_EVERY   60
/* The longest battle a replay holds: eight hours at 60 Hz. A tick past
 * it ends a recording and is refused in a file. */
#define TAK_REPLAY_MAX_TICKS    (8u * 60u * 60u * 60u)
#define TAK_REPLAY_ERR_MAX      256
/* What the saved game directory keeps of replays: the newest this many,
 * within this many bytes, oldest pruned first and never a save. The
 * page keeps browser storage to the same two numbers. */
#define TAK_REPLAY_KEEP         30
#define TAK_REPLAY_BUDGET_BYTES (48u * 1024u * 1024u)
/* The most rows the Replays list reads. */
#define TAK_REPLAY_LIST_MAX     64
#define TAK_REPLAY_IO_BYTES     8192

enum {
    TAK_REPLAYF_MATCH    = 0x01,   /* recorded in a multiplayer match */
    TAK_REPLAYF_MAP_FP   = 0x02,   /* map_fp holds the map's fingerprint */
    TAK_REPLAYF_FINISHED = 0x04    /* the recording closed; set on close */
};

typedef struct TAK_ReplayHeader {
    uint16_t version;
    uint32_t engine_build_id;
    uint64_t data_schema;
    uint64_t data_content;
    uint64_t data_group[TAK_DATA_GROUP_COUNT];
    uint8_t  map_fp[32];
    uint8_t  flags;
    uint8_t  local_seat;          /* the player who recorded it, 1..8 */
    uint8_t  turn_ticks;          /* a match's turn, 0 for a skirmish */
    uint64_t recorded_at_utc;
    char     map_kingdom[32];
    BattleConfig cfg;
    /* Filled on close. 0 in a recording that never closed. */
    uint32_t end_tick;
    uint32_t command_count;
} TAK_ReplayHeader;

/* ── Writing ──────────────────────────────────────────────────────── */

typedef struct TAK_ReplayWriter TAK_ReplayWriter;

/* Create the file and write the header. NULL with `err` filled when the
 * file cannot be made. */
TAK_ReplayWriter *TAK_ReplayWriter_Open(const char *path,
                                        const TAK_ReplayHeader *hdr,
                                        char *err, size_t err_cap);

/* One command as the queue applied it, cmd->tick and cmd->seat
 * included. Ticks never go backwards. Returns 0, or -1 when the command
 * was not taken: a tick out of order, an unreadable command, a full
 * file or a write that failed. */
int  TAK_ReplayWriter_Command(TAK_ReplayWriter *w, const TAK_GameCommand *cmd);

/* The simulation hash after tick `tick`'s orders ran. */
int  TAK_ReplayWriter_Checkpoint(TAK_ReplayWriter *w, uint32_t tick, uint32_t hash);

/* Push what is buffered to the file, so a tab that closes now leaves a
 * replay that plays to here. */
int  TAK_ReplayWriter_Flush(TAK_ReplayWriter *w);

/* Write the end record at `end_tick`, rewrite the header and close.
 * Frees `w` whatever happens. Returns 0 when the file is complete. */
int  TAK_ReplayWriter_Close(TAK_ReplayWriter *w, uint32_t end_tick);

/* The size guard for this writer only, for a test that needs a file
 * past TAK_REPLAY_MAX_BYTES. */
void TAK_ReplayWriter_SetMaxBytes(TAK_ReplayWriter *w, uint32_t max_bytes);

/* Bytes written so far, header included. */
uint32_t TAK_ReplayWriter_Bytes(const TAK_ReplayWriter *w);
/* 1 once the size guard stopped taking commands. */
int  TAK_ReplayWriter_Full(const TAK_ReplayWriter *w);
/* The last tick a command or checkpoint named. */
uint32_t TAK_ReplayWriter_LastTick(const TAK_ReplayWriter *w);

/* The size of a writer or a reader, which is all either ever holds. */
size_t TAK_ReplayWriter_Footprint(void);
size_t TAK_ReplayReader_Footprint(void);

/* ── Reading ──────────────────────────────────────────────────────── */

typedef struct TAK_ReplayReader TAK_ReplayReader;

typedef enum {
    TAK_REPLAY_REC_COMMAND = 1,
    TAK_REPLAY_REC_CHECKPOINT = 2,
    TAK_REPLAY_REC_END = 3
} TAK_ReplayRecordKind;

typedef struct TAK_ReplayRecord {
    uint8_t  kind;
    uint32_t tick;
    uint32_t hash;                  /* a checkpoint's */
    const TAK_GameCommand *cmd;     /* a command's, owned by the reader */
} TAK_ReplayRecord;

/* The header alone, checked field by field, and the file's size
 * against TAK_REPLAY_MAX_BYTES. 0, or -1 with `err` filled. */
int  TAK_Replay_ReadHeader(const char *path, TAK_ReplayHeader *out,
                           char *err, size_t err_cap);

/* Open a replay to play. The whole stream is walked once first, a
 * record at a time, so a file that is cut or damaged anywhere is
 * refused here rather than halfway through a battle. `playable_end`
 * is the tick playback stops at: the header's last tick, or for a
 * recording that never closed, its last checkpoint. */
TAK_ReplayReader *TAK_Replay_Open(const char *path, TAK_ReplayHeader *hdr,
                                  uint32_t *playable_end,
                                  char *err, size_t err_cap);

/* The next record. 1 with `out` filled, 0 at the end, -1 on a damaged
 * record. A record past the playable end reads as the end. */
int  TAK_Replay_Next(TAK_ReplayReader *r, TAK_ReplayRecord *out);

void TAK_Replay_Close(TAK_ReplayReader *r);

/* ── Compatibility ────────────────────────────────────────────────── */

/* Whether this build, with this data, plays the battle the header
 * recorded. Determinism needs the same engine build and the same data,
 * so anything else is refused with the reason. `fp` may be NULL when
 * nothing is mounted, which only matches a recording made the same
 * way. 0 when it plays, -1 with `err` filled. */
int  TAK_Replay_CheckCompatible(const TAK_ReplayHeader *hdr,
                                uint32_t engine_build_id,
                                const TAK_DataFingerprint *fp,
                                char *err, size_t err_cap);

/* The file name a new recording takes, from the UTC time it started:
 * "20260930-141502". No extension. */
void TAK_Replay_Slug(uint64_t utc_seconds, char *out, size_t cap);

#endif /* TAK_REPLAY_H */
