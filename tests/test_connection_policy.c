#include "control/connection_policy.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void)
{
    ph_control_policy legacy = {0};
    assert(!ph_control_connection_wanted(legacy, false, false, false));
    assert(ph_control_connection_wanted(legacy, true, false, false));
    assert(ph_control_connection_wanted(legacy, false, true, false));
    for (unsigned bits = 0; bits < 64; bits++) {
        ph_control_policy p = {.managed = true, .connected = (bits & 1) != 0,
            .enabled = (bits & 2) != 0, .screen_only = (bits & 4) != 0};
        bool sharing = (bits & 8) != 0, pairing = (bits & 16) != 0, paused = (bits & 32) != 0;
        bool allow = ph_control_connection_wanted(p, sharing, pairing, paused);
        if (!p.connected || !p.enabled || paused)
            assert(!allow);
        else if (!sharing && !pairing && p.screen_only)
            assert(!allow);
        else
            assert(allow);
    }
    puts("control connection: idle, sharing, explicit pairing, disable, disconnect and pause passed");
    return 0;
}
