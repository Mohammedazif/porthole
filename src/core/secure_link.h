#ifndef PORTHOLE_CORE_SECURE_LINK_H
#define PORTHOLE_CORE_SECURE_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Each synchronous LAN listener owns one TLS connection on its worker thread.
 * Explorer's loopback-only Send To socket uses the ordinary socket API. */
const char *ph_tls_fingerprint(void);
bool ph_tls_accept(int socket);
void ph_tls_end(void);
int ph_tls_read(int socket, void *data, int length);
/* Complete a framed operation by this monotonic deadline, in milliseconds. */
int ph_tls_read_until(int socket, void *data, int length, uint64_t deadline);
int ph_tls_write(int socket, const void *data, int length);
bool ph_tls_authorized(void);
const char *ph_tls_ticket(void);
bool ph_tls_pending(int socket);
bool ph_tls_authorize(int socket, bool (*verify)(void *, const char *), void *user);
#endif
