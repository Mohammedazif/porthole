#include "control/rotation_lock.h"

/* Within this the screen and the sensor agreeing counts as iOS following. */
#define FOLLOWED_MS 1500
/* The screen must have stayed this long before the phone turned away. */
#define STAYED_MS 1000

static bool landscape(int turn) { return turn == 6 || turn == 8; }

/* The screen's turn when iOS lays it out the way the phone is held
 * (measured: 6 with the top to the right, 8 with it to the left). */
static int turn_for_held(int held)
{
    return held == 1 ? 1 : held == 2 ? 3 : held == 3 ? 8 : held == 4 ? 6 : 0;
}

void ph_rotation_lock_begin(ph_rotation_lock *r, int remembered)
{
    *r = (ph_rotation_lock){.lock = remembered == 0 || remembered == 1 ? remembered
                                                                       : PH_ROTATION_LOCK_UNKNOWN};
}

bool ph_rotation_lock_observe(ph_rotation_lock *r, int report, int held, uint64_t now_ms)
{
    if (!r->seen) {
        /* Where the share starts says nothing about how iOS got there. */
        r->seen = true;
        r->report = report, r->held = held;
        r->report_ms = r->held_ms = 0;
        return false;
    }
    bool report_changed = report != r->report, held_changed = held != r->held;
    if (report_changed)
        r->report_ms = now_ms;
    if (held_changed)
        r->held_ms = now_ms;
    r->report = report, r->held = held;
    if (!(report_changed || held_changed) || !landscape(report) || (held != 3 && held != 4))
        return false;
    int verdict = PH_ROTATION_LOCK_UNKNOWN;
    if (turn_for_held(held) == report) {
        uint64_t apart = r->report_ms > r->held_ms ? r->report_ms - r->held_ms : r->held_ms - r->report_ms;
        if (r->report_ms && r->held_ms && apart <= FOLLOWED_MS)
            verdict = 0; /* the screen turned with the phone */
    } else if (held_changed && r->report_ms + STAYED_MS <= now_ms) {
        verdict = 1; /* turned to the other side; the screen stayed */
    }
    if (verdict == PH_ROTATION_LOCK_UNKNOWN || verdict == r->lock)
        return false;
    r->lock = verdict;
    return true;
}

int ph_rotation_lock_pointer_turn(const ph_rotation_lock *r, int report, int held)
{
    if (report == 1 || report == 0)
        return 0; /* a portrait screen: as shown, however the phone is held */
    /* Lock on, or not known yet: iOS takes the phone as upright. Until the
     * phone turns, upright is the better guess: a phone with the lock off
     * got to landscape by turning, which proves the lock off at once, and an
     * app that goes landscape by itself (YouTube full screen) is usually
     * started upright. A phone lying flat (no sensor reading) keeps what iOS
     * had, upright again. */
    if (r->lock != 0 || (held != 2 && held != 3 && held != 4))
        return 1;
    int turn = turn_for_held(held);
    return turn == report ? 0 : turn;
}
