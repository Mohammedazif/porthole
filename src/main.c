#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/version.h"

#ifdef PORTHOLE_HAVE_AIRPLAY
#include "app.h"
#define FEATURE_AIRPLAY "yes"
#else
#define FEATURE_AIRPLAY "no"
#endif

static void print_usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "Usage: %s [options]\n"
            "\n"
            "Show your iPhone's screen on this PC and control it with your mouse and keyboard.\n"
            "\n"
            "Options:\n"
            "  -n, --name NAME   Name shown in the iPhone's Screen Mirroring list (default: %s)\n"
            "      --random-ports  Let the system pick network ports instead of the fixed\n"
            "                    TCP 7000/7001/7100 and UDP 6000/6001/7011\n"
            "      --no-bluetooth  View only: don't control the phone over Bluetooth\n"
            "      --ble-helper PATH  Bluetooth helper script (default: the one in the source tree)\n"
            "      --debug       Log AirPlay protocol details\n"
            "  -v, --verbose     Show debug messages\n"
            "  -V, --version     Print version and build features, then exit\n"
            "  -h, --help        Show this help, then exit\n"
            "\n"
            "Hotkeys (Left Alt + key): H Home, S App Switcher, C Control Center,\n"
            "  N Notification Center, F Spotlight, P Screenshot, Left Back, Up/Down Volume.\n"
            "Ctrl on the PC acts as Command on the iPhone (Ctrl+C, Ctrl+V, ...).\n",
            argv0, PORTHOLE_NAME);
}

static void print_version(void)
{
    printf("%s %s\n", PORTHOLE_NAME, PORTHOLE_VERSION);
    printf("  Wi-Fi receiver (AirPlay core): %s\n", FEATURE_AIRPLAY);
}

int main(int argc, char **argv)
{
    const char *name = PORTHOLE_NAME;
    bool fixed_ports = true;
    bool debug = false;
    bool bluetooth = true;
#ifdef PORTHOLE_BLE_HELPER
    const char *ble_helper = PORTHOLE_BLE_HELPER;
#else
    const char *ble_helper = NULL;
#endif
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            print_usage(stdout, argv[0]);
            return 0;
        } else if (!strcmp(arg, "-V") || !strcmp(arg, "--version")) {
            print_version();
            return 0;
        } else if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
            ph_log_set_level(PH_LOG_DEBUG);
        } else if (!strcmp(arg, "--debug")) {
            debug = true;
            ph_log_set_level(PH_LOG_DEBUG);
        } else if (!strcmp(arg, "--random-ports")) {
            fixed_ports = false;
        } else if (!strcmp(arg, "--no-bluetooth")) {
            bluetooth = false;
        } else if (!strcmp(arg, "--ble-helper")) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                fprintf(stderr, "%s: %s needs a value\n", argv[0], arg);
                return 2;
            }
            ble_helper = argv[++i];
        } else if (!strcmp(arg, "-n") || !strcmp(arg, "--name")) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                fprintf(stderr, "%s: %s needs a value\n", argv[0], arg);
                return 2;
            }
            name = argv[++i];
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n\n", argv[0], arg);
            print_usage(stderr, argv[0]);
            return 2;
        }
    }

    PH_LOGI("%s %s, receiver name \"%s\"", PORTHOLE_NAME, PORTHOLE_VERSION, name);
#ifdef PORTHOLE_HAVE_AIRPLAY
    {
        ph_app_options opt = {.name = name,
                              .fixed_ports = fixed_ports,
                              .debug = debug,
                              .bluetooth = bluetooth,
                              .ble_helper = ble_helper};
        return ph_app_run(&opt);
    }
#else
    (void)fixed_ports;
    (void)debug;
    (void)bluetooth;
    (void)ble_helper;
    PH_LOGW("this build has no Wi-Fi receiver (configured with PORTHOLE_WITH_AIRPLAY=OFF)");
    return 0;
#endif
}
