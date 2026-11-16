#ifndef PORTHOLE_APP_H
#define PORTHOLE_APP_H

#include <stdbool.h>

typedef struct ph_app_options {
    const char *name;       /* receiver name shown on the iPhone */
    bool fixed_ports;       /* well-known ports, matching the firewall rules */
    bool debug;             /* verbose AirPlay protocol logging */
    bool bluetooth;         /* control the iPhone through the Bluetooth helper */
    bool audio;             /* play mirrored phone audio through the PC */
    bool diagnostics;       /* show measured local input/video timings */
    bool auto_setup;        /* offer guided setup until completed */
    bool setup;             /* explicitly open guided setup this run */
    const char *ble_helper; /* path of the Bluetooth helper script */
} ph_app_options;

/* Runs the mirror window until it is closed. Returns a process exit code. */
int ph_app_run(const ph_app_options *opt);

#endif
