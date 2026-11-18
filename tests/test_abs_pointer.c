/* Absolute pointer scripting: settle-before-press, pacing, drags, wheel drags and
 * edge gestures, against a recorder (no Bluetooth). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "control/abs_pointer.h"

typedef struct sent {
    uint64_t at;
    uint8_t buttons;
    double x, y;
} sent;

static sent log_[2048];
static int count_;
static uint64_t now_;
static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void report(void *user, uint8_t buttons, double x, double y)
{
    (void)user;
    if (count_ < 2048)
        log_[count_++] = (sent){now_, buttons, x, y};
}

static void fresh(ph_abs *a)
{
    ph_abs_init(a, (ph_abs_output){NULL, report});
    count_ = 0;
    now_ = 1000;
}

static void run_until(ph_abs *a, uint64_t until)
{
    while (now_ < until) {
        now_++;
        ph_abs_tick(a, now_);
    }
}

static int presses(void)
{
    int n = 0;
    uint8_t before = 0;
    for (int i = 0; i < count_; i++) {
        if ((log_[i].buttons & 1) && !(before & 1))
            n++;
        before = log_[i].buttons;
    }
    return n;
}

static int first_with_button(void)
{
    for (int i = 0; i < count_; i++)
        if (log_[i].buttons & 1)
            return i;
    return -1;
}

int main(void)
{
    ph_abs a;

    /* Hover goes straight to the phone, paced to one report per 15 ms with the
     * latest position winning. */
    fresh(&a);
    ph_abs_move(&a, .25, .5, now_);
    CHECK(count_ == 1 && log_[0].x == .25 && log_[0].y == .5 && !log_[0].buttons);
    now_ += 5;
    ph_abs_move(&a, .26, .5, now_);
    now_ += 5;
    ph_abs_move(&a, .27, .5, now_);
    CHECK(count_ == 1);
    run_until(&a, now_ + 10);
    CHECK(count_ == 2 && log_[1].x == .27);

    /* A press right after moving waits PH_ABS_SETTLE_MS at the same position:
     * iOS ignores X/Y in a button-change report. Motion meanwhile is held back. */
    fresh(&a);
    ph_abs_move(&a, .6, .7, now_);
    uint64_t moved = now_;
    now_ += 10;
    ph_abs_button(&a, 1, true, .6, .7, now_);
    ph_abs_move(&a, .9, .9, now_ + 5);
    run_until(&a, moved + PH_ABS_SETTLE_MS - 1);
    CHECK(first_with_button() < 0);
    run_until(&a, moved + PH_ABS_SETTLE_MS);
    int p = first_with_button();
    CHECK(p >= 0 && log_[p].x == .6 && log_[p].y == .7 && log_[p].at == moved + PH_ABS_SETTLE_MS);
    /* Held: further motion drags with the button down. */
    run_until(&a, now_ + 20);
    CHECK(log_[count_ - 1].buttons == 1 && log_[count_ - 1].x == .9);
    ph_abs_button(&a, 1, false, .9, .9, now_);
    CHECK(log_[count_ - 1].buttons == 0);

    /* Pointer resting long enough: the press goes out at once. */
    fresh(&a);
    ph_abs_move(&a, .4, .4, now_);
    run_until(&a, now_ + 500);
    ph_abs_button(&a, 1, true, .4, .4, now_);
    CHECK(log_[count_ - 1].buttons == 1);

    /* A quick click (released before its delayed press) still presses, then lifts. */
    fresh(&a);
    ph_abs_move(&a, .3, .3, now_);
    ph_abs_button(&a, 1, true, .3, .3, now_ + 1);
    ph_abs_button(&a, 1, false, .3, .3, now_ + 2);
    run_until(&a, now_ + 400);
    CHECK(presses() == 1 && log_[count_ - 1].buttons == 0);

    /* A fast double-click stays two presses. */
    fresh(&a);
    ph_abs_move(&a, .5, .5, now_);
    run_until(&a, now_ + 300);
    ph_abs_button(&a, 1, true, .5, .5, now_);
    ph_abs_button(&a, 1, false, .5, .5, now_ + 20);
    run_until(&a, now_ + 30);
    ph_abs_button(&a, 1, true, .5, .5, now_);
    ph_abs_button(&a, 1, false, .5, .5, now_ + 20);
    run_until(&a, now_ + 400);
    CHECK(presses() == 2 && log_[count_ - 1].buttons == 0);

    /* Double-click right after moving: the first click's release is still queued
     * when the second press arrives; it must lift first, not merge the presses. */
    fresh(&a);
    ph_abs_move(&a, .45, .55, now_);
    uint64_t t0 = now_;
    ph_abs_button(&a, 1, true, .45, .55, t0 + 1);
    ph_abs_button(&a, 1, false, .45, .55, t0 + 2);
    run_until(&a, t0 + PH_ABS_SETTLE_MS + 30); /* pressed; its release still queued */
    CHECK(log_[count_ - 1].buttons == 1);
    ph_abs_button(&a, 1, true, .45, .55, now_);
    ph_abs_button(&a, 1, false, .45, .55, now_ + 5);
    run_until(&a, now_ + 400);
    CHECK(presses() == 2 && log_[count_ - 1].buttons == 0);

    /* Wheel: the HID wheel does not scroll iOS, so notches become a held drag.
     * Scrolling down (negative) moves the finger up, then the pointer returns. */
    fresh(&a);
    ph_abs_move(&a, .5, .6, now_);
    ph_abs_wheel(&a, -2, .5, .6, false, now_);
    run_until(&a, now_ + 1000);
    p = first_with_button();
    CHECK(p >= 0);
    double lowest = 1;
    for (int i = p; i < count_; i++)
        if (log_[i].buttons & 1 && log_[i].y < lowest)
            lowest = log_[i].y;
    CHECK(fabs(lowest - (.6 - 2 * .07)) < 1e-6);
    CHECK(presses() == 1 && log_[count_ - 1].buttons == 0 && log_[count_ - 1].y == .6);
    /* Scrolling up near the bottom edge keeps the stroke on screen. */
    fresh(&a);
    ph_abs_move(&a, .5, .95, now_);
    ph_abs_wheel(&a, 3, .5, .95, false, now_);
    run_until(&a, now_ + 1000);
    for (int i = 0; i < count_; i++)
        CHECK(log_[i].y <= .97 + 1e-9);

    /* A fast wheel spin never leaves a queue of drags running after it stops:
     * one drag in progress plus at most one more. */
    fresh(&a);
    ph_abs_move(&a, .5, .5, now_);
    run_until(&a, now_ + 300);
    for (int i = 0; i < 30; i++) {
        ph_abs_wheel(&a, -1, .5, .5, false, now_);
        run_until(&a, now_ + 10);
    }
    uint64_t wheel_stopped = now_;
    run_until(&a, now_ + 3000);
    int after_stop = 0;
    for (int i = 1; i < count_; i++)
        if ((log_[i].buttons & 1) && !(log_[i - 1].buttons & 1) && log_[i].at > wheel_stopped)
            after_stop++;
    CHECK(after_stop <= 1);
    CHECK(log_[count_ - 1].buttons == 0);
    /* Wheel under a resting pointer presses without the settle delay. */
    fresh(&a);
    ph_abs_move(&a, .5, .5, now_);
    run_until(&a, now_ + 300);
    uint64_t wheel_at = now_;
    ph_abs_wheel(&a, -1, .5, .5, false, now_);
    run_until(&a, now_ + 500);
    p = first_with_button();
    CHECK(p >= 0 && log_[p].at - wheel_at < 20);

    /* Feed mode: one long swipe per wheel burst. */
    fresh(&a);
    ph_abs_move(&a, .5, .5, now_);
    for (int i = 0; i < 6; i++)
        ph_abs_wheel(&a, -1, .5, .5, true, now_ + (uint64_t)i * 40);
    run_until(&a, now_ + 1500);
    CHECK(presses() == 1);

    /* Edge gestures follow the measured strokes; App Switcher holds before lifting. */
    fresh(&a);
    ph_abs_move(&a, .5, .5, now_);
    CHECK(ph_abs_app_switcher(&a, now_));
    CHECK(ph_abs_busy(&a));
    run_until(&a, now_ + 2000);
    p = first_with_button();
    CHECK(p >= 0 && log_[p].y == 1);
    int last_held = p;
    while (last_held + 1 < count_ && log_[last_held + 1].buttons)
        last_held++;
    CHECK(fabs(log_[last_held].y - 760.0 / 1080) < 1e-6);
    CHECK(log_[last_held + 1].at - log_[last_held].at >= 700);
    CHECK(!ph_abs_busy(&a) && log_[count_ - 1].x == .5 && log_[count_ - 1].y == .5);
    fresh(&a);
    CHECK(ph_abs_home(&a, now_) && ph_abs_control_center(&a, now_)
          && ph_abs_notification_center(&a, now_));
    run_until(&a, now_ + 4000);
    CHECK(presses() == 3 && log_[count_ - 1].buttons == 0);

    /* Scripts never start over a held finger; a real press cancels a script. */
    fresh(&a);
    ph_abs_move(&a, .5, .5, now_);
    ph_abs_button(&a, 1, true, .5, .5, now_);
    CHECK(!ph_abs_home(&a, now_));
    ph_abs_button(&a, 1, false, .5, .5, now_);
    run_until(&a, now_ + 400);
    fresh(&a);
    ph_abs_move(&a, .5, .5, now_);
    CHECK(ph_abs_swipe(&a, .5, .8, .5, .2, 300, 0, now_));
    run_until(&a, now_ + 200); /* finger down mid-swipe */
    CHECK(log_[count_ - 1].buttons == 1);
    ph_abs_release_all(&a, now_);
    CHECK(log_[count_ - 1].buttons == 0 && !ph_abs_busy(&a));

    /* Double-tap and long press scripts. */
    fresh(&a);
    CHECK(ph_abs_tap(&a, .2, .3, 2, 60, now_));
    run_until(&a, now_ + 1000);
    CHECK(presses() == 2 && log_[count_ - 1].buttons == 0);
    fresh(&a);
    CHECK(ph_abs_tap(&a, .2, .3, 1, 1000, now_));
    run_until(&a, now_ + 2000);
    p = first_with_button();
    CHECK(p >= 0 && log_[p + 1].at - log_[p].at >= 1000);

    if (failures)
        return 1;
    puts("absolute pointer scripting passed");
    return 0;
}
