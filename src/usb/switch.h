#ifndef PORTHOLE_USB_SWITCH_H
#define PORTHOLE_USB_SWITCH_H

/* Which mirror source is visible. Prefer USB, then the companion's broadcast,
 * then AirPlay. A newly active high-priority source starts on a keyframe. */

#include <stdbool.h>
#include <stdint.h>

#define PH_SRC_USB_FALLBACK_MS 500

typedef enum ph_src {
    PH_SRC_NONE = 0,
    PH_SRC_WIFI,
    PH_SRC_BROADCAST,
    PH_SRC_USB,
} ph_src;

typedef struct ph_src_switch {
    ph_src current;
    bool usb_unplugged;
    bool have_wifi;
    bool have_broadcast;
    bool broadcast_needs_key;
    uint64_t last_wifi_ms;
    uint64_t last_broadcast_ms;
} ph_src_switch;

void ph_src_switch_init(ph_src_switch *s);

ph_src ph_src_switch_wifi_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms);
bool ph_src_switch_broadcast_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms);
ph_src ph_src_switch_broadcast_lost(ph_src_switch *s, uint64_t now_ms);
ph_src ph_src_switch_usb_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms);
ph_src ph_src_switch_usb_unplugged(ph_src_switch *s, uint64_t now_ms);
ph_src ph_src_switch_wifi_lost(ph_src_switch *s, uint64_t now_ms);

#endif
