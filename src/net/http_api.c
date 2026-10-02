/*
 * http_api.c -- the relay's read only JSON API.
 *
 * See tak_net_http.h for the routes. Everything is written into the
 * caller's buffer through one bounded appender, so a page that does not
 * fit comes back as a 500 rather than a cut off body.
 */

#include "tak_net_http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sized so the worst page fits TAK_HTTP_RESPONSE_MAX: a row is under
 * 400 bytes with every name escaped and every sum at its widest, and a
 * game under 4 KB with eight such seats. test_http_api holds the proof. */
#define LIMIT_TABLE_DEFAULT   100u
#define LIMIT_TABLE_MAX       200u
#define LIMIT_GAMES_DEFAULT   25u
#define LIMIT_GAMES_MAX       25u
#define LIMIT_MAPS_MAX        200u

/* ── Request parsing ──────────────────────────────────────────────────── */

typedef struct Request {
    char method[8];
    char path[128];
    char query[512];
} Request;

static void parse_request(const uint8_t *req, size_t len, Request *out) {
    memset(out, 0, sizeof(*out));
    const char *s = (const char *)req;
    size_t i = 0, k = 0;
    while (i < len && s[i] != ' ' && k + 1 < sizeof out->method) out->method[k++] = s[i++];
    while (i < len && s[i] == ' ') i++;
    k = 0;
    while (i < len && s[i] != ' ' && s[i] != '?' && s[i] != '\r' && s[i] != '\n') {
        if (k + 1 < sizeof out->path) out->path[k++] = s[i];
        i++;
    }
    if (i < len && s[i] == '?') {
        i++;
        k = 0;
        while (i < len && s[i] != ' ' && s[i] != '\r' && s[i] != '\n') {
            if (k + 1 < sizeof out->query) out->query[k++] = s[i];
            i++;
        }
    }
}

/* One numeric query field, clamped, with a default when absent. */
static uint32_t query_uint(const char *query, const char *key, uint32_t def,
                           uint32_t lo, uint32_t hi) {
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen + 1 && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            unsigned long v = strtoul(p + klen + 1, NULL, 10);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            return (uint32_t)v;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return def < lo ? lo : (def > hi ? hi : def);
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One text query field, percent decoded, "" when absent. A byte that
 * decodes to a control character is dropped. */
static void query_text(const char *query, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    out[0] = '\0';
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t k = 0;
            for (size_t i = klen + 1; i < seg && k + 1 < cap; i++) {
                int c = (unsigned char)p[i];
                if (c == '+') c = ' ';
                else if (c == '%' && i + 2 < seg && hex_digit(p[i + 1]) >= 0 &&
                         hex_digit(p[i + 2]) >= 0) {
                    c = hex_digit(p[i + 1]) * 16 + hex_digit(p[i + 2]);
                    i += 2;
                }
                if (c >= 0x20) out[k++] = (char)c;
            }
            out[k] = '\0';
            return;
        }
        if (!amp) break;
        p = amp + 1;
    }
}

/* A table id, "" when absent or when it is not one: lower case letters,
 * digits and dashes, as TAK_Ledger_TableId writes them. */
static void query_table(const char *query, char out[TAK_LEDGER_TABLE_ID_MAX]) {
    query_text(query, "table", out, TAK_LEDGER_TABLE_ID_MAX);
    for (const char *c = out; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) {
            out[0] = '\0';
            return;
        }
}

/* A number of milliseconds, 0 when absent. */
static uint64_t query_ms(const char *query, const char *key) {
    char text[24];
    query_text(query, key, text, sizeof text);
    return strtoull(text, NULL, 10);
}

/* ── JSON appender ────────────────────────────────────────────────────── */

typedef struct Json {
    char  *p;
    size_t cap, len;
    int    overflow;
} Json;

static void js_raw(Json *j, const char *s) {
    size_t n = strlen(s);
    if (j->overflow || j->len + n >= j->cap) { j->overflow = 1; return; }
    memcpy(j->p + j->len, s, n);
    j->len += n;
}

static void js_fmt(Json *j, const char *fmt, ...) {
    if (j->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->p + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= j->cap - j->len) { j->overflow = 1; return; }
    j->len += (size_t)n;
}

static void js_str(Json *j, const char *s) {
    js_raw(j, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[3] = { '\\', (char)c, 0 }; js_raw(j, e); }
        else if (c < 0x20) js_fmt(j, "\\u%04x", c);
        else { char e[2] = { (char)c, 0 }; js_raw(j, e); }
    }
    js_raw(j, "\"");
}

static void js_u64(Json *j, uint64_t v) { js_fmt(j, "%llu", (unsigned long long)v); }
static void js_i64(Json *j, int64_t v)  { js_fmt(j, "%lld", (long long)v); }

/* A player id is shown as sixteen hex digits, which a URL carries. */
static void js_player(Json *j, uint64_t id) {
    if (id == 0) { js_raw(j, "null"); return; }
    js_fmt(j, "\"%016llx\"", (unsigned long long)id);
}

static void js_row(Json *j, const TAK_LedgerRow *r) {
    js_raw(j, "{\"id\":");       js_player(j, r->player_id);
    js_raw(j, ",\"name\":");     js_str(j, r->name);
    js_raw(j, ",\"games\":");    js_u64(j, r->games);
    js_raw(j, ",\"wins\":");     js_u64(j, r->wins);
    js_raw(j, ",\"losses\":");   js_u64(j, r->losses);
    js_raw(j, ",\"score\":");    js_i64(j, r->score);
    js_raw(j, ",\"units_built\":"); js_i64(j, r->units_built);
    js_raw(j, ",\"kills\":");    js_i64(j, r->kills);
    js_raw(j, ",\"units_lost\":"); js_i64(j, r->units_lost);
    js_raw(j, ",\"time_ticks\":"); js_i64(j, r->ticks_alive);
    js_raw(j, ",\"first_played_ms\":"); js_u64(j, r->first_played_ms);
    js_raw(j, ",\"last_played_ms\":");  js_u64(j, r->last_played_ms);
    js_raw(j, "}");
}

/* ── The index ──────────────────────────────────────────────────────────
 * Everything a request needs beyond the matches it prints, worked out in
 * one pass when the ledger changes and reused until it changes again, so
 * an answer costs the rows it writes and not the whole ledger. The relay
 * answers on the thread that relays turns, so a slow answer is a pause
 * in every game. */

#define NAME_SLOTS (1u << 17)          /* twice every seat a full ledger holds */
#define MAP_SLOTS  (TAK_LEDGER_MATCHES_MAX * 2)

typedef struct MapCount { const char *name; uint32_t games; } MapCount;

static struct {
    const TAK_Ledger *ledger;
    uint64_t          stamp;
    uint32_t          builds;
    TAK_LedgerRow     rows[TAK_LEDGER_PLAYERS_MAX];   /* the table, wins first */
    uint32_t          row_count;
    uint64_t          name_id[NAME_SLOTS];            /* 0 is an empty slot */
    uint64_t          name_ms[NAME_SLOTS];
    char              name[NAME_SLOTS][TAK_NET_NAME_MAX];
    uint32_t          map_slot[MAP_SLOTS];
    MapCount          maps[TAK_LEDGER_MATCHES_MAX];   /* most played first */
    uint32_t          map_count;
    TAK_LedgerTable   tables[TAK_LEDGER_TABLES_MAX];  /* vanilla first */
    uint32_t          table_count;
} g_ix;

/* One table's rows, the last one asked for, built again when the
 * ledger or the table changes. A page reads one table at a time. */
static struct {
    uint64_t      stamp;
    uint32_t      builds;
    char          id[TAK_LEDGER_TABLE_ID_MAX];
    TAK_LedgerRow rows[TAK_LEDGER_PLAYERS_MAX];
    uint32_t      row_count;
} g_tx;

static uint32_t hash_u64(uint64_t v) { return (uint32_t)(v ^ (v >> 29) ^ (v >> 47)); }

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int map_cmp(const void *a, const void *b) {
    const MapCount *x = (const MapCount *)a, *y = (const MapCount *)b;
    if (x->games != y->games) return x->games > y->games ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void index_build(const TAK_Ledger *l) {
    g_ix.ledger = l;
    g_ix.stamp = l->stamp;
    g_ix.builds++;
    g_ix.row_count = TAK_Ledger_Table(l, g_ix.rows, TAK_LEDGER_PLAYERS_MAX);
    memset(g_ix.name_id, 0, sizeof g_ix.name_id);
    memset(g_ix.map_slot, 0xff, sizeof g_ix.map_slot);
    g_ix.map_count = 0;
    for (uint32_t i = 0; i < l->count; i++) {
        const TAK_LedgerMatch *m = &l->match[i];
        /* A player's name is the one from their newest game, later in
         * the file winning a tie, as TAK_Ledger_CurrentName has it. */
        for (int s = 0; s < m->seat_count; s++) {
            uint64_t id = m->seat[s].player_id;
            if (id == 0) continue;
            uint32_t h = hash_u64(id) & (NAME_SLOTS - 1);
            while (g_ix.name_id[h] && g_ix.name_id[h] != id) h = (h + 1) & (NAME_SLOTS - 1);
            if (!g_ix.name_id[h] || m->ended_ms >= g_ix.name_ms[h]) {
                g_ix.name_id[h] = id;
                g_ix.name_ms[h] = m->ended_ms;
                memcpy(g_ix.name[h], m->seat[s].name, TAK_NET_NAME_MAX);
                g_ix.name[h][TAK_NET_NAME_MAX - 1] = '\0';
            }
        }
        uint32_t h = hash_str(m->map_name) % MAP_SLOTS;
        while (g_ix.map_slot[h] != 0xffffffffu &&
               strcmp(g_ix.maps[g_ix.map_slot[h]].name, m->map_name) != 0)
            h = (h + 1) % MAP_SLOTS;
        if (g_ix.map_slot[h] == 0xffffffffu) {
            g_ix.map_slot[h] = g_ix.map_count;
            g_ix.maps[g_ix.map_count].name = m->map_name;
            g_ix.maps[g_ix.map_count].games = 0;
            g_ix.map_count++;
        }
        g_ix.maps[g_ix.map_slot[h]].games++;
    }
    /* The slots point into the unsorted list, and are not read again. */
    qsort(g_ix.maps, g_ix.map_count, sizeof g_ix.maps[0], map_cmp);
    g_ix.table_count = TAK_Ledger_Tables(l, g_ix.tables, TAK_LEDGER_TABLES_MAX);
}

/* The rows of one table, "" for every match. */
static const TAK_LedgerRow *table_rows(const TAK_Ledger *l, const char *table, uint32_t *count) {
    if (!table[0]) { *count = g_ix.row_count; return g_ix.rows; }
    if (!g_tx.builds || g_tx.stamp != l->stamp || strcmp(g_tx.id, table) != 0) {
        g_tx.builds++;
        g_tx.stamp = l->stamp;
        snprintf(g_tx.id, sizeof g_tx.id, "%s", table);
        g_tx.row_count = TAK_Ledger_TableIn(l, table, g_tx.rows, TAK_LEDGER_PLAYERS_MAX);
    }
    *count = g_tx.row_count;
    return g_tx.rows;
}

static const TAK_LedgerTable *find_table(const char *id) {
    for (uint32_t i = 0; i < g_ix.table_count; i++)
        if (strcmp(g_ix.tables[i].id, id) == 0) return &g_ix.tables[i];
    return NULL;
}

static void index_for(const TAK_Ledger *l) {
    if (g_ix.ledger != l || g_ix.stamp != l->stamp || !g_ix.builds) index_build(l);
}

static const char *current_name(uint64_t id) {
    uint32_t h = hash_u64(id) & (NAME_SLOTS - 1);
    while (g_ix.name_id[h]) {
        if (g_ix.name_id[h] == id) return g_ix.name[h];
        h = (h + 1) & (NAME_SLOTS - 1);
    }
    return NULL;
}

uint32_t TAK_Http_IndexBuilds(void) { return g_ix.builds; }

static void js_seat(Json *j, const TAK_LedgerSeat *s) {
    uint64_t player = s->player_id;
    js_raw(j, "{\"seat\":");     js_u64(j, s->seat);
    js_raw(j, ",\"kind\":");     js_raw(j, s->kind == TAK_NSLOT_COMPUTER ? "\"computer\"" : "\"human\"");
    js_raw(j, ",\"name\":");     js_str(j, s->name);
    js_raw(j, ",\"player\":");   js_player(j, player);
    /* The name the player goes by now, when it is not the one they
     * played this game under. */
    const char *now = player ? current_name(player) : NULL;
    if (now && strcmp(now, s->name) != 0) {
        js_raw(j, ",\"current\":");
        js_str(j, now);
    }
    js_raw(j, ",\"side\":");     js_u64(j, s->side);
    js_raw(j, ",\"colour\":");   js_u64(j, s->colour);
    js_raw(j, ",\"team\":");     js_u64(j, s->team);
    js_raw(j, ",\"standing\":"); js_raw(j, s->standing ? "true" : "false");
    js_raw(j, ",\"eliminated\":"); js_raw(j, s->eliminated ? "true" : "false");
    js_raw(j, ",\"place\":");    js_u64(j, s->place);
    js_raw(j, ",\"result\":");   js_raw(j, s->result == TAK_LEDGER_WON ? "\"won\"" : "\"lost\"");
    js_raw(j, ",\"units_built\":"); js_i64(j, s->units_built);
    js_raw(j, ",\"kills\":");    js_i64(j, s->kills);
    js_raw(j, ",\"losses\":");   js_i64(j, s->losses);
    js_raw(j, ",\"score\":");    js_i64(j, s->score);
    js_raw(j, ",\"time_ticks\":"); js_i64(j, s->last_alive_tick);
    js_raw(j, "}");
}

static void js_game(Json *j, const TAK_Ledger *l, const TAK_LedgerMatch *m) {
    js_raw(j, "{\"id\":");       js_u64(j, m->id);
    js_raw(j, ",\"map\":");      js_str(j, m->map_name);
    js_raw(j, ",\"started_ms\":"); js_u64(j, m->started_ms);
    js_raw(j, ",\"ended_ms\":");  js_u64(j, m->ended_ms);
    js_raw(j, ",\"end_tick\":");  js_u64(j, m->end_tick);
    js_raw(j, ",\"options\":");   js_u64(j, m->options);
    js_raw(j, ",\"unit_cap\":");  js_u64(j, m->unit_cap);
    js_raw(j, ",\"reports\":");   js_u64(j, m->reports);
    js_raw(j, ",\"disputed\":");  js_raw(j, m->disputed ? "true" : "false");
    js_raw(j, ",\"seats\":[");
    for (int s = 0; s < m->seat_count; s++) {
        if (s) js_raw(j, ",");
        js_seat(j, &m->seat[s]);
    }
    /* What it was filed under, after everything a page read before. */
    char table[TAK_LEDGER_TABLE_ID_MAX];
    TAK_Ledger_TableId(m, table);
    js_raw(j, "],\"table\":");    js_str(j, table);
    if (m->mod_name[0] || m->content_hash) {
        js_raw(j, ",\"mod\":");   js_str(j, m->mod_name);
        js_raw(j, ",\"mod_version\":"); js_str(j, m->mod_version);
        js_fmt(j, ",\"fingerprint\":\"%016llx\"", (unsigned long long)m->content_hash);
    }
    js_raw(j, "}");
}

/* ── Routes ───────────────────────────────────────────────────────────── */

static uint64_t g_also[TAK_LEDGER_PLAYERS_MAX];
static char     g_body[TAK_HTTP_RESPONSE_MAX];

static void js_table(Json *j, const TAK_LedgerTable *t) {
    js_raw(j, "{\"id\":");       js_str(j, t->id);
    js_raw(j, ",\"name\":");     js_str(j, t->mod_name);
    js_raw(j, ",\"version\":");  js_str(j, t->mod_version);
    if (t->content_hash) js_fmt(j, ",\"fingerprint\":\"%016llx\"", (unsigned long long)t->content_hash);
    else js_raw(j, ",\"fingerprint\":null");
    js_raw(j, ",\"vanilla\":");  js_raw(j, t->vanilla ? "true" : "false");
    js_raw(j, ",\"earlier\":");  js_raw(j, strcmp(t->id, TAK_LEDGER_TABLE_EARLIER) == 0 ? "true" : "false");
    js_fmt(j, ",\"games\":%u,\"disputed\":%u,\"last_played_ms\":",
           (unsigned)t->games, (unsigned)t->disputed);
    js_u64(j, t->last_played_ms);
    js_raw(j, "}");
}

static int route_leaderboard(const TAK_Ledger *l, const Request *rq, Json *j) {
    uint32_t offset = query_uint(rq->query, "offset", 0, 0, 0xffffffffu);
    uint32_t limit = query_uint(rq->query, "limit", LIMIT_TABLE_DEFAULT, 1, LIMIT_TABLE_MAX);
    char q[TAK_NET_NAME_MAX], table[TAK_LEDGER_TABLE_ID_MAX];
    query_text(rq->query, "q", q, sizeof q);
    query_table(rq->query, table);
    uint32_t count = 0;
    const TAK_LedgerRow *rows = table_rows(l, table, &count);
    js_raw(j, "{\"players\":[");
    uint32_t n = 0, written = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (q[0] && !TAK_Ledger_Holds(rows[i].name, q)) continue;
        if (n++ < offset || written >= limit) continue;
        if (written++) js_raw(j, ",");
        js_row(j, &rows[i]);
    }
    uint32_t games = l->count, disputed = TAK_Ledger_Disputed(l);
    if (table[0]) {
        const TAK_LedgerTable *t = find_table(table);
        games = t ? t->games : 0;
        disputed = t ? t->disputed : 0;
    }
    js_fmt(j, "],\"total\":%u,\"offset\":%u,\"limit\":%u,\"games\":%u,"
              "\"disputed\":%u,\"version\":%u",
           (unsigned)n, (unsigned)offset, (unsigned)limit,
           (unsigned)games, (unsigned)disputed, (unsigned)l->version);
    /* Asked for one table, the answer says which. Asked for none, it
     * reads as it did before tables. */
    if (table[0]) {
        js_raw(j, ",\"table\":");
        js_str(j, table);
    }
    js_raw(j, "}");
    return 200;
}

/* Every table, vanilla first, and the one a page opens on: the vanilla
 * table most played, or the first there is. */
static int route_tables(Json *j) {
    const char *def = NULL;
    uint32_t best = 0;
    for (uint32_t i = 0; i < g_ix.table_count; i++) {
        const TAK_LedgerTable *t = &g_ix.tables[i];
        if (t->vanilla && t->games > best) { best = t->games; def = t->id; }
    }
    if (!def && g_ix.table_count) def = g_ix.tables[0].id;
    js_raw(j, "{\"tables\":[");
    for (uint32_t i = 0; i < g_ix.table_count; i++) {
        if (i) js_raw(j, ",");
        js_table(j, &g_ix.tables[i]);
    }
    js_raw(j, "],\"default\":");
    if (def) js_str(j, def); else js_raw(j, "null");
    js_raw(j, "}");
    return 200;
}

static int parse_player_id(const char *hex, uint64_t *out) {
    if (strlen(hex) != 16) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = hex[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return v != 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* The filter a games list was asked for: a player's name as typed in
 * the game or as they go by now, a map, and a span of end times. */
typedef struct GameQuery {
    TAK_LedgerFilter f;
    char name[TAK_NET_NAME_MAX];
    char map[TAK_NET_MAP_NAME_MAX];
    char table[TAK_LEDGER_TABLE_ID_MAX];
    int  any;
} GameQuery;

static void game_query(const Request *rq, GameQuery *g) {
    memset(g, 0, sizeof *g);
    query_text(rq->query, "q", g->name, sizeof g->name);
    query_text(rq->query, "map", g->map, sizeof g->map);
    g->f.from_ms = query_ms(rq->query, "from");
    g->f.to_ms = query_ms(rq->query, "to");
    g->f.map = g->map;
    query_table(rq->query, g->table);
    g->f.table = g->table;
    if (g->name[0]) {
        g->f.name = g->name;
        uint32_t n = 0;
        for (uint32_t i = 0; i < g_ix.row_count; i++)
            if (TAK_Ledger_Holds(g_ix.rows[i].name, g->name)) g_also[n++] = g_ix.rows[i].player_id;
        qsort(g_also, n, sizeof g_also[0], cmp_u64);
        g->f.also = g_also;
        g->f.also_count = n;
    }
    g->any = g->name[0] || g->map[0] || g->table[0] || g->f.from_ms || g->f.to_ms;
}

static int route_player(const TAK_Ledger *l, const Request *rq, const char *id_text, Json *j) {
    uint64_t id;
    if (!parse_player_id(id_text, &id)) return 404;
    TAK_LedgerRow row;
    if (!TAK_Ledger_RowFor(l, id, &row)) return 404;
    static GameQuery g;
    game_query(rq, &g);
    /* Within one table a player's sums are that table's, under the name
     * they go by anywhere. */
    if (g.table[0]) {
        char name[TAK_NET_NAME_MAX];
        memcpy(name, row.name, sizeof name);
        (void)TAK_Ledger_RowIn(l, g.table, id, &row);
        memcpy(row.name, name, sizeof name);
    }
    uint32_t offset = query_uint(rq->query, "offset", 0, 0, 0xffffffffu);
    uint32_t limit = query_uint(rq->query, "limit", LIMIT_GAMES_DEFAULT, 1, LIMIT_GAMES_MAX);
    uint32_t ids[LIMIT_GAMES_MAX], total = 0;
    g.f.player = id;
    uint32_t n = TAK_Ledger_Games(l, &g.f, offset, ids, limit, &total);
    js_raw(j, "{\"player\":");
    js_row(j, &row);
    js_raw(j, ",\"games\":[");
    for (uint32_t i = 0; i < n; i++) {
        const TAK_LedgerMatch *m = TAK_Ledger_Find(l, ids[i]);
        if (!m) continue;
        if (i) js_raw(j, ",");
        js_game(j, l, m);
    }
    js_fmt(j, "],\"total\":%u,\"offset\":%u,\"limit\":%u}",
           (unsigned)total, (unsigned)offset, (unsigned)limit);
    return 200;
}

static int route_games(const TAK_Ledger *l, const Request *rq, Json *j) {
    uint32_t offset = query_uint(rq->query, "offset", 0, 0, 0xffffffffu);
    uint32_t limit = query_uint(rq->query, "limit", LIMIT_GAMES_DEFAULT, 1, LIMIT_GAMES_MAX);
    static GameQuery g;
    game_query(rq, &g);
    if (g.any) {
        uint32_t ids[LIMIT_GAMES_MAX], total = 0;
        uint32_t n = TAK_Ledger_Games(l, &g.f, offset, ids, limit, &total);
        js_raw(j, "{\"games\":[");
        for (uint32_t i = 0; i < n; i++) {
            if (i) js_raw(j, ",");
            js_game(j, l, TAK_Ledger_Find(l, ids[i]));
        }
        js_fmt(j, "],\"total\":%u,\"offset\":%u,\"limit\":%u}",
               (unsigned)total, (unsigned)offset, (unsigned)limit);
        return 200;
    }
    js_raw(j, "{\"games\":[");
    uint32_t written = 0;
    uint32_t start = l->count > offset ? l->count - offset : 0;
    for (uint32_t i = start; i > 0 && written < limit; i--, written++) {
        if (written) js_raw(j, ",");
        js_game(j, l, &l->match[i - 1]);
    }
    js_fmt(j, "],\"total\":%u,\"offset\":%u,\"limit\":%u}",
           (unsigned)l->count, (unsigned)offset, (unsigned)limit);
    return 200;
}

static int route_game(const TAK_Ledger *l, const char *id_text, Json *j) {
    char *end = NULL;
    unsigned long id = strtoul(id_text, &end, 10);
    if (!id_text[0] || !end || *end) return 404;
    const TAK_LedgerMatch *m = TAK_Ledger_Find(l, (uint32_t)id);
    if (!m) return 404;
    js_raw(j, "{\"game\":");
    js_game(j, l, m);
    js_raw(j, "}");
    return 200;
}

/* Every map a recorded game was played on, most played first, for the
 * page's map filter. */
static int route_maps(Json *j) {
    uint32_t n = g_ix.map_count;
    js_raw(j, "{\"maps\":[");
    for (uint32_t i = 0; i < n && i < LIMIT_MAPS_MAX; i++) {
        if (i) js_raw(j, ",");
        js_raw(j, "{\"name\":");
        js_str(j, g_ix.maps[i].name);
        js_fmt(j, ",\"games\":%u}", (unsigned)g_ix.maps[i].games);
    }
    js_fmt(j, "],\"total\":%u}", (unsigned)n);
    return 200;
}

static int route_health(const TAK_Ledger *l, const TAK_HttpLive *live, Json *j) {
    js_fmt(j, "{\"ok\":true,\"games\":%u,\"disputed\":%u,\"refused\":%u,\"version\":%u",
           (unsigned)l->count, (unsigned)TAK_Ledger_Disputed(l),
           (unsigned)l->refused, (unsigned)l->version);
    if (live) js_fmt(j, ",\"online\":%u", (unsigned)live->online);
    js_raw(j, "}");
    return 200;
}

static const char *room_status(uint8_t s) {
    switch (s) {
    case TAK_ROOM_OPEN:        return "open";
    case TAK_ROOM_IN_PROGRESS: return "playing";
    case TAK_ROOM_ENDED:       return "ended";
    default:                   return "starting";
    }
}

static int route_rooms(const TAK_HttpLive *live, Json *j) {
    if (!live) return 404;
    js_fmt(j, "{\"online\":%u,\"in_lobby\":%u,\"rooms\":[",
           (unsigned)live->online, (unsigned)live->in_lobby);
    uint32_t n = live->count < TAK_HTTP_LIVE_ROOMS ? live->count : TAK_HTTP_LIVE_ROOMS;
    for (uint32_t i = 0; i < n; i++) {
        const TAK_HttpLiveRoom *x = &live->room[i];
        const TAK_RoomSummary *s = &x->room;
        if (i) js_raw(j, ",");
        js_raw(j, "{\"code\":");
        js_str(j, s->code);
        js_raw(j, ",\"name\":");
        js_str(j, s->name);
        js_raw(j, ",\"host\":");
        js_str(j, s->host_name);
        js_raw(j, ",\"map\":");
        js_str(j, s->map_name);
        js_fmt(j, ",\"players\":%u,\"max\":%u,\"watchers\":%u,\"status\":\"%s\"",
               (unsigned)s->players, (unsigned)s->max_players, (unsigned)s->watchers,
               room_status(s->status));
        js_fmt(j, ",\"password\":%s,\"watchable\":%s,\"iron_plague\":%s",
               (s->flags & TAK_ROOMF_PASSWORD) ? "true" : "false",
               (s->flags & TAK_ROOMF_ALLOW_WATCHING) ? "true" : "false",
               (s->flags & TAK_ROOMF_IRON_PLAGUE) ? "true" : "false");
        js_fmt(j, ",\"build\":%u,\"ping\":%u,\"playing_secs\":%u",
               (unsigned)s->engine_build_id, (unsigned)x->host_ping_ms,
               (unsigned)x->playing_secs);
        /* A host from before protocol 4 names no mod set. */
        if (s->mod_name[0]) {
            js_raw(j, ",\"mod\":");
            js_str(j, s->mod_name);
            js_raw(j, ",\"mod_version\":");
            js_str(j, s->mod_version);
        }
        /* Only when true, so every other row reads as it did. */
        if (x->drop_in) js_raw(j, ",\"drop_in\":true");
        js_raw(j, "}");
    }
    js_raw(j, "]}");
    return 200;
}

static int dispatch(const TAK_Ledger *l, const TAK_HttpLive *live,
                    const Request *rq, Json *j) {
    const char *p = rq->path;
    if (strncmp(p, "/api/", 5) == 0 && strcmp(p, "/api/rooms") != 0 &&
        strcmp(p, "/api/health") != 0) index_for(l);
    if (strcmp(p, "/api/rooms") == 0) return route_rooms(live, j);
    if (strcmp(p, "/api/leaderboard") == 0) return route_leaderboard(l, rq, j);
    if (strcmp(p, "/api/games") == 0) return route_games(l, rq, j);
    if (strcmp(p, "/api/maps") == 0) return route_maps(j);
    if (strcmp(p, "/api/tables") == 0) return route_tables(j);
    if (strncmp(p, "/api/games/", 11) == 0) return route_game(l, p + 11, j);
    if (strncmp(p, "/api/players/", 13) == 0) return route_player(l, rq, p + 13, j);
    if (strcmp(p, "/api/health") == 0 || strcmp(p, "/health") == 0) return route_health(l, live, j);
    return 404;
}

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default:  return "Internal Server Error";
    }
}

size_t TAK_Http_Answer(const TAK_Ledger *l, const uint8_t *req, size_t len,
                       char *out, size_t cap) {
    return TAK_Http_AnswerLive(l, NULL, req, len, out, cap);
}

size_t TAK_Http_AnswerLive(const TAK_Ledger *l, const TAK_HttpLive *live,
                           const uint8_t *req, size_t len, char *out, size_t cap) {
    Request rq;
    parse_request(req, len, &rq);
    Json j = { g_body, sizeof g_body, 0, 0 };
    int code;
    int head_only = 0;
    if (strcmp(rq.method, "OPTIONS") == 0) {
        code = 204;
        head_only = 1;
    } else if (strcmp(rq.method, "GET") == 0 || strcmp(rq.method, "HEAD") == 0) {
        head_only = rq.method[0] == 'H';
        code = dispatch(l, live, &rq, &j);
        if (code != 200) { j.len = 0; j.overflow = 0; }
        if (code == 404) js_raw(&j, "{\"error\":\"not found\"}");
        if (j.overflow) {
            code = 500;
            j.len = 0; j.overflow = 0;
            js_raw(&j, "{\"error\":\"the page is too large, ask for fewer rows\"}");
        }
    } else {
        code = 405;
        js_raw(&j, "{\"error\":\"read only\"}");
    }
    g_body[j.len] = '\0';

    int n = snprintf(out, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status_text(code), (unsigned)(head_only ? 0 : j.len));
    if (n < 0 || (size_t)n >= cap) return 0;
    if (head_only || code == 204) return (size_t)n;
    if ((size_t)n + j.len > cap) return 0;
    memcpy(out + n, g_body, j.len);
    return (size_t)n + j.len;
}
