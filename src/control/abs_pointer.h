#ifndef PORTHOLE_CONTROL_ABS_POINTER_H
#define PORTHOLE_CONTROL_ABS_POINTER_H

#include <stdbool.h>
#include <stdint.h>

/* Absolute pointer control. With an absolute HID mouse, iOS maps X/Y straight to
 * screen fractions under AssistiveTouch (verified on iOS 26.6.1), so the PC mouse
 * position is forwarded as-is: no anchoring, acceleration model or video search.
 *
 * Measured iOS behaviour this module encodes:
 *  - X/Y in a report that changes a button are ignored, so a press is sent only
 *    after the position has been on screen for PH_ABS_SETTLE_MS (0 ms pressed the
 *    previous spot, 60 ms was unreliable after a long jump, 120 ms was reliable).
 *  - The HID wheel does not scroll; wheel notches become short held-button drags.
 *  - Home, App Switcher, Control Center and Notification Center are edge swipes.
 * Positions are fractions 0..1 of the phone picture. Reports are paced at least
 * PH_ABS_REPORT_MS apart so the helper's queue never builds latency. */

#define PH_ABS_SETTLE_MS 120
#define PH_ABS_REPORT_MS 15
#define PH_ABS_STEPS 160

typedef struct ph_abs_output {
    void *user;
    void (*report)(void *user, uint8_t buttons, double x, double y);
} ph_abs_output;

typedef struct ph_abs_step {
    uint64_t at_ms;
    uint8_t buttons;
    double x, y;
} ph_abs_step;

typedef struct ph_abs {
    ph_abs_output output;
    bool sent_valid;             /* x/y/buttons below reached the phone */
    double x, y;                 /* last sent position */
    uint8_t buttons;             /* last sent buttons */
    uint64_t sent_ms, moved_ms;  /* last report; last position change */
    double want_x, want_y;       /* where the PC mouse is */
    bool want_valid;
    uint8_t held;                /* physical buttons the user holds */
    uint8_t pending;             /* pressed by the user, waiting for the settle */
    uint64_t press_at_ms;
    uint8_t release_after;       /* released before the delayed press was sent */
    ph_abs_step steps[PH_ABS_STEPS]; /* a scripted gesture owns the pointer */
    unsigned int head, count;
    double wheel;                /* unscrolled notches (positive: content down) */
    double wheel_x, wheel_y;
    uint64_t wheel_ms;           /* last wheel notch */
    uint64_t feed_until_ms;      /* feed mode: ignore the rest of a wheel burst */
} ph_abs;

void ph_abs_init(ph_abs *a, ph_abs_output output);
/* PC mouse position over the picture. */
void ph_abs_move(ph_abs *a, double x, double y, uint64_t now_ms);
/* A physical button press or release at x/y (HID button bit). */
void ph_abs_button(ph_abs *a, uint8_t button, bool down, double x, double y, uint64_t now_ms);
/* Wheel notches at x/y: positive scrolls toward earlier content (up). feed_mode
 * turns a whole burst into one long swipe (Reels/Shorts). */
void ph_abs_wheel(ph_abs *a, double notches, double x, double y, bool feed_mode, uint64_t now_ms);
/* Scripted one-finger gesture: settle at x1/y1, press, glide to x2/y2 over
 * glide_ms, keep holding for hold_ms, release, then return to the PC mouse. */
bool ph_abs_swipe(ph_abs *a, double x1, double y1, double x2, double y2, uint32_t glide_ms,
                  uint32_t hold_ms, uint64_t now_ms);
/* `count` taps (2 = double-tap) at x/y, each held hold_ms (long press: 1000). */
bool ph_abs_tap(ph_abs *a, double x, double y, int count, uint32_t hold_ms, uint64_t now_ms);
bool ph_abs_home(ph_abs *a, uint64_t now_ms);
bool ph_abs_app_switcher(ph_abs *a, uint64_t now_ms);
bool ph_abs_control_center(ph_abs *a, uint64_t now_ms);
bool ph_abs_notification_center(ph_abs *a, uint64_t now_ms);
bool ph_abs_busy(const ph_abs *a);
/* Sends whatever is due; call from the main loop. */
void ph_abs_tick(ph_abs *a, uint64_t now_ms);
/* Focus loss, leaving the picture or disconnect: drop scripts, lift the finger. */
void ph_abs_release_all(ph_abs *a, uint64_t now_ms);

#endif
