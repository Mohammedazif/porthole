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

bool ph_config_write_line(const char *name, const char *line)
{
    char path[1100];
    FILE *f;
    bool ok;

    if (!ph_config_path(name, path, sizeof path))
        return false;
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
