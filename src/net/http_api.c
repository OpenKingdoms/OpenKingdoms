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

/* ── Request parsing ──────────────────────────────────────────────────── */

typedef struct Request {
    char method[8];
    char path[128];
    char query[128];
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

static void js_seat(Json *j, const TAK_LedgerSeat *s) {
    js_raw(j, "{\"seat\":");     js_u64(j, s->seat);
    js_raw(j, ",\"kind\":");     js_raw(j, s->kind == TAK_NSLOT_COMPUTER ? "\"computer\"" : "\"human\"");
    js_raw(j, ",\"name\":");     js_str(j, s->name);
    js_raw(j, ",\"player\":");   js_player(j, s->player_id);
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

static void js_game(Json *j, const TAK_LedgerMatch *m) {
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
    js_raw(j, "]}");
}

/* ── Routes ───────────────────────────────────────────────────────────── */

static TAK_LedgerRow g_rows[TAK_LEDGER_PLAYERS_MAX];
static char          g_body[TAK_HTTP_RESPONSE_MAX];

static int route_leaderboard(const TAK_Ledger *l, const Request *rq, Json *j) {
    uint32_t offset = query_uint(rq->query, "offset", 0, 0, 0xffffffffu);
    uint32_t limit = query_uint(rq->query, "limit", LIMIT_TABLE_DEFAULT, 1, LIMIT_TABLE_MAX);
    uint32_t n = TAK_Ledger_Table(l, g_rows, TAK_LEDGER_PLAYERS_MAX);
    js_raw(j, "{\"players\":[");
    uint32_t written = 0;
    for (uint32_t i = offset; i < n && written < limit; i++, written++) {
        if (written) js_raw(j, ",");
        js_row(j, &g_rows[i]);
    }
    js_fmt(j, "],\"total\":%u,\"offset\":%u,\"limit\":%u,\"games\":%u,"
              "\"disputed\":%u,\"version\":%u}",
           (unsigned)n, (unsigned)offset, (unsigned)limit,
           (unsigned)l->count, (unsigned)TAK_Ledger_Disputed(l), (unsigned)l->version);
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

static int route_player(const TAK_Ledger *l, const Request *rq, const char *id_text, Json *j) {
    uint64_t id;
    if (!parse_player_id(id_text, &id)) return 404;
    TAK_LedgerRow row;
    if (!TAK_Ledger_RowFor(l, id, &row)) return 404;
    uint32_t offset = query_uint(rq->query, "offset", 0, 0, 0xffffffffu);
    uint32_t limit = query_uint(rq->query, "limit", LIMIT_GAMES_DEFAULT, 1, LIMIT_GAMES_MAX);
    uint32_t ids[LIMIT_GAMES_MAX], total = 0;
    uint32_t n = TAK_Ledger_History(l, id, offset, ids, limit, &total);
    js_raw(j, "{\"player\":");
    js_row(j, &row);
    js_raw(j, ",\"games\":[");
    for (uint32_t i = 0; i < n; i++) {
        const TAK_LedgerMatch *m = TAK_Ledger_Find(l, ids[i]);
        if (!m) continue;
        if (i) js_raw(j, ",");
        js_game(j, m);
    }
    js_fmt(j, "],\"total\":%u,\"offset\":%u,\"limit\":%u}",
           (unsigned)total, (unsigned)offset, (unsigned)limit);
    return 200;
}

static int route_games(const TAK_Ledger *l, const Request *rq, Json *j) {
    uint32_t offset = query_uint(rq->query, "offset", 0, 0, 0xffffffffu);
    uint32_t limit = query_uint(rq->query, "limit", LIMIT_GAMES_DEFAULT, 1, LIMIT_GAMES_MAX);
    js_raw(j, "{\"games\":[");
    uint32_t written = 0;
    uint32_t start = l->count > offset ? l->count - offset : 0;
    for (uint32_t i = start; i > 0 && written < limit; i--, written++) {
        if (written) js_raw(j, ",");
        js_game(j, &l->match[i - 1]);
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
    js_game(j, m);
    js_raw(j, "}");
    return 200;
}

static int route_health(const TAK_Ledger *l, Json *j) {
    js_fmt(j, "{\"ok\":true,\"games\":%u,\"disputed\":%u,\"refused\":%u,\"version\":%u}",
           (unsigned)l->count, (unsigned)TAK_Ledger_Disputed(l),
           (unsigned)l->refused, (unsigned)l->version);
    return 200;
}

static int dispatch(const TAK_Ledger *l, const Request *rq, Json *j) {
    const char *p = rq->path;
    if (strcmp(p, "/api/leaderboard") == 0) return route_leaderboard(l, rq, j);
    if (strcmp(p, "/api/games") == 0) return route_games(l, rq, j);
    if (strncmp(p, "/api/games/", 11) == 0) return route_game(l, p + 11, j);
    if (strncmp(p, "/api/players/", 13) == 0) return route_player(l, rq, p + 13, j);
    if (strcmp(p, "/api/health") == 0 || strcmp(p, "/health") == 0) return route_health(l, j);
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
        code = dispatch(l, &rq, &j);
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
