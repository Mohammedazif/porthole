#include "video/cursor.h"

#include <math.h>
#include <stdlib.h>

static const double directions[16][2] = {
    {1, 0}, {.924, .383}, {.707, .707}, {.383, .924}, {0, 1}, {-.383, .924},
    {-.707, .707}, {-.924, .383}, {-1, 0}, {-.924, -.383}, {-.707, -.707},
    {-.383, -.924}, {0, -1}, {.383, -.924}, {.707, -.707}, {.924, -.383},
};

typedef struct cursor_samples {
    int inner[16][2], outer[16][2], change[2][16][2];
    int reach;
} cursor_samples;

/* Sampling geometry depends on cursor size, not on the candidate pixel. Compute
 * it once per search rather than repeating rounding in every candidate score. */
static void sample_offsets(cursor_samples *s, double r)
{
    s->reach = (int)lround(r);
    for (int i = 0; i < 16; i++) {
        for (int axis = 0; axis < 2; axis++) {
            s->inner[i][axis] = (int)lround((r - 1) * directions[i][axis]);
            s->outer[i][axis] = (int)lround((r + 2) * directions[i][axis]);
            s->change[0][i][axis] = (int)lround(r * .6 * directions[i][axis]);
            s->change[1][i][axis] = (int)lround(r * directions[i][axis]);
        }
    }
}

static double score(const ph_yuv_planes *p, int x, int y, const cursor_samples *s,
                    bool require_coherent)
{
    int positive = 0, negative = 0, total = 0;
    for (int i = 0; i < 16; i++) {
        int ix = x + s->inner[i][0];
        int iy = y + s->inner[i][1];
        int ox = x + s->outer[i][0];
        int oy = y + s->outer[i][1];
        int delta = p->y[iy * p->y_stride + ix] - p->y[oy * p->y_stride + ox];
        positive += delta >= 8;
        negative += delta <= -8;
        total += abs(delta) < 64 ? abs(delta) : 64;
    }
    int coherent = positive > negative ? positive : negative;
    return !require_coherent || coherent >= 13 ? (double)total * coherent / 16 : 0;
}

static bool valid(const ph_yuv_planes *p, const ph_rect *screen)
{
    return p && p->y && screen && p->y_stride > 0 && p->y_stride >= p->width
        && screen->w >= 80 && p->width >= screen->w && p->height >= screen->h
        && screen->h >= 80 && screen->x >= 0 && screen->y >= 0
        && screen->x <= p->width - screen->w && screen->y <= p->height - screen->h;
}

static bool changed(const ph_yuv_planes *p, const ph_rect *screen,
                    const ph_yuv_planes *before, const ph_rect *bs, int x, int y,
                    const cursor_samples *s)
{
    int count = 0;
    for (int i = 0; i < 16; i++) {
        for (int ring = 0; ring < 2; ring++) {
            int dx = s->change[ring][i][0];
            int dy = s->change[ring][i][1];
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
                            int x, int y, const cursor_samples *s)
{
    int matching = 0, edges = 0, total = 0;
    double sum = 0, old_sum = 0, squares = 0, old_squares = 0;
    for (int i = 0; i < 16; i++) {
        int ix = s->inner[i][0];
        int iy = s->inner[i][1];
        int ox = s->outer[i][0];
        int oy = s->outer[i][1];
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
                         int x, int y, const cursor_samples *s, double hx, double hy)
{
    if (before) {
        int reach = s->reach;
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
    double result = score(p, screen->x + x, screen->y + y, s, true);
    if (before) {
        /* Text can already form a stronger circular edge than the translucent
         * pointer. A few changed pixels beside that edge do not establish its
         * centre: count only the edge strength gained since the old frame. */
        result -= score(before, bs->x + x, bs->y + y, s, false);
        result = fmax(result, changed_ring(p, screen, before, bs, x, y, s));
    }
    if (result < 250 || (before && !changed(p, screen, before, bs, x, y, s)))
        return 0;
    return result - hypot(x - hx, y - hy) * 0.15;
}

static double cursor_shapes(const ph_yuv_planes *p, const ph_rect *screen,
                             const ph_yuv_planes *before, const ph_rect *bs,
                             int x, int y, const cursor_samples shapes[2], double hx, double hy)
{
    /* iOS contracts the dot during a held click. A hover-only coarse search can
     * skip that smaller dot entirely when its centre falls between grid points. */
    return fmax(candidate(p, screen, before, bs, x, y, &shapes[0], hx, hy),
                candidate(p, screen, before, bs, x, y, &shapes[1], hx, hy));
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
    cursor_samples shapes[3][2];
    for (int dr = -1; dr <= 1; dr++) {
        sample_offsets(&shapes[dr + 1][0], r + dr);
        sample_offsets(&shapes[dr + 1][1], (r + dr) * .82);
    }
    int margin = (int)ceil(r + 4);
    double left = fmax(margin, hint_x - radius);
    double right = fmin(screen->w - margin - 1, hint_x + radius);
    double top = fmax(margin, hint_y - radius);
    double bottom = fmin(screen->h - margin - 1, hint_y + radius);
    if (left > right || top > bottom)
        return false;
    int x0 = (int)left, x1 = (int)right, y0 = (int)top, y1 = (int)bottom;
    if (before) {
        /* Candidate centres must be near at least one changed pixel. Bound
         * that region once so an exact-pixel search does not rescan hundreds
         * of thousands of unchanged pixels for a small pointer correction. */
        int reach = (int)ceil(r + 1);
        int dirty_left = screen->w, dirty_right = -1;
        int dirty_top = screen->h, dirty_bottom = -1;
        int scan_left = x0 - reach, scan_right = x1 + reach;
        int scan_top = y0 - reach, scan_bottom = y1 + reach;
        for (int cy = scan_top; cy <= scan_bottom; cy++) {
            const uint8_t *a = p->y + (screen->y + cy) * p->y_stride + screen->x;
            const uint8_t *b = before->y + (bs->y + cy) * before->y_stride + bs->x;
            for (int cx = scan_left; cx <= scan_right; cx++) {
                if (abs(a[cx] - b[cx]) < 8)
                    continue;
                if (cx < dirty_left) dirty_left = cx;
                if (cx > dirty_right) dirty_right = cx;
                if (cy < dirty_top) dirty_top = cy;
                dirty_bottom = cy;
            }
        }
        if (dirty_right < 0)
            return false;
        if (dirty_left - reach > x0) x0 = dirty_left - reach;
        if (dirty_right + reach < x1) x1 = dirty_right + reach;
        if (dirty_top - reach > y0) y0 = dirty_top - reach;
        if (dirty_bottom + reach < y1) y1 = dirty_bottom + reach;
    }
    double best = 0;
    int bx = 0, by = 0;
    /* Over text, sometimes only the actual centre has a convincing changed
     * ring. A two-pixel grid can skip it before refinement is reached. */
    for (int cy = y0; cy <= y1; cy++) {
        for (int cx = x0; cx <= x1; cx++) {
            double s = cursor_shapes(p, screen, before, bs, cx, cy, shapes[1], hint_x, hint_y);
            if (s > best) {
                best = s;
                bx = cx;
                by = cy;
            }
        }
    }
    if (best < 250)
        return false;
    for (int cy = y0; cy <= y1; cy++) {
        for (int cx = x0; cx <= x1; cx++) {
            if (hypot(cx - bx, cy - by) > 2 * r
                && cursor_shapes(p, screen, before, bs, cx, cy, shapes[1], hint_x, hint_y) > best * .92)
                return false;
        }
    }
    double refined = 0;
    int rx = bx, ry = by;
    for (int cy = by - 1; cy <= by + 1; cy++) {
        for (int cx = bx - 1; cx <= bx + 1; cx++) {
            for (int dr = -1; dr <= 1; dr++) {
                double s = cursor_shapes(p, screen, before, bs, cx, cy, shapes[dr + 1],
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

bool ph_video_cursor_referenced(const ph_yuv_planes *p, const ph_rect *screen,
                                const ph_yuv_planes *before, const ph_rect *before_screen,
                                const ph_yuv_planes *reference, const ph_rect *reference_screen,
                                double old_x, double old_y, double hint_x, double hint_y,
                                double radius, double *x, double *y)
{
    if (!x || !y || !valid(p, screen) || !valid(before, before_screen)
        || !valid(reference, reference_screen)
        || screen->w != before_screen->w || screen->h != before_screen->h
        || screen->w != reference_screen->w || screen->h != reference_screen->h
        || !isfinite(old_x) || !isfinite(old_y) || old_x < 0 || old_y < 0
        || old_x >= screen->w || old_y >= screen->h)
        return false;
    double px, py;
    if (!ph_video_cursor_changed(p, screen, reference, reference_screen,
                                 hint_x, hint_y, radius, &px, &py))
        return false;
    double r = fmin(screen->w, screen->h) * 11.0 / 500;
    if (hypot(px - old_x, py - old_y) > 2 * r)
        return false;
    cursor_samples samples;
    sample_offsets(&samples, r);
    if (!changed(p, screen, before, before_screen, (int)px, (int)py, &samples))
        return false;
    /* The earlier picture is only a background reference while the scene is
     * unchanged outside the old dot. A new icon/menu cannot supply its circle. */
    int left = (int)fmax(0, fmin(old_x, px) - r - 5);
    int right = (int)fmin(screen->w - 1, fmax(old_x, px) + r + 5);
    int top = (int)fmax(0, fmin(old_y, py) - r - 5);
    int bottom = (int)fmin(screen->h - 1, fmax(old_y, py) + r + 5);
    int dirty = 0;
    for (int cy = top; cy <= bottom; cy++) {
        for (int cx = left; cx <= right; cx++) {
            if (hypot(cx - old_x, cy - old_y) <= r + 4)
                continue;
            int a = before->y[(before_screen->y + cy) * before->y_stride + before_screen->x + cx];
            int b = reference->y[(reference_screen->y + cy) * reference->y_stride
                                 + reference_screen->x + cx];
            if (abs(a - b) >= 8 && ++dirty > 3)
                return false;
        }
    }
    *x = px;
    *y = py;
    return true;
}

/* Change between two post-movement pictures that counts as video motion. The
 * dot itself is translucent, so mild background drift shows through it too;
 * only stronger change (moving objects, cuts) is masked. */
#ifndef SETTLED_MOTION
#define SETTLED_MOTION 8
#endif

bool ph_video_cursor_settled(const ph_yuv_planes *p, const ph_rect *screen,
                             const ph_yuv_planes *later, const ph_yuv_planes *later2,
                             const ph_yuv_planes *before, const ph_rect *before_screen,
                             int dilate, double hint_x, double hint_y, double radius,
                             double *x, double *y)
{
    if (!x || !y || !valid(p, screen) || !valid(later, screen) || !valid(before, before_screen)
        || (later2 && !valid(later2, screen)) || screen->w != before_screen->w
        || screen->h != before_screen->h || dilate < 0 || dilate > 4 || !isfinite(radius))
        return false;
    /* Pixels that still change between pictures taken after the movement are
     * video motion, not the dot, which has stopped. Hide them from the changed
     * search by giving the baseline the current value there. The window is the
     * search area plus the dot and dilation margin. */
    int w = screen->w, h = screen->h;
    int reach = (int)ceil(radius + fmin(w, h) * 11.0 / 500 + 8 + dilate);
    int left = (int)fmax(0, floor(hint_x) - reach), right = (int)fmin(w - 1, ceil(hint_x) + reach);
    int top = (int)fmax(0, floor(hint_y) - reach), bottom = (int)fmin(h - 1, ceil(hint_y) + reach);
    if (left > right || top > bottom)
        return false;
    int mw = right - left + 1, mh = bottom - top + 1;
    uint8_t *moving = calloc((size_t)mw * mh, 1);
    uint8_t *base = malloc((size_t)w * h);
    if (!moving || !base) {
        free(moving);
        free(base);
        return false;
    }
    for (int cy = top; cy <= bottom; cy++) {
        const uint8_t *a = p->y + (screen->y + cy) * p->y_stride + screen->x;
        const uint8_t *b = later->y + (screen->y + cy) * later->y_stride + screen->x;
        const uint8_t *c = later2 ? later2->y + (screen->y + cy) * later2->y_stride + screen->x : NULL;
        for (int cx = left; cx <= right; cx++)
            if (abs(a[cx] - b[cx]) >= SETTLED_MOTION || (c && abs(a[cx] - c[cx]) >= SETTLED_MOTION))
                moving[(cy - top) * mw + (cx - left)] = 1;
    }
    for (int cy = 0; cy < h; cy++)
        for (int cx = 0; cx < w; cx++)
            base[cy * w + cx] = before->y[(before_screen->y + cy) * before->y_stride
                                          + before_screen->x + cx];
    for (int cy = top; cy <= bottom; cy++) {
        for (int cx = left; cx <= right; cx++) {
            bool hide = false;
            for (int dy = -dilate; dy <= dilate && !hide; dy++) {
                int yy = cy + dy - top;
                if (yy < 0 || yy >= mh)
                    continue;
                for (int dx = -dilate; dx <= dilate && !hide; dx++) {
                    int xx = cx + dx - left;
                    hide = xx >= 0 && xx < mw && moving[yy * mw + xx];
                }
            }
            if (hide)
                base[cy * w + cx] = p->y[(screen->y + cy) * p->y_stride + screen->x + cx];
        }
    }
    ph_yuv_planes masked = {base, NULL, NULL, w, 0, w, h};
    ph_rect masked_screen = {0, 0, w, h};
    bool found = locate(p, screen, &masked, &masked_screen, hint_x, hint_y, radius, x, y);
    free(moving);
    free(base);
    return found;
}
