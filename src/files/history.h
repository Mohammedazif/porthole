#ifndef PORTHOLE_FILES_HISTORY_H
#define PORTHOLE_FILES_HISTORY_H

/* Main-thread transfer and clipboard history. Atomic per-user persistence;
 * removing records never touches source or destination files. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ph_transfer_state {
    PH_TRANSFER_QUEUED,
    PH_TRANSFER_ACTIVE,
    PH_TRANSFER_PAUSED,
    PH_TRANSFER_COMPLETE,
    PH_TRANSFER_FAILED,
    PH_TRANSFER_CANCELED,
    PH_TRANSFER_CHANGED
} ph_transfer_state;
typedef struct ph_file_entry {
    uint64_t id, bytes, confirmed;
    int64_t time;
    bool incoming;
    ph_transfer_state state;
    char device[129], name[256], path[1024], transfer_id[33];
    char digest[65], resume_key[80];
} ph_file_entry;
typedef struct ph_clip_entry {
    uint64_t id;
    int64_t time;
    bool incoming;
    char kind;
    size_t size;
    unsigned char *data;
} ph_clip_entry;
typedef struct ph_history ph_history;
typedef enum ph_file_availability {
    PH_FILE_PRESENT,
    PH_FILE_MISSING,
    PH_FILE_LOCATION_UNAVAILABLE,
    PH_FILE_NO_PATH
} ph_file_availability;

/* NULL path opens a volatile store, used when configuration is unavailable. */
ph_history *ph_history_open(const char *path);
void ph_history_close(ph_history *history);
bool ph_history_save(ph_history *history);
size_t ph_history_file_count(const ph_history *history);
size_t ph_history_clip_count(const ph_history *history);
/* Newest first. Pointers last until the next mutation. */
const ph_file_entry *ph_history_file(const ph_history *history, size_t index);
const ph_clip_entry *ph_history_clip(const ph_history *history, size_t index);
uint64_t ph_history_add_file(ph_history *history, const ph_file_entry *entry);
bool ph_history_update_file(ph_history *history, uint64_t id, ph_transfer_state state,
                            uint64_t confirmed, const char *path);
bool ph_history_replace_file(ph_history *history, uint64_t id, const ph_file_entry *entry);
/* Check the stored digest when available. Older records require both the
 * recorded size and basename. */
bool ph_history_match_file(const ph_file_entry *entry, const char *path);
uint64_t ph_history_add_clip(ph_history *history, char kind, const void *data, size_t size,
                             bool incoming);
bool ph_history_remove_file(ph_history *history, uint64_t id);
bool ph_history_remove_clips(ph_history *history, const uint64_t *ids, size_t count);
bool ph_history_remove_clip(ph_history *history, uint64_t id);
bool ph_history_clear_files(ph_history *history);
bool ph_history_clear_clips(ph_history *history);
/* Clear clipboard records in each local pairing history while retaining files. */
bool ph_history_clear_saved_clips(ph_history *current);
ph_file_availability ph_history_file_availability(const ph_file_entry *entry);

#endif
