/*
 * game_sound.c: game-level sound dispatcher.
 *
 * Wavs are loaded on first use and cached by name, the way the
 * original resolved names through its sound table (legacy:221090).
 * Unit voices come from the sound class tables (weighted pick),
 * impacts from the hit class table keyed by material.
 */

#include "tak_game_sound.h"
#include "tak_sound.h"
#include "tak_soundclass.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char             name[64];
    TAK_SoundEffect *effect;
} CacheEntry;

/* Every name ever asked for stays, found or not, so the cache owns
 * each effect it hands out and frees it at shutdown. */
static CacheEntry *s_cache = NULL;
static int         s_cache_count = 0;
static int         s_cache_cap = 0;

/* Debug recorder ring. */
#define DEBUG_EVENTS_MAX 256
static GameSoundEvent s_events[DEBUG_EVENTS_MAX];
static int            s_event_count = 0;
static int            s_event_head = 0;   /* next write slot */
static int            s_record = 0;

static int cache_lookup(const char *name, TAK_SoundEffect **out) {
    for (int i = 0; i < s_cache_count; i++) {
        if (tak_stricmp(s_cache[i].name, name) == 0) {
            *out = s_cache[i].effect;
            return 1;
        }
    }
    return 0;
}

static int cache_insert(const char *name, TAK_SoundEffect *sfx) {
    if (s_cache_count >= s_cache_cap) {
        int cap = s_cache_cap ? s_cache_cap * 2 : 256;
        CacheEntry *grown = (CacheEntry *)tak_realloc(
            s_cache, (size_t)cap * sizeof(CacheEntry));
        if (!grown) return 0;
        s_cache = grown;
        s_cache_cap = cap;
    }
    CacheEntry *ce = &s_cache[s_cache_count++];
    strncpy(ce->name, name, sizeof(ce->name) - 1);
    ce->name[sizeof(ce->name) - 1] = '\0';
    ce->effect = sfx;
    return 1;
}

/* Sound names arrive bare ("TONEARA") or with the extension
 * ("SWRDFL01.wav"). The files sit under sounds/ inside the language
 * archive, english/Sounds/ in a loose tree. A failed load is cached
 * as NULL so a missing wav costs one lookup. */
static TAK_SoundEffect *get_or_load(const char *name) {
    if (!name || name[0] == '\0') return NULL;

    TAK_SoundEffect *sfx = NULL;
    if (cache_lookup(name, &sfx)) return sfx;

    char stem[64];
    strncpy(stem, name, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    size_t len = strlen(stem);
    if (len > 4 && tak_stricmp(stem + len - 4, ".wav") == 0) stem[len - 4] = '\0';

    static const char *const prefixes[] = { "sounds/", "english/sounds/", "" };
    char path[256];
    for (int p = 0; p < 3; p++) {
        snprintf(path, sizeof(path), "%s%s.wav", prefixes[p], stem);
        sfx = TAK_Sound_LoadWAV(path);
        if (sfx) break;
    }
    if (!cache_insert(name, sfx)) {
        TAK_Sound_Unload(sfx);   /* never hand out what nothing will free */
        return NULL;
    }
    return sfx;
}

static void record_event(const char *name, int volume, int pan, int priority,
                         int positional, int world_x, int world_y, int loaded) {
    if (!s_record) return;
    GameSoundEvent *ev = &s_events[s_event_head];
    memset(ev, 0, sizeof(*ev));
    strncpy(ev->name, name, sizeof(ev->name) - 1);
    ev->volume = volume;
    ev->pan = pan;
    ev->priority = priority;
    ev->positional = positional;
    ev->world_x = world_x;
    ev->world_y = world_y;
    ev->loaded = loaded;
    s_event_head = (s_event_head + 1) % DEBUG_EVENTS_MAX;
    if (s_event_count < DEBUG_EVENTS_MAX) s_event_count++;
}

static void play_flat(const char *name, int volume, int priority) {
    if (!name || !name[0]) return;
    TAK_SoundEffect *sfx = get_or_load(name);
    record_event(name, volume, 0x40, priority, 0, 0, 0, sfx != NULL);
    if (sfx) TAK_Sound_Play(sfx, volume, 0x40, priority);
}

static void play_at(const char *name, int priority,
                    int world_x, int world_y,
                    int cam_x, int cam_y, int viewport_w, int viewport_h) {
    if (!name || !name[0]) return;
    int volume = 0, pan = 0;
    TAK_Sound_Spatialize(world_x, world_y, cam_x, cam_y,
                         viewport_w, viewport_h, &volume, &pan);
    TAK_SoundEffect *sfx = get_or_load(name);
    record_event(name, volume, pan, priority, 1, world_x, world_y, sfx != NULL);
    if (sfx) TAK_Sound_Play(sfx, volume, pan, priority);
}

int GameSound_Init(void) {
    s_cache_count = 0;
    return 0;
}

void GameSound_Shutdown(void) {
    for (int i = 0; i < s_cache_count; i++) {
        if (s_cache[i].effect) {
            TAK_Sound_Unload(s_cache[i].effect);
            s_cache[i].effect = NULL;
        }
    }
    tak_free(s_cache);
    s_cache = NULL;
    s_cache_cap = 0;
    s_cache_count = 0;
}

void GameSound_UnitVoice(const char *soundclass_name, const char *action) {
    if (!soundclass_name || !action) return;
    int class_id = SoundClass_Find(soundclass_name);
    if (class_id < 0) return;
    const char *sound_name = SoundClass_SelectSound(class_id, action);
    if (!sound_name) return;
    /* legacy:221274: priority 7, volume 0x7f, centre pan */
    play_flat(sound_name, 0x7f, 7);
}

void GameSound_PlayClass2D(const char *soundclass_name, const char *action,
                           int volume, int priority) {
    if (!soundclass_name) return;
    int class_id = SoundClass_Find(soundclass_name);
    if (class_id < 0) return;
    const char *sound_name = SoundClass_SelectSound(class_id, action);
    if (!sound_name) return;
    play_flat(sound_name, volume, priority);
}

void GameSound_WeaponHit(const char *hitclass, const char *material,
                          int world_x, int world_y,
                          int cam_x, int cam_y,
                          int viewport_w, int viewport_h) {
    if (!hitclass) return;
    const char *wav_name = SoundClass_SelectHitSound(hitclass, material);
    if (!wav_name) return;
    play_at(wav_name, 4, world_x, world_y, cam_x, cam_y, viewport_w, viewport_h);
}

void GameSound_PlayUI(const char *wav_name) {
    play_flat(wav_name, 0x7f, 7);
}

void GameSound_Play2D(const char *wav_name, int volume, int priority) {
    play_flat(wav_name, volume, priority);
}

void GameSound_PlayWorldWav(const char *wav_name, int priority,
                            int world_x, int world_y,
                            int cam_x, int cam_y,
                            int viewport_w, int viewport_h) {
    play_at(wav_name, priority, world_x, world_y,
            cam_x, cam_y, viewport_w, viewport_h);
}

/* Debug recorder */

void GameSound_DebugRecord(int enable) {
    s_record = enable ? 1 : 0;
}

void GameSound_DebugClear(void) {
    s_event_count = 0;
    s_event_head = 0;
}

int GameSound_DebugCount(void) {
    return s_event_count;
}

/* Index 0 is the oldest retained event. */
const GameSoundEvent *GameSound_DebugEvent(int index) {
    if (index < 0 || index >= s_event_count) return NULL;
    int oldest = (s_event_head - s_event_count + DEBUG_EVENTS_MAX) % DEBUG_EVENTS_MAX;
    return &s_events[(oldest + index) % DEBUG_EVENTS_MAX];
}

static int name_has_prefix(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++) {
        char a = name[i], b = prefix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
        if (a == '\0') return 0;
    }
    return 1;
}

int GameSound_DebugFindPrefix(const char *prefix) {
    if (!prefix) return -1;
    for (int i = s_event_count - 1; i >= 0; i--) {
        const GameSoundEvent *ev = GameSound_DebugEvent(i);
        if (ev && name_has_prefix(ev->name, prefix)) return i;
    }
    return -1;
}

int GameSound_DebugCountPrefix(const char *prefix) {
    if (!prefix) return 0;
    int n = 0;
    for (int i = 0; i < s_event_count; i++) {
        const GameSoundEvent *ev = GameSound_DebugEvent(i);
        if (ev && name_has_prefix(ev->name, prefix)) n++;
    }
    return n;
}
