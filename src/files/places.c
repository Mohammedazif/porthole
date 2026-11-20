#include "files/places.h"

#include "core/config.h"
#include "core/log.h"
#include "core/sock.h"
#include "core/upath.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

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

static bool is_dir(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = ph_wide(path);
    DWORD attr = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool same_dir(const char *a, const char *b)
{
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static void trim_slash(char *path)
{
    size_t n = strlen(path);

    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        if (n == 3 && path[1] == ':')
            break; /* keep C:\ */
        path[--n] = 0;
    }
}

static bool read_save(char *out, size_t cap)
{
    char path[1100];
    FILE *file;

    if (!ph_config_path("save-dir", path, sizeof path))
        return false;
    file = ph_fopen(path, "r");
    if (!file)
        return false;
    if (!fgets(out, (int)cap, file)) {
        fclose(file);
        return false;
    }
    fclose(file);
    out[strcspn(out, "\r\n")] = 0;
    return out[0] != 0;
}

static bool write_save(const char *line)
{
    char path[1100];
    FILE *file;

    if (!ph_config_path("save-dir", path, sizeof path))
        return false;
    file = ph_fopen(path, "w");
    if (!file)
        return false;
    fputs(line, file);
    fputc('\n', file);
    fclose(file);
    return true;
}

bool ph_place_save_dir(char *out, size_t cap)
{
    char line[1024];

    if (!out || cap < 4)
        return false;
    if (read_save(line, sizeof line)) {
        if (!strcmp(line, "desktop"))
            return ph_place_dir(PH_PLACE_DESKTOP, out, cap);
        if (strcmp(line, "downloads") != 0 && is_dir(line) && strlen(line) < cap) {
            snprintf(out, cap, "%s", line);
            return true;
        }
    }
    return ph_place_dir(PH_PLACE_DOWNLOADS, out, cap);
}

bool ph_place_save_label(char *out, size_t cap)
{
    char dir[1024], known[1024];
    const char *base;

    if (!out || cap < 12 || !ph_place_save_dir(dir, sizeof dir))
        return false;
    if (ph_place_dir(PH_PLACE_DOWNLOADS, known, sizeof known) && same_dir(dir, known)) {
        snprintf(out, cap, "Downloads");
        return true;
    }
    if (ph_place_dir(PH_PLACE_DESKTOP, known, sizeof known) && same_dir(dir, known)) {
        snprintf(out, cap, "Desktop");
        return true;
    }
    base = ph_path_base(dir);
    snprintf(out, cap, "%s", base[0] ? base : dir);
    return out[0] != 0;
}

bool ph_place_save_is_custom(void)
{
    char label[64];
    return ph_place_save_label(label, sizeof label) && strcmp(label, "Downloads") != 0;
}

bool ph_place_set_save_dir(const char *dir)
{
    char clean[1024], known[1024];

    if (!dir || !dir[0])
        return write_save("downloads");
    if (strlen(dir) >= sizeof clean || strchr(dir, '\n') || strchr(dir, '\r'))
        return false;
    snprintf(clean, sizeof clean, "%s", dir);
    trim_slash(clean);
    if (ph_place_dir(PH_PLACE_DOWNLOADS, known, sizeof known) && same_dir(clean, known))
        return write_save("downloads");
    if (ph_place_dir(PH_PLACE_DESKTOP, known, sizeof known) && same_dir(clean, known))
        return write_save("desktop");
    if (!is_dir(clean))
        return false;
    return write_save(clean);
}

bool ph_place_pick_folder(char *out, size_t cap)
{
    if (!out || cap < 4)
        return false;
#ifdef _WIN32
    {
        IFileOpenDialog *dialog = NULL;
        IShellItem *item = NULL;
        PWSTR wide = NULL;
        DWORD options = 0;
        HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        bool ok = false;

        if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                       &IID_IFileOpenDialog, (void **)&dialog))) {
            IFileOpenDialog_GetOptions(dialog, &options);
            IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
                                                    | FOS_PATHMUSTEXIST);
            IFileOpenDialog_SetTitle(dialog, L"Save files from the iPhone");
            if (SUCCEEDED(IFileOpenDialog_Show(dialog, NULL))
                && SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item))
                && SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide)) && wide
                && ph_narrow(wide, out, cap) && is_dir(out))
                ok = true;
        }
        CoTaskMemFree(wide);
        if (item)
            IShellItem_Release(item);
        if (dialog)
            IFileOpenDialog_Release(dialog);
        if (init == S_OK)
            CoUninitialize();
        return ok;
    }
#else
    {
        const char *cmd = NULL;
        FILE *pipe;
        char buf[1024];

        if (access("/usr/bin/zenity", X_OK) == 0)
            cmd = "zenity --file-selection --directory --title=\"Save files from the iPhone\" 2>/dev/null";
        else if (access("/usr/bin/kdialog", X_OK) == 0)
            cmd = "kdialog --getexistingdirectory \"$HOME\" --title \"Save files from the iPhone\" 2>/dev/null";
        if (!cmd)
            return false;
        pipe = popen(cmd, "r");
        if (!pipe)
            return false;
        if (!fgets(buf, sizeof buf, pipe)) {
            pclose(pipe);
            return false;
        }
        pclose(pipe);
        buf[strcspn(buf, "\r\n")] = 0;
        if (!buf[0] || strlen(buf) >= cap || !is_dir(buf))
            return false;
        snprintf(out, cap, "%s", buf);
        return true;
    }
#endif
}

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
