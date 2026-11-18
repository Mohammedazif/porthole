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
    bool pin_required;      /* unknown devices must enter a PIN before mirroring */
    bool four_k;            /* advertise 3840x2160 instead of 1920x1080 */
    bool autostart;         /* write an XDG autostart entry on Linux */
    int audio_delay_ms;     /* positive delays PC audio relative to the picture */
    const char *precise_touch; /* host:port of WebDriverAgent, or NULL */
    const char *exe_path;
    const char *ble_helper; /* path of the Bluetooth helper script */
    bool usb;               /* open a plugged-in iPhone as a USB screen */
    /* Absolute pointer: the PC mouse position maps straight to the phone screen.
     * The phone must have paired with the matching (absolute) report map. */
    bool absolute_pointer;
} ph_app_options;

/* Runs the mirror window until it is closed. Returns a process exit code. */
int ph_app_run(const ph_app_options *opt);

#endif
