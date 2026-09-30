/*
 * replay_session.c: recording the battle in play, and playing one back.
 *
 * See tak_replay_session.h. The file itself is net/replay.c's, and
 * this is only where it meets the queue, the world and the screens.
 */

#include "tak_replay_session.h"

#include "tak_command_emit.h"
#include "tak_command_queue.h"
#include "tak_data_fingerprint.h"
#include "tak_gameloop.h"
#include "tak_ingame_keys.h"
#include "tak_map_fingerprint.h"
#include "tak_mission_script.h"
#include "tak_net_match.h"
#include "tak_net_protocol.h"
#include "tak_net_session.h"
#include "tak_paths.h"
#include "tak_perf_probe.h"
#include "tak_savelist.h"
#include "tak_settings.h"
#include "tak_sides.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Push the file out this often, in checkpoints: a tab that closes
 * loses at most this much of the battle. */
#define REC_FLUSH_EVERY   10
#define REC_NOTIFY_EVERY  60

static struct {
    TAK_ReplayWriter *w;
    char     path[TAK_SAVE_PATH_MAX];
    uint32_t checkpoints;
    uint8_t  said_full;
} g_rec;

static struct {
    TAK_ReplayReader *reader;
    TAK_ReplayHeader  hdr;
    uint32_t end;
    uint8_t  playing;
    uint8_t  has_next;
    uint8_t  bad;
    uint8_t  paused;
    int      speed;
    uint32_t drift_tick;
    TAK_ReplayRecord next;
} g_play;

/* Kept past a stop, so F1's Restart can open it again. */
static char g_play_path[TAK_SAVE_PATH_MAX];
static int  g_play_restart;

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

/* ── recording ────────────────────────────────────────────────────── */

static void rec_observe(const TAK_GameCommand *cmd, void *user) {
    (void)user;
    if (!g_rec.w) return;
    if (TAK_ReplayWriter_Command(g_rec.w, cmd) != 0 &&
        TAK_ReplayWriter_Full(g_rec.w) && !g_rec.said_full) {
        g_rec.said_full = 1;
        fprintf(stderr, "Replay: the recording is full and ends here\n");
    }
}

int Replay_IsRecording(void) { return g_rec.w != NULL; }
const char *Replay_RecordPath(void) { return g_rec.w ? g_rec.path : ""; }

int Replay_RecordOpen(const char *path, char *err, size_t err_cap) {
    Replay_RecordClose();
    const GameWorld *world = World_Get();
    if (!world || !path) {
        set_err(err, err_cap, "No battle to record.");
        return -1;
    }
    TAK_ReplayHeader h;
    memset(&h, 0, sizeof h);
    h.engine_build_id = TAK_ENGINE_BUILD_ID;
    const TAK_DataFingerprint *fp = TAK_DataFingerprint_Get();
    if (fp) {
        h.data_schema = fp->schema;
        h.data_content = fp->content;
        for (int g = 0; g < TAK_DATA_GROUP_COUNT; g++) h.data_group[g] = fp->group[g];
    }
    if (TAK_MapFingerprint_FromName(world->map_name, h.map_fp) == 0)
        h.flags |= TAK_REPLAYF_MAP_FP;
    if (TAK_Match_IsLive()) {
        h.flags |= TAK_REPLAYF_MATCH;
        const TAK_NetClient *c = NetSession_Client();
        if (c) h.turn_ticks = c->start.turn_ticks;
    }
    int me = Units_LocalPlayer();
    h.local_seat = (uint8_t)(me >= 1 && me <= TAK_MAX_PLAYERS ? me : 1);
    h.recorded_at_utc = (uint64_t)time(NULL);
    h.cfg = world->cfg;
    /* The name the world was built from, which is what builds it again. */
    snprintf(h.cfg.map_name, sizeof h.cfg.map_name, "%s", world->map_name);
    snprintf(h.map_kingdom, sizeof h.map_kingdom, "%s", world->map_kingdom);

    g_rec.w = TAK_ReplayWriter_Open(path, &h, err, err_cap);
    if (!g_rec.w) return -1;
    snprintf(g_rec.path, sizeof g_rec.path, "%s", path);
    g_rec.checkpoints = 0;
    g_rec.said_full = 0;
    TAK_CmdQueue_SetObserver(rec_observe, NULL);
    return 0;
}

void Replay_RecordClose(void) {
    if (!g_rec.w) return;
    TAK_CmdQueue_SetObserver(NULL, NULL);
    uint32_t end = TAK_CmdQueue_Tick();
    int rc = TAK_ReplayWriter_Close(g_rec.w, end);
    g_rec.w = NULL;
    /* Under a second of battle is nothing anyone will want to watch. */
    if (rc != 0 || end < TAK_REPLAY_HASH_EVERY) {
        remove(g_rec.path);
    } else {
        fprintf(stderr, "Replay: recorded %s\n", g_rec.path);
    }
    Paths_NotifyPrefWritten();
    g_rec.path[0] = '\0';
}

void Replay_RecordBattle(void) {
    if (g_play.playing || g_rec.w) return;
    if (!Settings_GetInt("RecordReplays", 1)) return;
    if (PerfProbe_Active()) return;
    const GameWorld *world = World_Get();
    if (!world || !world->loaded) return;
    /* A mission's script and its placements are not in the header. */
    if (world->mission.path[0] || world->mission.objective_count > 0 ||
        MissionScript_HasScript()) return;
    /* A battle out of a save starts part way through, and a replay
     * starts from the seed. */
    if (World_IsRestoring() || TAK_CmdQueue_Tick() != 0) return;

    char slug[32];
    TAK_Replay_Slug((uint64_t)time(NULL), slug, sizeof slug);
    const char *dir = Paths_SaveDir();
    char path[TAK_SAVE_PATH_MAX];
    snprintf(path, sizeof path, "%s%s%s", dir, slug, TAK_REPLAY_EXT);
    for (int n = 2; n < 100; n++) {
        FILE *f = fopen(path, "rb");
        if (!f) break;
        fclose(f);
        snprintf(path, sizeof path, "%s%s-%d%s", dir, slug, n, TAK_REPLAY_EXT);
    }
    char err[TAK_REPLAY_ERR_MAX];
    if (Replay_RecordOpen(path, err, sizeof err) != 0)
        fprintf(stderr, "Replay: not recording: %s\n", err);
}

/* ── the list ─────────────────────────────────────────────────────── */

static void list_players(const BattleConfig *cfg, char *out, size_t cap) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        const PlayerSlot *p = &cfg->players[i];
        if (p->kind == TAK_SLOT_CLOSED) continue;
        char name[40];
        if (p->name[0]) snprintf(name, sizeof name, "%s", p->name);
        else if (p->kind == TAK_SLOT_AI) snprintf(name, sizeof name, "Computer");
        else snprintf(name, sizeof name, "Player %d", i + 1);
        int n = snprintf(out + len, cap - len, "%s%s", len ? ", " : "", name);
        if (n < 0 || (size_t)n >= cap - len) break;
        len += (size_t)n;
    }
}

static void list_fill(TAK_SaveEntry *e) {
    char err[TAK_REPLAY_ERR_MAX];
    TAK_ReplayHeader h;
    uint32_t end = 0;
    snprintf(e->game_time, sizeof e->game_time, "--:--:--");
    TAK_ReplayReader *r = TAK_Replay_Open(e->path, &h, &end, err, sizeof err);
    if (!r) {
        e->readable = 0;
        snprintf(e->refusal, sizeof e->refusal, "%s", err);
        return;
    }
    TAK_Replay_Close(r);
    e->readable = 1;
    if (TAK_Replay_CheckCompatible(&h, TAK_ENGINE_BUILD_ID, TAK_DataFingerprint_Get(),
                                   err, sizeof err) != 0) {
        e->readable = 0;
        snprintf(e->refusal, sizeof e->refusal, "%s", err);
    }
    snprintf(e->map, sizeof e->map, "%s", h.cfg.map_name);
    Sides_DisplayName(h.cfg.players[h.local_seat - 1].side, e->side, sizeof e->side);
    SaveList_FormatTime(end, SIM_TICKS_PER_SECOND, e->game_time, sizeof e->game_time);
    list_players(&h.cfg, e->detail, sizeof e->detail);
    e->saved_at_utc = h.recorded_at_utc;
    /* The row is when it was played, in the player's own time. */
    time_t t = (time_t)h.recorded_at_utc;
    struct tm *lt = localtime(&t);
    if (lt) strftime(e->slug, sizeof e->slug, "%Y-%m-%d %H:%M", lt);
}

static int list_newest_first(const void *a, const void *b) {
    const TAK_SaveEntry *x = (const TAK_SaveEntry *)a;
    const TAK_SaveEntry *y = (const TAK_SaveEntry *)b;
    if (x->saved_at_utc != y->saved_at_utc) return x->saved_at_utc > y->saved_at_utc ? -1 : 1;
    return strcmp(x->path, y->path);
}

int Replay_List(TAK_SaveEntry **out) {
    int n = SaveList_ScanExt(TAK_REPLAY_EXT, list_fill, out);
    if (n > 1 && out && *out) qsort(*out, (size_t)n, sizeof **out, list_newest_first);
    return n;
}

/* ── playback ─────────────────────────────────────────────────────── */

int Replay_IsPlaying(void) { return g_play.playing; }

const TAK_ReplayHeader *Replay_Header(void) {
    return g_play.reader ? &g_play.hdr : NULL;
}

void Replay_Stop(void) {
    if (g_play.reader) TAK_Replay_Close(g_play.reader);
    if (g_play.playing) TAK_Cmd_SetLocked(0);
    memset(&g_play, 0, sizeof g_play);
}

int Replay_Open(const char *path, char *err, size_t err_cap) {
    Replay_Stop();
    TAK_ReplayHeader h;
    uint32_t end = 0;
    TAK_ReplayReader *r = TAK_Replay_Open(path, &h, &end, err, err_cap);
    if (!r) return -1;
    if (TAK_Replay_CheckCompatible(&h, TAK_ENGINE_BUILD_ID, TAK_DataFingerprint_Get(),
                                   err, err_cap) != 0) {
        TAK_Replay_Close(r);
        return -1;
    }
    if (h.flags & TAK_REPLAYF_MAP_FP) {
        uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
        if (TAK_MapFingerprint_FromName(h.cfg.map_name, fp) != 0) {
            if (err && err_cap)
                snprintf(err, err_cap, "This replay was played on %s, which is not installed.",
                         h.cfg.map_name);
            TAK_Replay_Close(r);
            return -1;
        }
        if (memcmp(fp, h.map_fp, sizeof fp) != 0) {
            if (err && err_cap)
                snprintf(err, err_cap,
                         "Your copy of %s is not the map this replay was recorded on.",
                         h.cfg.map_name);
            TAK_Replay_Close(r);
            return -1;
        }
    }
    g_play.reader = r;
    g_play.hdr = h;
    g_play.end = end;
    if (path != g_play_path) snprintf(g_play_path, sizeof g_play_path, "%s", path);
    return 0;
}

void Replay_Attach(void) {
    if (!g_play.reader) return;
    g_play.playing = 1;
    g_play.has_next = 0;
    g_play.bad = 0;
    g_play.paused = 0;
    g_play.speed = 1;
    g_play.drift_tick = 0;
    TAK_Cmd_SetLocked(1);
}

int Replay_BeginWorld(TAK_Platform *platform) {
    if (!g_play.reader) return -1;
    /* A lobby left open would have the loading screen wait for a match. */
    NetSession_Disconnect();
    if (World_BeginLoad(platform, &g_play.hdr.cfg, g_play.hdr.cfg.map_name,
                        g_play.hdr.map_kingdom) != 0) {
        Replay_Stop();
        return -1;
    }
    /* The recorder's seat, for its view of the battle: its fog and its
     * sidebar. The simulation is the same from every seat. */
    Units_SetLocalPlayer(g_play.hdr.local_seat);
    Replay_Attach();
    return 0;
}

void Replay_RequestRestart(void) {
    if (g_play.playing) g_play_restart = 1;
}

int Replay_TakeRestart(TAK_Platform *platform) {
    if (!g_play_restart) return -1;
    g_play_restart = 0;
    char err[TAK_REPLAY_ERR_MAX];
    if (Replay_Open(g_play_path, err, sizeof err) != 0) {
        fprintf(stderr, "Replay: restart refused: %s\n", err);
        return -1;
    }
    return Replay_BeginWorld(platform);
}

/* The next record into the lookahead. A damaged one ends playback
 * where it stands. */
static int play_fetch(void) {
    if (g_play.has_next) return 1;
    if (g_play.bad || !g_play.reader) return 0;
    int rc = TAK_Replay_Next(g_play.reader, &g_play.next);
    if (rc < 0) {
        g_play.bad = 1;
        fprintf(stderr, "Replay: a damaged record at tick %u\n", TAK_CmdQueue_Tick());
    }
    g_play.has_next = rc > 0;
    return g_play.has_next;
}

void Replay_BeforeOrders(void) {
    if (!g_play.playing) return;
    uint32_t tick = TAK_CmdQueue_Tick();
    while (play_fetch()) {
        const TAK_ReplayRecord *n = &g_play.next;
        if (n->tick > tick) break;
        if (n->kind == TAK_REPLAY_REC_COMMAND &&
            TAK_CmdQueue_SubmitAt(n->cmd) != 0) {
            fprintf(stderr, "Replay: the queue refused a %s at tick %u\n",
                    TAK_CommandTypeName(n->cmd->type), tick);
        }
        g_play.has_next = 0;
    }
}

int Replay_CanAdvance(void) {
    if (!g_play.playing) return 1;
    if (g_play.bad) return 0;
    return TAK_CmdQueue_Tick() < g_play.end;
}

int Replay_WantsHash(uint32_t done_tick) {
    if (!g_rec.w && !g_play.playing) return 0;
    return done_tick % TAK_REPLAY_HASH_EVERY == 0;
}

void Replay_NoteHash(uint32_t done_tick, uint32_t hash) {
    if (g_rec.w) {
        if (TAK_ReplayWriter_Checkpoint(g_rec.w, done_tick, hash) == 0) {
            g_rec.checkpoints++;
            if (g_rec.checkpoints % REC_FLUSH_EVERY == 0) (void)TAK_ReplayWriter_Flush(g_rec.w);
            if (g_rec.checkpoints % REC_NOTIFY_EVERY == 0) Paths_NotifyPrefWritten();
        }
    }
    if (!g_play.playing) return;
    while (play_fetch()) {
        const TAK_ReplayRecord *n = &g_play.next;
        if (n->kind != TAK_REPLAY_REC_CHECKPOINT || n->tick > done_tick) break;
        if (n->tick == done_tick && n->hash != hash && !g_play.drift_tick) {
            g_play.drift_tick = done_tick;
            fprintf(stderr, "Replay: drifted at tick %u: recorded %08x, now %08x\n",
                    done_tick, n->hash, hash);
        }
        g_play.has_next = 0;
    }
}

/* ── controls ─────────────────────────────────────────────────────── */

static const int k_speeds[] = { 1, 2, 4, 8 };
#define SPEED_COUNT ((int)(sizeof k_speeds / sizeof k_speeds[0]))

int  Replay_Speed(void) { return g_play.speed ? g_play.speed : 1; }

void Replay_SetSpeed(int speed) {
    for (int i = 0; i < SPEED_COUNT; i++)
        if (k_speeds[i] == speed) g_play.speed = speed;
}

void Replay_StepSpeed(int dir) {
    int at = 0;
    for (int i = 0; i < SPEED_COUNT; i++) if (k_speeds[i] == Replay_Speed()) at = i;
    at += dir > 0 ? 1 : dir < 0 ? -1 : 0;
    if (at < 0) at = 0;
    if (at >= SPEED_COUNT) at = SPEED_COUNT - 1;
    g_play.speed = k_speeds[at];
}

int  Replay_Paused(void) { return g_play.paused; }
void Replay_SetPaused(int paused) { g_play.paused = paused ? 1 : 0; }

double Replay_Rate(void) {
    if (!g_play.playing || g_play.paused) return g_play.playing ? 0.0 : 1.0;
    return (double)Replay_Speed();
}

void Replay_ApplyKeys(const uint8_t *keys, const uint8_t *prev) {
    if (!g_play.playing || !keys || !prev) return;
    if ((keys[SDL_SCANCODE_SPACE] && !prev[SDL_SCANCODE_SPACE]) ||
        (keys[SDL_SCANCODE_PAUSE] && !prev[SDL_SCANCODE_PAUSE]))
        g_play.paused = !g_play.paused;
    switch (InGame_SpeedKey(keys, prev)) {
    case IG_SPEED_KEY_UP:   Replay_StepSpeed(1); break;
    case IG_SPEED_KEY_DOWN: Replay_StepSpeed(-1); break;
    default: break;
    }
}

uint32_t Replay_DriftTick(void) { return g_play.drift_tick; }
uint32_t Replay_Tick(void) { return TAK_CmdQueue_Tick(); }
uint32_t Replay_EndTick(void) { return g_play.end; }

int Replay_StatusLine(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!g_play.playing) return 0;
    char now[16], end[16];
    SaveList_FormatTime(TAK_CmdQueue_Tick(), SIM_TICKS_PER_SECOND, now, sizeof now);
    SaveList_FormatTime(g_play.end, SIM_TICKS_PER_SECOND, end, sizeof end);
    if (!Replay_CanAdvance()) {
        snprintf(out, cap, "Replay ended at %s.", now);
    } else if (g_play.paused) {
        snprintf(out, cap, "Replay paused at %s of %s. Space plays.", now, end);
    } else {
        snprintf(out, cap, "Replay %dx, %s of %s. Space pauses, - and = set the speed.",
                 Replay_Speed(), now, end);
    }
    return 1;
}

int Replay_DriftLine(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!g_play.playing) return 0;
    if (g_play.bad) {
        snprintf(out, cap, "The replay is damaged past this point.");
        return 1;
    }
    if (!g_play.drift_tick) return 0;
    char at[16];
    SaveList_FormatTime(g_play.drift_tick, SIM_TICKS_PER_SECOND, at, sizeof at);
    snprintf(out, cap, "This battle has drifted from the recording since %s.", at);
    return 1;
}
