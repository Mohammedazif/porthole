#ifndef PORTHOLE_CORE_MDNS_NAME_H
#define PORTHOLE_CORE_MDNS_NAME_H

/* The iPhone's own name from the local network.
 *
 * iOS gives an app the phone's name only with an Apple-granted entitlement,
 * and over Bluetooth LE the phone calls itself "iPhone". But on Wi-Fi it
 * answers multicast DNS for its address with a host name made from its name
 * ("Unknown" -> "Unknown.local", "-2" added when another device took it).
 * One reverse (PTR) query to the phone's address, with the unicast-response
 * bit, returns it on Linux and Windows alike. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Builds the PTR query for an IPv4 address ("192.168.0.221"); returns its
 * length, or 0 if the address is not IPv4 or the buffer is too small. */
size_t ph_mdns_reverse_query(const char *ipv4, uint8_t *out, size_t cap);
/* Reads the first PTR answer's target ("Unknown-2.local") from a reply. */
bool ph_mdns_reverse_answer(const uint8_t *reply, size_t len, char *host, size_t cap);
/* "Unknown-2.local" -> "Unknown", "Johns-iPhone.local" -> "Johns iPhone".
 * iOS drops some characters (an apostrophe) making the host name, so this is
 * the name as near as the host name keeps it. False if nothing is left. */
bool ph_mdns_device_name(const char *host, char *out, size_t cap);
/* Asks the phone at `ipv4`, waiting up to `timeout_ms`. Blocks: call it off
 * the main thread. */
bool ph_mdns_lookup_device_name(const char *ipv4, char *out, size_t cap, int timeout_ms);

#endif
