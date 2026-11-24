#ifndef PORTHOLE_BROADCAST_SCREEN_H
#define PORTHOLE_BROADCAST_SCREEN_H

/* The iPhone's own screen, sent by the Porthole app's screen-broadcast extension
 * (ReplayKit) over Wi-Fi. Unlike AirPlay mirroring, it is always what the phone
 * shows: Photos keeps its bars, and share sheets and alerts appear.
 *
 * Wire format, after the client's "PSCR 1\n" line: frames of a 4-byte big-endian
 * payload length, a 1-byte type and the payload. Type 1 is H.264 Annex-B video,
 * type 2 the same for a keyframe (SPS and PPS first). The PC answers each frame it has
 * taken with one byte, 'A'. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PH_SCREEN_PORT 47732
#define PH_SCREEN_FRAME_MAX (8u * 1024u * 1024u)

typedef struct ph_screen ph_screen;

typedef struct ph_screen_callbacks {
    void *user;
    /* Whether a phone at this IPv4 address may send its screen. */
    bool (*allow)(void *user, const char *address);
    bool (*authorize)(void *user, const char *ticket);
    void (*connected)(void *user, const char *address);
    void (*video)(void *user, const uint8_t *annexb, size_t len, bool keyframe);
    void (*disconnected)(void *user);
} ph_screen_callbacks;

/* Listens for a paired iPhone broadcast. NULL if the port cannot be opened. */
ph_screen *ph_screen_start(const ph_screen_callbacks *callbacks);
void ph_screen_disconnect(ph_screen *s);
void ph_screen_stop(ph_screen *screen);

/* Parses one frame header. Returns the payload length, or 0 for a bad header. */
uint32_t ph_screen_parse_header(const uint8_t header[5], uint32_t max_len, int *type);

#endif
