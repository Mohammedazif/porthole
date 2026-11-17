#include "usb/packet.h"

#include <stdlib.h>
#include <string.h>

static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t r64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

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

static void find_sdat(const uint8_t *data, size_t off, size_t end, ph_usb_frame *out)
{
    while (off < end && end - off >= 8) {
        uint32_t clen = r32(data + off);
        if (clen < 8 || (size_t)clen > end - off)
            return;
        uint32_t magic = r32(data + off + 4);
        if (magic == PH_USB_SDAT) {
            out->sdat = data + off + 8;
            out->sdat_len = (size_t)clen - 8;
            return;
        }
        if (magic == PH_USB_SBUF) {
            find_sdat(data, off + 8, off + (size_t)clen, out);
            if (out->sdat)
                return;
        }
        off += (size_t)clen;
    }
}

bool ph_usb_frame_parse(const uint8_t *data, size_t len, ph_usb_frame *out)
{
    if (!data || !out || len < 4 || len > PH_USB_FRAME_MAX)
        return false;
    uint32_t declared = r32(data);
    if ((size_t)declared != len)
        return false;

    memset(out, 0, sizeof *out);
    if (len < 8)
        return true;

    uint32_t magic = r32(data + 4);
    if (magic == PH_USB_PING)
        out->kind = PH_USB_FRAME_PING;
    else if (magic == PH_USB_SYNC)
        out->kind = PH_USB_FRAME_SYNC;
    else if (magic == PH_USB_ASYN)
        out->kind = PH_USB_FRAME_ASYN;
    else if (magic == PH_USB_RPLY)
        out->kind = PH_USB_FRAME_REPLY;

    if ((out->kind == PH_USB_FRAME_SYNC || out->kind == PH_USB_FRAME_ASYN) && len >= 16)
        out->clock_ref = r64(data + 8);
    if ((out->kind == PH_USB_FRAME_SYNC || out->kind == PH_USB_FRAME_ASYN) && len >= 20)
        out->subtype = r32(data + 16);
    if (out->kind == PH_USB_FRAME_SYNC && len >= 28)
        out->correlation = r64(data + 20);
    if (out->kind == PH_USB_FRAME_REPLY && len >= 16)
        out->correlation = r64(data + 8);
    if (out->kind == PH_USB_FRAME_SYNC && len >= 36 &&
        (out->subtype == PH_USB_CWPA || out->subtype == PH_USB_CVRP)) {
        out->device_clock = r64(data + 28);
        out->has_device_clock = true;
    }
    out->reply_required =
        out->kind == PH_USB_FRAME_PING || out->kind == PH_USB_FRAME_SYNC;

    if (out->kind == PH_USB_FRAME_ASYN &&
        (out->subtype == PH_USB_FEED || out->subtype == PH_USB_EAT) && len > 20)
        find_sdat(data, 20, len, out);
    return true;
}

size_t ph_usb_build_ping(uint8_t *dst, size_t cap)
{
    if (!dst || cap < 16)
        return 0;
    w32(dst, 16);
    w32(dst + 4, PH_USB_PING);
    w64(dst + 8, 0x0000000100000000ULL);
    return 16;
}

size_t ph_usb_build_need(uint8_t *dst, size_t cap, uint64_t clock_ref)
{
    if (!dst || cap < 20)
        return 0;
    w32(dst, 20);
    w32(dst + 4, PH_USB_ASYN);
    w64(dst + 8, clock_ref);
    w32(dst + 16, PH_USB_NEED);
    return 20;
}

static size_t build_clock_reply(uint8_t *dst, size_t cap, uint64_t correlation, uint64_t clock)
{
    if (!dst || cap < 28)
        return 0;
    w32(dst, 28);
    w32(dst + 4, PH_USB_RPLY);
    w64(dst + 8, correlation);
    w32(dst + 16, 0);
    w64(dst + 20, clock);
    return 28;
}

static size_t build_afmt_reply(uint8_t *dst, size_t cap, uint64_t correlation)
{
    /* AFMT RPLY string-key dict {"Error": 0}. */
    static const uint8_t dict[] = {
        0x2A, 0x00, 0x00, 0x00, 0x74, 0x63, 0x69, 0x64, 0x22, 0x00, 0x00, 0x00, 0x76, 0x79,
        0x65, 0x6B, 0x0D, 0x00, 0x00, 0x00, 0x6B, 0x72, 0x74, 0x73, 0x45, 0x72, 0x72, 0x6F,
        0x72, 0x0D, 0x00, 0x00, 0x00, 0x76, 0x62, 0x6D, 0x6E, 0x03, 0x00, 0x00, 0x00, 0x00,
    };
    _Static_assert(sizeof dict == 42, "AFMT dict");
    if (!dst || cap < 62)
        return 0;
    w32(dst, 62);
    w32(dst + 4, PH_USB_RPLY);
    w64(dst + 8, correlation);
    w32(dst + 16, 0);
    memcpy(dst + 20, dict, sizeof dict);
    return 62;
}

static size_t build_time_reply(uint8_t *dst, size_t cap, uint64_t correlation, uint64_t value)
{
    if (!dst || cap < 44)
        return 0;
    w32(dst, 44);
    w32(dst + 4, PH_USB_RPLY);
    w64(dst + 8, correlation);
    w32(dst + 16, 0);
    w64(dst + 20, value);
    w32(dst + 28, 1000000000u);
    w32(dst + 32, 1); /* CMTime kCMTimeFlags_Valid */
    w64(dst + 36, 0);
    return 44;
}

static size_t build_skew_reply(uint8_t *dst, size_t cap, uint64_t correlation)
{
    double skew = 48000.0;
    uint64_t bits = 0;
    if (!dst || cap < 28)
        return 0;
    memcpy(&bits, &skew, sizeof bits);
    w32(dst, 28);
    w32(dst + 4, PH_USB_RPLY);
    w64(dst + 8, correlation);
    w32(dst + 16, 0);
    w64(dst + 20, bits);
    return 28;
}

static size_t build_stop_reply(uint8_t *dst, size_t cap, uint64_t correlation)
{
    if (!dst || cap < 24)
        return 0;
    w32(dst, 24);
    w32(dst + 4, PH_USB_RPLY);
    w64(dst + 8, correlation);
    w32(dst + 16, 0);
    w32(dst + 20, 0);
    return 24;
}

size_t ph_usb_build_sync_reply(uint8_t *dst, size_t cap, uint32_t subtype, uint64_t correlation,
                               uint64_t clock_ref)
{
    switch (subtype) {
    case PH_USB_CWPA:
    case PH_USB_CVRP:
    case PH_USB_CLOK:
        return build_clock_reply(dst, cap, correlation, clock_ref);
    case PH_USB_AFMT:
        return build_afmt_reply(dst, cap, correlation);
    case PH_USB_TIME:
        return build_time_reply(dst, cap, correlation, clock_ref);
    case PH_USB_SKEW:
        return build_skew_reply(dst, cap, correlation);
    case PH_USB_STOP:
    case PH_USB_OG:
        return build_stop_reply(dst, cap, correlation);
    default:
        return 0;
    }
}

void ph_usb_reasm_init(ph_usb_reasm *r)
{
    if (r)
        memset(r, 0, sizeof *r);
}

void ph_usb_reasm_free(ph_usb_reasm *r)
{
    if (!r)
        return;
    free(r->buf);
    memset(r, 0, sizeof *r);
}

static int reasm_reject_length(ph_usb_reasm *r)
{
    if (r->len - r->start < 4)
        return 0;
    uint32_t declared = r32(r->buf + r->start);
    if (declared < 4 || declared > PH_USB_FRAME_MAX) {
        r->failed = true;
        return -1;
    }
    return 0;
}

static int reasm_grow(ph_usb_reasm *r)
{
    size_t used = r->len - r->start;
    if (r->start > 0 && r->buf) {
        memmove(r->buf, r->buf + r->start, used);
        r->start = 0;
        r->len = used;
    }
    if (r->cap > r->len)
        return 0;
    if (r->cap >= PH_USB_FRAME_MAX)
        return -1;
    size_t want = r->cap == 0 ? (size_t)65536 : r->cap * 2;
    if (want < r->cap || want > PH_USB_FRAME_MAX)
        want = PH_USB_FRAME_MAX;
    uint8_t *grown = realloc(r->buf, want);
    if (!grown)
        return -1;
    r->buf = grown;
    r->cap = want;
    return 0;
}

int ph_usb_reasm_push(ph_usb_reasm *r, const uint8_t *data, size_t n)
{
    if (!r || r->failed)
        return -1;
    if (n == 0)
        return 0;
    if (!data)
        return -1;
    if (r->start == r->len) {
        r->start = 0;
        r->len = 0;
    }

    size_t off = 0;
    while (off < n) {
        if (reasm_reject_length(r) != 0)
            return -1;
        if (r->len == r->cap) {
            if (reasm_grow(r) != 0) {
                r->failed = true;
                return -1;
            }
        }
        size_t room = r->cap - r->len;
        size_t take = n - off;
        if (take > room)
            take = room;
        memcpy(r->buf + r->len, data + off, take);
        r->len += take;
        off += take;
        if (reasm_reject_length(r) != 0)
            return -1;
    }
    return 0;
}

int ph_usb_reasm_pop(ph_usb_reasm *r, const uint8_t **frame, size_t *frame_len)
{
    if (!r || !frame || !frame_len || r->failed)
        return -1;
    size_t avail = r->len - r->start;
    if (avail < 4)
        return 0;
    uint32_t declared = r32(r->buf + r->start);
    if (declared < 4 || declared > PH_USB_FRAME_MAX) {
        r->failed = true;
        return -1;
    }
    if (avail < declared)
        return 0;
    *frame = r->buf + r->start;
    *frame_len = declared;
    r->start += declared;
    return 1;
}
