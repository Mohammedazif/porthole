#include <stdio.h>
#include <string.h>

#include "ui/tray.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

int main(void)
{
    ph_tray *tray = ph_tray_create();
    char text[512];
    char tiny[8];
    const char *enabled =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Porthole\n"
        "Exec=/usr/local/bin/porthole\n"
        "X-GNOME-Autostart-enabled=true\n"
        "Hidden=false\n";
    const char *disabled =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Porthole\n"
        "Exec=/usr/bin/porthole\n"
        "X-GNOME-Autostart-enabled=false\n"
        "Hidden=true\n";

    CHECK(tray != NULL);
    CHECK(!ph_tray_muted(tray));
    CHECK(!ph_tray_recording(tray));
    CHECK(!ph_tray_quit_requested(tray));
    CHECK(!ph_tray_show_requested(tray));

    ph_tray_invoke(tray, PH_TRAY_TOGGLE_MUTE);
    CHECK(ph_tray_muted(tray));
    ph_tray_invoke(tray, PH_TRAY_TOGGLE_MUTE);
    CHECK(!ph_tray_muted(tray));

    ph_tray_invoke(tray, PH_TRAY_TOGGLE_RECORD);
    CHECK(ph_tray_recording(tray) && !ph_tray_muted(tray));
    ph_tray_invoke(tray, PH_TRAY_TOGGLE_RECORD);
    CHECK(!ph_tray_recording(tray));

    ph_tray_invoke(tray, PH_TRAY_SHOW);
    CHECK(ph_tray_show_requested(tray));
    ph_tray_invoke(tray, PH_TRAY_SHOW);
    CHECK(ph_tray_show_requested(tray));
    ph_tray_clear_show(tray);
    CHECK(!ph_tray_show_requested(tray));

    ph_tray_invoke(tray, PH_TRAY_QUIT);
    CHECK(ph_tray_quit_requested(tray));
    ph_tray_invoke(tray, PH_TRAY_TOGGLE_MUTE);
    CHECK(ph_tray_quit_requested(tray) && ph_tray_muted(tray));

    ph_tray_invoke(NULL, PH_TRAY_QUIT);
    CHECK(!ph_tray_quit_requested(NULL));
    ph_tray_clear_show(NULL);
    ph_tray_destroy(tray);
    ph_tray_destroy(NULL);

    CHECK(ph_autostart_desktop(text, sizeof text, "/usr/local/bin/porthole", true));
    CHECK(strcmp(text, enabled) == 0);
    CHECK(ph_autostart_desktop(text, sizeof text, "/usr/bin/porthole", false));
    CHECK(strcmp(text, disabled) == 0);
    CHECK(!ph_autostart_desktop(text, sizeof text, NULL, true));
    CHECK(!ph_autostart_desktop(NULL, sizeof text, "/usr/bin/porthole", true));
    CHECK(!ph_autostart_desktop(tiny, sizeof tiny, "/usr/bin/porthole", true));

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all tray checks passed\n");
    return 0;
}
