#include "media/shot.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

#include "core/log.h"
#include "core/upath.h"

static void log_av(const char *what, int err)
{
    char text[AV_ERROR_MAX_STRING_SIZE];

    av_make_error_string(text, sizeof text, err);
    PH_LOGW("%s: %s", what, text);
}

int ph_shot_write_png(const struct AVFrame *frame, const char *path)
{
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    AVFrame *rgb = NULL;
    AVPacket *packet = NULL;
    struct SwsContext *sws = NULL;
    FILE *out = NULL;
    int err = -1;
    int failed = -1;

    if (!frame || !path || !path[0] || frame->width <= 0 || frame->height <= 0)
        return -1;
    if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
        return -1;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2])
        return -1;

    codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    ctx = avcodec_alloc_context3(codec);
    rgb = av_frame_alloc();
    packet = av_packet_alloc();
    if (!codec || !ctx || !rgb || !packet)
        goto done;
    ctx->width = frame->width;
    ctx->height = frame->height;
    ctx->pix_fmt = AV_PIX_FMT_RGB24;
    ctx->time_base = (AVRational){1, 1};
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        PH_LOGW("couldn't open the PNG encoder");
        goto done;
    }
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width = frame->width;
    rgb->height = frame->height;
    if (av_frame_get_buffer(rgb, 1) < 0)
        goto done;
    sws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                         frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL,
                         NULL);
    if (!sws)
        goto done;
    if (sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                  rgb->data, rgb->linesize) <= 0)
        goto done;
    rgb->pts = 0;
    err = avcodec_send_frame(ctx, rgb);
    if (err < 0) {
        log_av("couldn't encode a PNG frame", err);
        goto done;
    }
    err = avcodec_send_frame(ctx, NULL);
    if (err < 0 && err != AVERROR_EOF) {
        log_av("couldn't flush the PNG encoder", err);
        goto done;
    }
    err = avcodec_receive_packet(ctx, packet);
    if (err < 0) {
        log_av("PNG encoder produced no packet", err);
        goto done;
    }
    out = ph_fopen(path, "wb");
    if (!out) {
        PH_LOGW("couldn't create PNG %s", path);
        goto done;
    }
    if (fwrite(packet->data, 1, (size_t)packet->size, out) != (size_t)packet->size) {
        fclose(out);
        out = NULL;
        remove(path);
        goto done;
    }
    if (fclose(out) != 0) {
        out = NULL;
        remove(path);
        goto done;
    }
    out = NULL;
    failed = 0;
done:
    if (out)
        fclose(out);
    sws_freeContext(sws);
    av_packet_free(&packet);
    av_frame_free(&rgb);
    avcodec_free_context(&ctx);
    return failed;
}

int ph_shot_write_png_rect(const struct AVFrame *frame, int x, int y, int w, int h,
                           const char *path)
{
    AVFrame view;

    if (!frame || x < 0 || y < 0 || w < 2 || h < 2)
        return -1;
    if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
        return -1;
    if (x + w > frame->width || y + h > frame->height)
        return -1;
    x &= ~1;
    y &= ~1;
    w &= ~1;
    h &= ~1;
    if (w < 2 || h < 2 || x + w > frame->width || y + h > frame->height)
        return -1;
    memset(&view, 0, sizeof view);
    view.format = frame->format;
    view.width = w;
    view.height = h;
    view.data[0] = frame->data[0] + (size_t)y * (size_t)frame->linesize[0] + (size_t)x;
    view.data[1] = frame->data[1] + (size_t)(y / 2) * (size_t)frame->linesize[1] + (size_t)(x / 2);
    view.data[2] = frame->data[2] + (size_t)(y / 2) * (size_t)frame->linesize[2] + (size_t)(x / 2);
    view.linesize[0] = frame->linesize[0];
    view.linesize[1] = frame->linesize[1];
    view.linesize[2] = frame->linesize[2];
    return ph_shot_write_png(&view, path);
}
