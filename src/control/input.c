#include "control/input.h"

#include <string.h>

static void send_keys(ph_input *input)
{
    if (input->output.keyboard)
        input->output.keyboard(input->output.user, &input->keys);
}

static void send_buttons(ph_input *input)
{
    ph_hid_mouse mouse = {.buttons = input->buttons};
    if (input->output.mouse)
        input->output.mouse(input->output.user, &mouse);
}

static uint8_t shortcut_button(uint8_t usage)
{
    switch (usage) {
    case 0x16: return PH_HID_BTN_MIDDLE; /* S: App Switcher */
    case 0x06: return PH_HID_BTN_4;      /* C: Control Center */
    case 0x11: return PH_HID_BTN_5;      /* N: Notification Center */
    case 0x13: return PH_HID_BTN_6;      /* P: Screenshot */
    default: return 0;
    }
}

static uint16_t held_consumer(const ph_input *input)
{
    return input->consumer_count ? input->consumer_keys[input->consumer_count - 1] : 0;
}

static bool supported_consumer(uint16_t usage)
{
    switch (usage) {
    case PH_HID_CONSUMER_VOLUME_UP:
    case PH_HID_CONSUMER_VOLUME_DOWN:
    case PH_HID_CONSUMER_MUTE:
    case PH_HID_CONSUMER_PLAY_PAUSE:
    case PH_HID_CONSUMER_NEXT_TRACK:
    case PH_HID_CONSUMER_PREV_TRACK:
    case PH_HID_CONSUMER_STOP:
    case PH_HID_CONSUMER_EJECT:
        return true;
    default:
        return false;
    }
}

static void consumer_shortcut(ph_input *input, uint16_t usage)
{
    uint16_t held = held_consumer(input);
    if (input->output.consumer && usage != held) {
        input->output.consumer(input->output.user, usage);
        input->output.consumer(input->output.user, held);
    }
}

static void keyboard_shortcut(ph_input *input, uint8_t usage)
{
    ph_hid_keyboard keys = {.modifiers = PH_HID_MOD_LGUI, .keys = {usage}};
    if (!input->output.keyboard)
        return;
    input->output.keyboard(input->output.user, &keys);
    keys.keys[0] = 0;
    input->output.keyboard(input->output.user, &keys);
    send_keys(input);
}

static void shortcut_down(ph_input *input, uint8_t usage)
{
    uint8_t button = shortcut_button(usage);
    if (button) {
        input->buttons |= button;
        send_buttons(input);
        return;
    }
    switch (usage) {
    case 0x0B: consumer_shortcut(input, PH_HID_CONSUMER_AC_HOME); break; /* H */
    case 0x09: keyboard_shortcut(input, PH_HID_KEY_SPACE); break;       /* F: Spotlight */
    case 0x50: keyboard_shortcut(input, 0x2F); break;                  /* Left: Cmd+[ */
    case 0x52: consumer_shortcut(input, PH_HID_CONSUMER_VOLUME_UP); break;
    case 0x51: consumer_shortcut(input, PH_HID_CONSUMER_VOLUME_DOWN); break;
    default: break;
    }
}

void ph_input_key(ph_input *input, uint8_t usage, bool down, bool repeat, uint8_t modifiers)
{
    bool alt = (modifiers & PH_HID_MOD_LALT) != 0;
    bool changed;

    if (!input->enabled)
        return;
    if (down && (repeat || input->down[usage]))
        return; /* one action per physical press, including duplicate non-repeat events */
    input->down[usage] = down;

    if (usage == 0xE2) { /* Left Alt is local; release forwarded keys before a shortcut. */
        if (down) {
            if (ph_keyboard_release_all(&input->keys))
                send_keys(input);
        } else if (ph_keyboard_set_modifiers(&input->keys, modifiers, true)) {
            send_keys(input);
        }
        return;
    }

    if (!down && input->local[usage]) {
        uint8_t button = shortcut_button(usage);
        input->local[usage] = false;
        if (button && (input->buttons & button)) {
            input->buttons &= (uint8_t)~button;
            send_buttons(input);
        }
        if (ph_keyboard_set_modifiers(&input->keys,
                                       alt ? 0 : (uint8_t)(modifiers & ~PH_HID_MOD_LALT), true))
            send_keys(input);
        return; /* still local when Alt was released before this key */
    }

    if (down && alt) {
        input->local[usage] = true;
        /* Alt might have been held before the window gained focus. */
        if (ph_keyboard_release_all(&input->keys))
            send_keys(input);
        shortcut_down(input, usage);
        return;
    }

    changed = ph_keyboard_apply(&input->keys, usage, down, true);
    /* Key releases must reach the phone even while Alt is held. */
    changed = ph_keyboard_set_modifiers(&input->keys,
                                        alt ? 0 : (uint8_t)(modifiers & ~PH_HID_MOD_LALT), true)
              || changed;
    if (changed)
        send_keys(input);
}

void ph_input_consumer_key(ph_input *input, uint16_t usage, bool down, bool repeat)
{
    if (!input->enabled || !supported_consumer(usage))
        return;
    uint16_t previous = held_consumer(input);
    size_t index = 0;
    while (index < input->consumer_count && input->consumer_keys[index] != usage)
        index++;
    if (down) {
        if (repeat || index < input->consumer_count
            || input->consumer_count == PH_INPUT_CONSUMER_KEYS)
            return;
        input->consumer_keys[input->consumer_count++] = usage;
    } else {
        if (index == input->consumer_count)
            return;
        memmove(input->consumer_keys + index, input->consumer_keys + index + 1,
                (input->consumer_count - index - 1) * sizeof input->consumer_keys[0]);
        input->consumer_keys[--input->consumer_count] = 0;
    }
    uint16_t current = held_consumer(input);
    if (previous != current && input->output.consumer)
        input->output.consumer(input->output.user, current);
}

void ph_input_release_all(ph_input *input)
{
    if (ph_keyboard_release_all(&input->keys))
        send_keys(input);
    if (input->buttons) {
        input->buttons = 0;
        send_buttons(input);
    }
    if (input->consumer_count) {
        memset(input->consumer_keys, 0, sizeof input->consumer_keys);
        input->consumer_count = 0;
        if (input->output.consumer)
            input->output.consumer(input->output.user, 0);
    }
    memset(input->down, 0, sizeof input->down);
    memset(input->local, 0, sizeof input->local);
}

void ph_input_set_enabled(ph_input *input, bool enabled)
{
    if (input->enabled == enabled)
        return;
    ph_input_release_all(input);
    input->enabled = enabled;
    if (enabled) {
        send_keys(input);
        send_buttons(input);
        if (input->output.consumer)
            input->output.consumer(input->output.user, 0);
    }
}

void ph_input_type(ph_input *input, uint8_t usage, uint8_t modifiers)
{
    ph_input_key(input, usage, true, false, modifiers);
    ph_input_key(input, usage, false, false, 0);
}
