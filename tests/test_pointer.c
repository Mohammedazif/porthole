/* Exercise the real pointer state machine against a simulated accelerated phone.
 * Feedback arrives only after transport ACKs; input can arrive before either. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "control/pointer.h"

typedef struct event {
    ph_hid_mouse mouse;
    uint64_t at;
    double x, y;
} event;

typedef struct fixture {
    ph_pointer pointer;
    event events[2048];
    size_t count;
    unsigned int anchors, syncs, motion_since_sync, max_motion_batch;
    double x, y, true_x_scale, true_y_scale, true_linear, true_quadratic;
    bool bounded_video;
    uint64_t now;
} fixture;

static int failures;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static double travel(const fixture *f, int value)
{
    double magnitude = fabs((double)value);
    double result = f->true_linear * magnitude + f->true_quadratic * magnitude * magnitude;
    return value < 0 ? -result : result;
}

static void mouse(void *user, const ph_hid_mouse *m)
{
    fixture *f = user;
    double width = 500 * fmax(1, f->pointer.aspect);
    double height = width / f->pointer.aspect;
    f->x = fmin(width - 16, fmax(16, f->x + travel(f, m->dx) * f->true_x_scale));
    f->y = fmin(height - 16, fmax(16, f->y + travel(f, m->dy) * f->true_y_scale));
    if (m->dx == -127 && m->dy == -127) {
        f->anchors++;
        CHECK(!m->buttons); /* recovery must never create a huge drag */
    }
    if (m->dx || m->dy)
        f->motion_since_sync++;
    if (f->count < sizeof f->events / sizeof f->events[0])
        f->events[f->count++] = (event){*m, f->pointer.now_ms, f->x, f->y};
    else
        CHECK(false);
}

static void delay(void *user, uint16_t ms)
{
    (void)user;
    CHECK(ms > 0 && ms <= 150);
}

static void sync_reports(void *user, uint32_t id)
{
    fixture *f = user;
    CHECK(id & 0x80000000U);
    if (f->motion_since_sync > f->max_motion_batch)
        f->max_motion_batch = f->motion_since_sync;
    f->motion_since_sync = 0;
    f->syncs++;
}

static void init(fixture *f)
{
    memset(f, 0, sizeof *f);
    f->x = 400;
    f->y = 800;
    f->true_x_scale = f->true_y_scale = 1;
    f->true_linear = .25;
    f->true_quadratic = .055;
    f->now = 100;
    f->pointer.output = (ph_pointer_output){f, mouse, delay, sync_reports};
    ph_pointer_set_enabled(&f->pointer, true, f->now);
}

static void step(fixture *f)
{
    f->now += 40;
    ph_pointer_phase phase = f->pointer.phase;
    if (phase == PH_POINTER_MOVING || phase == PH_POINTER_BUTTON
        || phase == PH_POINTER_DRAINING)
        ph_pointer_ack(&f->pointer, f->pointer.sync_id, f->now);
    else if (phase == PH_POINTER_WAIT_VIDEO) {
        double width = 500 * fmax(1, f->pointer.aspect);
        double x = f->x / width, y = f->y * f->pointer.aspect / width;
        double hx, hy, radius;
        bool found = !f->bounded_video
                     || (ph_pointer_hint(&f->pointer, &hx, &hy, &radius)
                         && fabs(x - hx) <= radius
                         && fabs(y - hy) <= radius * f->pointer.aspect);
        ph_pointer_observe(&f->pointer, found, x, y, f->now);
    }
    ph_pointer_tick(&f->pointer, f->now);
}

static void settle(fixture *f)
{
    for (int i = 0; i < 100; i++) {
        if (f->pointer.phase == PH_POINTER_IDLE && !f->pointer.count)
            return;
        step(f);
    }
    CHECK(false);
}

static size_t first_button(const fixture *f, uint8_t button)
{
    for (size_t i = 0; i < f->count; i++)
        if (f->events[i].mouse.buttons & button)
            return i;
    return f->count;
}

static void confirmed_position(fixture *f, double x, double y, double aspect)
{
    f->x = x;
    f->y = y;
    f->pointer.aspect = aspect;
    double width = 500 * fmax(1, aspect);
    f->pointer.actual_x = x / width;
    f->pointer.actual_y = y * aspect / width;
    f->pointer.known = true;
    f->pointer.observed_at = f->now;
}

static void canceled_motion_confirmation(void)
{
    /* Leaving during either transport dispatch or its video wait clears the
     * click/hover intent, but must not throw away an already-sent endpoint. */
    for (int acknowledged = 0; acknowledged < 2; acknowledged++) {
        fixture f;
        init(&f);
        confirmed_position(&f, 250, 540, .46);
        CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true,
                                 .75, .7, .46, ++f.now));
        uint32_t id = f.pointer.sync_id;
        if (acknowledged)
            ph_pointer_ack(&f.pointer, id, f.now += 40);
        size_t sent = f.count;
        ph_pointer_cancel(&f.pointer, ++f.now);
        CHECK(!f.pointer.active && !f.pointer.count && !f.pointer.requested_buttons);
        CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count);
        if (!acknowledged)
            ph_pointer_ack(&f.pointer, id, f.now += 40);
        CHECK(f.pointer.phase == PH_POINTER_WAIT_VIDEO && !f.pointer.known);
        CHECK(f.count == sent && f.anchors == 0);
        /* No post-ACK settling interval has elapsed. The old picture cannot
         * confirm the endpoint, even though cancellation cleared the click. */
        ph_pointer_observe(&f.pointer, true, .5, .5, f.pointer.wait_since - 1);
        CHECK(!f.pointer.known && f.count == sent);
        ph_pointer_observe(&f.pointer, true, .5, .5, f.now + 1);
        CHECK(!f.pointer.known && f.count == sent);
        f.now += 40;
        ph_pointer_observe(&f.pointer, true, f.x / 500, f.y * .46 / 500, f.now);
        CHECK(f.pointer.known && f.pointer.phase == PH_POINTER_IDLE);
        CHECK(f.count == sent && !f.pointer.active); /* no cancelled-target follow-up */
        CHECK(ph_pointer_target(&f.pointer, .4, .5, .46, ++f.now));
        settle(&f);
        CHECK(ph_pointer_ready(&f.pointer, .4, .5, .46) && f.anchors == 0);
        CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count);
    }

    /* With no video while outside the window, expiry forgets the endpoint
     * quietly. Cancellation must never reveal/probe/move the inactive phone. */
    fixture f;
    init(&f);
    confirmed_position(&f, 250, 540, .46);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, ++f.now));
    ph_pointer_cancel(&f.pointer, ++f.now);
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now += 40);
    size_t sent = f.count;
    ph_pointer_tick(&f.pointer, f.now += 701);
    ph_pointer_tick(&f.pointer, f.now += 4000);
    CHECK(f.count == sent && !f.pointer.known && !f.pointer.active);
    CHECK(f.pointer.phase == PH_POINTER_IDLE && !ph_pointer_transport_stalled(&f.pointer));

    /* A second leave during that same confirmation must keep the original
     * feedback instead of silently turning re-entry into another corner sweep. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, f.now));
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now += 40);
    for (int i = 0; i < 20; i++) {
        ph_pointer_cancel(&f.pointer, ++f.now);
        CHECK(ph_pointer_target(&f.pointer, i % 2 ? .4 : .6, .5, .46, ++f.now));
        CHECK(f.pointer.phase == PH_POINTER_WAIT_VIDEO && f.anchors == 8);
    }
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .4, .5, .46) && f.anchors == 8);
}

static void canceled_motion_does_not_calibrate(void)
{
    /* Reproduce the live canceled sweep: its first stable image was (25,158),
     * versus a planned endpoint near (16,16). It is useful position feedback,
     * but must not turn Y gain 4.42 into 2.01 for the next long movement. */
    const double aspect = 500.0 / 1080;
    const double scale_x = 94.0 / 25.785, scale_y = 52.0 / 11.76;
    for (int acknowledged = 0; acknowledged < 2; acknowledged++) {
        for (int canceled = 0; canceled < 2; canceled++) {
            fixture f;
            init(&f);
            CHECK(ph_pointer_set_scale(&f.pointer, scale_x, scale_y));
            confirmed_position(&f, 162, 275, aspect);
            CHECK(ph_pointer_target(&f.pointer, 16.0 / 500, 16.0 / 1080, aspect, ++f.now));
            CHECK(f.pointer.learn_move && f.pointer.phase == PH_POINTER_MOVING);
            uint32_t id = f.pointer.sync_id;
            size_t sent = f.count;
            if (acknowledged)
                ph_pointer_ack(&f.pointer, id, f.now += 40);
            if (canceled)
                ph_pointer_cancel(&f.pointer, ++f.now);
            if (!acknowledged)
                ph_pointer_ack(&f.pointer, id, f.now += 40);
            CHECK(f.pointer.phase == PH_POINTER_WAIT_VIDEO);
            ph_pointer_observe(&f.pointer, true, 25.0 / 500, 158.0 / 1080, f.now += 42);
            CHECK(f.pointer.known);
            CHECK(fabs(f.pointer.actual_x - 25.0 / 500) < 1e-12);
            CHECK(fabs(f.pointer.actual_y - 158.0 / 1080) < 1e-12);
            if (canceled) {
                CHECK(f.pointer.scale_x == scale_x && f.pointer.scale_y == scale_y);
                CHECK(!f.pointer.learn_move && !f.pointer.active && !f.pointer.count);
                CHECK(f.pointer.phase == PH_POINTER_IDLE && f.count == sent);
            } else {
                /* Uninterrupted verified hover movement still calibrates. */
                CHECK(fabs(f.pointer.scale_x - 137.0 / 39.92) < 1e-12);
                CHECK(fabs(f.pointer.scale_y - 117.0 / 58.305) < 1e-12);
                CHECK(f.pointer.active && f.count > sent);
            }
        }
    }
}

int main(void)
{
    canceled_motion_does_not_calibrate();
    canceled_motion_confirmation();
    fixture f;
    init(&f);
    /* Motion before the first ACK coalesces. There is just one initial anchor. */
    CHECK(ph_pointer_target(&f.pointer, .5, .65, .46, f.now));
    size_t initial = f.count;
    CHECK(f.anchors == 8);
    CHECK(ph_pointer_target(&f.pointer, .7, .5, .46, ++f.now));
    CHECK(ph_pointer_target(&f.pointer, .8, .6, .46, ++f.now));
    CHECK(f.count == initial);
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .8, .6, .46));
    CHECK(f.anchors == 8 && !f.pointer.failed);
    CHECK(hypot(f.x - 400, f.y - .6 * 500 / .46) <= 4);

    /* No click can leave the queue before fresh position feedback confirms it. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .65, .46, f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .5, .65, .46, ++f.now));
    CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count);
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now + 20);
    ph_pointer_observe(&f.pointer, true, .032, .01472, f.now + 30);
    CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count); /* frame is too early */
    f.now += 30;
    settle(&f);
    size_t pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count);
    CHECK(hypot(f.events[pressed].x - 250, f.events[pressed].y - .65 * 500 / .46) <= 4);
    CHECK(f.events[f.count - 1].mouse.buttons == 0 && !f.pointer.buttons);
    CHECK(f.events[f.count - 1].at - f.events[pressed].at >= 80);

    /* A down and far-away up arriving before alignment still form a drag from
     * the original press location, never a tap at the final location. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .3, .3, .46, f.now));
    CHECK(ph_pointer_target(&f.pointer, .7, .7, .46, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .8, .8, .46, ++f.now));
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count);
    CHECK(hypot(f.events[pressed].x - 150, f.events[pressed].y - .3 * 500 / .46) <= 4);
    bool dragged = false;
    for (size_t i = pressed + 1; i < f.count; i++) {
        if (f.events[i].mouse.dx || f.events[i].mouse.dy) {
            CHECK(f.events[i].mouse.buttons == PH_HID_BTN_LEFT);
            dragged = true;
        }
    }
    CHECK(dragged && !f.pointer.buttons && f.anchors == 8);
    CHECK(hypot(f.x - 400, f.y - .8 * 500 / .46) <= 4);

    /* App icons move during a swipe. An off-path circle, including one within
     * the endpoint search region, must not steer or recalibrate a held drag. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .8, 700.0 * .46 / 500,
                             .46, f.now));
    settle(&f);
    double before_x_scale = f.pointer.scale_x, before_y_scale = f.pointer.scale_y;
    CHECK(ph_pointer_target(&f.pointer, .2, 700.0 * .46 / 500, .46, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .2,
                             700.0 * .46 / 500, .46, ++f.now));
    double hint_x, hint_y, hint_radius;
    CHECK(ph_pointer_hint(&f.pointer, &hint_x, &hint_y, &hint_radius));
    CHECK(hint_radius <= 40.0 / 500);
    f.now += 40;
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now);
    initial = f.count;
    f.now += 40;
    ph_pointer_observe(&f.pointer, true, .61, 805.0 * .46 / 500, f.now);
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_WAIT_VIDEO);
    f.now += 40;
    ph_pointer_observe(&f.pointer, true, .2, 724.0 * .46 / 500, f.now);
    CHECK(f.count == initial && f.pointer.buttons == PH_HID_BTN_LEFT);
    f.now += 40;
    ph_pointer_observe(&f.pointer, true, f.x / 500, f.y * .46 / 500, f.now);
    CHECK(f.pointer.buttons == 0 && f.pointer.phase == PH_POINTER_BUTTON);
    CHECK(f.pointer.scale_x == before_x_scale && f.pointer.scale_y == before_y_scale);
    CHECK(f.anchors == 8);
    settle(&f);

    /* Releasing a stationary long-held button never needs a new pointer reveal,
     * even if the old video observation has aged out. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, f.now));
    settle(&f);
    f.now += 1600;
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .5, .5, .46, f.now));
    CHECK(f.pointer.phase == PH_POINTER_BUTTON && !f.pointer.buttons);
    CHECK(f.events[f.count - 1].mouse.dx == 0 && f.events[f.count - 1].mouse.dy == 0);

    /* A stale calibration learns actual travel, then corrects before clicking. */
    init(&f);
    CHECK(ph_pointer_set_scale(&f.pointer, 2, 2));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_RIGHT, true, .55, .7, .46, f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_RIGHT, false, .55, .7, .46, ++f.now));
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_RIGHT);
    CHECK(pressed < f.count && !f.pointer.failed);
    CHECK(hypot(f.events[pressed].x - 275, f.events[pressed].y - .7 * 500 / .46) <= 4);
    CHECK(fabs(f.pointer.scale_x - 1) < .02 && fabs(f.pointer.scale_y - 1) < .02);

    /* Missing video drops queued clicks rather than falling back to an estimate. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, f.now));
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, 200);
    ph_pointer_observe(&f.pointer, false, 0, 0, 500);
    ph_pointer_tick(&f.pointer, 901);
    CHECK(f.pointer.failed && !f.pointer.count && !f.pointer.buttons);
    CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count);
    initial = f.count;
    CHECK(!ph_pointer_target(&f.pointer, .5, .5, .46, 1500));
    ph_pointer_tick(&f.pointer, 3000);
    CHECK(f.count == initial); /* repeated failed wheel polling cannot sweep again */

    /* A one-unit correction may not produce a detectable changed-video dot.
     * Keep an unpressed click across one neutral reveal, but never send its
     * press until the NEW probe fence and a fresh aligned observation arrive. */
    init(&f);
    f.true_linear = 1.2;
    f.true_quadratic = .04;
    CHECK(ph_pointer_set_scale(&f.pointer, 20, 20));
    confirmed_position(&f, 243, 810, .46);
    const double tiny_target = 247.1 / 500;
    const double tiny_y = 810 * .46 / 500;
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true,
                             tiny_target, tiny_y, .46, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false,
                             tiny_target, tiny_y, .46, ++f.now));
    CHECK(f.count == 1 && f.events[0].mouse.dx == 1 && !f.events[0].mouse.dy);
    uint32_t tiny_id = f.pointer.sync_id;
    ph_pointer_ack(&f.pointer, tiny_id, f.now += 20);
    ph_pointer_observe(&f.pointer, false, 0, 0, f.now += 40);
    CHECK(f.pointer.phase == PH_POINTER_WAIT_VIDEO && f.pointer.count == 2);
    ph_pointer_tick(&f.pointer, f.now += 661);
    CHECK(!f.pointer.failed && f.pointer.recoveries == 1 && f.pointer.count == 2);
    CHECK(f.count == 2 && f.events[1].mouse.dx == 2 && !f.events[1].mouse.dy);
    CHECK(!f.events[1].mouse.buttons && !f.pointer.buttons);
    CHECK(f.pointer.phase == PH_POINTER_MOVING && f.pointer.sync_id != tiny_id);
    CHECK(!ph_pointer_transport_stalled(&f.pointer) && f.anchors == 0);
    uint32_t probe_id = f.pointer.sync_id;
    ph_pointer_ack(&f.pointer, tiny_id, ++f.now); /* obsolete ACK cannot unlock press */
    ph_pointer_observe(&f.pointer, true, f.x / 500, f.y * .46 / 500, ++f.now);
    CHECK(f.pointer.phase == PH_POINTER_MOVING && first_button(&f, PH_HID_BTN_LEFT) == f.count);
    ph_pointer_ack(&f.pointer, probe_id, f.now += 20);
    ph_pointer_observe(&f.pointer, false, 0, 0, f.now += 40);
    CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count && f.pointer.count == 2);
    ph_pointer_observe(&f.pointer, true, f.x / 500, f.y * .46 / 500, ++f.now);
    pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count && hypot(f.events[pressed].x - 247.1,
                                     f.events[pressed].y - 810) <= 4);
    settle(&f);
    CHECK(!f.pointer.buttons && !f.pointer.count && !f.pointer.failed);
    CHECK(f.events[f.count - 1].at - f.events[pressed].at >= 80);

    /* A frozen image gets exactly one neutral attempt. Hover target updates
     * cannot replenish that pending click's allowance; no press is guessed and
     * video failure must not be confused with failure of its healthy ACK pipe. */
    init(&f);
    CHECK(ph_pointer_set_scale(&f.pointer, 20, 20));
    confirmed_position(&f, 243, 810, .46);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true,
                             tiny_target, tiny_y, .46, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false,
                             tiny_target, tiny_y, .46, ++f.now));
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, ++f.now);
    ph_pointer_tick(&f.pointer, f.now += 701);
    CHECK(f.pointer.recoveries == 1 && f.pointer.phase == PH_POINTER_MOVING);
    initial = f.count;
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, ++f.now);
    for (int i = 0; i < 10; i++) {
        CHECK(ph_pointer_target(&f.pointer, i % 2 ? .2 : .8, .5, .46, ++f.now));
        CHECK(f.pointer.recoveries == 1 && f.count == initial);
    }
    ph_pointer_tick(&f.pointer, f.now += 701);
    CHECK(f.pointer.failed && !f.pointer.count && !f.pointer.buttons);
    CHECK(f.count == initial && first_button(&f, PH_HID_BTN_LEFT) == f.count);
    CHECK(!ph_pointer_transport_stalled(&f.pointer));

    /* Failure to ACK a reveal is a real transport timeout. It never emits a
     * second reveal or a queued click while waiting on that missing fence. */
    init(&f);
    CHECK(ph_pointer_set_scale(&f.pointer, 20, 20));
    confirmed_position(&f, 243, 810, .46);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true,
                             tiny_target, tiny_y, .46, ++f.now));
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, ++f.now);
    ph_pointer_tick(&f.pointer, f.now += 701);
    initial = f.count;
    ph_pointer_tick(&f.pointer, f.now += 3000);
    CHECK(f.pointer.failed && !f.pointer.count && !f.pointer.buttons);
    CHECK(f.count == initial && first_button(&f, PH_HID_BTN_LEFT) == f.count);
    CHECK(ph_pointer_transport_stalled(&f.pointer));

    /* A full event queue fails by releasing everything, not by losing one up. */
    init(&f);
    for (unsigned int i = 0; i < PH_POINTER_QUEUE; i++)
        CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, (i % 2) == 0,
                                 .5, .5, .46, ++f.now));
    CHECK(!ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true,
                              .5, .5, .46, ++f.now));
    CHECK(f.pointer.failed && !f.pointer.count && !f.pointer.requested_buttons);
    CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count);

    /* Focus loss releases a drag and drains the cancelled movement before the
     * new target can issue reports. The old movement ACK cannot finish release. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, f.now));
    settle(&f);
    CHECK(f.pointer.buttons == PH_HID_BTN_LEFT);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, ++f.now));
    uint32_t stale_id = f.pointer.sync_id;
    ph_pointer_cancel(&f.pointer, ++f.now);
    CHECK(!f.pointer.buttons && !f.pointer.known && !f.pointer.count);
    CHECK(f.events[f.count - 1].mouse.buttons == 0);
    uint32_t release_id = f.pointer.sync_id;
    initial = f.count;
    CHECK(ph_pointer_target(&f.pointer, .4, .5, .46, ++f.now));
    CHECK(f.pointer.sync_id == release_id && release_id != stale_id);
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_DRAINING);
    ph_pointer_ack(&f.pointer, stale_id, ++f.now);
    CHECK(f.pointer.phase == PH_POINTER_DRAINING && f.count == initial);
    ph_pointer_ack(&f.pointer, release_id, ++f.now);
    CHECK(f.pointer.phase == PH_POINTER_MOVING && f.anchors == 16);
    CHECK(f.pointer.sync_id != release_id);

    /* A missing transport ACK while dragging also releases the physical button. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, f.now));
    settle(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, ++f.now));
    stale_id = f.pointer.sync_id;
    ph_pointer_ack(&f.pointer, 1, ++f.now); /* unrelated scroll ACK */
    CHECK(f.pointer.phase == PH_POINTER_MOVING);
    ph_pointer_tick(&f.pointer, f.now + 3001);
    CHECK(f.pointer.failed && !f.pointer.buttons && !f.pointer.requested_buttons);
    CHECK(f.events[f.count - 1].mouse.buttons == 0);
    CHECK(ph_pointer_transport_stalled(&f.pointer)); /* still true after the fresh release fence */
    initial = f.count;
    ph_pointer_ack(&f.pointer, stale_id, f.now + 3100);
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_DRAINING);
    CHECK(ph_pointer_transport_stalled(&f.pointer)); /* old move ACK cannot settle the release */
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now + 3200);
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_IDLE);
    CHECK(!ph_pointer_transport_stalled(&f.pointer));

    /* Video confirmation failure during a drag emits a fresh release fence.
     * Generic failed+DRAINING used to make the app kill the healthy helper
     * before that release's ACK could arrive a few milliseconds later. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, f.now));
    settle(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, ++f.now));
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, ++f.now);
    CHECK(f.pointer.phase == PH_POINTER_WAIT_VIDEO && f.pointer.buttons == PH_HID_BTN_LEFT);
    ph_pointer_tick(&f.pointer, f.now += 701);
    CHECK(f.pointer.failed && f.pointer.phase == PH_POINTER_DRAINING && !f.pointer.buttons);
    CHECK(!ph_pointer_transport_stalled(&f.pointer) && f.pointer.sent_at == f.now);
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now += 17);
    CHECK(f.pointer.phase == PH_POINTER_IDLE && !ph_pointer_transport_stalled(&f.pointer));

    /* A future timestamp must not underflow into a fictitious three-second
     * stall, including target/button queries while draining a cancellation. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, f.now));
    ph_pointer_cancel(&f.pointer, ++f.now);
    uint64_t sent_at = f.pointer.sent_at;
    ph_pointer_tick(&f.pointer, sent_at - 1);
    CHECK(!ph_pointer_transport_stalled(&f.pointer));
    CHECK(ph_pointer_target(&f.pointer, .4, .5, .46, sent_at - 1));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .4, .5, .46, sent_at - 1));
    CHECK(!ph_pointer_transport_stalled(&f.pointer));
    ph_pointer_tick(&f.pointer, sent_at + 3000);
    CHECK(ph_pointer_transport_stalled(&f.pointer));
    ph_pointer_cancel(&f.pointer, sent_at + 3001);
    ph_pointer_reset(&f.pointer, sent_at + 3002);
    ph_pointer_set_enabled(&f.pointer, false, sent_at + 3003);
    CHECK(ph_pointer_transport_stalled(&f.pointer)); /* intent changes cannot erase a real fault */
    ph_pointer_transport_reset(&f.pointer, sent_at + 3004);
    CHECK(!ph_pointer_transport_stalled(&f.pointer));

    /* A real sender dispatches asynchronously. Repeated leave/re-enter events
     * must not enqueue a fresh corner sweep for every cancellation while the
     * first sweep is still queued. New motion coalesces behind its ACK. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, f.now));
    stale_id = f.pointer.sync_id;
    initial = f.count;
    for (int i = 0; i < 100; i++) {
        ph_pointer_cancel(&f.pointer, ++f.now);
        CHECK(ph_pointer_target(&f.pointer, i % 2 ? .2 : .8, .7, .46, ++f.now));
        ph_pointer_tick(&f.pointer, f.now);
        CHECK(f.count == initial && f.anchors == 8);
        CHECK(f.pointer.sync_id == stale_id && f.pointer.phase == PH_POINTER_DRAINING);
    }
    ph_pointer_ack(&f.pointer, stale_id, ++f.now);
    CHECK(f.anchors == 8 && f.pointer.phase == PH_POINTER_WAIT_VIDEO);
    uint32_t next_id = f.pointer.sync_id;
    ph_pointer_ack(&f.pointer, stale_id, ++f.now);
    CHECK(f.pointer.sync_id == next_id && f.pointer.phase == PH_POINTER_WAIT_VIDEO);
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .2, .7, .46));

    /* A cancelled drag's release is itself fenced. A click queued meanwhile
     * cannot run until that release, re-anchoring and fresh video all finish. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, f.now));
    settle(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, ++f.now));
    stale_id = f.pointer.sync_id;
    ph_pointer_cancel(&f.pointer, ++f.now);
    release_id = f.pointer.sync_id;
    initial = f.count;
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .4, .5, .46, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .4, .5, .46, ++f.now));
    CHECK(f.pointer.count == 2 && f.count == initial && !f.pointer.buttons);
    ph_pointer_ack(&f.pointer, stale_id, ++f.now);
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_DRAINING);
    ph_pointer_ack(&f.pointer, release_id, ++f.now);
    settle(&f);
    bool clicked_after_release = false;
    for (size_t i = initial; i < f.count; i++) {
        if (f.events[i].mouse.buttons == PH_HID_BTN_LEFT) {
            clicked_after_release = true;
            CHECK(hypot(f.events[i].x - 200, f.events[i].y - .5 * 500 / .46) <= 4);
        }
    }
    CHECK(clicked_after_release && !f.pointer.buttons && !f.pointer.count);

    /* Geometry resets also retain the fence; only transport_reset may forget
     * commands that still exist in the helper's input queue. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, f.now));
    stale_id = f.pointer.sync_id;
    initial = f.count;
    ph_pointer_reset(&f.pointer, ++f.now);
    CHECK(ph_pointer_target(&f.pointer, .4, .5, 2.16, ++f.now));
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_DRAINING);
    ph_pointer_ack(&f.pointer, stale_id, ++f.now);
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .4, .5, 2.16));

    /* A broken transport never becomes an unbounded retry queue just because
     * targets change. A link reset can explicitly discard its missing ACK. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, f.now));
    initial = f.count;
    ph_pointer_cancel(&f.pointer, ++f.now);
    ph_pointer_tick(&f.pointer, f.now += 3001);
    CHECK(f.pointer.failed && f.pointer.phase == PH_POINTER_DRAINING);
    for (int i = 0; i < 20; i++)
        CHECK(!ph_pointer_target(&f.pointer, i % 2 ? .2 : .8, .7, .46, ++f.now));
    CHECK(f.count == initial);
    ph_pointer_set_enabled(&f.pointer, false, ++f.now);
    ph_pointer_transport_reset(&f.pointer, ++f.now);
    ph_pointer_set_enabled(&f.pointer, true, ++f.now);
    CHECK(ph_pointer_target(&f.pointer, .4, .5, .46, ++f.now));
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .4, .5, .46));

    /* Same-target wheel routing after idle can reveal the hidden pointer and
     * become ready again; it cannot wait forever on an observation that aged out. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .5, .5, .46, f.now));
    settle(&f);
    f.now += 1500;
    ph_pointer_tick(&f.pointer, f.now);
    CHECK(!ph_pointer_ready(&f.pointer, .5, .5, .46));
    CHECK(ph_pointer_target(&f.pointer, .5, .5, .46, f.now));
    CHECK(f.pointer.phase == PH_POINTER_MOVING);
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .5, .5, .46));

    /* The native cursor centre cannot reach the outside 16 model pixels. Edge
     * requests converge to its closest representable point, and wheel routing
     * agrees with the same normalization rather than stalling forever. */
    const double edge_targets[][2] = {{0, 0}, {.999, .999}, {0, .5}, {.5, .999}};
    for (size_t i = 0; i < sizeof edge_targets / sizeof edge_targets[0]; i++) {
        init(&f);
        double ex = edge_targets[i][0], ey = edge_targets[i][1];
        CHECK(ph_pointer_target(&f.pointer, ex, ey, .46, f.now));
        settle(&f);
        CHECK(ph_pointer_ready(&f.pointer, ex, ey, .46));
        CHECK(!f.pointer.failed && f.anchors == 8);
    }
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, 0, 0, .46, f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, 0, 0, .46, ++f.now));
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count && fabs(f.events[pressed].x - 16) < 1
          && fabs(f.events[pressed].y - 16) < 1);
    CHECK(!f.pointer.buttons && !f.pointer.failed);

    /* The same model units apply after rotation: landscape has width1080 and
     * height500, rather than shrinking the physical movement into a500px width. */
    const double landscape = 2.16;
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .8, .6, landscape, f.now));
    double hx, hy, hr;
    CHECK(ph_pointer_hint(&f.pointer, &hx, &hy, &hr));
    CHECK(fabs(hx - 16.0 / 1080) < .000001 && fabs(hy - 16.0 / 500) < .000001
          && fabs(hr - 40.0 / 1080) < .000001);
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .8, .6, landscape));
    CHECK(hypot(f.x - .8 * 1080, f.y - .6 * 500) <= 4);
    CHECK(fabs(f.pointer.scale_x - 1) < .02 && fabs(f.pointer.scale_y - 1) < .02);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .7, .4,
                             landscape, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .7, .4,
                             landscape, ++f.now));
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count
          && hypot(f.events[pressed].x - .7 * 1080, f.events[pressed].y - .4 * 500) <= 4);
    for (size_t i = 0; i < sizeof edge_targets / sizeof edge_targets[0]; i++) {
        init(&f);
        double ex = edge_targets[i][0], ey = edge_targets[i][1];
        CHECK(ph_pointer_target(&f.pointer, ex, ey, landscape, f.now));
        settle(&f);
        CHECK(ph_pointer_ready(&f.pointer, ex, ey, landscape));
        CHECK(!f.pointer.failed && f.anchors == 8);
        if (ex == 0)
            CHECK(fabs(f.x - 16) <= 4);
        if (ey > .99)
            CHECK(fabs(f.y - 484) <= 4);
    }

    /* Rotating during a drag releases its held button before anchoring for the
     * new geometry. A later release cannot resurrect the old gesture. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, f.now));
    settle(&f);
    CHECK(f.pointer.buttons == PH_HID_BTN_LEFT);
    initial = f.count;
    CHECK(ph_pointer_target(&f.pointer, .7, .6, landscape, ++f.now));
    CHECK(f.events[initial].mouse.buttons == 0);
    CHECK(!f.pointer.buttons && !f.pointer.requested_buttons && !f.pointer.count);
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .7, .6, landscape));
    initial = f.count;
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .7, .6,
                             landscape, ++f.now));
    CHECK(f.count == initial);

    /* A new button-down that itself introduces the changed orientation survives
     * the reset and is confirmed against the new geometry before it is sent. */
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_RIGHT, true, .4, .5, .46, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_RIGHT, false, .4, .5, .46, ++f.now));
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_RIGHT);
    CHECK(pressed < f.count && hypot(f.events[pressed].x - 200,
                                     f.events[pressed].y - .5 * 500 / .46) <= 4);
    CHECK(!f.pointer.buttons && !f.pointer.failed);

    /* Small continuous target changes must not exhaust the retry budget while
     * each acknowledged hop lands correctly. Input arrives before both the ACK
     * and video, so the phone remains behind the newest target throughout. */
    init(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .2, .46, f.now));
    settle(&f);
    for (int i = 0; i < 30; i++) {
        for (int j = 1; j <= 2; j++)
            CHECK(ph_pointer_target(&f.pointer, .5, .2 + (2 * i + j) * .0028,
                                    .46, ++f.now));
        step(&f);
        CHECK(f.pointer.buttons == PH_HID_BTN_LEFT && !f.pointer.failed);
    }
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .5, .368, .46, ++f.now));
    settle(&f);
    CHECK(!f.pointer.buttons && !f.pointer.failed && f.anchors == 8);
    CHECK(hypot(f.x - 250, f.y - .368 * 500 / .46) <= 4);

    /* A low but valid calibration can need hundreds of reports across a large
     * screen. Each confirmed segment must stay bounded so cancellation can drain
     * promptly; progress can complete the target without a retry-budget failure. */
    init(&f);
    CHECK(ph_pointer_set_scale(&f.pointer, .05, .05));
    f.true_x_scale = f.true_y_scale = .05;
    CHECK(ph_pointer_target(&f.pointer, .8, .9, .46, f.now));
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .8, .9, .46));
    CHECK(f.max_motion_batch <= 24 && f.syncs > 10 && f.anchors == 8);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .8, .9, .46, ++f.now));
    settle(&f);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .2, .2, .46, ++f.now));
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .2, .2, .46));
    CHECK(f.max_motion_batch <= 24 && f.anchors == 8 && !f.pointer.buttons);

    /* Tracking Sensitivity changes the small-step gain independently of the
     * large-step acceleration. The three actual wizard targets must converge
     * without fitting a new curve or accepting a click at an estimated point. */
    const double phone_curves[][2] = {{1.2, .04}, {1.19551, .03994}};
    const double initial_scales[] = {1, 1.5, 2, 4};
    const double guide_targets[][2] = {{.25, .35}, {.75, .65}, {.5, .75}};
    const double phone_aspect = 500.0 / 1080;
    for (size_t c = 0; c < sizeof phone_curves / sizeof phone_curves[0]; c++) {
        for (size_t s = 0; s < sizeof initial_scales / sizeof initial_scales[0]; s++) {
            init(&f);
            f.true_linear = phone_curves[c][0];
            f.true_quadratic = phone_curves[c][1];
            f.bounded_video = true;
            CHECK(ph_pointer_set_scale(&f.pointer, initial_scales[s], initial_scales[s]));
            for (size_t t = 0; t < sizeof guide_targets / sizeof guide_targets[0]; t++) {
                CHECK(ph_pointer_target(&f.pointer, guide_targets[t][0], guide_targets[t][1],
                                        phone_aspect, ++f.now));
                settle(&f);
                CHECK(ph_pointer_ready(&f.pointer, guide_targets[t][0], guide_targets[t][1],
                                       phone_aspect));
                CHECK(!f.pointer.failed && f.anchors == 8 && f.max_motion_batch <= 24);
            }
            CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .75,
                                     phone_aspect, ++f.now));
            settle(&f);
            double drag_scale_x = f.pointer.scale_x, drag_scale_y = f.pointer.scale_y;
            for (int i = 1; i <= 15; i++) {
                double y = .75 - i * .006;
                CHECK(ph_pointer_target(&f.pointer, .5, y, phone_aspect, ++f.now));
                settle(&f);
                CHECK(f.pointer.buttons == PH_HID_BTN_LEFT && !f.pointer.failed);
            }
            CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .5, .66,
                                     phone_aspect, ++f.now));
            settle(&f);
            CHECK(!f.pointer.buttons && !f.pointer.failed && f.anchors == 8);
            CHECK(hypot(f.x - 250, f.y - .66 * 1080) <= 4);
            CHECK(f.pointer.scale_x == drag_scale_x && f.pointer.scale_y == drag_scale_y);
        }
    }

    /* Directly reproduce the live final-target oscillation: a confirmed dot
     * seven pixels left of (250,810), at an independently changed small gain. */
    init(&f);
    f.true_linear = 1.2;
    f.true_quadratic = .04;
    CHECK(ph_pointer_set_scale(&f.pointer, 1.5, 1.5));
    f.bounded_video = true;
    confirmed_position(&f, 243, 810, phone_aspect);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .75,
                             phone_aspect, ++f.now));
    CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count);
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count && !f.pointer.failed);
    if (pressed < f.count)
        CHECK(hypot(f.events[pressed].x - 250, f.events[pressed].y - 810) <= 4);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .52, .75,
                             phone_aspect, ++f.now));
    settle(&f);
    CHECK(!f.pointer.buttons && !f.pointer.failed && f.anchors == 0);
    CHECK(hypot(f.x - 260, f.y - 810) <= 4);

    /* The quarter waypoint can be smaller than the model's smallest report.
     * Still emit one unit, then use its observed result rather than queuing an
     * empty move and waiting forever for an image that cannot change. */
    init(&f);
    f.true_linear = 1.2;
    f.true_quadratic = .04;
    f.bounded_video = true;
    CHECK(ph_pointer_set_scale(&f.pointer, 20, 20));
    confirmed_position(&f, 243, 810, phone_aspect);
    CHECK(ph_pointer_target(&f.pointer, .495, .75, phone_aspect, ++f.now));
    CHECK(f.count == 1 && f.events[0].mouse.dx == 1 && !f.events[0].mouse.dy);
    settle(&f);
    CHECK(ph_pointer_ready(&f.pointer, .495, .75, phone_aspect) && !f.pointer.failed);

    /* A partial prediction within four pixels of an unchanged frame is not
     * evidence of movement. Ignored tiny corrections must still exhaust the
     * retry budget, including releasing a drag whose endpoint cannot be reached. */
    for (int held = 0; held < 2; held++) {
        init(&f);
        CHECK(ph_pointer_target(&f.pointer, .5, .5, .46, f.now));
        settle(&f);
        if (held) {
            CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .5, .5, .46, ++f.now));
            settle(&f);
        }
        f.true_x_scale = f.true_y_scale = 0;
        if (held)
            CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false, .514, .5, .46, ++f.now));
        else
            CHECK(ph_pointer_target(&f.pointer, .514, .5, .46, ++f.now));
        for (int i = 0; i < 100 && !f.pointer.failed; i++)
            step(&f);
        CHECK(f.pointer.failed && !f.pointer.known && !f.pointer.buttons && !f.pointer.count);
        CHECK(f.events[f.count - 1].mouse.buttons == 0);
        CHECK(!ph_pointer_transport_stalled(&f.pointer));
        if (held) {
            CHECK(f.pointer.phase == PH_POINTER_DRAINING && f.pointer.sent_at == f.now);
            ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now + 17);
            CHECK(f.pointer.phase == PH_POINTER_IDLE && !ph_pointer_transport_stalled(&f.pointer));
        }
    }

    /* Ignored reports still exhaust the bounded retry budget. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .5, .5, .46, f.now));
    settle(&f);
    f.true_x_scale = f.true_y_scale = 0;
    CHECK(ph_pointer_target(&f.pointer, .8, .8, .46, ++f.now));
    for (int i = 0; i < 400 && !f.pointer.failed; i++)
        step(&f);
    CHECK(f.pointer.failed && !f.pointer.known);
    initial = f.count;
    for (int i = 0; i < 20; i++)
        step(&f);
    CHECK(f.count == initial);

    /* Wheel positioning tolerates a small video-confirmed offset. It must not
     * stall on sub-pixel correction cycles, or relax the next click's accuracy. */
    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .5, .5, .46, f.now));
    settle(&f);
    initial = f.count;
    CHECK(ph_pointer_scroll_target(&f.pointer, .52, .5, .46, ++f.now));
    CHECK(!ph_pointer_ready(&f.pointer, .52, .5, .46) && f.count == initial);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true, .52, .5, .46, ++f.now));
    CHECK(first_button(&f, PH_HID_BTN_LEFT) == f.count);
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count);
    CHECK(hypot(f.events[pressed].x - 260, f.events[pressed].y - .5 * 500 / .46) <= 4);

    init(&f);
    CHECK(ph_pointer_target(&f.pointer, .5, .5, .46, f.now));
    settle(&f);
    CHECK(!ph_pointer_scroll_target(&f.pointer, .7, .5, .46, ++f.now));
    settle(&f);
    CHECK(ph_pointer_scroll_target(&f.pointer, .7, .5, .46, ++f.now));
    f.now += 1600;
    CHECK(!ph_pointer_scroll_target(&f.pointer, .7, .5, .46, f.now));
    CHECK(f.pointer.phase == PH_POINTER_MOVING); /* stale video must be refreshed */

    /* A tiny residual with stale scale must converge before pressing. Increasing
     * the correction fraction makes this case oscillate despite faster normal drags. */
    init(&f);
    f.true_linear = 1.2;
    f.true_quadratic = .04;
    CHECK(ph_pointer_set_scale(&f.pointer, .5, .5));
    confirmed_position(&f, 250, 700, .46);
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, true,
                             245.9 / 500, 700 * .46 / 500, .46, ++f.now));
    CHECK(ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT, false,
                             245.9 / 500, 700 * .46 / 500, .46, ++f.now));
    settle(&f);
    pressed = first_button(&f, PH_HID_BTN_LEFT);
    CHECK(pressed < f.count);
    CHECK(hypot(f.events[pressed].x - 245.9, f.events[pressed].y - 700) <= 4);
    CHECK(!f.pointer.failed && !f.pointer.buttons && !f.pointer.count);

    /* Disconnection and bad inputs cannot emit mouse reports. */
    init(&f);
    CHECK(!ph_pointer_target(&f.pointer, NAN, .5, .46, f.now));
    CHECK(!ph_pointer_target(&f.pointer, .5, 1, .46, f.now));
    CHECK(!ph_pointer_button(&f.pointer, 3, true, .5, .5, .46, f.now));
    ph_pointer_set_enabled(&f.pointer, false, f.now);
    CHECK(!ph_pointer_target(&f.pointer, .5, .5, .46, f.now));
    CHECK(f.count == 0);

    if (failures)
        return 1;
    puts("all pointer feedback and button sequences passed");
    return 0;
}
