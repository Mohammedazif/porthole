#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/version.h"

#ifdef PORTHOLE_HAVE_AIRPLAY
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
            "  -v, --verbose     Show debug messages\n"
            "  -V, --version     Print version and build features, then exit\n"
            "  -h, --help        Show this help, then exit\n",
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
        } else if (!strcmp(arg, "-n") || !strcmp(arg, "--name")) {
            if (i + 1 >= argc) {
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
    PH_LOGD("Wi-Fi receiver compiled in: %s", FEATURE_AIRPLAY);
    PH_LOGW("Nothing to run yet: the Wi-Fi receiver arrives in Phase 1 (see PLAN.md).");
    return 0;
}
