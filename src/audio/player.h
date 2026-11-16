#ifndef PORTHOLE_AUDIO_PLAYER_H
#define PORTHOLE_AUDIO_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ph_audio ph_audio;

/* AirPlay audio -> FFmpeg -> bounded SDL playback queue. Callbacks may arrive
 * on different network threads; the player serializes decoder/device access. */
ph_audio *ph_audio_create(void);
bool ph_audio_configure(ph_audio *audio, unsigned char compression, unsigned short samples);
/* Compression: 2 ALAC, 4 AAC-LC, 8 AAC-ELD. Returns decoded sample frames, or -1
 * on error. The input is copied and is only needed for the duration of this call. */
int ph_audio_push(ph_audio *audio, unsigned char compression, const uint8_t *data, size_t len);
void ph_audio_set_volume(ph_audio *audio, float decibels);
void ph_audio_flush(ph_audio *audio);
uint32_t ph_audio_queued_ms(ph_audio *audio);
void ph_audio_destroy(ph_audio *audio);

#endif
