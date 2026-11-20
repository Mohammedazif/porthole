#ifndef PORTHOLE_FILES_CLIP_H
#define PORTHOLE_FILES_CLIP_H

#include <stdbool.h>
#include <stddef.h>

/* One clipboard item on the phone link. T is UTF-8 text, P is a PNG.
 * The line is "CLIP <kind> <bytes>" and the bytes follow with no extra newline. */
#define PH_CLIP_TEXT_MAX (256u * 1024u)
#define PH_CLIP_IMAGE_MAX (8u * 1024u * 1024u)

bool ph_clip_header(const char *line, char *kind, size_t *size);

#endif
