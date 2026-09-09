#ifndef TAK_SOUND_H
#define TAK_SOUND_H

#include <stdint.h>
#include <stddef.h>

/* ══════════════════════════════════════════════════════════════════════
 *  TAK Sound System
 *
 *  Replaces the original Miles Sound System (mss32.dll) with miniaudio.
 *  See docs/THIRDPARTY_MILES_SOUND.md for the full AIL_* → ma_* mapping.
 *
 *  Architecture mirrors the original engine (the legacy reference 307979-308850):
 *    - Sound effects loaded from VFS into memory (WAV PCM data)
 *    - Fixed pool of channels with priority-based eviction
 *    - Volume 0-127, pan 0-127 (0=left, 64=center, 127=right)
 *    - Separate music streaming system (see tak_music.h)
 *
 *  All game WAVs are 8-bit mono PCM at 11025 Hz. miniaudio resamples
 *  to the device's native rate automatically.
 * ════════════════════════════════════════════════════════════════════���═ */

/* ── Opaque handle for a loaded sound effect ────────────────────────
 *
 * Represents a WAV loaded into memory. Multiple channels can play the
 * same effect simultaneously (the original used _AIL_quick_copy_4 for
 * overlapping playback of the same sound, e.g. multiple archers
 * firing). Each TAK_Sound_Play creates a new ma_sound instance from
 * this shared source data. */
typedef struct TAK_SoundEffect TAK_SoundEffect;

/* ── Init / Shutdown ────────────────────────────────────────────────
 *
 * TAK_Sound_Init opens the audio device via miniaudio's high-level
 * engine API. The original engine called:
 *   _AIL_quick_startup_20(1, 0, 0x5622, 0x10, 2)
 * which means: digital audio, 22050 Hz, 16-bit, stereo.
 * miniaudio auto-detects the optimal device configuration.
 *
 * Call after VFS_Init (we need VFS for loading sounds).
 * Call TAK_Sound_Shutdown before VFS_Shutdown. */
int  TAK_Sound_Init(void);
void TAK_Sound_Shutdown(void);
int  TAK_Sound_IsInitialized(void);

/* ── Per-frame update ───────────────────────────────────────────────
 *
 * Scans all channels, frees any that finished playing, updates the
 * active-channel count. The original called SoundSystem_PeriodicUpdate
 * (line 308102) from the main loop. Call once per frame. */
void TAK_Sound_Update(void);

/* ── Volume ─────────────────────────────────────────────────────────
 *
 * Master SFX volume. Range 0-127, matching the original Miles API.
 * Internally normalized to 0.0-1.0 for miniaudio.
 * Default: 100 (~78%). The original default was 0x7F (127). */
void TAK_Sound_SetMasterVolume(int vol_0_127);
int  TAK_Sound_GetMasterVolume(void);

/* ── Channel configuration ──────────────────────────────────────────
 *
 * Max simultaneous playing sounds. The original defaulted to 8 via
 * SoundSystem_SetMaxChannels (line 308211). Setting this lower
 * increases the chance of priority eviction; setting higher uses
 * more CPU for mixing. */
void TAK_Sound_SetMaxChannels(int max);

/* ── Loading ────────────────────────────────────────────────────────
 *
 * Load a WAV file from VFS into memory. Returns NULL on failure.
 * The original used _AIL_mem_alloc_lock_4 + _AIL_quick_load_mem_8.
 * Caller must eventually call TAK_Sound_Unload to free. */
TAK_SoundEffect *TAK_Sound_LoadWAV(const char *vfs_path);

/* Load from an already-loaded memory buffer (e.g. from VFS_ReadFile).
 * The buffer is copied internally — caller can free the original. */
TAK_SoundEffect *TAK_Sound_LoadWAVFromMemory(const void *data, uint32_t size);

/* Unload a loaded sound, freeing PCM data and decoder resources. */
void TAK_Sound_Unload(TAK_SoundEffect *sfx);

/* ── Playback ───────────────────────────────────────────────────────
 *
 * Play a loaded sound on the next available channel.
 *   volume:   0-127 (per-sound volume, scaled by master)
 *   pan:      0-127 (0=hard left, 64=center, 127=hard right)
 *   priority: higher = harder to evict (original range 0-7)
 *
 * Returns 1 on success, 0 if all channels are busy and no lower-
 * priority sound could be evicted. Safe no-op if sound system is
 * not initialized or sfx is NULL.
 *
 * The original's equivalent call chain:
 *   SoundSys_PlaySound(soundId, pan, volume, priority, volume, loop)
 *   → SoundSystem_FindFreeChannel(priority)
 *   → _AIL_quick_copy_4 + _AIL_quick_set_volume_12 + _AIL_quick_play_8 */
int TAK_Sound_Play(TAK_SoundEffect *sfx, int volume, int pan, int priority);

/* Stop all currently playing sounds. */
void TAK_Sound_StopAll(void);

/* ── Positional audio ───────────────────────────────────────────────
 *
 * TA:K simulates spatial audio with stereo panning + distance
 * attenuation. No HRTF or true 3D — just L/R balance based on where
 * the source is relative to the camera, and volume falloff for
 * off-screen sources.
 *
 * The original engine's Sound_Play3DExtended (line 221131) calculates:
 *   pan = ((source_x - cam_center_x) * 64) / (viewport_w * 16) + 64
 *   volume *= distance_attenuation_factor
 *
 * These helpers compute pan and attenuation from world coordinates
 * and the current camera state, then call TAK_Sound_Play internally. */

/* Play a sound at a world position. Calculates pan and distance
 * attenuation from the current camera. cam_x/cam_y are in world
 * coords (pixels), viewport_w/h are in screen pixels.
 * Returns 1 on success, 0 if sound couldn't be played. */
int TAK_Sound_PlayPositional(TAK_SoundEffect *sfx, int volume, int priority,
                              int world_x, int world_y,
                              int cam_x, int cam_y,
                              int viewport_w, int viewport_h);

/* 2D convenience: play with explicit pan, no attenuation.
 * For UI sounds, menu clicks, etc. */
int TAK_Sound_Play2D(TAK_SoundEffect *sfx, int volume, int pan);

#endif /* TAK_SOUND_H */
