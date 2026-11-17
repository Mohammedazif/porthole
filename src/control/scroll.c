#include "control/scroll.h"

#include <math.h>
#include <stdlib.h>

#define MODEL_WIDTH 500.0
#define CORNER 16.0
#define ANCHOR_REPORTS 8
#define REPORTS_PER_NOTCH 3
#define NOTCHES_PER_BATCH 1
#define MAX_PENDING 4
#define POSITION_SLOP 20.0 /* tolerate small PC cursor movements during a wheel gesture */
#define FRAME_SETTLE_MS 32
#define FRAME_TIMEOUT_MS 180
#define GESTURE_IDLE_MS 500
#define ACK_TIMEOUT_MS 3000

static void start_batch(ph_scroll *scroll);

void ph_scroll_forget_bounds(ph_scroll *scroll)
{
    scroll->have_bar = false;
    scroll->max_bar_length = 0;
    scroll->track_top = 1;
    scroll->track_bottom = 0;
    scroll->blocked_y = 0;
    scroll->pinned_batches = 0;
    scroll->pinned_direction = 0;
    scroll->sent_had_bar = false;
}

static int clamp(int value, int limit)
{
    return value < -limit ? -limit : value > limit ? limit : value;
}

static int accumulate(int current, int delta)
{
    if ((current > 0 && delta < 0) || (current < 0 && delta > 0))
        current = 0; /* reversing direction replaces queued movement immediately */
    int64_t sum = (int64_t)current + delta;
    return sum < -MAX_PENDING ? -MAX_PENDING : sum > MAX_PENDING ? MAX_PENDING : (int)sum;
}

static void move_axis(ph_scroll *scroll, double distance, bool horizontal)
{
    static const int steps[] = {32, 16, 8, 4, 2, 1};
    double remaining = fabs(distance);
    int sign = distance < 0 ? -1 : 1;
    double scale = horizontal ? scroll->scale_x : scroll->scale_y;
    if (scale == 0)
        scale = 1;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        int step = steps[i];
        double pixels = (0.25 * step + 0.055 * step * step) * scale;
        int count = (int)floor(remaining / pixels);
        ph_hid_mouse mouse = {0};
        if (horizontal)
            mouse.dx = (int16_t)(sign * step);
        else
            mouse.dy = (int16_t)(sign * step);
        for (int j = 0; j < count; j++)
            scroll->output.mouse(scroll->output.user, &mouse);
        remaining -= count * pixels;
    }
}

static void start_batch(ph_scroll *scroll)
{
    if (scroll->failed || scroll->in_flight || scroll->waiting_frame
        || (!scroll->pending_x && !scroll->pending_y))
        return;
    if (scroll->position && !scroll->position(scroll->output.user, scroll->target_x,
                                               scroll->target_y, scroll->aspect))
        return;

    int x = clamp(scroll->pending_x, NOTCHES_PER_BATCH);
    int y = clamp(scroll->pending_y, NOTCHES_PER_BATCH);
    scroll->pending_x -= x;
    scroll->pending_y -= y;
    scroll->in_flight = true;
    scroll->sent_at = scroll->now_ms;
    scroll->sent_y = y;
    scroll->sent_had_bar = scroll->have_bar;
    scroll->sent_bar_top = scroll->bar_top;
    scroll->sent_bar_bottom = scroll->bar_bottom;

    bool anchor_needed = !scroll->positioned
                         || fabs(scroll->last_aspect - scroll->aspect) > 0.0001;
    double dx = (scroll->target_x - scroll->last_x) * MODEL_WIDTH;
    double dy = (scroll->target_y - scroll->last_y) * MODEL_WIDTH / scroll->aspect;
    if (!scroll->position && anchor_needed) {
        ph_hid_mouse anchor = {.dx = -127, .dy = -127};
        for (int i = 0; i < ANCHOR_REPORTS; i++)
            scroll->output.mouse(scroll->output.user, &anchor);
        scroll->output.delay(scroll->output.user, 150);
        move_axis(scroll, fmax(0, scroll->target_x * MODEL_WIDTH - CORNER), true);
        move_axis(scroll, fmax(0, scroll->target_y * MODEL_WIDTH / scroll->aspect - CORNER), false);
    } else if (!scroll->position && hypot(dx, dy) > POSITION_SLOP) {
        /* Follow a changed PC target without jumping through the top-left corner. */
        move_axis(scroll, dx, true);
        move_axis(scroll, dy, false);
    }
    if (anchor_needed || hypot(dx, dy) > POSITION_SLOP) {
        scroll->last_x = scroll->target_x;
        scroll->last_y = scroll->target_y;
        scroll->last_aspect = scroll->aspect;
        scroll->positioned = true;
        if (!scroll->position)
            scroll->output.delay(scroll->output.user, 60);
    }

    /* Keep normal wheel travel responsive. Close to a vertical edge, probe with
     * one unit so video feedback can stop the gesture without a large stretch. */
    int strength = scroll->feedback ? REPORTS_PER_NOTCH : 1;
    if (scroll->probe || (scroll->have_bar && ((y > 0 && scroll->bar_top < 0.2)
                            || (y < 0 && scroll->bar_bottom > 0.8))))
        strength = 1;
    scroll->probe = false;
    int nx = abs(x);
    int ny = abs(y) * strength;
    int count = nx > ny ? nx : ny;
    for (int i = 0; i < count; i++) {
        ph_hid_mouse mouse = {0};
        /* iOS uses natural vertical scrolling. Keep each axis's notch count distinct. */
        mouse.wheel = (int8_t)(i < ny ? (y > 0 ? -1 : 1) : 0);
        mouse.pan = (int8_t)(i < nx ? (x > 0 ? 1 : -1) : 0);
        scroll->output.mouse(scroll->output.user, &mouse);
    }
    ph_hid_mouse released = {0};
    scroll->output.mouse(scroll->output.user, &released);
    scroll->sync_id++;
    if (!scroll->sync_id)
        scroll->sync_id++;
    scroll->output.sync(scroll->output.user, scroll->sync_id);
}

bool ph_scroll_request(ph_scroll *scroll, double x, double y, double aspect,
                       int notches_x, int notches_y)
{
    if (!scroll->output.mouse || !scroll->output.delay || !scroll->output.sync
        || !isfinite(x) || !isfinite(y) || !isfinite(aspect) || x < 0 || x >= 1
        || y < 0 || y >= 1 || aspect < 0.1 || aspect > 10 || (!notches_x && !notches_y)
        || scroll->failed)
        return false;
    if (scroll->have_request && scroll->now_ms >= scroll->last_request_at
        && scroll->now_ms - scroll->last_request_at >= GESTURE_IDLE_MS) {
        /* The phone can navigate or discover more list entries between gestures.
         * Retry with one unit, rather than retaining another page's edge forever. */
        ph_scroll_cancel(scroll);
        ph_scroll_forget_bounds(scroll);
        scroll->probe = true;
    }
    scroll->have_request = true;
    scroll->last_request_at = scroll->now_ms;
    /* A different point in the same page does not change its scroll limits.
     * Navigation and button actions explicitly forget bounds in the app. */
    if (scroll->aspect > 0 && fabs(scroll->aspect - aspect) > 0.0001)
        ph_scroll_forget_bounds(scroll);
    scroll->target_x = x;
    scroll->target_y = y;
    scroll->aspect = aspect;
    scroll->pending_x = accumulate(scroll->pending_x, notches_x);
    if (scroll->blocked_y && notches_y
        && (notches_y > 0 ? 1 : -1) != scroll->blocked_y)
        scroll->blocked_y = 0; /* reversing can leave the current stopped gesture */
    if (scroll->blocked_y && (notches_y > 0 ? 1 : -1) == scroll->blocked_y)
        notches_y = 0;
    if (notches_y)
        scroll->pending_y = accumulate(scroll->pending_y, notches_y);
    start_batch(scroll);
    return true;
}

void ph_scroll_ack_at(ph_scroll *scroll, uint32_t id, uint64_t now_ms)
{
    scroll->now_ms = now_ms;
    if (scroll->in_flight && id == scroll->sync_id) {
        bool cancelled = scroll->cancelled || scroll->failed;
        scroll->in_flight = false;
        scroll->cancelled = false;
        scroll->failed = false;
        scroll->waiting_frame = scroll->feedback && !cancelled;
        if (cancelled)
            scroll->sent_y = 0;
        scroll->wait_since = scroll->now_ms;
        start_batch(scroll);
    }
}

void ph_scroll_ack(ph_scroll *scroll, uint32_t id)
{
    ph_scroll_ack_at(scroll, id, scroll->now_ms);
}

void ph_scroll_cancel(ph_scroll *scroll)
{
    scroll->pending_x = scroll->pending_y = 0;
    scroll->cancelled = scroll->in_flight;
    scroll->waiting_frame = false;
    scroll->pinned_batches = 0;
}

void ph_scroll_reset(ph_scroll *scroll)
{
    ph_scroll_cancel(scroll);
    scroll->positioned = false;
    scroll->have_request = false;
    scroll->probe = false;
    ph_scroll_forget_bounds(scroll);
}

void ph_scroll_transport_reset(ph_scroll *scroll)
{
    ph_scroll_reset(scroll);
    scroll->in_flight = false;
    scroll->cancelled = false;
    scroll->failed = false;
}

void ph_scroll_tick(ph_scroll *scroll, uint64_t now_ms)
{
    scroll->now_ms = now_ms;
    if (scroll->in_flight && now_ms - scroll->sent_at >= ACK_TIMEOUT_MS) {
        /* Keep the fence until a late ACK or explicit link reset. A new cursor
         * position must not turn a missing ACK into unlimited queued batches. */
        ph_scroll_cancel(scroll);
        scroll->failed = true;
        return;
    }
    if (scroll->waiting_frame && now_ms - scroll->wait_since >= FRAME_TIMEOUT_MS) {
        scroll->waiting_frame = false;
    }
    start_batch(scroll);
}

void ph_scroll_observe(ph_scroll *scroll, bool visible, double top, double bottom,
                       uint64_t now_ms)
{
    scroll->now_ms = now_ms;
    if (visible && isfinite(top) && isfinite(bottom) && top >= 0 && top < bottom
        && bottom <= 1) {
        const double tolerance = 0.005;
        double length = bottom - top;
        if (!scroll->have_bar) {
            scroll->track_top = top;
            scroll->track_bottom = bottom;
        }
        scroll->track_top = fmin(scroll->track_top, top);
        scroll->track_bottom = fmax(scroll->track_bottom, bottom);
        scroll->max_bar_length = fmax(scroll->max_bar_length, length);
        bool compressed = length < scroll->max_bar_length - tolerance;
        /* Large titles and bottom bars can resize the track during a gesture.
         * A compressed thumb at either end still means overscroll when its
         * current track origin differs from an earlier frame's origin. */
        bool at_top = top < 0.2 && (top <= scroll->track_top + tolerance || compressed);
        bool at_bottom = bottom > 0.8
                         && (bottom >= scroll->track_bottom - tolerance || compressed);
        bool fresh = scroll->waiting_frame && now_ms - scroll->wait_since >= FRAME_SETTLE_MS;
        bool pinned = fresh && scroll->sent_had_bar
                      && fabs(top - scroll->sent_bar_top) < 0.0005
                      && fabs(bottom - scroll->sent_bar_bottom) < 0.0005;
        if (fresh) {
            if (!pinned || scroll->pinned_direction != scroll->sent_y)
                scroll->pinned_batches = 0;
            if (pinned)
                scroll->pinned_batches++;
            scroll->pinned_direction = scroll->sent_y;
        }
        bool stopped = pinned && scroll->pinned_batches >= 2;
        /* UIKit shortens its scroll indicator when content is pulled past an edge.
         * Stop the continuing gesture rather than scaling every wheel event down. */
        if (fresh && scroll->sent_had_bar && (compressed || stopped)
            && at_top && scroll->sent_y > 0)
            scroll->blocked_y = 1;
        else if (fresh && scroll->sent_had_bar && (compressed || stopped)
                 && at_bottom && scroll->sent_y < 0)
            scroll->blocked_y = -1;
        /* Keep the edge latched through rebound, fading and detector noise.
         * A reversal or a fresh gesture releases it; navigation resets it too. */
        if (scroll->blocked_y && scroll->pending_y
            && (scroll->pending_y > 0 ? 1 : -1) == scroll->blocked_y)
            scroll->pending_y = 0;
        scroll->bar_top = top;
        scroll->bar_bottom = bottom;
        scroll->have_bar = true;
    }
    if (scroll->waiting_frame && now_ms - scroll->wait_since >= FRAME_SETTLE_MS) {
        scroll->waiting_frame = false;
        start_batch(scroll);
    }
}

bool ph_scroll_set_scale(ph_scroll *scroll, double scale_x, double scale_y)
{
    if (!isfinite(scale_x) || !isfinite(scale_y) || scale_x < 0.05 || scale_x > 20
        || scale_y < 0.05 || scale_y > 20)
        return false;
    if (scroll->scale_x != scale_x || scroll->scale_y != scale_y) {
        ph_scroll_reset(scroll);
        scroll->scale_x = scale_x;
        scroll->scale_y = scale_y;
    }
    return true;
}
