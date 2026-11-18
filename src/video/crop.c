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
