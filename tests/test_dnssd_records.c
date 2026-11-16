#include <stdio.h>
#include <string.h>

#include "airplay/dnssd_records.h"

static int failures, unregistered;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

/* TXT wire-format tests do not advertise services or require Windows. */
void dnssd_unregister_raop(dnssd_t *d) { d->raop_service = NULL; unregistered++; }
void dnssd_unregister_airplay(dnssd_t *d) { d->airplay_service = NULL; unregistered++; }

static bool has(const char *data, int length, const char *field)
{
    size_t pos = 0, n = strlen(field);
    while (pos < (size_t)length) {
        unsigned int size = (unsigned char)data[pos++];
        if (size > (size_t)length - pos)
            return false;
        if (size == n && !memcmp(data + pos, field, n))
            return true;
        pos += size;
    }
    return false;
}

int main(void)
{
    const unsigned char address[] = {0x52,0x5a,0xaa,0x4e,0x5b,0xdf};
    int error = -1, length = 0;
    dnssd_t *d = dnssd_init("Porthole", 8, (const char *)address, 6, &error, 0);
    CHECK(d && error == 0);
    if (!d)
        return 1;
    CHECK(!strcmp(dnssd_get_name(d, &length), "Porthole") && length == 8);
    CHECK(!memcmp(dnssd_get_hw_addr(d, &length), address, 6) && length == 6);
    char key[65];
    memset(key, 'a', 64);
    key[64] = 0;
    dnssd_set_pk(d, key);
    key[0] = 'b'; /* discovery keeps the key even if caller storage changes */
    for (int i = 0; i < 64; i++)
        dnssd_set_airplay_features(d, i, 0);
    dnssd_set_airplay_features(d, 31, 1);
    dnssd_set_airplay_features(d, 63, 1);
    dnssd_set_airplay_features(d, 64, 1);
    CHECK(dnssd_get_airplay_features(d) == UINT64_C(0x8000000080000000));
    const char *data = dnssd_get_airplay_txt(d, &length);
    CHECK(data && has(data, length, "deviceid=52:5a:aa:4e:5b:df"));
    CHECK(has(data, length, "features=0x80000000,0x80000000"));
    CHECK(has(data, length, "pw=false") && has(data, length, "flags=0x4"));
    CHECK(has(data, length, "model=AppleTV3,2") && has(data, length, "srcvers=220.68"));
    char field[68] = "pk=";
    memset(field + 3, 'a', 64);
    field[67] = 0;
    CHECK(has(data, length, field));
    data = dnssd_get_raop_txt(d, &length);
    CHECK(data && has(data, length, "ft=0x80000000,0x80000000"));
    CHECK(has(data, length, "cn=0,1,2,3") && has(data, length, "sr=44100"));
    CHECK(has(data, length, "ss=16") && has(data, length, "tp=UDP"));
    CHECK(has(data, length, field));
    dnssd_destroy(d);
    CHECK(unregistered == 2);
    d = dnssd_init("Phone", 5, (const char *)address, 6, &error, 1);
    CHECK(d);
    data = dnssd_get_airplay_txt(d, &length);
    CHECK(has(data, length, "pw=true"));
    dnssd_destroy(d);
    CHECK(!dnssd_init("Porthole", 8, (const char *)address, 5, &error, 0));
    CHECK(error == DNSSD_ERROR_HWADDRLEN);
    CHECK(!dnssd_init(NULL, 0, (const char *)address, 6, &error, 0));
    dnssd_destroy(NULL);
    if (failures)
        return 1;
    puts("native discovery TXT identities, feature bits, keys and formats passed");
    return 0;
}
