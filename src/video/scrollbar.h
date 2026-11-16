#ifndef PORTHOLE_VIDEO_SCROLLBAR_H
#define PORTHOLE_VIDEO_SCROLLBAR_H

#include "video/crop.h"

/* Finds the narrow iOS scroll indicator beside the phone picture. Coordinates are
 * relative to that picture. Missing/faded indicators return false. */
bool ph_video_scrollbar(const ph_yuv_planes *p, const ph_rect *screen, ph_rect *bar);

#endif
