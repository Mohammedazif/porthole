/* Real controllers against a paced asynchronous phone, rather than immediate
 * reports and same-formula feedback. Seeds and all time are deterministic. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "control/pointer.h"
#include "control/scroll.h"

#define ASPECT (500.0 / 1080.0)
#define QUEUE 128

typedef struct report {
    ph_hid_mouse mouse;
    uint64_t at;
    bool press;
    double click_x, click_y;
} report;

typedef struct ack {
    uint64_t at;
    uint32_t id;
    bool pending;
} ack;

typedef struct curve {
    const char *name;
    double linear, quadratic, exponent, x_gain, y_gain;
} curve;

typedef struct phone {
    ph_pointer pointer;
    ph_scroll scroll;
    report reports[QUEUE];
    ack acks[QUEUE];
    unsigned int first, queued, batch, maximum_batch, maximum_queued;
    unsigned int presses, resets, faults, actions, syncs, release_plans;
    unsigned int false_circles;
    uint32_t seed, random, last_ack;
    uint64_t now, available, stalled_since, blackout_until;
    uint64_t max_press_delay, max_release_delay, max_cancel_delay, max_video_release;
    double x, y, click_error;
    uint8_t physical_buttons, queued_buttons;
    bool failed, drop_next_ack, recovering;
    curve movement;
} phone;

static unsigned int failures;

static void check(phone *f, bool condition, const char *message)
{
    if (condition || f->failed)
        return;
    fprintf(stderr, "seed=%u time=%llu action=%u phase=%d buttons=%u/%u pending=%u "
                    "actual=(%.2f,%.2f) target=(%.3f,%.3f): %s\n", f->seed,
            (unsigned long long)f->now, f->actions, f->pointer.phase,
            f->pointer.buttons, f->physical_buttons, f->queued, f->x, f->y,
            f->pointer.target_x, f->pointer.target_y, message);
    f->failed = true;
    failures++;
}

static uint32_t random_value(phone *f)
{
    uint32_t value = f->random;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    f->random = value;
    return value;
}

static void enqueue(void *user, const ph_hid_mouse *mouse)
{
    phone *f = user;
    check(f, f->queued < QUEUE, "unbounded physical report queue");
    if (f->queued == QUEUE)
        return;
    if (f->available < f->now)
        f->available = f->now;
    f->available += 15;
    report *item = &f->reports[(f->first + f->queued) % QUEUE];
    *item = (report){.mouse = *mouse, .at = f->available};
    item->mouse.buttons |= f->pointer.buttons; /* Real app's pointer/button merge. */
    item->press = (item->mouse.buttons & (uint8_t)~f->queued_buttons) != 0;
    if (item->press) {
        check(f, f->pointer.count > 0, "press has no queued confirmed action");
        const ph_pointer_action *action = &f->pointer.actions[f->pointer.head];
        item->click_x = action->x * 500;
        item->click_y = action->y * 1080;
    }
    f->queued_buttons = item->mouse.buttons;
    f->queued++;
    if (f->queued > f->maximum_queued)
        f->maximum_queued = f->queued;
    if (mouse->dx || mouse->dy)
        f->batch++;
}

static void delay(void *user, uint16_t milliseconds)
{
    phone *f = user;
    if (f->available < f->now)
        f->available = f->now;
    f->available += milliseconds;
}

static void sync_output(void *user, uint32_t id)
{
    phone *f = user;
    f->syncs++;
    if (f->batch > f->maximum_batch)
        f->maximum_batch = f->batch;
    f->batch = 0;
    if (f->drop_next_ack) {
        f->drop_next_ack = false;
        f->faults++;
        return;
    }
    for (size_t i = 0; i < QUEUE; i++) {
        if (f->acks[i].pending)
            continue;
        f->acks[i] = (ack){.at = (f->available > f->now ? f->available : f->now)
                                  + 10 + random_value(f) % 141,
                           .id = id, .pending = true};
        return;
    }
    check(f, false, "unbounded ACK queue");
}

static bool position(void *user, double x, double y, double aspect)
{
    phone *f = user;
    bool ready = ph_pointer_scroll_target(&f->pointer, x, y, aspect, f->now);
    if (f->pointer.failed)
        ph_scroll_cancel(&f->scroll);
    return ready;
}

static double motion(const phone *f, int units, double position, bool horizontal)
{
    double magnitude = fabs((double)units);
    double travel = f->movement.linear * magnitude
                    + f->movement.quadratic * pow(magnitude, f->movement.exponent);
    double gain = horizontal ? f->movement.x_gain : f->movement.y_gain;
    /* Position-dependent gain and quarter-pixel quantization are independent
     * of the controller's fixed quadratic prediction. */
    travel *= gain * (1 + .06 * sin(position / 53));
    travel = nearbyint(travel * 4) / 4;
    return units < 0 ? -travel : travel;
}

static void advance(phone *f)
{
    f->now += 10;
    while (f->queued && f->reports[f->first].at <= f->now) {
        report item = f->reports[f->first];
        f->first = (f->first + 1) % QUEUE;
        f->queued--;
        check(f, !(item.mouse.dx == -127 && item.mouse.dy == -127 &&
                   (item.mouse.buttons || f->physical_buttons)), "corner sweep while physically dragging");
        f->x = fmin(484, fmax(16, f->x + motion(f, item.mouse.dx, f->x, true)));
        f->y = fmin(1064, fmax(16, f->y + motion(f, item.mouse.dy, f->y, false)));
        if (item.press) {
            double error = hypot(f->x - item.click_x, f->y - item.click_y);
            if (error > f->click_error)
                f->click_error = error;
            check(f, error <= 4.000001, "physical click exceeded confirmed four-pixel tolerance");
            f->presses++;
        }
        f->physical_buttons = item.mouse.buttons;
    }
    for (size_t i = 0; i < QUEUE; i++) {
        if (f->acks[i].pending && f->acks[i].at <= f->now) {
            uint32_t id = f->acks[i].id;
            f->acks[i].pending = false;
            ph_pointer_ack(&f->pointer, id, f->now);
            ph_scroll_ack_at(&f->scroll, id, f->now);
            f->last_ack = id;
        }
    }
    if (f->pointer.phase == PH_POINTER_WAIT_VIDEO) {
        uint64_t age = f->now - f->pointer.wait_since;
        if (age < 32) {
            /* An early stale circle cannot confirm the pending movement. */
            ph_pointer_observe(&f->pointer, true, .95, .95, f->now);
        } else if (age < 50 && f->pointer.buttons) {
            double old_x = f->pointer.actual_x, old_y = f->pointer.actual_y;
            double false_x = old_x < .5 ? .9 : .1;
            double false_y = old_y < .5 ? .9 : .1;
            ph_pointer_observe(&f->pointer, true, false_x, false_y, f->now);
            check(f, f->pointer.phase == PH_POINTER_WAIT_VIDEO
                     && f->pointer.actual_x == old_x && f->pointer.actual_y == old_y,
                  "a false off-path circle redirected a held drag");
            f->false_circles++;
        } else if (age >= 50 + f->pointer.sync_id % 91 && f->now >= f->blackout_until) {
            ph_pointer_observe(&f->pointer, true, f->x / 500, f->y / 1080, f->now);
        }
    }
    ph_pointer_tick(&f->pointer, f->now);
    ph_scroll_tick(&f->scroll, f->now);
    check(f, f->pointer.count <= PH_POINTER_QUEUE && f->scroll.pending_x >= -4
             && f->scroll.pending_x <= 4 && f->scroll.pending_y >= -4 && f->scroll.pending_y <= 4,
          "controller intent queue exceeded its bound");
    check(f, f->maximum_batch <= 24 && f->maximum_queued <= 25,
          "unacknowledged motion/release exceeded the bounded plan");
    if (!f->recovering && (ph_pointer_transport_stalled(&f->pointer) || f->scroll.failed)) {
        f->recovering = true;
        f->stalled_since = f->now;
        ph_pointer_set_enabled(&f->pointer, false, f->now);
        ph_scroll_cancel(&f->scroll);
    }
    if (f->recovering && f->now - f->stalled_since >= 400) {
        check(f, !f->physical_buttons, "timed-out controller failed to dispatch its held-button release");
        memset(f->acks, 0, sizeof f->acks);
        f->first = f->queued = 0;
        f->available = f->now;
        ph_pointer_transport_reset(&f->pointer, f->now);
        ph_scroll_transport_reset(&f->scroll);
        ph_pointer_set_enabled(&f->pointer, true, f->now);
        f->recovering = false;
        f->resets++;
    }
}

static void init(phone *f, uint32_t seed, curve movement)
{
    memset(f, 0, sizeof *f);
    f->seed = f->random = seed;
    f->movement = movement;
    f->x = 400;
    f->y = 800;
    f->now = 100;
    f->pointer.output = (ph_pointer_output){f, enqueue, delay, sync_output};
    f->scroll.output = (ph_scroll_output){f, enqueue, delay, sync_output};
    f->scroll.position = position;
    ph_pointer_set_enabled(&f->pointer, true, f->now);
}

static void finish(phone *f)
{
    ph_pointer_cancel(&f->pointer, f->now);
    ph_scroll_cancel(&f->scroll);
    f->blackout_until = 0;
    for (int i = 0; i < 500 && !f->failed; i++)
        advance(f);
    check(f, !f->physical_buttons && !f->pointer.buttons && !f->pointer.count && !f->queued,
          "cancel/focus loss left a physical held button or queued intent");
}

static void settle_target(phone *f, double x, double y)
{
    ph_pointer_target(&f->pointer, x, y, ASPECT, f->now);
    for (int i = 0; i < 500 && !f->failed && !ph_pointer_ready(&f->pointer, x, y, ASPECT); i++)
        advance(f);
    check(f, ph_pointer_ready(&f->pointer, x, y, ASPECT), "directed interaction failed initial alignment");
}

static void directed_drag_sequences(phone *f)
{
    settle_target(f, .4, .4);
    uint64_t started = f->now;
    ph_pointer_button(&f->pointer, PH_HID_BTN_LEFT, true, .4, .4, ASPECT, f->now);
    for (int i = 0; i < 100 && !f->failed && !f->physical_buttons; i++)
        advance(f);
    check(f, f->physical_buttons == PH_HID_BTN_LEFT, "directed drag never pressed the phone");
    f->max_press_delay = f->now - started;
    for (int i = 1; i <= 100 && !f->failed; i++) {
        if (!(i % 2))
            ph_pointer_target(&f->pointer, .4 + i * .001, .4 + i * .0008, ASPECT, f->now);
        advance(f);
    }
    started = f->now;
    unsigned int before_release = f->syncs;
    ph_pointer_button(&f->pointer, PH_HID_BTN_LEFT, false, .5, .48, ASPECT, f->now);
    for (int i = 0; i < 500 && !f->failed
                        && (f->physical_buttons || f->pointer.requested_buttons || f->pointer.count); i++)
        advance(f);
    check(f, !f->physical_buttons && !f->pointer.buttons && !f->pointer.requested_buttons,
          "directed drag did not deliver its requested physical release");
    f->max_release_delay = f->now - started;
    f->release_plans = f->syncs - before_release;

    settle_target(f, .5, .5);
    ph_pointer_button(&f->pointer, PH_HID_BTN_LEFT, true, .5, .5, ASPECT, f->now);
    for (int i = 0; i < 100 && !f->failed && !f->physical_buttons; i++)
        advance(f);
    check(f, f->physical_buttons == PH_HID_BTN_LEFT, "fault-injected drag never pressed the phone");
    f->blackout_until = f->now + 1600;
    started = f->now;
    ph_pointer_target(&f->pointer, .7, .7, ASPECT, f->now);
    for (int i = 0; i < 160 && !f->failed && f->physical_buttons; i++)
        advance(f);
    check(f, !f->physical_buttons && !f->pointer.buttons && !ph_pointer_transport_stalled(&f->pointer),
          "missing video left a held mouse button or falsely killed the healthy transport");
    f->max_video_release = f->now - started;
    f->blackout_until = 0;

    /* Interrupt three real held drags while motion is still physically queued.
     * Re-enter immediately: a new corner sweep must follow the old release. */
    for (int reset = 0; reset < 3 && !f->failed; reset++) {
        settle_target(f, .5, .5);
        ph_pointer_button(&f->pointer, PH_HID_BTN_LEFT, true, .5, .5, ASPECT, f->now);
        for (int i = 0; i < 100 && !f->failed && !f->physical_buttons; i++)
            advance(f);
        check(f, f->physical_buttons == PH_HID_BTN_LEFT, "focus/reset drag never pressed the phone");
        ph_pointer_target(&f->pointer, .75, .6, ASPECT, f->now);
        advance(f);
        started = f->now;
        if (reset == 0)
            ph_pointer_cancel(&f->pointer, f->now);
        else if (reset == 1)
            ph_pointer_reset(&f->pointer, f->now);
        else {
            ph_pointer_set_enabled(&f->pointer, false, f->now);
            ph_pointer_set_enabled(&f->pointer, true, f->now);
        }
        ph_scroll_cancel(&f->scroll);
        ph_pointer_target(&f->pointer, .3, .45, ASPECT, f->now);
        for (int i = 0; i < 100 && !f->failed && f->physical_buttons; i++)
            advance(f);
        if (f->now - started > f->max_cancel_delay)
            f->max_cancel_delay = f->now - started;
        settle_target(f, .3, .45);
        check(f, !f->physical_buttons && !f->pointer.buttons && !f->pointer.requested_buttons,
              "focus/reset/re-enter lost the old held-button release");
    }
}

int main(void)
{
    const curve curves[] = {
        {"quadratic", .25, .055, 2, 1, 1},
        {"changed-small-gain", 1.2, .04, 2, 1, 1},
        {"subquadratic", .4, .07, 1.85, .85, 1.2},
        {"superquadratic", .18, .04, 2.12, 1.15, .8},
    };
    const double targets[][2] = {{.25, .35}, {.75, .65}, {.5, .75}, {.8, .2}};
    unsigned int total_presses = 0, total_faults = 0, total_resets = 0, false_circles = 0;
    uint64_t press_delay = 0, release_delay = 0, cancel_delay = 0, video_release = 0;
    for (size_t c = 0; c < sizeof curves / sizeof curves[0]; c++) {
        phone f;
        init(&f, (uint32_t)c + 1, curves[c]);
        uint64_t maximum_latency = 0;
        for (size_t t = 0; t < sizeof targets / sizeof targets[0] && !f.failed; t++) {
            uint64_t started = f.now;
            ph_pointer_target(&f.pointer, targets[t][0], targets[t][1], ASPECT, f.now);
            for (int i = 0; i < 500 && !f.failed
                            && !ph_pointer_ready(&f.pointer, targets[t][0], targets[t][1], ASPECT); i++)
                advance(&f);
            uint64_t latency = f.now - started;
            if (latency > maximum_latency)
                maximum_latency = latency;
            check(&f, ph_pointer_ready(&f.pointer, targets[t][0], targets[t][1], ASPECT),
                  "static target failed to converge within five seconds under a perturbed phone curve");
        }
        printf("curve=%s max_target_latency_ms=%llu max_queued_reports=%u\n", curves[c].name,
               (unsigned long long)maximum_latency, f.maximum_queued);
        finish(&f);
    }

    for (uint32_t seed = 1; seed <= 64; seed++) {
        phone f;
        init(&f, seed * 0x9e3779b9U, curves[seed % 4]);
        directed_drag_sequences(&f);
        double x = .5, y = .5;
        for (int step = 0; step < 1200 && !f.failed; step++) {
            if (!(step % 3) && !f.recovering) {
                uint32_t action = random_value(&f) % 12;
                f.actions++;
                switch (action) {
                case 0: case 1: case 2: case 3:
                    x = .15 + .7 * (random_value(&f) % 1000) / 1000;
                    y = .15 + .7 * (random_value(&f) % 1000) / 1000;
                    ph_pointer_target(&f.pointer, x, y, ASPECT, f.now);
                    break;
                case 4:
                    ph_scroll_cancel(&f.scroll);
                    ph_pointer_button(&f.pointer, PH_HID_BTN_LEFT,
                                      !(f.pointer.requested_buttons & PH_HID_BTN_LEFT),
                                      x, y, ASPECT, f.now);
                    break;
                case 5:
                    ph_pointer_cancel(&f.pointer, f.now);
                    ph_scroll_cancel(&f.scroll);
                    break;
                case 6:
                    ph_pointer_reset(&f.pointer, f.now);
                    ph_scroll_reset(&f.scroll);
                    break;
                case 7:
                    if (!f.pointer.buttons && !f.pointer.requested_buttons)
                        ph_scroll_request(&f.scroll, x, y, ASPECT, 0, random_value(&f) & 1 ? 4 : -4);
                    break;
                case 8:
                    if (f.last_ack) {
                        ph_pointer_ack(&f.pointer, f.last_ack, f.now);
                        ph_scroll_ack_at(&f.scroll, f.last_ack, f.now);
                    }
                    break;
                case 9:
                    f.blackout_until = f.now + 900; /* Missing video, not a dead transport. */
                    break;
                case 10:
                    f.drop_next_ack = true;
                    break;
                case 11:
                    ph_pointer_set_enabled(&f.pointer, false, f.now);
                    ph_scroll_cancel(&f.scroll);
                    ph_pointer_set_enabled(&f.pointer, true, f.now);
                    break;
                }
            }
            advance(&f);
        }
        finish(&f);
        total_presses += f.presses;
        total_faults += f.faults;
        total_resets += f.resets;
        false_circles += f.false_circles;
        if (f.max_press_delay > press_delay) press_delay = f.max_press_delay;
        if (f.max_release_delay > release_delay) {
            release_delay = f.max_release_delay;
            printf("slowest_tracked_release seed=%u curve=%s delay_ms=%llu confirmed_plans=%u\n",
                   f.seed, f.movement.name, (unsigned long long)release_delay, f.release_plans);
        }
        if (f.max_cancel_delay > cancel_delay) cancel_delay = f.max_cancel_delay;
        if (f.max_video_release > video_release) video_release = f.max_video_release;
    }
    printf("seeded_sequences=64 actions_per_sequence=400 physical_presses=%u "
           "false_drag_circles=%u dropped_ACKs=%u transport_recoveries=%u failures=%u\n", total_presses,
           false_circles, total_faults, total_resets, failures);
    printf("max_physical_delay_ms aligned_press=%llu tracked_release=%llu focus_cancel_release=%llu "
           "video_failure_release=%llu\n", (unsigned long long)press_delay,
           (unsigned long long)release_delay, (unsigned long long)cancel_delay,
           (unsigned long long)video_release);
    if (total_presses < 320 || !false_circles) {
        fputs("insufficient directed held-drag/false-circle coverage\n", stderr);
        return 1;
    }
    return failures ? 1 : 0;
}
