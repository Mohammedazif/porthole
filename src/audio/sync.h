#ifndef PORTHOLE_AUDIO_SYNC_H
#define PORTHOLE_AUDIO_SYNC_H

#include <stdint.h>

typedef struct ph_sync ph_sync;

typedef enum ph_sync_kind {
    PH_SYNC_PLAY,
    PH_SYNC_HOLD,
    PH_SYNC_DROP
} ph_sync_kind;

typedef struct ph_sync_action {
    ph_sync_kind kind;
    int hold_or_drop_ms;
} ph_sync_action;

ph_sync *ph_sync_create(void);
void ph_sync_destroy(ph_sync *sync);
/* Clears the PTS mapping. The sync offset is kept. */
void ph_sync_reset(ph_sync *sync);
/* Positive offset delays audio. Clamped to [-500, 500] and kept across reset. */
void ph_sync_set_offset_ms(ph_sync *sync, int offset_ms);
void ph_sync_note_video(ph_sync *sync, uint64_t pts_ns);
/* Drop audio >40 ms ahead; hold audio >20 ms behind, at most 200 ms. duration_ms may be 0. */
ph_sync_action ph_sync_audio(ph_sync *sync, uint64_t audio_pts_ns, uint32_t duration_ms);

#endif
