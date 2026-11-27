#ifndef PORTHOLE_UI_TRANSFER_CARD_H
#define PORTHOLE_UI_TRANSFER_CARD_H

/* The receipt that slides in when a file arrives from the iPhone. */

#include <stdbool.h>

#include <SDL.h>

typedef struct ph_transfer_card ph_transfer_card;

ph_transfer_card *ph_transfer_card_create(void);
void ph_transfer_card_destroy(ph_transfer_card *c);
Uint32 ph_transfer_card_id(const ph_transfer_card *c);
void ph_transfer_card_set_theme(ph_transfer_card *c, bool dark);

/* path is NULL while a share is still arriving. done/total are that batch.
 * can_show adds the button that opens the folder the file landed in. */
void ph_transfer_card_show(ph_transfer_card *c, const char *path, int done, int total,
                           bool can_show, Uint64 now);
/* A batch from the iPhone, live: the item arriving now (name, bytes of size),
 * with done of total items already saved. */
void ph_transfer_card_receiving(ph_transfer_card *c, const char *name, unsigned long long bytes,
                                unsigned long long size, int done, int total, Uint64 now);
/* One item of the batch saved at path. The card sums the batch up when the
 * last of `total` arrives: one file to open, or several in a list. */
void ph_transfer_card_saved(ph_transfer_card *c, const char *path, int total, bool can_show, Uint64 now);
/* Folder name under the file. NULL clears it. */
void ph_transfer_card_place(ph_transfer_card *c, const char *place);
void ph_transfer_card_dismiss(ph_transfer_card *c, Uint64 now);
void ph_transfer_card_tick(ph_transfer_card *c, Uint64 now);
bool ph_transfer_card_animating(const ph_transfer_card *c);

/* True when the event belongs to the card. */
bool ph_transfer_card_event(ph_transfer_card *c, const SDL_Event *event, Uint64 now);

#endif
