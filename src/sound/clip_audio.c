/*
 * clip_audio.c -- a reel's soundtrack on its way to the mixer.
 *
 * A lock free ring the player writes on the game thread and the mixer
 * reads on its own, behind a data source of ours so that an empty ring
 * is silence and never the end of the sound.
 */

#include "tak_clip_audio.h"
#include "tak_sound.h"

/* Types only, the implementation is in sound.c. */
#include "miniaudio.h"

#include <stdlib.h>
#include <string.h>

extern ma_engine *TAK_Sound_GetEngine(void);

struct ClipAudio {
    ma_data_source_base ds;
    ma_pcm_rb   rb;
    ma_sound    sound;
    ma_engine  *engine;
    int         channels;
    int         rate;
    int         started;
    int         rb_up, sound_up;
    ma_uint64   played;
};

static ma_result clip_read(ma_data_source *src, void *out, ma_uint64 count,
                           ma_uint64 *read) {
    ClipAudio *ca = (ClipAudio *)src;
    ma_uint64 done = 0;
    size_t frame_bytes = (size_t)ca->channels * sizeof(int16_t);
    while (done < count) {
        ma_uint32 take = (ma_uint32)(count - done);
        void *p = NULL;
        if (ma_pcm_rb_acquire_read(&ca->rb, &take, &p) != MA_SUCCESS || take == 0) break;
        memcpy((uint8_t *)out + done * frame_bytes, p, (size_t)take * frame_bytes);
        ma_pcm_rb_commit_read(&ca->rb, take);
        done += take;
    }
    /* Dry is quiet, not over. */
    if (done < count) {
        memset((uint8_t *)out + done * frame_bytes, 0, (size_t)(count - done) * frame_bytes);
    }
    ca->played += done;
    if (read) *read = count;
    return MA_SUCCESS;
}

static ma_result clip_seek(ma_data_source *src, ma_uint64 frame) {
    (void)src; (void)frame;
    return MA_NOT_IMPLEMENTED;
}

static ma_result clip_format(ma_data_source *src, ma_format *fmt, ma_uint32 *ch,
                             ma_uint32 *rate, ma_channel *map, size_t map_cap) {
    ClipAudio *ca = (ClipAudio *)src;
    if (fmt) *fmt = ma_format_s16;
    if (ch) *ch = (ma_uint32)ca->channels;
    if (rate) *rate = (ma_uint32)ca->rate;
    if (map && map_cap) ma_channel_map_init_standard(ma_standard_channel_map_default, map, map_cap, (ma_uint32)ca->channels);
    return MA_SUCCESS;
}

static ma_result clip_cursor(ma_data_source *src, ma_uint64 *cursor) {
    ClipAudio *ca = (ClipAudio *)src;
    if (cursor) *cursor = ca->played;
    return MA_SUCCESS;
}

static ma_result clip_length(ma_data_source *src, ma_uint64 *len) {
    (void)src;
    if (len) *len = 0;
    return MA_NOT_IMPLEMENTED;
}

static ma_data_source_vtable g_clip_vtable = {
    clip_read, clip_seek, clip_format, clip_cursor, clip_length, NULL, 0
};

ClipAudio *ClipAudio_Open(int channels, int sample_rate) {
    if (channels < 1 || channels > 2 || sample_rate < 8000 || sample_rate > 96000) return NULL;
    if (!TAK_Sound_IsInitialized() || !TAK_Sound_IsEnabled()) return NULL;
    ma_engine *engine = TAK_Sound_GetEngine();
    if (!engine) return NULL;
    ClipAudio *ca = (ClipAudio *)calloc(1, sizeof(ClipAudio));
    if (!ca) return NULL;
    ca->engine = engine;
    ca->channels = channels;
    ca->rate = sample_rate;

    ma_data_source_config dcfg = ma_data_source_config_init();
    dcfg.vtable = &g_clip_vtable;
    if (ma_data_source_init(&dcfg, &ca->ds) != MA_SUCCESS) { free(ca); return NULL; }
    /* Two seconds of room. */
    if (ma_pcm_rb_init(ma_format_s16, (ma_uint32)channels, (ma_uint32)sample_rate * 2u,
                       NULL, NULL, &ca->rb) != MA_SUCCESS) {
        ma_data_source_uninit(&ca->ds);
        free(ca);
        return NULL;
    }
    ca->rb_up = 1;
    if (ma_sound_init_from_data_source(engine, &ca->ds, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       NULL, &ca->sound) != MA_SUCCESS) {
        ClipAudio_Close(ca);
        return NULL;
    }
    ca->sound_up = 1;
    return ca;
}

void ClipAudio_Close(ClipAudio *ca) {
    if (!ca) return;
    if (ca->sound_up) {
        ma_sound_stop(&ca->sound);
        ma_sound_uninit(&ca->sound);
    }
    if (ca->rb_up) ma_pcm_rb_uninit(&ca->rb);
    ma_data_source_uninit(&ca->ds);
    free(ca);
}

int ClipAudio_Push(ClipAudio *ca, const int16_t *pcm, int frames) {
    if (!ca || !pcm || frames <= 0) return 0;
    size_t frame_bytes = (size_t)ca->channels * sizeof(int16_t);
    int done = 0;
    while (done < frames) {
        ma_uint32 put = (ma_uint32)(frames - done);
        void *p = NULL;
        if (ma_pcm_rb_acquire_write(&ca->rb, &put, &p) != MA_SUCCESS || put == 0) break;
        memcpy(p, (const uint8_t *)pcm + (size_t)done * frame_bytes, (size_t)put * frame_bytes);
        ma_pcm_rb_commit_write(&ca->rb, put);
        done += (int)put;
    }
    return done;
}

int ClipAudio_Queued(const ClipAudio *ca) {
    return ca ? (int)ma_pcm_rb_available_read((ma_pcm_rb *)&ca->rb) : 0;
}

int ClipAudio_Capacity(const ClipAudio *ca) {
    return ca ? (int)ma_pcm_rb_get_subbuffer_size((ma_pcm_rb *)&ca->rb) : 0;
}

int ClipAudio_Start(ClipAudio *ca) {
    if (!ca || !ca->sound_up) return 0;
    if (ca->started) return 1;
    ma_sound_set_volume(&ca->sound, (float)TAK_Sound_GetMasterVolume() / 127.0f);
    if (ma_sound_start(&ca->sound) != MA_SUCCESS) return 0;
    ca->started = 1;
    return 1;
}

void ClipAudio_Flush(ClipAudio *ca) {
    if (!ca) return;
    ma_pcm_rb_reset(&ca->rb);
}

int64_t ClipAudio_Played(const ClipAudio *ca) {
    return ca ? (int64_t)ca->played : 0;
}
