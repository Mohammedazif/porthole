#include "video/cursor.h"

#include <math.h>
#include <stdlib.h>

static const double directions[16][2] = {
    {1, 0}, {.924, .383}, {.707, .707}, {.383, .924}, {0, 1}, {-.383, .924},
    {-.707, .707}, {-.924, .383}, {-1, 0}, {-.924, -.383}, {-.707, -.707},
    {-.383, -.924}, {0, -1}, {.383, -.924}, {.707, -.707}, {.924, -.383},
};

static double score(const ph_yuv_planes *p, int x, int y, double r)
{
    int positive = 0, negative = 0, total = 0;
    for (int i = 0; i < 16; i++) {
        int ix = x + (int)lround((r - 1) * directions[i][0]);
        int iy = y + (int)lround((r - 1) * directions[i][1]);
        int ox = x + (int)lround((r + 2) * directions[i][0]);
        int oy = y + (int)lround((r + 2) * directions[i][1]);
        int delta = p->y[iy * p->y_stride + ix] - p->y[oy * p->y_stride + ox];
        positive += delta >= 8;
        negative += delta <= -8;
        total += abs(delta) < 64 ? abs(delta) : 64;
    }
    int coherent = positive > negative ? positive : negative;
    return coherent >= 13 ? (double)total * coherent / 16 : 0;
}

static bool valid(const ph_yuv_planes *p, const ph_rect *screen)
{
    return p && p->y && screen && p->y_stride > 0 && p->y_stride >= p->width
        && screen->w >= 80 && p->width >= screen->w && p->height >= screen->h
        && screen->h >= 80 && screen->x >= 0 && screen->y >= 0
        && screen->x <= p->width - screen->w && screen->y <= p->height - screen->h;
}

static bool changed(const ph_yuv_planes *p, const ph_rect *screen,
                    const ph_yuv_planes *before, const ph_rect *bs, int x, int y, double r)
{
    int count = 0;
    for (int i = 0; i < 16; i++) {
        for (int ring = 0; ring < 2; ring++) {
            double sample = ring ? r : r * .6;
            int dx = (int)lround(sample * directions[i][0]);
            int dy = (int)lround(sample * directions[i][1]);
            int a = p->y[(screen->y + y + dy) * p->y_stride + screen->x + x + dx];
            int b = before->y[(bs->y + y + dy) * before->y_stride + bs->x + x + dx];
            count += abs(a - b) >= 8;
            if (count >= 3)
                return true;
        }
    }
    return false;
}

static double changed_ring(const ph_yuv_planes *p, const ph_rect *screen,
                            const ph_yuv_planes *before, const ph_rect *bs,
                            int x, int y, double r)
{
    int matching = 0, edges = 0, total = 0;
    double sum = 0, old_sum = 0, squares = 0, old_squares = 0;
    for (int i = 0; i < 16; i++) {
        int ix = (int)lround((r - 1) * directions[i][0]);
        int iy = (int)lround((r - 1) * directions[i][1]);
        int ox = (int)lround((r + 2) * directions[i][0]);
        int oy = (int)lround((r + 2) * directions[i][1]);
        int inside = p->y[(screen->y + y + iy) * p->y_stride + screen->x + x + ix];
        int outside = p->y[(screen->y + y + oy) * p->y_stride + screen->x + x + ox];
        int old_in = before->y[(bs->y + y + iy) * before->y_stride + bs->x + x + ix];
        int old_out = before->y[(bs->y + y + oy) * before->y_stride + bs->x + x + ox];
        int delta = abs(inside - old_in) - abs(outside - old_out);
        matching += delta >= 8;
        edges += abs(inside - outside) >= 8;
        if (delta > 0)
            total += delta < 64 ? delta : 64;
        sum += inside;
        old_sum += old_in;
        squares += inside * inside;
        old_squares += old_in * old_in;
    }
    /* A translucent dot on a colourful icon can be lighter than one side and
     * darker than the other. Its changed-pixel silhouette still forms a circle. */
    double variance = squares / 16 - (sum / 16) * (sum / 16);
    double old_variance = old_squares / 16 - (old_sum / 16) * (old_sum / 16);
    return matching >= 12 && edges >= 9 && variance <= old_variance * 1.1 + 16
               ? (double)total * matching / 16 : 0;
}

static double candidate(const ph_yuv_planes *p, const ph_rect *screen,
                         const ph_yuv_planes *before, const ph_rect *bs,
                         int x, int y, double r, double hx, double hy)
{
    if (before) {
        int reach = (int)lround(r);
        const int offsets[5][2] = {{0, 0}, {-reach, 0}, {reach, 0}, {0, -reach}, {0, reach}};
        bool dirty = false;
        for (int i = 0; i < 5; i++) {
            int cx = x + offsets[i][0], cy = y + offsets[i][1];
            int a = p->y[(screen->y + cy) * p->y_stride + screen->x + cx];
            int b = before->y[(bs->y + cy) * before->y_stride + bs->x + cx];
            if (abs(a - b) >= 8) {
                dirty = true;
                break;
            }
        }
        if (!dirty)
            return 0;
    }
    double result = score(p, screen->x + x, screen->y + y, r);
    if (before)
        result = fmax(result, changed_ring(p, screen, before, bs, x, y, r));
    if (result < 250 || (before && !changed(p, screen, before, bs, x, y, r)))
        return 0;
    return result - hypot(x - hx, y - hy) * 0.15;
}

static double cursor_shapes(const ph_yuv_planes *p, const ph_rect *screen,
                             const ph_yuv_planes *before, const ph_rect *bs,
                             int x, int y, double r, double hx, double hy)
{
    /* iOS contracts the dot during a held click. A hover-only coarse search can
     * skip that smaller dot entirely when its centre falls between grid points. */
    return fmax(candidate(p, screen, before, bs, x, y, r, hx, hy),
                candidate(p, screen, before, bs, x, y, r * .82, hx, hy));
}

static bool locate(const ph_yuv_planes *p, const ph_rect *screen,
                    const ph_yuv_planes *before, const ph_rect *bs, double hint_x,
                    double hint_y, double radius, double *x, double *y)
{
    if (!valid(p, screen) || !x || !y || !isfinite(hint_x) || !isfinite(hint_y)
        || !isfinite(radius) || radius < 0
        || (before && (!valid(before, bs) || screen->w != bs->w || screen->h != bs->h)))
        return false;
    double r = fmin(screen->w, screen->h) * 11.0 / 500;
    int margin = (int)ceil(r + 4);
    double left = fmax(margin, hint_x - radius);
    double right = fmin(screen->w - margin - 1, hint_x + radius);
    double top = fmax(margin, hint_y - radius);
    double bottom = fmin(screen->h - margin - 1, hint_y + radius);
    if (left > right || top > bottom)
        return false;
    int x0 = (int)left, x1 = (int)right, y0 = (int)top, y1 = (int)bottom;
    double best = 0;
    int bx = 0, by = 0;
    for (int cy = y0; cy <= y1; cy += 2) {
        for (int cx = x0; cx <= x1; cx += 2) {
            double s = cursor_shapes(p, screen, before, bs, cx, cy, r, hint_x, hint_y);
            if (s > best) {
                best = s;
                bx = cx;
                by = cy;
            }
        }
    }
    if (best < 250)
        return false;
    for (int cy = y0; cy <= y1; cy += 2) {
        for (int cx = x0; cx <= x1; cx += 2) {
            if (hypot(cx - bx, cy - by) > 2 * r
                && cursor_shapes(p, screen, before, bs, cx, cy, r, hint_x, hint_y) > best * .92)
                return false;
        }
    }
    double refined = 0;
    int rx = bx, ry = by;
    for (int cy = by - 1; cy <= by + 1; cy++) {
        for (int cx = bx - 1; cx <= bx + 1; cx++) {
            for (int dr = -1; dr <= 1; dr++) {
                double s = cursor_shapes(p, screen, before, bs, cx, cy, r + dr,
                                          hint_x, hint_y);
                if (s > refined) {
                    refined = s;
                    rx = cx;
                    ry = cy;
                }
            }
        }
    }
    *x = rx;
    *y = ry;
    return true;
}

bool ph_video_cursor(const ph_yuv_planes *p, const ph_rect *screen, double hint_x,
                     double hint_y, double radius, double *x, double *y)
{
    return locate(p, screen, NULL, NULL, hint_x, hint_y, radius, x, y);
}

bool ph_video_cursor_changed(const ph_yuv_planes *p, const ph_rect *screen,
                             const ph_yuv_planes *before, const ph_rect *before_screen,
                             double hint_x, double hint_y, double radius, double *x, double *y)
{
    if (!before)
        return false;
    return locate(p, screen, before, before_screen, hint_x, hint_y, radius, x, y);
}
