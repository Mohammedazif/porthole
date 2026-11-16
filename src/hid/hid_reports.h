#ifndef PORTHOLE_HID_REPORTS_H
#define PORTHOLE_HID_REPORTS_H

/* The virtual Bluetooth device Porthole presents to the iPhone: one HID device with
 * three reports (keyboard, 8-button mouse, consumer keys). Every Bluetooth backend
 * (Windows BLE, ESP32 dongle, Linux BlueZ) sends exactly these bytes.
 *
 * The report builders return the report *payload* without the report ID byte,
 * which is what HID-over-GATT expects (the ID lives in the Report Reference
 * descriptor). Transports that need the ID prepend it themselves. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PH_HID_REPORT_ID_KEYBOARD = 1,
    PH_HID_REPORT_ID_MOUSE = 2,
    PH_HID_REPORT_ID_CONSUMER = 3,
};

#define PH_HID_KEYBOARD_REPORT_LEN 8 /* modifiers, reserved, 6 key slots */
#define PH_HID_MOUSE_REPORT_LEN 7    /* buttons, dx(16), dy(16), wheel, pan */
#define PH_HID_CONSUMER_REPORT_LEN 2 /* one 16-bit consumer usage */

extern const uint8_t ph_hid_report_descriptor[];
extern const size_t ph_hid_report_descriptor_len;

/* Mouse buttons. iOS AssistiveTouch can map buttons 3-8 to actions such as Home,
 * App Switcher or Control Center ("Customize Additional Buttons"). */
#define PH_HID_BTN_LEFT 0x01
#define PH_HID_BTN_RIGHT 0x02
#define PH_HID_BTN_MIDDLE 0x04
#define PH_HID_BTN_4 0x08
#define PH_HID_BTN_5 0x10
#define PH_HID_BTN_6 0x20
#define PH_HID_BTN_7 0x40
#define PH_HID_BTN_8 0x80

/* Keyboard modifier bits. GUI is the Command key on iOS. */
#define PH_HID_MOD_LCTRL 0x01
#define PH_HID_MOD_LSHIFT 0x02
#define PH_HID_MOD_LALT 0x04
#define PH_HID_MOD_LGUI 0x08
#define PH_HID_MOD_RCTRL 0x10
#define PH_HID_MOD_RSHIFT 0x20
#define PH_HID_MOD_RALT 0x40
#define PH_HID_MOD_RGUI 0x80

/* Common keyboard usages (HID Usage Tables, page 0x07). */
#define PH_HID_KEY_ENTER 0x28
#define PH_HID_KEY_ESCAPE 0x29
#define PH_HID_KEY_BACKSPACE 0x2A
#define PH_HID_KEY_TAB 0x2B
#define PH_HID_KEY_SPACE 0x2C
#define PH_HID_KEY_RIGHT 0x4F
#define PH_HID_KEY_LEFT 0x50
#define PH_HID_KEY_DOWN 0x51
#define PH_HID_KEY_UP 0x52

/* Consumer usages (page 0x0C). Which ones iOS honours is checked in spike S5. */
#define PH_HID_CONSUMER_POWER 0x0030
#define PH_HID_CONSUMER_MENU 0x0040
#define PH_HID_CONSUMER_PLAY_PAUSE 0x00CD
#define PH_HID_CONSUMER_NEXT_TRACK 0x00B5
#define PH_HID_CONSUMER_PREV_TRACK 0x00B6
#define PH_HID_CONSUMER_EJECT 0x00B8 /* toggles the on-screen keyboard on iOS */
#define PH_HID_CONSUMER_MUTE 0x00E2
#define PH_HID_CONSUMER_VOLUME_UP 0x00E9
#define PH_HID_CONSUMER_VOLUME_DOWN 0x00EA
#define PH_HID_CONSUMER_AC_SEARCH 0x0221
#define PH_HID_CONSUMER_AC_HOME 0x0223

typedef struct ph_hid_mouse {
    uint8_t buttons; /* PH_HID_BTN_* bits currently held */
    int16_t dx, dy;  /* relative movement, must not be INT16_MIN */
    int8_t wheel;    /* vertical scroll, must not be INT8_MIN */
    int8_t pan;      /* horizontal scroll, must not be INT8_MIN */
} ph_hid_mouse;

typedef struct ph_hid_keyboard {
    uint8_t modifiers; /* PH_HID_MOD_* bits */
    uint8_t keys[6];   /* pressed key usages, 0 = empty slot */
} ph_hid_keyboard;

void ph_hid_build_mouse(const ph_hid_mouse *m, uint8_t out[PH_HID_MOUSE_REPORT_LEN]);
void ph_hid_build_keyboard(const ph_hid_keyboard *k, uint8_t out[PH_HID_KEYBOARD_REPORT_LEN]);
void ph_hid_build_consumer(uint16_t usage, uint8_t out[PH_HID_CONSUMER_REPORT_LEN]);

/* Maps a printable ASCII character (plus '\n', '\t', '\b') to a keyboard usage and
 * the modifiers needed on a US layout. Returns false if it can't be typed. */
bool ph_hid_ascii_to_key(char c, uint8_t *usage, uint8_t *modifiers);

#endif
