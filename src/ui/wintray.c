#include "ui/wintray.h"

#include <stdlib.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#define TRAY_CALLBACK (WM_APP + 41)
#define TRAY_ID 1

struct ph_wintray {
    HWND hwnd;
    NOTIFYICONDATAW icon;
    UINT taskbar_created;
    bool feed, paused, sound_on, can_copy;
    ph_wintray_command pending;
};

static void show_menu(ph_wintray *t)
{
    HMENU menu = CreatePopupMenu();
    POINT at;
    UINT picked;

    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, PH_WINTRAY_SHOW, L"Show Porthole");
    AppendMenuW(menu, MF_STRING, PH_WINTRAY_RECEIVED, L"Received files");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (t->feed ? MF_CHECKED : 0), PH_WINTRAY_FEED,
                L"Feed scrolling\tF4");
    AppendMenuW(menu, MF_STRING | (t->paused ? MF_CHECKED : 0), PH_WINTRAY_PAUSE, L"Pause controls");
    AppendMenuW(menu, MF_STRING | (t->sound_on ? MF_CHECKED : 0), PH_WINTRAY_SOUND,
                L"Play sound on PC");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (t->can_copy ? 0 : MF_GRAYED), PH_WINTRAY_COPY,
                L"Copy pairing details");
    AppendMenuW(menu, MF_STRING, PH_WINTRAY_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, PH_WINTRAY_QUIT, L"Quit Porthole");
    SetMenuDefaultItem(menu, PH_WINTRAY_SHOW, FALSE);
    GetCursorPos(&at);
    /* Without the foreground the menu would not close when clicking elsewhere. */
    SetForegroundWindow(t->hwnd);
    picked = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                  at.x, at.y, 0, t->hwnd, NULL);
    PostMessageW(t->hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (picked)
        t->pending = (ph_wintray_command)picked;
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ph_wintray *t = (ph_wintray *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (t && msg == TRAY_CALLBACK) {
        UINT event = LOWORD(lp);
        if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK || event == NIN_SELECT
            || event == NIN_KEYSELECT)
            t->pending = PH_WINTRAY_SHOW;
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
            show_menu(t);
        else if (event == NIN_BALLOONUSERCLICK)
            t->pending = PH_WINTRAY_RECEIVED;
        return 0;
    }
    /* Explorer restarted: the icon must be added again. */
    if (t && t->taskbar_created && msg == t->taskbar_created) {
        Shell_NotifyIconW(NIM_ADD, &t->icon);
        Shell_NotifyIconW(NIM_SETVERSION, &t->icon);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ph_wintray *ph_wintray_create(void)
{
    static const wchar_t *const klass = L"PortholeTray";
    WNDCLASSW wc = {0};
    HINSTANCE inst = GetModuleHandleW(NULL);
    ph_wintray *t = calloc(1, sizeof *t);

    if (!t)
        return NULL;
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = klass;
    RegisterClassW(&wc); /* fails harmlessly when already registered */
    /* A hidden top-level window: message-only windows miss TaskbarCreated. */
    t->hwnd = CreateWindowExW(0, klass, L"Porthole", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!t->hwnd) {
        free(t);
        return NULL;
    }
    SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, (LONG_PTR)t);
    t->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    t->icon.cbSize = sizeof t->icon;
    t->icon.hWnd = t->hwnd;
    t->icon.uID = TRAY_ID;
    t->icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    t->icon.uCallbackMessage = TRAY_CALLBACK;
    t->icon.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!t->icon.hIcon)
        t->icon.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcscpy(t->icon.szTip, L"Porthole");
    t->icon.uVersion = NOTIFYICON_VERSION_4;
    t->sound_on = true;
    if (!Shell_NotifyIconW(NIM_ADD, &t->icon)) {
        DestroyWindow(t->hwnd);
        free(t);
        return NULL;
    }
    Shell_NotifyIconW(NIM_SETVERSION, &t->icon);
    return t;
}

void ph_wintray_destroy(ph_wintray *t)
{
    if (!t)
        return;
    Shell_NotifyIconW(NIM_DELETE, &t->icon);
    if (t->icon.hIcon)
        DestroyIcon(t->icon.hIcon);
    DestroyWindow(t->hwnd);
    free(t);
}

void ph_wintray_set_state(ph_wintray *t, bool feed, bool paused, bool sound_on, bool can_copy,
                          const char *tip)
{
    wchar_t wide[128];

    if (!t)
        return;
    t->feed = feed;
    t->paused = paused;
    t->sound_on = sound_on;
    t->can_copy = can_copy;
    if (!tip || !MultiByteToWideChar(CP_UTF8, 0, tip, -1, wide, (int)(sizeof wide / sizeof wide[0])))
        return;
    if (wcscmp(wide, t->icon.szTip) == 0)
        return;
    wcsncpy(t->icon.szTip, wide, sizeof t->icon.szTip / sizeof t->icon.szTip[0] - 1);
    t->icon.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &t->icon);
    t->icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

bool ph_wintray_notify(ph_wintray *t, const char *title, const char *text)
{
    NOTIFYICONDATAW n;

    if (!t)
        return false;
    n = t->icon;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    if (!MultiByteToWideChar(CP_UTF8, 0, title ? title : "Porthole", -1, n.szInfoTitle,
                             (int)(sizeof n.szInfoTitle / sizeof n.szInfoTitle[0])))
        n.szInfoTitle[0] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, text ? text : "", -1, n.szInfo,
                             (int)(sizeof n.szInfo / sizeof n.szInfo[0])))
        n.szInfo[0] = 0;
    return Shell_NotifyIconW(NIM_MODIFY, &n) != FALSE;
}

ph_wintray_command ph_wintray_take(ph_wintray *t)
{
    ph_wintray_command c;

    if (!t)
        return PH_WINTRAY_NONE;
    c = t->pending;
    t->pending = PH_WINTRAY_NONE;
    return c;
}

#else

ph_wintray *ph_wintray_create(void)
{
    return NULL;
}

void ph_wintray_destroy(ph_wintray *tray)
{
    (void)tray;
}

void ph_wintray_set_state(ph_wintray *tray, bool feed, bool paused, bool sound_on, bool can_copy,
                          const char *tip)
{
    (void)tray;
    (void)feed;
    (void)paused;
    (void)sound_on;
    (void)can_copy;
    (void)tip;
}

ph_wintray_command ph_wintray_take(ph_wintray *tray)
{
    (void)tray;
    return PH_WINTRAY_NONE;
}

bool ph_wintray_notify(ph_wintray *tray, const char *title, const char *text)
{
    (void)tray;
    (void)title;
    (void)text;
    return false;
}

#endif
