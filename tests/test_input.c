/* Exercise complete PC event sequences without Bluetooth or an SDL window. */
#include <stdio.h>
#include <string.h>

#include "control/input.h"

enum { KEYBOARD, MOUSE, CONSUMER };
typedef struct record {
    int type;
    ph_hid_keyboard keys;
    ph_hid_mouse mouse;
    uint16_t consumer;
} record;
typedef struct fixture {
    ph_input input;
    record sent[64];
    size_t count;
} fixture;
static int failures;

#define CHECK(condition)                                                                            \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void keyboard(void *user, const ph_hid_keyboard *keys)
{
    fixture *f = user;
    f->sent[f->count++] = (record){.type = KEYBOARD, .keys = *keys};
}

static void mouse(void *user, const ph_hid_mouse *report)
{
    fixture *f = user;
    f->sent[f->count++] = (record){.type = MOUSE, .mouse = *report};
}

static void consumer(void *user, uint16_t usage)
{
    fixture *f = user;
    f->sent[f->count++] = (record){.type = CONSUMER, .consumer = usage};
}

static void init(fixture *f)
{
    memset(f, 0, sizeof *f);
    f->input.output = (ph_input_output){f, keyboard, mouse, consumer};
    f->input.enabled = true;
}

int main(void)
{
    fixture f;
    const uint8_t alt = PH_HID_MOD_LALT;

    /* Ctrl was pressed before the app gained focus: A must still be Cmd+A. */
    init(&f);
    ph_input_key(&f.input, 0x04, true, false, PH_HID_MOD_LCTRL);
    CHECK(f.count == 1 && f.sent[0].type == KEYBOARD);
    CHECK(f.sent[0].keys.modifiers == PH_HID_MOD_LGUI && f.sent[0].keys.keys[0] == 0x04);
    ph_input_key(&f.input, 0x04, false, false, PH_HID_MOD_LCTRL);
    ph_input_key(&f.input, 0xE0, false, false, 0);
    CHECK(f.sent[1].keys.modifiers == PH_HID_MOD_LGUI && f.sent[1].keys.keys[0] == 0);
    CHECK(f.sent[2].keys.modifiers == 0);

    /* Both Ctrl and GUI sides swap; Shift and right Alt remain intact. */
    init(&f);
    ph_input_key(&f.input, 0x04, true, false,
                 PH_HID_MOD_RCTRL | PH_HID_MOD_LGUI | PH_HID_MOD_RSHIFT | PH_HID_MOD_RALT);
    CHECK(f.sent[0].keys.modifiers
          == (PH_HID_MOD_RGUI | PH_HID_MOD_LCTRL | PH_HID_MOD_RSHIFT | PH_HID_MOD_RALT));

    /* All panel shortcuts have one held button, even for duplicate/repeat downs. */
    const uint8_t usages[] = {0x16, 0x06, 0x11, 0x13};
    const uint8_t buttons[] = {PH_HID_BTN_MIDDLE, PH_HID_BTN_4, PH_HID_BTN_5, PH_HID_BTN_6};
    for (size_t i = 0; i < sizeof usages; i++) {
        init(&f);
        ph_input_key(&f.input, 0xE2, true, false, alt);
        ph_input_key(&f.input, usages[i], true, false, alt);
        ph_input_key(&f.input, usages[i], true, true, alt);
        ph_input_key(&f.input, usages[i], true, false, alt);
        CHECK(f.count == 1 && f.sent[0].type == MOUSE);
        CHECK(f.sent[0].mouse.buttons == buttons[i]);
        ph_input_key(&f.input, usages[i], false, false, alt);
        ph_input_key(&f.input, 0xE2, false, false, 0);
        CHECK(f.count == 2 && f.sent[1].mouse.buttons == 0);
    }

    /* Releasing Alt before S must still release the mouse, without typing S. */
    init(&f);
    ph_input_key(&f.input, 0x16, true, false, alt); /* Alt held before focus */
    ph_input_key(&f.input, 0xE2, false, false, 0);
    ph_input_key(&f.input, 0x16, false, false, 0);
    CHECK(f.count == 2 && f.sent[0].type == MOUSE && f.sent[1].type == MOUSE);
    CHECK(f.input.buttons == 0 && f.input.keys.keys[0] == 0);

    /* A modifier first pressed with Alt remains releasable after Alt is released. */
    init(&f);
    ph_input_key(&f.input, 0xE2, true, false, alt);
    ph_input_key(&f.input, 0xE0, true, false, alt | PH_HID_MOD_LCTRL);
    ph_input_key(&f.input, 0xE2, false, false, PH_HID_MOD_LCTRL);
    ph_input_key(&f.input, 0xE0, false, false, 0);
    CHECK(f.count == 2 && f.sent[0].keys.modifiers == PH_HID_MOD_LGUI);
    CHECK(f.sent[1].keys.modifiers == 0);

    /* Focus loss releases a held shortcut; a later press can trigger it again. */
    init(&f);
    ph_input_key(&f.input, 0x06, true, false, alt);
    ph_input_release_all(&f.input);
    ph_input_key(&f.input, 0x06, true, false, alt);
    CHECK(f.count == 3 && f.sent[1].mouse.buttons == 0);
    CHECK(f.sent[2].mouse.buttons == PH_HID_BTN_4);

    /* A letter held before Alt cannot stay stuck or retrigger as a local hotkey. */
    init(&f);
    ph_input_key(&f.input, 0x16, true, false, 0);
    ph_input_key(&f.input, 0xE2, true, false, alt);
    ph_input_key(&f.input, 0x16, true, false, alt);
    ph_input_key(&f.input, 0x16, false, false, alt);
    ph_input_key(&f.input, 0xE2, false, false, 0);
    CHECK(f.count == 2 && f.sent[1].type == KEYBOARD && f.sent[1].keys.keys[0] == 0);
    CHECK(!f.input.down[0x16]);

    /* Spotlight sends Cmd+Space and restores an empty keyboard; F never leaks. */
    init(&f);
    ph_input_key(&f.input, 0x09, true, false, alt);
    ph_input_key(&f.input, 0xE2, false, false, 0);
    ph_input_key(&f.input, 0x09, false, false, 0);
    CHECK(f.count == 3 && f.sent[0].keys.modifiers == PH_HID_MOD_LGUI);
    CHECK(f.sent[0].keys.keys[0] == PH_HID_KEY_SPACE);
    CHECK(f.sent[2].keys.modifiers == 0 && f.sent[2].keys.keys[0] == 0);

    /* Home is also once per physical press; focus loss releases typed keys. */
    init(&f);
    ph_input_key(&f.input, 0x0B, true, false, alt);
    ph_input_key(&f.input, 0x0B, true, false, alt);
    CHECK(f.count == 2 && f.sent[0].type == CONSUMER);
    CHECK(f.sent[0].consumer == PH_HID_CONSUMER_AC_HOME && f.sent[1].consumer == 0);
    ph_input_release_all(&f.input);
    ph_input_key(&f.input, 0x04, true, false, 0);
    ph_input_release_all(&f.input);
    CHECK(f.sent[f.count - 1].keys.keys[0] == 0);

    /* Disconnected input is discarded; reconnect sends released state, not queued keys. */
    init(&f);
    ph_input_set_enabled(&f.input, false);
    ph_input_key(&f.input, 0x04, true, false, PH_HID_MOD_LCTRL);
    ph_input_key(&f.input, 0x16, true, false, alt);
    CHECK(f.count == 0 && !f.input.down[0x04] && !f.input.down[0x16]);
    ph_input_set_enabled(&f.input, true);
    CHECK(f.count == 3 && f.sent[0].keys.keys[0] == 0);
    CHECK(f.sent[1].mouse.buttons == 0 && f.sent[2].consumer == 0);
    ph_input_set_enabled(&f.input, true);
    CHECK(f.count == 3); /* repeated status notifications do not release active input */
    ph_input_key(&f.input, 0x04, true, false, 0);
    ph_input_set_enabled(&f.input, false);
    CHECK(f.sent[f.count - 1].keys.keys[0] == 0 && !f.input.down[0x04]);

    if (failures)
        return 1;
    puts("all input sequences passed");
    return 0;
}
