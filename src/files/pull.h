#ifndef PORTHOLE_FILES_PULL_H
#define PORTHOLE_FILES_PULL_H

#include <stdbool.h>
#include <stddef.h>

/* A press that starts on a photo and then leaves that picture becomes a file
 * drag. Movement that stays on the picture keeps controlling the phone, unless
 * ph_pull_allow_inside was set for this press: a short drag then lifts the photo. */
#define PH_PULL_TRAVEL_PX 64
#define PH_PULL_LIFT_PX 16

typedef struct ph_pull {
    bool armed;
    bool pulling;
    bool lift;
    int press_x, press_y;
} ph_pull;

void ph_pull_reset(ph_pull *p);
void ph_pull_press(ph_pull *p, bool presentation, bool on_picture, int x, int y);
/* The next motions of this armed press may lift the photo without leaving it. */
void ph_pull_allow_inside(ph_pull *p);
bool ph_pull_lifts(const ph_pull *p);

/* True on the motion that takes the file. */
bool ph_pull_motion(ph_pull *p, bool on_picture, int x, int y);
void ph_pull_release(ph_pull *p);

/* Largest single file either way: no practical limit (free space decides).
 * The iPhone app accepts the same (TransferStaging.maxFileBytes). */
#define PH_TRANSFER_MAX_BYTES (1ull << 40)

/* "FILE <bytes> <name>" with a single safe file name. max_bytes rejects a huge claim. */
bool ph_pull_parse_file(const char *line, unsigned long long max_bytes, unsigned long long *size,
                        char *name, size_t name_len);

#endif
