/* TXT field definitions adapted from UxPlay/lib/dnssd.c.
 * Copyright (C) 2011-2012 Juho Vähä-Herttua; modified by fduncanh 2022.
 * LGPL-2.1-or-later, as in the vendored UxPlay core. */
#include "airplay/dnssd_records.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnssdint.h"

static bool txt(ph_dnssd_record *record, const char *key, const char *value)
{
    size_t k = strlen(key), v = strlen(value), length = k + 1 + v;
    if (length > 255 || record->len + length + 1 > sizeof record->data)
        return false;
    record->data[record->len++] = (unsigned char)length;
    memcpy(record->data + record->len, key, k);
    record->len += k;
    record->data[record->len++] = '=';
    memcpy(record->data + record->len, value, v);
    record->len += v;
    return true;
}

bool ph_dnssd_build_records(dnssd_t *d)
{
    if (d->raop_txt.len && d->airplay_txt.len)
        return true; /* immutable while the receiver serves concurrent requests */
    char features[32], address[18];
    snprintf(features, sizeof features, "0x%X,0x%X", (unsigned)(uint32_t)d->features,
             (unsigned)(uint32_t)(d->features >> 32));
    snprintf(address, sizeof address, "%02x:%02x:%02x:%02x:%02x:%02x", d->hw_addr[0],
             d->hw_addr[1], d->hw_addr[2], d->hw_addr[3], d->hw_addr[4], d->hw_addr[5]);
    d->raop_txt.len = d->airplay_txt.len = 0;
    ph_dnssd_record *r = &d->raop_txt, *a = &d->airplay_txt;
    const char *password = d->pin_pw ? "true" : "false";
    return txt(r, "ch", RAOP_CH) && txt(r, "cn", RAOP_CN) && txt(r, "da", RAOP_DA)
        && txt(r, "et", RAOP_ET) && txt(r, "vv", RAOP_VV) && txt(r, "ft", features)
        && txt(r, "am", GLOBAL_MODEL) && txt(r, "md", RAOP_MD) && txt(r, "rhd", RAOP_RHD)
        && txt(r, "pw", password) && txt(r, "sf", RAOP_SF) && txt(r, "sr", RAOP_SR)
        && txt(r, "ss", RAOP_SS) && txt(r, "sv", RAOP_SV) && txt(r, "tp", RAOP_TP)
        && txt(r, "txtvers", RAOP_TXTVERS) && txt(r, "vs", RAOP_VS)
        && txt(r, "vn", RAOP_VN) && txt(r, "pk", d->pk)
        && txt(a, "deviceid", address) && txt(a, "features", features)
        && txt(a, "pw", password) && txt(a, "flags", "0x4")
        && txt(a, "model", GLOBAL_MODEL) && txt(a, "pk", d->pk)
        && txt(a, "pi", AIRPLAY_PI) && txt(a, "srcvers", AIRPLAY_SRCVERS)
        && txt(a, "vv", AIRPLAY_VV);
}

dnssd_t *dnssd_init(const char *name, int name_len, const char *hw_addr, int hw_addr_len,
                    int *error, unsigned char pin_pw)
{
    if (error)
        *error = DNSSD_ERROR_NOERROR;
    if (!name || name_len <= 0 || name_len > 50 || !hw_addr || hw_addr_len != 6) {
        if (error)
            *error = hw_addr_len != 6 ? DNSSD_ERROR_HWADDRLEN : DNSSD_ERROR_BADFEATURES;
        return NULL;
    }
    dnssd_t *d = calloc(1, sizeof *d);
    if (d)
        d->name = calloc((size_t)name_len + 1, 1);
    if (!d || !d->name) {
        if (error)
            *error = DNSSD_ERROR_OUTOFMEM;
        free(d);
        return NULL;
    }
    memcpy(d->name, name, (size_t)name_len);
    memcpy(d->hw_addr, hw_addr, sizeof d->hw_addr);
    d->name_len = name_len;
    d->pin_pw = pin_pw;
    d->features = strtoull(FEATURES_1, NULL, 16)
                  | (strtoull(FEATURES_2, NULL, 16) << 32);
    return d;
}

void dnssd_destroy(dnssd_t *d)
{
    if (!d)
        return;
    dnssd_unregister_raop(d);
    dnssd_unregister_airplay(d);
    free(d->name);
    free(d);
}

const char *dnssd_get_raop_txt(dnssd_t *d, int *length)
{
    if (!ph_dnssd_build_records(d))
        return NULL;
    *length = (int)d->raop_txt.len;
    return (const char *)d->raop_txt.data;
}

const char *dnssd_get_airplay_txt(dnssd_t *d, int *length)
{
    if (!ph_dnssd_build_records(d))
        return NULL;
    *length = (int)d->airplay_txt.len;
    return (const char *)d->airplay_txt.data;
}

const char *dnssd_get_name(dnssd_t *d, int *length)
{
    *length = d->name_len;
    return d->name;
}

const char *dnssd_get_hw_addr(dnssd_t *d, int *length)
{
    *length = sizeof d->hw_addr;
    return (const char *)d->hw_addr;
}

void dnssd_set_pk(dnssd_t *d, char *pk)
{
    snprintf(d->pk, sizeof d->pk, "%s", pk ? pk : "");
    d->raop_txt.len = d->airplay_txt.len = 0;
}

void dnssd_set_airplay_features(dnssd_t *d, int bit, int val)
{
    if (bit < 0 || bit > 63 || (val != 0 && val != 1))
        return;
    uint64_t mask = UINT64_C(1) << bit;
    d->features = val ? d->features | mask : d->features & ~mask;
    d->raop_txt.len = d->airplay_txt.len = 0;
}

uint64_t dnssd_get_airplay_features(dnssd_t *d)
{
    return d->features;
}
