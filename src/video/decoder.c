#include "video/decoder.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "core/log.h"
#include "core/metrics.h"

#define MAX_QUEUED 240 /* ~4 s at 60 fps; beyond that we're hopelessly behind */

typedef struct packet {
    struct packet *next;
    size_t len;
    uint64_t received_ms;
    uint64_t generation;
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
    uint64_t generation;

    AVFrame *latest; /* guarded by lock */
    uint64_t latest_seq;
    uint64_t consumed_seq;
    ph_frame_timing latest_timing;
    ph_decoder_stats stats;

    void (*on_frame)(void *user);
    void *user;
};

static void free_queue(ph_decoder *d)
{
    while (d->head) {
        packet *p = d->head;
        d->head = p->next;
        d->stats.packets_discarded++;
        free(p);
    }
    d->tail = NULL;
    d->queued = 0;
}

static void reset_locked(ph_decoder *d)
{
    free_queue(d);
    d->generation++;
    d->reset = true;
    av_frame_free(&d->latest);
    memset(&d->latest_timing, 0, sizeof d->latest_timing);
    d->consumed_seq = d->latest_seq;
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
    /* PTS carries local packet provenance only. Rendering remains immediate;
     * this does not schedule frames or change audio/video synchronization. */
    ctx->pkt_timebase = (AVRational){1, 1000};
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
        uint64_t generation = p->generation;
        pthread_mutex_unlock(&d->lock);

        if (av_new_packet(pkt, (int)p->len) == 0) {
            memcpy(pkt->data, p->data, p->len);
            pkt->pts = (int64_t)p->received_ms;
            if (avcodec_send_packet(ctx, pkt) < 0)
                PH_LOGD("decoder rejected a packet (waiting for the next keyframe)");
            av_packet_unref(pkt);
        }
        free(p);

        while (avcodec_receive_frame(ctx, frame) == 0) {
            uint64_t decoded_ms = ph_metrics_now_ms();
            ph_frame_timing timing = {.decoded_ms = decoded_ms};
            if (frame->pts != AV_NOPTS_VALUE && frame->pts > 0
                && (uint64_t)frame->pts <= decoded_ms) {
                timing.received_ms = (uint64_t)frame->pts;
                timing.valid = true;
            }
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
            d->stats.frames_decoded++;
            /* A stream can reset while FFmpeg is working without our lock.
             * Never publish the previous session's in-flight decoded image. */
            if (generation != d->generation || d->quit) {
                d->stats.frames_stale++;
                pthread_mutex_unlock(&d->lock);
                av_frame_free(&out);
                continue;
            }
            if (d->latest && d->consumed_seq != d->latest_seq)
                d->stats.frames_superseded++;
            av_frame_free(&d->latest);
            d->latest = out;
            d->latest_timing = timing;
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
    p->received_ms = ph_metrics_now_ms();
    memcpy(p->data, data, len);

    pthread_mutex_lock(&d->lock);
    d->stats.packets_received++;
    if (d->queued >= MAX_QUEUED) {
        /* Can't drop single packets without corrupting the picture; start over instead. The
         * phone sends SPS/PPS with every keyframe, so decoding resumes at the next one. */
        PH_LOGW("video decoder fell behind; skipping to the next keyframe");
        reset_locked(d);
    }
    p->generation = d->generation;
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
    reset_locked(d);
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->lock);
}

bool ph_decoder_latest(ph_decoder *d, AVFrame *dst, uint64_t *seq)
{
    return ph_decoder_latest_timed(d, dst, seq, NULL);
}

bool ph_decoder_latest_timed(ph_decoder *d, AVFrame *dst, uint64_t *seq,
                             ph_frame_timing *timing)
{
    bool fresh = false;

    if (timing)
        memset(timing, 0, sizeof *timing);
    if (!d || !dst || !seq)
        return false;
    pthread_mutex_lock(&d->lock);
    if (d->latest && d->latest_seq != *seq && av_frame_ref(dst, d->latest) == 0) {
        *seq = d->latest_seq;
        d->consumed_seq = d->latest_seq;
        if (timing)
            *timing = d->latest_timing;
        fresh = true;
    }
    pthread_mutex_unlock(&d->lock);
    return fresh;
}

void ph_decoder_get_stats(ph_decoder *d, ph_decoder_stats *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof *stats);
    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    *stats = d->stats;
    stats->queued_packets = (unsigned int)d->queued;
    pthread_mutex_unlock(&d->lock);
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
