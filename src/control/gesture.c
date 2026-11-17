#include "control/gesture.h"

#include <math.h>
#include <string.h>

#define MIN_PRESS_MS 80
#define VIDEO_TIMEOUT_MS 700
#define POSITION_MAX_AGE_MS 1200

enum { ALIGN, PRESS_ACK, HOLDING, SWIPING, RELEASE_ACK, GAP };

static bool coordinate(double x, double y)
{
    return isfinite(x) && isfinite(y) && x >= 0 && x < 1 && y >= 0 && y < 1;
}

static bool geometry(int width, int height)
{
    double aspect = height > 0 ? (double)width / height : 0;
    return width >= 80 && height >= 80 && aspect >= .1 && aspect <= 10;
}

static void finish(ph_gesture *g, ph_gesture_result result, uint64_t now_ms)
{
    if (result != PH_GESTURE_SUCCEEDED)
        ph_pointer_cancel(g->pointer, now_ms);
    g->active = false;
    g->result = result;
}

static bool target(ph_gesture *g, double x, double y, uint64_t now_ms)
{
    bool accepted = ph_pointer_target(g->pointer, x, y,
                                      (double)g->request.width / g->request.height, now_ms);
    g->target_x = g->pointer->target_x;
    g->target_y = g->pointer->target_y;
    return accepted;
}

/* ph_pointer_ready intentionally excludes held buttons. Use the same model,
 * reachable inset and strict four-pixel tolerance for a held swipe endpoint. */
static bool held_aligned(const ph_gesture *g, uint64_t now_ms)
{
    const ph_pointer *p = g->pointer;
    double aspect = (double)g->request.width / g->request.height;
    double width = 500 * fmax(1, aspect);
    double x = fmin(1 - 16 / width, fmax(16 / width, g->request.end_x));
    double y = fmin(1 - 16 * aspect / width, fmax(16 * aspect / width, g->request.end_y));
    return p->known && p->phase == PH_POINTER_IDLE && !p->count
           && p->buttons == PH_HID_BTN_LEFT && fabs(p->aspect - aspect) < .0001
           && now_ms >= p->observed_at && now_ms - p->observed_at <= POSITION_MAX_AGE_MS
           && hypot((x - p->actual_x) * width, (y - p->actual_y) * width / aspect) <= 4;
}

static void press(ph_gesture *g, uint64_t now_ms)
{
    ph_pointer *p = g->pointer;
    if (!ph_pointer_button(p, PH_HID_BTN_LEFT, true, g->request.start_x, g->request.start_y,
                            (double)g->request.width / g->request.height, now_ms)
        || p->phase != PH_POINTER_BUTTON || p->buttons != PH_HID_BTN_LEFT || p->count) {
        finish(g, PH_GESTURE_FAILED, now_ms);
        return;
    }
    g->stage = PRESS_ACK;
    g->fence = p->sync_id;
}

static void release(ph_gesture *g, uint64_t now_ms)
{
    ph_pointer *p = g->pointer;
    if (!ph_pointer_button(p, PH_HID_BTN_LEFT, false, g->target_x, g->target_y,
                            (double)g->request.width / g->request.height, now_ms)
        || p->phase != PH_POINTER_BUTTON || p->buttons || p->count) {
        finish(g, PH_GESTURE_FAILED, now_ms);
        return;
    }
    g->stage = RELEASE_ACK;
    g->fence = p->sync_id;
    g->released_at = now_ms;
}

void ph_gesture_init(ph_gesture *g, ph_pointer *pointer)
{
    if (!g)
        return;
    memset(g, 0, sizeof *g);
    g->pointer = pointer;
}

bool ph_gesture_start(ph_gesture *g, const ph_gesture_request *request, uint64_t now_ms)
{
    if (!g || !request || !g->pointer || g->active
        || request->kind < PH_GESTURE_TAP || request->kind > PH_GESTURE_SWIPE
        || !geometry(request->width, request->height)
        || !coordinate(request->start_x, request->start_y)
        || !coordinate(request->end_x, request->end_y)
        || (request->press_ms && request->press_ms < MIN_PRESS_MS)
        || request->press_ms > 2000 || request->gap_ms > 2000
        || request->double_tap_window_ms > 1000
        || request->duration_ms > 5000 || request->timeout_ms > 30000)
        return false;
    uint32_t gap = request->gap_ms ? request->gap_ms : 100;
    uint32_t window = request->double_tap_window_ms ? request->double_tap_window_ms : 500;
    if (request->kind == PH_GESTURE_DOUBLE_TAP && gap >= window)
        return false;
    ph_pointer *p = g->pointer;
    if (!p->enabled || !p->output.mouse || !p->output.delay || !p->output.sync
        || p->buttons || p->requested_buttons || p->count || p->failed
        || ph_pointer_transport_stalled(p) || p->phase == PH_POINTER_BUTTON
        || p->phase == PH_POINTER_DRAINING)
        return false;

    g->request = *request;
    if (!g->request.press_ms)
        g->request.press_ms = MIN_PRESS_MS;
    if (!g->request.gap_ms)
        g->request.gap_ms = gap;
    if (!g->request.double_tap_window_ms)
        g->request.double_tap_window_ms = window;
    if (!g->request.duration_ms)
        g->request.duration_ms = request->kind == PH_GESTURE_HOLD ? 600 : 500;
    if (!g->request.timeout_ms)
        g->request.timeout_ms = request->kind == PH_GESTURE_SWIPE ? 10000 : 5000;
    g->started_at = g->last_ms = now_ms;
    g->stage_at = 0;
    g->tap = 0;
    g->active = true;
    g->result = PH_GESTURE_RUNNING;
    g->stage = ALIGN;
    /* An unpressed hover may already be in flight. Drain/confirm its original
     * fence rather than pretending it was retracted or queuing a second anchor. */
    ph_pointer_cancel(p, now_ms);
    if (!target(g, request->start_x, request->start_y, now_ms)) {
        finish(g, PH_GESTURE_FAILED, now_ms);
        return false;
    }
    return true;
}

void ph_gesture_tick(ph_gesture *g, int width, int height, uint64_t now_ms)
{
    if (!g || !g->active)
        return;
    ph_pointer *p = g->pointer;
    double aspect = (double)g->request.width / g->request.height;
    if (width != g->request.width || height != g->request.height || now_ms < g->last_ms
        || !p->enabled || !p->active || fabs(p->aspect - aspect) > .0001
        || fabs(p->target_x - g->target_x) > .000001
        || fabs(p->target_y - g->target_y) > .000001 || p->count) {
        finish(g, PH_GESTURE_CANCELLED, now_ms);
        return;
    }
    g->last_ms = now_ms;
    if (now_ms - g->started_at >= g->request.timeout_ms) {
        finish(g, PH_GESTURE_TIMED_OUT, now_ms);
        return;
    }
    if (g->request.kind == PH_GESTURE_DOUBLE_TAP && g->tap == 1
        && (g->stage == GAP || g->stage == ALIGN)
        && now_ms - g->released_at >= g->request.double_tap_window_ms) {
        /* Do not turn delayed ACK/reconfirmation into a second single tap
         * seconds later. This is an enqueue policy, not iOS recognition timing. */
        finish(g, PH_GESTURE_FAILED, now_ms);
        return;
    }
    if (p->failed || ph_pointer_transport_stalled(p)
        || (p->phase == PH_POINTER_WAIT_VIDEO && now_ms >= p->wait_since
            && now_ms - p->wait_since >= VIDEO_TIMEOUT_MS)) {
        finish(g, PH_GESTURE_FAILED, now_ms);
        return;
    }
    ph_pointer_tick(p, now_ms);
    if (p->failed || ph_pointer_transport_stalled(p)) {
        finish(g, PH_GESTURE_FAILED, now_ms);
        return;
    }

    switch (g->stage) {
    case ALIGN:
        if (ph_pointer_ready(p, g->request.start_x, g->request.start_y, aspect))
            press(g, now_ms);
        break;
    case PRESS_ACK:
        if (p->sync_id != g->fence || p->buttons != PH_HID_BTN_LEFT) {
            finish(g, PH_GESTURE_CANCELLED, now_ms);
        } else if (p->phase == PH_POINTER_IDLE) {
            /* Start no earlier than the real press ACK. A delayed main-thread
             * tick is conservative: it lengthens the hold rather than truncating it. */
            g->stage_at = now_ms;
            g->stage = g->request.kind == PH_GESTURE_SWIPE ? SWIPING : HOLDING;
        }
        break;
    case HOLDING: {
        uint32_t duration = g->request.kind == PH_GESTURE_HOLD
                                ? g->request.duration_ms : g->request.press_ms;
        if (duration < g->request.press_ms)
            duration = g->request.press_ms;
        if (now_ms - g->stage_at >= duration)
            release(g, now_ms);
        break;
    }
    case SWIPING: {
        double elapsed = (double)(now_ms - g->stage_at);
        double progress = fmin(1, elapsed / g->request.duration_ms);
        double x = g->request.start_x + (g->request.end_x - g->request.start_x) * progress;
        double y = g->request.start_y + (g->request.end_y - g->request.start_y) * progress;
        if (!target(g, x, y, now_ms)) {
            finish(g, PH_GESTURE_FAILED, now_ms);
            break;
        }
        if (progress >= 1 && elapsed >= g->request.press_ms && held_aligned(g, now_ms))
            release(g, now_ms);
        break;
    }
    case RELEASE_ACK:
        if (p->sync_id != g->fence || p->buttons) {
            finish(g, PH_GESTURE_CANCELLED, now_ms);
        } else if (p->phase == PH_POINTER_IDLE) {
            if (g->request.kind == PH_GESTURE_DOUBLE_TAP && g->tap++ == 0) {
                g->stage_at = now_ms;
                g->stage = GAP;
            } else {
                finish(g, PH_GESTURE_SUCCEEDED, now_ms);
            }
        }
        break;
    case GAP:
        if (now_ms - g->stage_at >= g->request.gap_ms) {
            g->stage = ALIGN;
            if (!target(g, g->request.start_x, g->request.start_y, now_ms))
                finish(g, PH_GESTURE_FAILED, now_ms);
        }
        break;
    default:
        finish(g, PH_GESTURE_FAILED, now_ms);
        break;
    }
}

void ph_gesture_cancel(ph_gesture *g, uint64_t now_ms)
{
    if (g && g->active)
        finish(g, PH_GESTURE_CANCELLED, now_ms);
}

bool ph_gesture_active(const ph_gesture *g)
{
    return g && g->active;
}

ph_gesture_result ph_gesture_get_result(const ph_gesture *g)
{
    return g ? g->result : PH_GESTURE_IDLE;
}
