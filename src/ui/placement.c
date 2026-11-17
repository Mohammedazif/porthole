#include "ui/placement.h"

#include <stdio.h>
#include <string.h>

bool ph_placement_format(const ph_placement *placement, char *out, size_t out_len)
{
    int n;

    if (!placement || !placement->valid || !out || out_len == 0)
        return false;
    n = snprintf(out, out_len, "%d %d %d %d %d %d %d", placement->x, placement->y, placement->w,
                 placement->h, placement->fullscreen ? 1 : 0, placement->always_on_top ? 1 : 0,
                 placement->one_to_one ? 1 : 0);
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool ph_placement_parse(const char *line, ph_placement *out)
{
    int x, y, w, h, fullscreen, top, one_to_one;
    int used = 0;

    if (out)
        memset(out, 0, sizeof *out);
    if (!line || !out)
        return false;
    if (sscanf(line, "%d %d %d %d %d %d %d %n", &x, &y, &w, &h, &fullscreen, &top, &one_to_one,
               &used) != 7 ||
        used <= 0 || line[used] != '\0')
        return false;
    if (w <= 0 || h <= 0)
        return false;
    if ((fullscreen != 0 && fullscreen != 1) || (top != 0 && top != 1) ||
        (one_to_one != 0 && one_to_one != 1))
        return false;
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    out->fullscreen = fullscreen != 0;
    out->always_on_top = top != 0;
    out->one_to_one = one_to_one != 0;
    out->valid = true;
    return true;
}

void ph_chrome_one_to_one(int video_w, int video_h, int screen_w, int screen_h, int *out_w,
                          int *out_h)
{
    long long vw, vh, sw, sh, ow, oh;

    if (!out_w || !out_h)
        return;
    if (video_w < 1 || video_h < 1) {
        *out_w = 1;
        *out_h = 1;
        return;
    }
    if (screen_w < 1)
        screen_w = 1;
    if (screen_h < 1)
        screen_h = 1;
    if (video_w <= screen_w && video_h <= screen_h) {
        *out_w = video_w;
        *out_h = video_h;
        return;
    }
    vw = video_w;
    vh = video_h;
    sw = screen_w;
    sh = screen_h;
    /* Round half up from the tighter edge. The real quotient never exceeds the screen. */
    if (sw * vh <= sh * vw) {
        ow = sw;
        oh = (vh * sw + vw / 2) / vw;
    } else {
        oh = sh;
        ow = (vw * sh + vh / 2) / vh;
    }
    if (ow > sw)
        ow = sw;
    if (oh > sh)
        oh = sh;
    if (ow < 1)
        ow = 1;
    if (oh < 1)
        oh = 1;
    *out_w = (int)ow;
    *out_h = (int)oh;
}

static ph_window_insets insets_or_zero(const ph_window_insets *insets)
{
    ph_window_insets z = {0, 0, 0, 0};

    if (!insets)
        return z;
    z.top = insets->top > 0 ? insets->top : 0;
    z.left = insets->left > 0 ? insets->left : 0;
    z.bottom = insets->bottom > 0 ? insets->bottom : 0;
    z.right = insets->right > 0 ? insets->right : 0;
    return z;
}

static int clamp_int(int value, int lo, int hi)
{
    if (hi < lo)
        hi = lo;
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

void ph_chrome_fit_client(int pic_w, int pic_h, int usable_w, int usable_h,
                          const ph_window_insets *insets, int *out_w, int *out_h)
{
    ph_window_insets frame = insets_or_zero(insets);
    int inner_w, inner_h, limit_w, limit_h;

    if (!out_w || !out_h)
        return;
    if (usable_w < 1)
        usable_w = 1;
    if (usable_h < 1)
        usable_h = 1;
    inner_w = usable_w - frame.left - frame.right;
    inner_h = usable_h - frame.top - frame.bottom;
    if (inner_w < 1)
        inner_w = 1;
    if (inner_h < 1)
        inner_h = 1;
    /* 90% of the area inside the frame, so a rotated phone stays clear of the taskbar. */
    limit_w = inner_w * 9 / 10;
    limit_h = inner_h * 9 / 10;
    if (limit_w < 1)
        limit_w = 1;
    if (limit_h < 1)
        limit_h = 1;
    ph_chrome_one_to_one(pic_w, pic_h, limit_w, limit_h, out_w, out_h);
}

void ph_chrome_place_client(int client_w, int client_h, int usable_x, int usable_y,
                            int usable_w, int usable_h, const ph_window_insets *insets,
                            int anchor_x, int anchor_y, int anchor_w, int anchor_h,
                            bool center, int *out_x, int *out_y)
{
    ph_window_insets frame = insets_or_zero(insets);
    int x, y, min_x, max_x, min_y, max_y;

    if (!out_x || !out_y)
        return;
    if (client_w < 1)
        client_w = 1;
    if (client_h < 1)
        client_h = 1;
    if (usable_w < 1)
        usable_w = 1;
    if (usable_h < 1)
        usable_h = 1;
    if (center || anchor_w < 1 || anchor_h < 1) {
        int outer_w = frame.left + client_w + frame.right;
        int outer_h = frame.top + client_h + frame.bottom;
        x = usable_x + (usable_w - outer_w) / 2 + frame.left;
        y = usable_y + (usable_h - outer_h) / 2 + frame.top;
    } else {
        int old_cx = anchor_x - frame.left + (frame.left + anchor_w + frame.right) / 2;
        int old_cy = anchor_y - frame.top + (frame.top + anchor_h + frame.bottom) / 2;
        int outer_w = frame.left + client_w + frame.right;
        int outer_h = frame.top + client_h + frame.bottom;
        x = old_cx - outer_w / 2 + frame.left;
        y = old_cy - outer_h / 2 + frame.top;
    }
    min_x = usable_x + frame.left;
    max_x = usable_x + usable_w - frame.right - client_w;
    min_y = usable_y + frame.top;
    max_y = usable_y + usable_h - frame.bottom - client_h;
    *out_x = clamp_int(x, min_x, max_x);
    *out_y = clamp_int(y, min_y, max_y);
}
