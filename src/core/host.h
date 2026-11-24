#ifndef PORTHOLE_CORE_HOST_H
#define PORTHOLE_CORE_HOST_H

#include <stddef.h>

/* This computer's network name without the domain. */
void ph_host_name(char *out, size_t len);

#endif
