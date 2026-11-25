#ifndef PORTHOLE_CORE_METRICS_H
#define PORTHOLE_CORE_METRICS_H

/* Main-thread diagnostics. Times use ph_metrics_now_ms() or a shared monotonic
 * clock supplied by the caller. They measure local processing, not phone-visible
 * input lag or capture-to-display latency. Zero timestamps mean unavailable. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PH_METRICS_SAMPLES 64
#define PH_METRICS_PENDING 64
#define PH_METRICS_FRAMES 128
#define PH_METRICS_ACK_EXPIRY_MS 10000

typedef struct ph_metric_series {
    double values[PH_METRICS_SAMPLES];
    size_t next, used;
    uint64_t samples;
} ph_metric_series;

typedef struct ph_metric_summary {
    bool known;
    uint64_t samples;
    double last_ms, mean_ms, p95_ms;
} ph_metric_summary;

typedef struct ph_metrics_pending {
    bool used;
    uint32_t id;
    uint64_t sent_ms;
} ph_metrics_pending;

typedef struct ph_metrics {
    ph_metric_series receive_to_decode, receive_to_present, input_handoff, helper_ack;
    ph_metrics_pending pending[PH_METRICS_PENDING];
    uint64_t presented_at[PH_METRICS_FRAMES];
    size_t pending_next, frame_next, frame_used;
    uint64_t frames_presented, acks_received, acks_unmatched, acks_expired, acks_overwritten;
    /* Populated only from a measured backend value. Report pacing is not a
     * negotiated radio connection interval. Zero means unknown. */
    double bluetooth_interval_ms;
} ph_metrics;

typedef struct ph_metrics_snapshot {
    ph_metric_summary receive_to_decode, receive_to_present, input_handoff, helper_ack;
    bool fps_known, bluetooth_interval_known;
    double fps, bluetooth_interval_ms;
    uint64_t frames_presented, acks_received, acks_unmatched, acks_expired, acks_overwritten;
    size_t pending_acks;
} ph_metrics_snapshot;

uint64_t ph_metrics_now_ms(void);
/* The same monotonic clock in microseconds. */
uint64_t ph_metrics_now_us(void);
void ph_metrics_reset(ph_metrics *metrics);
void ph_metrics_frame(ph_metrics *metrics, uint64_t received_ms, uint64_t decoded_ms,
                       uint64_t presented_ms);
void ph_metrics_input(ph_metrics *metrics, uint64_t event_ms, uint64_t handed_off_ms);
void ph_metrics_hid_sent(ph_metrics *metrics, uint32_t id, uint64_t now_ms);
bool ph_metrics_hid_ack(ph_metrics *metrics, uint32_t id, uint64_t now_ms);
void ph_metrics_set_bluetooth_interval(ph_metrics *metrics, double measured_ms);
void ph_metrics_snapshot_get(ph_metrics *metrics, uint64_t now_ms, ph_metrics_snapshot *out);

#endif
