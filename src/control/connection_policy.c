#include "control/connection_policy.h"

bool ph_control_connection_wanted(ph_control_policy policy, bool sharing,
                                  bool pairing, bool paused)
{
    if (paused)
        return false;
    if (policy.managed) {
        if (!policy.connected || !policy.enabled)
            return false;
        return sharing || pairing || !policy.screen_only;
    }
    return sharing || pairing;
}
