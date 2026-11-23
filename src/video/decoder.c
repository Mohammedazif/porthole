#include "video/decoder.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

#include "core/log.h"
#include "core/metrics.h"
#include "video/codecs.h"

#define MAX_PACKET_BYTES (8u * 1024u * 1024u)
#define MAX_QUEUE_AGE_MS 350
#define MAX_CODED_PIXELS (4096 * 4096)

typedef struct packet {
    struct packet *next;
    size_t len;
    uint64_t received_ms;
    uint64_t generation;
    uint8_t *data; /* av_malloc, owned until av_packet_from_data */
} packet;

struct ph_decoder {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    packet *head, *tail;
    int queued;
    size_t queued_bytes;
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
    atomic_int max_edge; /* 0: keep the decoded size */
};

static void release_packet(packet *p)
{
    if (!p)
        return;
    av_free(p->data);
    free(p);
}

static void free_queue(ph_decoder *d)
{
    while (d->head) {
        packet *p = d->head;
        d->head = p->next;
        d->stats.packets_discarded++;
        release_packet(p);
    }
    d->tail = NULL;
    d->queued = 0;
    d->queued_bytes = 0;
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

static AVCodecContext *open_codec(enum AVCodecID codec_id)
{
    const AVCodec *decoder = avcodec_find_decoder(codec_id);
    AVCodecContext *ctx;

    if (!decoder)
        return NULL;
    ctx = avcodec_alloc_context3(decoder);
    if (!ctx)
        return NULL;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->max_pixels = MAX_CODED_PIXELS;
    ctx->thread_type = FF_THREAD_SLICE; /* frame threading would add a frame of latency each */
    ctx->thread_count = 4; /* one picture; do not spawn one thread per core */
    /* PTS carries local packet provenance only. Rendering remains immediate;
     * this does not schedule frames or change audio/video synchronization. */
    ctx->pkt_timebase = (AVRational){1, 1000};
    if (avcodec_open2(ctx, decoder, NULL) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

/* Even size that fits in max_edge on the longer side, or the original size. */
static void fit_edge(int width, int height, int max_edge, int *out_w, int *out_h)
{
    int long_edge = width > height ? width : height;
    int w, h;

    if (max_edge < 2 || long_edge <= max_edge || width < 2 || height < 2) {
        *out_w = width;
        *out_h = height;
        return;
    }
    if (width >= height) {
        w = max_edge & ~1;
        h = (int)(((long long)height * w + width / 2) / width) & ~1;
    } else {
        h = max_edge & ~1;
        w = (int)(((long long)width * h + height / 2) / height) & ~1;
    }
    if (w < 2)
        w = 2;
    if (h < 2)
        h = 2;
    *out_w = w;
    *out_h = h;
}

/* Converts unusual pixel formats to plain YUV 4:2:0 so the display path stays simple.
 * A max_edge above 0 also scales a larger picture down to that longer side. */
static AVFrame *to_yuv420p(const AVFrame *src, struct SwsContext **sws, int max_edge)
{
    AVFrame *dst;
    int dw, dh;

    fit_edge(src->width, src->height, max_edge, &dw, &dh);
    if ((src->format == AV_PIX_FMT_YUV420P || src->format == AV_PIX_FMT_YUVJ420P)
        && dw == src->width && dh == src->height)
        return av_frame_clone(src);
    *sws = sws_getCachedContext(*sws, src->width, src->height, src->format, dw, dh,
                                AV_PIX_FMT_YUV420P,
                                dw == src->width && dh == src->height ? SWS_BILINEAR
                                                                      : SWS_FAST_BILINEAR,
                                NULL, NULL, NULL);
    dst = av_frame_alloc();
    if (!*sws || !dst)
        goto fail;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = dw;
    dst->height = dh;
    dst->color_range = src->color_range;
    dst->colorspace = src->colorspace;
    if (av_frame_get_buffer(dst, 0) < 0)
        goto fail;
    if (sws_scale(*sws, (const uint8_t *const *)src->data, src->linesize, 0, src->height, dst->data,
                  dst->linesize) != dh)
        goto fail;
    return dst;
fail:
    av_frame_free(&dst);
    return NULL;
}

static void *decode_thread(void *arg)
{
    ph_decoder *d = arg;
    AVCodecContext *ctx = open_codec(AV_CODEC_ID_H264);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct SwsContext *sws = NULL;
    ph_codec current = ctx ? PH_CODEC_H264 : PH_CODEC_UNKNOWN;
    bool logged_h264 = ctx == NULL;
    bool logged_h265 = false;
    int w = 0, h = 0;

    if (!ctx)
        PH_LOGE("this FFmpeg build has no H.264 decoder");
    if (!pkt || !frame) {
        PH_LOGE("couldn't start the video decoder");
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
            if (ctx)
                avcodec_flush_buffers(ctx);
            continue;
        }
        p = d->head;
        if (ph_metrics_now_ms() - p->received_ms > MAX_QUEUE_AGE_MS) {
            reset_locked(d);
            pthread_mutex_unlock(&d->lock);
            continue;
        }
        d->head = p->next;
        if (!d->head)
            d->tail = NULL;
        d->queued--;
        d->queued_bytes -= p->len;
        uint64_t generation = p->generation;
        pthread_mutex_unlock(&d->lock);

        {
            /* A steady stream repeats the same codec. Read the first NAL only;
             * scan the whole access unit when the codec may have changed. */
            ph_codec detected = current == PH_CODEC_UNKNOWN ? PH_CODEC_UNKNOWN
                                                            : ph_codec_leading_nal(p->data, p->len);
            if (current == PH_CODEC_UNKNOWN || detected != current) {
                detected = ph_codec_from_annexb(p->data, p->len);
                if (current != PH_CODEC_UNKNOWN && detected != PH_CODEC_UNKNOWN
                    && detected != current
                    && !ph_codec_annexb_unambiguous(p->data, p->len, detected))
                    detected = PH_CODEC_UNKNOWN;
                if (detected != PH_CODEC_UNKNOWN && detected != current) {
                    enum AVCodecID codec_id = detected == PH_CODEC_H264 ? AV_CODEC_ID_H264
                                                                        : AV_CODEC_ID_HEVC;
                    avcodec_free_context(&ctx);
                    ctx = open_codec(codec_id);
                    if (!ctx) {
                        if (detected == PH_CODEC_H265) {
                            if (!logged_h265)
                                PH_LOGE("this FFmpeg build has no H.265 decoder");
                            logged_h265 = true;
                        } else if (!logged_h264) {
                            PH_LOGE("this FFmpeg build has no H.264 decoder");
                            logged_h264 = true;
                        }
                        current = PH_CODEC_UNKNOWN;
                        release_packet(p);
                        continue;
                    }
                    current = detected;
                }
            }
        }
        if (!ctx) {
            release_packet(p);
            continue;
        }

        av_packet_unref(pkt);
        if (av_packet_from_data(pkt, p->data, (int)p->len) == 0) {
            p->data = NULL;
            pkt->pts = (int64_t)p->received_ms;
            if (avcodec_send_packet(ctx, pkt) < 0)
                PH_LOGD("decoder rejected a packet (waiting for the next keyframe)");
            av_packet_unref(pkt);
        }
        release_packet(p);

        while (avcodec_receive_frame(ctx, frame) == 0) {
            uint64_t decoded_ms = ph_metrics_now_ms();
            ph_frame_timing timing = {.decoded_ms = decoded_ms};
            if (frame->pts != AV_NOPTS_VALUE && frame->pts > 0
                && (uint64_t)frame->pts <= decoded_ms) {
                timing.received_ms = (uint64_t)frame->pts;
                timing.valid = true;
            }
            AVFrame *out = to_yuv420p(frame, &sws, atomic_load(&d->max_edge));
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
    atomic_init(&d->max_edge, 0);
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

void ph_decoder_limit_edge(ph_decoder *d, int long_edge)
{
    if (d && long_edge >= 0)
        atomic_store(&d->max_edge, long_edge);
}

void ph_decoder_push(ph_decoder *d, const uint8_t *data, size_t len)
{
    uint8_t *copy;
    packet *p;

    if (!d || !data || !len || len > MAX_PACKET_BYTES)
        return;
    copy = av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE);
    p = malloc(sizeof *p);
    if (!copy || !p) {
        av_free(copy);
        free(p);
        return;
    }
    memcpy(copy, data, len);
    memset(copy + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    p->next = NULL;
    p->len = len;
    p->data = copy;
    pthread_mutex_lock(&d->lock);
    p->received_ms = ph_metrics_now_ms();
    d->stats.packets_received++;
    if (d->queued >= PH_DECODER_MAX_QUEUED ||
        len > PH_DECODER_MAX_QUEUED_BYTES - d->queued_bytes ||
        (d->head && p->received_ms - d->head->received_ms > MAX_QUEUE_AGE_MS)) {
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
    d->queued_bytes += len;
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
    stats->queued_bytes = d->queued_bytes;
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
