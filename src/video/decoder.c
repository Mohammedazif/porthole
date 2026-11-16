#include "video/decoder.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "core/log.h"

#define MAX_QUEUED 240 /* ~4 s at 60 fps; beyond that we're hopelessly behind */

typedef struct packet {
    struct packet *next;
    size_t len;
    uint8_t data[];
} packet;

struct ph_decoder {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    packet *head, *tail;
    int queued;
    bool reset;
    bool quit;

    AVFrame *latest; /* guarded by lock */
    uint64_t latest_seq;

    void (*on_frame)(void *user);
    void *user;
};

static void free_queue(ph_decoder *d)
{
    while (d->head) {
        packet *p = d->head;
        d->head = p->next;
        free(p);
    }
    d->tail = NULL;
    d->queued = 0;
}

static AVCodecContext *open_codec(void)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx;

    if (!codec) {
        PH_LOGE("this FFmpeg build has no H.264 decoder");
        return NULL;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_type = FF_THREAD_SLICE; /* frame threading would add a frame of latency each */
    ctx->thread_count = 0;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

/* Converts unusual pixel formats to plain YUV 4:2:0 so the display path stays simple. */
static AVFrame *to_yuv420p(const AVFrame *src, struct SwsContext **sws)
{
    AVFrame *dst;

    if (src->format == AV_PIX_FMT_YUV420P || src->format == AV_PIX_FMT_YUVJ420P)
        return av_frame_clone(src);
    *sws = sws_getCachedContext(*sws, src->width, src->height, src->format, src->width,
                                src->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    dst = av_frame_alloc();
    if (!*sws || !dst)
        goto fail;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = src->width;
    dst->height = src->height;
    if (av_frame_get_buffer(dst, 0) < 0)
        goto fail;
    sws_scale(*sws, (const uint8_t *const *)src->data, src->linesize, 0, src->height, dst->data,
              dst->linesize);
    return dst;
fail:
    av_frame_free(&dst);
    return NULL;
}

static void *decode_thread(void *arg)
{
    ph_decoder *d = arg;
    AVCodecContext *ctx = open_codec();
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct SwsContext *sws = NULL;
    int w = 0, h = 0;

    if (!ctx || !pkt || !frame) {
        PH_LOGE("couldn't start the H.264 decoder");
        goto done;
    }

    for (;;) {
        packet *p;

        pthread_mutex_lock(&d->lock);
        while (!d->head && !d->quit && !d->reset)
            pthread_cond_wait(&d->wake, &d->lock);
        if (d->quit) {
            pthread_mutex_unlock(&d->lock);
            break;
        }
        if (d->reset) {
            d->reset = false;
            pthread_mutex_unlock(&d->lock);
            avcodec_flush_buffers(ctx);
            continue;
        }
        p = d->head;
        d->head = p->next;
        if (!d->head)
            d->tail = NULL;
        d->queued--;
        pthread_mutex_unlock(&d->lock);

        if (av_new_packet(pkt, (int)p->len) == 0) {
            memcpy(pkt->data, p->data, p->len);
            if (avcodec_send_packet(ctx, pkt) < 0)
                PH_LOGD("decoder rejected a packet (waiting for the next keyframe)");
            av_packet_unref(pkt);
        }
        free(p);

        while (avcodec_receive_frame(ctx, frame) == 0) {
            AVFrame *out = to_yuv420p(frame, &sws);
            av_frame_unref(frame);
            if (!out)
                continue;
            if (out->width != w || out->height != h) {
                w = out->width;
                h = out->height;
                PH_LOGI("video %dx%d", w, h);
            }
            pthread_mutex_lock(&d->lock);
            av_frame_free(&d->latest);
            d->latest = out;
            d->latest_seq++;
            pthread_mutex_unlock(&d->lock);
            if (d->on_frame)
                d->on_frame(d->user);
        }
    }

done:
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return NULL;
}

ph_decoder *ph_decoder_start(void (*on_frame)(void *user), void *user)
{
    ph_decoder *d = calloc(1, sizeof *d);

    if (!d)
        return NULL;
    d->on_frame = on_frame;
    d->user = user;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->wake, NULL);
    if (pthread_create(&d->thread, NULL, decode_thread, d) != 0) {
        pthread_cond_destroy(&d->wake);
        pthread_mutex_destroy(&d->lock);
        free(d);
        return NULL;
    }
    return d;
}

void ph_decoder_push(ph_decoder *d, const uint8_t *data, size_t len)
{
    packet *p = malloc(sizeof *p + len);

    if (!p)
        return;
    p->next = NULL;
    p->len = len;
    memcpy(p->data, data, len);

    pthread_mutex_lock(&d->lock);
    if (d->queued >= MAX_QUEUED) {
        /* Can't drop single packets without corrupting the picture; start over instead. The
         * phone sends SPS/PPS with every keyframe, so decoding resumes at the next one. */
        PH_LOGW("video decoder fell behind; skipping to the next keyframe");
        free_queue(d);
        d->reset = true;
    }
    if (d->tail)
        d->tail->next = p;
    else
        d->head = p;
    d->tail = p;
    d->queued++;
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->lock);
}

void ph_decoder_reset(ph_decoder *d)
{
    pthread_mutex_lock(&d->lock);
    free_queue(d);
    d->reset = true;
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->lock);
}

bool ph_decoder_latest(ph_decoder *d, AVFrame *dst, uint64_t *seq)
{
    bool fresh = false;

    pthread_mutex_lock(&d->lock);
    if (d->latest && d->latest_seq != *seq && av_frame_ref(dst, d->latest) == 0) {
        *seq = d->latest_seq;
        fresh = true;
    }
    pthread_mutex_unlock(&d->lock);
    return fresh;
}

void ph_decoder_stop(ph_decoder *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    d->quit = true;
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->lock);
    pthread_join(d->thread, NULL);
    free_queue(d);
    av_frame_free(&d->latest);
    pthread_cond_destroy(&d->wake);
    pthread_mutex_destroy(&d->lock);
    free(d);
}
