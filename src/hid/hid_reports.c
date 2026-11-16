#include "hid/hid_reports.h"

#include <string.h>

/* clang-format off */
const uint8_t ph_hid_report_descriptor[] = {
    /* ---- Report 1: keyboard ---- */
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x06,             /* Usage (Keyboard) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, PH_HID_REPORT_ID_KEYBOARD, /* Report ID */
    0x05, 0x07,             /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,             /*   Usage Minimum (Left Control) */
    0x29, 0xE7,             /*   Usage Maximum (Right GUI) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x08,             /*   Report Count (8) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute): modifiers */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x01,             /*   Input (Constant): reserved byte */
    0x05, 0x08,             /*   Usage Page (LEDs) */
    0x19, 0x01,             /*   Usage Minimum (Num Lock) */
    0x29, 0x05,             /*   Usage Maximum (Kana) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x05,             /*   Report Count (5) */
    0x91, 0x02,             /*   Output (Data, Variable, Absolute): LEDs */
    0x75, 0x03,             /*   Report Size (3) */
    0x95, 0x01,             /*   Report Count (1) */
    0x91, 0x01,             /*   Output (Constant): LED padding */
    0x05, 0x07,             /*   Usage Page (Keyboard/Keypad) */
    0x19, 0x00,             /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x00,       /*   Usage Maximum (255) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x06,             /*   Report Count (6) */
    0x81, 0x00,             /*   Input (Data, Array): 6 key slots */
    0xC0,                   /* End Collection */

    /* ---- Report 2: mouse ---- */
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, PH_HID_REPORT_ID_MOUSE, /* Report ID */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x08,             /*     Usage Maximum (8) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x75, 0x01,             /*     Report Size (1) */
    0x95, 0x08,             /*     Report Count (8) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute): buttons */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x16, 0x01, 0x80,       /*     Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
    0x75, 0x10,             /*     Report Size (16) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x06,             /*     Input (Data, Variable, Relative): dx, dy */
    0x09, 0x38,             /*     Usage (Wheel) */
    0x15, 0x81,             /*     Logical Minimum (-127) */
    0x25, 0x7F,             /*     Logical Maximum (127) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x06,             /*     Input (Data, Variable, Relative): wheel */
    0x05, 0x0C,             /*     Usage Page (Consumer) */
    0x0A, 0x38, 0x02,       /*     Usage (AC Pan) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x06,             /*     Input (Data, Variable, Relative): pan */
    0xC0,                   /*   End Collection */
    0xC0,                   /* End Collection */

    /* ---- Report 3: consumer control ---- */
    0x05, 0x0C,             /* Usage Page (Consumer) */
    0x09, 0x01,             /* Usage (Consumer Control) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, PH_HID_REPORT_ID_CONSUMER, /* Report ID */
    0x19, 0x00,             /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x03,       /*   Usage Maximum (0x3FF) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x03,       /*   Logical Maximum (0x3FF) */
    0x75, 0x10,             /*   Report Size (16) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x00,             /*   Input (Data, Array): one usage */
    0xC0,                   /* End Collection */
};
/* clang-format on */

const size_t ph_hid_report_descriptor_len = sizeof ph_hid_report_descriptor;

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

void ph_hid_build_mouse(const ph_hid_mouse *m, uint8_t out[PH_HID_MOUSE_REPORT_LEN])
{
    out[0] = m->buttons;
    put_le16(&out[1], (uint16_t)m->dx);
    put_le16(&out[3], (uint16_t)m->dy);
    out[5] = (uint8_t)m->wheel;
    out[6] = (uint8_t)m->pan;
}

void ph_hid_build_keyboard(const ph_hid_keyboard *k, uint8_t out[PH_HID_KEYBOARD_REPORT_LEN])
{
    out[0] = k->modifiers;
    out[1] = 0;
    memcpy(&out[2], k->keys, sizeof k->keys);
}

void ph_hid_build_consumer(uint16_t usage, uint8_t out[PH_HID_CONSUMER_REPORT_LEN])
{
    put_le16(out, usage);
}

/* US layout punctuation: each entry is (unshifted char, shifted char, usage). */
static const struct {
    char plain, shifted;
    uint8_t usage;
} k_punct[] = {
    {'-', '_', 0x2D}, {'=', '+', 0x2E}, {'[', '{', 0x2F}, {']', '}', 0x30},
    {'\\', '|', 0x31}, {';', ':', 0x33}, {'\'', '"', 0x34}, {'`', '~', 0x35},
    {',', '<', 0x36}, {'.', '>', 0x37}, {'/', '?', 0x38},
};

/* Shifted digit row "!@#$%^&*()" maps onto usages for 1..9, 0. */
static const char k_shifted_digits[] = "!@#$%^&*()";

bool ph_hid_ascii_to_key(char c, uint8_t *usage, uint8_t *modifiers)
{
    size_t i;

    *modifiers = 0;
    if (c >= 'a' && c <= 'z') {
        *usage = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *usage = (uint8_t)(0x04 + (c - 'A'));
        *modifiers = PH_HID_MOD_LSHIFT;
        return true;
    }
    if (c >= '1' && c <= '9') {
        *usage = (uint8_t)(0x1E + (c - '1'));
        return true;
    }
    switch (c) {
    case '0':
        *usage = 0x27;
        return true;
    case '\n':
        *usage = PH_HID_KEY_ENTER;
        return true;
    case '\t':
        *usage = PH_HID_KEY_TAB;
        return true;
    case '\b':
        *usage = PH_HID_KEY_BACKSPACE;
        return true;
    case ' ':
        *usage = PH_HID_KEY_SPACE;
        return true;
    default:
        break;
    }
    for (i = 0; i < sizeof k_shifted_digits - 1; i++) {
        if (c == k_shifted_digits[i]) {
            *usage = (uint8_t)(0x1E + i); /* '!'->1 ... '('->9, ')'->0 */
            *modifiers = PH_HID_MOD_LSHIFT;
            return true;
        }
    }
    for (i = 0; i < sizeof k_punct / sizeof k_punct[0]; i++) {
        if (c == k_punct[i].plain || c == k_punct[i].shifted) {
            *usage = k_punct[i].usage;
            if (c == k_punct[i].shifted)
                *modifiers = PH_HID_MOD_LSHIFT;
            return true;
        }
    }
    return false;
}
