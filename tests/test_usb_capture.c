#include "usb/capture.h"
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

static const uint8_t ping_fixture[16] = {
    0x10, 0x00, 0x00, 0x00, 0x67, 0x6e, 0x69, 0x70, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static const uint8_t h264_bytes[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
static const uint8_t audio_bytes[] = {0x11, 0x22, 0x33, 0x44};

typedef struct mem_io {
    const uint8_t *reads[8];
    int read_len[8];
    int read_count;
    int read_pos;
    uint8_t writes[8][160];
    int write_len[8];
    int write_count;
    int fail_writes;
} mem_io;

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

static size_t build_sample(uint8_t *dst, size_t cap, uint32_t subtype, const uint8_t *media,
                           size_t media_len)
{
    size_t sdat = 8 + media_len;
    size_t sbuf = 8 + sdat;
    size_t total = 20 + sbuf;
    if (cap < total)
        return 0;
    w32(dst, (uint32_t)total);
    w32(dst + 4, PH_USB_ASYN);
    w64(dst + 8, 0x1111);
    w32(dst + 16, subtype);
    w32(dst + 20, (uint32_t)sbuf);
    w32(dst + 24, PH_USB_SBUF);
    w32(dst + 28, (uint32_t)sdat);
    w32(dst + 32, PH_USB_SDAT);
    memcpy(dst + 36, media, media_len);
    return total;
}

static size_t build_cvrp(uint8_t *dst, size_t cap, uint64_t correlation, uint64_t device_clock)
{
    if (cap < 36)
        return 0;
    w32(dst, 36);
    w32(dst + 4, PH_USB_SYNC);
    w64(dst + 8, 1);
    w32(dst + 16, PH_USB_CVRP);
    w64(dst + 20, correlation);
    w64(dst + 28, device_clock);
    return 36;
}

static int mem_read(void *user, uint8_t *dst, int cap, int timeout_ms)
{
    mem_io *m = user;
    int n;
    (void)timeout_ms;
    if (m->read_pos >= m->read_count)
        return 0;
    n = m->read_len[m->read_pos];
    if (n < 0) {
        m->read_pos++;
        return -1;
    }
    if (n == 0) {
        m->read_pos++;
        return 0;
    }
    if (n > cap)
        return -1;
    memcpy(dst, m->reads[m->read_pos], (size_t)n);
    m->read_pos++;
    return n;
}

static int mem_write(void *user, const uint8_t *src, int len, int timeout_ms)
{
    mem_io *m = user;
    (void)timeout_ms;
    if (m->fail_writes)
        return -1;
    if (len < 0 || m->write_count >= 8 || len > (int)sizeof m->writes[0])
        return -1;
    memcpy(m->writes[m->write_count], src, (size_t)len);
    m->write_len[m->write_count] = len;
    m->write_count++;
    return len;
}

static ph_usb_transport transport_of(mem_io *m)
{
    ph_usb_transport io;
    io.read = mem_read;
    io.write = mem_write;
    io.user = m;
    return io;
}

static void queue(mem_io *m, const uint8_t *data, int len)
{
    m->reads[m->read_count] = data;
    m->read_len[m->read_count] = len;
    m->read_count++;
}

static void test_ping_then_feed(void)
{
    uint8_t feed[128];
    size_t feed_len;
    mem_io mem;
    ph_usb_transport io;
    ph_usb_capture *cap;
    ph_usb_sample sample;
    uint8_t copy[64];
    int rc;

    memset(&mem, 0, sizeof mem);
    feed_len = build_sample(feed, sizeof feed, PH_USB_FEED, h264_bytes, sizeof h264_bytes);
    CHECK(feed_len > 0);
    queue(&mem, ping_fixture, (int)sizeof ping_fixture);
    queue(&mem, feed, (int)feed_len);
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(cap != NULL);
    memset(&sample, 0, sizeof sample);
    rc = ph_usb_capture_poll(cap, &sample, copy, sizeof copy);
    CHECK(rc == 1);
    CHECK(sample.video);
    CHECK(sample.len == sizeof h264_bytes);
    CHECK(sample.data == copy);
    CHECK(memcmp(sample.data, h264_bytes, sizeof h264_bytes) == 0);
    CHECK(mem.write_count == 1);
    CHECK(mem.write_len[0] == (int)sizeof ping_fixture);
    CHECK(memcmp(mem.writes[0], ping_fixture, sizeof ping_fixture) == 0);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == 0);
    CHECK(mem.write_count == 1);
    ph_usb_capture_close(cap);
}

static void test_cvrp_need_and_eat(void)
{
    uint8_t cvrp[36];
    uint8_t feed[128];
    uint8_t eat[128];
    uint64_t device_clock = 0x0000000113538DA0ULL;
    uint64_t correlation = 0xaabbccddeeff0011ULL;
    mem_io mem;
    ph_usb_transport io;
    ph_usb_capture *cap;
    ph_usb_sample sample;
    uint8_t copy[64];
    int saw_need = 0;
    int i;

    memset(&mem, 0, sizeof mem);
    CHECK(build_cvrp(cvrp, sizeof cvrp, correlation, device_clock) == 36);
    CHECK(build_sample(feed, sizeof feed, PH_USB_FEED, h264_bytes, sizeof h264_bytes) > 0);
    CHECK(build_sample(eat, sizeof eat, PH_USB_EAT, audio_bytes, sizeof audio_bytes) > 0);
    queue(&mem, cvrp, 36);
    queue(&mem, feed, (int)build_sample(feed, sizeof feed, PH_USB_FEED, h264_bytes, sizeof h264_bytes));
    queue(&mem, eat, (int)build_sample(eat, sizeof eat, PH_USB_EAT, audio_bytes, sizeof audio_bytes));
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == 1);
    CHECK(sample.video);
    CHECK(memcmp(copy, h264_bytes, sizeof h264_bytes) == 0);
    for (i = 0; i < mem.write_count; i++) {
        if (mem.write_len[i] == 20 && mem.writes[i][4] == 0x6e && mem.writes[i][16] == 0x64 &&
            memcmp(mem.writes[i] + 8, cvrp + 28, 8) == 0)
            saw_need = 1;
    }
    CHECK(saw_need);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == 1);
    CHECK(!sample.video);
    CHECK(sample.len == sizeof audio_bytes);
    CHECK(memcmp(sample.data, audio_bytes, sizeof audio_bytes) == 0);
    ph_usb_capture_close(cap);
}

static void test_split_and_errors(void)
{
    uint8_t feed[128];
    size_t feed_len;
    mem_io mem;
    ph_usb_transport io;
    ph_usb_capture *cap;
    ph_usb_sample sample;
    uint8_t copy[64];
    uint8_t huge[4] = {0x01, 0x00, 0x80, 0x00};
    ph_usb_transport bad;

    feed_len = build_sample(feed, sizeof feed, PH_USB_FEED, h264_bytes, sizeof h264_bytes);
    memset(&mem, 0, sizeof mem);
    queue(&mem, feed, (int)(feed_len / 2));
    queue(&mem, NULL, 0);
    queue(&mem, feed + feed_len / 2, (int)(feed_len - feed_len / 2));
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == 0);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == 1);
    CHECK(sample.video);
    CHECK(memcmp(copy, h264_bytes, sizeof h264_bytes) == 0);
    ph_usb_capture_close(cap);

    memset(&mem, 0, sizeof mem);
    queue(&mem, huge, 4);
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == -1);
    ph_usb_capture_close(cap);

    memset(&mem, 0, sizeof mem);
    queue(&mem, ping_fixture, (int)sizeof ping_fixture);
    mem.fail_writes = 1;
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == -1);
    ph_usb_capture_close(cap);

    memset(&mem, 0, sizeof mem);
    queue(&mem, NULL, -1);
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == -1);
    ph_usb_capture_close(cap);

    memset(&mem, 0, sizeof mem);
    queue(&mem, feed, (int)feed_len);
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, 2) == -1);
    ph_usb_capture_close(cap);

    memset(&mem, 0, sizeof mem);
    io = transport_of(&mem);
    cap = ph_usb_capture_open(&io);
    CHECK(ph_usb_capture_poll(cap, &sample, copy, sizeof copy) == 0);
    ph_usb_capture_close(cap);
    ph_usb_capture_close(NULL);

    CHECK(ph_usb_capture_open(NULL) == NULL);
    memset(&bad, 0, sizeof bad);
    CHECK(ph_usb_capture_open(&bad) == NULL);
    CHECK(ph_usb_capture_poll(NULL, &sample, copy, sizeof copy) == -1);
}

int main(void)
{
    test_ping_then_feed();
    test_cvrp_need_and_eat();
    test_split_and_errors();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all usb capture checks passed\n");
    return 0;
}
