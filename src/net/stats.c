/*
 * stats.c -- the site's visit counts (tak_net_stats.h).
 *
 * A day is a table of column, value and count with a hash index over
 * it. On disk it is text, a total a line and then a counted value a
 * line, tab separated, so a person can read a day with cat. A day file
 * is written whole beside the old one and swapped in, so a crash
 * leaves the last whole day.
 */

#if defined(_MSC_VER)
#define _CRT_RAND_S
#endif

#include "tak_net_stats.h"
#include "tak_bytes.h"
#include "tak_sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define DAY_MS          86400000ull
#define FILE_MAGIC      "okstats 1"
#define SEEN_MAGIC      "OKVIS001"
#define SEEN_FILE       "visitors.bin"
#define SEEN_LIMIT      (TAK_STATS_VISITORS_MAX / 4 * 3)
#define BATTLE_SECS_MAX 86400

/* ── Columns ──────────────────────────────────────────────────────────── */

static const char *const k_page[]    = { "landing", "play", "mods", "leaderboard", "stats", "privacy", "atlas", NULL };
static const char *const k_step[]    = { "visit", "files", "menu", "battle", "skirmish", "campaign", "multiplayer", NULL };
static const char *const k_mode[]    = { "campaign", "skirmish", "multiplayer", "replay", "load", "editor", NULL };
static const char *const k_mp[]      = { "host", "join", "dropin", "watch", "rejoin", NULL };
static const char *const k_load[]    = { "skirmish", "campaign", "battle", NULL };
static const char *const k_ai[]      = { "0", "1", "2", "3", "4", "5", "6", "7", NULL };
static const char *const k_level[]   = { "easy", "normal", "hard", "brutal", "mixed", "none", NULL };
static const char *const k_humans[]  = { "1", "2", "3", "4", "5", "6", "7", "8", NULL };
static const char *const k_onoff[]   = { "on", "off", NULL };
static const char *const k_side[]    = { "aramon", "veruna", "taros", "zhon", "creon", "none", NULL };
static const char *const k_out[]     = { "win", "loss", "draw", "quit", "restart", "disconnect", "desync", "over", "left", NULL };
static const char *const k_length[]  = { "under 1m", "1-5m", "5-15m", "15-30m", "30-60m", "1-2h", "over 2h", NULL };
static const char *const k_browser[] = { "chrome", "edge", "firefox", "safari", "opera", "samsung", NULL };
static const char *const k_os[]      = { "windows", "macos", "linux", "android", "ios", "chromeos", NULL };
static const char *const k_device[]  = { "desktop", "mobile", "tablet", NULL };
static const char *const k_screen[]  = { "<1024", "1024-1365", "1366-1599", "1600-1919", "1920-2559", "2560+", NULL };

/* How a column's values are checked: against a list, as free text, as
 * a region code, or built here from parts already checked. */
enum { ALLOW, FREE, REGION, BUILT };

typedef struct Column {
    const char        *name;
    uint8_t            kind;
    uint16_t           day_cap;      /* distinct values a day, "other" aside */
    uint16_t           span_cap;     /* ... across a report */
    uint16_t           top;          /* values a report lists, 0 for all */
    const char *const *allow;
} Column;

static const Column k_col[TAK_STAT_DIMS] = {
    [TAK_STAT_PAGE]       = { "page",       ALLOW,  0,    0,    0,  k_page },
    [TAK_STAT_STEP]       = { "step",       ALLOW,  0,    0,    0,  k_step },
    [TAK_STAT_MODE]       = { "mode",       ALLOW,  0,    0,    0,  k_mode },
    [TAK_STAT_MP]         = { "mp",         ALLOW,  0,    0,    0,  k_mp },
    [TAK_STAT_LOAD]       = { "load",       ALLOW,  0,    0,    0,  k_load },
    [TAK_STAT_CAMPAIGN]   = { "campaign",   FREE,   16,   1024, 30, NULL },
    [TAK_STAT_MISSION]    = { "mission",    FREE,   120,  1024, 30, NULL },
    [TAK_STAT_MAP]        = { "map",        FREE,   120,  1024, 30, NULL },
    [TAK_STAT_AI]         = { "ai",         ALLOW,  0,    0,    0,  k_ai },
    [TAK_STAT_LEVEL]      = { "level",      ALLOW,  0,    0,    0,  k_level },
    [TAK_STAT_HUMANS]     = { "humans",     ALLOW,  0,    0,    0,  k_humans },
    [TAK_STAT_REMASTERED] = { "remastered", ALLOW,  0,    0,    0,  k_onoff },
    [TAK_STAT_MOD]        = { "mod",        FREE,   32,   1024, 30, NULL },
    [TAK_STAT_SIDE]       = { "side",       ALLOW,  0,    0,    0,  k_side },
    [TAK_STAT_OUTCOME]    = { "outcome",    BUILT,  80,   80,   0,  NULL },
    [TAK_STAT_LENGTH]     = { "length",     BUILT,  64,   64,   0,  NULL },
    [TAK_STAT_SECS]       = { "secs",       BUILT,  8,    8,    0,  NULL },
    [TAK_STAT_ERROR]      = { "error",      FREE,   40,   1024, 30, NULL },
    [TAK_STAT_CRASH]      = { "crash",      FREE,   40,   1024, 30, NULL },
    [TAK_STAT_BROWSER]    = { "browser",    ALLOW,  0,    0,    0,  k_browser },
    [TAK_STAT_OS]         = { "os",         ALLOW,  0,    0,    0,  k_os },
    [TAK_STAT_DEVICE]     = { "device",     ALLOW,  0,    0,    0,  k_device },
    [TAK_STAT_SCREEN]     = { "screen",     ALLOW,  0,    0,    0,  k_screen },
    [TAK_STAT_REGION]     = { "region",     REGION, 40,   1024, 30, NULL },
};

static uint32_t list_len(const char *const *l) {
    uint32_t n = 0;
    while (l && l[n]) n++;
    return n;
}

static uint32_t day_cap(int dim) {
    return k_col[dim].kind == ALLOW ? list_len(k_col[dim].allow) : k_col[dim].day_cap;
}

static uint32_t span_cap(int dim) {
    return k_col[dim].kind == ALLOW ? list_len(k_col[dim].allow) : k_col[dim].span_cap;
}

const char *TAK_Stats_DimName(TAK_StatDim dim) {
    return (unsigned)dim < TAK_STAT_DIMS ? k_col[dim].name : "";
}

static int dim_by_name(const char *name) {
    for (int d = 0; d < TAK_STAT_DIMS; d++)
        if (strcmp(k_col[d].name, name) == 0) return d;
    return -1;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int same_folded(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* The list's own spelling of v, or "other". */
static const char *allowed(const char *const *list, const char *v) {
    if (v)
        for (uint32_t i = 0; list[i]; i++)
            if (same_folded(list[i], v)) return list[i];
    return "other";
}

/* Printable ASCII, runs of space folded, the ends trimmed, cut to fit.
 * A tab or line break reads as a space and other control bytes go. */
static void clean(const char *in, char *out, size_t cap) {
    size_t k = 0;
    int space = 0;
    for (; *in && k + 1 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') c = ' ';
        if (c < 0x20 || c == 0x7f) continue;
        if (c == ' ') { space = k > 0; continue; }
        if (space && k + 2 < cap) out[k++] = ' ';
        space = 0;
        out[k++] = (char)(c < 0x80 ? c : '?');
    }
    out[k] = '\0';
}

/* The value a column counts for what a batch said. */
static const char *column_value(int dim, const char *v, char *buf, size_t cap) {
    switch (k_col[dim].kind) {
    case ALLOW:
        return allowed(k_col[dim].allow, v);
    case REGION:
        if (!v || !v[0] || strcmp(v, "none") == 0) return "none";
        if (strlen(v) != 3) return "other";
        for (int i = 0; i < 3; i++) {
            int c = lower((unsigned char)v[i]);
            if (c < 'a' || c > 'z') return "other";
            buf[i] = (char)c;
        }
        buf[3] = '\0';
        return buf;
    default:
        clean(v ? v : "", buf, cap);
        return buf[0] ? buf : "other";
    }
}

/* ── A day's table ────────────────────────────────────────────────────── */

static uint32_t key_hash(int dim, const char *v) {
    uint32_t h = 2166136261u ^ (uint32_t)dim;
    h *= 16777619u;
    for (; *v; v++) { h ^= (unsigned char)*v; h *= 16777619u; }
    return h;
}

static TAK_StatsEntry *day_lookup(const TAK_StatsDay *d, int dim, const char *v, uint32_t *slot) {
    uint32_t h = key_hash(dim, v) & (TAK_STATS_SLOTS - 1);
    while (d->slot[h]) {
        const TAK_StatsEntry *e = &d->entry[d->slot[h] - 1];
        if (e->dim == dim && strcmp(e->value, v) == 0) return (TAK_StatsEntry *)e;
        h = (h + 1) & (TAK_STATS_SLOTS - 1);
    }
    if (slot) *slot = h;
    return NULL;
}

/* The entry for v, made if the column has room, else the column's
 * "other". NULL only when the whole day is full, which the caps keep
 * from happening. */
static TAK_StatsEntry *day_entry(TAK_StatsDay *d, int dim, const char *v) {
    uint32_t slot = 0;
    TAK_StatsEntry *e = day_lookup(d, dim, v, &slot);
    if (e) return e;
    int other = strcmp(v, "other") == 0;
    if (!other && d->per_dim[dim] >= day_cap(dim)) return day_entry(d, dim, "other");
    if (d->count >= TAK_STATS_ENTRIES_MAX) return other ? NULL : day_entry(d, dim, "other");
    e = &d->entry[d->count];
    e->dim = (uint8_t)dim;
    snprintf(e->value, sizeof e->value, "%s", v);
    e->n = 0;
    d->slot[slot] = (uint16_t)(d->count + 1);
    d->count++;
    if (!other) d->per_dim[dim]++;
    return e;
}

static uint32_t sat_add(uint32_t a, uint64_t b) {
    uint64_t s = (uint64_t)a + b;
    return s > 0xffffffffu ? 0xffffffffu : (uint32_t)s;
}

static void day_add(TAK_StatsDay *d, int dim, const char *v, uint64_t n) {
    TAK_StatsEntry *e = day_entry(d, dim, v);
    if (e) e->n = sat_add(e->n, n);
}

static void day_reset(TAK_StatsDay *d, uint32_t day) {
    memset(d, 0, sizeof *d);
    d->day = day;
}

uint32_t TAK_Stats_Count(const TAK_StatsDay *d, TAK_StatDim dim, const char *value) {
    if ((unsigned)dim >= TAK_STAT_DIMS || !value) return 0;
    const TAK_StatsEntry *e = day_lookup(d, (int)dim, value, NULL);
    return e ? e->n : 0;
}

/* ── Dates ────────────────────────────────────────────────────────────── */

/* Days since 1970-01-01 as a civil date (H. Hinnant's days_to_civil). */
void TAK_Stats_DayText(uint32_t day, char out[11]) {
    int64_t z = (int64_t)day + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t dd = doy - (153 * mp + 2) / 5 + 1;
    uint32_t mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2) y++;
    snprintf(out, 11, "%04d-%02u-%02u", (int)(y % 10000), (unsigned)(mm % 100), (unsigned)(dd % 100));
}

/* ── Randomness and hashing ───────────────────────────────────────────── */

static uint64_t random64(void) {
    uint64_t v = 0;
#if defined(_MSC_VER)
    unsigned int a = 0, b = 0;
    if (rand_s(&a) == 0 && rand_s(&b) == 0) v = ((uint64_t)a << 32) | b;
#elif !defined(_WIN32)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&v, 1, sizeof v, f) != sizeof v) v = 0;
        fclose(f);
    }
#endif
    if (v) return v;
    /* No system source: a hash of what differs from one call to the next. */
    static uint64_t counter;
    uint8_t mix[40], d[TAK_SHA256_BYTES];
    memset(mix, 0, sizeof mix);
    tak_put_u64(mix, ++counter);
    tak_put_u64(mix + 8, (uint64_t)time(NULL));
    tak_put_u64(mix + 16, (uint64_t)clock());
    tak_put_u64(mix + 24, (uint64_t)(uintptr_t)&v);
    tak_put_u64(mix + 32, (uint64_t)(uintptr_t)&counter);
    TAK_Sha256_Hash(mix, sizeof mix, d);
    v = tak_get_u64(d);
    return v ? v : 1;
}

/* The first eight bytes of SHA-256 over a key and a text, never 0. */
static uint64_t keyed(uint64_t key, const char *text) {
    uint8_t k[8], d[TAK_SHA256_BYTES];
    tak_put_u64(k, key);
    TAK_Sha256 c;
    TAK_Sha256_Init(&c);
    TAK_Sha256_Update(&c, k, sizeof k);
    TAK_Sha256_Update(&c, text, strlen(text));
    TAK_Sha256_Final(&c, d);
    uint64_t v = tak_get_u64(d);
    return v ? v : 1;
}

/* 1 when h is new today, 0 when seen, -1 when the day has no room. */
static int seen_add(TAK_Stats *s, uint64_t h) {
    uint32_t i = (uint32_t)(h ^ (h >> 32)) & (TAK_STATS_VISITORS_MAX - 1);
    while (s->seen[i]) {
        if (s->seen[i] == h) return 0;
        i = (i + 1) & (TAK_STATS_VISITORS_MAX - 1);
    }
    if (s->seen_count >= SEEN_LIMIT) return -1;
    s->seen[i] = h;
    s->seen_count++;
    return 1;
}

/* ── Files ────────────────────────────────────────────────────────────── */

static int flush_file(FILE *f) {
    if (fflush(f) != 0) return -1;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
    return fsync(fileno(f)) == 0 ? 0 : -1;
#endif
}

static int replace_file(const char *from, const char *to) {
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    if (rename(from, to) != 0) return -1;
    /* The rename itself is only durable once the folder is. */
    char dir[TAK_STATS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", to);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else snprintf(dir, sizeof dir, ".");
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { (void)fsync(fd); close(fd); }
    return 0;
#endif
}

static int make_dir(const char *dir) {
#if defined(_WIN32)
    if (_mkdir(dir) == 0) return 0;
    DWORD a = GetFileAttributesA(dir);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : -1;
#else
    if (mkdir(dir, 0700) == 0) return 0;
    struct stat st;
    return (errno == EEXIST && stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
#endif
}

static void path_in(const TAK_Stats *s, const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", s->dir, name);
}

static void day_path(const TAK_Stats *s, uint32_t day, char *out, size_t cap) {
    char name[16];
    TAK_Stats_DayText(day, name);
    strcat(name, ".txt");
    path_in(s, name, out, cap);
}

static int write_day(const TAK_Stats *s, const TAK_StatsDay *d) {
    char path[TAK_STATS_PATH_MAX + 32], tmp[TAK_STATS_PATH_MAX + 40], text[11];
    day_path(s, d->day, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    TAK_Stats_DayText(d->day, text);
    fprintf(f, FILE_MAGIC "\nday %s\nvisitors %u\nnew %u\nsessions %u\nviews %u\n"
               "batches %u\nevents %u\ndropped %u\nrefused %u\nlimited %u\ncapped %u\n",
            text, (unsigned)d->visitors, (unsigned)d->new_visitors, (unsigned)d->sessions,
            (unsigned)d->views, (unsigned)d->batches, (unsigned)d->events, (unsigned)d->dropped,
            (unsigned)d->refused, (unsigned)d->limited, (unsigned)d->capped);
    for (uint32_t i = 0; i < d->count; i++)
        fprintf(f, "%s\t%s\t%u\n", k_col[d->entry[i].dim].name, d->entry[i].value,
                (unsigned)d->entry[i].n);
    int ok = !ferror(f) && flush_file(f) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || replace_file(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

static int parse_u32(const char *t, uint32_t *out) {
    uint64_t v = 0;
    if (!*t) return 0;
    for (; *t; t++) {
        if (*t < '0' || *t > '9') return 0;
        v = v * 10 + (uint64_t)(*t - '0');
        if (v > 0xffffffffu) return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* A day file into d, as far as it makes sense. -1 when there is no
 * such file or it is not one of ours. */
static int read_day(const char *path, uint32_t day, TAK_StatsDay *d) {
    day_reset(d, day);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    if (!fgets(line, sizeof line, f) || strncmp(line, FILE_MAGIC, strlen(FILE_MAGIC)) != 0) {
        fclose(f);
        return -1;
    }
    static const struct { const char *name; size_t off; } k_scalar[] = {
        { "visitors", offsetof(TAK_StatsDay, visitors) },
        { "new",      offsetof(TAK_StatsDay, new_visitors) },
        { "sessions", offsetof(TAK_StatsDay, sessions) },
        { "views",    offsetof(TAK_StatsDay, views) },
        { "batches",  offsetof(TAK_StatsDay, batches) },
        { "events",   offsetof(TAK_StatsDay, events) },
        { "dropped",  offsetof(TAK_StatsDay, dropped) },
        { "refused",  offsetof(TAK_StatsDay, refused) },
        { "limited",  offsetof(TAK_StatsDay, limited) },
        { "capped",   offsetof(TAK_StatsDay, capped) },
    };
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        char *tab = strchr(line, '\t');
        uint32_t v;
        if (tab) {
            char *tab2 = strchr(tab + 1, '\t');
            if (!tab2) continue;
            *tab = *tab2 = '\0';
            int dim = dim_by_name(line);
            if (dim < 0 || !parse_u32(tab2 + 1, &v)) continue;
            char buf[TAK_STATS_VALUE_MAX];
            day_add(d, dim, column_value(dim, tab + 1, buf, sizeof buf), v);
        } else {
            char *sp = strchr(line, ' ');
            if (!sp) continue;
            *sp = '\0';
            if (!parse_u32(sp + 1, &v)) continue;
            for (size_t i = 0; i < sizeof k_scalar / sizeof k_scalar[0]; i++)
                if (strcmp(line, k_scalar[i].name) == 0)
                    *(uint32_t *)((char *)d + k_scalar[i].off) = v;
        }
    }
    fclose(f);
    return 0;
}

/* Today's visitors for a relay started again today, and then gone. */
static void take_seen(TAK_Stats *s, int day_was_read) {
    char path[TAK_STATS_PATH_MAX + 16];
    path_in(s, SEEN_FILE, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t head[24];
    if (day_was_read && fread(head, 1, sizeof head, f) == sizeof head &&
        memcmp(head, SEEN_MAGIC, 8) == 0 && tak_get_u32(head + 8) == s->today.day &&
        tak_get_u32(head + 12) <= SEEN_LIMIT) {
        uint32_t n = tak_get_u32(head + 12);
        s->salt = tak_get_u64(head + 16);
        uint8_t b[8];
        for (uint32_t i = 0; i < n && fread(b, 1, sizeof b, f) == sizeof b; i++) {
            uint64_t h = tak_get_u64(b);
            if (h) (void)seen_add(s, h);
        }
    }
    fclose(f);
    remove(path);
}

static void keep_seen(const TAK_Stats *s) {
    char path[TAK_STATS_PATH_MAX + 16], tmp[TAK_STATS_PATH_MAX + 24];
    path_in(s, SEEN_FILE, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint8_t head[24];
    memcpy(head, SEEN_MAGIC, 8);
    tak_put_u32(head + 8, s->today.day);
    tak_put_u32(head + 12, s->seen_count);
    tak_put_u64(head + 16, s->salt);
    int ok = fwrite(head, 1, sizeof head, f) == sizeof head;
    for (uint32_t i = 0; ok && i < TAK_STATS_VISITORS_MAX; i++) {
        if (!s->seen[i]) continue;
        uint8_t b[8];
        tak_put_u64(b, s->seen[i]);
        ok = fwrite(b, 1, sizeof b, f) == sizeof b;
    }
    ok = flush_file(f) == 0 && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok || replace_file(tmp, path) != 0) remove(tmp);
}

/* ── The day turning over ─────────────────────────────────────────────── */

int TAK_Stats_Flush(TAK_Stats *s) {
    if (!s->dir[0]) return -1;
    if (write_day(s, &s->today) != 0) {
        s->write_failures++;
        fprintf(stderr, "stats: could not write the day under %s\n", s->dir);
        return -1;
    }
    s->dirty = 0;
    return 0;
}

static void roll(TAK_Stats *s, uint64_t now_ms) {
    uint32_t day = (uint32_t)(now_ms / DAY_MS);
    if (day <= s->today.day) return;
    if (s->dirty) (void)TAK_Stats_Flush(s);
    day_reset(&s->today, day);
    s->dirty = 0;
    s->salt = random64();
    memset(s->seen, 0, sizeof s->seen);
    s->seen_count = 0;
    if (s->dir[0]) {
        char path[TAK_STATS_PATH_MAX + 16];
        path_in(s, SEEN_FILE, path, sizeof path);
        remove(path);
    }
}

void TAK_Stats_Init(TAK_Stats *s) {
    memset(s, 0, sizeof *s);
    s->salt = random64();
    s->source_key = random64();
}

int TAK_Stats_Open(TAK_Stats *s, const char *dir, uint64_t now_ms) {
    roll(s, now_ms);
    s->flushed_ms = now_ms;
    if (!dir || !dir[0] || strlen(dir) >= sizeof s->dir - 32 || make_dir(dir) != 0) return -1;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    char path[TAK_STATS_PATH_MAX + 32];
    day_path(s, s->today.day, path, sizeof path);
    int read = read_day(path, s->today.day, &s->today) == 0;
    take_seen(s, read);
    return 0;
}

/* The rate limit's tallies, hashed addresses and all, last the minute. */
static void turn_window(TAK_Stats *s, uint64_t now_ms) {
    uint64_t w = now_ms / TAK_STATS_WINDOW_MS;
    if (w == s->window) return;
    s->window = w;
    s->window_batches = 0;
    memset(s->source, 0, sizeof s->source);
}

void TAK_Stats_Tick(TAK_Stats *s, uint64_t now_ms) {
    roll(s, now_ms);
    turn_window(s, now_ms);
    if (s->dirty && s->dir[0] && now_ms - s->flushed_ms >= TAK_STATS_FLUSH_MS) {
        s->flushed_ms = now_ms;
        (void)TAK_Stats_Flush(s);
    }
}

void TAK_Stats_Close(TAK_Stats *s, uint64_t now_ms) {
    roll(s, now_ms);
    if (!s->dir[0]) return;
    if (s->dirty) (void)TAK_Stats_Flush(s);
    if (s->seen_count) keep_seen(s);
}

/* ── Who may post and who may read ────────────────────────────────────── */

int TAK_Stats_SetToken(TAK_Stats *s, const char *token) {
    s->token[0] = '\0';
    if (!token) return 0;
    while (*token == ' ' || *token == '\t' || *token == '\r' || *token == '\n') token++;
    size_t n = strlen(token);
    while (n && (token[n - 1] == ' ' || token[n - 1] == '\t' || token[n - 1] == '\r' ||
                 token[n - 1] == '\n')) n--;
    if (n == 0) return 0;
    if (n < 16 || n >= sizeof s->token) return -1;
    memcpy(s->token, token, n);
    s->token[n] = '\0';
    return 1;
}

int TAK_Stats_HasToken(const TAK_Stats *s) { return s->token[0] != '\0'; }

int TAK_Stats_AllowOrigin(TAK_Stats *s, const char *origin) {
    if (!origin || !origin[0] || s->origins >= TAK_STATS_ORIGINS_MAX) return -1;
    size_t n = strlen(origin);
    while (n && origin[n - 1] == '/') n--;
    if (n == 0 || n >= sizeof s->origin[0]) return -1;
    memcpy(s->origin[s->origins], origin, n);
    s->origin[s->origins][n] = '\0';
    s->origins++;
    return 0;
}

int TAK_Stats_KnownMod(TAK_Stats *s, const char *name) {
    if (!name || s->mods >= TAK_STATS_MODS_MAX) return -1;
    clean(name, s->mod[s->mods], sizeof s->mod[0]);
    if (s->mod[s->mods][0]) s->mods++;
    return 0;
}

/* What a mod set is counted as: vanilla, a registry name or custom. */
static const char *mod_value(const TAK_Stats *s, const char *v, char *buf, size_t cap) {
    clean(v, buf, cap);
    if (same_folded(buf, "vanilla")) return "vanilla";
    for (uint32_t i = 0; i < s->mods; i++)
        if (same_folded(buf, s->mod[i])) return s->mod[i];
    return "custom";
}

int TAK_Stats_OriginAllowed(const TAK_Stats *s, const char *origin) {
    if (s->origins == 0) return 1;
    if (!origin) return 0;
    for (uint32_t i = 0; i < s->origins; i++)
        if (strcmp(s->origin[i], origin) == 0) return 1;
    return 0;
}

/* This minute's tally for a source, the window turned over first. */
static TAK_StatsSource *source_for(TAK_Stats *s, const char *source, uint64_t now_ms) {
    uint64_t w = now_ms / TAK_STATS_WINDOW_MS;
    turn_window(s, now_ms);
    uint64_t key = keyed(s->source_key, source && source[0] ? source : "direct");
    uint32_t i = (uint32_t)(key ^ (key >> 32)) & (TAK_STATS_SOURCES - 1);
    for (uint32_t probe = 0; probe < TAK_STATS_SOURCES; probe++) {
        TAK_StatsSource *x = &s->source[i];
        if (x->key == key) return x;
        if (!x->key) { x->key = key; return x; }
        i = (i + 1) & (TAK_STATS_SOURCES - 1);
    }
    /* More sources this minute than the table holds share one tally. */
    static TAK_StatsSource crowd;
    static uint64_t crowd_window;
    if (crowd_window != w) { memset(&crowd, 0, sizeof crowd); crowd_window = w; }
    return &crowd;
}

int TAK_Stats_Authorized(TAK_Stats *s, const char *authorization, const char *source,
                         uint64_t now_ms) {
    TAK_StatsSource *src = source_for(s, source, now_ms);
    if (src->auth_failures >= TAK_STATS_AUTH_FAILURES) return -1;
    int ok = 0;
    const char *a = authorization;
    if (s->token[0] && a) {
        while (*a == ' ') a++;
        char scheme[8];
        size_t k = 0;
        while (k < 7 && a[k] && a[k] != ' ') { scheme[k] = a[k]; k++; }
        scheme[k] = 0;
        if (k == 6 && a[6] == ' ' && same_folded(scheme, "bearer")) {
            a += 6;
            while (*a == ' ') a++;
            size_t n = strlen(a);
            while (n && (a[n - 1] == ' ' || a[n - 1] == '\r' || a[n - 1] == '\n')) n--;
            size_t want = strlen(s->token);
            /* Every byte compared whatever the first difference. */
            unsigned diff = (unsigned)(n ^ want);
            for (size_t i = 0; i < want; i++)
                diff |= (unsigned char)s->token[i] ^ (unsigned char)(i < n ? a[i] : 0);
            ok = diff == 0;
        }
    }
    if (!ok) src->auth_failures++;
    return ok;
}

/* ── Reading a batch ──────────────────────────────────────────────────── */

#define FIELDS_MAX  12
#define FIELD_TEXT  128
#define KEY_MAX     16
#define DEPTH_MAX   4

typedef struct Field {
    char key[KEY_MAX];
    char text[FIELD_TEXT];    /* a number reads as its digits */
    int  is_num;
    int64_t num;
} Field;

typedef struct Event {
    int   nf;
    Field f[FIELDS_MAX];
} Event;

typedef struct Batch {
    int     version;
    int     has_ev;
    int     new_visitor;
    char    id[72];
    char    env[4][32];       /* browser, os, device, screen */
    uint32_t nev, extra;
    Event   ev[TAK_STATS_BATCH_EVENTS];
} Batch;

typedef struct Parser { const char *p, *end; } Parser;

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int peek(Parser *ps) {
    skip_ws(ps);
    return ps->p < ps->end ? (unsigned char)*ps->p : -1;
}

static int eat(Parser *ps, char c) {
    if (peek(ps) != (unsigned char)c) return 0;
    ps->p++;
    return 1;
}

static int hex4(const char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v * 16 + (uint32_t)d;
    }
    *out = v;
    return 1;
}

/* A string, decoded into out (NULL to step over it) and cut at cap.
 * Anything outside ASCII reads as one '?' a character. 0 when it is
 * not a string, a raw control byte in it included. */
static int parse_string(Parser *ps, char *out, size_t cap) {
    if (!eat(ps, '"')) return 0;
    size_t k = 0;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        uint32_t ch;
        if (c == '"') { if (out) out[k] = '\0'; return 1; }
        if (c < 0x20) return 0;
        if (c == '\\') {
            if (ps->p >= ps->end) return 0;
            char e = *ps->p++;
            switch (e) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u':
                if (ps->end - ps->p < 4 || !hex4(ps->p, &ch)) return 0;
                ps->p += 4;
                break;
            default: return 0;
            }
        } else if (c >= 0x80) {
            /* One '?' for a character, its continuation bytes skipped. */
            while (ps->p < ps->end && ((unsigned char)*ps->p & 0xc0) == 0x80) ps->p++;
            ch = '?';
        } else {
            ch = c;
        }
        if (ch >= 0x80) ch = '?';
        if (out && k + 1 < cap) out[k++] = (char)ch;
    }
    return 0;
}

/* A JSON number. Its whole part, clamped to 0..2^31-1, goes to out and
 * its text to text. 0 when it is not one. */
static int parse_number(Parser *ps, int64_t *out, char *text, size_t cap) {
    skip_ws(ps);
    const char *p = ps->p;
    int neg = 0;
    if (p < ps->end && *p == '-') { neg = 1; p++; }
    if (p >= ps->end || *p < '0' || *p > '9') return 0;
    int64_t v = 0;
    if (*p == '0') p++;
    else
        for (; p < ps->end && *p >= '0' && *p <= '9'; p++)
            if (v < 0x7fffffff) v = v * 10 + (*p - '0');
    if (p < ps->end && *p == '.') {
        p++;
        if (p >= ps->end || *p < '0' || *p > '9') return 0;
        while (p < ps->end && *p >= '0' && *p <= '9') p++;
    }
    if (p < ps->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < ps->end && (*p == '+' || *p == '-')) p++;
        if (p >= ps->end || *p < '0' || *p > '9') return 0;
        while (p < ps->end && *p >= '0' && *p <= '9') p++;
    }
    ps->p = p;
    if (v > 0x7fffffff) v = 0x7fffffff;
    if (neg) v = 0;
    *out = v;
    if (text) snprintf(text, cap, "%lld", (long long)v);
    return 1;
}

static int parse_word(Parser *ps, const char *w) {
    skip_ws(ps);
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n) != 0) return 0;
    ps->p += n;
    return 1;
}

/* Any value, stepped over. */
static int skip_value(Parser *ps, int depth) {
    int c = peek(ps);
    if (c == '"') return parse_string(ps, NULL, 0);
    if (c == '{' || c == '[') {
        if (depth >= DEPTH_MAX) return 0;
        char close = c == '{' ? '}' : ']';
        ps->p++;
        if (eat(ps, close)) return 1;
        for (;;) {
            if (close == '}') {
                if (!parse_string(ps, NULL, 0) || !eat(ps, ':')) return 0;
            }
            if (!skip_value(ps, depth + 1)) return 0;
            if (eat(ps, close)) return 1;
            if (!eat(ps, ',')) return 0;
        }
    }
    int64_t n;
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps, &n, NULL, 0);
    return parse_word(ps, "true") || parse_word(ps, "false") || parse_word(ps, "null");
}

/* One scalar into a field. A nested value is stepped over and leaves
 * the field out. */
static int parse_field(Parser *ps, Field *f, int *keep) {
    int c = peek(ps);
    *keep = 1;
    f->is_num = 0;
    f->num = 0;
    f->text[0] = '\0';
    if (c == '"') {
        char raw[FIELD_TEXT];
        if (!parse_string(ps, raw, sizeof raw)) return 0;
        clean(raw, f->text, sizeof f->text);
        return 1;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        f->is_num = 1;
        return parse_number(ps, &f->num, f->text, sizeof f->text);
    }
    if (parse_word(ps, "true")) { f->is_num = 1; f->num = 1; strcpy(f->text, "1"); return 1; }
    if (parse_word(ps, "false")) { f->is_num = 1; strcpy(f->text, "0"); return 1; }
    *keep = 0;
    if (parse_word(ps, "null")) return 1;
    return skip_value(ps, 2);
}

static int parse_event(Parser *ps, Event *e) {
    if (!eat(ps, '{')) return 0;
    if (e) e->nf = 0;
    if (eat(ps, '}')) return 1;
    for (;;) {
        char key[KEY_MAX + 8];
        Field scratch;
        if (!parse_string(ps, key, sizeof key) || !eat(ps, ':')) return 0;
        int keep = 0;
        int room = e && e->nf < FIELDS_MAX && strlen(key) < KEY_MAX;
        Field *f = room ? &e->f[e->nf] : &scratch;
        if (!parse_field(ps, f, &keep)) return 0;
        if (room && keep) {
            snprintf(f->key, sizeof f->key, "%s", key);
            e->nf++;
        }
        if (eat(ps, '}')) return 1;
        if (!eat(ps, ',')) return 0;
    }
}

static int parse_env(Parser *ps, Batch *b) {
    static const char *const k_env[4] = { "br", "os", "dev", "scr" };
    if (!eat(ps, '{')) return 0;
    if (eat(ps, '}')) return 1;
    for (;;) {
        char key[KEY_MAX + 8];
        if (!parse_string(ps, key, sizeof key) || !eat(ps, ':')) return 0;
        int which = -1;
        for (int i = 0; i < 4; i++) if (strcmp(key, k_env[i]) == 0) which = i;
        if (which >= 0 && peek(ps) == '"') {
            char raw[64];
            if (!parse_string(ps, raw, sizeof raw)) return 0;
            clean(raw, b->env[which], sizeof b->env[which]);
        } else if (!skip_value(ps, 2)) {
            return 0;
        }
        if (eat(ps, '}')) return 1;
        if (!eat(ps, ',')) return 0;
    }
}

static int parse_events(Parser *ps, Batch *b) {
    if (!eat(ps, '[')) return 0;
    b->has_ev = 1;
    if (eat(ps, ']')) return 1;
    for (;;) {
        Event *e = b->nev < TAK_STATS_BATCH_EVENTS ? &b->ev[b->nev] : NULL;
        if (!parse_event(ps, e)) return 0;
        if (e) b->nev++; else b->extra++;
        if (eat(ps, ']')) return 1;
        if (!eat(ps, ',')) return 0;
    }
}

static int id_ok(const char *id) {
    size_t n = strlen(id);
    if (n < 16 || n > 64) return 0;
    for (; *id; id++) {
        char c = *id;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int parse_batch(const char *body, size_t len, Batch *b) {
    memset(b, 0, offsetof(Batch, ev));
    Parser ps = { body, body + len };
    if (!eat(&ps, '{')) return 0;
    if (!eat(&ps, '}')) {
        for (;;) {
            char key[KEY_MAX + 8];
            if (!parse_string(&ps, key, sizeof key) || !eat(&ps, ':')) return 0;
            int64_t n;
            int ok;
            if (strcmp(key, "v") == 0) {
                ok = parse_number(&ps, &n, NULL, 0);
                b->version = (int)n;
            } else if (strcmp(key, "id") == 0) {
                ok = parse_string(&ps, b->id, sizeof b->id);
            } else if (strcmp(key, "nv") == 0) {
                Field f;
                int keep;
                ok = parse_field(&ps, &f, &keep);
                b->new_visitor = keep && f.is_num && f.num != 0;
            } else if (strcmp(key, "env") == 0) {
                ok = parse_env(&ps, b);
            } else if (strcmp(key, "ev") == 0) {
                ok = parse_events(&ps, b);
            } else {
                ok = skip_value(&ps, 1);
            }
            if (!ok) return 0;
            if (eat(&ps, '}')) break;
            if (!eat(&ps, ',')) return 0;
        }
    }
    skip_ws(&ps);
    return ps.p == ps.end && b->version == 1 && id_ok(b->id) && b->has_ev;
}

/* ── Counting a batch ─────────────────────────────────────────────────── */

static const Field *field(const Event *e, const char *key) {
    for (int i = 0; i < e->nf; i++)
        if (strcmp(e->f[i].key, key) == 0) return &e->f[i];
    return NULL;
}

static const char *text_of(const Event *e, const char *key) {
    const Field *f = field(e, key);
    return f ? f->text : NULL;
}

static void count(TAK_StatsDay *d, int dim, const char *v) {
    char buf[TAK_STATS_VALUE_MAX];
    day_add(d, dim, column_value(dim, v, buf, sizeof buf), 1);
}

static const char *length_bucket(int64_t secs) {
    static const int64_t k_upto[] = { 60, 300, 900, 1800, 3600, 7200 };
    for (int i = 0; i < 6; i++)
        if (secs < k_upto[i]) return k_length[i];
    return k_length[6];
}

static void count_start(const TAK_Stats *s, TAK_StatsDay *d, const Event *e) {
    const char *mode = allowed(k_mode, text_of(e, "mode"));
    const char *v;
    char buf[TAK_STATS_VALUE_MAX];
    day_add(d, TAK_STAT_MODE, mode, 1);
    if ((v = text_of(e, "sub")) != NULL) {
        if (strcmp(mode, "multiplayer") == 0) count(d, TAK_STAT_MP, v);
        else if (strcmp(mode, "load") == 0) count(d, TAK_STAT_LOAD, v);
    }
    if (strcmp(mode, "campaign") == 0) {
        if ((v = text_of(e, "camp")) != NULL && v[0]) count(d, TAK_STAT_CAMPAIGN, v);
        if ((v = text_of(e, "mission")) != NULL && v[0]) count(d, TAK_STAT_MISSION, v);
    } else if ((v = text_of(e, "map")) != NULL && v[0]) {
        count(d, TAK_STAT_MAP, v);
    }
    if ((v = text_of(e, "ai")) != NULL) count(d, TAK_STAT_AI, v);
    if ((v = text_of(e, "lvl")) != NULL) count(d, TAK_STAT_LEVEL, v);
    if ((v = text_of(e, "humans")) != NULL) count(d, TAK_STAT_HUMANS, v);
    const Field *rb = field(e, "rb");
    if (rb) {
        int on = rb->is_num ? rb->num != 0 : strcmp(allowed(k_onoff, rb->text), "on") == 0;
        day_add(d, TAK_STAT_REMASTERED, on ? "on" : "off", 1);
    }
    if ((v = text_of(e, "mod")) != NULL && v[0])
        day_add(d, TAK_STAT_MOD, mod_value(s, v, buf, sizeof buf), 1);
    if ((v = text_of(e, "side")) != NULL) count(d, TAK_STAT_SIDE, v);
}

static void count_end(TAK_StatsDay *d, const Event *e) {
    const char *mode = allowed(k_mode, text_of(e, "mode"));
    const char *out = allowed(k_out, text_of(e, "out"));
    const Field *f = field(e, "secs");
    int64_t secs = 0;
    if (f && f->is_num) secs = f->num;
    else if (f) secs = strtol(f->text, NULL, 10);
    if (secs < 0) secs = 0;
    if (secs > BATTLE_SECS_MAX) secs = BATTLE_SECS_MAX;
    char v[TAK_STATS_VALUE_MAX];
    snprintf(v, sizeof v, "%s:%s", mode, out);
    day_add(d, TAK_STAT_OUTCOME, v, 1);
    snprintf(v, sizeof v, "%s:%s", mode, length_bucket(secs));
    day_add(d, TAK_STAT_LENGTH, v, 1);
    day_add(d, TAK_STAT_SECS, mode, (uint64_t)secs);
}

static void count_batch(TAK_Stats *s, const Batch *b, const char *region) {
    TAK_StatsDay *d = &s->today;
    d->batches++;
    int fresh = seen_add(s, keyed(s->salt, b->id));
    if (fresh > 0) {
        d->visitors++;
        if (b->new_visitor) d->new_visitors++;
        count(d, TAK_STAT_BROWSER, b->env[0][0] ? b->env[0] : NULL);
        count(d, TAK_STAT_OS, b->env[1][0] ? b->env[1] : NULL);
        count(d, TAK_STAT_DEVICE, b->env[2][0] ? b->env[2] : NULL);
        count(d, TAK_STAT_SCREEN, b->env[3][0] ? b->env[3] : NULL);
        count(d, TAK_STAT_REGION, region);
    } else if (fresh < 0) {
        d->capped = 1;
    }
    d->dropped += b->extra;
    for (uint32_t i = 0; i < b->nev; i++) {
        const Event *e = &b->ev[i];
        const char *kind = text_of(e, "k");
        if (!kind) { d->dropped++; continue; }
        if (strcmp(kind, "view") == 0) {
            count(d, TAK_STAT_PAGE, text_of(e, "page"));
            d->views++;
            const Field *f = field(e, "s");
            if (f && f->is_num && f->num == 1) d->sessions++;
        } else if (strcmp(kind, "step") == 0) {
            count(d, TAK_STAT_STEP, text_of(e, "step"));
        } else if (strcmp(kind, "start") == 0) {
            count_start(s, d, e);
        } else if (strcmp(kind, "end") == 0) {
            count_end(d, e);
        } else if (strcmp(kind, "error") == 0) {
            const char *msg = text_of(e, "msg");
            const Field *f = field(e, "fatal");
            int fatal = f && f->is_num && f->num != 0;
            count(d, fatal ? TAK_STAT_CRASH : TAK_STAT_ERROR, msg && msg[0] ? msg : "unknown");
        } else {
            d->dropped++;
            continue;
        }
        d->events++;
    }
}

TAK_StatsResult TAK_Stats_Post(TAK_Stats *s, const char *body, size_t len,
                               const char *source, const char *region, uint64_t now_ms) {
    roll(s, now_ms);
    TAK_StatsSource *src = source_for(s, source, now_ms);
    s->dirty = 1;
    if (src->batches >= TAK_STATS_SOURCE_BATCHES || s->window_batches >= TAK_STATS_ALL_BATCHES) {
        s->today.limited++;
        return TAK_STATS_LIMITED;
    }
    src->batches++;
    s->window_batches++;
    static Batch b;
    if (!body || len == 0 || len > TAK_STATS_BODY_MAX || !parse_batch(body, len, &b)) {
        s->today.refused++;
        return TAK_STATS_MALFORMED;
    }
    if (src->events + b.nev > TAK_STATS_SOURCE_EVENTS) {
        s->today.limited++;
        return TAK_STATS_LIMITED;
    }
    src->events += b.nev;
    count_batch(s, &b, region);
    return TAK_STATS_TAKEN;
}

/* ── The report ───────────────────────────────────────────────────────── */

typedef struct Out { char *p; size_t cap, len; int over; } Out;

static void put(Out *o, const char *s) {
    size_t n = strlen(s);
    if (o->over || o->len + n >= o->cap) { o->over = 1; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void putf(Out *o, const char *fmt, ...) {
    if (o->over) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len) { o->over = 1; return; }
    o->len += (size_t)n;
}

static void put_str(Out *o, const char *s) {
    put(o, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[3] = { '\\', (char)c, 0 }; put(o, e); }
        else if (c < 0x20) putf(o, "\\u%04x", c);
        else { char e[2] = { (char)c, 0 }; put(o, e); }
    }
    put(o, "\"");
}

#define SPAN_MAX    8192u
#define SPAN_SLOTS  16384u

typedef struct SpanEntry {
    uint8_t  dim;
    char     value[TAK_STATS_VALUE_MAX];
    uint64_t n;
} SpanEntry;

typedef struct Row {
    uint32_t day, visitors, new_visitors, sessions, views, battles, ended, errors;
    uint32_t mode[7];
} Row;

static struct {
    SpanEntry e[SPAN_MAX];
    uint16_t  slot[SPAN_SLOTS];
    uint32_t  count;
    uint32_t  per_dim[TAK_STAT_DIMS];
    Row       row[TAK_STATS_DAYS_MAX];
    uint32_t  order[SPAN_MAX];
} g_span;

static TAK_StatsDay g_past;

static void span_add(int dim, const char *v, uint64_t n) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t h = key_hash(dim, v) & (SPAN_SLOTS - 1);
        while (g_span.slot[h]) {
            SpanEntry *e = &g_span.e[g_span.slot[h] - 1];
            if (e->dim == dim && strcmp(e->value, v) == 0) { e->n += n; return; }
            h = (h + 1) & (SPAN_SLOTS - 1);
        }
        int other = strcmp(v, "other") == 0;
        if ((other || g_span.per_dim[dim] < span_cap(dim)) && g_span.count < SPAN_MAX) {
            SpanEntry *e = &g_span.e[g_span.count];
            e->dim = (uint8_t)dim;
            snprintf(e->value, sizeof e->value, "%s", v);
            e->n = n;
            g_span.slot[h] = (uint16_t)(g_span.count + 1);
            g_span.count++;
            if (!other) g_span.per_dim[dim]++;
            return;
        }
        if (other) return;
        v = "other";
    }
}

static void span_day(const TAK_StatsDay *d, Row *r) {
    memset(r, 0, sizeof *r);
    r->day = d->day;
    r->visitors = d->visitors;
    r->new_visitors = d->new_visitors;
    r->sessions = d->sessions;
    r->views = d->views;
    for (uint32_t i = 0; i < d->count; i++) {
        const TAK_StatsEntry *e = &d->entry[i];
        span_add(e->dim, e->value, e->n);
        if (e->dim == TAK_STAT_MODE) {
            const char *m = allowed(k_mode, e->value);
            uint32_t k = 0;
            while (k_mode[k] && strcmp(k_mode[k], m) != 0) k++;
            r->mode[k] = sat_add(r->mode[k], e->n);
            r->battles = sat_add(r->battles, e->n);
        } else if (e->dim == TAK_STAT_OUTCOME) {
            r->ended = sat_add(r->ended, e->n);
        } else if (e->dim == TAK_STAT_ERROR || e->dim == TAK_STAT_CRASH) {
            r->errors = sat_add(r->errors, e->n);
        }
    }
}

static int by_count(const void *a, const void *b) {
    const SpanEntry *x = &g_span.e[*(const uint32_t *)a];
    const SpanEntry *y = &g_span.e[*(const uint32_t *)b];
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return strcmp(x->value, y->value);
}

static void put_column(Out *o, int dim) {
    uint32_t n = 0;
    uint64_t rest = 0;
    for (uint32_t i = 0; i < g_span.count; i++) {
        if (g_span.e[i].dim != dim) continue;
        if (strcmp(g_span.e[i].value, "other") == 0) rest += g_span.e[i].n;
        else g_span.order[n++] = i;
    }
    qsort(g_span.order, n, sizeof g_span.order[0], by_count);
    uint32_t top = k_col[dim].top ? k_col[dim].top : n;
    put_str(o, k_col[dim].name);
    put(o, ":{");
    int first = 1;
    for (uint32_t i = 0; i < n; i++) {
        const SpanEntry *e = &g_span.e[g_span.order[i]];
        if (i >= top) { rest += e->n; continue; }
        if (!first) put(o, ",");
        first = 0;
        put_str(o, e->value);
        putf(o, ":%llu", (unsigned long long)e->n);
    }
    if (rest) {
        if (!first) put(o, ",");
        putf(o, "\"other\":%llu", (unsigned long long)rest);
    }
    put(o, "}");
}

size_t TAK_Stats_ReportJson(TAK_Stats *s, uint32_t days, uint64_t now_ms, char *out, size_t cap) {
    roll(s, now_ms);
    if (days < 1) days = 1;
    if (days > TAK_STATS_DAYS_MAX) days = TAK_STATS_DAYS_MAX;
    uint32_t last = s->today.day, first = last - (days - 1);
    memset(g_span.slot, 0, sizeof g_span.slot);
    memset(g_span.per_dim, 0, sizeof g_span.per_dim);
    g_span.count = 0;
    uint64_t t[10] = { 0 };
    uint32_t capped = 0;
    for (uint32_t i = 0; i < days; i++) {
        uint32_t day = first + i;
        const TAK_StatsDay *d = &s->today;
        if (day != last) {
            char path[TAK_STATS_PATH_MAX + 32];
            day_path(s, day, path, sizeof path);
            if (!s->dir[0] || read_day(path, day, &g_past) != 0) day_reset(&g_past, day);
            d = &g_past;
        }
        span_day(d, &g_span.row[i]);
        t[0] += d->visitors; t[1] += d->new_visitors; t[2] += d->sessions; t[3] += d->views;
        t[4] += d->batches; t[5] += d->events; t[6] += d->dropped; t[7] += d->refused;
        t[8] += d->limited;
        if (d->capped) capped = 1;
    }

    Out o = { out, cap, 0, 0 };
    char a[11], b[11];
    TAK_Stats_DayText(first, a);
    TAK_Stats_DayText(last, b);
    putf(&o, "{\"from\":\"%s\",\"to\":\"%s\",\"days\":%u,\"series\":[", a, b, (unsigned)days);
    for (uint32_t i = 0; i < days; i++) {
        const Row *r = &g_span.row[i];
        TAK_Stats_DayText(r->day, a);
        putf(&o, "%s{\"day\":\"%s\",\"visitors\":%u,\"new\":%u,\"sessions\":%u,\"views\":%u,"
                 "\"battles\":%u,\"ended\":%u,\"errors\":%u,\"modes\":{",
             i ? "," : "", a, (unsigned)r->visitors, (unsigned)r->new_visitors,
             (unsigned)r->sessions, (unsigned)r->views, (unsigned)r->battles,
             (unsigned)r->ended, (unsigned)r->errors);
        int any = 0;
        for (uint32_t k = 0; k < 7; k++) {
            if (!r->mode[k]) continue;
            putf(&o, "%s\"%s\":%u", any ? "," : "", k_mode[k] ? k_mode[k] : "other",
                 (unsigned)r->mode[k]);
            any = 1;
        }
        put(&o, "}}");
    }
    putf(&o, "],\"totals\":{\"visitors\":%llu,\"new\":%llu,\"sessions\":%llu,\"views\":%llu,"
             "\"batches\":%llu,\"events\":%llu,\"dropped\":%llu,\"refused\":%llu,"
             "\"limited\":%llu,\"capped\":%u},\"counts\":{",
         (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2],
         (unsigned long long)t[3], (unsigned long long)t[4], (unsigned long long)t[5],
         (unsigned long long)t[6], (unsigned long long)t[7], (unsigned long long)t[8],
         (unsigned)capped);
    for (int dim = 0; dim < TAK_STAT_DIMS; dim++) {
        if (dim) put(&o, ",");
        put_column(&o, dim);
    }
    put(&o, "}}");
    if (o.over) return 0;
    out[o.len] = '\0';
    return o.len;
}
