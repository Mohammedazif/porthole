#ifndef PORTHOLE_FILES_ORIGIN_H
#define PORTHOLE_FILES_ORIGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "files/picture_hash.h"
#include "control/connection_policy.h"

/* The phone app connects here and answers a picture hash with the original file. */
#define PH_PULL_PORT 47731
/* A long 4K video can be several gigabytes. */
#define PH_PULL_MAX_BYTES (16ull * 1024ull * 1024ull * 1024ull)

typedef struct ph_origin ph_origin;

typedef enum ph_origin_result {
    PH_ORIGIN_IDLE = 0,
    PH_ORIGIN_PENDING,
    PH_ORIGIN_FILE,
    PH_ORIGIN_MISS
} ph_origin_result;

typedef enum ph_origin_start_status {
    PH_ORIGIN_START_OK,
    PH_ORIGIN_START_IDENTITY,
    PH_ORIGIN_START_LISTENER,
    PH_ORIGIN_START_FAILED
} ph_origin_start_status;

/* Listens on the LAN. hint receives the address and any active pairing code.
 * On failure returns NULL and optionally reports why. */
ph_origin *ph_origin_start(char *hint, size_t hint_len, ph_origin_start_status *status);
void ph_origin_stop(ph_origin *origin);

/* Replaces the pairing code, drops the connected phone, and rewrites hint. */
bool ph_origin_renew(ph_origin *origin, char *hint, size_t hint_len);
bool ph_origin_refresh_address(ph_origin *origin, char *hint, size_t hint_len);
bool ph_origin_add_phone(ph_origin *origin, char *hint, size_t hint_len);
bool ph_origin_choose_phone(ph_origin *origin, const char *id);
bool ph_origin_rename_phone(ph_origin *origin, const char *id, const char *name);
bool ph_origin_remove_saved_phone(ph_origin *origin, const char *id);
bool ph_origin_selected_id(ph_origin *origin, char id[65]);
unsigned ph_origin_generation(ph_origin *origin, const char *ticket);
unsigned ph_origin_inbox_generation(ph_origin *origin, const char *ticket);
void ph_origin_screen_live(ph_origin *origin, unsigned generation, bool live);
struct ph_session_state;
void ph_origin_sessions(ph_origin *origin, struct ph_session_state *state);

/* Drops the connected phone but keeps the pairing code. */
bool ph_origin_authorize(ph_origin *origin, const char *ticket);
bool ph_origin_verifying_controls(ph_origin *origin);
bool ph_origin_issue_control_proof(ph_origin *origin, const char code[17]);
bool ph_origin_control_proof_current(ph_origin *origin, const char code[17]);
bool ph_origin_controls_verified(ph_origin *origin);
void ph_origin_reset_control_verification(ph_origin *origin);
/* A stable opaque namespace for durable inbox records on this pairing. */
bool ph_origin_receive_profile(ph_origin *origin, const char *ticket, char out[65]);
void ph_origin_forget(ph_origin *origin);

/* Queues a regular file for the connected phone. False when the path is unusable
 * or another send is already waiting. The phone must have said FILES. */
bool ph_origin_put_file(ph_origin *origin, const char *path);
bool ph_origin_restart_file(ph_origin *origin, const char *path);
void ph_origin_cancel_push(ph_origin *origin);

typedef struct ph_origin_send {
    uint64_t history_id, bytes, confirmed;
    char path[1024], name[96], id[33], digest[65], resume_key[80];
} ph_origin_send;

typedef enum ph_origin_send_state {
    PH_SEND_NONE,
    PH_SEND_STORED,
    PH_SEND_FAILED,
    PH_SEND_CANCELED,
    PH_SEND_CHANGED,
    PH_SEND_QUEUED,
    PH_SEND_ACTIVE
} ph_origin_send_state;

/* Retry uses the same transfer identity and wire filename even when Locate
 * selected a different path. Restart intentionally uses a fresh wire ID. */
bool ph_origin_retry_file(ph_origin *origin, const ph_origin_send *previous, bool restart);
ph_origin_send_state ph_origin_take_send(ph_origin *origin, ph_origin_send *send);

/* True while a file is queued or still moving. name receives the file name. */
bool ph_origin_push_busy(ph_origin *origin, char *name, size_t name_len);

/* How much of the file moving now has gone, 0 to 100; -1 when nothing is moving. */
int ph_origin_push_percent(ph_origin *origin);

/* 0 none, 1 sent, 2 failed, 3 cancelled. name receives the file name. */
int ph_origin_take_push_path(ph_origin *origin, char *path, size_t path_len);
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

/* False once a phone has used the pairing code. The digits stay accepted for that
 * phone and stay off the screen until New code. */
bool ph_origin_code_expired(ph_origin *origin);
bool ph_origin_code_open(ph_origin *origin);
/* The name the phone sent for itself. Empty until it does. */
void ph_origin_phone_name(ph_origin *origin, char *out, size_t len);

/* IPv4 address of the phone that last gave the right code, kept after it disconnects. */
bool ph_origin_remember_verified_phone_name(ph_origin *origin, const char *name);
bool ph_origin_peer(ph_origin *origin, char *address, size_t len);

/* One result for the latest ask. FILE writes the received path. */
void ph_origin_cancel_pull(ph_origin *origin);
ph_origin_result ph_origin_take(ph_origin *origin, char *path, size_t path_len);

/* Clipboard on the same link. kind is 'T' (UTF-8) or 'P' (PNG).
 * put keeps the newest item until the phone has asked for clipboard sync.
 * take hands back one item the phone sent. Caller frees *data. */
#define PH_ORIGIN_CLIP_SENT 1u
#define PH_ORIGIN_CLIP_GOT 2u
#define PH_ORIGIN_CLIP_READY 4u

bool ph_origin_put_clip(ph_origin *origin, char kind, const void *data, size_t len);
bool ph_origin_take_clip(ph_origin *origin, char *kind, void **data, size_t *len);
/* SENT and GOT are consumed by this call. READY describes the current authenticated session. */
unsigned ph_origin_clip_flags(ph_origin *origin);

/* Asks the connected phone to send its pasteboard. Phones that do not know CLIP? ignore it.
 * waiting is true until that answer is taken, or for about two seconds. */
void ph_origin_ask_clip(ph_origin *origin);
bool ph_origin_clip_waiting(ph_origin *origin);

/* Phone preferences negotiated over the authenticated photo/file connection. */
ph_control_policy ph_origin_control_policy(ph_origin *origin);
ph_control_policy ph_origin_broadcast_control_policy(ph_origin *origin);

#endif
