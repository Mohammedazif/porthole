#ifndef PORTHOLE_VIDEO_CURSOR_H
#define PORTHOLE_VIDEO_CURSOR_H

#include "video/crop.h"

/* Locate the AssistiveTouch dot near an expected position. Returns picture-relative
 * pixels. A weak or ambiguous match is rejected; callers must not click on a guess. */
bool ph_video_cursor(const ph_yuv_planes *p, const ph_rect *screen, double hint_x,
                     double hint_y, double radius, double *x, double *y);
bool ph_video_cursor_changed(const ph_yuv_planes *p, const ph_rect *screen,
                             const ph_yuv_planes *before, const ph_rect *before_screen,
                             double hint_x, double hint_y, double radius, double *x, double *y);

/* For overlapping corrections, an earlier picture can retain the background
 * hidden by the old dot. Only use a recent reference from the same unpressed
 * movement. Reject changes to nearby content outside the confirmed old dot,
 * unchanged pictures, disappeared dots and ambiguous reference matches. */
bool ph_video_cursor_referenced(const ph_yuv_planes *p, const ph_rect *screen,
                                const ph_yuv_planes *before, const ph_rect *before_screen,
                                const ph_yuv_planes *reference, const ph_rect *reference_screen,
                                double old_x, double old_y, double hint_x, double hint_y,
                                double radius, double *x, double *y);

/* Changed-frame search robust to playing video: pixels that still change between
 * `p` and one or two pictures taken after it (`later`, optional `later2`, same
 * screen rect) are treated as video motion and excluded, optionally grown by
 * `dilate` pixels (0-4). The stopped dot stays visible against `before`. */
bool ph_video_cursor_settled(const ph_yuv_planes *p, const ph_rect *screen,
                             const ph_yuv_planes *later, const ph_yuv_planes *later2,
                             const ph_yuv_planes *before, const ph_rect *before_screen,
                             int dilate, double hint_x, double hint_y, double radius,
                             double *x, double *y);
#endif
