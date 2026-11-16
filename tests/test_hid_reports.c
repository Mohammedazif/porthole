/* Checks the HID report descriptor is well-formed and agrees with the report
 * builders, so a descriptor mistake is caught here rather than on the iPhone. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hid/hid_reports.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct desc_info {
    unsigned input_bits[256];  /* per report ID */
    unsigned output_bits[256]; /* per report ID */
    int ok;
} desc_info;

/* Walks short items only (Porthole never uses long items). */
static void parse_descriptor(const uint8_t *d, size_t len, desc_info *info)
{
    size_t i = 0;
    uint32_t report_size = 0, report_count = 0, report_id = 0;
    int depth = 0;

    memset(info, 0, sizeof *info);
    info->ok = 1;

    while (i < len) {
        uint8_t prefix = d[i++];
        size_t size = prefix & 3u;
        unsigned type = (prefix >> 2) & 3u;
        unsigned tag = prefix >> 4;
        uint32_t value = 0;
        size_t k;

        if (size == 3)
            size = 4;
        if (prefix == 0xFE || i + size > len) {
            fprintf(stderr, "descriptor: bad item at offset %zu\n", i - 1);
            info->ok = 0;
            return;
        }
        for (k = 0; k < size; k++)
            value |= (uint32_t)d[i + k] << (8 * k);
        i += size;

        if (type == 0) { /* Main */
            if (tag == 0x8 || tag == 0x9) {
                if (report_id == 0) {
                    fprintf(stderr, "descriptor: field before any Report ID\n");
                    info->ok = 0;
                }
                if (tag == 0x8)
                    info->input_bits[report_id] += report_size * report_count;
                else
                    info->output_bits[report_id] += report_size * report_count;
            } else if (tag == 0xA) {
                depth++;
            } else if (tag == 0xC) {
                if (--depth < 0) {
                    fprintf(stderr, "descriptor: unbalanced End Collection\n");
                    info->ok = 0;
                }
            }
        } else if (type == 1) { /* Global */
            if (tag == 0x7)
                report_size = value;
            else if (tag == 0x9)
                report_count = value;
            else if (tag == 0x8)
                report_id = value & 0xFF;
        }
    }
    if (depth != 0) {
        fprintf(stderr, "descriptor: %d unclosed collection(s)\n", depth);
        info->ok = 0;
    }
}

static void test_descriptor_matches_report_lengths(void)
{
    desc_info info;

    parse_descriptor(ph_hid_report_descriptor, ph_hid_report_descriptor_len, &info);
    CHECK(info.ok);
    CHECK(info.input_bits[PH_HID_REPORT_ID_KEYBOARD] == PH_HID_KEYBOARD_REPORT_LEN * 8);
    CHECK(info.input_bits[PH_HID_REPORT_ID_MOUSE] == PH_HID_MOUSE_REPORT_LEN * 8);
    CHECK(info.input_bits[PH_HID_REPORT_ID_CONSUMER] == PH_HID_CONSUMER_REPORT_LEN * 8);
    CHECK(info.output_bits[PH_HID_REPORT_ID_KEYBOARD] == 8); /* LED byte */
}

static void test_mouse_report_layout(void)
{
    ph_hid_mouse m = {PH_HID_BTN_LEFT | PH_HID_BTN_4, -2, 300, -1, 5};
    uint8_t out[PH_HID_MOUSE_REPORT_LEN];
    const uint8_t expected[PH_HID_MOUSE_REPORT_LEN] = {0x09, 0xFE, 0xFF, 0x2C, 0x01, 0xFF, 0x05};

    ph_hid_build_mouse(&m, out);
    CHECK(memcmp(out, expected, sizeof out) == 0);
}

static void test_keyboard_report_layout(void)
{
    ph_hid_keyboard k = {PH_HID_MOD_LGUI, {0x04, 0, 0, 0, 0, 0}};
    uint8_t out[PH_HID_KEYBOARD_REPORT_LEN];
    const uint8_t expected[PH_HID_KEYBOARD_REPORT_LEN] = {0x08, 0x00, 0x04, 0, 0, 0, 0, 0};

    ph_hid_build_keyboard(&k, out);
    CHECK(memcmp(out, expected, sizeof out) == 0);
}

static void test_consumer_report_layout(void)
{
    uint8_t out[PH_HID_CONSUMER_REPORT_LEN];

    ph_hid_build_consumer(PH_HID_CONSUMER_AC_HOME, out);
    CHECK(out[0] == 0x23 && out[1] == 0x02);
}

static void check_key(char c, uint8_t usage, uint8_t mods)
{
    uint8_t u = 0xEE, m = 0xEE;

    if (!ph_hid_ascii_to_key(c, &u, &m) || u != usage || m != mods) {
        fprintf(stderr, "ascii '%c' (0x%02X): got usage 0x%02X mods 0x%02X, want 0x%02X 0x%02X\n",
                c, (unsigned char)c, u, m, usage, mods);
        g_failures++;
    }
}

static void test_ascii_to_key(void)
{
    uint8_t u, m;
    int c, typeable = 0;

    check_key('a', 0x04, 0);
    check_key('Z', 0x1D, PH_HID_MOD_LSHIFT);
    check_key('1', 0x1E, 0);
    check_key('0', 0x27, 0);
    check_key('!', 0x1E, PH_HID_MOD_LSHIFT);
    check_key(')', 0x27, PH_HID_MOD_LSHIFT);
    check_key('?', 0x38, PH_HID_MOD_LSHIFT);
    check_key('/', 0x38, 0);
    check_key('"', 0x34, PH_HID_MOD_LSHIFT);
    check_key(' ', PH_HID_KEY_SPACE, 0);
    check_key('\n', PH_HID_KEY_ENTER, 0);

    /* Every printable ASCII character must be typeable on a US layout. */
    for (c = 0x20; c < 0x7F; c++)
        typeable += ph_hid_ascii_to_key((char)c, &u, &m);
    CHECK(typeable == 0x7F - 0x20);

    CHECK(!ph_hid_ascii_to_key('\x01', &u, &m));
    CHECK(!ph_hid_ascii_to_key((char)0xE9, &u, &m)); /* non-ASCII byte */
}

int main(void)
{
    test_descriptor_matches_report_lengths();
    test_mouse_report_layout();
    test_keyboard_report_layout();
    test_consumer_report_layout();
    test_ascii_to_key();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all HID report checks passed\n");
    return 0;
}
