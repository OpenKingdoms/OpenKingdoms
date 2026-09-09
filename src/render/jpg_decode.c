#include "tak_jpg.h"
#include "tak_memory.h"

#ifndef TAK_HAVE_FFMPEG
/* No FFmpeg (e.g. the WASM build): decode JPEGs with the vendored
 * stb_image instead. Same contract — RGBA32 pixels in a tak_malloc'd
 * buffer. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
#include <string.h>

int JPG_DecodeRGBA(const uint8_t *jpg, size_t size,
                    uint32_t **out_px, int *out_w, int *out_h) {
    if (out_px) *out_px = NULL;
    if (out_w)  *out_w  = 0;
    if (out_h)  *out_h  = 0;
    if (!jpg || size == 0 || !out_px || !out_w || !out_h) return -1;

    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(jpg, (int)size,
                                                &w, &h, &comp, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return -1;
    }
    uint32_t *px = (uint32_t *)tak_malloc((size_t)w * h * 4);
    if (!px) {
        stbi_image_free(rgba);
        return -1;
    }
    memcpy(px, rgba, (size_t)w * h * 4);
    stbi_image_free(rgba);
    *out_px = px;
    *out_w = w;
    *out_h = h;
    return 0;
}
#else
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

int JPG_DecodeRGBA(const uint8_t *jpg, size_t size,
                    uint32_t **out_px, int *out_w, int *out_h) {
    if (out_px) *out_px = NULL;
    if (out_w)  *out_w  = 0;
    if (out_h)  *out_h  = 0;

    if (!jpg || size == 0 || !out_px || !out_w || !out_h) return -1;

    AVCodecContext   *ctx   = NULL;
    AVPacket         *pkt   = NULL;
    AVFrame          *frame = NULL;
    struct SwsContext *sws  = NULL;
    uint32_t         *px    = NULL;
    int               rc    = -1;

    /* 1. Find the MJPEG codec. */
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) goto cleanup;

    /* 2. Allocate + open the codec context. */
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) goto cleanup;
    if (avcodec_open2(ctx, codec, NULL) < 0) goto cleanup;

    /* 3. Wrap the input bytes in an AVPacket. No copy — the MJPEG
     * decoder doesn't modify. Need AV_INPUT_BUFFER_PADDING_SIZE bytes
     * of padding after the data per libav docs; our JPG buffer comes
     * from VFS_ReadFile which allocates extra slack, but to be safe we
     * could copy. Skipping the copy for now — if we see decoder
     * misreads, revisit. */
    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;
    pkt->data = (uint8_t *)jpg;
    pkt->size = (int)size;

    /* 4. Allocate a frame, decode. */
    frame = av_frame_alloc();
    if (!frame) goto cleanup;
    if (avcodec_send_packet(ctx, pkt) < 0)    goto cleanup;
    if (avcodec_receive_frame(ctx, frame) < 0) goto cleanup;

    int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0) goto cleanup;

    /* 5. Convert frame->format to RGBA32 via sws. */
    sws = sws_getContext(w, h, frame->format,
                          w, h, AV_PIX_FMT_RGBA,
                          SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    px = (uint32_t *)tak_malloc((size_t)w * h * sizeof(uint32_t));
    if (!px) goto cleanup;

    uint8_t *dst_data[1]   = { (uint8_t *)px };
    int      dst_stride[1] = { w * 4 };
    sws_scale(sws, (const uint8_t * const *)frame->data, frame->linesize,
              0, h, dst_data, dst_stride);

    /* 6. Success: publish outputs, skip the px free in cleanup. */
    *out_px = px;
    *out_w  = w;
    *out_h  = h;
    px      = NULL;  /* ownership handed to caller */
    rc      = 0;

cleanup:
    if (sws)   sws_freeContext(sws);
    if (frame) av_frame_free(&frame);
    if (pkt)   av_packet_free(&pkt);
    if (ctx)   avcodec_free_context(&ctx);
    if (px)    tak_free(px);  /* only if we didn't hand ownership out */
    return rc;
}

#endif /* TAK_HAVE_FFMPEG */
