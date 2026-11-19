/* Finds the parts of Photos' screens that Porthole presses for the user. */
#include "video/photos_ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int luma_at(const ph_yuv_planes *p, double x, double y)
{
    int ix = (int)lround(x), iy = (int)lround(y);

    if (ix < 0)
        ix = 0;
    if (iy < 0)
        iy = 0;
    if (ix >= p->width)
        ix = p->width - 1;
    if (iy >= p->height)
        iy = p->height - 1;
    return p->y[iy * p->y_stride + ix];
}

/* How clearly a disc of radius r at cx, cy stands out all the way round: the third
 * weakest edge of sixteen around it. The left end of a wide bar is round too but has
 * no edge on its right, and a rounded square has none at its corners. */
static int disc_edge(const ph_yuv_planes *p, double cx, double cy, double r)
{
    int edge[16];

    for (int i = 0; i < 16; i++) {
        double angle = i * (3.14159265358979 / 8), c = cos(angle), s = sin(angle);
        edge[i] = abs(luma_at(p, cx + (r - 3) * c, cy + (r - 3) * s)
                      - luma_at(p, cx + (r + 3) * c, cy + (r + 3) * s));
    }
    for (int i = 0; i < 3; i++) /* partial sort: the three weakest first */
        for (int j = i + 1; j < 16; j++)
            if (edge[j] < edge[i]) {
                int t = edge[i];
                edge[i] = edge[j];
                edge[j] = t;
            }
    return edge[2];
}

/* A button's face is flat between its glyph and its rim. */
static bool flat_face(const ph_yuv_planes *p, double cx, double cy, double r)
{
    int lo = 255, hi = 0;

    for (int i = 0; i < 16; i++) {
        double angle = i * (3.14159265358979 / 8);
        int v = luma_at(p, cx + 0.8 * r * cos(angle), cy + 0.8 * r * sin(angle));

        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    return hi - lo <= 24;
}

static int strip_mean(const ph_yuv_planes *p, double x, double y0, double y1)
{
    int sum = 0, n = 0;

    for (double y = y0; y <= y1; y += 1, n++)
        sum += luma_at(p, x, y);
    return n ? sum / n : 0;
}

bool ph_photos_share_button(const ph_yuv_planes *p, ph_rect s, int *x, int *y)
{
    int best = 0;
    double bx = 0, by = 0, br = 0;

    if (!p || !p->y || s.w < 100 || s.h < 200)
        return false;
    for (double r = s.w * 0.04; r <= s.w * 0.075; r += 1)
        for (double cy = s.y + s.h * 0.915; cy <= s.y + s.h * 0.97; cy += 1)
            for (double cx = s.x + s.w * 0.07; cx <= s.x + s.w * 0.18; cx += 1) {
                int edge = disc_edge(p, cx, cy, r);

                if (edge > best && flat_face(p, cx, cy, r)) {
                    best = edge;
                    bx = cx;
                    by = cy;
                    br = r;
                }
            }
    if (best < 10)
        return false;
    /* Beside a lone button is the toolbar's own background, the same on both sides;
     * beside the end of a tab bar is the bar on one side only. */
    {
        int left = strip_mean(p, bx - br * 1.35, by - br * 0.3, by + br * 0.3);
        int right = strip_mean(p, bx + br * 1.7, by - br * 0.3, by + br * 0.3);

        if (abs(left - right) > 14)
            return false;
    }
    *x = (int)lround(bx);
    *y = (int)lround(by);
    return true;
}

/* The icon's purple, #C000FF, is Y 87 U 223 V 203; the stream blurs its edges. */
static bool icon_purple(const ph_yuv_planes *p, int cx, int cy)
{
    int u = p->u[cy * p->uv_stride + cx], v = p->v[cy * p->uv_stride + cx];
    return u >= 195 && v >= 175;
}

bool ph_photos_porthole_icon(const ph_yuv_planes *p, ph_rect s, int *x, int *y)
{
    int cw, ch, best = 0;
    unsigned char *seen;
    int *stack;

    if (!p || !p->y || !p->u || !p->v || s.w < 100 || s.h < 200)
        return false;
    cw = p->width / 2;
    ch = p->height / 2;
    seen = calloc((size_t)cw * ch, 1);
    stack = malloc(sizeof *stack * (size_t)cw * ch);
    if (!seen || !stack) {
        free(seen);
        free(stack);
        return false;
    }
    for (int cy = s.y / 2; cy < (s.y + s.h) / 2 && cy < ch; cy++)
        for (int cx = s.x / 2; cx < (s.x + s.w) / 2 && cx < cw; cx++) {
            int top = 0, cells = 0, x0 = cx, x1 = cx, y0 = cy, y1 = cy;

            if (seen[cy * cw + cx] || !icon_purple(p, cx, cy))
                continue;
            seen[cy * cw + cx] = 1;
            stack[top++] = cy * cw + cx;
            while (top > 0) {
                int at = stack[--top], ax = at % cw, ay = at / cw;
                static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};

                cells++;
                x0 = ax < x0 ? ax : x0;
                x1 = ax > x1 ? ax : x1;
                y0 = ay < y0 ? ay : y0;
                y1 = ay > y1 ? ay : y1;
                for (int k = 0; k < 4; k++) {
                    int nx = ax + dx[k], ny = ay + dy[k];
                    if (nx < 0 || ny < 0 || nx >= cw || ny >= ch || seen[ny * cw + nx]
                        || !icon_purple(p, nx, ny))
                        continue;
                    seen[ny * cw + nx] = 1;
                    stack[top++] = ny * cw + nx;
                }
            }
            /* A rounded square 6% to 25% of the screen's width, mostly purple (the
             * white ring takes about a fifth). */
            int bw = (x1 - x0 + 1) * 2, bh = (y1 - y0 + 1) * 2;
            if (bw < s.w * 6 / 100 || bw > s.w / 4 || bw * 5 < bh * 4 || bh * 5 < bw * 4)
                continue;
            if (cells * 100 < (x1 - x0 + 1) * (y1 - y0 + 1) * 45)
                continue;
            if (cells > best) {
                best = cells;
                *x = x0 + x1 + 1;
                *y = y0 + y1 + 1;
            }
        }
    free(seen);
    free(stack);
    return best > 0;
}
