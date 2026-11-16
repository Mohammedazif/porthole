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

#endif
