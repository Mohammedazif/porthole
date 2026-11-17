#ifndef PORTHOLE_USB_PACKET_H
#define PORTHOLE_USB_PACKET_H

/* QuickTime USB screen-capture frames. Length is a little-endian uint32 that
 * includes itself. The next uint32 is a FourCC stored little-endian, so the
 * bytes on the wire are the characters reversed (PING is 67 6e 69 70). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PH_USB_FRAME_MAX (8u * 1024u * 1024u)

#define PH_USB_APPLE_VID 0x05AC
#define PH_USB_VENDOR_CLASS 0xFF
#define PH_USB_ENABLE_REQUEST_TYPE 0x40
#define PH_USB_ENABLE_REQUEST 0x52 /* enables the hidden QuickTime configuration */
#define PH_USB_ENABLE_VALUE 0
#define PH_USB_ENABLE_INDEX 2
#define PH_USB_NOT_SCREEN_EP 0xFD

#define PH_USB_FCC(a, b, c, d)                                                                     \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) |                             \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

#define PH_USB_PING PH_USB_FCC('p', 'i', 'n', 'g')
#define PH_USB_SYNC PH_USB_FCC('s', 'y', 'n', 'c')
#define PH_USB_ASYN PH_USB_FCC('a', 's', 'y', 'n')
#define PH_USB_RPLY PH_USB_FCC('r', 'p', 'l', 'y')

#define PH_USB_CWPA PH_USB_FCC('c', 'w', 'p', 'a')
#define PH_USB_AFMT PH_USB_FCC('a', 'f', 'm', 't')
#define PH_USB_CVRP PH_USB_FCC('c', 'v', 'r', 'p')
#define PH_USB_CLOK PH_USB_FCC('c', 'l', 'o', 'k')
#define PH_USB_TIME PH_USB_FCC('t', 'i', 'm', 'e')
#define PH_USB_SKEW PH_USB_FCC('s', 'k', 'e', 'w')
#define PH_USB_STOP PH_USB_FCC('s', 't', 'o', 'p')
#define PH_USB_OG PH_USB_FCC('g', 'o', '!', ' ')

#define PH_USB_FEED PH_USB_FCC('f', 'e', 'e', 'd')
#define PH_USB_EAT PH_USB_FCC('e', 'a', 't', '!')
#define PH_USB_NEED PH_USB_FCC('n', 'e', 'e', 'd')
#define PH_USB_HPD1 PH_USB_FCC('h', 'p', 'd', '1')
#define PH_USB_HPA1 PH_USB_FCC('h', 'p', 'a', '1')
#define PH_USB_SPRP PH_USB_FCC('s', 'p', 'r', 'p')
#define PH_USB_SBUF PH_USB_FCC('s', 'b', 'u', 'f')
#define PH_USB_SDAT PH_USB_FCC('s', 'd', 'a', 't')

typedef enum ph_usb_frame_kind {
    PH_USB_FRAME_INVALID = 0,
    PH_USB_FRAME_PING,
    PH_USB_FRAME_SYNC,
    PH_USB_FRAME_ASYN,
    PH_USB_FRAME_REPLY,
} ph_usb_frame_kind;

typedef struct ph_usb_frame {
    ph_usb_frame_kind kind;
    uint32_t subtype;
    uint64_t clock_ref;
    uint64_t correlation;
    uint64_t device_clock;
    const uint8_t *sdat;
    size_t sdat_len;
    bool reply_required;
    bool has_device_clock;
} ph_usb_frame;

/* `data` is one complete frame, including the length prefix. Returns false when
 * `len` is not that declared length or the length is below 4 or above 8 MiB.
 * `sdat` points into `data` and is null when the frame has no sdat chunk. */
bool ph_usb_frame_parse(const uint8_t *data, size_t len, ph_usb_frame *out);

/* Builders return the number of bytes written, or 0 if `dst` is too small
 * or `subtype` is not a SYNC this builder answers. */
size_t ph_usb_build_ping(uint8_t *dst, size_t cap);
size_t ph_usb_build_need(uint8_t *dst, size_t cap, uint64_t clock_ref);

/* CWPA/CVRP/CLOK put `clock_ref` in the RPLY. TIME uses it as the CMTime value
 * in nanoseconds. AFMT ignores it and answers with an Error=0 dictionary.
 * SKEW answers 48000.0. STOP and OG answer zeros. `correlation` is copied
 * from the request. */
size_t ph_usb_build_sync_reply(uint8_t *dst, size_t cap, uint32_t subtype,
                               uint64_t correlation, uint64_t clock_ref);

typedef struct ph_usb_reasm {
    uint8_t *buf;
    size_t len;
    size_t start;
    size_t cap;
    bool failed;
} ph_usb_reasm;

void ph_usb_reasm_init(ph_usb_reasm *r);
void ph_usb_reasm_free(ph_usb_reasm *r);

/* 0 on success, -1 if a declared length is < 4 or > 8 MiB. */
int ph_usb_reasm_push(ph_usb_reasm *r, const uint8_t *data, size_t n);

/* 1 and sets *frame to a buffer valid until the next push or pop.
 * 0 if a frame is not complete yet. -1 if the length prefix is rejected. */
int ph_usb_reasm_pop(ph_usb_reasm *r, const uint8_t **frame, size_t *frame_len);

#endif
