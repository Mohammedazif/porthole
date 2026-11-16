#include "control/keyboard.h"

#include <string.h>

#define USAGE_FIRST_MODIFIER 0xE0 /* Left Control */
#define USAGE_LAST_MODIFIER 0xE7  /* Right GUI */

static uint8_t swap_usage(uint8_t usage)
{
    switch (usage) {
    case 0xE0: return 0xE3; /* Left Ctrl  -> Left Cmd */
    case 0xE3: return 0xE0; /* Left GUI   -> Left Ctrl */
    case 0xE4: return 0xE7; /* Right Ctrl -> Right Cmd */
    case 0xE7: return 0xE4; /* Right GUI  -> Right Ctrl */
    default: return usage;
    }
}

bool ph_keyboard_set_modifiers(ph_hid_keyboard *k, uint8_t modifiers, bool swap_ctrl_cmd)
{
    uint8_t mapped = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (modifiers & (1u << i)) {
            uint8_t usage = (uint8_t)(USAGE_FIRST_MODIFIER + i);
            if (swap_ctrl_cmd)
                usage = swap_usage(usage);
            mapped |= (uint8_t)(1u << (usage - USAGE_FIRST_MODIFIER));
        }
    }
    bool changed = k->modifiers != mapped;
    k->modifiers = mapped;
    return changed;
}

bool ph_keyboard_apply(ph_hid_keyboard *k, uint8_t usage, bool down, bool swap_ctrl_cmd)
{
    if (swap_ctrl_cmd)
        usage = swap_usage(usage);

    if (usage >= USAGE_FIRST_MODIFIER && usage <= USAGE_LAST_MODIFIER) {
        uint8_t bit = (uint8_t)(1u << (usage - USAGE_FIRST_MODIFIER));
        uint8_t before = k->modifiers;
        k->modifiers = down ? (uint8_t)(k->modifiers | bit) : (uint8_t)(k->modifiers & ~bit);
        return k->modifiers != before;
    }
    if (usage < 0x04 || usage > 0xA4) /* 0-3 are error codes; above 0xA4 is reserved/keypad extras */
        return false;

    for (int i = 0; i < 6; i++) {
        if (k->keys[i] == usage) {
            if (down)
                return false; /* already held */
            /* Remove and keep the array packed. */
            memmove(&k->keys[i], &k->keys[i + 1], (size_t)(5 - i));
            k->keys[5] = 0;
            return true;
        }
    }
    if (!down)
        return false;
    for (int i = 0; i < 6; i++) {
        if (k->keys[i] == 0) {
            k->keys[i] = usage;
            return true;
        }
    }
    return false; /* six keys already held */
}

bool ph_keyboard_release_all(ph_hid_keyboard *k)
{
    static const ph_hid_keyboard empty;
    bool had = memcmp(k, &empty, sizeof empty) != 0;
    memset(k, 0, sizeof *k);
    return had;
}
