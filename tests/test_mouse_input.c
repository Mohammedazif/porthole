/* Exercise the PC adapter without SDL, a Bluetooth helper, or phone input. */
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

#include "control/mouse_input.h"
#include "hid/hid_reports.h"

static unsigned int failures, events;

#define CHECK(condition)                                                                            \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void wheel(ph_mouse_input *input, double x, double y, bool flipped, bool shift,
                  int expected_x, int expected_y)
{
    int nx = 999, ny = 999;
    bool emitted = ph_mouse_input_wheel(input, x, y, flipped, shift, &nx, &ny);
    events++;
    if (nx != expected_x || ny != expected_y)
        fprintf(stderr, "event %u input=(%.17g,%.17g) flipped=%d shift=%d "
                        "output=(%d,%d) expected=(%d,%d)\n",
                events, x, y, flipped, shift, nx, ny, expected_x, expected_y);
    CHECK(emitted == (expected_x != 0 || expected_y != 0));
    CHECK(nx == expected_x && ny == expected_y);
    CHECK(isfinite(input->residual_x) && fabs(input->residual_x) < 1);
    CHECK(isfinite(input->residual_y) && fabs(input->residual_y) < 1);
}

static void button_numbers(void)
{
    const uint8_t expected[] = {0, PH_HID_BTN_LEFT, PH_HID_BTN_MIDDLE, PH_HID_BTN_RIGHT,
                               PH_HID_BTN_4, PH_HID_BTN_5, PH_HID_BTN_6, PH_HID_BTN_7,
                               PH_HID_BTN_8};
    unsigned int all = 0;
    for (unsigned int button = 0; button <= 255; button++) {
        uint8_t bit = ph_mouse_input_button(button);
        CHECK(bit == (button < sizeof expected / sizeof expected[0] ? expected[button] : 0));
        CHECK(!bit || !(bit & (bit - 1)));
        all |= bit;
    }
    CHECK(all == 255 && ph_mouse_input_button(UINT_MAX) == 0);
}

static void precise_accumulation(void)
{
    ph_mouse_input input = {0};
    for (int i = 0; i < 4; i++)
        wheel(&input, 0, .25, false, false, 0, i == 3 ? 1 : 0);
    CHECK(input.residual_y == 0);
    for (int i = 0; i < 10; i++)
        wheel(&input, .1, 0, false, false, i == 9 ? 1 : 0, 0);
    CHECK(input.residual_x == 0);
    for (int i = 0; i < 10; i++)
        wheel(&input, -.1, 0, false, false, i == 9 ? -1 : 0, 0);
    CHECK(input.residual_x == 0);
    wheel(&input, 2.75, -3.25, false, false, 2, -3);
    wheel(&input, .25, -.75, false, false, 1, -1);
    CHECK(input.residual_x == 0 && input.residual_y == 0);
    wheel(&input, DBL_MIN, -DBL_MIN, false, false, 0, 0);
    CHECK(input.residual_x == DBL_MIN && input.residual_y == -DBL_MIN);
}

static void jitter_and_reversal(void)
{
    ph_mouse_input input = {0};
    int total_x = 0, total_y = 0;
    /* Clearing a remainder on every direction change loses all this travel.
     * Independent axes must conserve net input despite opposite sensor jitter. */
    for (int i = 0; i < 2000; i++) {
        double x = i % 2 ? -.01 : .9;
        double y = i % 2 ? .02 : -.6;
        int nx, ny;
        ph_mouse_input_wheel(&input, x, y, false, false, &nx, &ny);
        events++;
        total_x += nx;
        total_y += ny;
        CHECK(fabs(input.residual_x) < 1 && fabs(input.residual_y) < 1);
    }
    CHECK(fabs(total_x + input.residual_x - 890) < 1e-9);
    CHECK(fabs(total_y + input.residual_y + 580) < 1e-9);
    ph_mouse_input_reset(&input);
    wheel(&input, .75, -.5, false, false, 0, 0);
    wheel(&input, -.5, .25, false, false, 0, 0);
    CHECK(input.residual_x == .25 && input.residual_y == -.25);
    wheel(&input, -1.5, 2.5, false, false, -1, 2);
    CHECK(input.residual_x == -.25 && input.residual_y == .25);
    for (int i = 0; i < 1000; i++) {
        wheel(&input, 1.25, -1.25, false, false, 1, -1);
        wheel(&input, -1.25, 1.25, false, false, -1, 1);
    }
    CHECK(input.residual_x == -.25 && input.residual_y == .25);
}

static void mapping_modes(void)
{
    ph_mouse_input input = {0};
    wheel(&input, 1, -2, true, false, -1, 2);
    wheel(&input, .25, .5, false, false, 0, 0);
    wheel(&input, .25, .5, false, false, 0, 1);
    wheel(&input, .5, 0, true, false, 0, 0); /* do not reuse old +.5 across flip */
    CHECK(input.residual_x == -.5);
    wheel(&input, .5, 0, true, false, -1, 0);
    wheel(&input, 0, .75, false, false, 0, 0);
    wheel(&input, .25, .5, false, true, 0, 0); /* new Shift mode starts fresh */
    CHECK(input.residual_x == .75 && input.residual_y == 0);
    wheel(&input, .25, 1, false, true, 2, 0); /* preserve genuine horizontal */
    wheel(&input, 1, 2, true, true, -3, 0);
    wheel(&input, 2, -1, false, true, 1, 0);
    wheel(&input, -.75, .75, false, true, 0, 0);
    wheel(&input, 0, .25, false, false, 0, 0); /* no Shift remainder leaks into Y */
    CHECK(input.residual_x == 0 && input.residual_y == .25);
}

static void resets_and_invalid_input(void)
{
    ph_mouse_input input = {0};
    /* The same reset is used for focus/session/help/cancel/target changes. */
    for (int i = 0; i < 5; i++) {
        wheel(&input, .75, -.75, false, false, 0, 0);
        ph_mouse_input_reset(&input);
        CHECK(input.residual_x == 0 && input.residual_y == 0 && !input.mapping_known);
    }
    const double bad[] = {NAN, INFINITY, -INFINITY};
    for (unsigned int i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        wheel(&input, .75, .5, false, false, 0, 0);
        wheel(&input, bad[i], 3, false, false, 0, 0);
        CHECK(!input.mapping_known && input.residual_x == 0 && input.residual_y == 0);
        wheel(&input, .75, .5, false, false, 0, 0);
        wheel(&input, 3, bad[i], false, false, 0, 0);
        CHECK(!input.mapping_known);
    }
    const double corrupt[] = {NAN, INFINITY, -INFINITY, 1, -1, DBL_MAX};
    for (unsigned int i = 0; i < sizeof corrupt / sizeof corrupt[0]; i++) {
        input.residual_x = corrupt[i];
        wheel(&input, 1, 0, false, false, 0, 0);
        input.residual_y = corrupt[i];
        wheel(&input, 0, 1, false, false, 0, 0);
    }
    int nx = 1, ny = 2;
    CHECK(!ph_mouse_input_wheel(NULL, 1, 1, false, false, &nx, &ny));
    CHECK(nx == 0 && ny == 0);
    CHECK(!ph_mouse_input_wheel(&input, 1, 1, false, false, NULL, &ny) && ny == 0);
    CHECK(!ph_mouse_input_wheel(&input, 1, 1, false, false, &nx, NULL) && nx == 0);
    CHECK(!ph_mouse_input_wheel(&input, 1, 1, false, false, &nx, &nx) && nx == 0);
    ph_mouse_input_reset(NULL);
}

static void bounded_large_input(void)
{
    ph_mouse_input input = {0};
    wheel(&input, .25, -.25, false, false, 0, 0);
    wheel(&input, DBL_MAX, -DBL_MAX, false, false,
          PH_MOUSE_INPUT_MAX_NOTCHES, -PH_MOUSE_INPUT_MAX_NOTCHES);
    CHECK(input.residual_x == .25 && input.residual_y == -.25);
    ph_mouse_input_reset(&input);
    wheel(&input, 1e100, -1e100, false, true, 0, 0);
    wheel(&input, DBL_MAX, DBL_MAX, false, true, PH_MOUSE_INPUT_MAX_NOTCHES, 0);
    wheel(&input, -DBL_MAX, -DBL_MAX, false, true, -PH_MOUSE_INPUT_MAX_NOTCHES, 0);
    wheel(&input, DBL_MAX, DBL_MAX, true, true, -PH_MOUSE_INPUT_MAX_NOTCHES, 0);
    ph_mouse_input_reset(&input);
    wheel(&input, 1000.25, -1000.75, false, false,
          PH_MOUSE_INPUT_MAX_NOTCHES, -PH_MOUSE_INPUT_MAX_NOTCHES);
    CHECK(input.residual_x == .25 && input.residual_y == -.75);
    wheel(&input, .75, -.25, false, false, 1, -1);
    wheel(&input, 0, 0, false, false, 0, 0); /* no queued whole-notch replay */
}

int main(void)
{
    button_numbers();
    precise_accumulation();
    jitter_and_reversal();
    mapping_modes();
    resets_and_invalid_input();
    bounded_large_input();
    if (failures)
        return 1;
    printf("PC mouse adapter passed: buttons 1-8, %u precise wheel events, jitter, reversal, "
           "mapping resets and bounded invalid input\n", events);
    return 0;
}
