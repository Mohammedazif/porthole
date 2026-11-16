#ifndef PORTHOLE_CONTROL_SCROLL_H
#define PORTHOLE_CONTROL_SCROLL_H

#include "hid/hid_reports.h"

typedef struct ph_scroll_output {
    void *user;
    void (*mouse)(void *user, const ph_hid_mouse *report);
    void (*delay)(void *user, uint16_t milliseconds);
    void (*sync)(void *user, uint32_t id);
} ph_scroll_output;

typedef struct ph_scroll {
    ph_scroll_output output;
    /* Optional live pointer controller. False defers wheel input until aligned. */
    bool (*position)(void *user, double x, double y, double aspect);
    int pending_x, pending_y;
    double target_x, target_y, aspect;
    double last_x, last_y, last_aspect;
    double scale_x, scale_y;
    bool positioned, in_flight;
    uint32_t sync_id;
    /* Video feedback prevents continued wheel input from stretching iOS past an edge. */
    bool feedback, waiting_frame, have_bar, sent_had_bar;
    double bar_top, bar_bottom, max_bar_length, track_top, track_bottom;
    double sent_bar_top, sent_bar_bottom;
    int blocked_y, sent_y;
    uint64_t now_ms, wait_since;
} ph_scroll;

/* Target coordinates are fractions of the phone picture; aspect is its width/height.
 * Position the phone pointer before scrolling and coalesce input while a batch is sent.
 * Placement uses the measured S3 model; this does not send clicks. */
bool ph_scroll_request(ph_scroll *scroll, double x, double y, double aspect,
                       int notches_x, int notches_y);
void ph_scroll_ack(ph_scroll *scroll, uint32_t id);
/* Stop pending wheel input while retaining the known phone pointer position. */
void ph_scroll_cancel(ph_scroll *scroll);
void ph_scroll_reset(ph_scroll *scroll);
/* Enable feedback in the live app. Feed each newly decoded frame (including a faded
 * indicator) and tick the clock so a missing video frame cannot stall input forever. */
void ph_scroll_tick(ph_scroll *scroll, uint64_t now_ms);
void ph_scroll_observe(ph_scroll *scroll, bool visible, double top, double bottom,
                       uint64_t now_ms);
void ph_scroll_forget_bounds(ph_scroll *scroll);
/* Per-axis multiplier for the measured movement model; zero-initialized state uses 1. */
bool ph_scroll_set_scale(ph_scroll *scroll, double scale_x, double scale_y);

#endif
