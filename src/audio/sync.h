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

#endif
