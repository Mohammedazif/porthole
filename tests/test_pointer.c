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
    double x, y, true_x_scale, true_y_scale;
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

static double travel(int value)
{
    double magnitude = fabs((double)value);
    double result = .25 * magnitude + .055 * magnitude * magnitude;
    return value < 0 ? -result : result;
}

static void mouse(void *user, const ph_hid_mouse *m)
{
    fixture *f = user;
    double width = 500 * fmax(1, f->pointer.aspect);
    double height = width / f->pointer.aspect;
    f->x = fmin(width - 16, fmax(16, f->x + travel(m->dx) * f->true_x_scale));
    f->y = fmin(height - 16, fmax(16, f->y + travel(m->dy) * f->true_y_scale));
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
    else if (phase == PH_POINTER_WAIT_VIDEO)
        ph_pointer_observe(&f->pointer, true, f->x / (500 * fmax(1, f->pointer.aspect)),
                            f->y * f->pointer.aspect / (500 * fmax(1, f->pointer.aspect)),
                            f->now);
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

int main(void)
{
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
    CHECK(f.pointer.phase == PH_POINTER_MOVING);
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
    initial = f.count;
    ph_pointer_ack(&f.pointer, stale_id, f.now + 3100);
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_DRAINING);
    ph_pointer_ack(&f.pointer, f.pointer.sync_id, f.now + 3200);
    CHECK(f.count == initial && f.pointer.phase == PH_POINTER_IDLE);

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
    CHECK(f.anchors == 16 && f.pointer.phase == PH_POINTER_MOVING);
    uint32_t next_id = f.pointer.sync_id;
    ph_pointer_ack(&f.pointer, stale_id, ++f.now);
    CHECK(f.pointer.sync_id == next_id && f.pointer.phase == PH_POINTER_MOVING);
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
