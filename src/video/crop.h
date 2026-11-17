#ifndef PORTHOLE_VIDEO_CROP_H
#define PORTHOLE_VIDEO_CROP_H

/* Finds the phone screen inside a decoded frame.
 *
 * When the app in front can rotate (e.g. the Photos viewer) but the phone is held upright,
 * iOS sends TV-shaped frames (e.g. 1920x1080) with the portrait screen in the middle and
 * black bars on both sides. The app shows and maps only the screen part. */

#include <stdbool.h>
#include <stdint.h>

typedef struct ph_rect {
    int x, y, w, h;
} ph_rect;

/* Planes of a YUV 4:2:0 picture. */
typedef struct ph_yuv_planes {
    const uint8_t *y, *u, *v;
    int y_stride, uv_stride;
    int width, height;
} ph_yuv_planes;

/* Returns the rectangle holding the phone screen; the whole frame unless it is a
 * landscape-shaped frame with visible content and pure black bars on both sides.
 * A frame matching the phone's native rotated shape is always kept whole.
 * `portrait_aspect` is width/height of the phone screen held upright (e.g. 0.462), learned
 * from portrait frames; pass 0 if unknown and the bars are found by scanning instead. */
ph_rect ph_video_content_rect(const ph_yuv_planes *p, double portrait_aspect);

/* Retain a confirmed crop while a same-size frame is too dark to distinguish
 * the phone screen from its bars. Zero-initialize/reset for each video session.
 * A new frame size defaults to the whole frame when its pixels are ambiguous. */
typedef struct ph_video_crop_state {
    ph_rect screen;
    int frame_width, frame_height;
} ph_video_crop_state;

ph_rect ph_video_content_rect_stable(const ph_yuv_planes *p, double portrait_aspect,
                                    ph_video_crop_state *state);
void ph_video_crop_reset(ph_video_crop_state *state);

#endif
