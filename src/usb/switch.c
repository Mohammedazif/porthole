#include "usb/switch.h"

#include <string.h>

void ph_src_switch_init(ph_src_switch *s)
{
    if (s)
        memset(s, 0, sizeof *s);
}

static bool recent(uint64_t now, uint64_t last_seen)
{
    return now >= last_seen && now - last_seen <= PH_SRC_USB_FALLBACK_MS;
}

static ph_src network_fallback(const ph_src_switch *s, uint64_t now_ms)
{
    if (s->have_broadcast && recent(now_ms, s->last_broadcast_ms))
        return PH_SRC_BROADCAST;
    if (s->have_wifi && recent(now_ms, s->last_wifi_ms))
        return PH_SRC_WIFI;
    return PH_SRC_NONE;
}

ph_src ph_src_switch_wifi_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms)
{
    (void)is_keyframe;
    s->have_wifi = true;
    s->last_wifi_ms = now_ms;
    if (s->current < PH_SRC_BROADCAST)
        s->current = PH_SRC_WIFI;
    return s->current;
}

bool ph_src_switch_broadcast_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms)
{
    s->last_broadcast_ms = now_ms;
    if (is_keyframe)
        s->have_broadcast = true;
    if (s->current == PH_SRC_USB)
        return false;
    if (is_keyframe) {
        s->broadcast_needs_key = false;
        s->current = PH_SRC_BROADCAST;
    }
    return s->current == PH_SRC_BROADCAST && !s->broadcast_needs_key;
}

ph_src ph_src_switch_broadcast_lost(ph_src_switch *s, uint64_t now_ms)
{
    s->have_broadcast = false;
    s->broadcast_needs_key = true;
    if (s->current == PH_SRC_BROADCAST)
        s->current = network_fallback(s, now_ms);
    return s->current;
}

ph_src ph_src_switch_usb_frame(ph_src_switch *s, bool is_keyframe, uint64_t now_ms)
{
    (void)now_ms;
    if (s->usb_unplugged)
        return s->current;
    if (is_keyframe) {
        s->broadcast_needs_key = true;
        s->current = PH_SRC_USB;
    }
    return s->current;
}

ph_src ph_src_switch_usb_unplugged(ph_src_switch *s, uint64_t now_ms)
{
    s->usb_unplugged = true;
    if (s->current != PH_SRC_USB)
        return s->current;
    s->current = network_fallback(s, now_ms);
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
