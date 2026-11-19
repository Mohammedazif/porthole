#include "video/crop.h"

#include <math.h>
#include <stdlib.h>

#define BLACK_MAX_Y 32       /* limited-range black is 16, full-range 0; allow encoder noise */
#define BLACK_MAX_CHROMA 12  /* |U-128|, |V-128| */
#define SAMPLE_STEP 4        /* check every 4th row/column */
#define MAX_NON_BLACK 0.005  /* tolerate 0.5% stray pixels */
#define EDGE_MARGIN 2        /* ignore columns right next to the content edge (blur) */

/* The darkest luma that still counts as background. AirPlay's limited-range black is
 * 16 and is kept at BLACK_MAX_Y. The screen broadcast is full range: Photos' black is
 * 0 there, and a dark screenshot on it is about 17, which BLACK_MAX_Y called black.
 * Set only for one item search, on the thread doing it. */
static _Thread_local int black_max_y = BLACK_MAX_Y;
#define FULL_RANGE_BLACK_MAX_Y 10

static bool pixel_is_black(const ph_yuv_planes *p, int x, int y)
{
    int luma = p->y[y * p->y_stride + x];
    int cu = p->u[(y / 2) * p->uv_stride + x / 2] - 128;
    int cv = p->v[(y / 2) * p->uv_stride + x / 2] - 128;
    return luma <= black_max_y && abs(cu) <= BLACK_MAX_CHROMA && abs(cv) <= BLACK_MAX_CHROMA;
}

/* Full-range black when every corner of a portrait phone screen is near 0. */
static int item_black_max(const ph_yuv_planes *p)
{
    int darkest = 255;

    if (p->width >= p->height)
        return BLACK_MAX_Y;
    for (int corner = 0; corner < 4; corner++) {
        int x = corner & 1 ? p->width - 1 : 0;
        int y = corner & 2 ? p->height - 1 : 0;
        int value = p->y[y * p->y_stride + x];
        if (value < darkest)
            darkest = value;
    }
    return darkest <= 4 ? FULL_RANGE_BLACK_MAX_Y : BLACK_MAX_Y;
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
/* Keep dark detail that the old 82% cutoff mistook for empty padding, while
 * allowing sparse Photos button text above a picture to stay outside it. */
#define ITEM_GUTTER_PERCENT 94

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

/* A dark phone screenshot can have black padding inside the photo too. Photos
 * draws a thin frame around it. Look for that complete enclosing frame before
 * growing from one of the screenshot's internal cards or text blocks. */
static bool outline_ink(const ph_yuv_planes *p, int x, int y, int black)
{
    int cu, cv;

    if (x < 0 || y < 0 || x >= p->width || y >= p->height)
        return false;
    cu = p->u[(y / 2) * p->uv_stride + x / 2] - 128;
    cv = p->v[(y / 2) * p->uv_stride + x / 2] - 128;
    return p->y[y * p->y_stride + x] > black + 8 || abs(cu) > 12 || abs(cv) > 12;
}

static bool thin_vertical(const ph_yuv_planes *p, int x, int y, int black)
{
    int dx;

    for (dx = -1; dx <= 1; dx++) {
        int at = x + dx;
        if (outline_ink(p, at, y, black) && !outline_ink(p, at - 3, y, black)
            && !outline_ink(p, at + 3, y, black))
            return true;
    }
    return false;
}

static int horizontal_outline(const ph_yuv_planes *p, int near_y, int black)
{
    int y, best = -1, best_distance = 99;

    for (y = near_y - 5; y <= near_y + 5; y++) {
        int x, count = 0;
        if (y < 4 || y >= p->height - 4)
            continue;
        for (x = p->width / 2 - 20; x <= p->width / 2 + 20; x++) {
            count += outline_ink(p, x, y, black) && !outline_ink(p, x, y - 3, black)
                     && !outline_ink(p, x, y + 3, black);
        }
        if (count >= 36 && abs(y - near_y) < best_distance) {
            best = y;
            best_distance = abs(y - near_y);
        }
    }
    return best;
}

static ph_rect outlined_phone_photo(const ph_yuv_planes *p, int px, int py)
{
    ph_rect best = {0};
    double best_coverage = 0;
    int left, black = 16;

    if (p->width >= p->height || p->width < 100)
        return best;
    /* VideoToolbox can send full-range black (0), AirPlay limited-range (16). */
    for (int corner = 0; corner < 4; corner++) {
        int x = corner & 1 ? p->width - 1 : 0;
        int y = corner & 2 ? p->height - 1 : 0;
        int value = p->y[y * p->y_stride + x];
        if (value < black)
            black = value;
    }
    for (left = 16; left < p->width / 3; left++) {
        int right = p->width - 1 - left;
        int w = right - left + 1;
        int h = (int)lround((double)w * p->height / p->width);
        int first = p->height, last = -1, count = 0, y, top, bottom;
        double coverage;

        if (px <= left || px >= right || h >= p->height - 32
            || !thin_vertical(p, left, py, black) || !thin_vertical(p, right, py, black))
            continue;
        for (y = 16; y < p->height - 16; y++) {
            if (thin_vertical(p, left, y, black) && thin_vertical(p, right, y, black)) {
                if (first == p->height)
                    first = y;
                last = y;
                count++;
            }
        }
        coverage = (double)count / h;
        if (coverage < .80 || coverage <= best_coverage || last < first)
            continue;
        top = horizontal_outline(p, (first + last - h) / 2, black);
        bottom = horizontal_outline(p, (first + last + h) / 2, black);
        if (top < 0 || bottom < 0 || py <= top || py >= bottom
            || abs((bottom - top + 1) - h) > 8)
            continue;
        best = finish_item(left, top, right, bottom, p->width, p->height);
        best_coverage = coverage;
    }
    return best;
}

/* Ink at the item's own left or right side on row y: Photos' thin frame line. */
static bool side_ink(const ph_yuv_planes *p, int y, int left, int right)
{
    for (int d = -2; d <= 2; d++) {
        int l = left + d, r = right + d;
        if ((l >= 0 && l < p->width && !pixel_is_black(p, l, y))
            || (r >= 0 && r < p->width && !pixel_is_black(p, r, y)))
            return true;
    }
    return false;
}

static int row_ink_percent(const ph_yuv_planes *p, int y, int left, int right)
{
    int x, ink = 0;

    for (x = left; x <= right; x++)
        ink += !pixel_is_black(p, x, y);
    return ink * 100 / (right - left + 1);
}

/* A dark screenshot's status bar or bottom edge is black like the page around it.
 * Photos still frames the screenshot: thin lines run down both sides through that band
 * and a line closes it. Thumbnail gaps have no side lines, so they are never crossed. */
static int framed_edge(const ph_yuv_planes *p, int from, int step, int left, int right)
{
    int y, sided = 0, rows = 0;

    for (y = from; y >= 0 && y < p->height && rows < 120; y += step, rows++) {
        if (row_ink_percent(p, y, left, right) >= 80)
            /* The frame's corners are rounded, so the top rows miss the side line. */
            return rows >= 4 && sided * 100 >= rows * 60 ? y : -1;
        sided += side_ink(p, y, left, right);
    }
    return -1;
}

static ph_rect item_rect(const ph_yuv_planes *p, int x, int y);

ph_rect ph_video_item_rect(const ph_yuv_planes *p, int x, int y)
{
    ph_rect item;

    if (!p || !p->y || !p->u || !p->v || p->width < 16 || p->height < 16)
        return (ph_rect){0};
    if (x < 0 || y < 0 || x >= p->width || y >= p->height)
        return (ph_rect){0};
    black_max_y = item_black_max(p);
    item = item_rect(p, x, y);
    black_max_y = BLACK_MAX_Y;
    return item;
}

static ph_rect item_rect(const ph_yuv_planes *p, int x, int y)
{
    int sx, sy, left, top, right, bottom;
    ph_rect item;

    item = outlined_phone_photo(p, x, y);
    if (item.w >= ITEM_MIN_PX)
        return item;
    sx = x;
    sy = y;
    /* A press on the one-pixel gap belongs to the neighbouring thumbnail.
     * Growing from the gap itself would step into both cells. */
    if (pixel_is_black(p, x, y) && !nearest_ink(p, x, y, &sx, &sy))
        return (ph_rect){0};
    grow_item(p, sx, sy, &left, &top, &right, &bottom);
    if (right - left >= ITEM_MIN_PX) {
        int edge = framed_edge(p, top - 1, -1, left, right);
        if (edge >= 0)
            top = edge;
        edge = framed_edge(p, bottom + 1, 1, left, right);
        if (edge >= 0)
            bottom = edge;
    }
    item = finish_item(left, top, right, bottom, p->width, p->height);
    if (item.w < ITEM_MIN_PX || outside_item(item, x, y) > ITEM_SEEK_PX)
        return (ph_rect){0};
    return item;
}

/* Edge strength between neighbouring columns (vertical edges) or rows, summed over a
 * band. Cell gaps in the grid show as a pair of strong edges at every pitch. */
static double *column_edges(const ph_yuv_planes *p, int y0, int y1)
{
    double *e = calloc((size_t)p->width, sizeof *e);

    if (!e)
        return NULL;
    for (int y = y0; y < y1; y += 2) {
        const uint8_t *row = p->y + (size_t)y * p->y_stride;
        for (int x = 1; x < p->width; x++)
            e[x] += abs((int)row[x] - (int)row[x - 1]);
    }
    return e;
}

static double *row_edges(const ph_yuv_planes *p, int y0, int y1)
{
    double *e = calloc((size_t)p->height, sizeof *e);

    if (!e)
        return NULL;
    for (int y = y0 > 1 ? y0 : 1; y < y1; y++) {
        const uint8_t *row = p->y + (size_t)y * p->y_stride;
        const uint8_t *above = row - p->y_stride;
        for (int x = 0; x < p->width; x += 2)
            e[y] += abs((int)row[x] - (int)above[x]);
    }
    return e;
}

static double window_max(const double *e, int n, double lo, double hi)
{
    double best = 0;

    for (int i = (int)floor(lo); i <= (int)ceil(hi); i++)
        if (i >= 0 && i < n && e[i] > best)
            best = e[i];
    return best;
}

static double typical(const double *e, int lo, int hi)
{
    double sum = 0;
    int n = 0;

    for (int i = lo; i < hi; i++) {
        sum += e[i];
        n++;
    }
    return n ? sum / n : 0;
}

#define GRID_GAP 2

bool ph_video_grid(const ph_yuv_planes *p, int x, int y, ph_grid *out)
{
    int y0, y1, best_n = 0;
    double best = 0, base, *cols, *rows;

    if (!p || !p->y || !out || p->width < 120 || p->height <= p->width || x < 0 || x >= p->width)
        return false;
    y0 = y - 240 < 40 ? 40 : y - 240;
    y1 = y + 240 > p->height - 40 ? p->height - 40 : y + 240;
    if (y1 - y0 < 120)
        return false;
    cols = column_edges(p, y0, y1);
    if (!cols)
        return false;
    base = typical(cols, 1, p->width) + 1;
    /* Columns from 2 to 15: Photos zooms between 3, 5, 7 and more. Every boundary has
     * to stand well above the picture's own texture, so a photo is never a grid. */
    for (int n = 2; n <= 15; n++) {
        double pitch = (double)(p->width + GRID_GAP) / n, sum = 0, weakest = 1e30;
        for (int k = 1; k < n; k++) {
            double at = k * pitch;
            double v = window_max(cols, p->width, at - GRID_GAP - 3, at + 2) / base;
            sum += v;
            if (v < weakest)
                weakest = v;
        }
        double score = sum / (n - 1);
        if (weakest >= 2.5 && score > best * 1.1) {
            best = score;
            best_n = n;
        }
    }
    free(cols);
    if (!best_n || best < 4)
        return false;
    out->columns = best_n;
    out->pitch = (double)(p->width + GRID_GAP) / best_n;
    out->gap = GRID_GAP;
    rows = row_edges(p, y0, y1);
    if (!rows)
        return false;
    /* Rows scroll: find where a row begins from the same edge pairs across the band. */
    {
        double phase_best = -1, row_base = typical(rows, y0 + 1, y1) + 1;
        int phase = 0, count = 0;
        for (int ph = 0; ph < (int)out->pitch; ph++) {
            double sum = 0;
            int n = 0;
            for (double at = y0 + ph; at < y1; at += out->pitch, n++)
                sum += window_max(rows, p->height, at - GRID_GAP - 2, at + 1);
            if (n && sum / n > phase_best) {
                phase_best = sum / n;
                phase = ph;
                count = n;
            }
        }
        free(rows);
        if (count < 1 || phase_best / row_base < 2.5)
            return false;
        out->row_start = fmod(y0 + phase, out->pitch);
    }
    return true;
}

ph_rect ph_grid_cell(const ph_grid *g, int x, int y, int width, int height)
{
    ph_rect r = {0};
    double col, row, x0, y0;

    if (!g || g->pitch < 8)
        return r;
    col = floor((x + 0.5) / g->pitch);
    row = floor((y - g->row_start + 0.5) / g->pitch);
    x0 = col * g->pitch;
    y0 = g->row_start + row * g->pitch;
    /* Every whole pixel of the thumbnail and no more: the gap sits just before x0.
     * Cutting further in looks like a zoom to the phone, and a 3% zoom alone costs
     * the right photo most of its lead over the others. */
    r.x = (int)ceil(x0 - 0.01);
    r.y = (int)ceil(y0 - 0.01);
    r.w = (int)floor(x0 + g->pitch - g->gap + 0.01) - r.x;
    r.h = (int)floor(y0 + g->pitch - g->gap + 0.01) - r.y;
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.h += r.y;
        r.y = 0;
    }
    if (r.x + r.w > width)
        r.w = width - r.x;
    if (r.y + r.h > height)
        r.h = height - r.y;
    if (r.w < 8 || r.h < 8)
        return (ph_rect){0};
    return r;
}

/* iOS's selection blue (about #0A84FF / #007AFF) is strongly blue-difference positive
 * and red-difference negative; photos rarely have it this saturated in a small disc. */
static bool badge_blue(const ph_yuv_planes *p, int cx, int cy)
{
    int u = p->u[cy * p->uv_stride + cx], v = p->v[cy * p->uv_stride + cx];
    int luma = p->y[(cy * 2) * p->y_stride + cx * 2];
    return u >= 170 && v <= 105 && luma >= 50 && luma <= 210;
}

static bool badge_white(const ph_yuv_planes *p, int x, int y)
{
    int u = p->u[(y / 2) * p->uv_stride + x / 2] - 128;
    int v = p->v[(y / 2) * p->uv_stride + x / 2] - 128;
    return p->y[y * p->y_stride + x] >= 190 && abs(u) <= 24 && abs(v) <= 24;
}

#define BADGE_CELLS_MAX 2048 /* the largest blob followed, in chroma cells */

/* Selection badges whose blue disc starts inside area: each one's disc, in luma pixels. */
static int find_badges(const ph_yuv_planes *p, ph_rect area, ph_rect *out, int max)
{
    int cw, ch, found = 0, ax0, ay0, ax1, ay1;
    unsigned char *seen;
    int *stack;

    if (!p || !p->y || !p->u || !p->v || !out || max < 1 || p->width < 32 || p->height < 32)
        return 0;
    cw = p->width / 2;
    ch = p->height / 2;
    ax0 = area.x / 2 > 0 ? area.x / 2 : 0;
    ay0 = area.y / 2 > 0 ? area.y / 2 : 0;
    ax1 = (area.x + area.w + 1) / 2 < cw ? (area.x + area.w + 1) / 2 : cw;
    ay1 = (area.y + area.h + 1) / 2 < ch ? (area.y + area.h + 1) / 2 : ch;
    seen = calloc((size_t)cw * ch, 1);
    stack = malloc(sizeof *stack * BADGE_CELLS_MAX);
    if (!seen || !stack) {
        free(seen);
        free(stack);
        return 0;
    }
    for (int cy = ay0; cy < ay1 && found < max; cy++) {
        for (int cx = ax0; cx < ax1 && found < max; cx++) {
            int top = 0, cells = 0, x0 = cx, x1 = cx, y0 = cy, y1 = cy, white = 0, area_cells;
            bool overflow = false;

            if (seen[cy * cw + cx] || !badge_blue(p, cx, cy))
                continue;
            seen[cy * cw + cx] = 1;
            stack[top++] = cy * cw + cx;
            while (top > 0) {
                int at = stack[--top], x = at % cw, y = at / cw;
                static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};

                cells++;
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y > y1 ? y : y1;
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= cw || ny >= ch || seen[ny * cw + nx]
                        || !badge_blue(p, nx, ny))
                        continue;
                    seen[ny * cw + nx] = 1;
                    if (top < BADGE_CELLS_MAX)
                        stack[top++] = ny * cw + nx;
                    else
                        overflow = true;
                }
            }
            /* A disc 8 to 48 pixels across, filled about as a circle around a tick. */
            int bw = (x1 - x0 + 1) * 2, bh = (y1 - y0 + 1) * 2;
            if (overflow || bw < 8 || bh < 8 || bw > 48 || bh > 48 || bw * 4 < bh * 3
                || bh * 4 < bw * 3)
                continue;
            area_cells = (x1 - x0 + 1) * (y1 - y0 + 1);
            if (cells * 100 < area_cells * 40 || cells * 100 > area_cells * 92)
                continue;
            for (int y = y0 * 2; y <= y1 * 2 + 1 && y < p->height; y++)
                for (int x = x0 * 2; x <= x1 * 2 + 1 && x < p->width; x++)
                    white += badge_white(p, x, y);
            if (white * 100 < bw * bh * 4)
                continue; /* no white tick: blue in a photo, not a badge */
            out[found++] = (ph_rect){x0 * 2, y0 * 2, bw, bh};
        }
    }
    free(seen);
    free(stack);
    return found;
}

int ph_video_selected_items(const ph_yuv_planes *p, const ph_grid *grid, ph_rect *out, int max)
{
    ph_rect discs[64];
    int n, found = 0;

    if (!p || !out || max < 1)
        return 0;
    n = find_badges(p, (ph_rect){0, 0, p->width, p->height}, discs, 64);
    for (int i = 0; i < n && found < max; i++) {
        int x0 = discs[i].x / 2, y0 = discs[i].y / 2;
        int x1 = x0 + discs[i].w / 2 - 1, y1 = y0 + discs[i].h / 2 - 1;
        ph_rect item;

        /* The badge sits in the thumbnail's bottom-right corner. */
        if (grid) {
            item = ph_grid_cell(grid, x0 + x1, y0 + y1, p->width, p->height);
            if (item.w < 16 || item.h < 16)
                continue;
        } else {
            item = ph_video_item_rect(p, x0 * 2 - 4 > 0 ? x0 * 2 - 4 : 0,
                                      y0 * 2 - 4 > 0 ? y0 * 2 - 4 : 0);
            if (item.w < ITEM_MIN_PX || item.h < ITEM_MIN_PX || item.w > p->width * 3 / 4)
                continue;
        }
        bool again = false;
        for (int i2 = 0; i2 < found; i2++)
            again |= out[i2].x == item.x && out[i2].y == item.y;
        if (!again)
            out[found++] = item;
    }
    return found;
}

int ph_video_badges_in(const ph_yuv_planes *p, ph_rect area, ph_rect *out, int max)
{
    int n = find_badges(p, area, out, max);

    /* Grow each disc over its white ring and soft shadow, kept inside the area. */
    for (int i = 0; i < n; i++) {
        int grow = out[i].w / 4 + 2;
        int x0 = out[i].x - grow, y0 = out[i].y - grow;
        int x1 = out[i].x + out[i].w + grow, y1 = out[i].y + out[i].h + grow;

        x0 = x0 > area.x ? x0 : area.x;
        y0 = y0 > area.y ? y0 : area.y;
        x1 = x1 < area.x + area.w ? x1 : area.x + area.w;
        y1 = y1 < area.y + area.h ? y1 : area.y + area.h;
        out[i] = (ph_rect){x0, y0, x1 - x0, y1 - y0};
    }
    return n;
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
