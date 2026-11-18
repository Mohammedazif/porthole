#ifndef PORTHOLE_FILES_PULL_H
#define PORTHOLE_FILES_PULL_H

#include <stdbool.h>
#include <stddef.h>

/* A press that starts on a photo and then leaves that picture becomes a file
 * drag. Movement that stays on the picture keeps controlling the phone. */
#define PH_PULL_TRAVEL_PX 64

typedef struct ph_pull {
    bool armed;
    bool pulling;
    int press_x, press_y;
} ph_pull;

void ph_pull_reset(ph_pull *p);
void ph_pull_press(ph_pull *p, bool presentation, bool on_picture, int x, int y);

/* True on the motion that leaves the picture far enough to take the file. */
bool ph_pull_motion(ph_pull *p, bool on_picture, int x, int y);
void ph_pull_release(ph_pull *p);

/* "FILE <bytes> <name>" with a single safe file name. max_bytes rejects a huge claim. */
bool ph_pull_parse_file(const char *line, unsigned long long max_bytes, unsigned long long *size,
                        char *name, size_t name_len);

#endif
