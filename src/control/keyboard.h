#ifndef PORTHOLE_CONTROL_KEYBOARD_H
#define PORTHOLE_CONTROL_KEYBOARD_H

/* Turns PC key presses into the virtual keyboard's HID report. Keys are USB HID usages
 * (page 7); SDL scancodes use the same numbers. */

#include <stdbool.h>
#include <stdint.h>

#include "hid/hid_reports.h"

/* With `swap_ctrl_cmd`, PC Ctrl becomes the iPhone's Command key and the Windows/Super
 * key becomes Control, so PC habits (Ctrl+C, Ctrl+V, Ctrl+A, Ctrl+Z) do the expected thing.
 * Returns true if the report changed and should be sent. Unknown or excess keys (more than
 * six at once) are ignored. */
bool ph_keyboard_apply(ph_hid_keyboard *k, uint8_t usage, bool down, bool swap_ctrl_cmd);

/* Sync from the current PC modifier snapshot (HID modifier bits). This also handles a
 * modifier pressed before the mirror window gained focus. */
bool ph_keyboard_set_modifiers(ph_hid_keyboard *k, uint8_t modifiers, bool swap_ctrl_cmd);

/* Releases everything; returns true if anything was pressed. */
bool ph_keyboard_release_all(ph_hid_keyboard *k);

#endif
