#ifndef PORTHOLE_CONTROL_PASTE_H
#define PORTHOLE_CONTROL_PASTE_H

/* Clipboard text as US QWERTY key-downs. The caller sends each key release.
 * Shift is the only modifier. No Ctrl/Cmd swap. ph_paste_clipboard is in the SDL app. */

#include <stddef.h>
#include <stdint.h>

#include "hid/hid_reports.h"

typedef struct ph_paste_event {
    uint8_t usage;
    uint8_t modifiers; /* PH_HID_MOD_* */
} ph_paste_event;

size_t ph_paste_encode(const char *text, ph_paste_event *out, size_t cap);

/* True when the text is non-empty US-QWERTY. CR is ignored so a Windows newline still counts. */
bool ph_paste_typeable(const char *text);

int ph_paste_clipboard(char *buf, size_t len);

#endif
