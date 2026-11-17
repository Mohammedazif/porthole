#include "usb/sample.h"

#include <string.h>

static int start_code_len(const uint8_t *p, size_t n)
{
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
        return 4;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
        return 3;
    return 0;
}

size_t ph_usb_to_annexb(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    size_t off = 0;
    size_t out = 0;

    if (!src || !dst || len == 0)
        return 0;
    if (start_code_len(src, len)) {
        if (cap < len)
            return 0;
        memcpy(dst, src, len);
        return len;
    }
    while (off + 4 <= len) {
        uint32_t n = ((uint32_t)src[off] << 24) | ((uint32_t)src[off + 1] << 16) |
                     ((uint32_t)src[off + 2] << 8) | (uint32_t)src[off + 3];
        if (n == 0 || (size_t)n > len - off - 4)
            return 0;
        if (out + 4 + (size_t)n > cap)
            return 0;
        dst[out] = 0;
        dst[out + 1] = 0;
        dst[out + 2] = 0;
        dst[out + 3] = 1;
        memcpy(dst + out + 4, src + off + 4, (size_t)n);
        out += 4 + (size_t)n;
        off += 4 + (size_t)n;
    }
    if (off != len)
        return 0;
    return out;
}

static bool nal_is_key(uint8_t nal)
{
    unsigned avc = nal & 0x1Fu;
    unsigned hevc = (nal >> 1) & 0x3Fu;
    bool avc_known = (avc >= 1 && avc <= 5) || avc == 7 || avc == 8;

    if (avc == 5 || avc == 7)
        return true;
    if (hevc == 19 || hevc == 20)
        return true;
    /* VPS shares a first byte with some H.264 slices, so require a non-H.264 header. */
    return hevc == 32 && !avc_known;
}

bool ph_usb_annexb_keyframe(const uint8_t *data, size_t len)
{
    size_t i = 0;

    if (!data)
        return false;
    while (i + 3 < len) {
        int start = start_code_len(data + i, len - i);
        if (!start) {
            i++;
            continue;
        }
        if (i + (size_t)start < len && nal_is_key(data[i + (size_t)start]))
            return true;
        i += (size_t)start + 1;
    }
    return false;
}
