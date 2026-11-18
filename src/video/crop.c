#include "video/crop.h"

#include <math.h>
#include <stdlib.h>

#define BLACK_MAX_Y 32       /* limited-range black is 16, full-range 0; allow encoder noise */
#define BLACK_MAX_CHROMA 12  /* |U-128|, |V-128| */
#define SAMPLE_STEP 4        /* check every 4th row/column */
#define MAX_NON_BLACK 0.005  /* tolerate 0.5% stray pixels */
#define EDGE_MARGIN 2        /* ignore columns right next to the content edge (blur) */

static bool pixel_is_black(const ph_yuv_planes *p, int x, int y)
{
    int luma = p->y[y * p->y_stride + x];
    int cu = p->u[(y / 2) * p->uv_stride + x / 2] - 128;
    int cv = p->v[(y / 2) * p->uv_stride + x / 2] - 128;
    return luma <= BLACK_MAX_Y && abs(cu) <= BLACK_MAX_CHROMA && abs(cv) <= BLACK_MAX_CHROMA;
}

/* True if columns [x0, x1) are (almost) entirely black. */
static bool columns_black(const ph_yuv_planes *p, int x0, int x1)
{
    long total = 0, non_black = 0;

    if (x1 <= x0)
        return false;
    for (int y = 0; y < p->height; y += SAMPLE_STEP) {
        for (int x = x0; x < x1; x += SAMPLE_STEP) {
            total++;
            non_black += !pixel_is_black(p, x, y);
        }
    }
    return total > 0 && non_black <= (long)(MAX_NON_BLACK * (double)total);
}

static bool column_black(const ph_yuv_planes *p, int x)
{
    return columns_black(p, x, x + 1);
}

static ph_rect make_rect(int left, int right, int height)
{
    /* Even offsets keep the crop aligned with the half-resolution chroma planes. */
    ph_rect r;
    r.x = left & ~1;
    r.w = ((right - r.x) + 1) & ~1;
    r.y = 0;
    r.h = height;
    return r;
}

/* First index in [lo, hi] whose column is not black. */
static int first_nonblack(const ph_yuv_planes *p, int lo, int hi)
{
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (column_black(p, mid))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Last index in [lo, hi] whose column is not black. */
static int last_nonblack(const ph_yuv_planes *p, int lo, int hi)
{
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (column_black(p, mid))
            hi = mid - 1;
        else
            lo = mid;
    }
    return lo;
}

static bool row_black(const ph_yuv_planes *p, int y)
{
    long total = 0, non_black = 0;

    if (y < 0 || y >= p->height)
        return true;
    for (int x = 0; x < p->width; x += SAMPLE_STEP) {
        total++;
        non_black += !pixel_is_black(p, x, y);
    }
    return total > 0 && non_black <= (long)(MAX_NON_BLACK * (double)total);
}

static int first_content_row(const ph_yuv_planes *p, int lo, int hi)
{
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (row_black(p, mid))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int last_content_row(const ph_yuv_planes *p, int lo, int hi)
{
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (row_black(p, mid))
            hi = mid - 1;
        else
            lo = mid;
    }
    return lo;
}

/* A portrait picture centred between black side bars. Wider than the phone is
 * still the picture the user is looking at; a landscape frame that fills the
 * canvas is left whole. */
static bool centred_portrait(const ph_yuv_planes *p, ph_rect *out)
{
    int W = p->width, H = p->height;
    int left, right, cw;

    if (W < 16 || !column_black(p, 0) || !column_black(p, W - 1))
        return false;
    left = first_nonblack(p, 0, W / 2);
    right = last_nonblack(p, W / 2, W - 1);
    if (left < 8 || W - 1 - right < 8 || abs(left - (W - 1 - right)) > 8)
        return false;
    cw = right - left + 1;
    if (cw < 8 || cw >= H || (double)cw / H < 0.30)
        return false;
    *out = make_rect(left, right + 1, H);
    return true;
}

static ph_rect content_rect(const ph_yuv_planes *p, double portrait_aspect, bool *ambiguous)
{
    ph_rect full = {0, 0, p->width, p->height};
    int W = p->width, H = p->height;
    *ambiguous = false;

    if (W <= H || H <= 0)
        return full; /* portrait-shaped frames are sent without bars */

    bool known_aspect = portrait_aspect > 0.2 && portrait_aspect < 1.0;
    /* A native rotated phone frame already has the reciprocal portrait shape.
     * Black video or a centred subtitle cannot turn it into a pillarboxed TV
     * canvas. Allow two short-edge pixels for codec/chroma size rounding. */
    if (known_aspect && fabs(W * portrait_aspect - H) <= 2.0)
        return full;

    if (columns_black(p, 0, W)) {
        *ambiguous = true;
        return full; /* black alone is no evidence of portrait side bars */
    }

    if (known_aspect) {
        int content_w = (int)lround(H * portrait_aspect);
        int bar = (W - content_w) / 2;
        if (bar >= 8 && columns_black(p, 0, bar - EDGE_MARGIN) &&
            columns_black(p, W - bar + EDGE_MARGIN, W))
            return make_rect(bar, W - bar, H);
    }

    /* A photo or video can sit in the middle, wider than the phone, with black
     * bars on both sides. Show that portrait picture instead of the landscape canvas. */
    {
        ph_rect portrait;
        if (centred_portrait(p, &portrait))
            return portrait;
    }
    return full;
}

ph_rect ph_video_picture_rect(const ph_yuv_planes *p)
{
    ph_rect full;
    int W, H, left, right, top, bottom, cw, ch, x1, y1;

    if (!p || !p->y || !p->u || !p->v || p->width < 16 || p->height < 16)
        return (ph_rect){0};
    W = p->width;
    H = p->height;
    full = (ph_rect){0, 0, W, H};
    left = first_nonblack(p, 0, W);
    right = last_nonblack(p, 0, W - 1);
    if (left >= W || right < left || column_black(p, left) || column_black(p, right))
        return full;
    top = first_content_row(p, 0, H);
    bottom = last_content_row(p, 0, H - 1);
    if (top >= H || bottom < top || row_black(p, top) || row_black(p, bottom))
        return full;
    if (left < 8)
        left = 0;
    if (W - 1 - right < 8)
        right = W - 1;
    if (top < 8)
        top = 0;
    if (H - 1 - bottom < 8)
        bottom = H - 1;
    cw = right - left + 1;
    ch = bottom - top + 1;
    if (cw < 64 || ch < 64)
        return full;
    if (left == 0 && top == 0 && right == W - 1 && bottom == H - 1)
        return full;
    x1 = (right + 2) & ~1;
    y1 = (bottom + 2) & ~1;
    if (x1 > W)
        x1 = W & ~1;
    if (y1 > H)
        y1 = H & ~1;
    full.x = left & ~1;
    full.y = top & ~1;
    if (x1 < full.x + 2 || y1 < full.y + 2)
        return (ph_rect){0, 0, W, H};
    full.w = x1 - full.x;
    full.h = y1 - full.y;
    return full;
}

#define ITEM_MIN_PX 64
#define ITEM_SEEK_PX 8
#define ITEM_GUTTER_PERCENT 82

static bool gutter_column(const ph_yuv_planes *p, int x, int y0, int y1)
{
    int y, total = 0, black = 0;

    if (x < 0 || x >= p->width || y1 < y0)
        return true;
    for (y = y0; y <= y1; y++) {
        total++;
        black += pixel_is_black(p, x, y);
    }
    return black * 100 >= total * ITEM_GUTTER_PERCENT;
}

static bool gutter_row(const ph_yuv_planes *p, int y, int x0, int x1)
{
    int x, total = 0, black = 0;

    if (y < 0 || y >= p->height || x1 < x0)
        return true;
    for (x = x0; x <= x1; x++) {
        total++;
        black += pixel_is_black(p, x, y);
    }
    return black * 100 >= total * ITEM_GUTTER_PERCENT;
}

/* Grows from a content pixel. A black gap stays outside, including the
 * single-pixel line between library thumbnails. */
static void grow_item(const ph_yuv_planes *p, int sx, int sy, int *left, int *top, int *right,
                      int *bottom)
{
    int l = sx, t = sy, r = sx, b = sy;
    int guard = 0;
    bool grew = true;
    int limit = p->width + p->height + 2;

    while (grew && guard++ < limit) {
        grew = false;
        if (l > 0 && !gutter_column(p, l - 1, t, b)) {
            l--;
            grew = true;
        }
        if (r + 1 < p->width && !gutter_column(p, r + 1, t, b)) {
            r++;
            grew = true;
        }
        if (t > 0 && !gutter_row(p, t - 1, l, r)) {
            t--;
            grew = true;
        }
        if (b + 1 < p->height && !gutter_row(p, b + 1, l, r)) {
            b++;
            grew = true;
        }
    }
    *left = l;
    *top = t;
    *right = r;
    *bottom = b;
}

static ph_rect finish_item(int left, int top, int right, int bottom, int width, int height)
{
    ph_rect out = {0};
    int x = (left + 1) & ~1;
    int y = (top + 1) & ~1;
    int x1 = (right + 1) & ~1;
    int y1 = (bottom + 1) & ~1;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > width)
        x1 = width & ~1;
    if (y1 > height)
        y1 = height & ~1;
    if (x1 - x < ITEM_MIN_PX || y1 - y < ITEM_MIN_PX)
        return out;
    out.x = x;
    out.y = y;
    out.w = x1 - x;
    out.h = y1 - y;
    return out;
}

static bool nearest_ink(const ph_yuv_planes *p, int x, int y, int *ox, int *oy)
{
    int dy, dx, best = ITEM_SEEK_PX + 1;
    bool found = false;

    for (dy = -ITEM_SEEK_PX; dy <= ITEM_SEEK_PX; dy++) {
        for (dx = -ITEM_SEEK_PX; dx <= ITEM_SEEK_PX; dx++) {
            int nx = x + dx, ny = y + dy, dist;

            if (nx < 0 || ny < 0 || nx >= p->width || ny >= p->height)
                continue;
            if (pixel_is_black(p, nx, ny))
                continue;
            dist = abs(dx) + abs(dy);
            if (dist < best) {
                best = dist;
                *ox = nx;
                *oy = ny;
                found = true;
            }
        }
    }
    return found;
}

static int outside_item(ph_rect r, int x, int y)
{
    int dx = 0, dy = 0;

    if (r.w < 1 || r.h < 1)
        return ITEM_SEEK_PX + 1;
    if (x < r.x)
        dx = r.x - x;
    else if (x >= r.x + r.w)
        dx = x - (r.x + r.w - 1);
    if (y < r.y)
        dy = r.y - y;
    else if (y >= r.y + r.h)
        dy = y - (r.y + r.h - 1);
    return dx > dy ? dx : dy;
}

ph_rect ph_video_item_rect(const ph_yuv_planes *p, int x, int y)
{
    int sx, sy, left, top, right, bottom;
    ph_rect item;

    if (!p || !p->y || !p->u || !p->v || p->width < 16 || p->height < 16)
        return (ph_rect){0};
    if (x < 0 || y < 0 || x >= p->width || y >= p->height)
        return (ph_rect){0};
    sx = x;
    sy = y;
    /* A press on the one-pixel gap belongs to the neighbouring thumbnail.
     * Growing from the gap itself would step into both cells. */
    if (pixel_is_black(p, x, y) && !nearest_ink(p, x, y, &sx, &sy))
        return (ph_rect){0};
    grow_item(p, sx, sy, &left, &top, &right, &bottom);
    item = finish_item(left, top, right, bottom, p->width, p->height);
    if (item.w < ITEM_MIN_PX || outside_item(item, x, y) > ITEM_SEEK_PX)
        return (ph_rect){0};
    return item;
}

ph_rect ph_video_content_rect(const ph_yuv_planes *p, double portrait_aspect)
{
    bool ambiguous;
    return content_rect(p, portrait_aspect, &ambiguous);
}

ph_rect ph_video_content_rect_stable(const ph_yuv_planes *p, double portrait_aspect,
                                    ph_video_crop_state *state)
{
    bool ambiguous;
    ph_rect screen = content_rect(p, portrait_aspect, &ambiguous);
    if (!state)
        return screen;
    if (ambiguous && state->frame_width == p->width && state->frame_height == p->height)
        screen = state->screen;
    state->screen = screen;
    state->frame_width = p->width;
    state->frame_height = p->height;
    return screen;
}

void ph_video_crop_reset(ph_video_crop_state *state)
{
    if (state)
        *state = (ph_video_crop_state){0};
}

bool ph_video_matches_phone_screen(ph_rect screen, double portrait_aspect)
{
    if (screen.w <= 0 || screen.h <= 0)
        return false;
    if (!isfinite(portrait_aspect) || portrait_aspect <= .2 || portrait_aspect >= 1)
        return true; /* Native phone shape has not been established. */
    int short_edge = screen.w < screen.h ? screen.w : screen.h;
    int long_edge = screen.w < screen.h ? screen.h : screen.w;
    return fabs(short_edge - long_edge * portrait_aspect) <= 2.0;
}
