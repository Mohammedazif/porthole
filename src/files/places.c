#include "files/places.h"

#include "core/config.h"
#include "core/log.h"
#include "core/sock.h"
#include "core/upath.h"

#include <errno.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

/* The local port the running Porthole takes "SEND <path>" on (files/origin.c). */
#define PH_PLACE_SEND_PORT 47734

bool ph_place_copy_unique(const char *source, const char *folder, const char *name, char *out,
                          size_t capacity)
{
    if (!source || !folder || !name || !*name || !out || !capacity ||
        strcmp(name, ph_path_base(name)) || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    out[0] = 0;
    unsigned char nonce[8];
    if (RAND_bytes(nonce, sizeof nonce) != 1)
        return false;
    char temporary[1100];
    int len = snprintf(temporary, sizeof temporary,
                       "%s/.porthole-%02x%02x%02x%02x%02x%02x%02x%02x.download", folder,
                       nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
    if (len < 0 || (size_t)len >= sizeof temporary)
        return false;
    FILE *in = ph_fopen(source, "rb");
    if (!in)
        return false;
    FILE *file = ph_fopen(temporary, "wbx");
    if (!file) {
        fclose(in);
        return false;
    }
    char buffer[65536];
    size_t count;
    bool ok = true;
    while (ok && (count = fread(buffer, 1, sizeof buffer, in)))
        ok = fwrite(buffer, 1, count, file) == count;
    ok = ok && !ferror(in) && ph_flush_file(file);
    fclose(in);
    if (fclose(file) != 0)
        ok = false;
    for (unsigned suffix = 1; ok && suffix < 1000; suffix++) {
        len = suffix == 1 ? snprintf(out, capacity, "%s/%s", folder, name)
                          : snprintf(out, capacity, "%s/%u-%s", folder, suffix, name);
        if (len < 0 || (size_t)len >= capacity)
            break;
        if (ph_publish_new(temporary, out))
            return true;
        if (!ph_path_exists(out))
            break;
    }
    ph_remove(temporary);
    out[0] = 0;
    return false;
}

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
        if (strcmp(line, "downloads") != 0) {
            if (!is_dir(line) || strlen(line) >= cap)
                return false;
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

static bool pick_path(char *out, size_t cap, bool folder)
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
            IFileOpenDialog_SetOptions(dialog, options |
                                                   (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST) |
                                                   FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            IFileOpenDialog_SetTitle(dialog, folder ? L"Receive folder" : L"Select file");
            if (SUCCEEDED(IFileOpenDialog_Show(dialog, NULL)) &&
                SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item)) &&
                SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide)) && wide &&
                ph_narrow(wide, out, cap) && (folder ? is_dir(out) : ph_file_size(out) >= 0))
                ok = true;
        }
        CoTaskMemFree(wide);
        if (item)
            IShellItem_Release(item);
        if (dialog)
            IFileOpenDialog_Release(dialog);
        if (SUCCEEDED(init))
            CoUninitialize();
        return ok;
    }
#else
    {
        const char *cmd = NULL;
        FILE *pipe;
        char buf[1024];

        if (access("/usr/bin/zenity", X_OK) == 0)
            cmd = !folder ? "zenity --file-selection"
                          : "zenity --file-selection --directory --title=\"Save files from the "
                            "iPhone\" 2>/dev/null";
        else if (access("/usr/bin/kdialog", X_OK) == 0)
            cmd = !folder ? "kdialog --getopenfilename"
                          : "kdialog --getexistingdirectory \"$HOME\" --title \"Save files from "
                            "the iPhone\" 2>/dev/null";
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
        if (!buf[0] || strlen(buf) >= cap || (folder ? !is_dir(buf) : ph_file_size(buf) < 0))
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

bool ph_place_pick_folder(char *out, size_t cap)
{
    return pick_path(out, cap, true);
}
bool ph_place_pick_file(char *out, size_t cap)
{
    return pick_path(out, cap, false);
}

bool ph_place_reveal(const char *path, bool select_file)
{
    if (!path || !ph_path_absolute(path) || !ph_path_exists(path))
        return false;
#ifdef _WIN32
    wchar_t *wide = ph_wide(path);
    if (!wide)
        return false;
    bool ok = false;
    if (select_file) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(wide);
        if (pidl) {
            ok = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, NULL, 0));
            ILFree(pidl);
        }
    } else
        ok = (INT_PTR)ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL) > 32;
    free(wide);
    return ok;
#else
    char directory[1100];
    if (snprintf(directory, sizeof directory, "%s", path) >= (int)sizeof directory)
        return false;
    if (select_file) {
        char *slash = strrchr(directory, '/');
        if (!slash)
            return false;
        if (slash == directory)
            slash[1] = 0;
        else
            *slash = 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        pid_t opener = fork();
        if (opener < 0)
            _exit(1);
        if (opener == 0) {
            if (select_file) {
                char uri[3400] = "array:string:file://";
                size_t used = strlen(uri);
                for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
                    if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || strchr("/-._~", *c))
                        uri[used++] = (char)*c;
                    else {
                        snprintf(uri + used, 4, "%%%02X", *c);
                        used += 3;
                    }
                }
                uri[used] = 0;
                pid_t show = fork();
                if (show == 0) {
                    execlp("dbus-send", "dbus-send", "--session", "--print-reply",
                           "--dest=org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                           "org.freedesktop.FileManager1.ShowItems", uri, "string:", (char *)NULL);
                    _exit(1);
                }
                int result;
                if (show > 0 && waitpid(show, &result, 0) == show && WIFEXITED(result) &&
                    WEXITSTATUS(result) == 0)
                    _exit(0);
            }
            execlp("xdg-open", "xdg-open", directory, (char *)NULL);
            _exit(1);
        }
        _exit(0);
    }
    int result;
    return pid > 0 && waitpid(pid, &result, 0) == pid && WIFEXITED(result) &&
           WEXITSTATUS(result) == 0;
#endif
}

int ph_place_pick_files(bool (*accept)(void *, const char *), void *user)
{
    if (!accept)
        return 0;
    int sent = 0;
#ifdef _WIN32
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IFileOpenDialog *dialog = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IFileOpenDialog, (void **)&dialog))) {
        DWORD options = 0, count = 0;
        IShellItemArray *items = NULL;
        IFileOpenDialog_GetOptions(dialog, &options);
        IFileOpenDialog_SetOptions(dialog, options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST |
                                               FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(IFileOpenDialog_Show(dialog, NULL)) &&
            SUCCEEDED(IFileOpenDialog_GetResults(dialog, &items))) {
            IShellItemArray_GetCount(items, &count);
            for (DWORD i = 0; i < count; i++) {
                IShellItem *item = NULL;
                PWSTR wide = NULL;
                char path[1024];
                bool ok = false;
                if (SUCCEEDED(IShellItemArray_GetItemAt(items, i, &item))) {
                    if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide)) &&
                        wide && ph_narrow(wide, path, sizeof path))
                        ok = accept(user, path);
                    IShellItem_Release(item);
                    CoTaskMemFree(wide);
                }
                if (!ok)
                    break;
                sent++;
            }
            IShellItemArray_Release(items);
        }
        IFileOpenDialog_Release(dialog);
    }
    if (SUCCEEDED(init))
        CoUninitialize();
#else
    const char *command = access("/usr/bin/zenity", X_OK) == 0
                              ? "zenity --file-selection --multiple --separator='\n'"
                          : access("/usr/bin/kdialog", X_OK) == 0
                              ? "kdialog --getopenfilename --multiple --separate-output"
                              : NULL;
    FILE *pipe = command ? popen(command, "r") : NULL;
    if (pipe) {
        char path[1100];
        while (fgets(path, sizeof path, pipe)) {
            size_t end = strcspn(path, "\r\n");
            if (!path[end])
                break;
            path[end] = 0;
            if (!accept(user, path))
                break;
            sent++;
        }
        pclose(pipe);
    }
#endif
    return sent;
}
