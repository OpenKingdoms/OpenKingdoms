#ifndef TAK_NET_STATS_H
#define TAK_NET_STATS_H

#include <stddef.h>
#include <stdint.h>

/*
 * The site's visit counts, kept by the relay for the people who run it.
 *
 * The browser page posts small batches of events to /api/ev and this
 * adds them into one day's counts. Only those counts are kept, one
 * file a day under a folder. The events themselves are never written
 * down, and neither is anything that names a person or a machine.
 *
 * A visitor is a random id the browser made for itself. Each batch is
 * hashed with a salt drawn fresh every UTC day, and the hashes are
 * held only to count each id once that day: they go when the day
 * does. A relay stopped and started on the same day keeps them across
 * the restart in a file it deletes as it reads it back.
 *
 * The rate limit is held in memory only, keyed on a hash of the
 * address the proxy names, with a key drawn at start.
 *
 * No sockets and no clock in here. The HTTP layer hands over a batch,
 * the wall clock and where the request came from, and asks for the
 * days back as JSON.
 *
 * A batch, as web/counts.js sends it:
 *
 *   {"v":1,"id":"<16 to 64 of A-Z a-z 0-9 _ ->","nv":1,
 *    "env":{"br":"chrome","os":"windows","dev":"desktop","scr":"1920-2559"},
 *    "ev":[{"k":"view","page":"landing","s":1},
 *          {"k":"step","step":"files"},
 *          {"k":"start","mode":"skirmish","map":"...","ai":3,"lvl":"hard",
 *           "rb":1,"mod":"vanilla","side":"zhon"},
 *          {"k":"end","mode":"skirmish","out":"win","secs":1260},
 *          {"k":"error","msg":"...","fatal":1}]}
 *
 * nv says the id was made on this page load, which is a new visitor.
 * s on a view says it began a tab's session. A value outside the
 * allowed set for its field is counted as "other", a field that is
 * not known is ignored, and an event of a kind that is not known is
 * dropped and counted as dropped.
 */

#define TAK_STATS_BODY_MAX      4096u   /* one batch's request body */
#define TAK_STATS_BATCH_EVENTS  24u     /* events one batch may carry */
#define TAK_STATS_VALUE_MAX     96u     /* a counted value, with its NUL */
#define TAK_STATS_ENTRIES_MAX   768u    /* counted values one day holds */
#define TAK_STATS_SLOTS         2048u   /* their hash index, a power of two */
#define TAK_STATS_DAYS_MAX      90u     /* the longest span a report covers */
#define TAK_STATS_VISITORS_MAX  (1u << 16)   /* hash slots for one day's ids */
#define TAK_STATS_SOURCES       1024u   /* sources the rate limit tells apart */
#define TAK_STATS_PATH_MAX      512u
#define TAK_STATS_TOKEN_MAX     128u
#define TAK_STATS_ORIGINS_MAX   4u

/* Per source each minute, and for everyone together. */
#define TAK_STATS_SOURCE_BATCHES   60u
#define TAK_STATS_SOURCE_EVENTS    240u
#define TAK_STATS_ALL_BATCHES      3000u
#define TAK_STATS_AUTH_FAILURES    10u
#define TAK_STATS_WINDOW_MS        60000u

/* How often a day with new counts is written while it runs. */
#define TAK_STATS_FLUSH_MS         60000u

/* What is counted, each a column of the day. */
typedef enum TAK_StatDim {
    TAK_STAT_PAGE = 0,      /* a page view, by page                     */
    TAK_STAT_STEP,          /* how far a session got, once per session  */
    TAK_STAT_MODE,          /* a battle started, by kind                */
    TAK_STAT_MP,            /* a multiplayer battle, by how it was entered */
    TAK_STAT_LOAD,          /* a loaded battle, by where it was loaded  */
    TAK_STAT_CAMPAIGN,      /* a campaign mission started, by campaign  */
    TAK_STAT_MISSION,       /* ... and by mission                       */
    TAK_STAT_MAP,           /* any other battle, by map                 */
    TAK_STAT_AI,            /* computer players in it                   */
    TAK_STAT_LEVEL,         /* their difficulty                         */
    TAK_STAT_HUMANS,        /* people in a multiplayer battle           */
    TAK_STAT_REMASTERED,    /* the Remastered Battlefield option        */
    TAK_STAT_MOD,           /* the mod set                              */
    TAK_STAT_SIDE,          /* the side played                          */
    TAK_STAT_OUTCOME,       /* a battle ended, as mode:outcome          */
    TAK_STAT_LENGTH,        /* ... as mode:length                       */
    TAK_STAT_SECS,          /* ... seconds played, summed by mode       */
    TAK_STAT_ERROR,         /* an error the page or the engine reported */
    TAK_STAT_CRASH,         /* one that stopped the engine              */
    TAK_STAT_BROWSER,       /* a visitor's browser family, once a day   */
    TAK_STAT_OS,            /* ... system family                        */
    TAK_STAT_DEVICE,        /* ... desktop, mobile or tablet            */
    TAK_STAT_SCREEN,        /* ... the screen's long side, bucketed     */
    TAK_STAT_REGION,        /* ... the proxy's edge region              */
    TAK_STAT_DIMS
} TAK_StatDim;

typedef struct TAK_StatsEntry {
    uint8_t  dim;
    char     value[TAK_STATS_VALUE_MAX];
    uint32_t n;
} TAK_StatsEntry;

/* One UTC day's counts. */
typedef struct TAK_StatsDay {
    uint32_t day;             /* days since 1970-01-01 */
    uint32_t visitors;        /* distinct ids */
    uint32_t new_visitors;    /* of those, ids made that day */
    uint32_t sessions;        /* views that began a tab's session */
    uint32_t views;
    uint32_t batches;         /* taken */
    uint32_t events;          /* counted */
    uint32_t dropped;         /* events not counted: unknown kind or past the cap */
    uint32_t refused;         /* batches that were not batches */
    uint32_t limited;         /* batches turned away by the rate limit */
    uint32_t capped;          /* 1 when more ids came than the day counts exactly */
    uint32_t count;
    uint32_t per_dim[TAK_STAT_DIMS];
    TAK_StatsEntry entry[TAK_STATS_ENTRIES_MAX];
    uint16_t slot[TAK_STATS_SLOTS];    /* entry index + 1, 0 is empty */
} TAK_StatsDay;

typedef struct TAK_StatsSource {
    uint64_t key;             /* 0 is a free slot */
    uint32_t batches, events, auth_failures;
} TAK_StatsSource;

typedef struct TAK_Stats {
    char         dir[TAK_STATS_PATH_MAX];   /* "" keeps counts in memory */
    TAK_StatsDay today;
    int          dirty;
    uint64_t     flushed_ms;
    uint64_t     salt;
    uint32_t     seen_count;
    uint64_t     seen[TAK_STATS_VISITORS_MAX];
    uint64_t     source_key;
    uint64_t     window;          /* now_ms / TAK_STATS_WINDOW_MS of the tallies */
    uint32_t     window_batches;
    TAK_StatsSource source[TAK_STATS_SOURCES];
    char         token[TAK_STATS_TOKEN_MAX];
    char         origin[TAK_STATS_ORIGINS_MAX][128];
    uint32_t     origins;
    uint32_t     write_failures;
} TAK_Stats;

typedef enum TAK_StatsResult {
    TAK_STATS_TAKEN = 0,      /* counted, perhaps with events dropped */
    TAK_STATS_MALFORMED,      /* not a batch, nothing counted */
    TAK_STATS_LIMITED         /* this source, or everyone, sent too much this minute */
} TAK_StatsResult;

/* Counts in memory only, for a relay given nowhere to keep them. */
void TAK_Stats_Init(TAK_Stats *s);

/* Counts kept under `dir`, made if missing. Today's file is read back
 * if there is one, and so is the restart file of today's visitors.
 * Returns 0, or -1 when the folder cannot be made, which leaves the
 * counts in memory only. */
int  TAK_Stats_Open(TAK_Stats *s, const char *dir, uint64_t now_ms);

/* The bearer token /api/stats asks for, "" or NULL for none, which
 * leaves the report unanswered. Returns 1 when it is kept, 0 for none,
 * and -1 for one shorter than 16 characters or too long, which is
 * refused and also leaves the report unanswered. */
int  TAK_Stats_SetToken(TAK_Stats *s, const char *token);
int  TAK_Stats_HasToken(const TAK_Stats *s);

/* A page origin that may post, as https://host. With none given, any
 * origin may. Returns -1 past TAK_STATS_ORIGINS_MAX. */
int  TAK_Stats_AllowOrigin(TAK_Stats *s, const char *origin);
int  TAK_Stats_OriginAllowed(const TAK_Stats *s, const char *origin);

/* Add one batch. `source` is the client's address as the proxy names
 * it and `region` the proxy's edge region, either NULL when not known. */
TAK_StatsResult TAK_Stats_Post(TAK_Stats *s, const char *body, size_t len,
                               const char *source, const char *region,
                               uint64_t now_ms);

/* 1 when `authorization` is "Bearer <the token>", 0 when it is not,
 * -1 when this source has failed too often this minute. */
int  TAK_Stats_Authorized(TAK_Stats *s, const char *authorization,
                          const char *source, uint64_t now_ms);

/* The last `days` days to today as JSON, into `out`. Returns its
 * length, or 0 when it does not fit. */
size_t TAK_Stats_ReportJson(TAK_Stats *s, uint32_t days, uint64_t now_ms,
                            char *out, size_t cap);

/* Rolls the day over and writes a changed day every TAK_STATS_FLUSH_MS.
 * Call it every pass of the loop. */
void TAK_Stats_Tick(TAK_Stats *s, uint64_t now_ms);

/* Write today now. 0, or -1 when it could not be written. */
int  TAK_Stats_Flush(TAK_Stats *s);

/* Write today and keep today's visitors for a restart today. */
void TAK_Stats_Close(TAK_Stats *s, uint64_t now_ms);

/* For a test and the files: a day number as YYYY-MM-DD, and a value
 * a dimension holds for that day, 0 when it holds none. */
void     TAK_Stats_DayText(uint32_t day, char out[11]);
uint32_t TAK_Stats_Count(const TAK_StatsDay *d, TAK_StatDim dim, const char *value);
const char *TAK_Stats_DimName(TAK_StatDim dim);

#endif /* TAK_NET_STATS_H */
