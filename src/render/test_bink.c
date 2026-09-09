/*
 * test_bink -- diagnostic tool for .bik files.
 *
 * Subcommands:
 *   info    <path>                  open + decode first frame (sanity check)
 *   count   <path> [<path>...]      per-file frame count, dims, fps
 *   extract <path> <out.ppm>        write frame 0 as P6 PPM
 *
 * Not a unit test — kept in the Debug tree because it's useful when
 * investigating layout/timing questions against the real game's videos.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

static int cmd_info(const char *path) {
    printf("Opening: %s\n", path);
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        printf("FAILED to open\n"); return 1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        printf("FAILED find_stream_info\n");
        avformat_close_input(&fmt);
        return 1;
    }
    printf("Streams: %u\n", fmt->nb_streams);

    int vidx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters *p = fmt->streams[i]->codecpar;
        printf("  Stream %u: codec=%d type=%d\n", i, p->codec_id, p->codec_type);
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) vidx = (int)i;
    }
    if (vidx < 0) {
        printf("No video stream\n");
        avformat_close_input(&fmt);
        return 1;
    }

    AVCodecParameters *par = fmt->streams[vidx]->codecpar;
    printf("Video: %dx%d codec_id=%d\n", par->width, par->height, par->codec_id);

    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { printf("No decoder found\n"); avformat_close_input(&fmt); return 1; }
    printf("Decoder: %s\n", codec->name);

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, par);
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        printf("FAILED to open decoder\n");
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return 1;
    }
    printf("Decoder opened\n");

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int decoded = 0;
    for (int attempt = 0; attempt < 50 && !decoded; attempt++) {
        int ret = av_read_frame(fmt, pkt);
        if (ret < 0) { printf("av_read_frame failed: %d\n", ret); break; }
        if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }

        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (avcodec_receive_frame(ctx, frame) == 0) {
            printf("DECODED: %dx%d format=%d\n", frame->width, frame->height, frame->format);
            decoded = 1;
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    printf(decoded ? "SUCCESS\n" : "FAILED to decode any frame\n");
    return decoded ? 0 : 1;
}

static int cmd_count_one(const char *path) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        printf("%-60s FAILED TO OPEN\n", path);
        return 1;
    }
    avformat_find_stream_info(fmt, NULL);

    AVCodecParameters *par = fmt->streams[0]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, par);
    avcodec_open2(ctx, codec, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int packets = 0, decoded = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == 0) {
            packets++;
            if (avcodec_send_packet(ctx, pkt) == 0) {
                while (avcodec_receive_frame(ctx, frame) == 0) {
                    decoded++;
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    AVRational fr = fmt->streams[0]->r_frame_rate;
    double fps = fr.den ? (double)fr.num / fr.den : 0.0;
    printf("%-60s %3d frames (%d pkts) %dx%d %.1f fps\n",
           path, decoded, packets, par->width, par->height, fps);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return 0;
}

static int cmd_extract(const char *src, const char *out_ppm) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, src, NULL, NULL) < 0) {
        printf("FAILED to open %s\n", src); return 1;
    }
    avformat_find_stream_info(fmt, NULL);
    AVCodecParameters *par = fmt->streams[0]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, par);
    avcodec_open2(ctx, codec, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    int got = 0;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == 0) {
            avcodec_send_packet(ctx, pkt);
            if (avcodec_receive_frame(ctx, frm) == 0) got = 1;
        }
        av_packet_unref(pkt);
    }
    int rc = 1;
    if (got) {
        int w = frm->width, h = frm->height;
        struct SwsContext *sws = sws_getContext(w, h, frm->format, w, h, AV_PIX_FMT_RGB24,
                                                SWS_BILINEAR, NULL, NULL, NULL);
        int tight = w * 3;
        int padded = (tight + 63) & ~63;  /* SIMD-safe over-alloc for sws_scale */
        uint8_t *rgb = (uint8_t *)malloc((size_t)padded * h + 64);
        uint8_t *dst[1] = { rgb };
        int dstls[1] = { padded };
        sws_scale(sws, (const uint8_t *const *)frm->data, frm->linesize, 0, h, dst, dstls);

        FILE *fp = fopen(out_ppm, "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", w, h);
            for (int y = 0; y < h; y++) fwrite(rgb + y * padded, 1, (size_t)tight, fp);
            fclose(fp);
            printf("wrote %s (%dx%d)\n", out_ppm, w, h);
            rc = 0;
        } else {
            printf("FAILED to open output %s\n", out_ppm);
        }
        free(rgb);
        sws_freeContext(sws);
    } else {
        printf("no frame decoded from %s\n", src);
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s info <path>\n"
            "       %s count <path> [<path>...]\n"
            "       %s extract <src.bik> <out.ppm>\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "info") == 0 && argc == 3) {
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "count") == 0 && argc >= 3) {
        int rc = 0;
        for (int i = 2; i < argc; i++) rc |= cmd_count_one(argv[i]);
        return rc;
    }
    if (strcmp(argv[1], "extract") == 0 && argc == 4) {
        return cmd_extract(argv[2], argv[3]);
    }
    fprintf(stderr, "unknown subcommand or bad args\n");
    return 1;
}
