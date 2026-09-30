/*
 * test_replay.c: the replay file, with synthetic commands and no world.
 *
 * What is written reads back the same, a file that is cut or damaged
 * anywhere is refused before a battle starts, a build or data mismatch
 * is refused with its reason, and neither side's memory grows with the
 * length of the battle.
 */

#include "test_framework.h"

#include "tak_bytes.h"
#include "tak_memory.h"
#include "tak_replay.h"
#include "tak_sides.h"
#include "tak_sim_hash.h"

#include <stdio.h>
#include <string.h>

#define RP_FILE  "test_replay_tmp.okreplay"
#define RP_COPY  "test_replay_cut.okreplay"

static TAK_ReplayHeader rp_header(void) {
    TAK_ReplayHeader h;
    memset(&h, 0, sizeof h);
    h.engine_build_id = 12;
    h.data_schema = 3;
    h.data_content = 0x1122334455667788ull;
    for (int g = 0; g < TAK_DATA_GROUP_COUNT; g++) h.data_group[g] = 0x100u + (uint64_t)g;
    for (int i = 0; i < 32; i++) h.map_fp[i] = (uint8_t)(i * 7);
    h.flags = TAK_REPLAYF_MATCH | TAK_REPLAYF_MAP_FP;
    h.local_seat = 2;
    h.turn_ticks = 3;
    h.recorded_at_utc = 1790000000u;
    snprintf(h.map_kingdom, sizeof h.map_kingdom, "taros");
    BattleConfig_SetDefaults(&h.cfg);
    snprintf(h.cfg.map_name, sizeof h.cfg.map_name, "Two Rivers");
    h.cfg.seed = 0xdeadbeefu;
    h.cfg.units_per_side = 700;
    h.cfg.map_revealed = 1;
    h.cfg.crusades_balance = 1;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        PlayerSlot *p = &h.cfg.players[i];
        p->kind = i < 3 ? (i == 2 ? TAK_SLOT_AI : TAK_SLOT_HUMAN) : TAK_SLOT_CLOSED;
        p->side = i == 1 ? TAK_SIDE_CREON : i;
        p->team = i % 5;
        p->color = i;
        p->ai_difficulty = i == 2 ? 3 : 0;
        p->start_pos = i == 0 ? 2 : 0;
        snprintf(p->name, sizeof p->name, "Player %d", i + 1);
    }
    return h;
}

/* Command n of a synthetic stream: every few ticks, all eight seats,
 * and now and then the biggest command the wire carries. */
static TAK_GameCommand g_cmd;

static const TAK_GameCommand *rp_cmd(uint32_t n) {
    memset(&g_cmd, 0, sizeof g_cmd);
    g_cmd.tick = n / 3u * 5u;
    g_cmd.seat = (uint8_t)(1u + n % 8u);
    g_cmd.type = (uint8_t)(1u + n % (TAK_CMD_COUNT - 1u));
    g_cmd.target_x = (int32_t)(n * 16u) - 4000;
    g_cmd.target_y = (int32_t)(n * 3u);
    g_cmd.target_unit_id = n * 11u;
    g_cmd.build_type_id = (uint16_t)n;
    g_cmd.arg = (uint16_t)(n * 5u);
    uint16_t units = (uint16_t)(n % 97u == 0 ? TAK_COMMAND_MAX_UNITS : n % 5u);
    if (g_cmd.type == TAK_CMD_MOVE_FORMATION && units == 0) units = 3;
    g_cmd.unit_count = units;
    for (uint16_t i = 0; i < units; i++) {
        g_cmd.unit_ids[i] = n * 1000u + i;
        g_cmd.unit_dx[i] = (int16_t)(i * 3 - 100);
        g_cmd.unit_dy[i] = (int16_t)(50 - i);
    }
    if (g_cmd.type != TAK_CMD_MOVE_FORMATION) {
        memset(g_cmd.unit_dx, 0, sizeof g_cmd.unit_dx);
        memset(g_cmd.unit_dy, 0, sizeof g_cmd.unit_dy);
    }
    return &g_cmd;
}

static uint32_t rp_hash(uint32_t tick) { return TAK_HashU32(TAK_SIM_HASH_SEED, tick * 2654435761u); }

/* A recording of `count` commands, with a checkpoint every sixty ticks
 * as the game writes them. Closed at `end` unless `close` is 0, when it
 * is flushed and dropped the way a closing tab leaves it. */
static int rp_write(const char *path, uint32_t count, int close, uint32_t *out_end) {
    TAK_ReplayHeader h = rp_header();
    char err[TAK_REPLAY_ERR_MAX];
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(path, &h, err, sizeof err);
    if (!w) return -1;
    uint32_t next_cp = TAK_REPLAY_HASH_EVERY;
    for (uint32_t n = 0; n < count; n++) {
        const TAK_GameCommand *c = rp_cmd(n);
        while (next_cp <= c->tick) {
            if (TAK_ReplayWriter_Checkpoint(w, next_cp, rp_hash(next_cp)) != 0) return -1;
            next_cp += TAK_REPLAY_HASH_EVERY;
        }
        if (TAK_ReplayWriter_Command(w, c) != 0) return -1;
    }
    uint32_t end = rp_cmd(count ? count - 1 : 0)->tick + 1;
    if (out_end) *out_end = end;
    if (!close) {
        /* What a tab that closes now leaves on disk: everything flushed,
         * no end record, the header as it was first written. The writer
         * is then closed to free it and the file put back as it was. */
        static uint8_t body[1 << 20];
        if (TAK_ReplayWriter_Flush(w) != 0) return -1;
        FILE *f = fopen(path, "rb");
        if (!f) return -1;
        size_t keep = fread(body, 1, sizeof body, f);
        fclose(f);
        (void)TAK_ReplayWriter_Close(w, end);
        f = fopen(path, "wb");
        if (!f) return -1;
        fwrite(body, 1, keep, f);
        fclose(f);
        return keep > TAK_REPLAY_HEADER_BYTES ? 0 : -1;
    }
    return TAK_ReplayWriter_Close(w, end);
}

static long rp_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int rp_copy_prefix(const char *from, const char *to, long n, long flip_at) {
    static uint8_t buf[1 << 20];
    FILE *f = fopen(from, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n > (long)got) n = (long)got;
    if (flip_at >= 0 && flip_at < n) buf[flip_at] ^= 0x5a;
    f = fopen(to, "wb");
    if (!f) return -1;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    return 0;
}

static int rp_same_cmd(const TAK_GameCommand *a, const TAK_GameCommand *b) {
    if (a->seat != b->seat || a->tick != b->tick || a->type != b->type) return 0;
    if (a->unit_count != b->unit_count || a->target_x != b->target_x) return 0;
    if (a->target_y != b->target_y || a->target_unit_id != b->target_unit_id) return 0;
    if (a->build_type_id != b->build_type_id || a->arg != b->arg) return 0;
    for (int i = 0; i < a->unit_count; i++) {
        if (a->unit_ids[i] != b->unit_ids[i]) return 0;
        if (a->unit_dx[i] != b->unit_dx[i] || a->unit_dy[i] != b->unit_dy[i]) return 0;
    }
    return 1;
}

/* ── the round trip ────────────────────────────────────────────────── */

TEST(what_is_written_reads_back_the_same) {
    uint32_t end = 0;
    ASSERT_EQ_INT(0, rp_write(RP_FILE, 500, 1, &end));

    TAK_ReplayHeader want = rp_header(), got;
    uint32_t playable = 0;
    char err[TAK_REPLAY_ERR_MAX] = "";
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, &got, &playable, err, sizeof err);
    if (!r) printf("(%s) ", err);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT((int)end, (int)playable);
    ASSERT_EQ_INT((int)end, (int)got.end_tick);
    ASSERT_EQ_INT(500, (int)got.command_count);
    ASSERT(got.flags & TAK_REPLAYF_FINISHED);
    ASSERT(got.flags & TAK_REPLAYF_MATCH);
    ASSERT_EQ_INT((int)want.engine_build_id, (int)got.engine_build_id);
    ASSERT(want.data_content == got.data_content);
    ASSERT(memcmp(want.data_group, got.data_group, sizeof want.data_group) == 0);
    ASSERT(memcmp(want.map_fp, got.map_fp, sizeof want.map_fp) == 0);
    ASSERT_EQ_INT(2, got.local_seat);
    ASSERT_EQ_INT(3, got.turn_ticks);
    ASSERT(got.recorded_at_utc == want.recorded_at_utc);
    ASSERT_EQ_STR("Two Rivers", got.cfg.map_name);
    ASSERT_EQ_STR("taros", got.map_kingdom);
    ASSERT_EQ_INT((int)want.cfg.seed, (int)got.cfg.seed);
    ASSERT_EQ_INT(700, got.cfg.units_per_side);
    ASSERT_EQ_INT(1, got.cfg.map_revealed);
    ASSERT_EQ_INT(1, got.cfg.crusades_balance);
    ASSERT_EQ_INT(want.cfg.line_of_sight, got.cfg.line_of_sight);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        const PlayerSlot *a = &want.cfg.players[i], *b = &got.cfg.players[i];
        ASSERT_EQ_INT(a->kind, b->kind);
        ASSERT_EQ_INT(a->side, b->side);
        ASSERT_EQ_INT(a->team, b->team);
        ASSERT_EQ_INT(a->color, b->color);
        ASSERT_EQ_INT(a->ai_difficulty, b->ai_difficulty);
        ASSERT_EQ_INT(a->start_pos, b->start_pos);
        ASSERT_EQ_STR(a->name, b->name);
    }

    /* Every command in the order written, checkpoints between them. */
    uint32_t n = 0, checkpoints = 0, last_tick = 0;
    TAK_ReplayRecord rec;
    int rc;
    static TAK_GameCommand expect;
    while ((rc = TAK_Replay_Next(r, &rec)) == 1) {
        ASSERT(rec.tick >= last_tick);
        last_tick = rec.tick;
        if (rec.kind == TAK_REPLAY_REC_CHECKPOINT) {
            ASSERT_EQ_INT((int)rp_hash(rec.tick), (int)rec.hash);
            ASSERT_EQ_INT(0, (int)(rec.tick % TAK_REPLAY_HASH_EVERY));
            checkpoints++;
            continue;
        }
        ASSERT_EQ_INT(TAK_REPLAY_REC_COMMAND, rec.kind);
        expect = *rp_cmd(n);
        ASSERT(rp_same_cmd(&expect, rec.cmd));
        n++;
    }
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(500, (int)n);
    ASSERT_EQ_INT((int)(rp_cmd(499)->tick / TAK_REPLAY_HASH_EVERY), (int)checkpoints);
    TAK_Replay_Close(r);

    /* The header alone reads the same. */
    TAK_ReplayHeader only;
    ASSERT_EQ_INT(0, TAK_Replay_ReadHeader(RP_FILE, &only, err, sizeof err));
    ASSERT_EQ_INT((int)end, (int)only.end_tick);
    remove(RP_FILE);
}

/* A tab closed mid battle leaves a file with no end record and a header
 * that never became final. It plays to its last checkpoint, and the
 * commands written after that are not played. */
TEST(an_unfinished_recording_plays_to_its_last_checkpoint) {
    uint32_t end = 0;
    ASSERT_EQ_INT(0, rp_write(RP_FILE, 300, 0, &end));
    TAK_ReplayHeader got;
    uint32_t playable = 0;
    char err[TAK_REPLAY_ERR_MAX] = "";
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, &got, &playable, err, sizeof err);
    if (!r) printf("(%s) ", err);
    ASSERT_NOT_NULL(r);
    ASSERT(!(got.flags & TAK_REPLAYF_FINISHED));
    uint32_t last_cp = rp_cmd(299)->tick / TAK_REPLAY_HASH_EVERY * TAK_REPLAY_HASH_EVERY;
    ASSERT_EQ_INT((int)last_cp, (int)playable);
    TAK_ReplayRecord rec;
    int rc;
    while ((rc = TAK_Replay_Next(r, &rec)) == 1) {
        if (rec.kind == TAK_REPLAY_REC_COMMAND) ASSERT(rec.tick < playable);
    }
    ASSERT_EQ_INT(0, rc);
    TAK_Replay_Close(r);

    /* Cut mid record, as a flush that did not finish leaves it: still
     * the same last checkpoint. */
    long size = rp_file_size(RP_FILE);
    ASSERT_EQ_INT(0, rp_copy_prefix(RP_FILE, RP_COPY, size - 3, -1));
    r = TAK_Replay_Open(RP_COPY, &got, &playable, err, sizeof err);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT((int)last_cp, (int)playable);
    TAK_Replay_Close(r);
    remove(RP_FILE);
    remove(RP_COPY);
}

/* ── damage ────────────────────────────────────────────────────────── */

/* Every length short of the whole file is refused, header included,
 * and none of them crashes or reads past what it was given. */
TEST(a_cut_file_is_refused_at_every_length) {
    ASSERT_EQ_INT(0, rp_write(RP_FILE, 120, 1, NULL));
    long size = rp_file_size(RP_FILE);
    ASSERT(size > TAK_REPLAY_HEADER_BYTES);
    char err[TAK_REPLAY_ERR_MAX];
    int refused = 0;
    for (long n = 0; n < size; n++) {
        ASSERT_EQ_INT(0, rp_copy_prefix(RP_FILE, RP_COPY, n, -1));
        err[0] = '\0';
        TAK_ReplayReader *r = TAK_Replay_Open(RP_COPY, NULL, NULL, err, sizeof err);
        if (r) {
            printf("(opened at %ld of %ld) ", n, size);
            TAK_Replay_Close(r);
            continue;
        }
        ASSERT(err[0] != '\0');
        refused++;
    }
    ASSERT_EQ_INT((int)size, refused);
    remove(RP_FILE);
    remove(RP_COPY);
}

/* One damaged byte anywhere, header or stream, and the file is refused:
 * the header carries a checksum and every record is under the stream's. */
TEST(a_damaged_byte_anywhere_is_refused) {
    ASSERT_EQ_INT(0, rp_write(RP_FILE, 120, 1, NULL));
    long size = rp_file_size(RP_FILE);
    char err[TAK_REPLAY_ERR_MAX];
    int opened = 0;
    for (long at = 0; at < size; at++) {
        ASSERT_EQ_INT(0, rp_copy_prefix(RP_FILE, RP_COPY, size, at));
        TAK_ReplayReader *r = TAK_Replay_Open(RP_COPY, NULL, NULL, err, sizeof err);
        if (r) {
            printf("(opened with byte %ld damaged) ", at);
            opened++;
            TAK_Replay_Close(r);
        }
    }
    ASSERT_EQ_INT(0, opened);
    /* A byte more at the end is damage too. */
    FILE *f = fopen(RP_FILE, "ab");
    ASSERT_NOT_NULL(f);
    fputc(0, f);
    fclose(f);
    ASSERT_NULL(TAK_Replay_Open(RP_FILE, NULL, NULL, err, sizeof err));
    remove(RP_FILE);
    remove(RP_COPY);
}

TEST(a_file_that_is_not_a_replay_is_refused) {
    FILE *f = fopen(RP_COPY, "wb");
    ASSERT_NOT_NULL(f);
    for (int i = 0; i < 2000; i++) fputc(i * 31, f);
    fclose(f);
    char err[TAK_REPLAY_ERR_MAX] = "";
    ASSERT_NULL(TAK_Replay_Open(RP_COPY, NULL, NULL, err, sizeof err));
    ASSERT_EQ_STR("This is not a replay file.", err);
    ASSERT_NULL(TAK_Replay_Open("no_such_file.okreplay", NULL, NULL, err, sizeof err));
    ASSERT_EQ_STR("The replay file could not be opened.", err);
    remove(RP_COPY);
}

/* A format this build does not read names both formats, even when the
 * header's checksum is right for what it says. */
TEST(another_format_version_is_refused) {
    ASSERT_EQ_INT(0, rp_write(RP_FILE, 10, 1, NULL));
    static uint8_t buf[1 << 16];
    FILE *f = fopen(RP_FILE, "rb");
    ASSERT_NOT_NULL(f);
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    tak_put_u16(buf + 8, TAK_REPLAY_VERSION + 1);
    tak_put_u32(buf + TAK_REPLAY_HEADER_BYTES - 4,
                TAK_HashBytes(TAK_SIM_HASH_SEED, buf, TAK_REPLAY_HEADER_BYTES - 4));
    f = fopen(RP_FILE, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(buf, 1, n, f);
    fclose(f);
    char err[TAK_REPLAY_ERR_MAX] = "";
    ASSERT_NULL(TAK_Replay_Open(RP_FILE, NULL, NULL, err, sizeof err));
    char want[TAK_REPLAY_ERR_MAX];
    snprintf(want, sizeof want, "This replay is in format %d, and this build reads format %d.",
             TAK_REPLAY_VERSION + 1, TAK_REPLAY_VERSION);
    ASSERT_EQ_STR(want, err);
    remove(RP_FILE);
}

/* ── compatibility ─────────────────────────────────────────────────── */

TEST(the_build_and_the_data_must_match) {
    TAK_ReplayHeader h = rp_header();
    TAK_DataFingerprint fp;
    memset(&fp, 0, sizeof fp);
    fp.schema = h.data_schema;
    fp.content = h.data_content;
    memcpy(fp.group, h.data_group, sizeof fp.group);
    char err[TAK_REPLAY_ERR_MAX] = "";
    ASSERT_EQ_INT(0, TAK_Replay_CheckCompatible(&h, 12, &fp, err, sizeof err));

    ASSERT_EQ_INT(-1, TAK_Replay_CheckCompatible(&h, 13, &fp, err, sizeof err));
    ASSERT(strstr(err, "engine build 12") != NULL);
    ASSERT(strstr(err, "build 13") != NULL);

    TAK_DataFingerprint other = fp;
    other.group[TAK_DATA_GROUP_SCRIPTS] ^= 1u;
    other.content ^= 1u;
    ASSERT_EQ_INT(-1, TAK_Replay_CheckCompatible(&h, 12, &other, err, sizeof err));
    ASSERT(strstr(err, "scripts") != NULL);

    other = fp;
    other.schema++;
    ASSERT_EQ_INT(-1, TAK_Replay_CheckCompatible(&h, 12, &other, err, sizeof err));

    other = fp;
    other.content ^= 4u;
    ASSERT_EQ_INT(-1, TAK_Replay_CheckCompatible(&h, 12, &other, err, sizeof err));

    /* Nothing mounted matches only a recording made the same way. */
    ASSERT_EQ_INT(-1, TAK_Replay_CheckCompatible(&h, 12, NULL, err, sizeof err));
    TAK_ReplayHeader bare = h;
    bare.data_schema = bare.data_content = 0;
    memset(bare.data_group, 0, sizeof bare.data_group);
    ASSERT_EQ_INT(0, TAK_Replay_CheckCompatible(&bare, 12, NULL, err, sizeof err));
}


/* ── hostile files ─────────────────────────────────────────────────── */

/* A stream built by hand, past every rule the writer keeps, so the
 * reader's own checks are what is tested. The header is a real one with
 * its last tick, count and checksum put right for the stream. */
static uint8_t  g_craft[1 << 20];
static size_t   g_craft_n;
static uint32_t g_craft_sum, g_craft_tick;

static void craft_put(const uint8_t *p, size_t n) {
    memcpy(g_craft + g_craft_n, p, n);
    g_craft_n += n;
}

static size_t craft_varint(uint8_t *p, uint32_t v) {
    size_t n = 0;
    while (v >= 0x80u) { p[n++] = (uint8_t)(v | 0x80u); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}

static int craft_begin(const TAK_ReplayHeader *h) {
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, h, NULL, 0);
    if (!w || TAK_ReplayWriter_Checkpoint(w, 60, 1) != 0) return -1;
    if (TAK_ReplayWriter_Close(w, 60) != 0) return -1;
    FILE *f = fopen(RP_FILE, "rb");
    if (!f) return -1;
    size_t got = fread(g_craft, 1, TAK_REPLAY_HEADER_BYTES, f);
    fclose(f);
    g_craft_n = got;
    g_craft_sum = TAK_SIM_HASH_SEED;
    g_craft_tick = 0;
    return got == TAK_REPLAY_HEADER_BYTES ? 0 : -1;
}

static void craft_record(uint8_t tag, uint32_t tick, const uint8_t *body, size_t n) {
    uint8_t rec[16];
    size_t k = 0;
    rec[k++] = tag;
    k += craft_varint(rec + k, tick - g_craft_tick);
    size_t at = g_craft_n;
    craft_put(rec, k);
    if (n) craft_put(body, n);
    g_craft_sum = TAK_HashBytes(g_craft_sum, g_craft + at, g_craft_n - at);
    g_craft_tick = tick;
}

static void craft_cmd(uint32_t tick, const TAK_GameCommand *c) {
    uint8_t body[8 + TAK_COMMAND_MAX_BYTES];
    size_t wire = 0, k = 0;
    body[k++] = c->seat;
    TAK_CommandSerialize(c, body + 8, TAK_COMMAND_MAX_BYTES, &wire);
    k += craft_varint(body + k, (uint32_t)wire);
    memmove(body + k, body + 8, wire);
    craft_record(TAK_REPLAY_REC_COMMAND, tick, body, k + wire);
}

static void craft_cp(uint32_t tick) {
    uint8_t body[8];
    tak_put_u32(body, 1);
    tak_put_u32(body + 4, g_craft_sum);
    craft_record(TAK_REPLAY_REC_CHECKPOINT, tick, body, 8);
}

static int craft_end(uint32_t tick, uint32_t count) {
    uint8_t body[4];
    tak_put_u32(body, g_craft_sum);
    craft_record(TAK_REPLAY_REC_END, tick, body, 4);
    tak_put_u32(g_craft + 564, tick);
    tak_put_u32(g_craft + 568, count);
    tak_put_u32(g_craft + 572, TAK_HashBytes(TAK_SIM_HASH_SEED, g_craft, 572));
    FILE *f = fopen(RP_FILE, "wb");
    if (!f) return -1;
    fwrite(g_craft, 1, g_craft_n, f);
    fclose(f);
    return 0;
}

/* The crafted stream reads when it keeps the rules, so a refusal below
 * is the rule and not the crafting. */
TEST(a_crafted_stream_that_keeps_the_rules_opens) {
    TAK_ReplayHeader h = rp_header();
    ASSERT_EQ_INT(0, craft_begin(&h));
    craft_cmd(10, rp_cmd(1));
    craft_cp(60);
    craft_cmd(100, rp_cmd(2));
    craft_cp(120);
    ASSERT_EQ_INT(0, craft_end(150, 2));
    uint32_t end = 0;
    char err[TAK_REPLAY_ERR_MAX] = "";
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, NULL, &end, err, sizeof err);
    if (!r) printf("(%s) ", err);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(150, (int)end);
    TAK_Replay_Close(r);
    remove(RP_FILE);
}

/* Probe 1: one checkpoint and an end 828 days later. */
TEST(an_end_tick_past_the_longest_battle_is_refused) {
    TAK_ReplayHeader h = rp_header();
    ASSERT_EQ_INT(0, craft_begin(&h));
    craft_cp(60);
    ASSERT_EQ_INT(0, craft_end(4294967040u, 0));
    char err[TAK_REPLAY_ERR_MAX] = "";
    ASSERT_NULL(TAK_Replay_Open(RP_FILE, NULL, NULL, err, sizeof err));
    remove(RP_FILE);
}

/* An end past the eight hour bound, checkpoints all the way, is refused
 * as surely as one with none. */
TEST(a_file_longer_than_eight_hours_is_refused) {
    TAK_ReplayHeader h = rp_header();
    ASSERT_EQ_INT(0, craft_begin(&h));
    for (uint32_t t = 60; t <= TAK_REPLAY_MAX_TICKS + 60; t += 60) craft_cp(t);
    ASSERT_EQ_INT(0, craft_end(TAK_REPLAY_MAX_TICKS + 60, 0));
    ASSERT_NULL(TAK_Replay_Open(RP_FILE, NULL, NULL, NULL, 0));
    remove(RP_FILE);
}

/* A command long after the last checkpoint is a stream no recorder
 * wrote: the game checkpoints every sixty ticks. */
TEST(a_record_far_past_its_last_checkpoint_is_refused) {
    TAK_ReplayHeader h = rp_header();
    ASSERT_EQ_INT(0, craft_begin(&h));
    craft_cp(60);
    craft_cmd(1000, rp_cmd(3));
    ASSERT_EQ_INT(0, craft_end(1001, 1));
    ASSERT_NULL(TAK_Replay_Open(RP_FILE, NULL, NULL, NULL, 0));
    remove(RP_FILE);
}

/* Probe 2: a correctly chained stream past the size guard. The writer
 * is let past it for the test. */
TEST(a_file_past_the_size_guard_is_refused) {
    TAK_ReplayHeader h = rp_header();
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, &h, NULL, 0);
    ASSERT_NOT_NULL(w);
    TAK_ReplayWriter_SetMaxBytes(w, TAK_REPLAY_MAX_BYTES + (2u << 20));
    static TAK_GameCommand big;
    memset(&big, 0, sizeof big);
    big.type = TAK_CMD_MOVE_FORMATION;
    big.unit_count = TAK_COMMAND_MAX_UNITS;
    big.seat = 1;
    uint32_t t = 0;
    while (TAK_ReplayWriter_Bytes(w) < TAK_REPLAY_MAX_BYTES + (1u << 20)) {
        if (t && t % 60 == 0) ASSERT_EQ_INT(0, TAK_ReplayWriter_Checkpoint(w, t, t));
        big.tick = t;
        ASSERT_EQ_INT(0, TAK_ReplayWriter_Command(w, &big));
        t++;
    }
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Close(w, t));
    ASSERT(rp_file_size(RP_FILE) > (long)TAK_REPLAY_MAX_BYTES);
    char err[TAK_REPLAY_ERR_MAX] = "";
    ASSERT_NULL(TAK_Replay_Open(RP_FILE, NULL, NULL, err, sizeof err));
    ASSERT(strstr(err, "larger") != NULL);
    TAK_ReplayHeader got;
    ASSERT_EQ_INT(-1, TAK_Replay_ReadHeader(RP_FILE, &got, err, sizeof err));
    remove(RP_FILE);
}

/* Probe 3: each field outside what the engine has, one at a time. */
/* Both sides of one rule: the writer will not record the spoiled
 * header, and the same spoil forged into a written file's bytes, with
 * its checksum put right, is refused by the reader. */
static int rp_forged_refused(void (*forge)(uint8_t *head)) {
    static uint8_t buf[1 << 16];
    if (rp_write(RP_FILE, 10, 1, NULL) != 0) return 0;
    FILE *f = fopen(RP_FILE, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    forge(buf);
    tak_put_u32(buf + 572, TAK_HashBytes(TAK_SIM_HASH_SEED, buf, 572));
    f = fopen(RP_FILE, "wb");
    if (!f) return 0;
    fwrite(buf, 1, n, f);
    fclose(f);
    TAK_ReplayHeader got;
    int head = TAK_Replay_ReadHeader(RP_FILE, &got, NULL, 0);
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, NULL, NULL, NULL, 0);
    if (r) TAK_Replay_Close(r);
    remove(RP_FILE);
    return head != 0 && r == NULL;
}

static int rp_header_refused(void (*spoil)(TAK_ReplayHeader *h),
                             void (*forge)(uint8_t *head)) {
    TAK_ReplayHeader h = rp_header();
    spoil(&h);
    if (TAK_Replay_HeaderValid(&h)) return 0;
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, &h, NULL, 0);
    if (w) {
        TAK_ReplayWriter_Close(w, 0);
        remove(RP_FILE);
        return 0;
    }
    return rp_forged_refused(forge);
}

/* Header offsets: flags 104, seat 105, turn 106, map 116, units 248,
 * seats from 260 at 38 bytes each (kind, side, team, colour,
 * difficulty, start, name). */
#define SEAT_AT(i, f) (260 + 38 * (i) + (f))

static void spoil_side(TAK_ReplayHeader *h)   { h->cfg.players[0].side = 250; }
static void forge_side(uint8_t *b)            { b[SEAT_AT(0, 1)] = 250; }
static void spoil_team(TAK_ReplayHeader *h)   { h->cfg.players[1].team = 200; }
static void forge_team(uint8_t *b)            { b[SEAT_AT(1, 2)] = 200; }
static void spoil_ai(TAK_ReplayHeader *h)     { h->cfg.players[2].ai_difficulty = 99; }
static void forge_ai(uint8_t *b)              { b[SEAT_AT(2, 4)] = 99; }
static void spoil_units(TAK_ReplayHeader *h)  { h->cfg.units_per_side = -5; }
static void forge_units(uint8_t *b)           { tak_put_u32(b + 248, (uint32_t)-5); }
static void spoil_units2(TAK_ReplayHeader *h) { h->cfg.units_per_side = TAK_UNITS_PER_SIDE_MAX + 1; }
static void forge_units2(uint8_t *b)          { tak_put_u32(b + 248, TAK_UNITS_PER_SIDE_MAX + 1); }
static void spoil_turn(TAK_ReplayHeader *h)   { h->turn_ticks = 200; }
static void forge_turn(uint8_t *b)            { b[106] = 200; }
static void spoil_flags(TAK_ReplayHeader *h)  { h->flags |= 0x80; }
static void forge_flags(uint8_t *b)           { b[104] |= 0x80; }
static void spoil_seat(TAK_ReplayHeader *h)   { h->local_seat = 5; }   /* a closed seat */
static void forge_seat(uint8_t *b)            { b[105] = 5; }
static void spoil_nobody(TAK_ReplayHeader *h) {
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) h->cfg.players[i].kind = TAK_SLOT_CLOSED;
}
static void forge_nobody(uint8_t *b) {
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) b[SEAT_AT(i, 0)] = TAK_SLOT_CLOSED;
}
static void spoil_map(TAK_ReplayHeader *h)    { snprintf(h->cfg.map_name, sizeof h->cfg.map_name, "../x"); }
static void forge_map(uint8_t *b)             { memcpy(b + 116, "../x", 5); }
/* A name the writer cleans rather than refuses, so only the forged file
 * can carry the byte. */
static void forge_name(uint8_t *b)            { b[SEAT_AT(1, 6) + 3] = '\t'; }

TEST(a_header_field_out_of_range_is_refused) {
    ASSERT(rp_header_refused(spoil_side, forge_side));
    ASSERT(rp_header_refused(spoil_team, forge_team));
    ASSERT(rp_header_refused(spoil_ai, forge_ai));
    ASSERT(rp_header_refused(spoil_units, forge_units));
    ASSERT(rp_header_refused(spoil_units2, forge_units2));
    ASSERT(rp_header_refused(spoil_turn, forge_turn));
    ASSERT(rp_header_refused(spoil_flags, forge_flags));
    ASSERT(rp_header_refused(spoil_seat, forge_seat));
    ASSERT(rp_header_refused(spoil_nobody, forge_nobody));
    ASSERT(rp_header_refused(spoil_map, forge_map));
    ASSERT(rp_forged_refused(forge_name));
}

/* A rule toggle is one or zero. The writer only writes those, so the
 * byte is set by hand, checksum and all. */
TEST(a_rule_toggle_that_is_not_zero_or_one_is_refused) {
    ASSERT_EQ_INT(0, rp_write(RP_FILE, 10, 1, NULL));
    static uint8_t buf[1 << 16];
    FILE *f = fopen(RP_FILE, "rb");
    ASSERT_NOT_NULL(f);
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    buf[252] = 2;   /* line of sight */
    tak_put_u32(buf + 572, TAK_HashBytes(TAK_SIM_HASH_SEED, buf, 572));
    f = fopen(RP_FILE, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(buf, 1, n, f);
    fclose(f);
    ASSERT_NULL(TAK_Replay_Open(RP_FILE, NULL, NULL, NULL, 0));
    remove(RP_FILE);
}

/* Re-check: a player's name reaches the writer as the room or the
 * settings file carried it, control bytes and all. What is recorded
 * has to play, so the name is recorded clean. */
TEST(a_player_name_with_a_tab_is_recorded_and_plays) {
    TAK_ReplayHeader h = rp_header();
    snprintf(h.cfg.players[1].name, sizeof h.cfg.players[1].name, "Bob\tSmith\x7f");
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, &h, NULL, 0);
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Checkpoint(w, 60, 1));
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Close(w, 60));
    TAK_ReplayHeader got;
    char err[TAK_REPLAY_ERR_MAX] = "";
    int rc = TAK_Replay_ReadHeader(RP_FILE, &got, err, sizeof err);
    if (rc) printf("(%s) ", err);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_STR("Bob Smith ", got.cfg.players[1].name);
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, NULL, NULL, NULL, 0);
    ASSERT_NOT_NULL(r);
    TAK_Replay_Close(r);
    remove(RP_FILE);
}

static uint32_t g_fuzz = 0x2545f491u;
static uint32_t fuzz(void) {
    g_fuzz ^= g_fuzz << 13; g_fuzz ^= g_fuzz >> 17; g_fuzz ^= g_fuzz << 5;
    return g_fuzz;
}
/* A value in range most of the time and any byte the rest, so both
 * sides of every limit are reached. */
static int fuzz_field(int lo, int hi) {
    if (fuzz() % 60) return lo + (int)(fuzz() % (uint32_t)(hi - lo + 1));
    return (int)(fuzz() % 256u);
}
static void fuzz_text(char *s, size_t cap) {
    size_t n = fuzz() % cap;
    for (size_t i = 0; i < n; i++) {
        uint32_t k = fuzz() % 8u;
        s[i] = (char)(k == 0 ? 1 + fuzz() % 31u : k == 1 ? 0x7f + fuzz() % 129u
                    : k == 2 ? "/\\:."[fuzz() % 4u] : 'a' + fuzz() % 26u);
    }
    s[n] = '\0';
}

/* Whatever the writer takes, the reader takes: one rule on both sides.
 * Headers are drawn at random across and past every limit. */
TEST(every_header_the_writer_takes_the_reader_takes) {
    int taken = 0, refused = 0;
    for (int n = 0; n < 3000; n++) {
        TAK_ReplayHeader h = rp_header();
        h.local_seat = (uint8_t)fuzz_field(1, TAK_MAX_PLAYERS);
        h.turn_ticks = (uint8_t)fuzz_field(0, 60);
        h.flags = (uint8_t)fuzz_field(0, 7);
        h.cfg.units_per_side = fuzz() % 20 ? fuzz_field(200, 2000)
                                          : (int)fuzz() - (int)(fuzz() >> 1);
        h.cfg.line_of_sight = fuzz_field(0, 1);
        h.cfg.power_codes = (int)fuzz();
        if (fuzz() % 20 == 0) fuzz_text(h.cfg.map_name, sizeof h.cfg.map_name);
        if (fuzz() % 3 == 0) fuzz_text(h.map_kingdom, sizeof h.map_kingdom);
        for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
            PlayerSlot *p = &h.cfg.players[i];
            p->kind = (TakSlotKind)fuzz_field(0, 2);
            p->side = fuzz_field(0, TAK_SIDES_MAX - 1);
            p->team = fuzz_field(0, 4);
            p->color = fuzz_field(0, TAK_PLAYER_COLOR_COUNT - 1);
            p->ai_difficulty = fuzz_field(0, 3);
            p->start_pos = fuzz_field(0, TAK_MAX_PLAYERS);
            if (fuzz() % 2) fuzz_text(p->name, sizeof p->name);
        }
        TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, &h, NULL, 0);
        if (!w) { refused++; continue; }
        TAK_ReplayWriter_Checkpoint(w, 60, 1);
        TAK_ReplayWriter_Close(w, 60);
        char err[TAK_REPLAY_ERR_MAX] = "";
        TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, NULL, NULL, err, sizeof err);
        if (!r) printf("(header %d written and refused: %s) ", n, err);
        ASSERT_NOT_NULL(r);
        TAK_Replay_Close(r);
        taken++;
    }
    remove(RP_FILE);
    printf("(%d taken, %d refused) ", taken, refused);
    ASSERT(taken > 100);
    ASSERT(refused > 100);
}

/* The writer keeps the same rules the reader holds it to. */
TEST(the_writer_keeps_within_a_checkpoint_and_eight_hours) {
    TAK_ReplayHeader h = rp_header();
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, &h, NULL, 0);
    ASSERT_NOT_NULL(w);
    TAK_GameCommand c = *rp_cmd(1);
    c.tick = 30;
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Command(w, &c));
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Checkpoint(w, 60, 1));
    c.tick = 500;                       /* no checkpoint since 60 */
    ASSERT_EQ_INT(-1, TAK_ReplayWriter_Command(w, &c));
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Close(w, 100000));
    uint32_t end = 0;
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, NULL, &end, NULL, 0);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(120, (int)end);       /* no further than it can vouch for */
    TAK_Replay_Close(r);
    remove(RP_FILE);
}

/* ── bounds ────────────────────────────────────────────────────────── */

/* A long battle costs the writer and the reader what one short battle
 * does: their own fixed size and nothing that grows with the log. */
TEST(memory_stays_the_same_whatever_the_length) {
    ASSERT(TAK_ReplayWriter_Footprint() < 32u * 1024u);
    ASSERT(TAK_ReplayReader_Footprint() < 32u * 1024u);

    TakMemStats before, after;
    tak_mem_reset_stats();
    tak_mem_get_stats(&before);
    ASSERT_EQ_INT(0, rp_write(RP_FILE, 20000, 1, NULL));
    tak_mem_get_stats(&after);
    ASSERT(after.peak_live_bytes - before.live_bytes <= TAK_ReplayWriter_Footprint() + 64u);
    ASSERT_EQ_INT((int)before.live_bytes, (int)after.live_bytes);
    ASSERT(rp_file_size(RP_FILE) > 256 * 1024);

    tak_mem_reset_stats();
    tak_mem_get_stats(&before);
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, NULL, NULL, NULL, 0);
    ASSERT_NOT_NULL(r);
    TAK_ReplayRecord rec;
    uint32_t cmds = 0;
    while (TAK_Replay_Next(r, &rec) == 1) if (rec.kind == TAK_REPLAY_REC_COMMAND) cmds++;
    TAK_Replay_Close(r);
    tak_mem_get_stats(&after);
    ASSERT_EQ_INT(20000, (int)cmds);
    ASSERT(after.peak_live_bytes - before.live_bytes <= TAK_ReplayReader_Footprint() + 64u);
    ASSERT_EQ_INT((int)before.live_bytes, (int)after.live_bytes);
    remove(RP_FILE);
}

/* Past the size guard the file takes no more, and closes at the tick
 * of the first command it turned away, so it still plays what it has. */
TEST(a_full_recording_stops_and_still_plays) {
    TAK_ReplayHeader h = rp_header();
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, &h, NULL, 0);
    ASSERT_NOT_NULL(w);
    static TAK_GameCommand big;
    memset(&big, 0, sizeof big);
    big.type = TAK_CMD_MOVE_FORMATION;
    big.unit_count = TAK_COMMAND_MAX_UNITS;
    big.seat = 1;
    uint32_t taken = 0, first_refused = 0;
    for (uint32_t i = 0; i < 20000; i++) {
        big.tick = i;
        /* As the game does, a checkpoint every sixty ticks. */
        if (i && i % 60 == 0 && TAK_ReplayWriter_Checkpoint(w, i, i) != 0) {
            first_refused = i;
            break;
        }
        if (TAK_ReplayWriter_Command(w, &big) == 0) { taken++; continue; }
        first_refused = i;
        break;
    }
    ASSERT(TAK_ReplayWriter_Full(w));
    ASSERT(first_refused > 0);
    ASSERT_EQ_INT(-1, TAK_ReplayWriter_Checkpoint(w, first_refused + 60, 1));
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Close(w, first_refused + 5000));
    ASSERT(rp_file_size(RP_FILE) <= (long)TAK_REPLAY_MAX_BYTES);

    TAK_ReplayHeader got;
    uint32_t playable = 0;
    TAK_ReplayReader *r = TAK_Replay_Open(RP_FILE, &got, &playable, NULL, 0);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT((int)first_refused, (int)playable);
    ASSERT_EQ_INT((int)taken, (int)got.command_count);
    TAK_Replay_Close(r);
    remove(RP_FILE);
}

TEST(ticks_never_go_backwards) {
    TAK_ReplayHeader h = rp_header();
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(RP_FILE, &h, NULL, 0);
    ASSERT_NOT_NULL(w);
    TAK_GameCommand c = *rp_cmd(40);
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Checkpoint(w, 60, 1));
    c.tick = 100;
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Command(w, &c));
    c.tick = 99;
    ASSERT_EQ_INT(-1, TAK_ReplayWriter_Command(w, &c));
    ASSERT_EQ_INT(-1, TAK_ReplayWriter_Checkpoint(w, 59, 1));
    c.seat = 0;
    c.tick = 100;
    ASSERT_EQ_INT(-1, TAK_ReplayWriter_Command(w, &c));
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Close(w, 160));
    remove(RP_FILE);
}

TEST(a_new_recording_is_named_for_when_it_started) {
    char slug[32];
    TAK_Replay_Slug(0, slug, sizeof slug);
    ASSERT_EQ_STR("19700101-000000", slug);
    TAK_Replay_Slug(1790000000u, slug, sizeof slug);
    ASSERT_EQ_STR("20260921-141320", slug);
    TAK_Replay_Slug(951782400u, slug, sizeof slug);     /* a leap day */
    ASSERT_EQ_STR("20000229-000000", slug);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("The replay file");
    RUN(what_is_written_reads_back_the_same);
    RUN(an_unfinished_recording_plays_to_its_last_checkpoint);
    RUN(a_new_recording_is_named_for_when_it_started);
    TEST_SUITE("Damage");
    RUN(a_cut_file_is_refused_at_every_length);
    RUN(a_damaged_byte_anywhere_is_refused);
    RUN(a_file_that_is_not_a_replay_is_refused);
    RUN(another_format_version_is_refused);
    TEST_SUITE("Hostile files");
    RUN(a_crafted_stream_that_keeps_the_rules_opens);
    RUN(an_end_tick_past_the_longest_battle_is_refused);
    RUN(a_file_longer_than_eight_hours_is_refused);
    RUN(a_record_far_past_its_last_checkpoint_is_refused);
    RUN(a_file_past_the_size_guard_is_refused);
    RUN(a_header_field_out_of_range_is_refused);
    RUN(a_rule_toggle_that_is_not_zero_or_one_is_refused);
    RUN(the_writer_keeps_within_a_checkpoint_and_eight_hours);
    RUN(a_player_name_with_a_tab_is_recorded_and_plays);
    RUN(every_header_the_writer_takes_the_reader_takes);
    TEST_SUITE("Compatibility");
    RUN(the_build_and_the_data_must_match);
    TEST_SUITE("Bounds");
    RUN(memory_stays_the_same_whatever_the_length);
    RUN(a_full_recording_stops_and_still_plays);
    RUN(ticks_never_go_backwards);
    TEST_REPORT();
}
