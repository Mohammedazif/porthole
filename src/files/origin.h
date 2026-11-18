#ifndef PORTHOLE_FILES_ORIGIN_H
#define PORTHOLE_FILES_ORIGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The phone app connects here and answers a picture hash with the original file. */
#define PH_PULL_PORT 47731
#define PH_PULL_MAX_BYTES (200ull * 1024ull * 1024ull)

typedef struct ph_origin ph_origin;

typedef enum ph_origin_result {
    PH_ORIGIN_IDLE = 0,
    PH_ORIGIN_PENDING,
    PH_ORIGIN_FILE,
    PH_ORIGIN_MISS
} ph_origin_result;

/* Listens on the LAN. hint receives "address:port code XXXXXXXX" for the help text.
 * NULL if the socket cannot be opened; dragging still saves the screen picture. */
ph_origin *ph_origin_start(char *hint, size_t hint_len);
void ph_origin_stop(ph_origin *origin);

/* Asks the connected phone. thumb is PH_PICTURE_THUMB squared luma cells or NULL.
 * False when no phone is connected. */
bool ph_origin_ask(ph_origin *origin, uint64_t hash, double aspect, const uint8_t *thumb,
                   const char *dir);

/* One result for the latest ask. FILE writes the received path. */
ph_origin_result ph_origin_take(ph_origin *origin, char *path, size_t path_len);

#endif
