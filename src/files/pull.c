#include "files/pull.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void ph_pull_reset(ph_pull *p)
{
    if (p)
        memset(p, 0, sizeof *p);
}

void ph_pull_press(ph_pull *p, bool presentation, bool on_picture, int x, int y)
{
    if (!p || p->pulling)
        return;
    p->armed = presentation && on_picture;
    p->press_x = x;
    p->press_y = y;
}

bool ph_pull_motion(ph_pull *p, bool on_picture, int x, int y)
{
    long dx, dy;

    if (!p || !p->armed || p->pulling || on_picture)
        return false;
    dx = (long)x - p->press_x;
    dy = (long)y - p->press_y;
    if (dx * dx + dy * dy < (long)PH_PULL_TRAVEL_PX * PH_PULL_TRAVEL_PX)
        return false;
    p->pulling = true;
    p->armed = false;
    return true;
}

void ph_pull_release(ph_pull *p)
{
    if (!p)
        return;
    p->armed = false;
    p->pulling = false;
}

bool ph_pull_parse_file(const char *line, unsigned long long max_bytes, unsigned long long *size,
                        char *name, size_t name_len)
{
    unsigned long long n = 0;
    char raw[128];
    size_t i, o = 0;
    bool dot = false;

    if (!line || !size || !name || name_len < 2 || max_bytes < 1)
        return false;
    raw[0] = 0;
    if (sscanf(line, "FILE %llu %127s", &n, raw) != 2)
        return false;
    if (n < 1 || n > max_bytes || !raw[0] || raw[0] == '.')
        return false;
    for (i = 0; raw[i] && o + 1 < name_len && o < 80; i++) {
        unsigned char c = (unsigned char)raw[i];

        if (c == '/' || c == '\\' || c == ':')
            return false;
        if (c == ' ')
            c = '_';
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-'))
            return false;
        if (c == '.')
            dot = true;
        name[o++] = (char)c;
    }
    name[o] = 0;
    if (!dot || !o || name[0] == '.')
        return false;
    *size = n;
    return true;
}
