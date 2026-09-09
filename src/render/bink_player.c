/*
 * bink_player.c -- Bink video decoder using FFmpeg
 *
 * Pre-decodes all video frames at open time into RGBA buffers.
 * Playback just indexes into the pre-decoded array — no FFmpeg
 * calls during the game loop, avoiding heap interference.
 */

#include "tak_bink.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef TAK_HAVE_FFMPEG
/* No FFmpeg on this platform/toolchain (e.g. the WASM build) — every
 * BinkPlayer_Open fails and callers fall back to static sprites, the
 * same path taken when a .bik file is missing. */
BinkPlayer *BinkPlayer_Open(const char *path) { (void)path; return NULL; }
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
#else

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

struct BinkPlayer {
    uint32_t **frames;      /* array of pre-decoded RGBA buffers */
    int num_frames;
    int current_frame;
    int width, height;
    double frame_duration;
    int finished;
};

BinkPlayer *BinkPlayer_Open(const char *path) {
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int video_idx = -1;

    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) return NULL;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = i;
            break;
        }
    }
    if (video_idx < 0) { avformat_close_input(&fmt_ctx); return NULL; }

    AVCodecParameters *par = fmt_ctx->streams[video_idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { avformat_close_input(&fmt_ctx); return NULL; }

    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, par);
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();

    int width = par->width;
    int height = par->height;

    /* Compute frame duration */
    AVRational fr = fmt_ctx->streams[video_idx]->r_frame_rate;
    double frame_dur = (fr.num > 0 && fr.den > 0) ? (double)fr.den / (double)fr.num : 1.0 / 15.0;

    /* Pre-decode all frames */
    int capacity = 64;
    int count = 0;
    uint32_t **decoded = (uint32_t **)malloc(capacity * sizeof(uint32_t *));
    if (!decoded) goto cleanup;

    int pkt_count = 0, recv_eagain = 0, recv_eof = 0, recv_err = 0, recv_ok = 0;
    int eof_sent = 0;
    while (1) {
        int ret = 0;
        if (!eof_sent) {
            ret = av_read_frame(fmt_ctx, pkt);
            if (ret < 0) {
                /* EOF on input — flush the decoder by sending a NULL packet,
                 * then keep draining buffered frames. */
                avcodec_send_packet(codec_ctx, NULL);
                eof_sent = 1;
            } else if (pkt->stream_index != video_idx) {
                av_packet_unref(pkt);
                continue;
            } else {
                pkt_count++;
                ret = avcodec_send_packet(codec_ctx, pkt);
                av_packet_unref(pkt);
                if (ret < 0) continue;
            }
        }

        /* Drain all frames the decoder has ready from this packet. */
        while (1) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN)) { recv_eagain++; break; }
            if (ret == AVERROR_EOF)     { recv_eof++; goto done_decode; }
            if (ret < 0)                { recv_err++; break; }
            recv_ok++;

            if (frame->width <= 0 || frame->height <= 0 || !frame->data[0]) {
                av_frame_unref(frame);
                continue;
            }

            /* Create scaler on first valid frame */
            if (!sws_ctx) {
                sws_ctx = sws_getContext(
                    frame->width, frame->height, frame->format,
                    frame->width, frame->height, AV_PIX_FMT_RGBA,
                    SWS_BILINEAR, NULL, NULL, NULL);
                if (!sws_ctx) { av_frame_unref(frame); goto done_decode; }
                width = frame->width;
                height = frame->height;
            }

            /* Grow array if needed */
            if (count >= capacity) {
                capacity *= 2;
                uint32_t **tmp = (uint32_t **)realloc(decoded, capacity * sizeof(uint32_t *));
                if (!tmp) { av_frame_unref(frame); goto done_decode; }
                decoded = tmp;
            }

            /* Allocate padded temp buffer for sws_scale (SIMD alignment safety),
             * then copy to a tight RGBA buffer for our use. */
            int tight_stride = width * 4;
            int padded_stride = ((tight_stride) + 63) & ~63; /* 64-byte align */
            uint8_t *temp = (uint8_t *)malloc((size_t)padded_stride * height + 64);
            if (!temp) { av_frame_unref(frame); goto done_decode; }

            uint8_t *dst_data[1] = { temp };
            int dst_linesize[1] = { padded_stride };

            sws_scale(sws_ctx,
                      (const uint8_t *const *)frame->data,
                      frame->linesize,
                      0, height,
                      dst_data, dst_linesize);

            /* Copy to tight-packed RGBA buffer */
            uint32_t *rgba = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
            if (!rgba) { free(temp); av_frame_unref(frame); goto done_decode; }
            for (int row = 0; row < height; row++) {
                memcpy((uint8_t *)rgba + row * tight_stride,
                       temp + row * padded_stride,
                       tight_stride);
            }
            free(temp);

            decoded[count++] = rgba;
            av_frame_unref(frame);
        }
    }

done_decode:
    (void)pkt_count; (void)recv_eagain; (void)recv_eof; (void)recv_err; (void)recv_ok;

cleanup:
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);

    if (count == 0) {
        if (decoded) free(decoded);
        return NULL;
    }

    BinkPlayer *bp = (BinkPlayer *)calloc(1, sizeof(BinkPlayer));
    if (!bp) {
        for (int i = 0; i < count; i++) free(decoded[i]);
        free(decoded);
        return NULL;
    }

    bp->frames = decoded;
    bp->num_frames = count;
    bp->current_frame = 0;
    bp->width = width;
    bp->height = height;
    bp->frame_duration = frame_dur;
    bp->finished = 0;

    fprintf(stderr, "BinkPlayer: decoded %d frames from %s (%dx%d)\n",
            count, path, width, height);

    return bp;
}

void BinkPlayer_Close(BinkPlayer *bp) {
    if (!bp) return;
    for (int i = 0; i < bp->num_frames; i++) free(bp->frames[i]);
    free(bp->frames);
    free(bp);
}

int BinkPlayer_NextFrame(BinkPlayer *bp) {
    if (!bp || bp->finished) return 0;
    bp->current_frame++;
    if (bp->current_frame >= bp->num_frames) {
        bp->finished = 1;
        return 0;
    }
    return 1;
}

const uint32_t *BinkPlayer_GetPixels(BinkPlayer *bp) {
    if (!bp || bp->current_frame < 0 || bp->current_frame >= bp->num_frames)
        return NULL;
    return bp->frames[bp->current_frame];
}

int BinkPlayer_GetWidth(BinkPlayer *bp) { return bp ? bp->width : 0; }
int BinkPlayer_GetHeight(BinkPlayer *bp) { return bp ? bp->height : 0; }
double BinkPlayer_GetFrameDuration(BinkPlayer *bp) { return bp ? bp->frame_duration : 0; }

void BinkPlayer_Rewind(BinkPlayer *bp) {
    if (!bp) return;
    bp->current_frame = 0;
    bp->finished = 0;
}

int BinkPlayer_IsFinished(BinkPlayer *bp) { return bp ? bp->finished : 1; }

int BinkPlayer_GetFrameCount(BinkPlayer *bp) { return bp ? bp->num_frames : 0; }

void BinkPlayer_SeekTo(BinkPlayer *bp, int frame) {
    if (!bp) return;
    if (frame < 0) frame = 0;
    if (frame >= bp->num_frames) frame = bp->num_frames - 1;
    bp->current_frame = frame;
    bp->finished = 0;
}

#endif /* TAK_HAVE_FFMPEG */
