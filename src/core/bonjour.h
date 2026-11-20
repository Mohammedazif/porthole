#ifndef PORTHOLE_CORE_BONJOUR_H
#define PORTHOLE_CORE_BONJOUR_H

#include <stdint.h>

/* Advertises one TCP service on the LAN so the iPhone app can find this PC. Bonjour
 * comes from the Avahi compatibility library, loaded at run time the same way UxPlay
 * loads it, so the build needs no extra dependency. NULL when it is unavailable. */
typedef struct ph_bonjour ph_bonjour;

/* txt is a DNS-SD TXT record (length byte, then text, repeated). NULL advertises none. */
ph_bonjour *ph_bonjour_advertise(const char *name, const char *type, uint16_t port,
                                 const void *txt, uint16_t txt_len);
void ph_bonjour_stop(ph_bonjour *bonjour);

#endif
