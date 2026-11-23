#ifndef PORTHOLE_FILES_WINCLIP_H
#define PORTHOLE_FILES_WINCLIP_H

/* Pictures and text on the Windows clipboard. Elsewhere every call returns false. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A picture copied since the last call, as PNG no larger than max. Caller frees *out. */
bool ph_winclip_read_png(uint8_t **out, size_t *len, size_t max);

#ifdef _WIN32
typedef struct ph_winclip_image ph_winclip_image;

/* Create and destroy the hidden native clipboard owner on the UI thread. */
void ph_winclip_init(void);
void ph_winclip_shutdown(void);
uint32_t ph_winclip_sequence(void);
bool ph_winclip_image_supported(const void *png, size_t len);

/* Prepare on a worker, then publish on the UI thread if the clipboard sequence
 * still matches. Returns 1 if written, -1 on error, or 0 if superseded. */
ph_winclip_image *ph_winclip_prepare_image(const void *png, size_t len);
int ph_winclip_publish_image(ph_winclip_image *image, uint32_t expected);
void ph_winclip_discard_image(ph_winclip_image *image);
#endif

/* UTF-8 text, at most max bytes, with Windows newlines as LF. Caller frees *out. */
bool ph_winclip_read_text(char **out, size_t *len, size_t max);

/* Puts UTF-8 on the clipboard as CF_UNICODETEXT. Lone LF is stored as CRLF. */
bool ph_winclip_write_text(const char *utf8, size_t len);

#endif
