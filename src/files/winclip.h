#ifndef PORTHOLE_FILES_WINCLIP_H
#define PORTHOLE_FILES_WINCLIP_H

/* Pictures and text on the Windows clipboard. Elsewhere every call returns false. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A picture copied since the last call, as PNG no larger than max. Caller frees *out. */
bool ph_winclip_read_png(uint8_t **out, size_t *len, size_t max);

/* Puts a PNG on the clipboard as PNG and as a bitmap. */
bool ph_winclip_write_png(const void *png, size_t len);

/* UTF-8 text, at most max bytes, with Windows newlines as LF. Caller frees *out. */
bool ph_winclip_read_text(char **out, size_t *len, size_t max);

/* Puts UTF-8 on the clipboard as CF_UNICODETEXT. Lone LF is stored as CRLF. */
bool ph_winclip_write_text(const char *utf8, size_t len);

#endif
