#ifndef PORTHOLE_USB_CAPTURE_H
#define PORTHOLE_USB_CAPTURE_H

/* USB screen session over an injected transport. PING and SYNC are answered
 * here. FEED/EAT sdat bytes are copied out for the decoder. */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct ph_usb_transport {
    int (*read)(void *user, uint8_t *dst, int cap, int timeout_ms);
    int (*write)(void *user, const uint8_t *src, int len, int timeout_ms);
    void *user;
} ph_usb_transport;

typedef struct ph_usb_capture ph_usb_capture;

typedef struct ph_usb_sample {
    bool video;
    const uint8_t *data;
    size_t len;
} ph_usb_sample;

ph_usb_capture *ph_usb_capture_open(const ph_usb_transport *io);

/* 1 when `out` holds a sample copied into `copy_buf`. 0 on timeout or when
 * more bytes are needed. -1 on a hard error. `out->data` aliases `copy_buf`. */
int ph_usb_capture_poll(ph_usb_capture *cap, ph_usb_sample *out, uint8_t *copy_buf,
                        size_t copy_cap);

void ph_usb_capture_close(ph_usb_capture *cap);

#ifdef PH_USB_HAVE_LIBUSB
#include <stdatomic.h>

/* Cancel between device probes and bounded USB operations. The stop flag
 * must outlive the capture so reads and writes can observe shutdown too. */
ph_usb_capture *ph_usb_libusb_open(const atomic_bool *stop);

/* WinUSB INF for the capture interface of a connected Apple device.
 * Does not claim the interface. Returns 0 when the file is written. */
int ph_usb_write_winusb_inf(const char *path);
#endif

#endif
