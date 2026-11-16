#ifndef PORTHOLE_CONTROL_POINTER_H
#define PORTHOLE_CONTROL_POINTER_H

#include "hid/hid_reports.h"

#define PH_POINTER_QUEUE 16

typedef struct ph_pointer_output {
    void *user;
    void (*mouse)(void *user, const ph_hid_mouse *report);
    void (*delay)(void *user, uint16_t milliseconds);
    void (*sync)(void *user, uint32_t id);
} ph_pointer_output;

typedef struct ph_pointer_action {
    double x, y, aspect;
    uint8_t button;
    bool down;
} ph_pointer_action;

typedef enum ph_pointer_phase {
    PH_POINTER_IDLE,
    PH_POINTER_MOVING,
    PH_POINTER_BUTTON,
    PH_POINTER_WAIT_VIDEO,
    PH_POINTER_DRAINING, /* cancelled output still queued in the transport */
} ph_pointer_phase;

/* Main-thread state. Zero-initialize, assign output, then enable when HID is ready.
 * Positions are fractions of the phone picture. No button is pressed on an estimate:
 * a movement must receive a fresh, confirmed video position first. iOS constrains
 * the dot centre to an inset of 16 model pixels (500 pixels on the short edge);
 * edge requests use the closest reachable point, including button actions and
 * wheel readiness. */
typedef struct ph_pointer {
    ph_pointer_output output;
    bool enabled, active, known, failed;
    ph_pointer_phase phase;
    double target_x, target_y, aspect;
    double target_tolerance;
    double actual_x, actual_y;
    double scale_x, scale_y;
    double hint_x, hint_y, search_radius;
    double sent_start_x, sent_start_y, sent_dx, sent_dy;
    bool learn_move, anchoring;
    uint8_t buttons, requested_buttons;
    ph_pointer_action actions[PH_POINTER_QUEUE];
    unsigned int head, count, attempts, recoveries;
    uint32_t sync_id;
    uint64_t sent_at, wait_since, observed_at, hold_until, retry_at, now_ms;
} ph_pointer;

void ph_pointer_set_enabled(ph_pointer *p, bool enabled, uint64_t now_ms);
bool ph_pointer_set_scale(ph_pointer *p, double x, double y);
bool ph_pointer_target(ph_pointer *p, double x, double y, double aspect, uint64_t now_ms);
/* Position for a wheel gesture, accepting fresh video within 20 model pixels.
 * Returns readiness; clicks and held drags continue to require 4-pixel alignment. */
bool ph_pointer_scroll_target(ph_pointer *p, double x, double y, double aspect, uint64_t now_ms);
bool ph_pointer_button(ph_pointer *p, uint8_t button, bool down, double x, double y,
                       double aspect, uint64_t now_ms);
void ph_pointer_ack(ph_pointer *p, uint32_t id, uint64_t now_ms);
void ph_pointer_observe(ph_pointer *p, bool found, double x, double y, uint64_t now_ms);
void ph_pointer_tick(ph_pointer *p, uint64_t now_ms);
bool ph_pointer_ready(const ph_pointer *p, double x, double y, double aspect);
/* Search radius is expressed as a fraction of picture WIDTH, even for Y search. */
bool ph_pointer_hint(const ph_pointer *p, double *x, double *y, double *radius);
/* Cancel intent and release held buttons; reset also forgets the location.
 * Both retain a transport fence while already-sent reports drain. */
void ph_pointer_cancel(ph_pointer *p, uint64_t now_ms);
void ph_pointer_reset(ph_pointer *p, uint64_t now_ms);
/* Only when the old link/helper is gone: discard its unacknowledgeable fence. */
void ph_pointer_transport_reset(ph_pointer *p, uint64_t now_ms);

#endif
