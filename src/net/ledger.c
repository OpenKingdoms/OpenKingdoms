/*
 * ledger.c -- every finished match, and the sums the leaderboard shows.
 *
 * See tak_net_ledger.h for the shape. The file is a header and then
 * records back to back, each a tag, a length, a payload and a checksum
 * over all three, written with the bounded writer the protocol uses.
 * The checksum is what lets a reader step over a corrupt record and
 * find the next one: a length byte gone wrong fails the check, and the
 * reader walks forward a byte at a time until a record checks again.
 */

#include "tak_net_ledger.h"
#include "tak_bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define HEADER_LEN   12u     /* magic, u16 version, u16 reserved */
#define RECORD_HEAD  3u      /* u8 tag, u16 length */
#define RECORD_TAIL  4u      /* u32 checksum */
#define RECORD_MAX   4096u
#define PAYLOAD_MAX  (RECORD_MAX - RECORD_HEAD - RECORD_TAIL)

/* ── Identity ─────────────────────────────────────────────────────────── */

uint64_t TAK_Ledger_PlayerId(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    size_t a = 0, b = n;
    while (a < b && (name[a] == ' ' || name[a] == '\t')) a++;
    while (b > a && (name[b - 1] == ' ' || name[b - 1] == '\t')) b--;
    if (a == b) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = a; i < b; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h ^= c;
        h *= 1099511628211ull;
    }
    return h ? h : 1u;
}

/* ── Placing ──────────────────────────────────────────────────────────── */

/* Later is better, and standing beats any fall. */
static int seat_better(const TAK_LedgerSeat *a, const TAK_LedgerSeat *b) {
    if (a->standing != b->standing) return a->standing > b->standing;
    if (a->standing) return 0;
    return a->last_alive_tick > b->last_alive_tick;
}

void TAK_Ledger_Place(TAK_LedgerMatch *m) {
    for (int i = 0; i < m->seat_count; i++) {
        int better = 0;
        for (int j = 0; j < m->seat_count; j++)
            if (j != i && seat_better(&m->seat[j], &m->seat[i])) better++;
        m->seat[i].place = (uint8_t)(better + 1);
        m->seat[i].result = m->seat[i].standing ? TAK_LEDGER_WON : TAK_LEDGER_LOST;
    }
}

int TAK_Ledger_SameTallies(const TAK_LedgerMatch *a, const TAK_LedgerMatch *b) {
    if (a->seat_count != b->seat_count || a->end_tick != b->end_tick) return 0;
    for (int i = 0; i < a->seat_count; i++) {
        const TAK_LedgerSeat *x = &a->seat[i], *y = &b->seat[i];
        if (x->seat != y->seat || x->standing != y->standing ||
            x->eliminated != y->eliminated ||
            x->units_built != y->units_built || x->kills != y->kills ||
            x->losses != y->losses || x->score != y->score ||
            x->last_alive_tick != y->last_alive_tick) return 0;
    }
    return 1;
}

/* ── Records ──────────────────────────────────────────────────────────── */

static uint32_t crc_table[256];

static uint32_t crc32(const uint8_t *p, size_t n) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* Fill in the length and append the checksum. Returns the whole size. */
static size_t seal(uint8_t *rec, size_t len, size_t cap) {
    if (len + RECORD_TAIL > cap || len - RECORD_HEAD > PAYLOAD_MAX) return 0;
    tak_put_u16(rec + 1, (uint16_t)(len - RECORD_HEAD));
    tak_put_u32(rec + len, crc32(rec, len));
    return len + RECORD_TAIL;
}

/* A match with no device seat keeps the first format, so a ledger from
 * before devices is rewritten byte for byte as it was. */
static int has_device_seat(const TAK_LedgerMatch *m) {
    for (int i = 0; i < m->seat_count; i++)
        if (m->seat[i].ident != TAK_LEDGER_IDENT_NAME) return 1;
    return 0;
}

size_t TAK_Ledger_EncodeMatch(const TAK_LedgerMatch *m, void *out, size_t cap) {
    if (m->seat_count > TAK_NET_SEATS) return 0;
    int device = has_device_seat(m);
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, cap);
    TAK_BW_U8(&w, device ? TAK_LEDGER_TAG_MATCH_DEVICE : TAK_LEDGER_TAG_MATCH);
    TAK_BW_U16(&w, 0);
    TAK_BW_U32(&w, m->id);
    TAK_BW_U32(&w, m->relay_match_id);
    TAK_BW_U64(&w, m->started_ms);
    TAK_BW_U64(&w, m->ended_ms);
    TAK_BW_U32(&w, m->end_tick);
    TAK_BW_U32(&w, m->options);
    TAK_BW_U16(&w, m->unit_cap);
    TAK_BW_U8(&w, m->stats_version);
    TAK_BW_U8(&w, m->reports);
    TAK_BW_U8(&w, m->disputed);
    TAK_BW_Str(&w, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BW_Bytes(&w, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    TAK_BW_U8(&w, m->seat_count);
    for (int i = 0; i < m->seat_count; i++) {
        const TAK_LedgerSeat *s = &m->seat[i];
        TAK_BW_U8(&w, s->seat);
        TAK_BW_U8(&w, s->kind);
        TAK_BW_U8(&w, s->side);
        TAK_BW_U8(&w, s->colour);
        TAK_BW_U8(&w, s->team);
        TAK_BW_U8(&w, s->standing);
        TAK_BW_U8(&w, s->eliminated);
        TAK_BW_U8(&w, s->place);
        TAK_BW_U8(&w, s->result);
        TAK_BW_U64(&w, s->player_id);
        TAK_BW_Str(&w, s->name, TAK_NET_NAME_MAX);
        TAK_BW_I32(&w, s->units_built);
        TAK_BW_I32(&w, s->kills);
        TAK_BW_I32(&w, s->losses);
        TAK_BW_I32(&w, s->score);
        TAK_BW_I32(&w, s->last_alive_tick);
        if (device) TAK_BW_U8(&w, s->ident);
    }
    if (!TAK_BW_Ok(&w)) return 0;
    return seal(w.data, w.len, cap);
}

int TAK_Ledger_DecodeMatch(TAK_LedgerMatch *m, const void *p, size_t len) {
    return TAK_Ledger_DecodeMatchTag(m, TAK_LEDGER_TAG_MATCH, p, len);
}

int TAK_Ledger_DecodeMatchTag(TAK_LedgerMatch *m, uint8_t tag, const void *p, size_t len) {
    int device = tag == TAK_LEDGER_TAG_MATCH_DEVICE;
    if (!device && tag != TAK_LEDGER_TAG_MATCH) return -1;
    TAK_ByteReader r;
    memset(m, 0, sizeof(*m));
    TAK_BR_Init(&r, p, len);
    m->id = TAK_BR_U32(&r);
    m->relay_match_id = TAK_BR_U32(&r);
    m->started_ms = TAK_BR_U64(&r);
    m->ended_ms = TAK_BR_U64(&r);
    m->end_tick = TAK_BR_U32(&r);
    m->options = TAK_BR_U32(&r);
    m->unit_cap = TAK_BR_U16(&r);
    m->stats_version = TAK_BR_U8(&r);
    m->reports = TAK_BR_U8(&r);
    m->disputed = TAK_BR_U8(&r);
    TAK_BR_Str(&r, m->map_name, TAK_NET_MAP_NAME_MAX);
    TAK_BR_Bytes(&r, m->map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    m->seat_count = TAK_BR_U8(&r);
    if (m->seat_count > TAK_NET_SEATS) return -1;
    for (int i = 0; i < m->seat_count; i++) {
        TAK_LedgerSeat *s = &m->seat[i];
        s->seat = TAK_BR_U8(&r);
        s->kind = TAK_BR_U8(&r);
        s->side = TAK_BR_U8(&r);
        s->colour = TAK_BR_U8(&r);
        s->team = TAK_BR_U8(&r);
        s->standing = TAK_BR_U8(&r);
        s->eliminated = TAK_BR_U8(&r);
        s->place = TAK_BR_U8(&r);
        s->result = TAK_BR_U8(&r);
        s->player_id = TAK_BR_U64(&r);
        TAK_BR_Str(&r, s->name, TAK_NET_NAME_MAX);
        s->units_built = TAK_BR_I32(&r);
        s->kills = TAK_BR_I32(&r);
        s->losses = TAK_BR_I32(&r);
        s->score = TAK_BR_I32(&r);
        s->last_alive_tick = TAK_BR_I32(&r);
        if (device) {
            s->ident = TAK_BR_U8(&r);
            if (s->ident > TAK_LEDGER_IDENT_DEVICE) return -1;
        }
    }
    return TAK_BR_Done(&r) ? 0 : -1;
}

static size_t encode_confirm(uint32_t id, int agrees, void *out, size_t cap) {
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, cap);
    TAK_BW_U8(&w, TAK_LEDGER_TAG_CONFIRM);
    TAK_BW_U16(&w, 0);
    TAK_BW_U32(&w, id);
    TAK_BW_U8(&w, (uint8_t)(agrees ? 1 : 0));
    if (!TAK_BW_Ok(&w)) return 0;
    return seal(w.data, w.len, cap);
}

static size_t encode_claim(const TAK_LedgerClaim *c, void *out, size_t cap) {
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, cap);
    TAK_BW_U8(&w, TAK_LEDGER_TAG_CLAIM);
    TAK_BW_U16(&w, 0);
    TAK_BW_U64(&w, c->name_id);
    TAK_BW_U64(&w, c->player_id);
    if (!TAK_BW_Ok(&w)) return 0;
    return seal(w.data, w.len, cap);
}

/* ── In memory ────────────────────────────────────────────────────────── */

void TAK_Ledger_Init(TAK_Ledger *l) {
    memset(l, 0, sizeof(*l));
    l->next_id = 1;
}

static TAK_LedgerMatch *find_mut(TAK_Ledger *l, uint32_t id) {
    /* Ids rise with the index, so a record sits at or below its id. */
    uint32_t i = id < l->count ? id : l->count;
    while (i > 0) {
        TAK_LedgerMatch *m = &l->match[i - 1];
        if (m->id == id) return m;
        if (m->id < id) break;
        i--;
    }
    return NULL;
}

const TAK_LedgerMatch *TAK_Ledger_Find(const TAK_Ledger *l, uint32_t id) {
    return find_mut((TAK_Ledger *)l, id);
}

static int take_match(TAK_Ledger *l, const TAK_LedgerMatch *m) {
    if (l->count >= TAK_LEDGER_MATCHES_MAX) { l->refused++; return -1; }
    if (m->id == 0 || m->id < l->next_id) return -1;
    l->match[l->count++] = *m;
    for (int i = 0; i < m->seat_count; i++)
        if (m->seat[i].ident == TAK_LEDGER_IDENT_NAME && m->seat[i].player_id) l->legacy_end = l->count;
    l->next_id = m->id + 1;
    l->version++;
    return 0;
}

uint64_t TAK_Ledger_ClaimOf(const TAK_Ledger *l, uint64_t name_id) {
    if (name_id == 0) return 0;
    for (uint32_t i = 0; i < l->claims; i++)
        if (l->claim[i].name_id == name_id) return l->claim[i].player_id;
    return 0;
}

/* The first claim on a name stands. */
static int take_claim(TAK_Ledger *l, const TAK_LedgerClaim *c) {
    if (c->name_id == 0 || c->player_id == 0) return -1;
    if (TAK_Ledger_ClaimOf(l, c->name_id)) return -1;
    if (l->claims >= TAK_LEDGER_PLAYERS_MAX) return -1;
    l->claim[l->claims++] = *c;
    l->version++;
    return 0;
}

uint64_t TAK_Ledger_SeatPlayer(const TAK_Ledger *l, const TAK_LedgerSeat *s) {
    if (s->player_id == 0 || s->ident != TAK_LEDGER_IDENT_NAME) return s->player_id;
    uint64_t owner = TAK_Ledger_ClaimOf(l, s->player_id);
    return owner ? owner : s->player_id;
}

static int take_confirm(TAK_Ledger *l, uint32_t id, int agrees) {
    TAK_LedgerMatch *m = find_mut(l, id);
    if (!m) return -1;
    if (agrees) { if (m->reports < 255) m->reports++; }
    else m->disputed = 1;
    l->version++;
    return 0;
}

/* One record's worth of bytes, checked. Returns its whole size, or 0
 * when there is no sound record at `p`. `short_ok` says a record that
 * runs past `len` may still be arriving rather than being garbage. */
static size_t record_at(const uint8_t *p, size_t len, int short_ok, int *wait) {
    *wait = 0;
    if (len < RECORD_HEAD) { *wait = short_ok; return 0; }
    size_t n = tak_get_u16(p + 1);
    if (n > PAYLOAD_MAX) return 0;
    size_t whole = RECORD_HEAD + n + RECORD_TAIL;
    if (whole > len) { *wait = short_ok; return 0; }
    if (crc32(p, RECORD_HEAD + n) != tak_get_u32(p + RECORD_HEAD + n)) return 0;
    return whole;
}

size_t TAK_Ledger_Load(TAK_Ledger *l, const void *bytes, size_t len, int more) {
    const uint8_t *p = (const uint8_t *)bytes;
    size_t at = 0;
    while (at < len) {
        int wait = 0;
        size_t whole = record_at(p + at, len - at, more, &wait);
        if (wait) break;
        if (whole == 0) { at++; l->bad_bytes++; continue; }
        uint8_t tag = p[at];
        const uint8_t *body = p + at + RECORD_HEAD;
        size_t n = whole - RECORD_HEAD - RECORD_TAIL;
        if (tag == TAK_LEDGER_TAG_MATCH || tag == TAK_LEDGER_TAG_MATCH_DEVICE) {
            TAK_LedgerMatch m;
            if (TAK_Ledger_DecodeMatchTag(&m, tag, body, n) != 0 || take_match(l, &m) != 0)
                l->bad_records++;
        } else if (tag == TAK_LEDGER_TAG_CLAIM) {
            TAK_LedgerClaim c;
            if (n == 16) { c.name_id = tak_get_u64(body); c.player_id = tak_get_u64(body + 8); }
            if (n != 16 || take_claim(l, &c) != 0) l->bad_records++;
        } else if (tag == TAK_LEDGER_TAG_CONFIRM) {
            if (n != 5 || take_confirm(l, tak_get_u32(body), body[4] != 0) != 0)
                l->bad_records++;
        } else {
            l->bad_records++;           /* newer than us: skipped by length */
        }
        at += whole;
    }
    return at;
}

/* ── The file ─────────────────────────────────────────────────────────── */

static void write_header(FILE *f) {
    uint8_t h[HEADER_LEN];
    memcpy(h, TAK_LEDGER_FILE_MAGIC, 8);
    tak_put_u16(h + 8, TAK_LEDGER_FILE_VERSION);
    tak_put_u16(h + 10, 0);
    fwrite(h, 1, sizeof h, f);
}

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
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/* Write every record again into a file beside the old one, and swap
 * them only once the new one is whole. The old file is never cut. */
static int rewrite(TAK_Ledger *l) {
    char tmp[TAK_LEDGER_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", l->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    write_header(f);
    uint8_t rec[RECORD_MAX];
    int ok = 1;
    /* Claims first: they only name players, so where they sit among the
     * matches does not change what the ledger reads as. */
    for (uint32_t i = 0; i < l->claims && ok; i++) {
        size_t n = encode_claim(&l->claim[i], rec, sizeof rec);
        ok = n && fwrite(rec, 1, n, f) == n;
    }
    for (uint32_t i = 0; i < l->count && ok; i++) {
        size_t n = TAK_Ledger_EncodeMatch(&l->match[i], rec, sizeof rec);
        ok = n && fwrite(rec, 1, n, f) == n;
    }
    ok = ok && flush_file(f) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || replace_file(tmp, l->path) != 0) { remove(tmp); return -1; }
    return 0;
}

int TAK_Ledger_Open(TAK_Ledger *l, const char *path) {
    TAK_Ledger_Init(l);
    if (!path || !path[0] || strlen(path) >= sizeof l->path) return -1;
    strcpy(l->path, path);

    int fresh = 1;
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t head[HEADER_LEN];
        size_t got = fread(head, 1, sizeof head, f);
        if (got > 0) {
            fresh = 0;
            if (got != sizeof head || memcmp(head, TAK_LEDGER_FILE_MAGIC, 8) != 0 ||
                tak_get_u16(head + 8) != TAK_LEDGER_FILE_VERSION) {
                fclose(f);
                return -1;              /* not ours, leave it alone */
            }
            static uint8_t buf[RECORD_MAX * 16];
            size_t held = 0;
            for (;;) {
                size_t want = sizeof buf - held;
                size_t read = fread(buf + held, 1, want, f);
                int more = read == want;
                size_t have = held + read;
                size_t used = TAK_Ledger_Load(l, buf, have, more);
                memmove(buf, buf + used, have - used);
                held = have - used;
                if (!more) break;
            }
        }
        fclose(f);
        /* Bytes that were not records are dropped from the file too. */
        if (!fresh && l->bad_bytes && rewrite(l) != 0) return -1;
    }
    if (fresh) {
        /* Nothing, or an empty file, is a new ledger. */
        f = fopen(path, "wb");
        if (!f) return -1;
        write_header(f);
        int ok = flush_file(f) == 0;
        ok = (fclose(f) == 0) && ok;
        if (!ok) return -1;
    }
    f = fopen(path, "ab");
    if (!f) return -1;
    l->file = f;
    return 0;
}

void TAK_Ledger_Close(TAK_Ledger *l) {
    if (l->file) fclose((FILE *)l->file);
    l->file = NULL;
}

static void append(TAK_Ledger *l, const uint8_t *rec, size_t n) {
    if (!l->file || n == 0) return;
    FILE *f = (FILE *)l->file;
    if (fwrite(rec, 1, n, f) != n || flush_file(f) != 0) {
        l->write_failures++;
        fprintf(stderr, "ledger: could not write to %s\n", l->path);
    }
}

/* Whether any match holds a seat keyed by this typed name. */
static int name_has_records(const TAK_Ledger *l, uint64_t name_id) {
    for (uint32_t i = 0; i < l->legacy_end; i++) {
        const TAK_LedgerMatch *m = &l->match[i];
        for (int s = 0; s < m->seat_count; s++)
            if (m->seat[s].ident == TAK_LEDGER_IDENT_NAME && m->seat[s].player_id == name_id)
                return 1;
    }
    return 0;
}

/* A device playing under a name whose old records nobody has claimed
 * takes them, so the history moves with the player the first time
 * they play after the board went over to devices. */
static void claim_names(TAK_Ledger *l, const TAK_LedgerMatch *m) {
    for (int s = 0; s < m->seat_count; s++) {
        const TAK_LedgerSeat *st = &m->seat[s];
        if (st->ident != TAK_LEDGER_IDENT_DEVICE || st->player_id == 0) continue;
        TAK_LedgerClaim c = { TAK_Ledger_PlayerId(st->name), st->player_id };
        if (c.name_id == 0 || TAK_Ledger_ClaimOf(l, c.name_id)) continue;
        if (!name_has_records(l, c.name_id)) continue;
        uint8_t rec[32];
        size_t n = encode_claim(&c, rec, sizeof rec);
        if (n && take_claim(l, &c) == 0) append(l, rec, n);
    }
}

uint32_t TAK_Ledger_Record(TAK_Ledger *l, const TAK_LedgerMatch *m) {
    if (l->count >= TAK_LEDGER_MATCHES_MAX) { l->refused++; return 0; }
    claim_names(l, m);
    TAK_LedgerMatch copy = *m;
    copy.id = l->next_id;
    copy.reports = 1;
    copy.disputed = 0;
    uint8_t rec[RECORD_MAX];
    size_t n = TAK_Ledger_EncodeMatch(&copy, rec, sizeof rec);
    if (n == 0 || take_match(l, &copy) != 0) return 0;
    append(l, rec, n);
    return copy.id;
}

int TAK_Ledger_Confirm(TAK_Ledger *l, uint32_t id, int agrees) {
    if (take_confirm(l, id, agrees) != 0) return -1;
    uint8_t rec[16];
    append(l, rec, encode_confirm(id, agrees, rec, sizeof rec));
    return 0;
}

uint32_t TAK_Ledger_Disputed(const TAK_Ledger *l) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < l->count; i++) n += l->match[i].disputed ? 1 : 0;
    return n;
}

/* ── Sums ─────────────────────────────────────────────────────────────── */

static void row_add(TAK_LedgerRow *row, const TAK_LedgerMatch *m,
                    const TAK_LedgerSeat *s) {
    if (row->games == 0 || m->ended_ms >= row->last_played_ms) {
        memcpy(row->name, s->name, TAK_NET_NAME_MAX);
        row->last_played_ms = m->ended_ms;
    }
    if (row->games == 0 || m->ended_ms < row->first_played_ms)
        row->first_played_ms = m->ended_ms;
    row->games++;
    if (s->result == TAK_LEDGER_WON) row->wins++; else row->losses++;
    row->score += s->score;
    row->units_built += s->units_built;
    row->kills += s->kills;
    row->units_lost += s->losses;
    row->ticks_alive += s->last_alive_tick;
}

int TAK_Ledger_RowFor(const TAK_Ledger *l, uint64_t player_id, TAK_LedgerRow *row) {
    memset(row, 0, sizeof(*row));
    if (player_id == 0) return 0;
    row->player_id = player_id;
    for (uint32_t i = 0; i < l->count; i++) {
        const TAK_LedgerMatch *m = &l->match[i];
        if (m->disputed) continue;
        for (int s = 0; s < m->seat_count; s++)
            if (TAK_Ledger_SeatPlayer(l, &m->seat[s]) == player_id) row_add(row, m, &m->seat[s]);
    }
    return row->games > 0;
}

/* Wins, then score, then games, then name, then id, so two tables
 * built from the same records come out in the same order. */
static int row_cmp(const void *a, const void *b) {
    const TAK_LedgerRow *x = (const TAK_LedgerRow *)a;
    const TAK_LedgerRow *y = (const TAK_LedgerRow *)b;
    if (x->wins != y->wins) return x->wins > y->wins ? -1 : 1;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    if (x->games != y->games) return x->games > y->games ? -1 : 1;
    int c = strcmp(x->name, y->name);
    if (c) return c;
    if (x->player_id != y->player_id) return x->player_id < y->player_id ? -1 : 1;
    return 0;
}

uint32_t TAK_Ledger_Table(const TAK_Ledger *l, TAK_LedgerRow *rows, uint32_t cap) {
    /* Open addressing over the rows, so a big ledger is one pass. */
    enum { SLOTS = TAK_LEDGER_PLAYERS_MAX * 2 };
    static uint32_t slot[SLOTS];
    memset(slot, 0xff, sizeof slot);
    uint32_t n = 0;
    for (uint32_t i = 0; i < l->count; i++) {
        const TAK_LedgerMatch *m = &l->match[i];
        if (m->disputed) continue;
        for (int s = 0; s < m->seat_count; s++) {
            const TAK_LedgerSeat *st = &m->seat[s];
            uint64_t id = TAK_Ledger_SeatPlayer(l, st);
            if (id == 0) continue;
            uint32_t h = (uint32_t)(id ^ (id >> 32)) % SLOTS;
            TAK_LedgerRow *row = NULL;
            for (;;) {
                if (slot[h] == 0xffffffffu) break;
                if (rows[slot[h]].player_id == id) { row = &rows[slot[h]]; break; }
                h = (h + 1) % SLOTS;
            }
            if (!row) {
                if (n >= cap || n >= TAK_LEDGER_PLAYERS_MAX) continue;
                row = &rows[n];
                memset(row, 0, sizeof(*row));
                row->player_id = id;
                slot[h] = n++;
            }
            row_add(row, m, st);
        }
    }
    qsort(rows, n, sizeof(rows[0]), row_cmp);
    return n;
}

int TAK_Ledger_CurrentName(const TAK_Ledger *l, uint64_t player_id,
                           char out[TAK_NET_NAME_MAX]) {
    out[0] = '\0';
    if (player_id == 0) return 0;
    /* Ids rise with the end time, so the newest game is found first,
     * disputed or not: a name is who they are, not what they scored. */
    uint64_t best = 0;
    int found = 0;
    for (uint32_t i = l->count; i > 0; i--) {
        const TAK_LedgerMatch *m = &l->match[i - 1];
        if (found && m->ended_ms < best) break;
        for (int s = 0; s < m->seat_count; s++) {
            if (TAK_Ledger_SeatPlayer(l, &m->seat[s]) != player_id) continue;
            if (!found || m->ended_ms > best) {
                memcpy(out, m->seat[s].name, TAK_NET_NAME_MAX);
                out[TAK_NET_NAME_MAX - 1] = '\0';
                best = m->ended_ms;
                found = 1;
            }
        }
    }
    return found;
}

int TAK_Ledger_Holds(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < n && hay[k]) {
            char a = hay[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (k == n) return 1;
    }
    return 0;
}

static int id_in(const uint64_t *ids, uint32_t n, uint64_t id) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ids[mid] == id) return 1;
        if (ids[mid] < id) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static int filter_holds(const TAK_Ledger *l, const TAK_LedgerFilter *f,
                        const TAK_LedgerMatch *m) {
    if (!f) return 1;
    if (f->from_ms && m->ended_ms < f->from_ms) return 0;
    if (f->to_ms && m->ended_ms > f->to_ms) return 0;
    if (f->map && f->map[0] && !TAK_Ledger_Holds(m->map_name, f->map)) return 0;
    int want_name = f->name && f->name[0];
    int by_player = !f->player, by_name = !want_name;
    for (int s = 0; s < m->seat_count && !(by_player && by_name); s++) {
        const TAK_LedgerSeat *st = &m->seat[s];
        uint64_t id = TAK_Ledger_SeatPlayer(l, st);
        if (!by_player && id == f->player) by_player = 1;
        if (!by_name && st->kind == TAK_NSLOT_HUMAN &&
            (TAK_Ledger_Holds(st->name, f->name) ||
             (id && f->also && id_in(f->also, f->also_count, id))))
            by_name = 1;
    }
    return by_player && by_name;
}

uint32_t TAK_Ledger_Games(const TAK_Ledger *l, const TAK_LedgerFilter *f,
                          uint32_t offset, uint32_t *ids, uint32_t cap,
                          uint32_t *total) {
    uint32_t seen = 0, written = 0;
    for (uint32_t i = l->count; i > 0; i--) {
        const TAK_LedgerMatch *m = &l->match[i - 1];
        if (!filter_holds(l, f, m)) continue;
        if (seen >= offset && written < cap) ids[written++] = m->id;
        seen++;
    }
    if (total) *total = seen;
    return written;
}

uint32_t TAK_Ledger_History(const TAK_Ledger *l, uint64_t player_id,
                            uint32_t offset, uint32_t *ids, uint32_t cap,
                            uint32_t *total) {
    if (player_id == 0) { if (total) *total = 0; return 0; }
    TAK_LedgerFilter f;
    memset(&f, 0, sizeof f);
    f.player = player_id;
    return TAK_Ledger_Games(l, &f, offset, ids, cap, total);
}
