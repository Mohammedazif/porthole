#ifndef PORTHOLE_USB_SAMPLE_H
#define PORTHOLE_USB_SAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Annex-B is copied through. A buffer of 4-byte big-endian NAL lengths is
 * rewritten with 00 00 00 01. Returns 0 when `src` is neither shape or `dst`
 * is too small. */
size_t ph_usb_to_annexb(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

/* True for an H.264 IDR or SPS, or an H.265 IDR or VPS, in Annex-B. */
bool ph_usb_annexb_keyframe(const uint8_t *data, size_t len);

#endif
