/* PC key events -> HID keyboard report, including the Ctrl/Cmd swap. */
#include <stdio.h>
#include <string.h>

#include "control/keyboard.h"
#include "hid/link_proto.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

int main(void)
{
    ph_hid_keyboard k;
    memset(&k, 0, sizeof k);

    /* Plain key press/release. */
    CHECK(ph_keyboard_apply(&k, 0x04, true, true)); /* A */
    CHECK(k.keys[0] == 0x04);
    CHECK(!ph_keyboard_apply(&k, 0x04, true, true)); /* auto-repeat: no change */
    CHECK(ph_keyboard_apply(&k, 0x04, false, true));
    CHECK(k.keys[0] == 0);

    /* Ctrl+C on the PC becomes Cmd+C on the iPhone. */
    CHECK(ph_keyboard_apply(&k, 0xE0, true, true)); /* Left Ctrl */
    CHECK(k.modifiers == PH_HID_MOD_LGUI);
    CHECK(ph_keyboard_apply(&k, 0x06, true, true)); /* C */
    CHECK(k.modifiers == PH_HID_MOD_LGUI && k.keys[0] == 0x06);
    CHECK(ph_keyboard_apply(&k, 0x06, false, true));
    CHECK(ph_keyboard_apply(&k, 0xE0, false, true));
    CHECK(k.modifiers == 0);

    /* The Windows key becomes Control; without the swap keys pass through unchanged. */
    CHECK(ph_keyboard_apply(&k, 0xE3, true, true));
    CHECK(k.modifiers == PH_HID_MOD_LCTRL);
    CHECK(ph_keyboard_apply(&k, 0xE3, false, true));
    CHECK(ph_keyboard_apply(&k, 0xE0, true, false));
    CHECK(k.modifiers == PH_HID_MOD_LCTRL);
    CHECK(ph_keyboard_apply(&k, 0xE0, false, false));

    /* Releasing a middle key keeps the array packed; a seventh key is ignored. */
    for (int i = 0; i < 6; i++)
        CHECK(ph_keyboard_apply(&k, (uint8_t)(0x04 + i), true, true));
    CHECK(!ph_keyboard_apply(&k, 0x0A, true, true));
    CHECK(ph_keyboard_apply(&k, 0x06, false, true));
    CHECK(k.keys[0] == 0x04 && k.keys[1] == 0x05 && k.keys[2] == 0x07 && k.keys[5] == 0);

    /* Release all (e.g. when the window loses focus). */
    CHECK(ph_keyboard_release_all(&k));
    CHECK(!ph_keyboard_release_all(&k));

    /* Unknown usages are ignored. */
    CHECK(!ph_keyboard_apply(&k, 0x00, true, true));
    CHECK(!ph_keyboard_apply(&k, 0xF0, true, true));

    /* Link frames: [type][length][payload]. */
    {
        uint8_t out[PH_LINK_MAX_FRAME], payload[2] = {0x23, 0x02};
        CHECK(ph_link_frame(PH_LINK_CONSUMER, payload, 2, out) == 4);
        CHECK(out[0] == PH_LINK_CONSUMER && out[1] == 2 && out[2] == 0x23 && out[3] == 0x02);
        CHECK(ph_link_frame(PH_LINK_KEYBOARD, payload, 9, out) == 0);
    }

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all keyboard checks passed\n");
    return 0;
}
