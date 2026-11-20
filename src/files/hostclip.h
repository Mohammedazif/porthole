#ifndef PORTHOLE_FILES_HOSTCLIP_H
#define PORTHOLE_FILES_HOSTCLIP_H

#include <stddef.h>
#include <stdbool.h>

/* PC clipboard. Text uses SDL. A PNG uses the desktop clipboard.
 * changed() and apply() run on the thread that pumped SDL. */

void ph_hostclip_init(void);
void ph_hostclip_pump(void);
void ph_hostclip_shutdown(void);

/* A copy the user made on this PC. Caller frees *data. */
bool ph_hostclip_changed(char *kind, void **data, size_t *len);

/* A copy that arrived from the iPhone. A later changed() skips it. */
void ph_hostclip_apply(char kind, const void *data, size_t len);

#endif
