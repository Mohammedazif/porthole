#ifndef PORTHOLE_USB_WINUSB_H
#define PORTHOLE_USB_WINUSB_H

/* Picks the QuickTime bulk interface and writes a WinUSB INF for that
 * interface only. No Win32 calls, so this compiles on Linux. */

#include <stdbool.h>
#include <stddef.h>

typedef struct ph_usb_iface {
    int number;
    int cls;
    int subclass;
    int protocol;
    int in_ep;
    int out_ep;
    bool bulk_in;
} ph_usb_iface;

/* Interface number of the vendor-class bulk IN interface to claim, or -1.
 * Skips interface 0 and a bulk IN endpoint address of 0xFD. The highest
 * matching interface number wins. */
int ph_usb_select_capture(const ph_usb_iface *list, int count);

/* INF text binding WinUSB to USB\VID_xxxx&PID_xxxx&MI_xx. Returns 0, or -1
 * if the buffer is too small or the arguments are out of range. */
int ph_usb_winusb_inf(char *out, size_t out_len, int vid, int pid, int iface);

#endif
