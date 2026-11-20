#include "files/places.h"

#include "core/log.h"
#include "core/sock.h"
#include "core/upath.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The local port the running Porthole takes "SEND <path>" on (files/origin.c). */
#define PH_PLACE_SEND_PORT 47734

#ifdef _WIN32

#define COBJMACROS
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <knownfolders.h>
#include <shobjidl.h>

static bool known_folder(const KNOWNFOLDERID *id, char *out, size_t cap)
{
    PWSTR wide = NULL;
    bool ok = SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, NULL, &wide)) && wide
              && ph_narrow(wide, out, cap);

    CoTaskMemFree(wide);
    return ok;
}

bool ph_place_dir(ph_place place, char *out, size_t cap)
{
    if (!out || cap < 4)
        return false;
    return known_folder(place == PH_PLACE_DOWNLOADS ? &FOLDERID_Downloads : &FOLDERID_Desktop, out,
                        cap);
}

bool ph_place_clipboard_file(char *out, size_t cap)
{
    HANDLE data;
    bool ok = false;

    if (!out || cap < 4 || !IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(NULL))
        return false;
    data = GetClipboardData(CF_HDROP);
    if (data) {
        HDROP drop = (HDROP)GlobalLock(data);
        if (drop) {
            wchar_t path[MAX_PATH * 4];
            if (DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) == 1
                && DragQueryFileW(drop, 0, path, (UINT)(sizeof path / sizeof path[0])) > 0
                && ph_narrow(path, out, cap))
                ok = ph_file_size(out) > 0;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return ok;
}

void ph_place_install_send_to(void)
{
    wchar_t exe[MAX_PATH * 2];
    PWSTR folder = NULL;
    wchar_t link[MAX_PATH * 2];
    IShellLinkW *shell = NULL;
    IPersistFile *file = NULL;
    HRESULT init;
    DWORD n = GetModuleFileNameW(NULL, exe, (DWORD)(sizeof exe / sizeof exe[0]));

    if (!n || n >= sizeof exe / sizeof exe[0])
        return;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_SendTo, KF_FLAG_CREATE, NULL, &folder)) || !folder)
        return;
    _snwprintf(link, sizeof link / sizeof link[0], L"%ls\\iPhone (Porthole).lnk", folder);
    link[sizeof link / sizeof link[0] - 1] = 0;
    CoTaskMemFree(folder);
    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* Rewritten each start, so a moved Porthole folder still gets the files. */
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW,
                                   (void **)&shell))) {
        IShellLinkW_SetPath(shell, exe);
        IShellLinkW_SetArguments(shell, L"--send");
        IShellLinkW_SetDescription(shell, L"Send to the iPhone connected to Porthole");
        IShellLinkW_SetIconLocation(shell, exe, 0);
        if (SUCCEEDED(IShellLinkW_QueryInterface(shell, &IID_IPersistFile, (void **)&file))) {
            if (FAILED(IPersistFile_Save(file, link, TRUE)))
                PH_LOGW("send: could not add Porthole to Send to");
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(shell);
    }
    if (SUCCEEDED(init))
        CoUninitialize();
}

#else

static bool home_dir(const char *leaf, char *out, size_t cap)
{
    const char *home = getenv("HOME");

    if (!home || !home[0] || snprintf(out, cap, "%s/%s", home, leaf) >= (int)cap)
        return false;
    if (ph_mkdir(out) != 0 && errno != EEXIST)
        return false;
    return true;
}

bool ph_place_dir(ph_place place, char *out, size_t cap)
{
    if (!out || cap < 4)
        return false;
    return home_dir(place == PH_PLACE_DOWNLOADS ? "Downloads" : "Desktop", out, cap);
}

bool ph_place_clipboard_file(char *out, size_t cap)
{
    (void)out;
    (void)cap;
    return false; /* the app reads file:// text from the SDL clipboard instead */
}

void ph_place_install_send_to(void)
{
}

#endif

int ph_place_send_to_running(int count, char **paths)
{
    int taken = 0;

    for (int i = 0; i < count; i++) {
        struct sockaddr_in addr;
        char line[1200], reply[8] = "";
        int fd = ph_sock_open(), n, got = 0;

        if (fd < 0)
            break;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(PH_PLACE_SEND_PORT);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
            ph_sock_close(fd);
            break; /* Porthole is not running */
        }
        ph_sock_timeout(fd, 5);
        n = snprintf(line, sizeof line, "SEND %s\n", paths[i]);
        if (n > 0 && n < (int)sizeof line && send(fd, line, n, MSG_NOSIGNAL) == n) {
            while (got < (int)sizeof reply - 1 && recv(fd, reply + got, 1, 0) == 1 && reply[got] != '\n')
                got++;
            reply[got] = 0;
            if (!strcmp(reply, "OK"))
                taken++;
        }
        ph_sock_close(fd);
    }
    return taken;
}

int ph_place_send_command(int count, char **paths)
{
    int taken = 0;

    for (int i = 0; i < count; i++) {
        char full[1100];
        char *one = full;
#ifdef _WIN32
        snprintf(full, sizeof full, "%s", paths[i]);
#else
        if (!realpath(paths[i], full))
            snprintf(full, sizeof full, "%s", paths[i]);
#endif
        taken += ph_place_send_to_running(1, &one);
    }
    if (taken < count) {
        char text[256];
        snprintf(text, sizeof text,
                 count == 1 ? "Porthole couldn't send this file.\n\n"
                              "Open Porthole on this PC, connect your iPhone, then try again."
                            : "Porthole sent %d of %d files.\n\n"
                              "Open Porthole on this PC, connect your iPhone, then try again.",
                 taken, count);
#ifdef _WIN32
        {
            wchar_t *wide = ph_wide(text);
            MessageBoxW(NULL, wide ? wide : L"Porthole couldn't send the files.", L"Porthole",
                        MB_OK | MB_ICONWARNING);
            free(wide);
        }
#else
        fprintf(stderr, "%s\n", text);
#endif
        return 1;
    }
    return 0;
}
