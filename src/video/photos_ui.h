#ifndef PORTHOLE_VIDEO_PHOTOS_UI_H
#define PORTHOLE_VIDEO_PHOTOS_UI_H

/* Parts of Photos' own screens that Porthole presses for the user, so that Photos
 * itself sends the exact originals through its Share sheet: nothing is matched. */

#include <stdbool.h>

#include "video/crop.h"

/* Photos' Share button: a round button at the bottom left of the screen, shown in
 * Select mode and on one photo or video. The library grid has a wide tab bar there
 * instead. screen is the phone screen inside the frame. On success *x, *y is its
 * centre in frame pixels. */
bool ph_photos_share_button(const ph_yuv_planes *p, ph_rect screen, int *x, int *y);

/* The Porthole app's icon in a Share sheet: a solid purple rounded square. On
 * success *x, *y is its centre in frame pixels. */
bool ph_photos_porthole_icon(const ph_yuv_planes *p, ph_rect screen, int *x, int *y);

#endif
