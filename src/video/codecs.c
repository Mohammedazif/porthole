#include "video/codecs.h"

#include <string.h>

typedef struct ph_codec_votes {
    bool h264;
    bool h265;
    bool ambiguous;
} ph_codec_votes;

static bool h264_type(unsigned type)
{
    return (type >= 1 && type <= 5) || type == 7 || type == 8;
}

static bool h265_type(unsigned type)
{
    return type == 32 || type == 33 || type == 34 || type == 19 || type == 20;
}

static void consider_nal(uint8_t nal, ph_codec_votes *votes)
{
    unsigned avc = nal & 0x1Fu;
    unsigned hevc = (nal >> 1) & 0x3Fu;
    bool is_h264 = h264_type(avc);
    bool is_h265 = h265_type(hevc);

    /* A header that matches both tables is not a vote for both codecs. */
    if (is_h264 && is_h265)
        votes->ambiguous = true;
    else if (is_h264)
        votes->h264 = true;
    else if (is_h265)
        votes->h265 = true;
}

static ph_codec_votes scan_annexb(const uint8_t *data, size_t len)
{
    ph_codec_votes votes = {false, false, false};
    size_t i = 0;

    if (!data)
        return votes;
    while (i < len && !(votes.h264 && votes.h265)) {
        size_t remain = len - i;
        size_t start_len = 0;

        if (remain >= 3 && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            start_len = 3;
        else if (remain >= 4 && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0
                 && data[i + 3] == 1)
            start_len = 4;
        else {
            i++;
            continue;
        }
        if (remain <= start_len)
            break;
        consider_nal(data[i + start_len], &votes);
        i += start_len + 1;
    }
    return votes;
}

ph_codec ph_codec_from_annexb(const uint8_t *data, size_t len)
{
    ph_codec_votes votes = scan_annexb(data, len);

    if (votes.h264 && votes.h265)
        return PH_CODEC_UNKNOWN;
    if (votes.h264)
        return PH_CODEC_H264;
    if (votes.h265 || votes.ambiguous)
        return PH_CODEC_H265;
    return PH_CODEC_UNKNOWN;
}

bool ph_codec_annexb_unambiguous(const uint8_t *data, size_t len, ph_codec codec)
{
    ph_codec_votes votes = scan_annexb(data, len);

    if (codec == PH_CODEC_H264)
        return votes.h264 && !votes.h265;
    if (codec == PH_CODEC_H265)
        return votes.h265 && !votes.h264;
    return false;
}

ph_codec_advertise ph_codec_advertise_for(bool four_k)
{
    ph_codec_advertise out;

    if (four_k) {
        out.width = 3840;
        out.height = 2160;
        out.max_fps = 30;
    } else {
        out.width = 1920;
        out.height = 1080;
        out.max_fps = 60;
    }
    return out;
}

ph_device_kind ph_device_kind_from_model(const char *model)
{
    if (!model)
        return PH_DEVICE_UNKNOWN;
    if (strncmp(model, "iPad", 4) == 0)
        return PH_DEVICE_IPAD;
    if (strncmp(model, "iPhone", 6) == 0)
        return PH_DEVICE_IPHONE;
    return PH_DEVICE_UNKNOWN;
}
