#ifndef PORTHOLE_FILES_ORIGIN_H
#define PORTHOLE_FILES_ORIGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "files/picture_hash.h"

/* The phone app connects here and answers a picture hash with the original file. */
#define PH_PULL_PORT 47731
/* Bonjour type the iPhone browses to list this PC. The pairing code is not in the advert. */
#define PH_PULL_SERVICE "_porthole-pc._tcp"
/* A long 4K video can be several gigabytes. */
#define PH_PULL_MAX_BYTES (16ull * 1024ull * 1024ull * 1024ull)

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

/* Replaces the pairing code, drops the connected phone, and rewrites hint. */
bool ph_origin_renew(ph_origin *origin, char *hint, size_t hint_len);

/* Drops the connected phone but keeps the pairing code. */
void ph_origin_forget(ph_origin *origin);

/* Queues a regular file for the connected phone. False when the path is unusable
 * or another send is already waiting. The phone must have said FILES. */
bool ph_origin_put_file(ph_origin *origin, const char *path);
void ph_origin_cancel_push(ph_origin *origin);

/* True while a file is queued or still moving. name receives the file name. */
bool ph_origin_push_busy(ph_origin *origin, char *name, size_t name_len);

/* How much of the file moving now has gone, 0 to 100; -1 when nothing is moving. */
int ph_origin_push_percent(ph_origin *origin);

/* 0 none, 1 sent, 2 failed, 3 cancelled. name receives the file name. */
int ph_origin_take_push(ph_origin *origin, char *name, size_t name_len);

/* Asks the connected phone. thumb is PH_PICTURE_THUMB squared luma cells or NULL;
 * detail is PH_PICTURE_DETAIL squared cells of the same picture, or NULL. It is sent
 * as a fifth PULL field, which phones before 2.5 refuse.
 * False when no phone is connected. */
/* The whole phone screen under a drag, for a picture whose edges cannot be seen,
 * such as a black screenshot on Photos' black. The phone places each library item
 * on it the way Photos shows one item, fitted to the screen. Phones from 2.6. */
typedef struct ph_origin_screen {
    double aspect;
    uint8_t thumb[PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    uint8_t detail[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL];
} ph_origin_screen;

bool ph_origin_ask(ph_origin *origin, uint64_t hash, double aspect, const uint8_t *thumb,
                   const uint8_t *detail, const ph_origin_screen *screen, const char *dir);

/* The name this PC is listed under in the iPhone app (the computer's host name). */
void ph_origin_pc_name(char *out, size_t len);

/* IPv4 address of the phone that last gave the right code, kept after it disconnects. */
bool ph_origin_peer(ph_origin *origin, char *address, size_t len);

/* One result for the latest ask. FILE writes the received path. */
ph_origin_result ph_origin_take(ph_origin *origin, char *path, size_t path_len);

/* Clipboard on the same link. kind is 'T' (UTF-8) or 'P' (PNG).
 * put keeps the newest item until the phone has asked for clipboard sync.
 * take hands back one item the phone sent. Caller frees *data. */
#define PH_ORIGIN_CLIP_SENT 1u
#define PH_ORIGIN_CLIP_GOT 2u
#define PH_ORIGIN_CLIP_READY 4u

bool ph_origin_put_clip(ph_origin *origin, char kind, const void *data, size_t len);
bool ph_origin_take_clip(ph_origin *origin, char *kind, void **data, size_t *len);
unsigned ph_origin_clip_flags(ph_origin *origin);

#endif
