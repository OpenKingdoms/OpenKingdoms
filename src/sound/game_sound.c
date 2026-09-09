/*
 * game_sound.c — Game-level sound dispatcher (Phase 7)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  GAME AUDIO LESSON: Wiring audio into gameplay events
 * ═══════════════════════════════════════════════════════════════════
 *
 * This file is the glue between gameplay events (unit selected, sword
 * hits armor, button clicked) and the audio system. It handles:
 *
 * 1. ON-DEMAND LOADING WITH CACHING
 *    The first time a sound is needed, it's loaded from the VFS and
 *    decoded. After that, it's cached in a simple hash table so the
 *    second play is instant. The original engine did the same thing
 *    via StringTable_FindOrAdd (line 221090) — lazy-load on first use.
 *
 * 2. SOUND CLASS DISPATCH
 *    When a unit is selected, we don't play a specific WAV. We look up
 *    the unit's soundclass (from its FBI file), ask the sound class
 *    system for a weighted-random sound name, then load+play that.
 *    This gives automatic audio variety — the same action produces
 *    different sounds each time.
 *
 * 3. PATH CONSTRUCTION
 *    Sound names in the TDF files don't include paths or extensions.
 *    "TONEARA" becomes "sounds/TONEARA.wav" for VFS lookup. The
 *    original engine had a similar name→path resolver.
 */

#include "tak_game_sound.h"
#include "tak_sound.h"
#include "tak_soundclass.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>

/* ── Sound cache ─────────────────────────────────────────────────── */

#define CACHE_MAX 512

typedef struct {
    char             name[64];  /* Normalized sound name (uppercase) */
    TAK_SoundEffect *effect;
} CacheEntry;

static CacheEntry s_cache[CACHE_MAX];
static int        s_cache_count = 0;
static int        s_initialized = 0;

/* ── Cache lookup / insert ───────────────────────────────────────── */

static TAK_SoundEffect *cache_find(const char *name) {
    for (int i = 0; i < s_cache_count; i++) {
        if (tak_stricmp(s_cache[i].name, name) == 0)
            return s_cache[i].effect;
    }
    return NULL;
}

static void cache_insert(const char *name, TAK_SoundEffect *sfx) {
    if (s_cache_count >= CACHE_MAX) return;  /* cache full, skip */
    CacheEntry *ce = &s_cache[s_cache_count++];
    strncpy(ce->name, name, sizeof(ce->name) - 1);
    ce->name[sizeof(ce->name) - 1] = '\0';
    ce->effect = sfx;
}

/* Try to load a sound by name. Searches several VFS paths since the
 * original engine stores sounds in language-specific directories.
 * Caches the result (even NULL on failure, to avoid retrying). */
static TAK_SoundEffect *get_or_load(const char *name) {
    if (!name || name[0] == '\0') return NULL;

    /* Check cache first */
    TAK_SoundEffect *sfx = cache_find(name);
    if (sfx) return sfx;

    /* Check if we already failed to load this name (NULL sentinel) */
    for (int i = 0; i < s_cache_count; i++) {
        if (tak_stricmp(s_cache[i].name, name) == 0) return NULL;
    }

    /* Try several VFS paths. TA:K stores sounds in:
     *   sounds/<name>.wav          (primary)
     *   english/sounds/<name>.wav  (language-specific) */
    char path[256];
    const char *prefixes[] = { "sounds/", "english/sounds/", "" };

    for (int p = 0; p < 3; p++) {
        /* Try with .wav extension */
        snprintf(path, sizeof(path), "%s%s.wav", prefixes[p], name);
        sfx = TAK_Sound_LoadWAV(path);
        if (sfx) {
            cache_insert(name, sfx);
            return sfx;
        }

        /* Try without extension (maybe the name already has .wav) */
        snprintf(path, sizeof(path), "%s%s", prefixes[p], name);
        sfx = TAK_Sound_LoadWAV(path);
        if (sfx) {
            cache_insert(name, sfx);
            return sfx;
        }
    }

    /* Cache the failure so we don't retry every frame */
    cache_insert(name, NULL);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────── */

int GameSound_Init(void) {
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_count = 0;
    s_initialized = 1;
    return 0;
}

void GameSound_Shutdown(void) {
    for (int i = 0; i < s_cache_count; i++) {
        if (s_cache[i].effect) {
            TAK_Sound_Unload(s_cache[i].effect);
            s_cache[i].effect = NULL;
        }
    }
    s_cache_count = 0;
    s_initialized = 0;
}

void GameSound_UnitAction(const char *soundclass_name, const char *action,
                           int volume, int world_x, int world_y,
                           int cam_x, int cam_y,
                           int viewport_w, int viewport_h) {
    if (!s_initialized || !soundclass_name || !action) return;

    int class_id = SoundClass_Find(soundclass_name);
    if (class_id < 0) return;

    const char *sound_name = SoundClass_SelectSound(class_id, action);
    if (!sound_name) return;

    TAK_SoundEffect *sfx = get_or_load(sound_name);
    if (!sfx) return;

    TAK_Sound_PlayPositional(sfx, volume, 3, /* priority: unit voices */
                              world_x, world_y,
                              cam_x, cam_y, viewport_w, viewport_h);
}

void GameSound_WeaponHit(const char *hitclass, const char *material,
                          int volume, int world_x, int world_y,
                          int cam_x, int cam_y,
                          int viewport_w, int viewport_h) {
    if (!s_initialized || !hitclass) return;

    const char *wav_name = SoundClass_SelectHitSound(hitclass, material);
    if (!wav_name) return;

    TAK_SoundEffect *sfx = get_or_load(wav_name);
    if (!sfx) return;

    TAK_Sound_PlayPositional(sfx, volume, 2, /* priority: lower than voices */
                              world_x, world_y,
                              cam_x, cam_y, viewport_w, viewport_h);
}

void GameSound_PlayUI(const char *wav_name) {
    if (!s_initialized || !wav_name) return;

    TAK_SoundEffect *sfx = get_or_load(wav_name);
    if (!sfx) return;

    TAK_Sound_Play2D(sfx, 100, 64);  /* 78% volume, center pan */
}

void GameSound_PlayWorldWav(const char *wav_name, int volume,
                            int world_x, int world_y,
                            int cam_x, int cam_y,
                            int viewport_w, int viewport_h) {
    if (!s_initialized || !wav_name || !wav_name[0]) return;

    TAK_SoundEffect *sfx = get_or_load(wav_name);
    if (!sfx) return;

    TAK_Sound_PlayPositional(sfx, volume, 2,
                             world_x, world_y,
                             cam_x, cam_y, viewport_w, viewport_h);
}
