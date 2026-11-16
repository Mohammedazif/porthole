#ifndef PORTHOLE_HID_LINK_H
#define PORTHOLE_HID_LINK_H

/* Runs a Bluetooth helper process and sends it HID reports (see link_proto.h). */

#include <stdbool.h>
#include <stdint.h>

#include "hid/hid_reports.h"

typedef struct ph_hid_link ph_hid_link;

/* Called on a background thread for every status line from the helper, and with "exited"
 * when the helper stops. */
typedef void (*ph_hid_link_status_fn)(void *user, const char *line);

/* Starts the helper (`argv[0]` is looked up in PATH). NULL on failure (logged). */
ph_hid_link *ph_hid_link_start(char *const argv[], ph_hid_link_status_fn on_status, void *user);

/* Queue reports. The helper sends mouse reports, sync and delay one at a time at its
 * fixed interval; keyboard and consumer reports use a second lane of their own, so a
 * key press never waits behind queued pointer movement. Order is kept within a lane,
 * not between the two. These return false if the helper is gone. Main thread only. */
bool ph_hid_link_keyboard(ph_hid_link *l, const ph_hid_keyboard *k);
bool ph_hid_link_mouse(ph_hid_link *l, const ph_hid_mouse *m);
bool ph_hid_link_consumer(ph_hid_link *l, uint16_t usage);
bool ph_hid_link_sync(ph_hid_link *l, uint32_t id);
bool ph_hid_link_delay(ph_hid_link *l, uint16_t milliseconds);

/* Main-thread lifecycle: request_stop closes stdin once and rejects new reports.
 * Poll regularly to reap without blocking, escalating EOF -> TERM -> KILL with
 * bounded grace periods. Poll returns true once the child is reaped; it does not
 * free the link. The reader emits "exited" after reaping, then stop can free it. */
void ph_hid_link_request_stop(ph_hid_link *l);
bool ph_hid_link_poll(ph_hid_link *l);

/* Final shutdown: drives the same bounded stop sequence, joins the reader and
 * frees the link. During normal rendering use request_stop/poll instead. */
void ph_hid_link_stop(ph_hid_link *l);

#endif
