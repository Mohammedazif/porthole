#ifndef PORTHOLE_CONTROL_INPUT_H
#define PORTHOLE_CONTROL_INPUT_H

#include "control/keyboard.h"

#define PH_INPUT_CONSUMER_KEYS 8

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
    uint16_t consumer_keys[PH_INPUT_CONSUMER_KEYS];
    uint8_t consumer_count;
} ph_input;

/* Disabled input is discarded. Re-enabling starts with released reports, so keys held
 * during a lost connection cannot be replayed when the phone reconnects. */
void ph_input_set_enabled(ph_input *input, bool enabled);

/* Keys are USB usages; modifiers are the current PC HID modifier bits. Left Alt is
 * reserved for local shortcuts. Button shortcuts stay held until their key is released. */
void ph_input_key(ph_input *input, uint8_t usage, bool down, bool repeat, uint8_t modifiers);
/* The single-usage consumer report follows the newest held supported key.
 * Releasing it restores the previous held key. Repeats/duplicates, unsupported
 * actions (including Power), and disabled input are ignored. */
void ph_input_consumer_key(ph_input *input, uint16_t usage, bool down, bool repeat);
void ph_input_release_all(ph_input *input);
/* Types one generated character (clipboard paste): press with `modifiers`, then
 * release the key and those modifiers together so none stays held afterwards. */
void ph_input_type(ph_input *input, uint8_t usage, uint8_t modifiers);

#endif
