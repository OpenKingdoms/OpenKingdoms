/*
 * bink_player.c -- Bink video decoder using FFmpeg
 *
 * The container stays open and each frame is decoded as it comes due,
 * into one RGBA buffer. Advance steps one frame at most per call, the
 * way the original waits on the decoder's clock (legacy:35289).
 */

#include "tak_bink.h"
#include "tak_paths.h"
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* The install spells its file names in a case the original's strings
 * do not, and not every file system forgives that: try the name as
 * given, then in capitals, then in lower case. */
static int find_clip(const char *rel_path, char *path, size_t cap) {
    if (!rel_path || !rel_path[0]) return 0;
    snprintf(path, cap, "%s/%s", Paths_GameDir(), rel_path);
    char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt == 1)
            for (char *p = name; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (attempt == 2)
            for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

#ifndef TAK_HAVE_FFMPEG
/* No FFmpeg on this platform/toolchain (e.g. the WASM build): every
 * BinkPlayer_Open fails and callers fall back to static sprites, the
 * same path taken when a .bik file is missing. */
BinkPlayer *BinkPlayer_Open(const char *path) { (void)path; return NULL; }
BinkPlayer *BinkPlayer_OpenClip(const char *rel_path) { (void)rel_path; return NULL; }
int BinkPlayer_ClipExists(const char *rel_path) { (void)rel_path; (void)find_clip; return 0; }
void BinkPlayer_Close(BinkPlayer *bp) { (void)bp; }
int BinkPlayer_NextFrame(BinkPlayer *bp) { (void)bp; return 0; }
const uint32_t *BinkPlayer_GetPixels(BinkPlayer *bp) { (void)bp; return NULL; }
int BinkPlayer_GetWidth(BinkPlayer *bp) { (void)bp; return 0; }
int BinkPlayer_GetHeight(BinkPlayer *bp) { (void)bp; return 0; }
double BinkPlayer_GetFrameDuration(BinkPlayer *bp) { (void)bp; return 0.0; }
void BinkPlayer_Rewind(BinkPlayer *bp) { (void)bp; }
int BinkPlayer_IsFinished(BinkPlayer *bp) { (void)bp; return 1; }
int BinkPlayer_GetFrameCount(BinkPlayer *bp) { (void)bp; return 0; }
void BinkPlayer_SeekTo(BinkPlayer *bp, int frame) { (void)bp; (void)frame; }
int BinkPlayer_Advance(BinkPlayer *bp, double dt) { (void)bp; (void)dt; return 0; }
int BinkPlayer_CurrentFrame(BinkPlayer *bp) { (void)bp; return -1; }
int BinkPlayer_OpenCount(void) { return 0; }
#else

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

struct BinkPlayer {
    AVFormatContext *fmt;
    AVCodecContext *codec;
    struct SwsContext *sws;
    AVFrame *frame;
    AVPacket *pkt;
    int video_idx;
    int drained;            /* the decoder has been told the file ended */

    uint8_t *scaled;        /* sws output, rows padded for its SIMD */
    int scaled_stride;
    uint32_t *rgba;         /* the frame on show, tight rows */
    int width, height;
    int num_frames;
    int current_frame;
    double frame_duration;
    double timer;           /* wall time owed to the next frame */
    int finished;
};

static int s_open_count;

BinkPlayer *BinkPlayer_OpenClip(const char *rel_path) {
    char path[1024];
    if (!find_clip(rel_path, path, sizeof(path))) return NULL;
    return BinkPlayer_Open(path);
}

int BinkPlayer_ClipExists(const char *rel_path) {
    char path[1024];
    return find_clip(rel_path, path, sizeof(path));
}

/* Decode the next video frame into bp->rgba. 0 at the end of the file. */
static int decode_next(BinkPlayer *bp) {
    for (;;) {
        int ret = avcodec_receive_frame(bp->codec, bp->frame);
        if (ret == 0) {
            AVFrame *f = bp->frame;
            if (f->width <= 0 || f->height <= 0 || !f->data[0]) {
                av_frame_unref(f);
                continue;
            }
            if (!bp->sws) {
                bp->sws = sws_getContext(f->width, f->height, f->format,
                                         f->width, f->height, AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR, NULL, NULL, NULL);
                if (!bp->sws) { av_frame_unref(f); return 0; }
                bp->width = f->width;
                bp->height = f->height;
                bp->scaled_stride = (f->width * 4 + 63) & ~63;
                bp->scaled = (uint8_t *)malloc((size_t)bp->scaled_stride * f->height + 64);
                bp->rgba = (uint32_t *)malloc((size_t)f->width * f->height * 4);
                if (!bp->scaled || !bp->rgba) { av_frame_unref(f); return 0; }
            }
            if (f->width != bp->width || f->height != bp->height) {
                av_frame_unref(f);
                continue;
            }
            uint8_t *dst[1] = { bp->scaled };
            int dst_stride[1] = { bp->scaled_stride };
            sws_scale(bp->sws, (const uint8_t *const *)f->data, f->linesize,
                      0, bp->height, dst, dst_stride);
            for (int row = 0; row < bp->height; row++) {
                memcpy((uint8_t *)bp->rgba + (size_t)row * bp->width * 4,
                       bp->scaled + (size_t)row * bp->scaled_stride,
                       (size_t)bp->width * 4);
            }
            av_frame_unref(f);
            return 1;
        }
        if (ret == AVERROR_EOF) return 0;
        if (ret != AVERROR(EAGAIN)) return 0;
        if (bp->drained) return 0;
        ret = av_read_frame(bp->fmt, bp->pkt);
        if (ret < 0) {
            avcodec_send_packet(bp->codec, NULL);
            bp->drained = 1;
            continue;
        }
        if (bp->pkt->stream_index != bp->video_idx) {
            av_packet_unref(bp->pkt);
            continue;
        }
        avcodec_send_packet(bp->codec, bp->pkt);
        av_packet_unref(bp->pkt);
    }
}

BinkPlayer *BinkPlayer_Open(const char *path) {
    Uint64 t0 = SDL_GetPerformanceCounter();
    BinkPlayer *bp = (BinkPlayer *)calloc(1, sizeof(BinkPlayer));
    if (!bp) return NULL;
    bp->video_idx = -1;

    if (avformat_open_input(&bp->fmt, path, NULL, NULL) < 0) {
        free(bp);
        return NULL;
    }
    if (avformat_find_stream_info(bp->fmt, NULL) < 0) goto fail;

    for (unsigned i = 0; i < bp->fmt->nb_streams; i++) {
        if (bp->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            bp->video_idx = (int)i;
            break;
        }
    }
    if (bp->video_idx < 0) goto fail;

    AVStream *st = bp->fmt->streams[bp->video_idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) goto fail;
    bp->codec = avcodec_alloc_context3(codec);
    if (!bp->codec) goto fail;
    avcodec_parameters_to_context(bp->codec, st->codecpar);
    if (avcodec_open2(bp->codec, codec, NULL) < 0) goto fail;
    bp->frame = av_frame_alloc();
    bp->pkt = av_packet_alloc();
    if (!bp->frame || !bp->pkt) goto fail;

    /* The header's rate. The demuxer writes it to avg_frame_rate and
     * the time base; r_frame_rate is a guess from timestamps that is
     * wrong on a one-frame clip. */
    AVRational fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = av_inv_q(st->time_base);
    if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
    bp->frame_duration = (fr.num > 0 && fr.den > 0)
                         ? (double)fr.den / (double)fr.num : 1.0 / 15.0;

    /* The header's frame count, by whichever field the demuxer filled.
     * The time base is one frame, so the duration counts frames too. */
    int64_t n = st->nb_frames;
    if (n <= 0) n = st->duration;
    if (n <= 0) n = avformat_index_get_entries_count(st);
    bp->num_frames = (n > 0 && n < 1000000) ? (int)n : 0;

    if (!decode_next(bp)) goto fail;
    bp->current_frame = 0;
    if (bp->num_frames <= 0) bp->num_frames = 1;

    s_open_count++;
    double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                (double)SDL_GetPerformanceFrequency();
    fprintf(stderr, "BinkPlayer: opened %s (%dx%d, %d frames, %.4g fps) in %.1f ms\n",
            path, bp->width, bp->height, bp->num_frames, 1.0 / bp->frame_duration, ms);
    return bp;

fail:
    BinkPlayer_Close(bp);
    return NULL;
}

void BinkPlayer_Close(BinkPlayer *bp) {
    if (!bp) return;
    if (bp->sws) sws_freeContext(bp->sws);
    if (bp->frame) av_frame_free(&bp->frame);
    if (bp->pkt) av_packet_free(&bp->pkt);
    if (bp->codec) avcodec_free_context(&bp->codec);
    if (bp->fmt) avformat_close_input(&bp->fmt);
    free(bp->scaled);
    free(bp->rgba);
    free(bp);
}

int BinkPlayer_NextFrame(BinkPlayer *bp) {
    if (!bp || bp->finished) return 0;
    if (bp->current_frame + 1 >= bp->num_frames || !decode_next(bp)) {
        /* The last frame stays on show. */
        bp->num_frames = bp->current_frame + 1;
        bp->finished = 1;
        return 0;
    }
    bp->current_frame++;
    return 1;
}

const uint32_t *BinkPlayer_GetPixels(BinkPlayer *bp) {
    return bp ? bp->rgba : NULL;
}

int BinkPlayer_GetWidth(BinkPlayer *bp) { return bp ? bp->width : 0; }
int BinkPlayer_GetHeight(BinkPlayer *bp) { return bp ? bp->height : 0; }
double BinkPlayer_GetFrameDuration(BinkPlayer *bp) { return bp ? bp->frame_duration : 0; }

void BinkPlayer_Rewind(BinkPlayer *bp) {
    if (!bp) return;
    bp->timer = 0;
    bp->finished = 0;
    if (bp->current_frame == 0) return;
    /* The demuxer only seeks to the first frame, which is all a
     * rewind wants. */
    if (av_seek_frame(bp->fmt, bp->video_idx, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        bp->finished = 1;
        return;
    }
    avcodec_flush_buffers(bp->codec);
    bp->drained = 0;
    bp->current_frame = 0;
    if (!decode_next(bp)) bp->finished = 1;
}

int BinkPlayer_IsFinished(BinkPlayer *bp) { return bp ? bp->finished : 1; }

int BinkPlayer_GetFrameCount(BinkPlayer *bp) { return bp ? bp->num_frames : 0; }

void BinkPlayer_SeekTo(BinkPlayer *bp, int frame) {
    if (!bp) return;
    if (frame < 0) frame = 0;
    if (frame >= bp->num_frames) frame = bp->num_frames - 1;
    if (frame < bp->current_frame) BinkPlayer_Rewind(bp);
    bp->finished = 0;
    while (bp->current_frame < frame && BinkPlayer_NextFrame(bp)) {}
    bp->timer = 0;
    bp->finished = 0;
}

int BinkPlayer_Advance(BinkPlayer *bp, double dt) {
    if (!bp || bp->finished) return 0;
    if (dt > 0) bp->timer += dt;
    if (bp->timer < bp->frame_duration) return 0;
    bp->timer -= bp->frame_duration;
    /* At most one frame of debt is carried into the next call. */
    if (bp->timer > bp->frame_duration) bp->timer = bp->frame_duration;
    return BinkPlayer_NextFrame(bp);
}

int BinkPlayer_CurrentFrame(BinkPlayer *bp) { return bp ? bp->current_frame : -1; }

int BinkPlayer_OpenCount(void) { return s_open_count; }

#endif /* TAK_HAVE_FFMPEG */
