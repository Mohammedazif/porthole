#include <math.h>
#include <stdio.h>

#include "core/metrics.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; \
} } while (0)

int main(void)
{
    ph_metrics metrics = {0};
    ph_metrics_snapshot view;
    ph_metrics_snapshot_get(&metrics, 1000, &view);
    CHECK(!view.receive_to_decode.known && !view.receive_to_present.known);
    CHECK(!view.input_handoff.known && !view.helper_ack.known);
    CHECK(!view.fps_known && !view.bluetooth_interval_known && !view.pending_acks);

    /* Device-visible latency cannot be invented from a missing or reversed
     * clock. Presentation FPS can still be observed without packet provenance. */
    ph_metrics_frame(&metrics, 0, 100, 120);
    ph_metrics_frame(&metrics, 200, 150, 250);
    ph_metrics_frame(&metrics, 260, 270, 265);
    ph_metrics_snapshot_get(&metrics, 300, &view);
    CHECK(!view.receive_to_decode.known && !view.receive_to_present.known);
    CHECK(view.fps_known && view.frames_presented == 3 && view.fps == 3);

    ph_metrics_reset(&metrics);
    ph_metrics_frame(&metrics, 100, 112, 125);
    ph_metrics_input(&metrics, 200, 203);
    ph_metrics_hid_sent(&metrics, 0x80000001, 220);
    CHECK(ph_metrics_hid_ack(&metrics, 0x80000001, 270));
    ph_metrics_snapshot_get(&metrics, 300, &view);
    CHECK(view.receive_to_decode.known && view.receive_to_decode.last_ms == 12);
    CHECK(view.receive_to_present.known && view.receive_to_present.last_ms == 25);
    CHECK(view.input_handoff.known && view.input_handoff.last_ms == 3);
    CHECK(view.helper_ack.known && view.helper_ack.last_ms == 50);
    CHECK(view.acks_received == 1 && view.pending_acks == 0);
    CHECK(!view.bluetooth_interval_known); /* helper report spacing proves no radio interval */

    CHECK(!ph_metrics_hid_ack(&metrics, 0x80000001, 280));
    ph_metrics_hid_sent(&metrics, 12, 500);
    CHECK(!ph_metrics_hid_ack(&metrics, 12, 499));
    CHECK(ph_metrics_hid_ack(&metrics, 12, 525));
    ph_metrics_snapshot_get(&metrics, 550, &view);
    CHECK(view.acks_unmatched == 2 && view.helper_ack.samples == 2);
    CHECK(view.helper_ack.mean_ms == 37.5 && view.helper_ack.p95_ms == 50);

    /* A new connection can restart IDs: timing must use the newest dispatch. */
    ph_metrics_hid_sent(&metrics, 15, 600);
    ph_metrics_hid_sent(&metrics, 15, 650);
    CHECK(ph_metrics_hid_ack(&metrics, 15, 675));
    ph_metrics_snapshot_get(&metrics, 700, &view);
    CHECK(view.helper_ack.last_ms == 25 && view.acks_overwritten == 1);

    ph_metrics_hid_sent(&metrics, 20, 1000);
    ph_metrics_snapshot_get(&metrics, 10999, &view);
    CHECK(view.pending_acks == 1 && view.acks_expired == 0);
    CHECK(!ph_metrics_hid_ack(&metrics, 20, 11000));
    ph_metrics_snapshot_get(&metrics, 11000, &view);
    CHECK(view.pending_acks == 0 && view.acks_expired == 1);

    /* Delayed/unmatched helper ACKs cannot grow telemetry without bound. */
    ph_metrics_reset(&metrics);
    for (uint32_t i = 0; i <= PH_METRICS_PENDING; i++)
        ph_metrics_hid_sent(&metrics, i, 1000 + i);
    ph_metrics_snapshot_get(&metrics, 2000, &view);
    CHECK(view.pending_acks == PH_METRICS_PENDING && view.acks_overwritten == 1);
    CHECK(!ph_metrics_hid_ack(&metrics, 0, 2000));
    CHECK(ph_metrics_hid_ack(&metrics, PH_METRICS_PENDING, 2000));

    ph_metrics_reset(&metrics);
    for (uint64_t i = 1; i <= 100; i++)
        ph_metrics_input(&metrics, 1000, 1000 + i);
    ph_metrics_snapshot_get(&metrics, 2000, &view);
    CHECK(view.input_handoff.samples == 100 && view.input_handoff.last_ms == 100);
    CHECK(view.input_handoff.mean_ms == 68.5 && view.input_handoff.p95_ms == 97);
    ph_metrics_input(&metrics, 0, 10);
    ph_metrics_input(&metrics, 100, 99);
    ph_metrics_snapshot_get(&metrics, 2000, &view);
    CHECK(view.input_handoff.samples == 100);

    ph_metrics_reset(&metrics);
    for (uint64_t i = 0; i < 64; i++)
        ph_metrics_frame(&metrics, 999 + i * 16, 1000 + i * 16, 1001 + i * 16);
    ph_metrics_snapshot_get(&metrics, 2009, &view);
    CHECK(view.fps_known && view.fps == 63 && view.frames_presented == 64);
    ph_metrics_snapshot_get(&metrics, 3009, &view);
    CHECK(view.fps_known && view.fps == 0); /* idle phone, not a fake 60 fps */
    uint64_t samples = view.receive_to_decode.samples;
    ph_metrics_frame(&metrics, 1, 2, 3); /* regressed presentation clock */
    ph_metrics_snapshot_get(&metrics, 3009, &view);
    CHECK(view.frames_presented == 64 && view.receive_to_decode.samples == samples);

    ph_metrics_set_bluetooth_interval(&metrics, 15);
    ph_metrics_snapshot_get(&metrics, 3100, &view);
    CHECK(view.bluetooth_interval_known && view.bluetooth_interval_ms == 15);
    ph_metrics_set_bluetooth_interval(&metrics, NAN);
    ph_metrics_snapshot_get(&metrics, 3100, &view);
    CHECK(!view.bluetooth_interval_known && view.bluetooth_interval_ms == 0);

    uint64_t now = ph_metrics_now_ms();
    CHECK(now > 0 && ph_metrics_now_ms() >= now);
    ph_metrics_reset(&metrics);
    ph_metrics_snapshot_get(&metrics, now, &view);
    CHECK(!view.helper_ack.known && !view.fps_known && !view.frames_presented);
    if (failures)
        return 1;
    puts("local timing, unknown values, bounded ACK tracking and idle FPS passed");
    return 0;
}
