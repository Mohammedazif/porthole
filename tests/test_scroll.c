/* Wheel routing, independent axes, and bounded input while Bluetooth is busy. */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control/scroll.h"

typedef struct fixture {
    ph_scroll scroll;
    ph_hid_mouse reports[512];
    size_t count;
    unsigned int delays, syncs;
    uint32_t last_sync;
} fixture;
static int failures;

#define CHECK(condition)                                                                            \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void mouse(void *user, const ph_hid_mouse *report)
{
    fixture *f = user;
    if (f->count < sizeof f->reports / sizeof f->reports[0])
        f->reports[f->count++] = *report;
    else
        CHECK(false);
    CHECK(report->buttons == 0); /* Positioning and scrolling must never click. */
}
static void delay(void *user, uint16_t milliseconds)
{
    fixture *f = user;
    CHECK(milliseconds > 0 && milliseconds < 1000);
    f->delays++;
}
static void sync_reports(void *user, uint32_t id)
{
    fixture *f = user;
    f->syncs++;
    f->last_sync = id;
}
static void init(fixture *f)
{
    memset(f, 0, sizeof *f);
    f->scroll.output = (ph_scroll_output){f, mouse, delay, sync_reports};
}
static int wheel_total(const fixture *f)
{
    int total = 0;
    for (size_t i = 0; i < f->count; i++)
        total += f->reports[i].wheel;
    return total;
}
static int pan_total(const fixture *f)
{
    int total = 0;
    for (size_t i = 0; i < f->count; i++)
        total += f->reports[i].pan;
    return total;
}
static int anchors(const fixture *f)
{
    int count = 0;
    for (size_t i = 0; i < f->count; i++)
        count += f->reports[i].dx < 0 && f->reports[i].dy < 0;
    return count;
}

int main(void)
{
    fixture f;
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -1));
    CHECK(wheel_total(&f) == 1 && pan_total(&f) == 0 && f.syncs == 1);
    CHECK(anchors(&f) > 0 && f.delays == 2);
    bool moved_right = false, moved_down = false, saw_wheel = false;
    for (size_t i = 0; i < f.count; i++) {
        if (f.reports[i].wheel) {
            CHECK(moved_right && moved_down);
            saw_wheel = true;
        }
        if (!saw_wheel) {
            moved_right |= f.reports[i].dx > 0;
            moved_down |= f.reports[i].dy > 0;
        }
    }
    CHECK(saw_wheel);

    /* New notches accumulate instead of flooding the transport; stale ACKs do nothing. */
    size_t first_count = f.count;
    int first_anchors = anchors(&f);
    uint32_t first_id = f.last_sync;
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -3));
    CHECK(f.count == first_count);
    ph_scroll_ack(&f.scroll, first_id + 1);
    CHECK(f.count == first_count);
    ph_scroll_ack(&f.scroll, first_id);
    CHECK(wheel_total(&f) == 2 && f.syncs == 2 && f.scroll.pending_y == -2);
    CHECK(anchors(&f) == first_anchors); /* same target: keep the existing position */
    /* Wheel-only follow-ups must not move the pointer or insert a per-notch pause. */
    for (size_t i = first_count; i < f.count; i++)
        CHECK(f.reports[i].dx == 0 && f.reports[i].dy == 0);
    CHECK(f.delays == 2);

    /* A diagonal event preserves each axis's count instead of multiplying the smaller one. */
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 1, -2));
    CHECK(wheel_total(&f) == 1 && pan_total(&f) == 1);
    ph_scroll_ack(&f.scroll, f.last_sync);
    CHECK(wheel_total(&f) == 2 && pan_total(&f) == 1);

    /* Follow a changed PC target relatively; only orientation needs a fresh anchor. */
    int n = anchors(&f);
    ph_scroll_ack(&f.scroll, f.last_sync);
    CHECK(ph_scroll_request(&f.scroll, 0.8, 0.7, 0.46, 0, 1));
    CHECK(anchors(&f) == n);
    ph_scroll_ack(&f.scroll, f.last_sync);
    CHECK(ph_scroll_request(&f.scroll, 0.8, 0.7, 2.16, 0, 1));
    CHECK(anchors(&f) == 2 * n);

    /* Disconnect/focus loss drops queued work, and an old ACK cannot finish a new batch. */
    uint32_t old_id = f.last_sync;
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, 4));
    ph_scroll_reset(&f.scroll);
    first_count = f.count;
    ph_scroll_ack(&f.scroll, old_id);
    CHECK(f.count == first_count && f.scroll.pending_y == 0);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, 1));
    CHECK(f.last_sync != old_id);
    ph_scroll_ack(&f.scroll, old_id);
    CHECK(f.scroll.in_flight);

    /* Huge wheel events and accumulated bursts remain bounded and cannot overflow. */
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, INT_MAX));
    CHECK(wheel_total(&f) == -1 && f.count < 128);
    first_count = f.count;
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, INT_MAX));
    CHECK(f.scroll.pending_y <= 4 && f.count == first_count);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, INT_MIN));
    CHECK(f.scroll.pending_y >= -4 && f.count == first_count);

    /* Reversing discards the old direction instead of continuing a queued burst. */
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, 20));
    CHECK(f.scroll.pending_y == 3);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, -1));
    CHECK(f.scroll.pending_y == -1);
    ph_scroll_ack(&f.scroll, f.last_sync);
    CHECK(wheel_total(&f) == 0 && f.scroll.pending_y == 0);

    /* Small hand movements and focus changes do not send the pointer to the corner. */
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, 1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    first_count = f.count;
    n = anchors(&f);
    ph_scroll_cancel(&f.scroll);
    CHECK(f.scroll.positioned);
    CHECK(ph_scroll_request(&f.scroll, 0.51, 0.505, 0.46, 0, 1));
    CHECK(anchors(&f) == n);
    for (size_t i = first_count; i < f.count; i++)
        CHECK(abs(f.reports[i].dx) <= 1 && f.reports[i].dy == 0);

    init(&f);
    CHECK(!ph_scroll_request(&f.scroll, NAN, 0.5, 0.46, 0, 1));
    CHECK(!ph_scroll_request(&f.scroll, 0.5, INFINITY, 0.46, 0, 1));
    CHECK(!ph_scroll_request(&f.scroll, -0.1, 0.5, 0.46, 0, 1));
    CHECK(!ph_scroll_request(&f.scroll, 1, 0.5, 0.46, 0, 1));
    CHECK(!ph_scroll_request(&f.scroll, 0.5, 0.5, 0, 0, 1));
    CHECK(!ph_scroll_request(&f.scroll, 0.5, 0.5, 0.46, 0, 0));
    CHECK(f.count == 0);

    /* Calibration changes motion, preserves wheel strength, and invalidates old position. */
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -1));
    first_count = f.count;
    CHECK(ph_scroll_set_scale(&f.scroll, 2, 2));
    CHECK(!f.scroll.positioned && !f.scroll.in_flight);
    f.count = 0;
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -1));
    CHECK(f.count < first_count && wheel_total(&f) == 1);
    CHECK(!ph_scroll_set_scale(&f.scroll, NAN, 1));
    CHECK(!ph_scroll_set_scale(&f.scroll, 1, INFINITY));
    CHECK(!ph_scroll_set_scale(&f.scroll, 0, 1));
    CHECK(!ph_scroll_set_scale(&f.scroll, 1, 21));
    CHECK(f.scroll.scale_x == 2 && f.scroll.scale_y == 2);

    /* Reproduce iOS's compressed indicator at the top. Extra queued wheel
     * events stop at that edge; fading and delayed ACKs must not restart them. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, 121.0 / 1080, 452.0 / 1080, 100);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, 1));
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, 40));
    ph_scroll_ack(&f.scroll, f.last_sync);
    first_count = f.count;
    CHECK(f.scroll.waiting_frame);
    ph_scroll_observe(&f.scroll, true, 121.0 / 1080, 440.0 / 1080, 140);
    CHECK(f.scroll.blocked_y == 1 && f.scroll.pending_y == 0 && f.count == first_count);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, 100));
    ph_scroll_observe(&f.scroll, false, 0, 0, 200);
    ph_scroll_tick(&f.scroll, 1000);
    CHECK(f.count == first_count);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -1));
    CHECK(f.count > first_count); /* reversing can leave the edge immediately */
    ph_scroll_ack(&f.scroll, f.last_sync);
    ph_scroll_observe(&f.scroll, true, 160.0 / 1080, 491.0 / 1080, 1050);
    CHECK(f.scroll.blocked_y == 0);

    /* A full-size thumb pinned at the edge also stops input; waiting for a
     * severely shortened thumb would let the blank space grow first. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, 121.0 / 1080, 452.0 / 1080, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .65, .46, 0, 1));
    CHECK(wheel_total(&f) == -1); /* small probe when moving into the nearby edge */
    ph_scroll_ack(&f.scroll, f.last_sync);
    ph_scroll_observe(&f.scroll, true, 121.0 / 1080, 452.0 / 1080, 140);
    CHECK(f.scroll.blocked_y == 1);

    /* Same reproduction at the bottom, including a jump straight to a short
     * indicator rather than observing every intermediate animation frame. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, 534.0 / 1080, 865.0 / 1080, 100);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -1));
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -40));
    ph_scroll_ack(&f.scroll, f.last_sync);
    first_count = f.count;
    ph_scroll_observe(&f.scroll, true, 1006.0 / 1080, 1015.0 / 1080, 140);
    CHECK(f.scroll.blocked_y == -1 && f.count == first_count && f.scroll.pending_y == 0);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -40));
    CHECK(f.count == first_count);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, 1));
    CHECK(f.count > first_count);

    /* A normal moving indicator permits the next notch after a fresh frame;
     * missing frames have a bounded timeout instead of hanging scrolling. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, .3, .6, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    CHECK(wheel_total(&f) == 3); /* normal travel is restored with video guarding */
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    first_count = f.count;
    ph_scroll_observe(&f.scroll, true, .31, .61, 110);
    CHECK(f.count == first_count); /* a frame still in transit before the input */
    ph_scroll_observe(&f.scroll, true, .32, .62, 140);
    CHECK(f.count > first_count && !f.scroll.blocked_y);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    first_count = f.count;
    ph_scroll_tick(&f.scroll, 321);
    CHECK(f.count > first_count);
    ph_scroll_reset(&f.scroll);
    CHECK(!f.scroll.have_bar && !f.scroll.blocked_y && !f.scroll.waiting_frame);

    if (failures)
        return 1;
    puts("all scroll sequences passed");
    return 0;
}
