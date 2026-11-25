#include "audio/sync.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct ph_sync {
    bool have_video;
    bool have_audio;
    uint64_t video_pts;
    uint64_t video_at_ms;
    uint64_t audio_pts;
    int offset_ms;
};

#define HOLD_ABOVE_MS 40
#define HOLD_MAX_MS 200
#define IMPLAUSIBLE_LEAD_MS 1000

static bool jumped_back(bool have, uint64_t previous, uint64_t pts)
{
    return have && previous > pts && previous - pts > 500000000ULL;
}

static ph_sync_action decide(uint64_t audio_pts, uint64_t video_now, int offset_ms)
{
    ph_sync_action action = {PH_SYNC_PLAY, 0, 0, true};
    const int64_t ms = 1000000LL;
    int64_t lead;

    /* Leads are bounded to a few seconds in practice; saturate far outliers. */
    if (audio_pts >= video_now) {
        uint64_t ahead = audio_pts - video_now;
        lead = ahead > (uint64_t)(INT32_MAX / 2) * (uint64_t)ms ? (int64_t)(INT32_MAX / 2) * ms
                                                                : (int64_t)ahead;
    } else {
        uint64_t behind = video_now - audio_pts;
        lead = behind > (uint64_t)(INT32_MAX / 2) * (uint64_t)ms ? -(int64_t)(INT32_MAX / 2) * ms
                                                                 : -(int64_t)behind;
    }
    lead += (int64_t)offset_ms * ms; /* positive offset delays audio: treat it as further ahead */
    action.lead_ms = (int)(lead / ms);

    if (lead > HOLD_ABOVE_MS * ms && lead <= IMPLAUSIBLE_LEAD_MS * ms) {
        action.kind = PH_SYNC_HOLD;
        action.hold_or_drop_ms = lead > HOLD_MAX_MS * ms ? HOLD_MAX_MS : (int)(lead / ms);
    }
    return action;
}

ph_sync *ph_sync_create(void)
{
    return calloc(1, sizeof(ph_sync));
}

void ph_sync_destroy(ph_sync *sync)
{
    free(sync);
}

void ph_sync_reset(ph_sync *sync)
{
    int offset;

    if (!sync)
        return;
    offset = sync->offset_ms;
    sync->have_video = false;
    sync->have_audio = false;
    sync->video_pts = 0;
    sync->video_at_ms = 0;
    sync->audio_pts = 0;
    sync->offset_ms = offset;
}

void ph_sync_set_offset_ms(ph_sync *sync, int offset_ms)
{
    if (!sync)
        return;
    if (offset_ms > 500)
        offset_ms = 500;
    if (offset_ms < -500)
        offset_ms = -500;
    sync->offset_ms = offset_ms;
}

void ph_sync_note_video_at(ph_sync *sync, uint64_t pts_ns, uint64_t now_ms)
{
    if (!sync)
        return;
    if (pts_ns == 0 || jumped_back(sync->have_video, sync->video_pts, pts_ns)) {
        ph_sync_reset(sync);
        if (pts_ns == 0)
            return;
    }
    sync->have_video = true;
    sync->video_pts = pts_ns;
    sync->video_at_ms = now_ms;
}

void ph_sync_note_video(ph_sync *sync, uint64_t pts_ns)
{
    ph_sync_note_video_at(sync, pts_ns, 0);
}

ph_sync_action ph_sync_audio_at(ph_sync *sync, uint64_t audio_pts_ns, uint32_t duration_ms,
                                uint64_t now_ms)
{
    ph_sync_action play = {PH_SYNC_PLAY, 0, 0, false};

    (void)duration_ms;
    if (!sync)
        return play;
    if (audio_pts_ns == 0 || jumped_back(sync->have_audio, sync->audio_pts, audio_pts_ns)) {
        ph_sync_reset(sync);
        if (audio_pts_ns == 0)
            return play;
        sync->have_audio = true;
        sync->audio_pts = audio_pts_ns;
        return play;
    }
    sync->have_audio = true;
    sync->audio_pts = audio_pts_ns;
    if (!sync->have_video)
        return play;
    uint64_t video_now = sync->video_pts;
    if (now_ms > sync->video_at_ms)
        video_now += (now_ms - sync->video_at_ms) * 1000000ULL;
    return decide(audio_pts_ns, video_now, sync->offset_ms);
}

#define AV_WINDOW_MS 5000
#define AV_MAX_VIDEO_DELAY_MS 3000

void ph_av_delay_reset(ph_av_delay *d)
{
    if (d)
        memset(d, 0, sizeof *d);
}

static int64_t capture_ms(uint64_t capture_ns)
{
    return (int64_t)(capture_ns / 1000000ULL);
}

/* A capture clock that runs backwards, or jumps a minute, is a new session. */
static void follow_capture(ph_av_delay *d, uint64_t capture_ns)
{
    if (d->last_capture_ns &&
        (capture_ns + 1000000000ULL < d->last_capture_ns ||
         capture_ns > d->last_capture_ns + 60000000000ULL))
        ph_av_delay_reset(d);
    if (capture_ns > d->last_capture_ns)
        d->last_capture_ns = capture_ns;
}

void ph_av_delay_arrived(ph_av_delay *d, uint64_t capture_ns, uint64_t now_ms)
{
    if (!d || !capture_ns || capture_ns > INT64_MAX)
        return;
    follow_capture(d, capture_ns);
    if (!d->window_since_ms || now_ms - d->window_since_ms >= AV_WINDOW_MS) {
        d->arrival_min[1] = d->arrival_min[0];
        d->have_arrival[1] = d->have_arrival[0];
        d->have_arrival[0] = false;
        d->window_since_ms = now_ms;
    }
    int64_t gap = (int64_t)now_ms - capture_ms(capture_ns);
    if (!d->have_arrival[0] || gap < d->arrival_min[0]) {
        d->arrival_min[0] = gap;
        d->have_arrival[0] = true;
    }
}

static int compare_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

void ph_av_delay_presented(ph_av_delay *d, uint64_t capture_ns, uint64_t now_ms)
{
    if (!d || !capture_ns || capture_ns > INT64_MAX)
        return;
    follow_capture(d, capture_ns);
    d->shown[d->next] = (int64_t)now_ms - capture_ms(capture_ns);
    d->next = (d->next + 1) % PH_AV_DELAY_FRAMES;
    if (d->count < PH_AV_DELAY_FRAMES)
        d->count++;
    int64_t sorted[PH_AV_DELAY_FRAMES];
    memcpy(sorted, d->shown, (size_t)d->count * sizeof sorted[0]);
    qsort(sorted, (size_t)d->count, sizeof sorted[0], compare_i64);
    int64_t median = sorted[d->count / 2];
    if (!d->have_applied || llabs(median - d->applied) > PH_AV_DELAY_STEP_MS) {
        d->applied = median;
        d->have_applied = true;
    }
}

bool ph_av_delay_local_us(const ph_av_delay *d, uint64_t capture_ns, int64_t *local_us)
{
    if (!d || !local_us || !d->have_applied || !capture_ns || capture_ns > INT64_MAX)
        return false;
    *local_us = (int64_t)(capture_ns / 1000ULL) + d->applied * 1000;
    return true;
}

#define AV_AUDIO_WINDOW_MS 3000

void ph_av_delay_audio_arrived(ph_av_delay *d, uint64_t capture_ns, uint64_t now_ms)
{
    if (!d || !capture_ns || capture_ns > INT64_MAX)
        return;
    if (!d->audio_since_ms || now_ms - d->audio_since_ms >= AV_AUDIO_WINDOW_MS) {
        d->audio_max[1] = d->audio_max[0];
        d->have_audio[1] = d->have_audio[0];
        d->have_audio[0] = false;
        d->audio_since_ms = now_ms;
    }
    int64_t gap = (int64_t)now_ms - capture_ms(capture_ns);
    if (!d->have_audio[0] || gap > d->audio_max[0]) {
        d->audio_max[0] = gap;
        d->have_audio[0] = true;
    }
}

bool ph_av_delay_audio_us(const ph_av_delay *d, uint64_t capture_ns, int margin_ms,
                          int64_t *local_us)
{
    if (!d || !local_us || !capture_ns || capture_ns > INT64_MAX)
        return false;
    bool have_audio = d->have_audio[0] || d->have_audio[1];
    if (!d->have_applied && !have_audio)
        return false;
    int64_t own = 0;
    if (have_audio) {
        own = d->have_audio[0] ? d->audio_max[0] : d->audio_max[1];
        if (d->have_audio[1] && d->audio_max[1] > own)
            own = d->audio_max[1];
        own += margin_ms;
    }
    int64_t offset = own;
    if (d->have_applied) {
        offset = d->applied;
        if (have_audio && own > offset)
            offset = own < d->applied + PH_AV_AUDIO_LAG_MAX_MS ? own
                                                               : d->applied + PH_AV_AUDIO_LAG_MAX_MS;
    }
    *local_us = (int64_t)(capture_ns / 1000ULL) + offset * 1000;
    return true;
}

bool ph_av_delay_video_ms(const ph_av_delay *d, int fastest_trip_ms, int *delay_ms)
{
    if (!d || !delay_ms || !d->have_applied || (!d->have_arrival[0] && !d->have_arrival[1]))
        return false;
    int64_t fastest = d->have_arrival[0] ? d->arrival_min[0] : d->arrival_min[1];
    if (d->have_arrival[1] && d->arrival_min[1] < fastest)
        fastest = d->arrival_min[1];
    int64_t delay = d->applied - fastest + fastest_trip_ms;
    if (delay < 0)
        delay = 0;
    if (delay > AV_MAX_VIDEO_DELAY_MS)
        delay = AV_MAX_VIDEO_DELAY_MS;
    *delay_ms = (int)delay;
    return true;
}

ph_sync_action ph_sync_audio(ph_sync *sync, uint64_t audio_pts_ns, uint32_t duration_ms)
{
    return ph_sync_audio_at(sync, audio_pts_ns, duration_ms, 0);
}
