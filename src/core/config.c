#include "core/config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define PH_MKDIR(path) _mkdir(path)
#define PH_SEP "\\"
#else
#define PH_MKDIR(path) mkdir(path, 0700)
#define PH_SEP "/"
#endif

static bool ensure_dir(const char *path)
{
    if (PH_MKDIR(path) == 0 || errno == EEXIST)
        return true;
    return false;
}

bool ph_config_dir(char *out, size_t out_len)
{
    int n;
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata)
        return false;
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
    f = fopen(path, "r");
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
    f = fopen(path, "w");
    if (!f)
        return false;
    ok = fprintf(f, "%s\n", line) > 0;
    return fclose(f) == 0 && ok;
}
