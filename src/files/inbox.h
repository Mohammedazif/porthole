#ifndef PORTHOLE_FILES_INBOX_H
#define PORTHOLE_FILES_INBOX_H

/* Photos and videos the iPhone sends from the Share sheet (select several in Photos,
 * Share, Porthole). The phone sends the files themselves, so nothing is matched.
 *
 * Wire format: the phone says "PSHR 1 <count>\n", then for each file
 * "FILE <bytes> <name>\n" and the bytes; the PC answers "OK\n" once each file is
 * saved. "DONE\n" ends the batch and the PC answers "BYE\n". */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PH_INBOX_PORT 47733
#define PH_INBOX_SERVICE "_porthole-share._tcp"
#define PH_INBOX_MAX_FILES 500

typedef struct ph_inbox ph_inbox;

typedef struct ph_inbox_item {
    char id[33], name[96], path[1024], message[128];
    char digest[65];
    uint64_t bytes, total;
    enum { PH_INBOX_RECEIVING, PH_INBOX_INTERRUPTED, PH_INBOX_STORED } state;
} ph_inbox_item;

typedef struct ph_inbox_callbacks {
    void *user;
    /* Whether the phone at this IPv4 address may send files. */
    bool (*allow)(void *user, const char *address);
    bool (*authorize)(void *user, const char *ticket);
    bool (*profile)(void *user, const char *ticket, char out[65]);
    void (*progress)(void *user, int done, int total);
    void (*item)(void *user, const ph_inbox_item *item);
    void (*saved)(void *user, const char *path); /* already in the receive folder */
    /* A finished legacy batch: each saved file's final path, one per line. */
    void (*received)(void *user, const char *paths, int count);
} ph_inbox_callbacks;

/* Saves batches under dir, each in its own folder. save_label is the folder name
 * the iPhone share sheet shows ("Downloads" when NULL). NULL if the port cannot
 * be opened. */
ph_inbox *ph_inbox_start(const char *dir, const char *save_label, const ph_inbox_callbacks *callbacks);
void ph_inbox_disconnect(ph_inbox *inbox);
void ph_inbox_stop(ph_inbox *inbox);

/* The share sheet reads this name from the PC on the network. */
void ph_inbox_set_save_label(ph_inbox *inbox, const char *save_label);

/* The file count of a "PSHR 1 <count>" hello, or -1. */
int ph_inbox_parse_hello(const char *line);

#endif
