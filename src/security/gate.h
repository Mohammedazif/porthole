#ifndef PORTHOLE_SECURITY_GATE_H
#define PORTHOLE_SECURITY_GATE_H

/* Application allow-list. Separate from AirPlay pairing. Unknown devices need a
 * 4-digit PIN unless open-LAN is on. Five wrong PINs reject that id until reset. */

#include <stdbool.h>

typedef struct ph_gate ph_gate;

typedef enum ph_gate_decision {
    PH_GATE_ADMIT = 0,
    PH_GATE_PIN,
    PH_GATE_REJECT
} ph_gate_decision;

ph_gate *ph_gate_create(void);
void ph_gate_destroy(ph_gate *gate);

void ph_gate_set_rng(ph_gate *gate, unsigned (*rng)(void *rng_user), void *user);
void ph_gate_set_open(ph_gate *gate, bool open_lan);

ph_gate_decision ph_gate_decide(ph_gate *gate, const char *device_id, const char *name,
                                const char *model);

const char *ph_gate_pending_pin(const ph_gate *gate);
const char *ph_gate_pending_id(const ph_gate *gate);

bool ph_gate_submit_pin(ph_gate *gate, const char *device_id, const char *pin);
void ph_gate_reset_failures(ph_gate *gate, const char *device_id);

/* Lines are "device_id<TAB>name". Ids and names are at most 128 bytes. */
bool ph_gate_save(ph_gate *gate, const char *path);
bool ph_gate_load(ph_gate *gate, const char *path);

#endif
