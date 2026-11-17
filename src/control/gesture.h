#ifndef PORTHOLE_CONTROL_GESTURE_H
#define PORTHOLE_CONTROL_GESTURE_H

#include "control/pointer.h"

typedef enum ph_gesture_kind {
    PH_GESTURE_TAP,
    PH_GESTURE_DOUBLE_TAP,
    PH_GESTURE_HOLD,
    PH_GESTURE_SWIPE,
} ph_gesture_kind;

typedef enum ph_gesture_result {
    PH_GESTURE_IDLE,
    PH_GESTURE_RUNNING,
    PH_GESTURE_SUCCEEDED,
    PH_GESTURE_CANCELLED,
    PH_GESTURE_FAILED,
    PH_GESTURE_TIMED_OUT,
} ph_gesture_result;

typedef struct ph_gesture_request {
    ph_gesture_kind kind;
    double start_x, start_y, end_x, end_y; /* normalized picture coordinates */
    int width, height;                    /* current cropped picture, not window */
    uint32_t duration_ms; /* hold/swipe: 0 uses 600/500 ms; at most 5000 */
    uint32_t press_ms;    /* tap/double tap: 0 uses 80 ms; otherwise 80..2000 */
    uint32_t gap_ms;      /* double tap: 0 uses 100 ms after first release ACK */
    uint32_t double_tap_window_ms; /* 0 uses 500 ms, at most 1000: first release
                                   * enqueue to second press enqueue, not an OS promise */
    uint32_t timeout_ms;  /* 0 uses 5 seconds (swipe: 10); at most 30000 */
} ph_gesture_request;

/* Main-thread controller. All HID output goes through the supplied pointer.
 * Its caller continues delivering real pointer ACK/video observations. While
 * active, tick this controller before any separate pointer tick, so its missing
 * video deadline can cancel without a pointer recovery probe. Manual input,
 * focus loss and disconnection must cancel the gesture before other work.
 * Swipe duration is a best-effort trajectory; ACK/video alignment can extend it.
 * No touch injection, inertial flick, multi-touch or delivery guarantee is implied. */
typedef struct ph_gesture {
    ph_pointer *pointer;
    ph_gesture_request request;
    ph_gesture_result result;
    unsigned int stage, tap;
    uint32_t fence;
    uint64_t started_at, stage_at, released_at, last_ms;
    double target_x, target_y;
    bool active;
} ph_gesture;

void ph_gesture_init(ph_gesture *gesture, ph_pointer *pointer);
bool ph_gesture_start(ph_gesture *gesture, const ph_gesture_request *request, uint64_t now_ms);
void ph_gesture_tick(ph_gesture *gesture, int width, int height, uint64_t now_ms);
void ph_gesture_cancel(ph_gesture *gesture, uint64_t now_ms);
bool ph_gesture_active(const ph_gesture *gesture);
ph_gesture_result ph_gesture_get_result(const ph_gesture *gesture);

#endif
