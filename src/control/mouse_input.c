#include "control/mouse_input.h"

#include <float.h>
#include <math.h>

#include "hid/hid_reports.h"

uint8_t ph_mouse_input_button(unsigned int pc_button)
{
    static const uint8_t buttons[] = {0, PH_HID_BTN_LEFT, PH_HID_BTN_MIDDLE, PH_HID_BTN_RIGHT,
                                      PH_HID_BTN_4, PH_HID_BTN_5, PH_HID_BTN_6,
                                      PH_HID_BTN_7, PH_HID_BTN_8};
    return pc_button < sizeof buttons / sizeof buttons[0] ? buttons[pc_button] : 0;
}

void ph_mouse_input_reset(ph_mouse_input *input)
{
    if (input)
        *input = (ph_mouse_input){0};
}

static double finite_sum(double x, double y)
{
    if (y > 0 && x > DBL_MAX - y)
        return DBL_MAX;
    if (y < 0 && x < -DBL_MAX - y)
        return -DBL_MAX;
    return x + y;
}

static int whole_notches(double delta, double *residual)
{
    /* Split before bounding: 32.25 must keep its .25, and DBL_MAX must never
     * overflow an integer conversion or wipe a previously saved fraction. */
    double integral;
    double fraction = modf(delta, &integral);
    integral = fmax(-PH_MOUSE_INPUT_MAX_NOTCHES, fmin(PH_MOUSE_INPUT_MAX_NOTCHES, integral));
    double total = integral + fraction + *residual;
    double tolerance = 8 * DBL_EPSILON * fmax(1, fabs(total));
    double whole = total < 0 ? ceil(total - tolerance) : floor(total + tolerance);
    *residual = total - whole;
    if (whole != 0 && fabs(*residual) <= tolerance)
        *residual = 0; /* ten .1 steps must not require an eleventh event */
    int result = (int)whole; /* bounded to [-33,33] before conversion */
    if (result > PH_MOUSE_INPUT_MAX_NOTCHES)
        result = PH_MOUSE_INPUT_MAX_NOTCHES;
    if (result < -PH_MOUSE_INPUT_MAX_NOTCHES)
        result = -PH_MOUSE_INPUT_MAX_NOTCHES;
    return result;
}

bool ph_mouse_input_wheel(ph_mouse_input *input, double precise_x, double precise_y,
                          bool flipped, bool shift, int *notches_x, int *notches_y)
{
    if (notches_x)
        *notches_x = 0;
    if (notches_y)
        *notches_y = 0;
    if (!input || !notches_x || !notches_y || notches_x == notches_y
        || !isfinite(precise_x) || !isfinite(precise_y)
        || !isfinite(input->residual_x) || !isfinite(input->residual_y)
        || fabs(input->residual_x) >= 1 || fabs(input->residual_y) >= 1) {
        ph_mouse_input_reset(input);
        return false;
    }
    if (input->mapping_known && (input->flipped != flipped || input->shift != shift)) {
        input->residual_x = 0;
        input->residual_y = 0;
    }
    input->mapping_known = true;
    input->flipped = flipped;
    input->shift = shift;
    if (flipped) {
        precise_x = -precise_x;
        precise_y = -precise_y;
    }
    if (shift) {
        precise_x = finite_sum(precise_x, precise_y);
        precise_y = 0;
    }
    *notches_x = whole_notches(precise_x, &input->residual_x);
    *notches_y = whole_notches(precise_y, &input->residual_y);
    return *notches_x != 0 || *notches_y != 0;
}
