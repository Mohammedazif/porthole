#ifndef PORTHOLE_FILES_HOSTCLIP_H
#define PORTHOLE_FILES_HOSTCLIP_H

#include <stddef.h>
#include <stdbool.h>

/* PC clipboard. Text uses SDL. A PNG uses the desktop clipboard.
 * changed() and apply() run on the thread that pumped SDL. */

void ph_hostclip_init(void);
/* Windows image completion: +1 copied, -1 failed, 0 no clipboard change. */
int ph_hostclip_pump(void);
void ph_hostclip_shutdown(void);
bool ph_hostclip_pending(void);
void ph_hostclip_cancel_pending(void);

/* A copy the user made on this PC. Caller frees *data. */
bool ph_hostclip_changed(char *kind, void **data, size_t *len);

/* A copy that arrived from the iPhone. A later changed() skips it. */
/* True when written or accepted for asynchronous Windows image conversion. */
bool ph_hostclip_apply(char kind, const void *data, size_t len);

#endif
