#ifndef PORTHOLE_AUDIO_SYNC_H
#define PORTHOLE_AUDIO_SYNC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ph_sync ph_sync;

typedef enum ph_sync_kind {
    PH_SYNC_PLAY,
    PH_SYNC_HOLD,
    PH_SYNC_DROP /* reserved; the current policy never drops for sync */
} ph_sync_kind;

typedef struct ph_sync_action {
    ph_sync_kind kind;
    int hold_or_drop_ms;
    /* Audio PTS minus the video presentation time (plus the user offset), in ms.
     * Positive: audio arrived ahead of its picture. Valid when have_lead. */
    int lead_ms;
    bool have_lead;
} ph_sync_action;

ph_sync *ph_sync_create(void);
void ph_sync_destroy(ph_sync *sync);
/* Clears the PTS mapping. The sync offset is kept. */
void ph_sync_reset(ph_sync *sync);
/* Positive offset delays audio. Clamped to [-500, 500] and kept across reset. */
void ph_sync_set_offset_ms(ph_sync *sync, int offset_ms);

/* Both PTS values are the phone's presentation timeline (ntp_time_remote). Video
 * is drawn as it arrives, so the picture "now" is the latest video PTS plus the
 * local time since it arrived; AirPlay sends no frames while the screen is still.
 *
 * Audio ahead of that picture by more than 40 ms is held for its lead, at most
 * 200 ms. Audio level with or behind the picture plays immediately: delaying it
 * would only make it later. Leads beyond 1 s are a broken mapping and play. */
void ph_sync_note_video_at(ph_sync *sync, uint64_t pts_ns, uint64_t now_ms);
ph_sync_action ph_sync_audio_at(ph_sync *sync, uint64_t audio_pts_ns, uint32_t duration_ms,
                                uint64_t now_ms);
/* The same with no elapsed time between the latest video and this audio. */
void ph_sync_note_video(ph_sync *sync, uint64_t pts_ns);
ph_sync_action ph_sync_audio(ph_sync *sync, uint64_t audio_pts_ns, uint32_t duration_ms);

/* Screen-broadcast timing. Frames carry the iPhone's capture time (its host
 * clock). The PC records when each one arrives and when it is drawn, so sound
 * can be given the same delay as the picture:
 *
 *  - ReplayKit audio has capture times on the same iPhone clock. It plays at
 *    its capture time plus the typical gap between a frame's capture time and
 *    its presentation, on the PC clock. The clock offset cancels out.
 *  - AirPlay audio is due on the iPhone's AirPlay clock, when the iPhone shows
 *    the matching picture. It needs the capture-to-display time itself. That
 *    is the presentation gap less the fastest arrival gap (the clock offset
 *    plus the quickest encode and Wi-Fi trip), plus that quickest trip.
 *
 * The presentation gap is a median of recent frames. The value used moves only
 * when the median moves by more than PH_AV_DELAY_STEP_MS, so ordinary frame
 * jitter does not cut or stretch the sound.
 *
 * ReplayKit audio can arrive about as late as the picture is drawn. Matching
 * the picture exactly would leave no time to play it, so it also waits at
 * least its own recent worst arrival gap plus a margin. That allowance is
 * capped at PH_AV_AUDIO_LAG_MAX_MS behind the picture; later sound is skipped. */
#define PH_AV_DELAY_FRAMES 31
#define PH_AV_DELAY_STEP_MS 25
#define PH_AV_AUDIO_LAG_MAX_MS 200

typedef struct ph_av_delay {
    int64_t shown[PH_AV_DELAY_FRAMES]; /* presented PC ms minus capture ms */
    int count, next;
    int64_t arrival_min[2]; /* fastest arrival PC ms minus capture ms, two windows */
    bool have_arrival[2];
    uint64_t window_since_ms;
    int64_t applied;
    bool have_applied;
    uint64_t last_capture_ns;
    int64_t audio_max[2]; /* slowest audio arrival PC ms minus capture ms, two windows */
    bool have_audio[2];
    uint64_t audio_since_ms;
} ph_av_delay;

void ph_av_delay_reset(ph_av_delay *d);
void ph_av_delay_arrived(ph_av_delay *d, uint64_t capture_ns, uint64_t now_ms);
void ph_av_delay_presented(ph_av_delay *d, uint64_t capture_ns, uint64_t now_ms);
/* PC microseconds at which something captured at capture_ns should be heard.
 * False until a frame has been presented. */
bool ph_av_delay_local_us(const ph_av_delay *d, uint64_t capture_ns, int64_t *local_us);
/* ReplayKit audio captured at capture_ns arrived now. */
void ph_av_delay_audio_arrived(ph_av_delay *d, uint64_t capture_ns, uint64_t now_ms);
/* When that audio should be heard: with its picture, or after its own slowest
 * recent arrival plus margin_ms, at most PH_AV_AUDIO_LAG_MAX_MS after the
 * picture. False before any frame was presented or audio arrived. */
bool ph_av_delay_audio_us(const ph_av_delay *d, uint64_t capture_ns, int margin_ms,
                          int64_t *local_us);
/* Capture-to-display time of the shared screen, assuming the fastest frame
 * took fastest_trip_ms to arrive. False until frames have arrived and shown. */
bool ph_av_delay_video_ms(const ph_av_delay *d, int fastest_trip_ms, int *delay_ms);

#endif
