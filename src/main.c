#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/upath.h"
#include "core/version.h"
#include "files/places.h"

#ifdef PORTHOLE_HAVE_AIRPLAY
#include <SDL.h>
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
            "  -n, --name NAME   Legacy AirPlay receiver name (default: %s)\n"
            "      --random-ports  Let the system pick network ports instead of the fixed\n"
            "                    TCP 7000/7001/7100 and UDP 6000/6001/7011\n"
            "      --no-bluetooth  View only: don't control the phone over Bluetooth\n"
            "      --relative-pointer  Legacy relative mouse with video tracking (the iPhone\n"
            "                    must be forgotten and re-paired after switching modes)\n"
            "      --absolute-pointer  Exact absolute pointer (default)\n"
            "      --no-audio     Mute audio playback on the PC\n"
            "      --setup        Open guided pairing and calibration setup\n"
            "      --no-setup     Start on the connection screen (default; F2 opens setup)\n"
            "      --diagnostics  Show local timing diagnostics and log summaries\n"
            "      --pin          Require a 4-digit PIN the first time a device connects\n"
            "      --four-k       Advertise a 3840x2160 H.264 or H.265 picture\n"
            "      --audio-delay MS  Shift PC audio relative to the picture (-500..500)\n"
            "      --precise-touch HOST:PORT  Optional WebDriverAgent taps (off by default)\n"
            "      --autostart    Write a Linux autostart entry for this executable\n"
            "      --no-usb       Leave a plugged-in iPhone on its normal USB driver\n"
            "      --ble-helper PATH  Bluetooth helper (Windows executable or Linux Python script)\n"
            "      --send FILE...  Send files to the iPhone through the Porthole already open\n"
            "      --debug       Log AirPlay protocol details\n"
            "  -v, --verbose     Show debug messages\n"
            "  -V, --version     Print version and build features, then exit\n"
            "  -h, --help        Show this help, then exit\n"
            "\n"
            "Hotkeys (Left Alt + key): H Home, S App Switcher, C Control Center,\n"
            "  N Notification Center, F Spotlight, P Screenshot, Left Back, Up/Down Volume.\n"
            "  Enter fullscreen, T always on top, 1 one-to-one,\n"
            "  V types the PC clipboard into the focused field.\n"
            "  With the Porthole app connected, copy and paste move both ways.\n"
            "  On the iPhone, paste with Ctrl+V.\n"
            "  M mute PC audio, =/- PC volume, ,/. audio sync.\n"
            "Ctrl on the PC acts as Command on the iPhone (Ctrl+C, Ctrl+V, ...).\n"
            "F1 help; F2 guided setup; F3 local timing diagnostics; F6 next device;\n"
            "F8 save a PNG; F9 start or stop an H.264 recording.\n"
            "Drag a fullscreen photo off the window to take its file.\n"
            "A plugged-in iPhone becomes the picture after its first USB keyframe.\n"
            "Unplugging returns to Wi-Fi when a Wi-Fi frame arrived within 500 ms.\n",
            argv0, PORTHOLE_NAME);
}

static void print_version(void)
{
    printf("%s %s\n", PORTHOLE_NAME, PORTHOLE_VERSION);
    printf("  Wi-Fi receiver (AirPlay core): %s\n", FEATURE_AIRPLAY);
}

#ifdef _WIN32
#include <io.h>

/* Started from the Start menu, Porthole has nowhere to print its log. Keep it
 * in the settings folder as porthole.log (the previous run's as
 * porthole.old.log) so a problem can be reported. A console or a redirect
 * keeps receiving it as before, and the Bluetooth helper writes to it too. */
static void keep_log_file(void)
{
    HANDLE current = GetStdHandle(STD_ERROR_HANDLE);
    if (current && current != INVALID_HANDLE_VALUE && GetFileType(current) != FILE_TYPE_UNKNOWN)
        return;
    char dir[480], path[512], old[520];
    if (!ph_config_dir(dir, sizeof dir) ||
        snprintf(path, sizeof path, "%s\\porthole.log", dir) >= (int)sizeof path ||
        snprintf(old, sizeof old, "%s\\porthole.old.log", dir) >= (int)sizeof old)
        return;
    ph_remove(old);
    ph_rename(path, old);
    wchar_t *wide = ph_wide(path);
    FILE *file = wide ? _wfreopen(wide, L"w", stderr) : NULL;
    free(wide);
    if (!file)
        return;
    setvbuf(stderr, NULL, _IONBF, 0);
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (handle != INVALID_HANDLE_VALUE)
        SetStdHandle(STD_ERROR_HANDLE, handle);
}
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    keep_log_file();
#endif
    const char *name = PORTHOLE_NAME;
    bool fixed_ports = true;
    bool debug = false;
#if defined(_WIN32) && !defined(PORTHOLE_HAVE_WINDOWS_BLUETOOTH)
    bool bluetooth = false;
#else
    bool bluetooth = true;
#endif
    bool audio = true;
    bool diagnostics = false, setup = false, auto_setup = false;
    bool pin_required = false, four_k = false, autostart = false, usb = true;
    bool absolute_pointer = true; /* both Bluetooth helpers publish the absolute mouse */
    bool pointer_chosen = false;
    int audio_delay_ms = 0;
    const char *precise_touch = NULL;
    const char *ble_helper = NULL;
    int i;

    /* Explorer's Send to, or a terminal: hand files to the Porthole already running. */
    if (argc >= 2 && !strcmp(argv[1], "--send"))
        return ph_place_send_command(argc - 2, argv + 2);

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
        } else if (!strcmp(arg, "--relative-pointer")) {
            absolute_pointer = false;
            pointer_chosen = true;
        } else if (!strcmp(arg, "--absolute-pointer")) {
            absolute_pointer = true;
            pointer_chosen = true;
        } else if (!strcmp(arg, "--no-audio")) {
            audio = false;
        } else if (!strcmp(arg, "--setup")) {
            setup = true;
        } else if (!strcmp(arg, "--no-setup")) {
            auto_setup = false;
            setup = false;
        } else if (!strcmp(arg, "--diagnostics")) {
            diagnostics = true;
        } else if (!strcmp(arg, "--pin")) {
            pin_required = true;
        } else if (!strcmp(arg, "--four-k")) {
            four_k = true;
        } else if (!strcmp(arg, "--autostart")) {
            autostart = true;
        } else if (!strcmp(arg, "--no-usb")) {
            usb = false;
        } else if (!strcmp(arg, "--audio-delay")) {
            char *end = NULL;
            long delay;
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs a value\n", argv[0], arg);
                return 2;
            }
            delay = strtol(argv[++i], &end, 10);
            if (!end || *end || delay < -500 || delay > 500) {
                fprintf(stderr, "%s: --audio-delay needs an integer from -500 to 500\n", argv[0]);
                return 2;
            }
            audio_delay_ms = (int)delay;
        } else if (!strcmp(arg, "--precise-touch")) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                fprintf(stderr, "%s: %s needs HOST:PORT\n", argv[0], arg);
                return 2;
            }
            precise_touch = argv[++i];
        } else if (!strcmp(arg, "--ble-helper")) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                fprintf(stderr, "%s: %s needs a value\n", argv[0], arg);
                return 2;
            }
            ble_helper = argv[++i];
#ifdef _WIN32
            bluetooth = true; /* explicit helper also works in a mirror-only build */
#endif
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

    if (!pointer_chosen) {
        char mode[32];
        if (ph_config_read_line("pointer-mode", mode, sizeof mode)) {
            if (!strcmp(mode, "relative"))
                absolute_pointer = false;
            else if (!strcmp(mode, "absolute"))
                absolute_pointer = true;
        }
    }

    PH_LOGI("%s %s, receiver name \"%s\"", PORTHOLE_NAME, PORTHOLE_VERSION, name);
#ifdef PORTHOLE_HAVE_AIRPLAY
    {
        char *bundled_helper = NULL;
#if defined(_WIN32) || defined(PORTHOLE_INSTALLED_BLE_HELPER)
        if (!ble_helper) {
            char *base = SDL_GetBasePath();
            if (base) {
#ifdef _WIN32
                static const char suffix[] = "ble-winrt/porthole-ble-winrt.exe";
#else
                static const char suffix[] = PORTHOLE_INSTALLED_BLE_HELPER;
#endif
                size_t length = strlen(base) + sizeof suffix;
                bundled_helper = malloc(length);
                if (bundled_helper) {
                    snprintf(bundled_helper, length, "%s%s", base, suffix);
                    if (ph_file_size(bundled_helper) >= 0)
                        ble_helper = bundled_helper;
                }
                SDL_free(base);
            }
        }
#endif
#ifdef PORTHOLE_BLE_HELPER
        if (!ble_helper)
            ble_helper = PORTHOLE_BLE_HELPER;
#endif
        if (bluetooth && !ble_helper)
            PH_LOGW("couldn't locate the Bluetooth helper");
        ph_app_options opt = {.name = name,
                              .fixed_ports = fixed_ports,
                              .debug = debug,
                              .bluetooth = bluetooth,
                              .audio = audio,
                              .diagnostics = diagnostics,
                              .auto_setup = auto_setup,
                              .setup = setup,
                              .pin_required = pin_required,
                              .four_k = four_k,
                              .autostart = autostart,
                              .audio_delay_ms = audio_delay_ms,
                              .precise_touch = precise_touch,
                              .exe_path = argv[0],
                              .ble_helper = ble_helper,
                              .usb = usb,
                              .absolute_pointer = absolute_pointer};
        int result = ph_app_run(&opt);
        free(bundled_helper);
        return result;
    }
#else
    (void)fixed_ports;
    (void)debug;
    (void)bluetooth;
    (void)audio;
    (void)diagnostics;
    (void)setup;
    (void)auto_setup;
    (void)pin_required;
    (void)four_k;
    (void)autostart;
    (void)audio_delay_ms;
    (void)precise_touch;
    (void)ble_helper;
    (void)usb;
    (void)absolute_pointer;
    PH_LOGW("this build has no Wi-Fi receiver (configured with PORTHOLE_WITH_AIRPLAY=OFF)");
    return 0;
#endif
}
