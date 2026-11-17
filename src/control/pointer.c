#include "control/pointer.h"

#include <math.h>

#define MODEL_WIDTH 500.0
#define CORNER 16.0
#define ALIGNMENT_PX 4.0
#define SCROLL_ALIGNMENT_PX 20.0
#define FRAME_SETTLE_MS 32
#define FRAME_TIMEOUT_MS 700
#define ACK_TIMEOUT_MS 3000
#define POSITION_MAX_AGE_MS 1200
#define MIN_PRESS_MS 80
#define MAX_ATTEMPTS 8
#define MAX_MOVE_REPORTS 24 /* bound one unacknowledged plan to 360 ms at 15 ms/report */

static void pump(ph_pointer *p);

static bool awaiting_ack(const ph_pointer *p)
{
    return p->phase == PH_POINTER_MOVING || p->phase == PH_POINTER_BUTTON
           || p->phase == PH_POINTER_DRAINING;
}

static bool ack_expired(const ph_pointer *p, uint64_t now_ms)
{
    return awaiting_ack(p) && now_ms >= p->sent_at
           && now_ms - p->sent_at >= ACK_TIMEOUT_MS;
}

static double model_width(double aspect)
{
    /* The measured movement model uses a 500-pixel short edge, rather than a
     * 500-pixel width. Rotating the phone must not rescale its physical motion. */
    return MODEL_WIDTH * fmax(1, aspect);
}

static bool valid(double x, double y, double aspect)
{
    return isfinite(x) && isfinite(y) && isfinite(aspect) && x >= 0 && x < 1 && y >= 0
           && y < 1 && aspect >= 0.1 && aspect <= 10;
}

static void reachable(double *x, double *y, double aspect)
{
    /* iOS constrains the centre of its dot to an inset around the display. Use
     * the nearest native position consistently, including wheel readiness. */
    double width = model_width(aspect);
    double mx = CORNER / width, my = CORNER * aspect / width;
    *x = fmin(1 - mx, fmax(mx, *x));
    *y = fmin(1 - my, fmax(my, *y));
}

static double distance(double x1, double y1, double x2, double y2, double aspect)
{
    double width = model_width(aspect);
    return hypot((x1 - x2) * width, (y1 - y2) * width / aspect);
}

static void emit_mouse(ph_pointer *p, int dx, int dy)
{
    ph_hid_mouse mouse = {.buttons = p->buttons, .dx = (int16_t)dx, .dy = (int16_t)dy};
    p->output.mouse(p->output.user, &mouse);
}

static void sync(ph_pointer *p, ph_pointer_phase phase)
{
    p->phase = phase;
    p->sent_at = p->now_ms;
    p->sync_id = ((p->sync_id + 1) & 0x7fffffffU) | 0x80000000U;
    p->output.sync(p->output.user, p->sync_id);
}

static double move_axis(ph_pointer *p, double pixels, bool horizontal, int *reports_left)
{
    static const int steps[] = {32, 16, 8, 4, 2, 1};
    double scale = horizontal ? p->scale_x : p->scale_y;
    if (!scale)
        scale = 1;
    double remaining = fabs(pixels), moved = 0;
    bool correction = remaining < 20;
    if (correction) {
        /* A changed tracking sensitivity can amplify tiny reports much more
         * than the large reports used for the saved scale. Approach a nearby
         * target in confirmed partial steps instead of oscillating across it.
         * Already-aligned axes need no nudge while the other axis catches up. */
        if (remaining <= ALIGNMENT_PX / 2)
            return 0;
        remaining *= .25;
    }
    int sign = pixels < 0 ? -1 : 1;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        int step = steps[i];
        double travel = (0.25 * step + 0.055 * step * step) * scale;
        int count = (int)floor(remaining / travel);
        /* Partial moves are confirmed before continuing. A stale low scale must
         * not queue seconds of uninterruptible movement behind a held button. */
        if (count > *reports_left)
            count = *reports_left;
        for (int j = 0; j < count; j++)
            emit_mouse(p, horizontal ? sign * step : 0, horizontal ? 0 : sign * step);
        *reports_left -= count;
        moved += count * travel;
        remaining -= count * travel;
    }
    if (correction && !moved && *reports_left > 0) {
        /* Quantization must not turn a real correction into an empty batch
         * that never reveals the cursor. Still confirm this one-unit result. */
        emit_mouse(p, horizontal ? sign : 0, horizontal ? 0 : sign);
        (*reports_left)--;
        moved = (.25 + .055) * scale;
    }
    return sign * moved;
}

static void anchor(ph_pointer *p)
{
    /* Never anchor with a button held: that would turn recovery into a swipe. */
    for (int i = 0; i < 8; i++)
        emit_mouse(p, -127, -127);
    p->output.delay(p->output.user, 150);
    double width = model_width(p->aspect);
    p->hint_x = CORNER / width;
    p->hint_y = CORNER * p->aspect / width;
    p->search_radius = 40.0 / width;
    p->anchoring = true;
    p->learn_move = false;
    sync(p, PH_POINTER_MOVING);
}

static void move_to(ph_pointer *p, double x, double y)
{
    double width = model_width(p->aspect);
    double dx = (x - p->actual_x) * width;
    double dy = (y - p->actual_y) * width / p->aspect;
    p->sent_start_x = p->actual_x;
    p->sent_start_y = p->actual_y;
    int reports_left = MAX_MOVE_REPORTS;
    p->sent_dx = move_axis(p, dx, true, &reports_left);
    p->sent_dy = move_axis(p, dy, false, &reports_left);
    p->hint_x = p->actual_x + p->sent_dx / width;
    p->hint_y = p->actual_y + p->sent_dy * p->aspect / width;
    p->search_radius = fmin(0.9, (40 + hypot(dx, dy) * 0.65) / width);
    if (p->buttons)
        p->search_radius = fmin(p->search_radius, 40.0 / width);
    /* Scrolling/dragged content can contain convincing circular UI elements.
     * Its animation must never calibrate or redirect a held mouse button. */
    p->learn_move = p->buttons == 0;
    p->anchoring = false;
    p->attempts++;
    sync(p, PH_POINTER_MOVING);
}

static void reveal(ph_pointer *p)
{
    emit_mouse(p, 2, 0);
    p->sent_start_x = p->actual_x;
    p->sent_start_y = p->actual_y;
    p->sent_dx = .72 * (p->scale_x ? p->scale_x : 1);
    p->sent_dy = 0;
    p->hint_x = p->actual_x + p->sent_dx / model_width(p->aspect);
    p->hint_y = p->actual_y;
    p->search_radius = 40.0 / model_width(p->aspect);
    p->learn_move = false;
    p->anchoring = false;
    sync(p, PH_POINTER_MOVING);
}

static void pop_action(ph_pointer *p)
{
    p->head = (p->head + 1) % PH_POINTER_QUEUE;
    p->count--;
    p->attempts = 0;
    p->recoveries = 0;
}

static void fail(ph_pointer *p)
{
    ph_pointer_cancel(p, p->now_ms);
    p->confirm_after_drain = false;
    if (p->phase == PH_POINTER_WAIT_VIDEO)
        p->phase = PH_POINTER_IDLE;
    p->known = false;
    p->failed = true;
    p->retry_at = p->now_ms + 500;
}

static void pump(ph_pointer *p)
{
    if (!p->enabled || !p->active || p->phase != PH_POINTER_IDLE || p->now_ms < p->retry_at
        || !p->output.mouse || !p->output.delay || !p->output.sync)
        return;
    double x = p->target_x, y = p->target_y;
    if (p->count) {
        const ph_pointer_action *action = &p->actions[p->head];
        x = action->x;
        y = action->y;
        p->aspect = action->aspect;
    }
    if (!p->known) {
        if (p->buttons)
            fail(p);
        else
            anchor(p);
        return;
    }
    double tolerance = p->count || p->buttons || !p->target_tolerance
                           ? ALIGNMENT_PX : p->target_tolerance;
    if (distance(x, y, p->actual_x, p->actual_y, p->aspect) > tolerance) {
        if (p->attempts >= MAX_ATTEMPTS) {
            fail(p);
            return;
        }
        move_to(p, x, y);
        return;
    }
    p->attempts = 0;
    p->recoveries = 0;
    if (!p->count)
        return;
    ph_pointer_action action = p->actions[p->head];
    if (action.down && p->now_ms - p->observed_at > POSITION_MAX_AGE_MS) {
        /* A tiny movement reveals a hidden pointer before a click. It is not a
         * click, and its resulting position still has to be confirmed in video. */
        reveal(p);
        return;
    }
    if (!action.down && p->now_ms < p->hold_until)
        return;
    if (action.down) {
        p->buttons |= action.button;
        p->hold_until = p->now_ms + MIN_PRESS_MS;
    } else {
        p->buttons &= (uint8_t)~action.button;
    }
    emit_mouse(p, 0, 0);
    pop_action(p);
    sync(p, PH_POINTER_BUTTON);
}

void ph_pointer_cancel(ph_pointer *p, uint64_t now_ms)
{
    p->now_ms = now_ms;
    bool pending = awaiting_ack(p);
    bool waiting_video = p->phase == PH_POINTER_WAIT_VIDEO;
    bool moving = p->phase == PH_POINTER_MOVING;
    /* Releasing a held drag sends a new fence/baseline and invalidates its
     * path geometry. Retain endpoint feedback only for unpressed motion. */
    bool confirming = !p->buttons && (moving || waiting_video || p->confirm_after_drain);
    if (moving || waiting_video || confirming)
        p->known = false;
    /* Cancelling our intent cannot retract reports already in the helper's
     * pipe. Keep their fence so rapid leave/re-enter events cannot flood it
     * with corner sweeps while ignoring every superseded acknowledgement. */
    p->phase = pending ? PH_POINTER_DRAINING
                      : waiting_video && confirming ? PH_POINTER_WAIT_VIDEO : PH_POINTER_IDLE;
    p->confirm_after_drain = pending && confirming;
    p->active = false;
    p->failed = false;
    p->head = p->count = 0;
    p->attempts = p->recoveries = 0;
    p->requested_buttons = 0;
    if (p->buttons) {
        p->buttons = 0;
        if (p->output.mouse) {
            emit_mouse(p, 0, 0);
            if (p->output.sync) {
                p->confirm_after_drain = confirming;
                sync(p, PH_POINTER_DRAINING); /* include the release in the drain */
            }
        }
    }
}

void ph_pointer_reset(ph_pointer *p, uint64_t now_ms)
{
    ph_pointer_cancel(p, now_ms);
    p->confirm_after_drain = false;
    if (p->phase == PH_POINTER_WAIT_VIDEO)
        p->phase = PH_POINTER_IDLE;
    p->known = false;
    p->aspect = 0;
    p->failed = false;
    p->retry_at = 0;
}

void ph_pointer_set_enabled(ph_pointer *p, bool enabled, uint64_t now_ms)
{
    if (p->enabled == enabled)
        return;
    ph_pointer_reset(p, now_ms);
    p->enabled = enabled;
}

void ph_pointer_transport_reset(ph_pointer *p, uint64_t now_ms)
{
    ph_pointer_reset(p, now_ms);
    p->phase = PH_POINTER_IDLE;
    p->ack_timed_out = false;
}

bool ph_pointer_set_scale(ph_pointer *p, double x, double y)
{
    if (!isfinite(x) || !isfinite(y) || x < 0.05 || x > 20 || y < 0.05 || y > 20)
        return false;
    if (p->scale_x != x || p->scale_y != y) {
        ph_pointer_reset(p, p->now_ms);
        p->scale_x = x;
        p->scale_y = y;
    }
    return true;
}

static bool target(ph_pointer *p, double x, double y, double aspect, uint64_t now_ms,
                   double tolerance)
{
    if (!p->enabled || !valid(x, y, aspect))
        return false;
    if (p->phase == PH_POINTER_DRAINING && ack_expired(p, now_ms)) {
        p->now_ms = now_ms;
        p->failed = true;
        p->ack_timed_out = true;
        return false; /* wait for transport recovery, even if the target changes */
    }
    reachable(&x, &y, aspect);
    bool changed = !p->aspect || fabs(p->aspect - aspect) > 0.0001
                   || distance(x, y, p->target_x, p->target_y, aspect) > ALIGNMENT_PX;
    if (p->failed && !changed) {
        p->now_ms = now_ms;
        return false; /* repeated scroll polling must not restart a failed sweep */
    }
    if (p->aspect && fabs(p->aspect - aspect) > 0.0001)
        ph_pointer_reset(p, now_ms);
    if (!p->active || changed) {
        p->attempts = 0;
        /* Hover changes cannot renew recovery while the original click waits. */
        if (!p->count)
            p->recoveries = 0;
        p->failed = false;
    }
    p->now_ms = now_ms;
    p->target_x = x;
    p->target_y = y;
    p->aspect = aspect;
    p->target_tolerance = tolerance;
    p->active = true;
    if (p->phase == PH_POINTER_IDLE && p->known && !p->count && !p->buttons
        && now_ms - p->observed_at > POSITION_MAX_AGE_MS
        && distance(x, y, p->actual_x, p->actual_y, aspect) <= tolerance) {
        reveal(p);
        return true;
    }
    pump(p);
    return true;
}

bool ph_pointer_target(ph_pointer *p, double x, double y, double aspect, uint64_t now_ms)
{
    return target(p, x, y, aspect, now_ms, ALIGNMENT_PX);
}

bool ph_pointer_button(ph_pointer *p, uint8_t button, bool down, double x, double y,
                       double aspect, uint64_t now_ms)
{
    if (!p->enabled || !valid(x, y, aspect) || !button || (button & (button - 1)))
        return false;
    if (p->phase == PH_POINTER_DRAINING && ack_expired(p, now_ms)) {
        p->ack_timed_out = true;
        return false;
    }
    reachable(&x, &y, aspect);
    if (p->aspect && fabs(p->aspect - aspect) > 0.0001)
        ph_pointer_reset(p, now_ms);
    bool requested = (p->requested_buttons & button) != 0;
    if (down == requested)
        return true;
    if (p->count >= PH_POINTER_QUEUE) {
        fail(p); /* never discard just a release and leave the phone dragging */
        return false;
    }
    if (down)
        p->requested_buttons |= button;
    else
        p->requested_buttons &= (uint8_t)~button;
    unsigned int tail = (p->head + p->count) % PH_POINTER_QUEUE;
    p->actions[tail] = (ph_pointer_action){x, y, aspect, button, down};
    p->count++;
    p->failed = false; /* a new physical click may explicitly retry a failed hover */
    return ph_pointer_target(p, x, y, aspect, now_ms);
}

void ph_pointer_ack(ph_pointer *p, uint32_t id, uint64_t now_ms)
{
    if (id != p->sync_id || !(id & 0x80000000U))
        return;
    p->ack_timed_out = false;
    p->now_ms = now_ms;
    if (p->phase == PH_POINTER_MOVING) {
        p->phase = PH_POINTER_WAIT_VIDEO;
        p->wait_since = now_ms;
    } else if (p->phase == PH_POINTER_DRAINING) {
        /* Cancelling a target cannot undo its queued motion. Confirm where it
         * actually stopped before re-entering, rather than sweeping a corner
         * every time the PC mouse briefly leaves the window. */
        p->phase = p->confirm_after_drain ? PH_POINTER_WAIT_VIDEO : PH_POINTER_IDLE;
        p->confirm_after_drain = false;
        p->wait_since = now_ms;
        p->failed = false;
        p->retry_at = now_ms;
        pump(p);
    } else if (p->phase == PH_POINTER_BUTTON) {
        p->phase = PH_POINTER_IDLE;
        pump(p);
    }
}

static double learned_scale(double scale, double expected, double actual)
{
    if (!scale)
        scale = 1;
    if (fabs(expected) < 20 || expected * actual <= 0)
        return scale;
    double ratio = actual / expected;
    if (ratio < 0.4 || ratio > 2.5)
        return scale;
    return fmin(20, fmax(0.05, scale * ratio));
}

static bool held_observation(const ph_pointer *p, double x, double y)
{
    if (!p->buttons)
        return true;
    double width = model_width(p->aspect);
    double dx = (x - p->sent_start_x) * width;
    double dy = (y - p->sent_start_y) * width / p->aspect;
    double length = hypot(p->sent_dx, p->sent_dy);
    if (distance(x, y, p->hint_x, p->hint_y, p->aspect) > 40)
        return false;
    if (length < 0.5)
        return hypot(dx, dy) <= ALIGNMENT_PX;
    double cross_track = fabs(p->sent_dx * dy - p->sent_dy * dx) / length;
    double progress = (p->sent_dx * dx + p->sent_dy * dy) / length;
    /* A horizontal/vertical drag cannot acquire orthogonal motion from an
     * animated icon. Keep both the endpoint and its path geometrically plausible. */
    return cross_track <= 12 && progress >= -4 && progress <= length + 16;
}

void ph_pointer_observe(ph_pointer *p, bool found, double x, double y, uint64_t now_ms)
{
    p->now_ms = now_ms;
    if (p->phase != PH_POINTER_WAIT_VIDEO || now_ms < p->wait_since
        || now_ms - p->wait_since < FRAME_SETTLE_MS
        || !found || !valid(x, y, p->aspect) || !held_observation(p, x, y))
        return;
    double width = model_width(p->aspect);
    double moved_x = (x - p->sent_start_x) * width;
    double moved_y = (y - p->sent_start_y) * width / p->aspect;
    double travel = hypot(p->sent_dx, p->sent_dy);
    if (!p->anchoring && travel > 0
        && (p->sent_dx * moved_x + p->sent_dy * moved_y) / travel >= .5
        && distance(x, y, p->hint_x, p->hint_y, p->aspect) <= ALIGNMENT_PX)
        p->attempts = 0; /* confirmed progress, including a moving drag target */
    if (p->learn_move) {
        p->scale_x = learned_scale(p->scale_x, p->sent_dx, moved_x);
        p->scale_y = learned_scale(p->scale_y, p->sent_dy, moved_y);
    }
    p->actual_x = x;
    p->actual_y = y;
    p->known = true;
    p->observed_at = now_ms;
    p->phase = PH_POINTER_IDLE;
    p->anchoring = false;
    p->failed = false;
    pump(p);
}

void ph_pointer_tick(ph_pointer *p, uint64_t now_ms)
{
    p->now_ms = now_ms;
    if (ack_expired(p, now_ms)) {
        p->ack_timed_out = true;
        fail(p);
        return;
    }
    if (p->phase == PH_POINTER_WAIT_VIDEO && now_ms - p->wait_since >= FRAME_TIMEOUT_MS) {
        if (!p->active) {
            /* Video may disappear when the mouse leaves. Forget quietly; an
             * inactive cancellation must never reveal or move the phone. */
            p->phase = PH_POINTER_IDLE;
            p->known = false;
            return;
        }
        bool pending_press = p->count && p->actions[p->head].down && p->known
                             && !p->anchoring && !p->ack_timed_out;
        if ((p->count && !pending_press) || p->buttons || p->recoveries) {
            fail(p);
            return;
        }
        /* A tiny correction can disappear in the rendered video. One neutral
         * probe may reveal it, including before an unpressed queued click. The
         * new fence and fresh position feedback still precede every press. */
        p->recoveries++;
        emit_mouse(p, 2, 0);
        p->learn_move = false;
        p->anchoring = false;
        sync(p, PH_POINTER_MOVING);
        return;
    }
    pump(p);
}

bool ph_pointer_transport_stalled(const ph_pointer *p)
{
    return p->ack_timed_out;
}

static bool ready(const ph_pointer *p, double x, double y, double aspect, double tolerance)
{
    if (!valid(x, y, aspect))
        return false;
    reachable(&x, &y, aspect);
    return p->enabled && p->known && p->phase == PH_POINTER_IDLE && !p->count && !p->buttons
           && fabs(p->aspect - aspect) < 0.0001
           && p->now_ms - p->observed_at <= POSITION_MAX_AGE_MS
           && distance(x, y, p->actual_x, p->actual_y, aspect) <= tolerance;
}

bool ph_pointer_ready(const ph_pointer *p, double x, double y, double aspect)
{
    return ready(p, x, y, aspect, ALIGNMENT_PX);
}

bool ph_pointer_scroll_target(ph_pointer *p, double x, double y, double aspect, uint64_t now_ms)
{
    return target(p, x, y, aspect, now_ms, SCROLL_ALIGNMENT_PX)
           && ready(p, x, y, aspect, SCROLL_ALIGNMENT_PX);
}

bool ph_pointer_hint(const ph_pointer *p, double *x, double *y, double *radius)
{
    if (!x || !y || !radius || (p->phase != PH_POINTER_MOVING
                              && p->phase != PH_POINTER_WAIT_VIDEO))
        return false;
    *x = p->hint_x;
    *y = p->hint_y;
    *radius = p->search_radius;
    return true;
}
