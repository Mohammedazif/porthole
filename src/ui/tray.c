#include "ui/tray.h"

#include <stdio.h>
#include <stdlib.h>

struct ph_tray {
    bool muted;
    bool recording;
    bool quit;
    bool show;
};

ph_tray *ph_tray_create(void)
{
    return calloc(1, sizeof(ph_tray));
}

void ph_tray_destroy(ph_tray *tray)
{
    free(tray);
}

void ph_tray_invoke(ph_tray *tray, ph_tray_action action)
{
    if (!tray)
        return;
    switch (action) {
    case PH_TRAY_SHOW:
        tray->show = true;
        break;
    case PH_TRAY_TOGGLE_MUTE:
        tray->muted = !tray->muted;
        break;
    case PH_TRAY_TOGGLE_RECORD:
        tray->recording = !tray->recording;
        break;
    case PH_TRAY_QUIT:
        tray->quit = true;
        break;
    }
}

bool ph_tray_muted(const ph_tray *tray)
{
    return tray && tray->muted;
}

bool ph_tray_recording(const ph_tray *tray)
{
    return tray && tray->recording;
}

bool ph_tray_quit_requested(const ph_tray *tray)
{
    return tray && tray->quit;
}

bool ph_tray_show_requested(const ph_tray *tray)
{
    return tray && tray->show;
}

void ph_tray_clear_show(ph_tray *tray)
{
    if (tray)
        tray->show = false;
}

bool ph_autostart_desktop(char *out, size_t out_len, const char *exe_path, bool enable)
{
    int n;

    if (!out || out_len == 0 || !exe_path)
        return false;
    n = snprintf(out, out_len,
                 "[Desktop Entry]\n"
                 "Type=Application\n"
                 "Name=Porthole\n"
                 "Exec=%s\n"
                 "X-GNOME-Autostart-enabled=%s\n"
                 "Hidden=%s\n",
                 exe_path, enable ? "true" : "false", enable ? "false" : "true");
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

#ifdef _WIN32
bool ph_tray_win32_message(ph_tray *tray, unsigned message, uintptr_t wparam, intptr_t lparam)
{
    unsigned event;

    if (!tray)
        return false;
    if (message == PH_TRAY_WM_CALLBACK) {
        event = (unsigned)lparam;
        if (event == PH_TRAY_WM_LBUTTONUP || event == PH_TRAY_WM_LBUTTONDBLCLK) {
            ph_tray_invoke(tray, PH_TRAY_SHOW);
            return true;
        }
        return false;
    }
    if (message != PH_TRAY_WM_COMMAND)
        return false;
    switch ((unsigned)(wparam & 0xffffu)) {
    case PH_TRAY_CMD_SHOW:
        ph_tray_invoke(tray, PH_TRAY_SHOW);
        return true;
    case PH_TRAY_CMD_TOGGLE_MUTE:
        ph_tray_invoke(tray, PH_TRAY_TOGGLE_MUTE);
        return true;
    case PH_TRAY_CMD_TOGGLE_RECORD:
        ph_tray_invoke(tray, PH_TRAY_TOGGLE_RECORD);
        return true;
    case PH_TRAY_CMD_QUIT:
        ph_tray_invoke(tray, PH_TRAY_QUIT);
        return true;
    default:
        return false;
    }
}
#endif
