#include "video/scrollbar.h"

#include <stdlib.h>

bool ph_video_scrollbar(const ph_yuv_planes *p, const ph_rect *screen, ph_rect *bar)
{
    if (!p || !p->y || !screen || !bar || p->y_stride < p->width || screen->x < 0
        || screen->y < 0 || screen->w < 80 || screen->h < 80
        || screen->x > p->width - screen->w || screen->y > p->height - screen->h)
        return false;

    int pad = screen->w / 100;
    if (pad < 3)
        pad = 3;
    int inset = screen->w / 40;
    if (inset < 7)
        inset = 7;
    int first_y = screen->h * 8 / 100, end_y = screen->h * 98 / 100;
    int min_length = screen->h / 250;
    if (min_length < 5)
        min_length = 5;
    ph_rect best = {0};
    for (int x = screen->w - inset; x < screen->w - 2; x++) {
        int start = -1, last = 0;
        int right = x + pad < screen->w ? x + pad : screen->w - 1;
        for (int y = first_y; y <= end_y; y++) {
            bool hit = false;
            if (y < end_y) {
                const uint8_t *row = p->y + (screen->y + y) * p->y_stride + screen->x;
                int left = row[x - pad], centre = row[x], outside = row[right];
                int a = centre - left, b = centre - outside;
                /* A thin light OR dark stripe on an otherwise flat background. */
                hit = abs(left - outside) < 14 && a * b > 0 && abs(a) >= 18
                      && abs(b) >= 18;
            }
            if (hit) {
                if (start < 0)
                    start = y;
                last = y;
            }
            if (start >= 0 && (y - last > 2 || y == end_y)) {
                int length = last - start + 1;
                if (length >= min_length && length > best.h)
                    best = (ph_rect){x, start, 1, length};
                start = -1;
            }
        }
    }
    if (!best.h)
        return false;
    *bar = best;
    return true;
}
