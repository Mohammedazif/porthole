#ifndef PORTHOLE_CONTROL_SETUP_H
#define PORTHOLE_CONTROL_SETUP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ph_setup_step {
    PH_SETUP_PREPARE,
    PH_SETUP_BLUETOOTH,
    PH_SETUP_MIRROR,
    PH_SETUP_CALIBRATE,
    PH_SETUP_FINISH
} ph_setup_step;

typedef enum ph_setup_error {
    PH_SETUP_OK,
    PH_SETUP_CONNECTION_LOST,
    PH_SETUP_POINTER_UNCONFIRMED,
    PH_SETUP_SAVE_FAILED,
    PH_SETUP_CANCELED
} ph_setup_error;

typedef enum ph_setup_action {
    PH_SETUP_ACTION_NONE,
    PH_SETUP_ACTION_NEXT,
    PH_SETUP_ACTION_BACK,
    PH_SETUP_ACTION_CALIBRATE,
    PH_SETUP_ACTION_CLOSE
} ph_setup_action;

#define PH_SETUP_TARGETS 3
#define PH_SETUP_TARGET_TIMEOUT_MS 8000

/* Local setup state. The user confirms iPhone-only settings; Bluetooth and
 * mirroring gates use actual connection state, and calibration requires video. */
typedef struct ph_setup {
    bool visible, controls, prepared, bluetooth_ready, mirroring;
    bool calibrating, calibrated, completed;
    ph_setup_step step;
    ph_setup_error error;
    unsigned int target;
    uint64_t target_since;
} ph_setup;

void ph_setup_open(ph_setup *setup, bool controls);
void ph_setup_close(ph_setup *setup);
void ph_setup_update(ph_setup *setup, bool bluetooth_ready, bool mirroring);
bool ph_setup_next(ph_setup *setup);
void ph_setup_back(ph_setup *setup);
bool ph_setup_start_calibration(ph_setup *setup, uint64_t now_ms);
bool ph_setup_target(const ph_setup *setup, double *x, double *y);
void ph_setup_observe(ph_setup *setup, bool ready, bool failed, uint64_t now_ms);
void ph_setup_abort(ph_setup *setup, ph_setup_error error);

#endif
