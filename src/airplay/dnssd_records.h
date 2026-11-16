#ifndef PORTHOLE_DNSSD_RECORDS_H
#define PORTHOLE_DNSSD_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dnssd.h"

/* Private Windows discovery state. TXT fields follow the pinned UxPlay core;
 * keeping their wire encoding portable lets Linux CI check compatibility. */
typedef struct ph_dnssd_record {
    unsigned char data[2048];
    size_t len;
} ph_dnssd_record;

struct dnssd_s {
    char *name;
    int name_len;
    unsigned char hw_addr[6];
    char pk[65];
    uint64_t features;
    unsigned char pin_pw;
    ph_dnssd_record raop_txt, airplay_txt;
    void *raop_service, *airplay_service;
};

bool ph_dnssd_build_records(dnssd_t *d);

#endif
