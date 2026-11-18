#include "audio/sync.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

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

ph_sync_action ph_sync_audio(ph_sync *sync, uint64_t audio_pts_ns, uint32_t duration_ms)
{
    return ph_sync_audio_at(sync, audio_pts_ns, duration_ms, 0);
}
