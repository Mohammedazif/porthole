#include "usb/sample.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static void test_annexb_passthrough(void)
{
    const uint8_t src[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
    uint8_t dst[16];
    size_t n = ph_usb_to_annexb(src, sizeof src, dst, sizeof dst);

    CHECK(n == sizeof src);
    CHECK(memcmp(dst, src, sizeof src) == 0);
    CHECK(ph_usb_annexb_keyframe(dst, n));
}

static void test_avcc_lengths(void)
{
    const uint8_t src[] = {0x00, 0x00, 0x00, 0x02, 0x67, 0x42, 0x00, 0x00, 0x00, 0x01, 0x68};
    const uint8_t expect[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x00, 0x01, 0x68};
    uint8_t dst[16];
    size_t n = ph_usb_to_annexb(src, sizeof src, dst, sizeof dst);

    CHECK(n == sizeof expect);
    CHECK(memcmp(dst, expect, sizeof expect) == 0);
    CHECK(ph_usb_annexb_keyframe(dst, n));
}

static void test_rejects_bad_lengths(void)
{
    const uint8_t src[] = {0x00, 0x00, 0x00, 0x05, 0x01, 0x02};
    uint8_t dst[16];
    const uint8_t slice[] = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a};

    CHECK(ph_usb_to_annexb(src, sizeof src, dst, sizeof dst) == 0);
    const uint8_t vps[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01};

    CHECK(ph_usb_to_annexb(NULL, 1, dst, sizeof dst) == 0);
    CHECK(!ph_usb_annexb_keyframe(slice, sizeof slice));
    CHECK(ph_usb_annexb_keyframe(vps, sizeof vps));
    CHECK(!ph_usb_annexb_keyframe(NULL, 0));
}

int main(void)
{
    test_annexb_passthrough();
    test_avcc_lengths();
    test_rejects_bad_lengths();
    if (g_failures) {
        fprintf(stderr, "%d USB sample checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
