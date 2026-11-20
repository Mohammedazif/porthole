#include "control/paste.h"

bool ph_paste_typeable(const char *text)
{
    const unsigned char *p;
    bool any = false;

    if (!text)
        return false;
    for (p = (const unsigned char *)text; *p != 0; p++) {
        uint8_t usage = 0;
        uint8_t modifiers = 0;

        if (*p == '\r')
            continue;
        if (*p >= 0x80 || !ph_hid_ascii_to_key((char)*p, &usage, &modifiers))
            return false;
        any = true;
    }
    return any;
}

size_t ph_paste_encode(const char *text, ph_paste_event *out, size_t cap)
{
    const unsigned char *p;
    size_t written = 0;

    if (!text || !out || cap == 0)
        return 0;
    for (p = (const unsigned char *)text; *p != 0; p++) {
        uint8_t usage = 0;
        uint8_t modifiers = 0;

        if (*p >= 0x80 || !ph_hid_ascii_to_key((char)*p, &usage, &modifiers))
            continue;
        if (written == cap)
            break;
        out[written].usage = usage;
        out[written].modifiers = modifiers;
        written++;
    }
    return written;
}
