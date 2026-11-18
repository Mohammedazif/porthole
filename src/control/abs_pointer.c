#include "control/abs_pointer.h"

#include <math.h>
#include <string.h>

#define WHEEL_NOTCH 0.07      /* fraction of picture height dragged per notch */
#define WHEEL_MAX_DRAG 0.35
#define WHEEL_GLIDE_MS 150
#define FEED_SWIPE 0.45
#define FEED_BURST_MS 600

static double clamp01(double v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static void send(ph_abs *a, uint8_t buttons, double x, double y, uint64_t now_ms)
{
    x = clamp01(x);
    y = clamp01(y);
    if (!a->sent_valid || x != a->x || y != a->y)
        a->moved_ms = now_ms;
    a->x = x;
    a->y = y;
    a->buttons = buttons;
    a->sent_valid = true;
    a->sent_ms = now_ms;
    if (a->output.report)
        a->output.report(a->output.user, buttons, x, y);
}

void ph_abs_init(ph_abs *a, ph_abs_output output)
{
    memset(a, 0, sizeof *a);
    a->output = output;
}

bool ph_abs_busy(const ph_abs *a)
{
    return a->count > 0 || a->pending;
}

static bool push(ph_abs *a, uint64_t at_ms, uint8_t buttons, double x, double y)
{
    if (a->count >= PH_ABS_STEPS)
        return false;
    a->steps[(a->head + a->count++) % PH_ABS_STEPS] = (ph_abs_step){at_ms, buttons, x, y};
    return true;
}

/* Builds a finger stroke after anything already queued. Returns its end time. */
static uint64_t stroke(ph_abs *a, uint64_t start_ms, double x1, double y1, double x2, double y2,
                       uint32_t glide_ms, uint32_t hold_ms)
{
    uint64_t t = start_ms;
    push(a, t, 0, x1, y1);
    t += PH_ABS_SETTLE_MS;
    push(a, t, 1, x1, y1);
    int steps = (int)(glide_ms / PH_ABS_REPORT_MS);
    for (int i = 1; i <= steps; i++) {
        double f = (double)i / steps;
        t += PH_ABS_REPORT_MS;
        push(a, t, 1, x1 + (x2 - x1) * f, y1 + (y2 - y1) * f);
    }
    t += hold_ms > 80 ? hold_ms : 80;
    push(a, t, 0, x2, y2);
    return t;
}

static uint64_t script_start(const ph_abs *a, uint64_t now_ms)
{
    if (a->count)
        return a->steps[(a->head + a->count - 1) % PH_ABS_STEPS].at_ms + PH_ABS_REPORT_MS;
    return now_ms;
}

static bool scripted(ph_abs *a, double x1, double y1, double x2, double y2, uint32_t glide_ms,
                     uint32_t hold_ms, uint64_t now_ms)
{
    if (a->held || a->pending)
        return false; /* never script over a finger the user is holding */
    unsigned int before = a->count;
    uint64_t end = stroke(a, script_start(a, now_ms), x1, y1, x2, y2, glide_ms, hold_ms);
    if (a->count - before < 4) { /* queue full: drop the partial stroke */
        a->count = before;
        return false;
    }
    if (a->want_valid) /* afterwards the phone pointer goes back under the PC mouse */
        push(a, end + PH_ABS_REPORT_MS, 0, a->want_x, a->want_y);
    return true;
}

bool ph_abs_swipe(ph_abs *a, double x1, double y1, double x2, double y2, uint32_t glide_ms,
                  uint32_t hold_ms, uint64_t now_ms)
{
    return scripted(a, x1, y1, x2, y2, glide_ms, hold_ms, now_ms);
}

bool ph_abs_tap(ph_abs *a, double x, double y, int count, uint32_t hold_ms, uint64_t now_ms)
{
    if (a->held || a->pending || count < 1 || count > 3)
        return false;
    uint64_t t = script_start(a, now_ms);
    push(a, t, 0, x, y);
    t += PH_ABS_SETTLE_MS;
    for (int i = 0; i < count; i++) {
        push(a, t, 1, x, y);
        t += hold_ms > 60 ? hold_ms : 60;
        push(a, t, 0, x, y);
        t += 90; /* inside iOS's double-tap interval */
    }
    return true;
}

/* Edge gestures measured on iOS 26.6.1 (portrait picture fractions). */
bool ph_abs_home(ph_abs *a, uint64_t now_ms)
{
    return scripted(a, .5, 1072.0 / 1080, .5, 600.0 / 1080, 120, 0, now_ms);
}

bool ph_abs_app_switcher(ph_abs *a, uint64_t now_ms)
{
    return scripted(a, .5, 1, .5, 760.0 / 1080, 250, 700, now_ms);
}

bool ph_abs_control_center(ph_abs *a, uint64_t now_ms)
{
    return scripted(a, .94, 6.0 / 1080, .94, 520.0 / 1080, 250, 0, now_ms);
}

bool ph_abs_notification_center(ph_abs *a, uint64_t now_ms)
{
    return scripted(a, .12, 6.0 / 1080, .12, 600.0 / 1080, 250, 0, now_ms);
}

void ph_abs_move(ph_abs *a, double x, double y, uint64_t now_ms)
{
    a->want_x = clamp01(x);
    a->want_y = clamp01(y);
    a->want_valid = true;
    ph_abs_tick(a, now_ms);
}

void ph_abs_button(ph_abs *a, uint8_t button, bool down, double x, double y, uint64_t now_ms)
{
    if (!button)
        return;
    a->want_x = clamp01(x);
    a->want_y = clamp01(y);
    a->want_valid = true;
    if (down) {
        if (a->held & button)
            return;
        a->count = 0; /* a real press takes over from any scripted gesture */
        a->wheel = 0;
        bool lifted = false;
        if (a->buttons && !a->held) {
            /* Down only because of a script or a not-yet-sent quick release (a fast
             * double-click): lift first so the presses stay separate. */
            send(a, 0, a->x, a->y, now_ms);
            lifted = true;
        }
        a->held |= button;
        if (a->buttons) {
            /* Another button is already down: add this one where the finger is. */
            send(a, a->buttons | button, a->x, a->y, now_ms);
            return;
        }
        a->pending |= button;
        if (!a->sent_valid || a->x != a->want_x || a->y != a->want_y)
            send(a, 0, a->want_x, a->want_y, now_ms);
        a->press_at_ms = a->moved_ms + PH_ABS_SETTLE_MS;
        if (lifted && a->press_at_ms < now_ms + PH_ABS_REPORT_MS)
            a->press_at_ms = now_ms + PH_ABS_REPORT_MS;
        ph_abs_tick(a, now_ms);
        return;
    }
    if (!(a->held & button))
        return;
    a->held &= (uint8_t)~button;
    if (a->pending & button) {
        a->release_after |= button; /* lift right after the delayed press */
        return;
    }
    send(a, a->buttons & (uint8_t)~button, a->x, a->y, now_ms);
}

void ph_abs_wheel(ph_abs *a, double notches, double x, double y, bool feed_mode, uint64_t now_ms)
{
    if (a->held || a->pending || !isfinite(notches) || notches == 0)
        return;
    a->want_x = clamp01(x);
    a->want_y = clamp01(y);
    a->want_valid = true;
    if (feed_mode) {
        if (now_ms < a->feed_until_ms) {
            a->feed_until_ms = now_ms + FEED_BURST_MS;
            return; /* one swipe per wheel burst */
        }
        a->feed_until_ms = now_ms + FEED_BURST_MS;
        double dy = notches < 0 ? -FEED_SWIPE : FEED_SWIPE;
        double y1 = notches < 0 ? .72 : .28;
        scripted(a, .5, y1, .5, y1 + dy, 160, 0, now_ms);
        return;
    }
    a->wheel += notches;
    a->wheel_x = a->want_x;
    a->wheel_y = a->want_y;
    ph_abs_tick(a, now_ms);
}

static void start_wheel_drag(ph_abs *a, uint64_t now_ms)
{
    double drag = a->wheel * WHEEL_NOTCH;
    if (drag > WHEEL_MAX_DRAG)
        drag = WHEEL_MAX_DRAG;
    if (drag < -WHEEL_MAX_DRAG)
        drag = -WHEEL_MAX_DRAG;
    a->wheel -= drag / WHEEL_NOTCH;
    if (fabs(a->wheel) < .01)
        a->wheel = 0;
    /* Scrolling down (negative notches) moves content up: the finger goes up. Keep
     * the stroke on screen by starting from the side it moves away from. */
    double y1 = a->wheel_y, y2 = a->wheel_y + drag;
    if (y2 < .03) {
        y1 += .03 - y2;
        y2 = .03;
    } else if (y2 > .97) {
        y1 -= y2 - .97;
        y2 = .97;
    }
    scripted(a, a->wheel_x, clamp01(y1), a->wheel_x, clamp01(y2), WHEEL_GLIDE_MS, 0, now_ms);
}

void ph_abs_tick(ph_abs *a, uint64_t now_ms)
{
    if (a->pending) {
        if (now_ms < a->press_at_ms)
            return; /* the position must stay put until the press is sent */
        send(a, a->buttons | a->pending, a->x, a->y, now_ms);
        a->pending = 0;
        if (a->release_after) {
            /* Give the press its own report interval before lifting. */
            push(a, now_ms + 60, a->buttons & (uint8_t)~a->release_after, a->x, a->y);
            a->release_after = 0;
        }
        return;
    }
    if (a->count) {
        ph_abs_step *s = &a->steps[a->head];
        if (now_ms < s->at_ms || (a->sent_valid && now_ms - a->sent_ms < PH_ABS_REPORT_MS))
            return;
        send(a, s->buttons, s->x, s->y, now_ms);
        a->head = (a->head + 1) % PH_ABS_STEPS;
        a->count--;
        return;
    }
    if (a->wheel != 0 && !a->held) {
        start_wheel_drag(a, now_ms);
        return;
    }
    if (a->want_valid && (!a->sent_valid || a->x != a->want_x || a->y != a->want_y)
        && (!a->sent_valid || now_ms - a->sent_ms >= PH_ABS_REPORT_MS))
        send(a, a->buttons, a->want_x, a->want_y, now_ms); /* hover or held drag */
}

void ph_abs_release_all(ph_abs *a, uint64_t now_ms)
{
    a->count = 0;
    a->head = 0;
    a->pending = 0;
    a->release_after = 0;
    a->held = 0;
    a->wheel = 0;
    if (a->sent_valid && a->buttons)
        send(a, 0, a->x, a->y, now_ms);
}
