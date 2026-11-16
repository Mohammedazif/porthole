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

/* Closes the helper's input (it then cleans up and exits) and waits for it. */
void ph_hid_link_stop(ph_hid_link *l);

#endif
