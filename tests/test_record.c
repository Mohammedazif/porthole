/* Annex-B H.264 access units mux to a readable MP4 without re-encoding. */
#ifdef main
#undef main
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

#include "media/record.h"
#include "temp_path.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static const AVCodec *encoder_candidates[3];
static int encoder_count;

static void collect_encoders(void)
{
    const AVCodec *found = avcodec_find_encoder(AV_CODEC_ID_H264);
    const AVCodec *x264 = avcodec_find_encoder_by_name("libx264");
    const AVCodec *openh264 = avcodec_find_encoder_by_name("libopenh264");

    encoder_count = 0;
    if (found)
        encoder_candidates[encoder_count++] = found;
    if (x264 && x264 != found)
        encoder_candidates[encoder_count++] = x264;
    if (openh264 && openh264 != found && openh264 != x264)
        encoder_candidates[encoder_count++] = openh264;
}

static AVCodecContext *open_encoder(void)
{
    int i;

    for (i = 0; i < encoder_count; i++) {
        const AVCodec *enc = encoder_candidates[i];
        AVCodecContext *ctx = avcodec_alloc_context3(enc);
        if (!ctx)
            continue;
        ctx->width = 16;
        ctx->height = 16;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->time_base = (AVRational){1, 60};
        ctx->framerate = (AVRational){60, 1};
        ctx->gop_size = 12;
        ctx->max_b_frames = 0;
        ctx->bit_rate = 200000;
        ctx->thread_count = 1;
        ctx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;
        if (ctx->priv_data && enc->name && strcmp(enc->name, "libx264") == 0) {
            (void)av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
            (void)av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        }
        if (avcodec_open2(ctx, enc, NULL) == 0) {
            fprintf(stderr, "using H.264 encoder %s\n", enc->name);
            return ctx;
        }
        avcodec_free_context(&ctx);
    }
    return NULL;
}

static int emit_packets(AVCodecContext *ctx, AVFrame *frame, ph_record *rec, int stepped,
                        int *packets, int *keys)
{
    AVPacket *pkt = av_packet_alloc();
    int err;

    if (!pkt)
        return -1;
    err = avcodec_send_frame(ctx, frame);
    if (err < 0) {
        av_packet_free(&pkt);
        return -1;
    }
    while ((err = avcodec_receive_packet(ctx, pkt)) == 0) {
        uint64_t pts = 0;
        bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (stepped)
            pts = (uint64_t)(*packets + 1) * 20ULL * 1000000ULL;
        if (ph_record_video(rec, pkt->data, (size_t)pkt->size, pts, key) != 0) {
            fprintf(stderr, "mux failed for %d bytes", pkt->size);
            if (pkt->size >= 4)
                fprintf(stderr, " starting %02x %02x %02x %02x", pkt->data[0], pkt->data[1],
                        pkt->data[2], pkt->data[3]);
            fputc('\n', stderr);
            av_packet_free(&pkt);
            return -1;
        }
        (*packets)++;
        if (key)
            (*keys)++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (err != AVERROR(EAGAIN) && err != AVERROR_EOF)
        return -1;
    return 0;
}

static int record_frames(AVCodecContext *ctx, const char *path, int stepped)
{
    /* 32×32 baseline SPS. The generated stream above is 16×16. */
    static const uint8_t resized_sps[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdc, 0x96, 0x84, 0x00,
        0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xf2, 0x3c, 0x48, 0x9e,
    };
    AVFrame *frame = av_frame_alloc();
    ph_record *rec = NULL;
    int packets = 0, keys = 0, n, y;
    int rc = -1;

    if (!frame)
        return -1;
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    if (av_frame_get_buffer(frame, 16) < 0)
        goto done;
    rec = ph_record_open(path);
    if (!rec)
        goto done;
    for (n = 0; n < 3; n++) {
        if (av_frame_make_writable(frame) < 0)
            goto done;
        for (y = 0; y < frame->height; y++)
            memset(frame->data[0] + y * frame->linesize[0], 32 + n * 20, (size_t)frame->width);
        for (y = 0; y < frame->height / 2; y++) {
            memset(frame->data[1] + y * frame->linesize[1], 128, (size_t)frame->width / 2);
            memset(frame->data[2] + y * frame->linesize[2], 128, (size_t)frame->width / 2);
        }
        frame->pts = n;
        if (emit_packets(ctx, frame, rec, stepped, &packets, &keys))
            goto done;
    }
    if (emit_packets(ctx, NULL, rec, stepped, &packets, &keys))
        goto done;
    if (packets < 1 || keys < 1)
        goto done;
    CHECK(ph_record_video(rec, resized_sps, sizeof resized_sps, 0, false) ==
          PH_RECORD_FORMAT_CHANGED);
    rc = ph_record_close(rec);
    rec = NULL;
    if (rc != 0)
        rc = -1;
done:
    if (rec)
        ph_record_close(rec);
    av_frame_free(&frame);
    return rc;
}

static int container_ok(const char *path)
{
    AVFormatContext *in = NULL;
    AVCodecContext *ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    const AVCodec *decoder;
    int video = -1;
    unsigned i;
    int decoded = 0;
    int64_t duration = 0;
    int rc = -1;

    if (avformat_open_input(&in, path, NULL, NULL) < 0) {
        fprintf(stderr, "couldn't open %s\n", path);
        return -1;
    }
    if (avformat_find_stream_info(in, NULL) < 0) {
        fprintf(stderr, "couldn't read stream info from %s\n", path);
        goto done;
    }
    for (i = 0; i < in->nb_streams; i++) {
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            video = (int)i;
    }
    if (video < 0)
        goto done;
    duration = in->duration;
    if (duration <= 0 && in->streams[video]->duration > 0)
        duration = av_rescale_q(in->streams[video]->duration, in->streams[video]->time_base,
                                AV_TIME_BASE_Q);
    if (duration <= 0) {
        fprintf(stderr, "mp4 duration is zero (fmt=%" PRId64 " stream=%" PRId64 ")\n", in->duration,
                in->streams[video]->duration);
        goto done;
    }
    decoder = avcodec_find_decoder(in->streams[video]->codecpar->codec_id);
    ctx = avcodec_alloc_context3(decoder);
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!decoder || !ctx || !pkt || !frame || avcodec_parameters_to_context(ctx, in->streams[video]->codecpar) < 0
        || avcodec_open2(ctx, decoder, NULL) < 0)
        goto done;
    while (av_read_frame(in, pkt) == 0) {
        if (pkt->stream_index == video && avcodec_send_packet(ctx, pkt) == 0) {
            while (avcodec_receive_frame(ctx, frame) == 0)
                decoded++;
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) == 0)
        decoded++;
    if (decoded < 1) {
        fprintf(stderr, "mp4 video stream did not decode\n");
        goto done;
    }
    rc = 0;
done:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&in);
    return rc;
}

static int record_and_check(int stepped)
{
    char path[1024];
    AVCodecContext *ctx = open_encoder();
    int fd = ph_test_temp_file(path, sizeof path);
    int rc = -1;

    if (!ctx || fd < 0) {
        if (fd >= 0)
            close(fd);
        avcodec_free_context(&ctx);
        return -1;
    }
    close(fd);
    rc = record_frames(ctx, path, stepped);
    if (rc == 0)
        rc = container_ok(path);
    else
        fprintf(stderr, "recording failed for %s timestamps\n", stepped ? "explicit" : "synthesized");
    remove(path);
    avcodec_free_context(&ctx);
    return rc;
}

int main(void)
{
    AVCodecContext *ctx;

    CHECK(ph_record_open(NULL) == NULL);
    CHECK(ph_record_close(NULL) == 0);
    av_log_set_level(AV_LOG_ERROR);
    collect_encoders();
    ctx = open_encoder();
    CHECK(ctx);
    avcodec_free_context(&ctx);
    if (g_failures)
        return 1;
    CHECK(record_and_check(1) == 0);
    CHECK(record_and_check(0) == 0);

    if (g_failures) {
        fprintf(stderr, "%d record check(s) failed\n", g_failures);
        return 1;
    }
    printf("mp4 video stream and duration passed\n");
    return 0;
}
