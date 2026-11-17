#include "usb/packet.h"

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

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void w64(uint8_t *p, uint64_t v)
{
    w32(p, (uint32_t)v);
    w32(p + 4, (uint32_t)(v >> 32));
}

static const uint8_t ping_fixture[16] = {
    0x10, 0x00, 0x00, 0x00, 0x67, 0x6e, 0x69, 0x70, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static const uint8_t afmt_dict[42] = {
    0x2A, 0x00, 0x00, 0x00, 0x74, 0x63, 0x69, 0x64, 0x22, 0x00, 0x00, 0x00, 0x76, 0x79,
    0x65, 0x6B, 0x0D, 0x00, 0x00, 0x00, 0x6B, 0x72, 0x74, 0x73, 0x45, 0x72, 0x72, 0x6F,
    0x72, 0x0D, 0x00, 0x00, 0x00, 0x76, 0x62, 0x6D, 0x6E, 0x03, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t skew_bits[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xe7, 0x40};

static size_t build_feed(uint8_t *dst, size_t cap, const uint8_t *media, size_t media_len,
                         size_t *media_off)
{
    size_t sdat = 8 + media_len;
    size_t sbuf = 8 + 8 + sdat;
    size_t total = 20 + sbuf;
    if (cap < total)
        return 0;
    w32(dst, (uint32_t)total);
    w32(dst + 4, PH_USB_ASYN);
    w64(dst + 8, 0x0000000113538DA0ULL);
    w32(dst + 16, PH_USB_FEED);
    w32(dst + 20, (uint32_t)sbuf);
    w32(dst + 24, PH_USB_SBUF);
    w32(dst + 28, 8);
    w32(dst + 32, PH_USB_FCC('o', 'p', 't', 's'));
    w32(dst + 36, (uint32_t)sdat);
    w32(dst + 40, PH_USB_SDAT);
    memcpy(dst + 44, media, media_len);
    *media_off = 44;
    return total;
}

static void test_ping(void)
{
    ph_usb_frame frame;
    uint8_t built[16];
    uint8_t tiny[15];

    CHECK(ph_usb_frame_parse(ping_fixture, sizeof ping_fixture, &frame));
    CHECK(frame.kind == PH_USB_FRAME_PING);
    CHECK(frame.reply_required);
    CHECK(frame.subtype == 0);
    CHECK(frame.clock_ref == 0);
    CHECK(frame.sdat == NULL);
    CHECK(frame.sdat_len == 0);
    CHECK(ph_usb_build_ping(built, sizeof built) == 16);
    CHECK(memcmp(built, ping_fixture, 16) == 0);
    CHECK(ph_usb_frame_parse(built, 16, &frame));
    CHECK(frame.kind == PH_USB_FRAME_PING);
    CHECK(ph_usb_build_ping(tiny, sizeof tiny) == 0);
    CHECK(ph_usb_build_ping(NULL, 16) == 0);
    CHECK(!ph_usb_frame_parse(ping_fixture, 15, &frame));
    CHECK(!ph_usb_frame_parse(ping_fixture, 17, &frame));
    CHECK(!ph_usb_frame_parse(NULL, 16, &frame));
    CHECK(!ph_usb_frame_parse(ping_fixture, 16, NULL));
}

static void test_feed_sdat(void)
{
    uint8_t media[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
    uint8_t frame_bytes[128];
    size_t media_off = 0;
    size_t n;
    ph_usb_frame frame;

    n = build_feed(frame_bytes, sizeof frame_bytes, media, sizeof media, &media_off);
    CHECK(n > 0);
    CHECK(ph_usb_frame_parse(frame_bytes, n, &frame));
    CHECK(frame.kind == PH_USB_FRAME_ASYN);
    CHECK(frame.subtype == PH_USB_FEED);
    CHECK(!frame.reply_required);
    CHECK(frame.clock_ref == 0x0000000113538DA0ULL);
    CHECK(frame.sdat == frame_bytes + media_off);
    CHECK(frame.sdat_len == sizeof media);
    CHECK(memcmp(frame.sdat, media, sizeof media) == 0);
}

static void test_need(void)
{
    uint8_t need[20];
    uint8_t expect[20] = {
        0x14, 0x00, 0x00, 0x00, 0x6e, 0x79, 0x73, 0x61, 0xa0, 0x8d,
        0x53, 0x13, 0x01, 0x00, 0x00, 0x00, 0x64, 0x65, 0x65, 0x6e,
    };
    ph_usb_frame frame;
    uint64_t clock = 0x0000000113538DA0ULL;

    CHECK(ph_usb_build_need(need, sizeof need, clock) == 20);
    CHECK(memcmp(need, expect, 20) == 0);
    CHECK(ph_usb_build_need(need, 19, clock) == 0);
    CHECK(ph_usb_frame_parse(need, 20, &frame));
    CHECK(frame.kind == PH_USB_FRAME_ASYN);
    CHECK(frame.subtype == PH_USB_NEED);
    CHECK(frame.clock_ref == clock);
    CHECK(!frame.reply_required);
    CHECK(frame.sdat == NULL);
}

static void test_sync_replies(void)
{
    struct {
        uint32_t subtype;
        size_t size;
    } rows[] = {
        {PH_USB_CWPA, 28}, {PH_USB_AFMT, 62}, {PH_USB_CVRP, 28}, {PH_USB_CLOK, 28},
        {PH_USB_TIME, 44}, {PH_USB_SKEW, 28}, {PH_USB_STOP, 24}, {PH_USB_OG, 24},
    };
    uint64_t correlation = 0x1122334455667788ULL;
    uint64_t clock = 0x00007FA66CE20CB0ULL;
    size_t i;

    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        uint8_t buf[128];
        ph_usb_frame frame;
        size_t n = ph_usb_build_sync_reply(buf, sizeof buf, rows[i].subtype, correlation, clock);
        CHECK(n == rows[i].size);
        CHECK(ph_usb_build_sync_reply(buf, n - 1, rows[i].subtype, correlation, clock) == 0);
        CHECK(ph_usb_frame_parse(buf, n, &frame));
        CHECK(frame.kind == PH_USB_FRAME_REPLY);
        CHECK(frame.correlation == correlation);
        CHECK(!frame.reply_required);
    }

    {
        uint8_t buf[64];
        uint8_t time_scale[4] = {0x00, 0xca, 0x9a, 0x3b};
        size_t n = ph_usb_build_sync_reply(buf, sizeof buf, PH_USB_AFMT, correlation, clock);
        CHECK(n == 62);
        CHECK(memcmp(buf + 20, afmt_dict, sizeof afmt_dict) == 0);
        n = ph_usb_build_sync_reply(buf, sizeof buf, PH_USB_SKEW, correlation, clock);
        CHECK(memcmp(buf + 20, skew_bits, sizeof skew_bits) == 0);
        n = ph_usb_build_sync_reply(buf, sizeof buf, PH_USB_TIME, correlation, 0x0102030405060708ULL);
        CHECK(n == 44);
        CHECK(memcmp(buf + 28, time_scale, 4) == 0);
        CHECK(buf[32] == 1 && buf[33] == 0 && buf[34] == 0 && buf[35] == 0);
        n = ph_usb_build_sync_reply(buf, sizeof buf, PH_USB_STOP, correlation, clock);
        CHECK(buf[16] == 0 && buf[17] == 0 && buf[18] == 0 && buf[19] == 0);
        CHECK(buf[20] == 0 && buf[21] == 0 && buf[22] == 0 && buf[23] == 0);
        n = ph_usb_build_sync_reply(buf, sizeof buf, PH_USB_CWPA, correlation, clock);
        CHECK(n == 28);
        CHECK(buf[16] == 0 && buf[20] == 0xb0 && buf[21] == 0x0c);
    }
    CHECK(ph_usb_build_sync_reply((uint8_t[8]){0}, 8, PH_USB_FEED, correlation, clock) == 0);
    CHECK(ph_usb_build_sync_reply(NULL, 64, PH_USB_CWPA, correlation, clock) == 0);
}

static void test_cvrp_clock(void)
{
    uint8_t raw[36];
    ph_usb_frame frame;
    uint64_t device = 0x0000000113538DA0ULL;
    uint64_t correlation = 0xaabbccddeeff0011ULL;

    w32(raw, 36);
    w32(raw + 4, PH_USB_SYNC);
    w64(raw + 8, 1);
    w32(raw + 16, PH_USB_CVRP);
    w64(raw + 20, correlation);
    w64(raw + 28, device);
    CHECK(ph_usb_frame_parse(raw, 36, &frame));
    CHECK(frame.kind == PH_USB_FRAME_SYNC);
    CHECK(frame.subtype == PH_USB_CVRP);
    CHECK(frame.reply_required);
    CHECK(frame.clock_ref == 1);
    CHECK(frame.correlation == correlation);
    CHECK(frame.has_device_clock);
    CHECK(frame.device_clock == device);
}

static void test_reasm(void)
{
    uint8_t media[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42};
    uint8_t frame_bytes[128];
    size_t media_off = 0;
    size_t n;
    ph_usb_reasm reasm;
    const uint8_t *got = NULL;
    size_t got_len = 0;
    ph_usb_frame frame;
    size_t off;

    n = build_feed(frame_bytes, sizeof frame_bytes, media, sizeof media, &media_off);
    ph_usb_reasm_init(&reasm);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 0);
    CHECK(ph_usb_reasm_push(&reasm, frame_bytes, 2) == 0);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 0);
    CHECK(ph_usb_reasm_push(&reasm, frame_bytes + 2, 5) == 0);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 0);
    CHECK(ph_usb_reasm_push(&reasm, frame_bytes + 7, n - 7) == 0);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 1);
    CHECK(got_len == n);
    CHECK(memcmp(got, frame_bytes, n) == 0);
    CHECK(ph_usb_frame_parse(got, got_len, &frame));
    CHECK(frame.sdat_len == sizeof media);
    CHECK(memcmp(frame.sdat, media, sizeof media) == 0);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 0);

    ph_usb_reasm_free(&reasm);
    ph_usb_reasm_init(&reasm);
    for (off = 0; off < sizeof ping_fixture; off++)
        CHECK(ph_usb_reasm_push(&reasm, ping_fixture + off, 1) == 0);
    CHECK(ph_usb_reasm_push(&reasm, ping_fixture, sizeof ping_fixture) == 0);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 1);
    CHECK(got_len == sizeof ping_fixture);
    CHECK(memcmp(got, ping_fixture, sizeof ping_fixture) == 0);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 1);
    CHECK(memcmp(got, ping_fixture, sizeof ping_fixture) == 0);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 0);
    ph_usb_reasm_free(&reasm);
    ph_usb_reasm_free(&reasm);
}

static void test_oversize(void)
{
    ph_usb_reasm reasm;
    const uint8_t *got = NULL;
    size_t got_len = 0;
    uint8_t bad[4] = {0x01, 0x00, 0x80, 0x00};
    uint8_t max_len[4] = {0x00, 0x00, 0x80, 0x00};
    uint8_t short_len[4] = {0x03, 0x00, 0x00, 0x00};
    uint8_t zero_len[4] = {0x00, 0x00, 0x00, 0x00};

    ph_usb_reasm_init(&reasm);
    CHECK(ph_usb_reasm_push(&reasm, bad, 2) == 0);
    CHECK(ph_usb_reasm_push(&reasm, bad + 2, 2) == -1);
    CHECK(reasm.failed);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == -1);
    CHECK(ph_usb_reasm_push(&reasm, ping_fixture, sizeof ping_fixture) == -1);
    ph_usb_reasm_free(&reasm);

    ph_usb_reasm_init(&reasm);
    CHECK(ph_usb_reasm_push(&reasm, bad, 4) == -1);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == -1);
    ph_usb_reasm_free(&reasm);

    ph_usb_reasm_init(&reasm);
    CHECK(ph_usb_reasm_push(&reasm, short_len, 4) == -1);
    ph_usb_reasm_free(&reasm);

    ph_usb_reasm_init(&reasm);
    CHECK(ph_usb_reasm_push(&reasm, zero_len, 4) == -1);
    ph_usb_reasm_free(&reasm);

    ph_usb_reasm_init(&reasm);
    CHECK(ph_usb_reasm_push(&reasm, max_len, 4) == 0);
    CHECK(!reasm.failed);
    CHECK(ph_usb_reasm_pop(&reasm, &got, &got_len) == 0);
    ph_usb_reasm_free(&reasm);
}

int main(void)
{
    test_ping();
    test_feed_sdat();
    test_need();
    test_sync_replies();
    test_cvrp_clock();
    test_reasm();
    test_oversize();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all usb packet checks passed\n");
    return 0;
}
