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

static void timed_ack_sequences(void)
{
    fixture f;
    init(&f);
    ph_scroll_tick(&f.scroll, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -4));
    uint32_t old_id = f.last_sync;
    size_t first_count = f.count;
    CHECK(f.scroll.pending_y == -3 && f.scroll.sent_at == 100);

    /* The ACK is already present when this event is handled. Running an old
     * batch's timeout first would discard the user's remaining wheel input. */
    ph_scroll_ack_at(&f.scroll, old_id, 3101);
    CHECK(!f.scroll.failed && f.scroll.in_flight);
    CHECK(f.count > first_count && f.last_sync != old_id && f.syncs == 2);
    CHECK(wheel_total(&f) == 2 && f.scroll.pending_y == -2);
    CHECK(f.scroll.sent_at == 3101 && f.scroll.now_ms == 3101);
    ph_scroll_tick(&f.scroll, 3101);
    CHECK(!f.scroll.failed && f.scroll.pending_y == -2);
    ph_scroll_tick(&f.scroll, 6100);
    CHECK(!f.scroll.failed); /* The follow-up owns a full, unchanged deadline. */
    ph_scroll_tick(&f.scroll, 6101);
    CHECK(f.scroll.failed && f.scroll.in_flight && !f.scroll.pending_y);

    /* Unrelated pointer fences update time without pumping or timing out a
     * wheel batch. Its own ACK can then retire the old work before the tick. */
    init(&f);
    ph_scroll_tick(&f.scroll, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -2));
    old_id = f.last_sync;
    first_count = f.count;
    ph_scroll_ack_at(&f.scroll, old_id + 1, 3101);
    CHECK(!f.scroll.failed && f.scroll.in_flight && f.scroll.pending_y == -1);
    CHECK(f.count == first_count && f.last_sync == old_id && f.scroll.sent_at == 100);
    ph_scroll_ack_at(&f.scroll, old_id, 3110);
    CHECK(f.scroll.sent_at == 3110 && f.count > first_count && !f.scroll.pending_y);
    ph_scroll_tick(&f.scroll, 3110);
    CHECK(!f.scroll.failed);

    /* Video feedback waits from the actual ACK, rather than accepting an
     * earlier frame or immediately expiring a wait based on a stale clock. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, .3, .6, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -2));
    old_id = f.last_sync;
    first_count = f.count;
    ph_scroll_ack_at(&f.scroll, old_id, 3101);
    CHECK(f.scroll.waiting_frame && !f.scroll.failed && f.scroll.pending_y == -1);
    CHECK(f.scroll.wait_since == 3101 && f.count == first_count);
    ph_scroll_tick(&f.scroll, 3101);
    CHECK(f.scroll.waiting_frame && f.count == first_count);
    ph_scroll_observe(&f.scroll, true, .31, .61, 3110);
    CHECK(f.scroll.waiting_frame && f.count == first_count);
    ph_scroll_observe(&f.scroll, true, .32, .62, 3140);
    CHECK(f.scroll.in_flight && !f.scroll.waiting_frame && f.count > first_count);
    CHECK(f.scroll.sent_at == 3140);
}

static void dynamic_content_sequences(void)
{
    for (int direction = -1; direction <= 1; direction += 2) {
        fixture f;
        init(&f);
        f.scroll.feedback = true;
        double old_top = direction < 0 ? .62 : .10;
        double old_bottom = direction < 0 ? .90 : .38;
        double grown_top = direction < 0 ? .68 : .12;
        double grown_bottom = direction < 0 ? .88 : .32;
        double next_top = direction < 0 ? .70 : .10;
        double next_bottom = direction < 0 ? .90 : .30;
        double end_top = direction < 0 ? .74 : .06;
        double end_bottom = direction < 0 ? .94 : .26;
        ph_scroll_observe(&f.scroll, true, old_top, old_bottom, 100);
        CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, direction * 4));
        ph_scroll_ack_at(&f.scroll, f.last_sync, 140);
        /* Content extension shortens the thumb by moving BOTH ends inward.
         * It is advancing toward the requested end while moving away from the
         * previously visible end, rather than stretching against that end. */
        ph_scroll_observe(&f.scroll, true, grown_top, grown_bottom, 180);
        CHECK(!f.scroll.blocked_y && f.scroll.in_flight && f.scroll.pending_y == direction * 2);
        CHECK(fabs(f.scroll.max_bar_length - .20) < .001 && f.syncs == 2);
        ph_scroll_ack_at(&f.scroll, f.last_sync, 220);
        ph_scroll_observe(&f.scroll, true, next_top, next_bottom, 260);
        CHECK(!f.scroll.blocked_y && f.scroll.in_flight && f.scroll.pending_y == direction);
        CHECK(fabs(f.scroll.max_bar_length - .20) < .001 && f.syncs == 3);

        /* The same uninterrupted gesture must still stop when the new, full
         * thumb actually remains pinned at its requested end for two batches. */
        CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, direction * 4));
        ph_scroll_ack_at(&f.scroll, f.last_sync, 300);
        ph_scroll_observe(&f.scroll, true, end_top, end_bottom, 340);
        ph_scroll_ack_at(&f.scroll, f.last_sync, 380);
        ph_scroll_observe(&f.scroll, true, end_top, end_bottom, 420);
        CHECK(!f.scroll.blocked_y && f.scroll.in_flight);
        ph_scroll_ack_at(&f.scroll, f.last_sync, 460);
        size_t before_stop = f.count;
        ph_scroll_observe(&f.scroll, true, end_top, end_bottom, 500);
        CHECK(f.scroll.blocked_y == direction && !f.scroll.pending_y && f.count == before_stop);

        /* An elastic animation can initially look like the same inward
         * contraction. If it then compresses against its fixed new end, the
         * next small probe must stop it rather than rebasing indefinitely. */
        init(&f);
        f.scroll.feedback = true;
        ph_scroll_observe(&f.scroll, true, old_top, old_bottom, 100);
        CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, direction * 4));
        ph_scroll_ack_at(&f.scroll, f.last_sync, 140);
        ph_scroll_observe(&f.scroll, true, grown_top, grown_bottom, 180);
        CHECK(!f.scroll.blocked_y && abs(wheel_total(&f)) == 2);
        ph_scroll_ack_at(&f.scroll, f.last_sync, 220);
        before_stop = f.count;
        ph_scroll_observe(&f.scroll, true,
                          direction < 0 ? grown_top + .02 : grown_top,
                          direction < 0 ? grown_bottom : grown_bottom - .02, 260);
        CHECK(f.scroll.blocked_y == direction && !f.scroll.pending_y
              && f.count == before_stop && abs(wheel_total(&f)) == 2);
    }
}

int main(void)
{
    timed_ack_sequences();
    dynamic_content_sequences();
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
    ph_scroll_transport_reset(&f.scroll);
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
    CHECK(!f.scroll.positioned && f.scroll.in_flight && f.scroll.cancelled);
    f.count = 0;
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -1));
    CHECK(f.count == 0); /* calibration cannot overtake the old transport batch */
    ph_scroll_ack(&f.scroll, f.last_sync);
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
    /* The rebound can move the thumb off its learned minimum while the same
     * wheel burst is still arriving. It must not reopen the stopped direction. */
    ph_scroll_observe(&f.scroll, true, 135.0 / 1080, 466.0 / 1080, 160);
    CHECK(f.scroll.blocked_y == 1);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, 100));
    ph_scroll_observe(&f.scroll, false, 0, 0, 200);
    ph_scroll_tick(&f.scroll, 300);
    CHECK(f.count == first_count);
    /* Repositioning inside the same page does not forget that it is at an edge. */
    CHECK(ph_scroll_request(&f.scroll, 0.7, 0.5, 0.46, 0, 100));
    CHECK(f.count == first_count && f.scroll.blocked_y == 1);
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
    CHECK(f.scroll.blocked_y == 0); /* one delayed picture cannot prove an edge */
    CHECK(ph_scroll_request(&f.scroll, .5, .65, .46, 0, 1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    ph_scroll_observe(&f.scroll, true, 121.0 / 1080, 452.0 / 1080, 180);
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
    ph_scroll_observe(&f.scroll, true, 668.0 / 1080, 999.0 / 1080, 160);
    CHECK(f.scroll.blocked_y == -1);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, -40));
    CHECK(f.count == first_count);
    CHECK(ph_scroll_request(&f.scroll, 0.5, 0.65, 0.46, 0, 1));
    CHECK(f.count > first_count);

    /* A large title expands near the top: the indicator's track starts lower
     * than its earlier minimum. A shorter thumb at that new origin is still an
     * edge, not permission to keep pushing into the rubber-band animation. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, 121.0 / 1080, 452.0 / 1080, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, 1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    first_count = f.count;
    ph_scroll_observe(&f.scroll, true, 180.0 / 1080, 490.0 / 1080, 140);
    CHECK(f.scroll.blocked_y == 1);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, 40));
    CHECK(f.count == first_count);

    /* The bottom track can move inward too (for example, a bottom toolbar).
     * Its compressed indicator must not have to reach an obsolete maximum. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, 684.0 / 1080, 1015.0 / 1080, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    CHECK(wheel_total(&f) == 1);
    ph_scroll_ack(&f.scroll, f.last_sync);
    first_count = f.count;
    ph_scroll_observe(&f.scroll, true, 700.0 / 1080, 990.0 / 1080, 140);
    CHECK(f.scroll.blocked_y == -1);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -40));
    CHECK(f.count == first_count);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 1, 0));
    CHECK(f.count > first_count && pan_total(&f) == 1 && f.scroll.blocked_y == -1);
    ph_scroll_reset(&f.scroll);
    CHECK(!f.scroll.blocked_y && !f.scroll.have_bar);

    /* Cancellation drops intent, not already queued transport work. Repeated
     * cursor changes cannot flood replacement batches before the old ACK. */
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    old_id = f.last_sync;
    first_count = f.count;
    for (int i = 0; i < 100; i++) {
        ph_scroll_cancel(&f.scroll);
        CHECK(ph_scroll_request(&f.scroll, i % 2 ? .2 : .8, .5, .46, 0, -1));
        CHECK(f.count == first_count && f.last_sync == old_id);
        CHECK(f.scroll.in_flight && f.scroll.pending_y == -1);
    }
    ph_scroll_ack(&f.scroll, old_id);
    CHECK(f.count > first_count && f.last_sync != old_id);
    CHECK(f.scroll.in_flight && !f.scroll.pending_y);
    first_count = f.count;
    ph_scroll_ack(&f.scroll, old_id);
    CHECK(f.count == first_count && f.scroll.in_flight);

    /* A missing ACK stays bounded; changed coordinates cannot bypass it. A
     * late fence or explicit reconnect reset permits a fresh gesture. */
    init(&f);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    old_id = f.last_sync;
    first_count = f.count;
    ph_scroll_cancel(&f.scroll);
    ph_scroll_tick(&f.scroll, 3001);
    CHECK(f.scroll.failed && f.scroll.in_flight);
    CHECK(!ph_scroll_request(&f.scroll, .8, .5, .46, 0, -1));
    CHECK(f.count == first_count);
    ph_scroll_ack(&f.scroll, old_id);
    CHECK(!f.scroll.failed && !f.scroll.in_flight && f.count == first_count);
    CHECK(ph_scroll_request(&f.scroll, .8, .5, .46, 0, -1));
    CHECK(f.count > first_count);
    ph_scroll_transport_reset(&f.scroll);
    CHECK(!f.scroll.in_flight && !f.scroll.failed);

    init(&f);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -4));
    first_count = f.count;
    CHECK(f.scroll.pending_y == -3);
    ph_scroll_tick(&f.scroll, 3001);
    CHECK(f.scroll.failed && !f.scroll.pending_y && f.count == first_count);
    ph_scroll_ack(&f.scroll, f.last_sync);
    CHECK(!f.scroll.failed && !f.scroll.in_flight && f.count == first_count);

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

    /* A delayed picture near the lower screen is followed by real progress.
     * One unchanged frame must not latch the remainder of a long list. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, .62, .9, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    ph_scroll_observe(&f.scroll, true, .62, .9, 140);
    CHECK(!f.scroll.blocked_y);
    ph_scroll_observe(&f.scroll, true, .64, .92, 160);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    ph_scroll_observe(&f.scroll, true, .66, .94, 200);
    CHECK(!f.scroll.blocked_y);

    /* Navigation on the phone and newly discovered Bluetooth devices can change
     * the thumb length. A new gesture must not reuse another page's maximum. */
    init(&f);
    f.scroll.feedback = true;
    ph_scroll_observe(&f.scroll, true, .4, .95, 100);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -1));
    ph_scroll_ack(&f.scroll, f.last_sync);
    ph_scroll_observe(&f.scroll, true, .6, .95, 140);
    CHECK(f.scroll.blocked_y == -1);
    first_count = f.count;
    int before_wheel = wheel_total(&f);
    ph_scroll_tick(&f.scroll, 1000);
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, -4));
    CHECK(f.count > first_count && wheel_total(&f) == before_wheel + 1);
    ph_scroll_ack(&f.scroll, f.last_sync);
    ph_scroll_observe(&f.scroll, true, .65, .9, 1040);
    CHECK(!f.scroll.blocked_y && fabs(f.scroll.max_bar_length - .25) < .001);

    /* Old compression after reversal must not relatch a direction whose last
     * batch has already finished, before the new pointer target is ready. */
    ph_scroll_cancel(&f.scroll);
    f.scroll.blocked_y = -1;
    CHECK(ph_scroll_request(&f.scroll, .5, .5, .46, 0, 1));
    ph_scroll_cancel(&f.scroll);
    ph_scroll_observe(&f.scroll, true, .8, .95, 1060);
    CHECK(!f.scroll.blocked_y);

    if (failures)
        return 1;
    puts("all scroll sequences passed");
    return 0;
}
