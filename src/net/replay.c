/*
 * replay.c: the replay file, written a record at a time and read back
 * the same way.
 *
 * See tak_replay.h for the layout. Neither side allocates past its own
 * fixed size: the writer holds one buffer of pending bytes and the
 * reader one window onto the file, whatever the battle's length.
 */

#include "tak_replay.h"

#include "tak_bytes.h"
#include "tak_memory.h"
#include "tak_sim_hash.h"

#include <stdio.h>
#include <string.h>

static const char k_magic[8] = { 'O', 'K', 'R', 'E', 'P', 'L', 'A', 'Y' };

/* Where the fields the close rewrites sit, and the checksum after them. */
#define HDR_CHECKSUM_AT (TAK_REPLAY_HEADER_BYTES - 4)

/* A tag, a delta, a seat, a length and the largest command. */
#define REC_MAX (1 + 5 + 1 + 5 + TAK_COMMAND_MAX_BYTES)
/* An end record, kept free under the size guard so a full file still
 * closes. */
#define END_RESERVE (1 + 5 + 4)

static void set_err(char *err, size_t cap, const char *msg) {
    if (!err || cap == 0) return;
    snprintf(err, cap, "%s", msg);
}

/* ── the header ───────────────────────────────────────────────────── */

static void header_encode(const TAK_ReplayHeader *h, uint8_t out[TAK_REPLAY_HEADER_BYTES]) {
    TAK_ByteWriter w;
    TAK_BW_Init(&w, out, TAK_REPLAY_HEADER_BYTES);
    TAK_BW_Bytes(&w, k_magic, sizeof k_magic);
    TAK_BW_U16(&w, TAK_REPLAY_VERSION);
    TAK_BW_U16(&w, TAK_REPLAY_HEADER_BYTES);
    TAK_BW_U32(&w, h->engine_build_id);
    TAK_BW_U64(&w, h->data_schema);
    TAK_BW_U64(&w, h->data_content);
    for (int g = 0; g < TAK_DATA_GROUP_COUNT; g++) TAK_BW_U64(&w, h->data_group[g]);
    TAK_BW_Bytes(&w, h->map_fp, sizeof h->map_fp);
    TAK_BW_U8(&w, h->flags);
    TAK_BW_U8(&w, h->local_seat);
    TAK_BW_U8(&w, h->turn_ticks);
    TAK_BW_U8(&w, 0);
    TAK_BW_U64(&w, h->recorded_at_utc);
    TAK_BW_Str(&w, h->cfg.map_name, sizeof h->cfg.map_name);
    TAK_BW_Str(&w, h->map_kingdom, sizeof h->map_kingdom);
    TAK_BW_U32(&w, h->cfg.seed);
    TAK_BW_U32(&w, (uint32_t)h->cfg.units_per_side);
    TAK_BW_U8(&w, (uint8_t)(h->cfg.line_of_sight != 0));
    TAK_BW_U8(&w, (uint8_t)(h->cfg.map_revealed != 0));
    TAK_BW_U8(&w, (uint8_t)(h->cfg.monarch_expendable != 0));
    TAK_BW_U8(&w, (uint8_t)(h->cfg.random_start_locations != 0));
    TAK_BW_U8(&w, (uint8_t)(h->cfg.power_codes != 0));
    TAK_BW_U8(&w, (uint8_t)(h->cfg.slow_game != 0));
    TAK_BW_U8(&w, (uint8_t)(h->cfg.crusades_balance != 0));
    TAK_BW_U8(&w, (uint8_t)(h->cfg.numbered_starts != 0));
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        const PlayerSlot *p = &h->cfg.players[i];
        TAK_BW_U8(&w, (uint8_t)p->kind);
        TAK_BW_U8(&w, (uint8_t)p->side);
        TAK_BW_U8(&w, (uint8_t)p->team);
        TAK_BW_U8(&w, (uint8_t)p->color);
        TAK_BW_U8(&w, (uint8_t)p->ai_difficulty);
        TAK_BW_U8(&w, (uint8_t)p->start_pos);
        TAK_BW_Str(&w, p->name, sizeof p->name);
    }
    TAK_BW_U32(&w, h->end_tick);
    TAK_BW_U32(&w, h->command_count);
    TAK_BW_U32(&w, TAK_HashBytes(TAK_SIM_HASH_SEED, out, HDR_CHECKSUM_AT));
}

static int header_decode(const uint8_t in[TAK_REPLAY_HEADER_BYTES],
                         TAK_ReplayHeader *h, char *err, size_t cap) {
    memset(h, 0, sizeof *h);
    if (memcmp(in, k_magic, sizeof k_magic) != 0) {
        set_err(err, cap, "This is not a replay file.");
        return -1;
    }
    TAK_ByteReader r;
    TAK_BR_Init(&r, in, TAK_REPLAY_HEADER_BYTES);
    (void)TAK_BR_Take(&r, sizeof k_magic);
    h->version = TAK_BR_U16(&r);
    uint16_t size = TAK_BR_U16(&r);
    if (h->version != TAK_REPLAY_VERSION || size != TAK_REPLAY_HEADER_BYTES) {
        if (err && cap)
            snprintf(err, cap, "This replay is in format %u, and this build reads format %u.",
                     (unsigned)h->version, (unsigned)TAK_REPLAY_VERSION);
        return -1;
    }
    uint32_t want = TAK_HashBytes(TAK_SIM_HASH_SEED, in, HDR_CHECKSUM_AT);
    if (tak_get_u32(in + HDR_CHECKSUM_AT) != want) {
        set_err(err, cap, "This replay's header is damaged.");
        return -1;
    }
    h->engine_build_id = TAK_BR_U32(&r);
    h->data_schema = TAK_BR_U64(&r);
    h->data_content = TAK_BR_U64(&r);
    for (int g = 0; g < TAK_DATA_GROUP_COUNT; g++) h->data_group[g] = TAK_BR_U64(&r);
    TAK_BR_Bytes(&r, h->map_fp, sizeof h->map_fp);
    h->flags = TAK_BR_U8(&r);
    h->local_seat = TAK_BR_U8(&r);
    h->turn_ticks = TAK_BR_U8(&r);
    (void)TAK_BR_U8(&r);
    h->recorded_at_utc = TAK_BR_U64(&r);
    TAK_BR_Str(&r, h->cfg.map_name, sizeof h->cfg.map_name);
    TAK_BR_Str(&r, h->map_kingdom, sizeof h->map_kingdom);
    h->cfg.seed = TAK_BR_U32(&r);
    h->cfg.units_per_side = (int)TAK_BR_U32(&r);
    h->cfg.line_of_sight = TAK_BR_U8(&r);
    h->cfg.map_revealed = TAK_BR_U8(&r);
    h->cfg.monarch_expendable = TAK_BR_U8(&r);
    h->cfg.random_start_locations = TAK_BR_U8(&r);
    h->cfg.power_codes = TAK_BR_U8(&r);
    h->cfg.slow_game = TAK_BR_U8(&r);
    h->cfg.crusades_balance = TAK_BR_U8(&r);
    h->cfg.numbered_starts = TAK_BR_U8(&r);
    int bad = 0;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        PlayerSlot *p = &h->cfg.players[i];
        uint8_t kind = TAK_BR_U8(&r);
        p->side = TAK_BR_U8(&r);
        p->team = TAK_BR_U8(&r);
        p->color = TAK_BR_U8(&r);
        p->ai_difficulty = TAK_BR_U8(&r);
        p->start_pos = TAK_BR_U8(&r);
        TAK_BR_Str(&r, p->name, sizeof p->name);
        if (kind > TAK_SLOT_AI || p->color >= TAK_PLAYER_COLOR_COUNT ||
            p->start_pos > TAK_MAX_PLAYERS) bad = 1;
        p->kind = (TakSlotKind)kind;
    }
    h->end_tick = TAK_BR_U32(&r);
    h->command_count = TAK_BR_U32(&r);
    if (!TAK_BR_Ok(&r) || bad || h->local_seat < 1 || h->local_seat > TAK_MAX_PLAYERS ||
        !h->cfg.map_name[0]) {
        set_err(err, cap, "This replay's header is damaged.");
        return -1;
    }
    return 0;
}

/* ── variable length integers ─────────────────────────────────────── */

static size_t varint_put(uint8_t *p, uint32_t v) {
    size_t n = 0;
    while (v >= 0x80u) {
        p[n++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

/* At most five bytes, and the fifth may carry only the top four bits. */
static uint32_t varint_get(TAK_ByteReader *r) {
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t b = TAK_BR_U8(r);
        if (!TAK_BR_Ok(r)) return 0;
        if (i == 4 && (b & 0xF0u)) { r->overrun = 1; return 0; }
        v |= (uint32_t)(b & 0x7Fu) << (7 * i);
        if (!(b & 0x80u)) return v;
    }
    r->overrun = 1;
    return 0;
}

/* ── writing ──────────────────────────────────────────────────────── */

struct TAK_ReplayWriter {
    FILE    *f;
    TAK_ReplayHeader hdr;
    uint8_t  buf[TAK_REPLAY_IO_BYTES];
    size_t   len;
    uint32_t bytes;        /* header and records, flushed or not */
    uint32_t sum;          /* over every record byte so far */
    uint32_t last_tick;
    uint32_t count;
    uint32_t full_tick;    /* the first tick a command was turned away */
    uint8_t  full;
    uint8_t  failed;
    uint8_t  rec[REC_MAX];
    uint8_t  wire[TAK_COMMAND_MAX_BYTES];
};

size_t TAK_ReplayWriter_Footprint(void) { return sizeof(TAK_ReplayWriter); }

static int wr_flush(TAK_ReplayWriter *w) {
    if (w->failed) return -1;
    if (w->len && fwrite(w->buf, 1, w->len, w->f) != w->len) {
        w->failed = 1;
        return -1;
    }
    w->len = 0;
    return 0;
}

static int wr_emit(TAK_ReplayWriter *w, const uint8_t *p, size_t n) {
    if (w->failed) return -1;
    if (w->len + n > sizeof w->buf && wr_flush(w) != 0) return -1;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
    w->bytes += (uint32_t)n;
    w->sum = TAK_HashBytes(w->sum, p, n);
    return 0;
}

TAK_ReplayWriter *TAK_ReplayWriter_Open(const char *path,
                                        const TAK_ReplayHeader *hdr,
                                        char *err, size_t err_cap) {
    if (!path || !hdr) {
        set_err(err, err_cap, "No replay to write.");
        return NULL;
    }
    TAK_ReplayWriter *w = (TAK_ReplayWriter *)tak_calloc(1, sizeof *w);
    if (!w) {
        set_err(err, err_cap, "Out of memory for the replay.");
        return NULL;
    }
    w->hdr = *hdr;
    w->hdr.version = TAK_REPLAY_VERSION;
    w->hdr.flags &= (uint8_t)~TAK_REPLAYF_FINISHED;
    w->hdr.end_tick = 0;
    w->hdr.command_count = 0;
    w->sum = TAK_SIM_HASH_SEED;
    w->f = fopen(path, "wb");
    if (!w->f) {
        set_err(err, err_cap, "The replay file could not be created.");
        tak_free(w);
        return NULL;
    }
    uint8_t head[TAK_REPLAY_HEADER_BYTES];
    header_encode(&w->hdr, head);
    if (fwrite(head, 1, sizeof head, w->f) != sizeof head) {
        set_err(err, err_cap, "The replay file could not be written.");
        fclose(w->f);
        remove(path);
        tak_free(w);
        return NULL;
    }
    w->bytes = TAK_REPLAY_HEADER_BYTES;
    return w;
}

int TAK_ReplayWriter_Command(TAK_ReplayWriter *w, const TAK_GameCommand *cmd) {
    if (!w || !cmd || w->failed) return -1;
    if (w->full) return -1;
    if (cmd->tick < w->last_tick) return -1;
    if (cmd->seat < 1 || cmd->seat > TAK_MAX_PLAYERS) return -1;
    size_t wire = 0;
    if (TAK_CommandSerialize(cmd, w->wire, sizeof w->wire, &wire) != 0) return -1;
    size_t n = 0;
    w->rec[n++] = TAK_REPLAY_REC_COMMAND;
    n += varint_put(w->rec + n, cmd->tick - w->last_tick);
    w->rec[n++] = cmd->seat;
    n += varint_put(w->rec + n, (uint32_t)wire);
    memcpy(w->rec + n, w->wire, wire);
    n += wire;
    if ((uint64_t)w->bytes + n + END_RESERVE > TAK_REPLAY_MAX_BYTES) {
        /* Nothing after this can be trusted to replay, so the file ends
         * at this tick when it closes. */
        w->full = 1;
        w->full_tick = cmd->tick;
        return -1;
    }
    if (wr_emit(w, w->rec, n) != 0) return -1;
    w->last_tick = cmd->tick;
    w->count++;
    return 0;
}

int TAK_ReplayWriter_Checkpoint(TAK_ReplayWriter *w, uint32_t tick, uint32_t hash) {
    if (!w || w->failed || w->full) return -1;
    if (tick < w->last_tick) return -1;
    uint8_t rec[1 + 5 + 8];
    size_t n = 0;
    uint32_t before = w->sum;
    rec[n++] = TAK_REPLAY_REC_CHECKPOINT;
    n += varint_put(rec + n, tick - w->last_tick);
    tak_put_u32(rec + n, hash);
    n += 4;
    tak_put_u32(rec + n, before);
    n += 4;
    if ((uint64_t)w->bytes + n + END_RESERVE > TAK_REPLAY_MAX_BYTES) {
        w->full = 1;
        w->full_tick = tick;
        return -1;
    }
    if (wr_emit(w, rec, n) != 0) return -1;
    w->last_tick = tick;
    return 0;
}

int TAK_ReplayWriter_Flush(TAK_ReplayWriter *w) {
    if (!w) return -1;
    if (wr_flush(w) != 0) return -1;
    return fflush(w->f) == 0 ? 0 : -1;
}

int TAK_ReplayWriter_Close(TAK_ReplayWriter *w, uint32_t end_tick) {
    if (!w) return -1;
    if (w->full && w->full_tick < end_tick) end_tick = w->full_tick;
    if (end_tick < w->last_tick) end_tick = w->last_tick;
    uint8_t rec[END_RESERVE];
    size_t n = 0;
    uint32_t before = w->sum;
    rec[n++] = TAK_REPLAY_REC_END;
    n += varint_put(rec + n, end_tick - w->last_tick);
    tak_put_u32(rec + n, before);
    n += 4;
    int rc = wr_emit(w, rec, n);
    if (rc == 0) rc = wr_flush(w);
    if (rc == 0) {
        w->hdr.flags |= TAK_REPLAYF_FINISHED;
        w->hdr.end_tick = end_tick;
        w->hdr.command_count = w->count;
        uint8_t head[TAK_REPLAY_HEADER_BYTES];
        header_encode(&w->hdr, head);
        if (fseek(w->f, 0, SEEK_SET) != 0 ||
            fwrite(head, 1, sizeof head, w->f) != sizeof head) rc = -1;
    }
    if (fclose(w->f) != 0) rc = -1;
    tak_free(w);
    return rc;
}

uint32_t TAK_ReplayWriter_Bytes(const TAK_ReplayWriter *w) { return w ? w->bytes : 0; }
int TAK_ReplayWriter_Full(const TAK_ReplayWriter *w) { return w ? w->full : 0; }
uint32_t TAK_ReplayWriter_LastTick(const TAK_ReplayWriter *w) { return w ? w->last_tick : 0; }

/* ── reading ──────────────────────────────────────────────────────── */

struct TAK_ReplayReader {
    FILE    *f;
    TAK_ReplayHeader hdr;
    uint8_t  buf[TAK_REPLAY_IO_BYTES];
    size_t   pos, len;
    uint8_t  eof;
    long     off;          /* file offset of the next record */
    long     limit;        /* no record at or past this is played */
    uint32_t tick;
    uint32_t sum;
    uint32_t count;
    TAK_GameCommand cmd;
};

size_t TAK_ReplayReader_Footprint(void) { return sizeof(TAK_ReplayReader); }

/* Keep the window as full as the file allows, so a whole record is in
 * it whenever the file holds one. */
static void rd_fill(TAK_ReplayReader *r) {
    if (r->pos > 0) {
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos;
        r->pos = 0;
    }
    while (!r->eof && r->len < sizeof r->buf) {
        size_t got = fread(r->buf + r->len, 1, sizeof r->buf - r->len, r->f);
        r->len += got;
        if (got == 0) r->eof = 1;
    }
}

enum { RD_OK = 1, RD_EOF = 0, RD_BAD = -1, RD_CUT = -2 };

/* One record. RD_CUT when the file stops inside it. */
static int rd_record(TAK_ReplayReader *r, TAK_ReplayRecord *out) {
    if (r->len - r->pos < REC_MAX) rd_fill(r);
    if (r->pos >= r->len) return RD_EOF;
    TAK_ByteReader br;
    TAK_BR_Init(&br, r->buf + r->pos, r->len - r->pos);
    uint8_t tag = TAK_BR_U8(&br);
    uint32_t delta = varint_get(&br);
    if (!TAK_BR_Ok(&br)) return r->eof ? RD_CUT : RD_BAD;
    if (delta > UINT32_MAX - r->tick) return RD_BAD;
    memset(out, 0, sizeof *out);
    out->kind = tag;
    out->tick = r->tick + delta;
    uint32_t before = r->sum;
    switch (tag) {
    case TAK_REPLAY_REC_COMMAND: {
        uint8_t seat = TAK_BR_U8(&br);
        uint32_t wire = varint_get(&br);
        if (!TAK_BR_Ok(&br)) return r->eof ? RD_CUT : RD_BAD;
        if (seat < 1 || seat > TAK_MAX_PLAYERS || wire == 0 ||
            wire > TAK_COMMAND_MAX_BYTES) return RD_BAD;
        const uint8_t *p = TAK_BR_Take(&br, wire);
        if (!p) return r->eof ? RD_CUT : RD_BAD;
        size_t used = 0;
        if (TAK_CommandDeserialize(&r->cmd, p, wire, &used) != 0 || used != wire)
            return RD_BAD;
        r->cmd.seat = seat;
        r->cmd.tick = out->tick;
        out->cmd = &r->cmd;
        break;
    }
    case TAK_REPLAY_REC_CHECKPOINT: {
        out->hash = TAK_BR_U32(&br);
        uint32_t sum = TAK_BR_U32(&br);
        if (!TAK_BR_Ok(&br)) return r->eof ? RD_CUT : RD_BAD;
        if (sum != before) return RD_BAD;
        break;
    }
    case TAK_REPLAY_REC_END: {
        uint32_t sum = TAK_BR_U32(&br);
        if (!TAK_BR_Ok(&br)) return r->eof ? RD_CUT : RD_BAD;
        if (sum != before) return RD_BAD;
        break;
    }
    default:
        return RD_BAD;
    }
    r->sum = TAK_HashBytes(r->sum, r->buf + r->pos, br.pos);
    r->pos += br.pos;
    r->off += (long)br.pos;
    r->tick = out->tick;
    if (tag == TAK_REPLAY_REC_COMMAND) r->count++;
    return RD_OK;
}

static void rd_rewind(TAK_ReplayReader *r) {
    r->pos = r->len = 0;
    r->eof = 0;
    r->off = TAK_REPLAY_HEADER_BYTES;
    r->tick = 0;
    r->sum = TAK_SIM_HASH_SEED;
    r->count = 0;
}

static FILE *open_header(const char *path, TAK_ReplayHeader *out,
                         char *err, size_t cap) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        set_err(err, cap, "The replay file could not be opened.");
        return NULL;
    }
    uint8_t head[TAK_REPLAY_HEADER_BYTES];
    size_t got = fread(head, 1, sizeof head, f);
    if (got < sizeof k_magic || memcmp(head, k_magic, sizeof k_magic) != 0) {
        set_err(err, cap, "This is not a replay file.");
        fclose(f);
        return NULL;
    }
    if (got != sizeof head) {
        set_err(err, cap, "This replay is cut short.");
        fclose(f);
        return NULL;
    }
    if (header_decode(head, out, err, cap) != 0) {
        fclose(f);
        return NULL;
    }
    return f;
}

int TAK_Replay_ReadHeader(const char *path, TAK_ReplayHeader *out,
                          char *err, size_t err_cap) {
    TAK_ReplayHeader tmp;
    FILE *f = open_header(path, out ? out : &tmp, err, err_cap);
    if (!f) return -1;
    fclose(f);
    return 0;
}

TAK_ReplayReader *TAK_Replay_Open(const char *path, TAK_ReplayHeader *hdr,
                                  uint32_t *playable_end,
                                  char *err, size_t err_cap) {
    if (playable_end) *playable_end = 0;
    TAK_ReplayReader *r = (TAK_ReplayReader *)tak_calloc(1, sizeof *r);
    if (!r) {
        set_err(err, err_cap, "Out of memory for the replay.");
        return NULL;
    }
    r->f = open_header(path, &r->hdr, err, err_cap);
    if (!r->f) {
        tak_free(r);
        return NULL;
    }
    int finished = (r->hdr.flags & TAK_REPLAYF_FINISHED) != 0;
    rd_rewind(r);

    /* The whole stream once, a window at a time. A finished recording
     * must reach its end record exactly as the header says. One that
     * never closed plays up to the last checkpoint its checksum
     * vouches for, and whatever the tab wrote after that is dropped. */
    uint32_t end = 0;
    long limit = TAK_REPLAY_HEADER_BYTES;
    const char *why = NULL;
    TAK_ReplayRecord rec;
    for (;;) {
        int rc = rd_record(r, &rec);
        if (rc == RD_OK && rec.kind == TAK_REPLAY_REC_CHECKPOINT) {
            end = rec.tick;
            limit = r->off;
            continue;
        }
        if (rc == RD_OK && rec.kind == TAK_REPLAY_REC_END) {
            end = rec.tick;
            limit = r->off;
            rd_fill(r);
            if (r->pos != r->len) why = "This replay is damaged.";
            else if (finished && (end != r->hdr.end_tick ||
                                  r->count != r->hdr.command_count))
                why = "This replay is damaged.";
            break;
        }
        if (rc == RD_OK) continue;
        /* A tab that closed mid write leaves a record cut at the end of
         * the file, never one that reads wrong. */
        if (rc == RD_BAD) why = "This replay is damaged.";
        else if (finished) why = "This replay is cut short.";
        break;
    }
    if (!why && end == 0) why = "This replay holds no play.";
    if (why) {
        set_err(err, err_cap, why);
        fclose(r->f);
        tak_free(r);
        return NULL;
    }

    if (fseek(r->f, TAK_REPLAY_HEADER_BYTES, SEEK_SET) != 0) {
        set_err(err, err_cap, "The replay file could not be read.");
        fclose(r->f);
        tak_free(r);
        return NULL;
    }
    rd_rewind(r);
    r->limit = limit;
    if (hdr) *hdr = r->hdr;
    if (playable_end) *playable_end = end;
    return r;
}

int TAK_Replay_Next(TAK_ReplayReader *r, TAK_ReplayRecord *out) {
    if (!r || !out) return -1;
    if (r->off >= r->limit) return 0;
    int rc = rd_record(r, out);
    if (rc == RD_OK) return out->kind == TAK_REPLAY_REC_END ? 0 : 1;
    return rc == RD_EOF ? 0 : -1;
}

void TAK_Replay_Close(TAK_ReplayReader *r) {
    if (!r) return;
    if (r->f) fclose(r->f);
    tak_free(r);
}

/* ── compatibility ────────────────────────────────────────────────── */

int TAK_Replay_CheckCompatible(const TAK_ReplayHeader *hdr,
                               uint32_t engine_build_id,
                               const TAK_DataFingerprint *fp,
                               char *err, size_t err_cap) {
    if (!hdr) {
        set_err(err, err_cap, "No replay.");
        return -1;
    }
    if (hdr->engine_build_id != engine_build_id) {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "This replay was recorded on engine build %u and this is build %u. "
                     "A replay plays only on the build that recorded it.",
                     (unsigned)hdr->engine_build_id, (unsigned)engine_build_id);
        return -1;
    }
    TAK_DataFingerprint none;
    memset(&none, 0, sizeof none);
    if (!fp) fp = &none;
    if (hdr->data_schema != fp->schema) {
        set_err(err, err_cap,
                "This replay was recorded by a build that reads the game files differently.");
        return -1;
    }
    for (int g = 0; g < TAK_DATA_GROUP_COUNT; g++) {
        if (hdr->data_group[g] == fp->group[g]) continue;
        if (err && err_cap)
            snprintf(err, err_cap,
                     "The %s in your game files differ from the ones this replay was "
                     "recorded with, so it would not play the same battle.",
                     TAK_DataFingerprint_GroupName(g));
        return -1;
    }
    if (hdr->data_content != fp->content) {
        set_err(err, err_cap,
                "Your game files differ from the ones this replay was recorded with.");
        return -1;
    }
    return 0;
}

/* ── names ────────────────────────────────────────────────────────── */

/* Days since 1970-01-01 as a civil date, proleptic Gregorian. */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

void TAK_Replay_Slug(uint64_t utc_seconds, char *out, size_t cap) {
    if (!out || cap == 0) return;
    int y = 1970;
    unsigned m = 1, d = 1;
    civil_from_days((int64_t)(utc_seconds / 86400u), &y, &m, &d);
    unsigned s = (unsigned)(utc_seconds % 86400u);
    snprintf(out, cap, "%04d%02u%02u-%02u%02u%02u", y, m, d,
             s / 3600u, (s / 60u) % 60u, s % 60u);
}
