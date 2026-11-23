#ifndef PORTHOLE_CONTROL_CONNECTION_POLICY_H
#define PORTHOLE_CONTROL_CONNECTION_POLICY_H

#include <stdbool.h>

typedef struct ph_control_policy {
    bool managed;      /* an authenticated phone supplied preferences */
    bool connected;    /* that phone's authenticated connection is still live */
    bool enabled;
    bool screen_only;
    bool verified;     /* the current Bluetooth peer proved its keyboard link */
} ph_control_policy;

/* Pairing is an explicit setup action, not an idle Wi-Fi connection. */
bool ph_control_connection_wanted(ph_control_policy policy, bool sharing,
                                  bool pairing, bool paused);

#endif
