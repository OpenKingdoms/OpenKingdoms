#ifndef TAK_NET_LEDGER_H
#define TAK_NET_LEDGER_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_protocol.h"

/*
 * The ledger: every finished multiplayer match the relay has been told
 * about, and the sums the leaderboard shows.
 *
 * Design in docs/notes/2026-09-14-multiplayer-leaderboard.md. The short
 * version: one append only file of records written with the protocol's
 * own bounded codec, read whole into this struct at start, and every
 * question the site asks is answered from memory. No database, no
 * allocation, no network, so the same code runs in okrelay and in a
 * test with nothing but a scratch file.
 *
 * A player is the name they typed, trimmed and compared without case.
 * Two people who type the same name share a record and anyone can type
 * another player's name. That is accepted for a playtest community and
 * a check can be added later without touching the records.
 */

#define TAK_LEDGER_MATCHES_MAX   8192
#define TAK_LEDGER_PLAYERS_MAX   8192
#define TAK_LEDGER_FILE_MAGIC    "OKLEDGER"
#define TAK_LEDGER_FILE_VERSION  2
#define TAK_LEDGER_PATH_MAX      512

typedef enum TAK_LedgerResult {
    TAK_LEDGER_LOST = 0,
    TAK_LEDGER_WON  = 1
} TAK_LedgerResult;

/* Record tags in the file. A reader skips a tag it does not know. */
typedef enum TAK_LedgerTag {
    TAK_LEDGER_TAG_MATCH   = 1,
    TAK_LEDGER_TAG_CONFIRM = 2
} TAK_LedgerTag;

typedef struct TAK_LedgerSeat {
    uint8_t  seat;
    uint8_t  kind;            /* TAK_NSLOT_HUMAN or TAK_NSLOT_COMPUTER */
    uint8_t  side, colour, team;
    uint8_t  standing;        /* had units and had not resigned at the end */
    uint8_t  eliminated;
    uint8_t  place;           /* 1 is best, ties share */
    uint8_t  result;          /* TAK_LedgerResult */
    uint64_t player_id;       /* 0 for a computer */
    char     name[TAK_NET_NAME_MAX];
    /* The end screen's columns (docs/notes/2026-09-10-end-of-battle.md). */
    int32_t  units_built;
    int32_t  kills;
    int32_t  losses;
    int32_t  score;
    int32_t  last_alive_tick;
} TAK_LedgerSeat;

typedef struct TAK_LedgerMatch {
    uint32_t id;              /* the ledger's own, from 1, never reused */
    uint32_t relay_match_id;  /* the relay's, which restarts with it */
    uint64_t started_ms;      /* unix milliseconds, 0 when unknown */
    uint64_t ended_ms;
    uint32_t end_tick;
    uint32_t options;         /* TAK_ROOMOPT_* */
    uint16_t unit_cap;
    uint8_t  stats_version;
    uint8_t  reports;         /* clients that sent the same tallies */
    uint8_t  disputed;        /* a client sent different ones */
    uint8_t  seat_count;
    char     map_name[TAK_NET_MAP_NAME_MAX];
    uint8_t  map_fingerprint[TAK_NET_FINGERPRINT_BYTES];
    TAK_LedgerSeat seat[TAK_NET_SEATS];
} TAK_LedgerMatch;

/* One row of the leaderboard table: a player's sums over every match. */
typedef struct TAK_LedgerRow {
    uint64_t player_id;
    char     name[TAK_NET_NAME_MAX];   /* as last typed */
    uint32_t games, wins, losses;
    int64_t  score;
    int64_t  units_built;
    int64_t  kills;
    int64_t  units_lost;
    int64_t  ticks_alive;
    uint64_t first_played_ms, last_played_ms;
} TAK_LedgerRow;

typedef struct TAK_Ledger {
    TAK_LedgerMatch match[TAK_LEDGER_MATCHES_MAX];
    uint32_t count;
    uint32_t next_id;
    /* Grows with every record written, so a page that polls can tell
     * whether anything changed without reading the table. */
    uint32_t version;
    uint32_t refused;         /* matches the cap turned away */
    uint32_t bad_records;     /* sound records on load that would not read */
    uint32_t bad_bytes;       /* bytes on load that were no record at all */
    uint32_t write_failures;  /* appends the file did not take */
    void    *file;            /* FILE*, NULL when memory only */
    char     path[TAK_LEDGER_PATH_MAX];
} TAK_Ledger;

/* Memory only. Records live until the process ends. */
void TAK_Ledger_Init(TAK_Ledger *l);

/* Read the file at `path`, or make it, and keep it open for appends.
 * An empty file is a new ledger. Returns -1 when the path cannot be
 * read or written or holds something that is not a ledger, and then
 * the file is left exactly as it was. Bytes that are not sound records
 * are skipped and the file rewritten beside itself without them. */
int  TAK_Ledger_Open(TAK_Ledger *l, const char *path);
void TAK_Ledger_Close(TAK_Ledger *l);

/* The player a typed name stands for. Trimmed, compared without case,
 * hashed. 0 for an empty name, never 0 otherwise. */
uint64_t TAK_Ledger_PlayerId(const char *name);

/* Fill place and result for every seat. Everyone standing shares first
 * place, the rest rank by how long they lasted, ties share a place. */
void TAK_Ledger_Place(TAK_LedgerMatch *m);

/* Record one finished match. Assigns the id and stamps reports to 1.
 * Returns the id, or 0 when the ledger is full. */
uint32_t TAK_Ledger_Record(TAK_Ledger *l, const TAK_LedgerMatch *m);

/* Another client reported the same match. `agrees` says whether its
 * tallies matched what was recorded. Returns 0, or -1 for no such id. */
int  TAK_Ledger_Confirm(TAK_Ledger *l, uint32_t id, int agrees);

/* Whether two seat sets carry the same tallies, for Confirm's caller. */
int  TAK_Ledger_SameTallies(const TAK_LedgerMatch *a, const TAK_LedgerMatch *b);

const TAK_LedgerMatch *TAK_Ledger_Find(const TAK_Ledger *l, uint32_t id);

/* How many games are disputed. Those are left out of every sum. */
uint32_t TAK_Ledger_Disputed(const TAK_Ledger *l);

/* Every player who sat in an undisputed match, one row each, wins
 * first. Writes up to `cap` rows and returns how many. */
uint32_t TAK_Ledger_Table(const TAK_Ledger *l, TAK_LedgerRow *rows, uint32_t cap);

/* One player's row. Returns 1, or 0 when they never played. */
int  TAK_Ledger_RowFor(const TAK_Ledger *l, uint64_t player_id, TAK_LedgerRow *row);

/* The ids of a player's matches, newest first, from `offset`. Writes up
 * to `cap` and returns how many. `total` gets the whole count. */
uint32_t TAK_Ledger_History(const TAK_Ledger *l, uint64_t player_id,
                            uint32_t offset, uint32_t *ids, uint32_t cap,
                            uint32_t *total);

/* The codec, which is also the file format. Encode writes a whole
 * record, tag and length first and a checksum last, and returns its
 * size or 0. Decode reads the payload between them. */
size_t TAK_Ledger_EncodeMatch(const TAK_LedgerMatch *m, void *out, size_t cap);
int    TAK_Ledger_DecodeMatch(TAK_LedgerMatch *m, const void *p, size_t len);

/* Take records from bytes read from a file after its header. Bytes
 * that are no record are stepped over and counted in bad_bytes. With
 * `more` set a record cut off at the end is left for the next call.
 * Returns how many bytes were consumed. */
size_t TAK_Ledger_Load(TAK_Ledger *l, const void *bytes, size_t len, int more);

#endif /* TAK_NET_LEDGER_H */
