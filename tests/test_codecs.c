/* Annex-B codec detection, advertised mirror size, and Apple model prefixes. */
#include <stdio.h>
#include <string.h>

#include "video/codecs.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static const uint8_t sc3[] = {0x00, 0x00, 0x01};
static const uint8_t sc4[] = {0x00, 0x00, 0x00, 0x01};

static void check_one(const uint8_t *start, size_t start_len, uint8_t nal, ph_codec expect)
{
    uint8_t buf[8];

    memcpy(buf, start, start_len);
    buf[start_len] = nal;
    CHECK(ph_codec_from_annexb(buf, start_len + 1) == expect);
}

int main(void)
{
    ph_codec_advertise def, four_k;
    static const uint8_t no_start[] = {0x00, 0x00, 0x02, 0x67, 0x42};
    static const uint8_t start_only[] = {0x00, 0x00, 0x00, 0x01};
    static const uint8_t h264_au[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x00, 0x01, 0x68,
        0xce, 0x00, 0x00, 0x01, 0x65, 0x88, 0x00, 0x00, 0x00, 0x01, 0x41, 0x9a,
    };
    static const uint8_t h265_au[] = {
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0x00, 0x00, 0x01, 0x26, 0x01,
    };
    static const uint8_t mixed[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x00, 0x00, 0x01, 0x40};
    unsigned i;
    static const uint8_t h264_nals[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x08, 0x65, 0x67, 0x68};
    static const uint8_t h265_nals[] = {0x40, 0x42, 0x44, 0x26, 0x28};

    CHECK(ph_codec_from_annexb(NULL, 0) == PH_CODEC_UNKNOWN);
    CHECK(ph_codec_from_annexb(no_start, sizeof no_start) == PH_CODEC_UNKNOWN);
    CHECK(ph_codec_from_annexb(start_only, sizeof start_only) == PH_CODEC_UNKNOWN);
    CHECK(ph_codec_from_annexb(sc3, sizeof sc3) == PH_CODEC_UNKNOWN);

    for (i = 0; i < sizeof h264_nals; i++) {
        check_one(sc3, sizeof sc3, h264_nals[i], PH_CODEC_H264);
        check_one(sc4, sizeof sc4, h264_nals[i], PH_CODEC_H264);
    }
    for (i = 0; i < sizeof h265_nals; i++) {
        check_one(sc3, sizeof sc3, h265_nals[i], PH_CODEC_H265);
        check_one(sc4, sizeof sc4, h265_nals[i], PH_CODEC_H265);
    }

    CHECK(ph_codec_from_annexb(h264_au, sizeof h264_au) == PH_CODEC_H264);
    CHECK(ph_codec_annexb_unambiguous(h264_au, sizeof h264_au, PH_CODEC_H264));
    CHECK(!ph_codec_annexb_unambiguous(h264_au, sizeof h264_au, PH_CODEC_H265));
    CHECK(ph_codec_from_annexb(h265_au, sizeof h265_au) == PH_CODEC_H265);
    CHECK(ph_codec_annexb_unambiguous(h265_au, sizeof h265_au, PH_CODEC_H265));
    CHECK(ph_codec_leading_nal(NULL, 0) == PH_CODEC_UNKNOWN);
    CHECK(ph_codec_leading_nal(no_start, sizeof no_start) == PH_CODEC_UNKNOWN);
    CHECK(ph_codec_leading_nal(h264_au, sizeof h264_au) == PH_CODEC_H264);
    CHECK(ph_codec_leading_nal(h265_au, sizeof h265_au) == PH_CODEC_H265);
    CHECK(ph_codec_leading_nal(mixed, sizeof mixed) == PH_CODEC_H264);
    CHECK(ph_codec_from_annexb(mixed, sizeof mixed) == PH_CODEC_UNKNOWN);
    CHECK(!ph_codec_annexb_unambiguous(mixed, sizeof mixed, PH_CODEC_H264));
    CHECK(!ph_codec_annexb_unambiguous(mixed, sizeof mixed, PH_CODEC_H265));

    def = ph_codec_advertise_for(false);
    four_k = ph_codec_advertise_for(true);
    CHECK(def.width == 1920 && def.height == 1080 && def.max_fps == 60);
    CHECK(four_k.width == 3840 && four_k.height == 2160 && four_k.max_fps == 30);

    CHECK(ph_device_kind_from_model(NULL) == PH_DEVICE_UNKNOWN);
    CHECK(ph_device_kind_from_model("") == PH_DEVICE_UNKNOWN);
    CHECK(ph_device_kind_from_model("iPod") == PH_DEVICE_UNKNOWN);
    CHECK(ph_device_kind_from_model("iPod touch") == PH_DEVICE_UNKNOWN);
    CHECK(ph_device_kind_from_model("ipad") == PH_DEVICE_UNKNOWN);
    CHECK(ph_device_kind_from_model("IPHONE") == PH_DEVICE_UNKNOWN);
    CHECK(ph_device_kind_from_model("iPad") == PH_DEVICE_IPAD);
    CHECK(ph_device_kind_from_model("iPad8,1") == PH_DEVICE_IPAD);
    CHECK(ph_device_kind_from_model("iPad Pro") == PH_DEVICE_IPAD);
    CHECK(ph_device_kind_from_model("iPhone") == PH_DEVICE_IPHONE);
    CHECK(ph_device_kind_from_model("iPhone14,2") == PH_DEVICE_IPHONE);
    CHECK(ph_device_kind_from_model("xiPad") == PH_DEVICE_UNKNOWN);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all codec checks passed\n");
    return 0;
}
