#ifndef PORTHOLE_FILES_DIGEST_H
#define PORTHOLE_FILES_DIGEST_H

#include <stdbool.h>
#include <stdio.h>

/* SHA-256 from the start of file. Rewinds on success. The optional callback runs
 * periodically so the caller can service urgent work or cancel during a large read. */
bool ph_file_digest(FILE *file, char hex[65], bool (*work)(void *), void *user);

#endif
