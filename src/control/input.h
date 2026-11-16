#ifndef PORTHOLE_CONTROL_INPUT_H
#define PORTHOLE_CONTROL_INPUT_H

#include "control/keyboard.h"

typedef struct ph_input_output {
    void *user;
    void (*keyboard)(void *user, const ph_hid_keyboard *report);
    void (*mouse)(void *user, const ph_hid_mouse *report);
    void (*consumer)(void *user, uint16_t usage);
} ph_input_output;

typedef struct ph_input {
    ph_input_output output;
    ph_hid_keyboard keys;
    bool down[256];
    bool local[256];
    uint8_t buttons;
    bool enabled;
} ph_input;

/* Disabled input is discarded. Re-enabling starts with released reports, so keys held
 * during a lost connection cannot be replayed when the phone reconnects. */
void ph_input_set_enabled(ph_input *input, bool enabled);

/* Keys are USB usages; modifiers are the current PC HID modifier bits. Left Alt is
 * reserved for local shortcuts. Button shortcuts stay held until their key is released. */
void ph_input_key(ph_input *input, uint8_t usage, bool down, bool repeat, uint8_t modifiers);
void ph_input_release_all(ph_input *input);

#endif
