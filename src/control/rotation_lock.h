#ifndef PORTHOLE_CONTROL_ROTATION_LOCK_H
#define PORTHOLE_CONTROL_ROTATION_LOCK_H

/* Whether the iPhone's Rotation Lock is on, learned from how its screen
 * follows the phone, and the turn the absolute pointer is aimed from.
 *
 * Measured on iOS 26: iOS places the absolute pointer by its own idea of how
 * the phone is held. With Rotation Lock off that follows the phone; with it
 * on it stays upright, even while an app (YouTube full screen) shows
 * landscape. Apps can't read the lock, and the broadcast extension gets no
 * orientation from UIDevice, so the PC watches instead:
 *  - lock off: iOS turns the screen with the phone, about half a second from
 *    the motion sensor's reading (either can come first);
 *  - lock on: the phone turns to the other landscape side and the landscape
 *    screen stays where it was (with the lock off iOS would have turned it
 *    before the sensor even reports, which lags by a second) - unless that
 *    landscape had already turned with the phone: then the lock is off and
 *    the app just doesn't flip (YouTube, measured), and iOS aims the pointer
 *    by the phone as held.
 * Until either is seen the pointer is aimed as if upright (see the .c).
 * Turns are EXIF orientations (1 upright, 3 upside down, 6, 8); held is the
 * motion sensor's byte (1 upright, 2 upside down, 3 top left, 4 top right). */

#include <stdbool.h>
#include <stdint.h>

#define PH_ROTATION_LOCK_UNKNOWN (-1)

typedef struct ph_rotation_lock {
    int lock;                 /* 1 on, 0 off, PH_ROTATION_LOCK_UNKNOWN */
    bool seen;                /* a turn message arrived this share */
    int report, held;         /* last seen */
    bool followed;            /* this landscape turned with the phone */
    uint64_t report_ms, held_ms; /* when each last changed */
} ph_rotation_lock;

/* Starts a share, keeping what was learned before (the lock rarely changes). */
void ph_rotation_lock_begin(ph_rotation_lock *r, int remembered);
/* Takes one turn report. Returns true when the guess changed. */
bool ph_rotation_lock_observe(ph_rotation_lock *r, int report, int held, uint64_t now_ms);
/* The turn of the screen as iOS places the pointer: 0 for as shown (the
 * screen's own layout), else the EXIF turn to aim the picture to. */
int ph_rotation_lock_pointer_turn(const ph_rotation_lock *r, int report, int held);

#endif
