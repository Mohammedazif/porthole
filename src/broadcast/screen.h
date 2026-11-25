#ifndef PORTHOLE_BROADCAST_SCREEN_H
#define PORTHOLE_BROADCAST_SCREEN_H

/* The iPhone's own screen, sent by the Porthole app's screen-broadcast extension
 * (ReplayKit) over Wi-Fi. Unlike AirPlay mirroring, it is always what the phone
 * shows: Photos keeps its bars, and share sheets and alerts appear.
 *
 * Wire format, after the client's "PSCR 1\n" line: frames of a 4-byte big-endian
 * payload length, a 1-byte type and the payload. Type 1 is H.264 Annex-B video,
 * type 2 the same for a keyframe (SPS and PPS first). The PC answers each frame it has
 * taken with one byte, 'A'. With PSCR 2, type 3 carries 44.1 kHz stereo PCM16LE.
 * PSCR 3 puts an 8-byte big-endian capture time in nanoseconds after the type.
 * The length still describes only the media payload. A zero length with type 4
 * explicitly ends PSCR 3, with no timestamp. The PC replies with 'E'.
 * PSCR 4 adds type 5, sent before the first frame and whenever it changes:
 * its first byte is the EXIF orientation (1 up, 3 down, 6 right, 8 left) that
 * turns the phone's screen as mounted upright. The phone places an absolute
 * pointer on its screen as mounted, so the PC undoes that turn for pointer
 * positions. Up to 15 more bytes describe the capture for the log. */
#define PH_SCREEN_TURN_MAX 16u

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PH_SCREEN_PORT 47732
#define PH_SCREEN_FRAME_MAX (8u * 1024u * 1024u)
#define PH_SCREEN_AUDIO_MAX 8192u

typedef struct ph_screen ph_screen;

typedef struct ph_screen_callbacks {
    void *user;
    /* Whether a phone at this IPv4 address may send its screen. */
    bool (*allow)(void *user, const char *address);
    bool (*authorize)(void *user, const char *ticket);
    void (*connected)(void *user, const char *address);
    void (*video)(void *user, const uint8_t *annexb, size_t len, bool keyframe, uint64_t pts_ns);
    void (*audio)(void *user, const uint8_t *pcm, size_t len, uint64_t pts_ns);
    void (*orientation)(void *user, const uint8_t *turn, size_t len);
    void (*ended)(void *user);
    void (*disconnected)(void *user);
} ph_screen_callbacks;

/* Listens for a paired iPhone broadcast. NULL if the port cannot be opened. */
ph_screen *ph_screen_start(const ph_screen_callbacks *callbacks);
void ph_screen_disconnect(ph_screen *s);
void ph_screen_stop(ph_screen *screen);

/* Parses one frame header. Returns the payload length, or 0 for a bad header. */
uint32_t ph_screen_parse_header(const uint8_t header[5], uint32_t max_len, int *type);

#endif
