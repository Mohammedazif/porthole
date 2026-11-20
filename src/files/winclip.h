#ifndef PORTHOLE_FILES_WINCLIP_H
#define PORTHOLE_FILES_WINCLIP_H

/* Pictures on the Windows clipboard as PNG bytes. Elsewhere both return false. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A picture copied since the last call, as PNG no larger than max. Caller frees *out. */
bool ph_winclip_read_png(uint8_t **out, size_t *len, size_t max);

/* Puts a PNG on the clipboard as PNG and as a bitmap. */
bool ph_winclip_write_png(const void *png, size_t len);

#endif
