#ifndef PORTHOLE_VIDEO_CODECS_H
#define PORTHOLE_VIDEO_CODECS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ph_codec {
    PH_CODEC_UNKNOWN = 0,
    PH_CODEC_H264,
    PH_CODEC_H265
} ph_codec;

/* Annex-B start codes 00 00 01 and 00 00 00 01. No start code, or both codecs, is UNKNOWN. */
ph_codec ph_codec_from_annexb(const uint8_t *data, size_t len);

/* True when a NAL is only `codec` and no NAL is only the other codec. */
bool ph_codec_annexb_unambiguous(const uint8_t *data, size_t len, ph_codec codec);

typedef struct ph_codec_advertise {
    unsigned short width, height, max_fps;
} ph_codec_advertise;

/* false: 1920x1080@60. true: 3840x2160@30. */
ph_codec_advertise ph_codec_advertise_for(bool four_k);

typedef enum ph_device_kind {
    PH_DEVICE_UNKNOWN = 0,
    PH_DEVICE_IPHONE,
    PH_DEVICE_IPAD
} ph_device_kind;

/* Case-sensitive prefix: "iPad" or "iPhone". NULL, "iPod", and anything else are unknown. */
ph_device_kind ph_device_kind_from_model(const char *model);

#endif
