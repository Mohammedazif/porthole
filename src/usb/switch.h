#ifndef PORTHOLE_USB_SWITCH_H
#define PORTHOLE_USB_SWITCH_H

/* Which mirror source is visible. Wi-Fi is used until a USB keyframe arrives.
 * After that, Wi-Fi video is ignored until USB is unplugged. */

#include <stdbool.h>
#include <stdint.h>

#define PH_SRC_USB_FALLBACK_MS 500

typedef enum ph_src {
    PH_SRC_NONE = 0,
    PH_SRC_WIFI,
    PH_SRC_USB,
} ph_src;

typedef struct ph_src_switch {
    ph_src current;
    bool usb_unplugged;
    bool have_wifi;
    uint64_t last_wifi_ms;
} ph_src_switch;

void ph_src_switch_init(ph_src_switch *s);

ph_src ph_src_switch_wifi_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms);
ph_src ph_src_switch_usb_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms);
ph_src ph_src_switch_usb_unplugged(ph_src_switch *s, uint64_t now_ms);
ph_src ph_src_switch_wifi_lost(ph_src_switch *s, uint64_t now_ms);

#endif
