#include "control/setup.h"

#include <string.h>

void ph_setup_open(ph_setup *s, bool controls)
{
    memset(s, 0, sizeof *s);
    s->visible = true;
    s->controls = controls;
    s->step = controls ? PH_SETUP_PREPARE : PH_SETUP_MIRROR;
}

void ph_setup_close(ph_setup *s)
{
    s->visible = false;
    if (s->calibrating)
        ph_setup_abort(s, PH_SETUP_CANCELED);
}

void ph_setup_abort(ph_setup *s, ph_setup_error error)
{
    s->calibrating = false;
    s->calibrated = false;
    s->error = error;
}

void ph_setup_update(ph_setup *s, bool bluetooth_ready, bool mirroring)
{
    s->bluetooth_ready = bluetooth_ready;
    s->mirroring = mirroring;
    if (s->visible && !s->controls && s->step == PH_SETUP_FINISH && !mirroring)
        s->step = PH_SETUP_MIRROR;
    if (s->controls && s->calibrated && (!bluetooth_ready || !mirroring)) {
        s->calibrated = false;
        s->error = PH_SETUP_CONNECTION_LOST;
        if (s->visible && s->step == PH_SETUP_FINISH)
            s->step = PH_SETUP_CALIBRATE;
    }
    if (s->calibrating && (!bluetooth_ready || !mirroring))
        ph_setup_abort(s, PH_SETUP_CONNECTION_LOST);
}

bool ph_setup_next(ph_setup *s)
{
    if (!s->visible || s->calibrating)
        return false;
    switch (s->step) {
    case PH_SETUP_PREPARE:
        s->prepared = true; /* explicit user confirmation, never an inferred iOS setting */
        s->step = PH_SETUP_BLUETOOTH;
        break;
    case PH_SETUP_BLUETOOTH:
        if (!s->bluetooth_ready)
            return false;
        s->step = PH_SETUP_MIRROR;
        break;
    case PH_SETUP_MIRROR:
        if (!s->mirroring || (s->controls && !s->bluetooth_ready))
            return false;
        s->step = s->controls ? PH_SETUP_CALIBRATE : PH_SETUP_FINISH;
        break;
    case PH_SETUP_CALIBRATE:
        if (!s->calibrated || !s->bluetooth_ready || !s->mirroring)
            return false;
        s->step = PH_SETUP_FINISH;
        break;
    case PH_SETUP_FINISH:
        if (!s->mirroring || (s->controls && !s->calibrated))
            return false;
        s->completed = true;
        s->visible = false;
        break;
    }
    return true;
}

void ph_setup_back(ph_setup *s)
{
    if (!s->visible)
        return;
    if (s->calibrating)
        ph_setup_abort(s, PH_SETUP_CANCELED);
    if (s->step == PH_SETUP_FINISH)
        s->step = s->controls ? PH_SETUP_CALIBRATE : PH_SETUP_MIRROR;
    else if (s->controls && s->step > PH_SETUP_PREPARE)
        s->step--;
    else
        ph_setup_close(s);
    s->error = PH_SETUP_OK;
}

bool ph_setup_start_calibration(ph_setup *s, uint64_t now_ms)
{
    if (!s->visible || s->calibrating || s->step != PH_SETUP_CALIBRATE || !s->controls
        || !s->prepared || !s->bluetooth_ready || !s->mirroring)
        return false;
    s->calibrating = true;
    s->calibrated = false;
    s->target = 0;
    s->target_since = now_ms;
    s->error = PH_SETUP_OK;
    return true;
}

bool ph_setup_target(const ph_setup *s, double *x, double *y)
{
    static const double targets[PH_SETUP_TARGETS][2] = {{.25, .35}, {.75, .65}, {.5, .75}};
    if (!s->calibrating || s->target >= PH_SETUP_TARGETS || !x || !y)
        return false;
    *x = targets[s->target][0];
    *y = targets[s->target][1];
    return true;
}

void ph_setup_observe(ph_setup *s, bool ready, bool failed, uint64_t now_ms)
{
    if (!s->calibrating)
        return;
    if (failed || (now_ms >= s->target_since
                   && now_ms - s->target_since >= PH_SETUP_TARGET_TIMEOUT_MS)) {
        ph_setup_abort(s, PH_SETUP_POINTER_UNCONFIRMED);
        return;
    }
    if (ready) {
        s->target++;
        s->target_since = now_ms;
        if (s->target == PH_SETUP_TARGETS) {
            s->calibrating = false;
            s->calibrated = true;
        }
    }
}
