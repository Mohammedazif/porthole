#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core/mdns_name.h"

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* The reply an iPhone named "Unknown" (host name taken, so "Unknown-2") sent
 * to the reverse query for 192.168.0.221, byte for byte. */
static const uint8_t iphone_reply[] = {
    0x00, 0x00, 0x84, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0x32, 0x32,
    0x31, 0x01, 0x30, 0x03, 0x31, 0x36, 0x38, 0x03, 0x31, 0x39, 0x32, 0x07, 0x69, 0x6e, 0x2d,
    0x61, 0x64, 0x64, 0x72, 0x04, 0x61, 0x72, 0x70, 0x61, 0x00, 0x00, 0x0c, 0x80, 0x01, 0xc0,
    0x0c, 0x00, 0x0c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x11, 0x09, 0x55, 0x6e, 0x6b,
    0x6e, 0x6f, 0x77, 0x6e, 0x2d, 0x32, 0x05, 0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x00};

static void test_query(void)
{
    uint8_t query[96];
    size_t length = ph_mdns_reverse_query("192.168.0.221", query, sizeof query);
    /* The question section of the phone's reply echoes the query exactly. */
    CHECK(length == 44);
    CHECK(!memcmp(query + 4, iphone_reply + 4, 2) && !memcmp(query + 12, iphone_reply + 12, length - 12));
    CHECK(ph_mdns_reverse_query("fe80::1", query, sizeof query) == 0);
    CHECK(ph_mdns_reverse_query("192.168.0.256", query, sizeof query) == 0);
    CHECK(ph_mdns_reverse_query("192.168.0.1", query, 20) == 0);
}

static void test_answer(void)
{
    char host[64];
    CHECK(ph_mdns_reverse_answer(iphone_reply, sizeof iphone_reply, host, sizeof host));
    CHECK(!strcmp(host, "Unknown-2.local"));
    /* Cut short anywhere, it is refused, never read past the end. */
    for (size_t n = 0; n < sizeof iphone_reply; n++)
        CHECK(!ph_mdns_reverse_answer(iphone_reply, n, host, sizeof host));
    CHECK(!ph_mdns_reverse_answer(iphone_reply, sizeof iphone_reply, host, 8));
    /* A query (not a response) is not an answer. */
    uint8_t query[96];
    size_t length = ph_mdns_reverse_query("192.168.0.221", query, sizeof query);
    CHECK(!ph_mdns_reverse_answer(query, length, host, sizeof host));
    /* A compression pointer that loops is refused. */
    uint8_t loop[sizeof iphone_reply];
    memcpy(loop, iphone_reply, sizeof loop);
    loop[56] = 0xc0, loop[57] = 56;
    CHECK(!ph_mdns_reverse_answer(loop, sizeof loop, host, sizeof host));
}

static void test_device_name(void)
{
    char name[64];
    CHECK(ph_mdns_device_name("Unknown-2.local", name, sizeof name) && !strcmp(name, "Unknown"));
    CHECK(ph_mdns_device_name("Unknown.local.", name, sizeof name) && !strcmp(name, "Unknown"));
    CHECK(ph_mdns_device_name("Johns-iPhone.local", name, sizeof name) && !strcmp(name, "Johns iPhone"));
    CHECK(ph_mdns_device_name("iPhone-15-Pro-3.local", name, sizeof name) && !strcmp(name, "iPhone 15 Pro"));
    CHECK(ph_mdns_device_name("Phone-2", name, sizeof name) && !strcmp(name, "Phone"));
    CHECK(!ph_mdns_device_name("-2.local", name, sizeof name) || strcmp(name, ""));
    CHECK(!ph_mdns_device_name(".local", name, sizeof name));
    CHECK(!ph_mdns_device_name("host.example.com", name, sizeof name));
    CHECK(!ph_mdns_device_name("Unknown.local", name, 4));
}

static void test_host_form(void)
{
    CHECK(ph_mdns_host_form_of("John's iPhone", "Johns iPhone"));
    CHECK(ph_mdns_host_form_of("Unknown", "Unknown"));
    CHECK(ph_mdns_host_form_of("iPhone 15 - Pro", "iPhone 15 Pro"));
    CHECK(!ph_mdns_host_form_of("Unknown", "Johns iPhone"));
    CHECK(!ph_mdns_host_form_of("John's iPhone", "John's iPhone")); /* a chosen name stays */
    CHECK(!ph_mdns_host_form_of("'", ""));
}

int main(void)
{
    test_query();
    test_answer();
    test_device_name();
    test_host_form();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
