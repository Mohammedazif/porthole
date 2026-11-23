#include "core/config.h"
#include "core/upath.h"
#include <openssl/rand.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <shlobj.h>
#define PH_SEP "\\"
#else
#define PH_SEP "/"
#endif

static bool ensure_dir(const char *path)
{
    if (ph_mkdir(path) == 0 || errno == EEXIST)
        return true;
    return false;
}

bool ph_config_dir(char *out, size_t out_len)
{
    int n;
#ifdef _WIN32
    wchar_t wide[1024];
    char appdata[1024];
    DWORD length = GetEnvironmentVariableW(L"APPDATA", wide, (DWORD)(sizeof wide / sizeof *wide));
    if (!length) {
        PWSTR known = NULL;
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &known)))
            return false;
        bool ok = ph_narrow(known, appdata, sizeof appdata);
        CoTaskMemFree(known);
        if (!ok)
            return false;
    } else if (length >= sizeof wide / sizeof *wide || !ph_narrow(wide, appdata, sizeof appdata)) {
        return false;
    }
    n = snprintf(out, out_len, "%s\\Porthole", appdata);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char base[1024];

    if (xdg && *xdg) {
        n = snprintf(base, sizeof base, "%s", xdg);
    } else if (home && *home) {
        n = snprintf(base, sizeof base, "%s/.config", home);
    } else {
        return false;
    }
    if (n < 0 || (size_t)n >= sizeof base || !ensure_dir(base))
        return false;
    n = snprintf(out, out_len, "%s/porthole", base);
#endif
    if (n < 0 || (size_t)n >= out_len)
        return false;
    return ensure_dir(out);
}

bool ph_config_path(const char *name, char *out, size_t out_len)
{
    char dir[1024];
    int n;

    if (!ph_config_dir(dir, sizeof dir))
        return false;
    n = snprintf(out, out_len, "%s" PH_SEP "%s", dir, name);
    return n >= 0 && (size_t)n < out_len;
}

bool ph_config_read_line(const char *name, char *out, size_t out_len)
{
    char path[1100];
    FILE *f;
    bool ok = false;

    if (!ph_config_path(name, path, sizeof path))
        return false;
    f = ph_fopen(path, "rb");
    if (!f)
        return false;
    if (fgets(out, (int)out_len, f)) {
        out[strcspn(out, "\r\n")] = '\0';
        ok = out[0] != '\0';
    }
    fclose(f);
    return ok;
}

static bool write_config_file(const char *path, const char *line)
{
    FILE *f;
    bool ok;

    unsigned char random[8];
    char temporary[1140];
    if (RAND_bytes(random, sizeof random) != 1)
        return false;
    int n = snprintf(temporary, sizeof temporary, "%s.%02x%02x%02x%02x%02x%02x%02x%02x.tmp", path,
                     random[0], random[1], random[2], random[3], random[4], random[5], random[6],
                     random[7]);
    if (n < 0 || (size_t)n >= sizeof temporary)
        return false;
    f = ph_fopen(temporary, "wbx");
    if (!f)
        return false;
    ok = true;
#ifndef _WIN32
    ok = fchmod(fileno(f), 0600) == 0;
#endif
    ok = ok && fprintf(f, "%s\n", line) > 0 && fflush(f) == 0;
    if (fclose(f) != 0)
        ok = false;
    if (ok && ph_rename(temporary, path) == 0)
        return true;
    ph_remove(temporary);
    return false;
}

bool ph_config_write_line(const char *name, const char *line)
{
    char path[1100];
    return ph_config_path(name, path, sizeof path) && write_config_file(path, line);
}

#ifdef _WIN32
static const wchar_t startup_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool ph_config_launch_managed(void)
{
    PWSTR directory = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_CommonStartup, 0, NULL, &directory)))
        return false;
    wchar_t link[1024];
    int n = swprintf(link, sizeof link / sizeof *link, L"%ls\\Porthole.lnk", directory);
    CoTaskMemFree(directory);
    return n > 0 && (size_t)n < sizeof link / sizeof *link &&
           GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES;
}

static bool startup_command(wchar_t command[261])
{
    DWORD length = GetModuleFileNameW(NULL, command + 1, 259);
    if (!length || length >= 259)
        return false;
    command[0] = L'"';
    command[length + 1] = L'"';
    command[length + 2] = L'\0';
    return true;
}

bool ph_config_launch_at_sign_in(void)
{
    wchar_t command[261], saved[261];
    DWORD size = sizeof saved;
    return startup_command(command) &&
           RegGetValueW(HKEY_CURRENT_USER, startup_key, L"Porthole", RRF_RT_REG_SZ,
                        NULL, saved, &size) == ERROR_SUCCESS &&
           _wcsicmp(command, saved) == 0;
}

bool ph_config_set_launch_at_sign_in(bool enabled)
{
    if (ph_config_launch_managed())
        return false;
    if (!enabled) {
        LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, startup_key, L"Porthole");
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    wchar_t command[261];
    if (!startup_command(command))
        return false;
    return RegSetKeyValueW(HKEY_CURRENT_USER, startup_key, L"Porthole", REG_SZ, command,
                           (DWORD)((wcslen(command) + 1) * sizeof *command)) == ERROR_SUCCESS;
}
#else
bool ph_config_launch_managed(void)
{
    return false;
}

static bool startup_path(char out[1100], bool create)
{
    char parent[1024];
    if (!ph_config_dir(parent, sizeof parent))
        return false;
    *strrchr(parent, '/') = '\0';
    char directory[1050];
    int n = snprintf(directory, sizeof directory, "%s/autostart", parent);
    if (n < 0 || (size_t)n >= sizeof directory || (create && !ensure_dir(directory)))
        return false;
    n = snprintf(out, 1100, "%s/porthole-startup.desktop", directory);
    return n >= 0 && n < 1100;
}

bool ph_config_launch_at_sign_in(void)
{
    char path[1100];
    return startup_path(path, false) && access(path, F_OK) == 0;
}

bool ph_config_set_launch_at_sign_in(bool enabled)
{
    char path[1100];
    if (!startup_path(path, enabled))
        return false;
    if (!enabled)
        return ph_remove(path) == 0 || errno == ENOENT;
    return access("/usr/bin/porthole", X_OK) == 0 &&
           write_config_file(path, "[Desktop Entry]\nType=Application\nName=Porthole\n"
                                   "Exec=/usr/bin/porthole\nTryExec=/usr/bin/porthole\nTerminal=false");
}
#endif
