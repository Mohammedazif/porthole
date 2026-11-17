#ifndef PORTHOLE_CONTROL_MOUSE_INPUT_H
#define PORTHOLE_CONTROL_MOUSE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Limit one input event without retaining an unbounded whole-notch backlog.
 * The scroll controller applies its own smaller pending/report limits. */
#define PH_MOUSE_INPUT_MAX_NOTCHES 32

typedef struct ph_mouse_input {
    double residual_x, residual_y;
    bool mapping_known, flipped, shift;
} ph_mouse_input;

/* PC/SDL numbering: 1 left, 2 middle, 3 right, 4/5 side buttons, through 8.
 * Returns the single HID button bit, or zero for an unsupported number. */
uint8_t ph_mouse_input_button(unsigned int pc_button);

/* Zero-initialization is sufficient. Reset when input intent is cancelled or
 * the target/session changes, including focus loss, help and guided setup.
 * Resetting this adapter does not cancel the scroll controller's pending work
 * or forget its learned boundary; those operations are separate. */
void ph_mouse_input_reset(ph_mouse_input *input);

/* Convert finite, precise wheel deltas to whole PC notches. Apply flipped once;
 * Shift adds vertical travel to horizontal travel, preserving a real X delta.
 * Opposite fractional input cancels the saved remainder instead of discarding
 * it on every small direction jitter. A Shift/flipped mapping change clears
 * old remainders before processing the new event.
 *
 * Returns true when at least one whole notch is available. Both output pointers
 * must be non-null and distinct; they are zeroed on a false return. Fractions
 * stay strictly between -1 and 1. Non-finite input/state resets and fails closed;
 * finite large deltas saturate the whole-notch output but retain their fraction.
 * No native iPhone natural-scroll inversion belongs here: scroll.c owns it. */
bool ph_mouse_input_wheel(ph_mouse_input *input, double precise_x, double precise_y,
                          bool flipped, bool shift, int *notches_x, int *notches_y);

#endif
