#ifndef PORTHOLE_HID_LINK_PROTO_H
#define PORTHOLE_HID_LINK_PROTO_H

/* Wire format between the app and a Bluetooth helper (stdin of the helper process, or a
 * serial port for the ESP32 dongle): [type: 1 byte][length: 1 byte][payload].
 * The helper answers with text lines: ready, advertising, connected, disconnected,
 * subscribed, sync <id>, error <message>. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    PH_LINK_KEYBOARD = 1, /* 8-byte keyboard report */
    PH_LINK_MOUSE = 2,    /* 7-byte mouse report */
    PH_LINK_CONSUMER = 3, /* 2-byte consumer report */
    /* 6-byte absolute pointer report [buttons][x 16 LE][y 16 LE][wheel], x/y 0..32767.
     * Only for a helper started with the absolute report map. */
    PH_LINK_ABS_MOUSE = 4,
    /* 4-byte id: the helper prints "sync <id>" once the mouse reports before it are sent */
    PH_LINK_SYNC = 16,
    PH_LINK_INTERVAL = 17, /* 2-byte milliseconds between reports */
    PH_LINK_DELAY = 18     /* 2-byte pause in milliseconds, after all preceding reports */
};

#define PH_LINK_MAX_FRAME (2 + 8)

/* Writes one frame into `out` (at least PH_LINK_MAX_FRAME bytes); returns its size, or 0 if
 * the payload is too long. */
static inline size_t ph_link_frame(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len > PH_LINK_MAX_FRAME - 2)
        return 0;
    out[0] = type;
    out[1] = (uint8_t)len;
    memcpy(out + 2, payload, len);
    return len + 2;
}

#endif
