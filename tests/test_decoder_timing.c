/* A real 32x32 black H.264 IDR, generated locally with x264 (SEI removed).
 * No encoder, phone, display or network is needed by this regression. */
#include <stdatomic.h>
#include <stdio.h>

#ifdef _WIN32
#include <SDL.h>
#endif

#include <libavutil/frame.h>
#include <libavutil/time.h>

#include "core/metrics.h"
#include "video/decoder.h"

static const uint8_t black_idr[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdc, 0x96, 0x84, 0x00,
    0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xf2, 0x3c, 0x48, 0x9e,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01, 0x65,
    0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xc9, 0xd7, 0x5e,
};

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; \
} } while (0)

#ifdef PORTHOLE_TEST_DECODER_RACE
static atomic_bool delay_next_clone, clone_paused, release_clone;
AVFrame *__real_av_frame_clone(const AVFrame *src);
AVFrame *__wrap_av_frame_clone(const AVFrame *src);

AVFrame *__wrap_av_frame_clone(const AVFrame *src)
{
    AVFrame *copy = __real_av_frame_clone(src);
    if (atomic_exchange(&delay_next_clone, false)) {
        atomic_store(&clone_paused, true);
        uint64_t deadline = ph_metrics_now_ms() + 4000;
        while (!atomic_load(&release_clone) && ph_metrics_now_ms() < deadline)
            av_usleep(1000);
    }
    return copy;
}
#endif

static void frame_ready(void *user)
{
    atomic_uint *frames = user;
    atomic_fetch_add(frames, 1);
}

static bool wait_frames(atomic_uint *frames, unsigned int count)
{
    uint64_t deadline = ph_metrics_now_ms() + 3000;
    while (atomic_load(frames) < count && ph_metrics_now_ms() < deadline)
        av_usleep(1000);
    return atomic_load(frames) >= count;
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    atomic_uint frames = 0;
    ph_decoder *decoder = ph_decoder_start(frame_ready, &frames);
    AVFrame *frame = av_frame_alloc();
    ph_frame_timing timing;
    ph_decoder_stats stats;
    uint64_t sequence = 0;
    CHECK(decoder && frame);
    if (!decoder || !frame)
        goto done;

    uint64_t began = ph_metrics_now_ms();
    ph_decoder_push(decoder, black_idr, sizeof black_idr);
    CHECK(wait_frames(&frames, 1));
    CHECK(ph_decoder_latest_timed(decoder, frame, &sequence, &timing));
    CHECK(frame->width == 32 && frame->height == 32 && timing.capture_pts_ns == 0);
    CHECK(timing.valid && timing.received_ms >= began);
    CHECK(timing.decoded_ms >= timing.received_ms && timing.decoded_ms <= ph_metrics_now_ms());
    CHECK(sequence == 1 && frame->pts == (int64_t)timing.received_ms);
    av_frame_unref(frame);
    CHECK(!ph_decoder_latest_timed(decoder, frame, &sequence, &timing) && !timing.valid);

    /* The worker may decode several frames before the event loop can display
     * one. Timing must follow the retained newest picture, not its predecessor. */
    av_usleep(10000);
    began = ph_metrics_now_ms();
    for (unsigned int i = 0; i < 3; i++)
        ph_decoder_push_timestamped(decoder, black_idr, sizeof black_idr, 1000000000ULL + i);
    CHECK(wait_frames(&frames, 4));
    CHECK(ph_decoder_latest_timed(decoder, frame, &sequence, &timing));
    CHECK(sequence == 4 && timing.valid && timing.received_ms >= began);
    CHECK(timing.capture_pts_ns == 1000000002ULL);
    CHECK(timing.decoded_ms >= timing.received_ms);
    ph_decoder_get_stats(decoder, &stats);
    CHECK(stats.packets_received == 4 && stats.frames_decoded == 4);
    CHECK(stats.frames_superseded == 2 && stats.queued_packets == 0);
    CHECK(stats.packets_discarded == 0);
    av_frame_unref(frame);

    ph_decoder_reset(decoder);
    began = ph_metrics_now_ms();
    ph_decoder_push(decoder, black_idr, sizeof black_idr);
    CHECK(wait_frames(&frames, 5));
    CHECK(ph_decoder_latest_timed(decoder, frame, &sequence, &timing));
    CHECK(timing.valid && timing.received_ms >= began && sequence == 5);
    av_frame_unref(frame);

#ifdef PORTHOLE_TEST_DECODER_RACE
    /* Hold a real decoded output immediately before publication, then reset.
     * A cached or in-flight image from the old stream must never reappear. */
    atomic_store(&delay_next_clone, true);
    ph_decoder_push(decoder, black_idr, sizeof black_idr);
    uint64_t deadline = ph_metrics_now_ms() + 3000;
    while (!atomic_load(&clone_paused) && ph_metrics_now_ms() < deadline)
        av_usleep(1000);
    CHECK(atomic_load(&clone_paused));
    /* The worker is inside publication, so these copies sit in the queue.
     * Overflow must discard a whole run and stay within the cap. */
    for (unsigned int i = 0; i < PH_DECODER_MAX_QUEUED + 30; i++)
        ph_decoder_push(decoder, black_idr, sizeof black_idr);
    ph_decoder_get_stats(decoder, &stats);
    CHECK(stats.queued_packets <= PH_DECODER_MAX_QUEUED);
    CHECK(stats.packets_discarded >= PH_DECODER_MAX_QUEUED);
    ph_decoder_reset(decoder);
    uint64_t empty_sequence = 0;
    CHECK(!ph_decoder_latest_timed(decoder, frame, &empty_sequence, &timing));
    av_frame_unref(frame);
    atomic_store(&release_clone, true);
    deadline = ph_metrics_now_ms() + 3000;
    do {
        ph_decoder_get_stats(decoder, &stats);
        if (stats.frames_stale || atomic_load(&frames) > 5)
            break;
        av_usleep(1000);
    } while (ph_metrics_now_ms() < deadline);
    CHECK(stats.frames_stale == 1 && atomic_load(&frames) == 5);
    CHECK(!ph_decoder_latest_timed(decoder, frame, &sequence, &timing));
    av_frame_unref(frame);
    began = ph_metrics_now_ms();
    ph_decoder_push(decoder, black_idr, sizeof black_idr);
    CHECK(wait_frames(&frames, 6));
    CHECK(ph_decoder_latest_timed(decoder, frame, &sequence, &timing));
    CHECK(sequence == 6 && timing.valid && timing.received_ms >= began);
    av_frame_unref(frame);
#endif

    ph_decoder_get_stats(NULL, &stats);
    CHECK(!stats.packets_received && !stats.frames_decoded);
done:
#ifdef PORTHOLE_TEST_DECODER_RACE
    atomic_store(&release_clone, true);
#endif
    av_frame_free(&frame);
    ph_decoder_stop(decoder);
    if (failures)
        return 1;
    puts("H.264 provenance, decode timing, superseded frames and reset generations passed");
    return 0;
}
