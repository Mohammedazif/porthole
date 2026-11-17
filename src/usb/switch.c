#include "usb/switch.h"

#include <string.h>

void ph_src_switch_init(ph_src_switch *s)
{
    if (s)
        memset(s, 0, sizeof *s);
}

ph_src ph_src_switch_wifi_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms)
{
    (void)is_keyframe;
    s->have_wifi = true;
    s->last_wifi_ms = now_ms;
    if (s->current != PH_SRC_USB)
        s->current = PH_SRC_WIFI;
    return s->current;
}

ph_src ph_src_switch_usb_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms)
{
    (void)now_ms;
    if (s->usb_unplugged)
        return s->current;
    if (is_keyframe)
        s->current = PH_SRC_USB;
    return s->current;
}

ph_src ph_src_switch_usb_unplugged(ph_src_switch *s, uint64_t now_ms)
{
    s->usb_unplugged = true;
    if (s->current != PH_SRC_USB)
        return s->current;
    if (s->have_wifi && now_ms >= s->last_wifi_ms &&
        now_ms - s->last_wifi_ms <= (uint64_t)PH_SRC_USB_FALLBACK_MS)
        s->current = PH_SRC_WIFI;
    else
        s->current = PH_SRC_NONE;
    return s->current;
}

ph_src ph_src_switch_wifi_lost(ph_src_switch *s, uint64_t now_ms)
{
    (void)now_ms;
    s->have_wifi = false;
    if (s->current == PH_SRC_WIFI)
        s->current = PH_SRC_NONE;
    return s->current;
}
