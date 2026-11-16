#include <stdio.h>

#include "control/setup.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    ph_setup s;
    double x, y;
    ph_setup_open(&s, true);
    CHECK(s.visible && !s.prepared && s.step == PH_SETUP_PREPARE);
    CHECK(!ph_setup_start_calibration(&s, 100));
    CHECK(ph_setup_next(&s) && s.prepared && s.step == PH_SETUP_BLUETOOTH);
    CHECK(!ph_setup_next(&s));
    ph_setup_update(&s, true, false);
    CHECK(ph_setup_next(&s) && s.step == PH_SETUP_MIRROR);
    CHECK(!ph_setup_next(&s));
    ph_setup_update(&s, true, true);
    CHECK(ph_setup_next(&s) && s.step == PH_SETUP_CALIBRATE);
    CHECK(!ph_setup_next(&s));
    CHECK(ph_setup_start_calibration(&s, 100));
    CHECK(ph_setup_target(&s, &x, &y) && x == .25 && y == .35);
    ph_setup_observe(&s, false, false, 200);
    CHECK(s.target == 0 && s.calibrating);
    ph_setup_observe(&s, true, false, 300);
    CHECK(!ph_setup_start_calibration(&s, 350) && s.target == 1 && s.target_since == 300);
    CHECK(ph_setup_target(&s, &x, &y) && x == .75 && y == .65);
    ph_setup_observe(&s, true, false, 400);
    CHECK(ph_setup_target(&s, &x, &y) && x == .5 && y == .75);
    ph_setup_observe(&s, true, false, 500);
    CHECK(s.calibrated && !s.calibrating && !ph_setup_target(&s, &x, &y));
    CHECK(ph_setup_next(&s) && s.step == PH_SETUP_FINISH);
    ph_setup_update(&s, false, true);
    CHECK(!s.calibrated && s.step == PH_SETUP_CALIBRATE && !ph_setup_next(&s));
    ph_setup_update(&s, true, true);
    CHECK(ph_setup_start_calibration(&s, 1000));
    ph_setup_observe(&s, true, false, 1000 + PH_SETUP_TARGET_TIMEOUT_MS);
    CHECK(!s.calibrated && !s.calibrating && s.error == PH_SETUP_POINTER_UNCONFIRMED);
    CHECK(ph_setup_start_calibration(&s, 10000));
    ph_setup_update(&s, true, false);
    CHECK(s.error == PH_SETUP_CONNECTION_LOST && !s.calibrating);
    ph_setup_update(&s, true, true);
    CHECK(ph_setup_start_calibration(&s, 12000));
    ph_setup_back(&s);
    CHECK(!s.calibrating && s.step == PH_SETUP_MIRROR);
    CHECK(ph_setup_next(&s) && ph_setup_start_calibration(&s, 13000));
    for (int i = 0; i < PH_SETUP_TARGETS; i++)
        ph_setup_observe(&s, true, false, 13100 + (unsigned int)i);
    CHECK(ph_setup_next(&s) && ph_setup_next(&s) && s.completed && !s.visible);

    /* Viewing-only builds never require an unavailable Bluetooth backend. */
    ph_setup_open(&s, false);
    CHECK(s.step == PH_SETUP_MIRROR && !ph_setup_next(&s));
    ph_setup_update(&s, false, true);
    CHECK(ph_setup_next(&s) && s.step == PH_SETUP_FINISH);
    ph_setup_update(&s, false, false);
    CHECK(s.step == PH_SETUP_MIRROR && !ph_setup_next(&s));
    ph_setup_update(&s, false, true);
    CHECK(ph_setup_next(&s) && s.step == PH_SETUP_FINISH);
    CHECK(ph_setup_next(&s) && s.completed && !s.visible);
    ph_setup_open(&s, true);
    ph_setup_close(&s);
    CHECK(!s.visible && !s.completed);
    ph_setup_open(&s, true);
    ph_setup_update(&s, true, true);
    CHECK(ph_setup_next(&s) && ph_setup_next(&s) && ph_setup_next(&s));
    CHECK(ph_setup_start_calibration(&s, 20000));
    ph_setup_close(&s);
    CHECK(!s.visible && !s.calibrating && !s.calibrated && s.error == PH_SETUP_CANCELED);
    if (failures)
        return 1;
    puts("guided setup gates, calibration confirmations, cancellation and mirror-only flow passed");
    return 0;
}
