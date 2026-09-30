#ifndef TAK_REPLAY_SESSION_H
#define TAK_REPLAY_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "tak_platform.h"
#include "tak_replay.h"
#include "tak_savelist.h"

/*
 * Recording the battle being played, and playing a recorded one back.
 *
 * Recording sits on the command queue's observer, which sees every
 * command in the order the queue applies it, local and remote alike.
 * The AI gives no commands through the queue: every machine runs it
 * from the seed, and so does a replay.
 *
 * Playback puts each recorded command back into the queue at the tick
 * it ran on, just before that tick, and locks out every local order,
 * so the simulation is the same one a match runs and the player only
 * looks. At the recorded checkpoints the simulation hash is compared,
 * and a replay that leaves the recorded game says so.
 */

/* ── Recording ────────────────────────────────────────────────────── */

/* The battle that just opened is recorded if it can be: a skirmish or a
 * match started fresh, not a campaign mission, not a battle out of a
 * save, and not a replay. Off when the RecordReplays setting is 0. */
void Replay_RecordBattle(void);

/* Record the current world into `path`. 0, or -1 with `err` filled. */
int  Replay_RecordOpen(const char *path, char *err, size_t err_cap);

/* Close the recording at the queue's current tick. */
void Replay_RecordClose(void);

int  Replay_IsRecording(void);
/* The file being written, "" when none. */
const char *Replay_RecordPath(void);

/* ── Playback ─────────────────────────────────────────────────────── */

/* Every replay in the saved game directory, newest first, as rows for
 * the load dialog: the date on the row, the recorder's kingdom, the
 * map, the length, and the players in `detail`. A replay this build
 * cannot play keeps its row with the reason, as a save does. Free with
 * SaveList_Free. */
int  Replay_List(TAK_SaveEntry **out);

/* Keep the newest `keep` replays within `budget_bytes`, removing the
 * oldest first. Only .okreplay files are touched, and never
 * `spare_path`, the recording being written. Returns how many went. */
int  Replay_Prune(int keep, uint32_t budget_bytes, const char *spare_path);

/* Open a replay and check it plays here: the file, the engine build,
 * the game data and, when there is a map to hash, the map. 0, or -1
 * with the reason in words a player can act on. */
int  Replay_Open(const char *path, char *err, size_t err_cap);

/* The header of the replay opened, or NULL. */
const TAK_ReplayHeader *Replay_Header(void);

/* Build the recorded battle's world and start playing onto it. 0, and
 * the caller moves to the loading screen, or -1. */
int  Replay_BeginWorld(TAK_Platform *platform);

/* Start playing onto the world already built, which is how a test that
 * builds its own world drives playback. */
void Replay_Attach(void);

/* Stop playing or opening and let local orders through again. */
void Replay_Stop(void);

int  Replay_IsPlaying(void);

/* F1's Restart during playback: the same replay from its start. */
void Replay_RequestRestart(void);
/* Taken once after the battle closed. 0 when a restart began. */
int  Replay_TakeRestart(TAK_Platform *platform);

/* ── The tick ─────────────────────────────────────────────────────── */

/* Before the queue runs its tick: during playback, the commands
 * recorded for it go in. */
void Replay_BeforeOrders(void);

/* May the simulation run another tick? 0 once playback reaches the
 * last recorded tick. Outside playback always 1. */
int  Replay_CanAdvance(void);

/* Whether the tick just done wants the simulation hash, which a match
 * may also be computing. */
int  Replay_WantsHash(uint32_t done_tick);
/* The hash after `done_tick`'s orders: written when recording, compared
 * when playing. */
void Replay_NoteHash(uint32_t done_tick, uint32_t hash);

/* ── Playback controls ────────────────────────────────────────────── */

/* 1, 2, 4 or 8. */
int  Replay_Speed(void);
void Replay_SetSpeed(int speed);
/* One step up or down the four speeds. */
void Replay_StepSpeed(int dir);
int  Replay_Paused(void);
void Replay_SetPaused(int paused);
/* What the timer runs at: 0 while paused. */
double Replay_Rate(void);

/* The playback keys, edge detected: Space and Pause pause, = and -
 * step the speed. */
void Replay_ApplyKeys(const uint8_t *keys, const uint8_t *prev);

/* The first checkpoint whose hash differed, or 0 when none has. */
uint32_t Replay_DriftTick(void);
/* Where playback stands and where it ends. */
uint32_t Replay_Tick(void);
uint32_t Replay_EndTick(void);

/* The line the battle shows during playback, and a second line when
 * the replay drifted. Each returns 0 and writes "" when there is
 * nothing to say. */
int  Replay_StatusLine(char *out, size_t cap);
int  Replay_DriftLine(char *out, size_t cap);

#endif /* TAK_REPLAY_SESSION_H */
