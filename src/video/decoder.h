#ifndef PORTHOLE_VIDEO_DECODER_H
#define PORTHOLE_VIDEO_DECODER_H

/* H.264 decoder on its own thread. Packets go in from any thread; only the newest decoded
 * picture is kept, so the display never falls behind the phone. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AVFrame;

typedef struct ph_decoder ph_decoder;

typedef struct ph_frame_timing {
    bool valid;
    /* Local monotonic receipt of the access unit and completion of its decode.
     * These are not iPhone capture timestamps or synchronized AirPlay NTP time. */
    uint64_t received_ms, decoded_ms;
} ph_frame_timing;

/* Past this many waiting access units the queue is dropped and decoding resumes
 * at the next keyframe. One picture is retained either way. */
#define PH_DECODER_MAX_QUEUED 240

typedef struct ph_decoder_stats {
    uint64_t packets_received, packets_discarded;
    uint64_t frames_decoded, frames_superseded, frames_stale;
    unsigned int queued_packets;
} ph_decoder_stats;

/* `on_frame` is called on the decoder thread whenever a new picture is ready. */
ph_decoder *ph_decoder_start(void (*on_frame)(void *user), void *user);

/* Copies one access unit and queues it. Safe to call from any thread. */
void ph_decoder_push(ph_decoder *d, const uint8_t *data, size_t len);

/* Drops queued data and decoder state (use when a new stream starts). */
void ph_decoder_reset(ph_decoder *d);

/* If a picture newer than *seq exists, references it into `dst` (YUV 4:2:0), updates *seq
 * and returns true. `dst` must be an allocated, empty AVFrame. */
bool ph_decoder_latest(ph_decoder *d, struct AVFrame *dst, uint64_t *seq);
/* Same operation, also returns receipt/decode timing for this exact picture.
 * The clock is ph_metrics_now_ms(); invalid timing must be displayed as unknown. */
bool ph_decoder_latest_timed(ph_decoder *d, struct AVFrame *dst, uint64_t *seq,
                             ph_frame_timing *timing);
void ph_decoder_get_stats(ph_decoder *d, ph_decoder_stats *stats);

void ph_decoder_stop(ph_decoder *d);

#endif
