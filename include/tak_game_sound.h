#ifndef TAK_GAME_SOUND_H
#define TAK_GAME_SOUND_H

/* Game-level sound dispatcher: turns gameplay events into wav plays.
 * Wavs load on first use through the VFS and stay cached. The legacy
 * game-side sound functions live at legacy:220920-221891. */

int  GameSound_Init(void);
void GameSound_Shutdown(void);

/* Voice line for a unit sound class action ("select", "move", "attack",
 * "guard", "patrol", "default"). Weighted pick from the class table,
 * then a centre-panned play at full volume, priority 7. The original
 * never positions these (legacy:221569-221630). */
void GameSound_UnitVoice(const char *soundclass_name, const char *action);

/* Impact by hit class ("sword", "arrow", "cannon", ...) and material
 * (the bodytype of the unit struck, NULL for ground). Priority 4
 * (legacy:245014). */
void GameSound_WeaponHit(const char *hitclass, const char *material,
                          int world_x, int world_y,
                          int cam_x, int cam_y,
                          int viewport_w, int viewport_h);

/* Interface cue: centre pan, full volume, priority 7 (legacy:221090). */
void GameSound_PlayUI(const char *wav_name);

/* Any centre-panned play with an explicit volume and priority. Widget
 * clicks use 0x55 at priority 4 (legacy:332867). */
void GameSound_Play2D(const char *wav_name, int volume, int priority);

/* Positional play of a bare wav name. Volume follows the viewport rule
 * in TAK_Sound_Spatialize, priority is the category the caller passes
 * (legacy:221131-221200). */
void GameSound_PlayWorldWav(const char *wav_name, int priority,
                            int world_x, int world_y,
                            int cam_x, int cam_y,
                            int viewport_w, int viewport_h);

/* Sound class entry played flat at a given volume: the ambient feature
 * loop wants subclass 0 at 0x40 (legacy:128684). action NULL = first. */
void GameSound_PlayClass2D(const char *soundclass_name, const char *action,
                           int volume, int priority);

/* Debug recorder: keeps what would have played so tests can assert
 * without an audio device. Records after name resolution and gating. */
typedef struct GameSoundEvent {
    char name[64];       /* wav name handed to the loader */
    int  volume;         /* 0..127 after the viewport rule */
    int  pan;            /* 0..127, 64 is centre */
    int  priority;
    int  positional;     /* 1 when a world position was given */
    int  world_x, world_y;
    int  loaded;         /* 1 when the wav resolved through the VFS */
} GameSoundEvent;

void GameSound_DebugRecord(int enable);
void GameSound_DebugClear(void);
int  GameSound_DebugCount(void);
const GameSoundEvent *GameSound_DebugEvent(int index);
/* Newest event whose name starts with prefix (case-insensitive), or -1. */
int  GameSound_DebugFindPrefix(const char *prefix);
int  GameSound_DebugCountPrefix(const char *prefix);

/* Test seam: the next name the cache is asked to keep fails to store,
 * so a test can check that a wav the cache cannot hold is not handed
 * out and not left behind. */
void GameSound_DebugFailCacheInsertOnce(void);

#endif /* TAK_GAME_SOUND_H */
