#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "core/metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

uint64_t ph_metrics_now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency)
        || ticks.QuadPart < 0 || frequency.QuadPart <= 0)
        return 0;
    uint64_t counter = (uint64_t)ticks.QuadPart, rate = (uint64_t)frequency.QuadPart;
    return counter / rate * 1000000 + counter % rate * 1000000 / rate;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
#endif
}

uint64_t ph_metrics_now_ms(void)
{
    return ph_metrics_now_us() / 1000;
}

void ph_metrics_reset(ph_metrics *m)
{
    if (m)
        memset(m, 0, sizeof *m);
}

static void sample(ph_metric_series *series, uint64_t start_ms, uint64_t end_ms)
{
    if (!start_ms || !end_ms || end_ms < start_ms)
        return;
    series->values[series->next] = (double)(end_ms - start_ms);
    series->next = (series->next + 1) % PH_METRICS_SAMPLES;
    if (series->used < PH_METRICS_SAMPLES)
        series->used++;
    series->samples++;
}

void ph_metrics_frame(ph_metrics *m, uint64_t received_ms, uint64_t decoded_ms,
                       uint64_t presented_ms)
{
    if (!m || !presented_ms)
        return;
    if (m->frame_used) {
        size_t last = (m->frame_next + PH_METRICS_FRAMES - 1) % PH_METRICS_FRAMES;
        if (presented_ms < m->presented_at[last])
            return;
    }
    /* Never combine timestamps from different clocks or a reversed pipeline. */
    if (received_ms && decoded_ms && received_ms <= decoded_ms && decoded_ms <= presented_ms) {
        sample(&m->receive_to_decode, received_ms, decoded_ms);
        sample(&m->receive_to_present, received_ms, presented_ms);
    }
    m->presented_at[m->frame_next] = presented_ms;
    m->frame_next = (m->frame_next + 1) % PH_METRICS_FRAMES;
    if (m->frame_used < PH_METRICS_FRAMES)
        m->frame_used++;
    m->frames_presented++;
}

void ph_metrics_input(ph_metrics *m, uint64_t event_ms, uint64_t handed_off_ms)
{
    if (m)
        sample(&m->input_handoff, event_ms, handed_off_ms);
}

static void expire(ph_metrics *m, uint64_t now_ms)
{
    for (size_t i = 0; i < PH_METRICS_PENDING; i++) {
        ph_metrics_pending *p = &m->pending[i];
        if (p->used && now_ms >= p->sent_ms
            && now_ms - p->sent_ms >= PH_METRICS_ACK_EXPIRY_MS) {
            p->used = false;
            m->acks_expired++;
        }
    }
}

void ph_metrics_hid_sent(ph_metrics *m, uint32_t id, uint64_t now_ms)
{
    if (!m || !now_ms)
        return;
    expire(m, now_ms);
    /* IDs can restart when a helper restarts. Replace a duplicate instead of
     * accidentally timing its eventual ACK against an unrelated old request. */
    for (size_t i = 0; i < PH_METRICS_PENDING; i++) {
        if (m->pending[i].used && m->pending[i].id == id) {
            m->pending[i].sent_ms = now_ms;
            m->acks_overwritten++;
            return;
        }
    }
    size_t slot = m->pending_next;
    for (size_t i = 0; i < PH_METRICS_PENDING; i++) {
        size_t candidate = (m->pending_next + i) % PH_METRICS_PENDING;
        if (!m->pending[candidate].used) {
            slot = candidate;
            break;
        }
    }
    if (m->pending[slot].used)
        m->acks_overwritten++;
    m->pending[slot] = (ph_metrics_pending){true, id, now_ms};
    m->pending_next = (slot + 1) % PH_METRICS_PENDING;
}

bool ph_metrics_hid_ack(ph_metrics *m, uint32_t id, uint64_t now_ms)
{
    if (!m || !now_ms)
        return false;
    expire(m, now_ms);
    for (size_t i = 0; i < PH_METRICS_PENDING; i++) {
        ph_metrics_pending *p = &m->pending[i];
        if (p->used && p->id == id && now_ms >= p->sent_ms) {
            sample(&m->helper_ack, p->sent_ms, now_ms);
            p->used = false;
            m->acks_received++;
            return true;
        }
    }
    m->acks_unmatched++;
    return false;
}

void ph_metrics_set_bluetooth_interval(ph_metrics *m, double measured_ms)
{
    if (m)
        m->bluetooth_interval_ms = isfinite(measured_ms) && measured_ms > 0 ? measured_ms : 0;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static ph_metric_summary summarize(const ph_metric_series *series)
{
    ph_metric_summary out = {.samples = series->samples};
    if (!series->used)
        return out;
    out.known = true;
    out.last_ms = series->values[(series->next + PH_METRICS_SAMPLES - 1) % PH_METRICS_SAMPLES];
    double sorted[PH_METRICS_SAMPLES];
    for (size_t i = 0; i < series->used; i++) {
        sorted[i] = series->values[i];
        out.mean_ms += sorted[i];
    }
    out.mean_ms /= (double)series->used;
    qsort(sorted, series->used, sizeof sorted[0], compare_double);
    size_t p95 = (series->used * 95 + 99) / 100 - 1;
    out.p95_ms = sorted[p95];
    return out;
}

void ph_metrics_snapshot_get(ph_metrics *m, uint64_t now_ms, ph_metrics_snapshot *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!m)
        return;
    expire(m, now_ms);
    out->receive_to_decode = summarize(&m->receive_to_decode);
    out->receive_to_present = summarize(&m->receive_to_present);
    out->input_handoff = summarize(&m->input_handoff);
    out->helper_ack = summarize(&m->helper_ack);
    out->bluetooth_interval_known = m->bluetooth_interval_ms > 0;
    out->bluetooth_interval_ms = m->bluetooth_interval_ms;
    out->frames_presented = m->frames_presented;
    out->acks_received = m->acks_received;
    out->acks_unmatched = m->acks_unmatched;
    out->acks_expired = m->acks_expired;
    out->acks_overwritten = m->acks_overwritten;
    for (size_t i = 0; i < PH_METRICS_PENDING; i++)
        out->pending_acks += m->pending[i].used;
    if (!m->frame_used || !now_ms)
        return;
    /* Observed presentation rate over the most recent second. An idle screen
     * legitimately reports zero: AirPlay need not resend unchanged pictures. */
    out->fps_known = true;
    for (size_t i = 0; i < m->frame_used; i++) {
        uint64_t at = m->presented_at[i];
        if (now_ms >= at && now_ms - at < 1000)
            out->fps += 1;
    }
}
