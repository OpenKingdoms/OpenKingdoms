#ifndef TAK_MUSIC_H
#define TAK_MUSIC_H

/* ══════════════════════════════════════════════════════════════════════
 *  TAK Music Streaming System
 *
 *  Replaces MusicSys_* from the legacy reference (lines 308412-308820).
 *
 *  Music tracks are WAV files in the game's Music/ directory (outside
 *  HPI archives — loose filesystem files). They are STREAMED, not
 *  loaded into memory, because each track is 3-5 MB and there are 20.
 *
 *  Three playback modes (matching the original engine):
 *    OFF        — no music
 *    SEQUENTIAL — track1 → track2 → ... → track20 → track1 (loop)
 *    SHUFFLE    — randomized order, reshuffled each cycle
 *
 *  Volume is independent from the SFX volume — players can have loud
 *  effects but quiet music, or vice versa, via the Options screen.
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum TAK_MusicMode {
    TAK_MUSIC_OFF        = 0,
    TAK_MUSIC_SEQUENTIAL = 1,
    TAK_MUSIC_SHUFFLE    = 2,
} TAK_MusicMode;

/* Initialize the music system. Scans the Music/ directory in the game
 * folder for track*.wav files. Call after TAK_Sound_Init.
 * game_dir is the root game directory (same as TAK_GAME_DIR). */
int  TAK_Music_Init(const char *game_dir);
void TAK_Music_Shutdown(void);

/* Call once per frame. Checks if the current track finished and
 * auto-advances to the next one (in sequential or shuffle mode). */
void TAK_Music_Update(void);

/* Set playback mode. Changing to OFF stops playback. Changing to
 * SEQUENTIAL or SHUFFLE starts from track 1 (or a random track). */
void TAK_Music_SetMode(TAK_MusicMode mode);
TAK_MusicMode TAK_Music_GetMode(void);

/* Volume: 0-127 (independent of SFX volume). Default: 64 (50%). */
void TAK_Music_SetVolume(int vol_0_127);
int  TAK_Music_GetVolume(void);

/* Play a specific track (1-based, like the original). */
void TAK_Music_PlayTrack(int track_number);

/* Pause / resume. */
void TAK_Music_Pause(int pause);

/* Query. */
int  TAK_Music_IsPlaying(void);
int  TAK_Music_GetTrackCount(void);

#endif /* TAK_MUSIC_H */
