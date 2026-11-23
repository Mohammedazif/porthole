#ifndef PORTHOLE_FILES_PAIRED_PHONES_H
#define PORTHOLE_FILES_PAIRED_PHONES_H

#include <stdbool.h>

#define PH_PAIRED_PHONE_LIMIT 8
#define PH_REVOKED_PHONE_LIMIT 32

typedef struct ph_paired_phone {
    char key[33], id[65], seed[9], remote[64], alias[64];
} ph_paired_phone;

typedef struct ph_paired_phones {
    ph_paired_phone items[PH_PAIRED_PHONE_LIMIT];
    int count, selected;
    char revoked[PH_REVOKED_PHONE_LIMIT][65];
    int revoked_count;
} ph_paired_phones;

void ph_paired_phones_load(ph_paired_phones *phones);
bool ph_paired_phones_save(const ph_paired_phones *phones);
bool ph_paired_phone_identity(const char *key, char out[65]);
int ph_paired_phones_find(const ph_paired_phones *phones, const char *id);
bool ph_paired_phones_is_revoked(const ph_paired_phones *phones, const char *id);
void ph_paired_phones_revoke(ph_paired_phones *phones, const char *id);
const ph_paired_phone *ph_paired_phones_selected(const ph_paired_phones *phones);

#endif
