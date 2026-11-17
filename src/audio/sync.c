#include "audio/sync.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

struct ph_sync {
    bool have_video;
    bool have_audio;
    uint64_t video_pts;
    uint64_t audio_pts;
    int offset_ms;
};

static bool jumped_back(bool have, uint64_t previous, uint64_t pts)
{
    return have && previous > pts && previous - pts > 500000000ULL;
}

/* Positive offset_ms delays audio (subtracted from audio PTS minus video PTS). */
static ph_sync_action decide(uint64_t audio_pts, uint64_t video_pts, int offset_ms)
{
    ph_sync_action action = {PH_SYNC_PLAY, 0};
    int64_t delta;
    const int64_t ms = 1000000LL;

    if (audio_pts >= video_pts) {
        uint64_t ahead = audio_pts - video_pts;
        delta = ahead > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)ahead;
    } else {
        uint64_t behind = video_pts - audio_pts;
        delta = behind > (uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)behind;
    }

    {
        int64_t shift = (int64_t)offset_ms * ms;
        if (shift > 0) {
            if (delta < INT64_MIN + shift)
                delta = INT64_MIN;
            else
                delta -= shift;
        } else if (shift < 0) {
            int64_t add = -shift;
            if (delta > INT64_MAX - add)
                delta = INT64_MAX;
            else
                delta += add;
        }
    }

    if (delta > 40 * ms) {
        int64_t reported = delta / ms;
        action.kind = PH_SYNC_DROP;
        action.hold_or_drop_ms = reported > INT_MAX ? INT_MAX : (int)reported;
        return action;
    }
    if (delta < -(20 * ms)) {
        int64_t deficit = -delta;
        if (deficit > 200 * ms)
            deficit = 200 * ms;
        action.kind = PH_SYNC_HOLD;
        action.hold_or_drop_ms = (int)(deficit / ms);
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

void ph_sync_note_video(ph_sync *sync, uint64_t pts_ns)
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
}

ph_sync_action ph_sync_audio(ph_sync *sync, uint64_t audio_pts_ns, uint32_t duration_ms)
{
    ph_sync_action play = {PH_SYNC_PLAY, 0};

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
    return decide(audio_pts_ns, sync->video_pts, sync->offset_ms);
}
