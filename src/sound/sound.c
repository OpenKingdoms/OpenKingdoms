/*
 * sound.c — TAK sound effect engine (Phases 1-3)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  GAME AUDIO LESSON: How digital audio works in games
 * ═══════════════════════════════════════════════════════════════════
 *
 * When you call TAK_Sound_Init(), here's what happens under the hood:
 *
 * 1. miniaudio opens the OS audio device (Core Audio on macOS, WASAPI
 *    on Windows). This gives us a callback: the OS asks for audio
 *    samples ~100 times/sec, we fill a buffer with PCM data — raw
 *    numbers representing the speaker cone's position at each moment.
 *
 * 2. The ma_engine creates a mixer on a background thread. When you
 *    call ma_sound_start(), the sound's PCM samples get mixed into
 *    the output buffer alongside other playing sounds. Mixing is just
 *    addition — add sample values together, clamp to prevent overflow.
 *
 * 3. Each sound has volume (multiplier on samples) and pan (L/R
 *    balance). The mixer applies these before summing.
 *
 * TA:K audio specs: 8-bit mono PCM, 11025 Hz, all WAV files.
 *
 * ═══════════════════════════════════════════════════════════════════
 *  CHANNEL MANAGEMENT LESSON: Why games limit concurrent sounds
 * ═══════════════════════════════════════════════════════════════════
 *
 * In a battle with 50 units, every sword swing wants a sound. If all
 * 50 play at once: CPU spikes, audio turns to mush, and players can't
 * distinguish critical sounds. Solution: a fixed pool of channels
 * (original: 8 max). When full, evict the lowest-priority oldest
 * sound. Critical sounds (unit voices, alarms) always win over
 * expendable sounds (distant footsteps). Players never notice.
 */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "tak_sound.h"
#include "tak_memory.h"
#include "tak_hpi.h"

#include <stdio.h>
#include <string.h>

/* ── Configuration ───────────────────────────────────────────────── */

#define TAK_MAX_CHANNELS        32
#define TAK_DEFAULT_MAX_ACTIVE   8

/* ── Internal types ──────────────────────────────────────────────── */

/* A loaded sound effect — PCM decoded once at load time.
 * Multiple channels can reference the same effect simultaneously. */
struct TAK_SoundEffect {
    void      *decoded_pcm;    /* Decoded PCM samples (tak_malloc'd)   */
    ma_uint64  frame_count;    /* Number of audio frames                */
    ma_format  format;         /* Sample format after decode            */
    ma_uint32  channels;       /* Channel count (1=mono, 2=stereo)      */
    ma_uint32  sample_rate;    /* Samples per second                    */
};

/* A playing channel — owns an ma_audio_buffer (view into the effect's
 * decoded PCM) and an ma_sound (the playback instance). */
typedef struct TAK_SoundChannel {
    ma_audio_buffer  audio_buf;   /* References effect's decoded_pcm    */
    ma_sound         sound;       /* Playback instance                  */
    int              active;      /* 1 if playing                       */
    int              buf_inited;  /* 1 if audio_buf was initialized     */
    int              snd_inited;  /* 1 if sound was initialized         */
    int              priority;    /* Higher = harder to evict            */
    uint32_t         timestamp;   /* Monotonic for age-based eviction   */
} TAK_SoundChannel;

/* ── Global state ────────────────────────────────────────────────── */

static struct {
    ma_engine        engine;
    int              initialized;
    int              max_active;
    int              active_count;
    int              master_volume;     /* 0-127 */
    uint32_t         next_timestamp;
    TAK_SoundChannel channels[TAK_MAX_CHANNELS];
} g_snd;

/* ── Channel cleanup helper ──────────────────────────────────────── */

static void channel_cleanup(TAK_SoundChannel *ch) {
    if (ch->snd_inited) {
        ma_sound_stop(&ch->sound);
        ma_sound_uninit(&ch->sound);
        ch->snd_inited = 0;
    }
    if (ch->buf_inited) {
        ma_audio_buffer_uninit(&ch->audio_buf);
        ch->buf_inited = 0;
    }
    ch->active = 0;
}

/* ── Init / Shutdown ─────────────────────────────────────────────── */

int TAK_Sound_Init(void) {
    if (g_snd.initialized) return 0;
    memset(&g_snd, 0, sizeof(g_snd));

    ma_engine_config cfg = ma_engine_config_init();
    cfg.channels   = 2;   /* stereo */
    cfg.sampleRate = 0;   /* native device rate; miniaudio resamples */

    ma_result r = ma_engine_init(&cfg, &g_snd.engine);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "TAK_Sound_Init: ma_engine_init failed: %s\n",
                ma_result_description(r));
        return -1;
    }

    g_snd.initialized   = 1;
    g_snd.max_active    = TAK_DEFAULT_MAX_ACTIVE;
    g_snd.master_volume = 100;
    ma_engine_set_volume(&g_snd.engine, 100.0f / 127.0f);

    ma_device *dev = ma_engine_get_device(&g_snd.engine);
    if (dev) {
        fprintf(stderr, "Sound: initialized (%s, %u Hz, %u ch)\n",
                dev->playback.name,
                (unsigned)dev->sampleRate,
                (unsigned)dev->playback.channels);
    } else {
        fprintf(stderr, "Sound: initialized\n");
    }

    return 0;
}

void TAK_Sound_Shutdown(void) {
    if (!g_snd.initialized) return;

    for (int i = 0; i < TAK_MAX_CHANNELS; i++) {
        if (g_snd.channels[i].active) {
            channel_cleanup(&g_snd.channels[i]);
        }
    }

    ma_engine_uninit(&g_snd.engine);
    g_snd.initialized  = 0;
    g_snd.active_count = 0;
    fprintf(stderr, "Sound: shutdown complete\n");
}

int TAK_Sound_IsInitialized(void) {
    return g_snd.initialized;
}

/* Used by music.c to access the shared engine instance. */
ma_engine *TAK_Sound_GetEngine(void) {
    return g_snd.initialized ? &g_snd.engine : NULL;
}

/* ── Volume ──────────────────────────────────────────────────────── */

void TAK_Sound_SetMasterVolume(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    g_snd.master_volume = vol;
    if (g_snd.initialized)
        ma_engine_set_volume(&g_snd.engine, (float)vol / 127.0f);
}

int TAK_Sound_GetMasterVolume(void) {
    return g_snd.master_volume;
}

/* ── Channel configuration ───────────────────────────────────────── */

void TAK_Sound_SetMaxChannels(int max) {
    if (max < 1) max = 1;
    if (max > TAK_MAX_CHANNELS) max = TAK_MAX_CHANNELS;
    g_snd.max_active = max;
}

/* ── Per-frame update ────────────────────────────────────────────── */

void TAK_Sound_Update(void) {
    if (!g_snd.initialized) return;

    for (int i = 0; i < TAK_MAX_CHANNELS; i++) {
        TAK_SoundChannel *ch = &g_snd.channels[i];
        if (!ch->active) continue;

        if (ch->snd_inited && !ma_sound_is_playing(&ch->sound)) {
            channel_cleanup(ch);
            g_snd.active_count--;
        }
    }
}

/* ── Channel eviction ────────────────────────────────────────────── */

static int find_or_evict_channel(int new_priority) {
    /* Look for an empty slot within the active cap */
    if (g_snd.active_count < g_snd.max_active) {
        for (int i = 0; i < TAK_MAX_CHANNELS; i++) {
            if (!g_snd.channels[i].active) return i;
        }
    }

    /* At cap: steal a strictly lower priority channel or give up. */
    int      active[TAK_MAX_CHANNELS];
    int      prio[TAK_MAX_CHANNELS];
    uint32_t serial[TAK_MAX_CHANNELS];
    for (int i = 0; i < TAK_MAX_CHANNELS; i++) {
        active[i] = g_snd.channels[i].active;
        prio[i]   = g_snd.channels[i].priority;
        serial[i] = g_snd.channels[i].timestamp;
    }
    int victim = TAK_Sound_ChooseVictim(TAK_MAX_CHANNELS, active, prio,
                                        serial, new_priority);
    if (victim >= 0) {
        channel_cleanup(&g_snd.channels[victim]);
        g_snd.active_count--;
        return victim;
    }

    return -1;
}

int TAK_Sound_ChooseVictim(int count, const int *active,
                           const int *priority, const uint32_t *serial,
                           int new_priority) {
    int victim = -1;
    for (int i = 0; i < count; i++) {
        if (!active[i]) continue;
        if (priority[i] >= new_priority) continue;   /* legacy:308182 */
        if (victim < 0 ||
            priority[i] < priority[victim] ||
            (priority[i] == priority[victim] && serial[i] < serial[victim])) {
            victim = i;
        }
    }
    return victim;
}

int TAK_Sound_FreeChannels(void) {
    /* Without a device nothing is ever busy, so the callers that only
     * speak into a free channel keep speaking (and tests can hear). */
    if (!g_snd.initialized) return TAK_DEFAULT_MAX_ACTIVE;
    int n = g_snd.max_active - g_snd.active_count;
    return n > 0 ? n : 0;
}

/* ── Loading ─────────────────────────────────────────────────────── */

TAK_SoundEffect *TAK_Sound_LoadWAV(const char *vfs_path) {
    if (!vfs_path) return NULL;

    void    *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(vfs_path, &data, &size) != 0 || !data)
        return NULL;

    TAK_SoundEffect *sfx = TAK_Sound_LoadWAVFromMemory(data, size);
    tak_free(data);
    return sfx;
}

TAK_SoundEffect *TAK_Sound_LoadWAVFromMemory(const void *data, uint32_t size) {
    if (!data || size == 0) return NULL;

    /* Decode the WAV from memory into raw PCM samples.
     * We do this once at load time so Play() is cheap. */
    ma_decoder_config dec_cfg = ma_decoder_config_init_default();
    ma_decoder decoder;
    ma_result r = ma_decoder_init_memory(data, size, &dec_cfg, &decoder);
    if (r != MA_SUCCESS) return NULL;

    ma_uint64 frame_count = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &frame_count);
    if (frame_count == 0) {
        ma_decoder_uninit(&decoder);
        return NULL;
    }

    ma_format  fmt  = decoder.outputFormat;
    ma_uint32  ch   = decoder.outputChannels;
    ma_uint32  rate = decoder.outputSampleRate;
    ma_uint32  bpf  = ma_get_bytes_per_frame(fmt, ch);

    void *pcm = tak_malloc_named("sound.pcm", (size_t)(frame_count * bpf));
    if (!pcm) { ma_decoder_uninit(&decoder); return NULL; }

    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&decoder, pcm, frame_count, &frames_read);
    ma_decoder_uninit(&decoder);

    if (frames_read == 0) { tak_free(pcm); return NULL; }

    TAK_SoundEffect *sfx = (TAK_SoundEffect *)tak_malloc(sizeof(TAK_SoundEffect));
    if (!sfx) { tak_free(pcm); return NULL; }

    sfx->decoded_pcm = pcm;
    sfx->frame_count = frames_read;
    sfx->format      = fmt;
    sfx->channels    = ch;
    sfx->sample_rate = rate;

    return sfx;
}

void TAK_Sound_Unload(TAK_SoundEffect *sfx) {
    if (!sfx) return;
    if (sfx->decoded_pcm) tak_free(sfx->decoded_pcm);
    tak_free(sfx);
}

/* ── Playback ────────────────────────────────────────────────────── */

int TAK_Sound_Play(TAK_SoundEffect *sfx, int volume, int pan, int priority) {
    if (!g_snd.initialized || !sfx || !sfx->decoded_pcm) return 0;

    int slot = find_or_evict_channel(priority);
    if (slot < 0) return 0;

    TAK_SoundChannel *ch = &g_snd.channels[slot];

    /* Create an ma_audio_buffer that references the effect's decoded PCM.
     * This is a lightweight view — no data copy. The audio buffer must
     * stay alive while the ma_sound plays from it. */
    ma_audio_buffer_config abcfg = ma_audio_buffer_config_init(
        sfx->format, sfx->channels, sfx->frame_count,
        sfx->decoded_pcm, NULL);
    abcfg.sampleRate = sfx->sample_rate;

    ma_result r = ma_audio_buffer_init(&abcfg, &ch->audio_buf);
    if (r != MA_SUCCESS) return 0;
    ch->buf_inited = 1;

    /* Create an ma_sound from the audio buffer as a data source.
     * NO_SPATIALIZATION: we handle pan manually via L/R volume.
     * NO_PITCH: we don't need pitch shifting. */
    r = ma_sound_init_from_data_source(
        &g_snd.engine, &ch->audio_buf,
        MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH,
        NULL, &ch->sound);
    if (r != MA_SUCCESS) {
        ma_audio_buffer_uninit(&ch->audio_buf);
        ch->buf_inited = 0;
        return 0;
    }
    ch->snd_inited = 1;

    /* Volume: 0-127 → 0.0-1.0 */
    float vol_f = (float)volume / 127.0f;

    /* Pan: 0-127 → -1.0..+1.0 (0=left, 64=center, 127=right) */
    float pan_f = ((float)pan - 64.0f) / 63.0f;
    if (pan_f < -1.0f) pan_f = -1.0f;
    if (pan_f >  1.0f) pan_f =  1.0f;

    ma_sound_set_volume(&ch->sound, vol_f);
    ma_sound_set_pan(&ch->sound, pan_f);
    ma_sound_set_looping(&ch->sound, MA_FALSE);
    ma_sound_start(&ch->sound);

    ch->active    = 1;
    ch->priority  = priority;
    ch->timestamp = g_snd.next_timestamp++;
    g_snd.active_count++;

    return 1;
}

void TAK_Sound_StopAll(void) {
    if (!g_snd.initialized) return;

    for (int i = 0; i < TAK_MAX_CHANNELS; i++) {
        if (g_snd.channels[i].active) {
            channel_cleanup(&g_snd.channels[i]);
        }
    }
    g_snd.active_count = 0;
}

/* ── Positional audio ────────────────────────────────────────────── */

/* Volume is a flat two-step: 0x7f inside the viewport, 0x40 outside,
 * with no distance term (legacy:221181-221190). Pan is 64 plus 64 times
 * the offset from the viewport centre over the full viewport width,
 * clamped to 0..127 (legacy:221191-221194). */
void TAK_Sound_Spatialize(int world_x, int world_y,
                          int cam_x, int cam_y,
                          int viewport_w, int viewport_h,
                          int *out_volume, int *out_pan) {
    int inside = world_x >= cam_x && world_x <= cam_x + viewport_w &&
                 world_y >= cam_y && world_y <= cam_y + viewport_h;
    int volume = inside ? 0x7f : 0x40;
    int pan = 0x40;
    if (viewport_w > 0) {
        pan = ((world_x - viewport_w / 2 - cam_x) * 0x40) / viewport_w + 0x40;
        if (pan < 0)    pan = 0;
        if (pan > 0x7f) pan = 0x7f;
    }
    if (out_volume) *out_volume = volume;
    if (out_pan)    *out_pan = pan;
}

int TAK_Sound_PlayPositional(TAK_SoundEffect *sfx, int priority,
                              int world_x, int world_y,
                              int cam_x, int cam_y,
                              int viewport_w, int viewport_h) {
    int vol = 0, pan = 0;
    TAK_Sound_Spatialize(world_x, world_y, cam_x, cam_y,
                         viewport_w, viewport_h, &vol, &pan);
    return TAK_Sound_Play(sfx, vol, pan, priority);
}

int TAK_Sound_Play2D(TAK_SoundEffect *sfx, int volume, int pan, int priority) {
    return TAK_Sound_Play(sfx, volume, pan, priority);
}
