#ifndef PORTHOLE_AUDIO_PLAYER_H
#define PORTHOLE_AUDIO_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ph_audio ph_audio;

/* AirPlay audio -> FFmpeg -> bounded SDL playback queue. Callbacks may arrive
 * on different network threads; the player serializes decoder/device access. */
ph_audio *ph_audio_create(void);
void ph_audio_set_airplay_format(ph_audio *audio, unsigned char compression, unsigned short samples);
void ph_audio_set_airplay_volume(ph_audio *audio, float decibels);
void ph_audio_flush_airplay(ph_audio *audio);
bool ph_audio_configure(ph_audio *audio, unsigned char compression, unsigned short samples);
/* Compression: 2 ALAC, 4 AAC-LC, 8 AAC-ELD. Returns decoded sample frames, or -1
 * on error. The input is copied and is only needed for the duration of this call. */
int ph_audio_push(ph_audio *audio, unsigned char compression, const uint8_t *data, size_t len);
/* 44.1 kHz stereo signed 16-bit little-endian ReplayKit application audio. */
int ph_audio_push_broadcast(ph_audio *audio, const uint8_t *pcm, size_t len);
int ph_audio_push_broadcast_timed(ph_audio *audio, const uint8_t *pcm, size_t len, uint64_t pts_ns);
void ph_audio_note_broadcast_video(ph_audio *audio, uint64_t pts_ns);
void ph_audio_set_volume(ph_audio *audio, float decibels);
void ph_audio_flush(ph_audio *audio);
uint32_t ph_audio_queued_ms(ph_audio *audio);
void ph_audio_destroy(ph_audio *audio);

/* queued_ms counts device audio plus PCM waiting out a sync hold. */
struct ph_audio_settings {
    bool muted;
    float volume_db;
    int sync_offset_ms;
    uint32_t queued_ms;
};

/* Video ntp_time_remote, in nanoseconds, for ph_audio_push_timed. */
void ph_audio_note_video_pts(ph_audio *audio, uint64_t pts_ns);
/* ph_audio_push with ntp_time_remote. Dropped packets return 0. */
int ph_audio_push_timed(ph_audio *audio, unsigned char compression, const uint8_t *data,
                        size_t len, uint64_t pts_ns);
/* now_ms is monotonic milliseconds; hold deadlines use the previous call. */
void ph_audio_pump(ph_audio *audio, uint64_t now_ms);
bool ph_audio_settings(const ph_audio *audio, struct ph_audio_settings *out);
/* Muted playback still decodes and tracks sync, but does not queue to the device. */
void ph_audio_set_muted(ph_audio *audio, bool muted);
/* Adds delta decibels and clamps to [-30, 0]. */
void ph_audio_adjust_volume_db(ph_audio *audio, float delta);
/* Positive offset delays audio. Clamped to [-500, 500] and kept across flush. */
void ph_audio_set_sync_offset_ms(ph_audio *audio, int offset_ms);

#endif
