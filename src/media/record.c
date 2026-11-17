#include "media/record.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include "core/log.h"

/* 90 kHz keeps millisecond PTS and 60 fps frame steps exact. */
#define TICKS_PER_SEC 90000
#define TICKS_PER_FRAME 1500

struct ph_record {
    AVFormatContext *fmt;
    AVStream *stream;
    uint8_t *sps;
    uint8_t *pps;
    size_t sps_len;
    size_t pps_len;
    int width;
    int height;
    bool header_written;
    bool origin_set;
    uint64_t origin_ns;
    uint64_t synth;
    bool have_raw;
    int64_t last_raw;
    bool have_out;
    int64_t last_out;
    int64_t last_duration;
    uint8_t *pending;
    size_t pending_len;
    uint64_t pending_pts;
    bool pending_key;
    bool have_pending;
};

struct buf {
    uint8_t *data;
    size_t len;
    size_t cap;
};

struct bits {
    const uint8_t *data;
    size_t size;
    size_t pos;
};

static void log_av(const char *what, int err)
{
    char text[AV_ERROR_MAX_STRING_SIZE];

    av_make_error_string(text, sizeof text, err);
    PH_LOGW("%s: %s", what, text);
}

static void buf_free(struct buf *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static int buf_append(struct buf *buffer, const void *src, size_t n)
{
    uint8_t *grown;
    size_t cap;

    if (n > SIZE_MAX - buffer->len)
        return -1;
    if (buffer->len + n > buffer->cap) {
        cap = buffer->cap ? buffer->cap : 4096;
        while (cap < buffer->len + n) {
            if (cap > SIZE_MAX / 2)
                return -1;
            cap *= 2;
        }
        grown = realloc(buffer->data, cap);
        if (!grown)
            return -1;
        buffer->data = grown;
        buffer->cap = cap;
    }
    memcpy(buffer->data + buffer->len, src, n);
    buffer->len += n;
    return 0;
}

static int append_avcc_nal(struct buf *buffer, const uint8_t *nal, size_t len)
{
    uint8_t hdr[4];

    if (!len || len > UINT32_MAX)
        return -1;
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    if (buf_append(buffer, hdr, 4) || buf_append(buffer, nal, len))
        return -1;
    return 0;
}

static int save_param(uint8_t **dst, size_t *dst_len, const uint8_t *nal, size_t len)
{
    uint8_t *copy;

    if (!len || len > 4096)
        return -1;
    copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, nal, len);
    free(*dst);
    *dst = copy;
    *dst_len = len;
    return 0;
}

static size_t find_start(const uint8_t *data, size_t len, size_t from, size_t *sc_len)
{
    size_t i;

    *sc_len = 0;
    for (i = from; i + 3 <= len; i++) {
        if (data[i] != 0 || data[i + 1] != 0)
            continue;
        if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
            *sc_len = 4;
            return i;
        }
        if (data[i + 2] == 1) {
            *sc_len = 3;
            return i;
        }
    }
    return len;
}

static int bits_u(struct bits *reader, int width, uint32_t *out)
{
    uint32_t value = 0;
    int i;

    if (width < 0 || width > 32 || reader->pos + (size_t)width > reader->size * 8)
        return -1;
    for (i = 0; i < width; i++) {
        size_t at = reader->pos++;
        unsigned bit = (unsigned)((reader->data[at >> 3] >> (7 - (at & 7))) & 1);
        value = (value << 1) | bit;
    }
    *out = value;
    return 0;
}

static int bits_ue(struct bits *reader, uint32_t *out)
{
    int zeros = 0;
    uint32_t bit = 0;
    uint32_t rest = 0;

    while (zeros < 31) {
        if (bits_u(reader, 1, &bit))
            return -1;
        if (bit)
            break;
        zeros++;
    }
    if (!bit)
        return -1;
    if (zeros && bits_u(reader, zeros, &rest))
        return -1;
    *out = ((1u << zeros) - 1u) + rest;
    return 0;
}

static int bits_se(struct bits *reader, int32_t *out)
{
    uint32_t code;

    if (bits_ue(reader, &code))
        return -1;
    if (code & 1u)
        *out = (int32_t)((code >> 1) + 1u);
    else
        *out = -(int32_t)(code >> 1);
    return 0;
}

static int skip_scaling_list(struct bits *reader, int size)
{
    int last = 8;
    int next = 8;
    int i;

    for (i = 0; i < size; i++) {
        if (next != 0) {
            int32_t delta;
            if (bits_se(reader, &delta))
                return -1;
            next = (last + delta + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
    return 0;
}

static bool high_profile(uint32_t profile)
{
    switch (profile) {
    case 100: case 110: case 122: case 244:
    case 44: case 83: case 86: case 118:
    case 128: case 138: case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

static uint8_t *rbsp_payload(const uint8_t *nal, size_t len, size_t *out_len)
{
    uint8_t *buf;
    size_t i, j = 0;
    int zeros = 0;

    if (len < 2)
        return NULL;
    buf = malloc(len);
    if (!buf)
        return NULL;
    for (i = 1; i < len; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        buf[j++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    *out_len = j;
    return buf;
}

static int sps_dimensions(const uint8_t *nal, size_t len, int *width, int *height)
{
    uint8_t *rbsp;
    size_t rbsp_len = 0;
    struct bits reader;
    uint32_t profile = 0, chroma = 1, separate = 0, tmp = 0;
    uint32_t frame_mbs_only = 1, cropping = 0, w_mbs = 0, h_units = 0;
    int32_t signed_tmp = 0;
    int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    int width_px, height_px, unit_x, unit_y;
    int rc = -1;

    rbsp = rbsp_payload(nal, len, &rbsp_len);
    if (!rbsp || rbsp_len < 3)
        goto done;
    reader.data = rbsp;
    reader.size = rbsp_len;
    reader.pos = 0;
    if (bits_u(&reader, 8, &profile) || bits_u(&reader, 8, &tmp) || bits_u(&reader, 8, &tmp)
        || bits_ue(&reader, &tmp))
        goto done;
    if (high_profile(profile)) {
        if (bits_ue(&reader, &chroma))
            goto done;
        if (chroma == 3 && bits_u(&reader, 1, &separate))
            goto done;
        if (bits_ue(&reader, &tmp) || bits_ue(&reader, &tmp) || bits_u(&reader, 1, &tmp)
            || bits_u(&reader, 1, &tmp))
            goto done;
        if (tmp) {
            int count = chroma != 3 ? 8 : 12;
            int i;
            for (i = 0; i < count; i++) {
                uint32_t present;
                if (bits_u(&reader, 1, &present))
                    goto done;
                if (present && skip_scaling_list(&reader, i < 6 ? 16 : 64))
                    goto done;
            }
        }
    }
    if (bits_ue(&reader, &tmp) || bits_ue(&reader, &tmp))
        goto done;
    if (tmp == 0) {
        if (bits_ue(&reader, &tmp))
            goto done;
    } else if (tmp == 1) {
        uint32_t cycle, i;
        if (bits_u(&reader, 1, &tmp) || bits_se(&reader, &signed_tmp) || bits_se(&reader, &signed_tmp)
            || bits_ue(&reader, &cycle))
            goto done;
        if (cycle > 255)
            goto done;
        for (i = 0; i < cycle; i++) {
            if (bits_se(&reader, &signed_tmp))
                goto done;
        }
    } else if (tmp > 2) {
        goto done;
    }
    if (bits_ue(&reader, &tmp) || bits_u(&reader, 1, &tmp) || bits_ue(&reader, &w_mbs)
        || bits_ue(&reader, &h_units) || bits_u(&reader, 1, &frame_mbs_only))
        goto done;
    if (!frame_mbs_only && bits_u(&reader, 1, &tmp))
        goto done;
    if (bits_u(&reader, 1, &tmp) || bits_u(&reader, 1, &cropping))
        goto done;
    if (cropping) {
        uint32_t side;
        if (bits_ue(&reader, &side))
            goto done;
        crop_l = (int)side;
        if (bits_ue(&reader, &side))
            goto done;
        crop_r = (int)side;
        if (bits_ue(&reader, &side))
            goto done;
        crop_t = (int)side;
        if (bits_ue(&reader, &side))
            goto done;
        crop_b = (int)side;
    }
    if (separate)
        chroma = 0;
    if (!high_profile(profile))
        chroma = 1;
    width_px = (int)(w_mbs + 1) * 16;
    height_px = (int)(h_units + 1) * 16 * (frame_mbs_only ? 1 : 2);
    if (chroma == 0) {
        unit_x = 1;
        unit_y = frame_mbs_only ? 1 : 2;
    } else {
        int sub_w = (chroma == 1 || chroma == 2) ? 2 : 1;
        int sub_h = chroma == 1 ? 2 : 1;
        unit_x = sub_w;
        unit_y = sub_h * (frame_mbs_only ? 1 : 2);
    }
    if (cropping) {
        width_px -= (crop_l + crop_r) * unit_x;
        height_px -= (crop_t + crop_b) * unit_y;
    }
    if (width_px <= 0 || height_px <= 0 || width_px > 16384 || height_px > 16384)
        goto done;
    *width = width_px;
    *height = height_px;
    rc = 0;
done:
    free(rbsp);
    return rc;
}

static int64_t raw_ticks(ph_record *rec, uint64_t pts_ns, int64_t *duration)
{
    int64_t ticks;

    if (pts_ns == 0) {
        ticks = (int64_t)rec->synth * TICKS_PER_FRAME;
        rec->synth++;
        *duration = TICKS_PER_FRAME;
    } else {
        uint64_t delta;
        if (!rec->origin_set) {
            rec->origin_set = true;
            rec->origin_ns = pts_ns;
        }
        delta = pts_ns >= rec->origin_ns ? pts_ns - rec->origin_ns : 0;
        ticks = (int64_t)((delta / 1000000ULL) * 90ULL
                          + ((delta % 1000000ULL) * 90ULL) / 1000000ULL);
        if (rec->have_raw && ticks > rec->last_raw)
            *duration = ticks - rec->last_raw;
        else
            *duration = TICKS_PER_FRAME;
    }
    if (rec->have_raw && ticks <= rec->last_raw)
        ticks = rec->last_raw + 1;
    rec->last_raw = ticks;
    rec->have_raw = true;
    return ticks;
}

static int write_access_unit(ph_record *rec, const uint8_t *avcc, size_t len, uint64_t pts_ns,
                             bool key)
{
    AVPacket *pkt;
    AVRational src = {1, TICKS_PER_SEC};
    int64_t raw_duration = TICKS_PER_FRAME;
    int64_t ticks;
    int err;

    if (!rec->header_written || !avcc || !len || len > INT_MAX)
        return -1;
    ticks = raw_ticks(rec, pts_ns, &raw_duration);
    pkt = av_packet_alloc();
    if (!pkt)
        return -1;
    if (av_new_packet(pkt, (int)len) < 0) {
        av_packet_free(&pkt);
        return -1;
    }
    memcpy(pkt->data, avcc, len);
    pkt->stream_index = rec->stream->index;
    pkt->pts = av_rescale_q(ticks, src, rec->stream->time_base);
    pkt->duration = av_rescale_q(raw_duration, src, rec->stream->time_base);
    if (rec->have_out && pkt->pts <= rec->last_out)
        pkt->pts = rec->last_out + 1;
    if (pkt->duration <= 0)
        pkt->duration = 1;
    pkt->dts = pkt->pts;
    rec->last_out = pkt->pts;
    rec->last_duration = pkt->duration;
    rec->have_out = true;
    if (key)
        pkt->flags |= AV_PKT_FLAG_KEY;
    err = av_interleaved_write_frame(rec->fmt, pkt);
    av_packet_free(&pkt);
    if (err < 0) {
        log_av("couldn't write an mp4 video packet", err);
        return -1;
    }
    return 0;
}

static int write_header(ph_record *rec)
{
    uint8_t *extra;
    size_t extra_len, o = 0;
    int err;

    if (rec->header_written)
        return 0;
    if (!rec->sps || !rec->pps || rec->sps_len < 4 || !rec->pps_len)
        return -1;
    if (sps_dimensions(rec->sps, rec->sps_len, &rec->width, &rec->height)) {
        PH_LOGW("couldn't read H.264 picture size from SPS");
        return -1;
    }
    extra_len = 11 + rec->sps_len + rec->pps_len;
    extra = av_mallocz(extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!extra)
        return -1;
    extra[o++] = 1;
    extra[o++] = rec->sps[1];
    extra[o++] = rec->sps[2];
    extra[o++] = rec->sps[3];
    extra[o++] = 0xFF;
    extra[o++] = 0xE1;
    extra[o++] = (uint8_t)(rec->sps_len >> 8);
    extra[o++] = (uint8_t)rec->sps_len;
    memcpy(extra + o, rec->sps, rec->sps_len);
    o += rec->sps_len;
    extra[o++] = 1;
    extra[o++] = (uint8_t)(rec->pps_len >> 8);
    extra[o++] = (uint8_t)rec->pps_len;
    memcpy(extra + o, rec->pps, rec->pps_len);
    o += rec->pps_len;
    av_freep(&rec->stream->codecpar->extradata);
    rec->stream->codecpar->extradata = extra;
    rec->stream->codecpar->extradata_size = (int)o;
    rec->stream->codecpar->width = rec->width;
    rec->stream->codecpar->height = rec->height;
    rec->stream->codecpar->profile = rec->sps[1];
    rec->stream->codecpar->level = rec->sps[3];
    err = avformat_write_header(rec->fmt, NULL);
    if (err < 0) {
        log_av("couldn't write the mp4 header", err);
        return -1;
    }
    rec->header_written = true;
    return 0;
}

static int stash(ph_record *rec, const uint8_t *avcc, size_t len, uint64_t pts_ns, bool key)
{
    uint8_t *copy = malloc(len);

    if (!copy)
        return -1;
    memcpy(copy, avcc, len);
    free(rec->pending);
    rec->pending = copy;
    rec->pending_len = len;
    rec->pending_pts = pts_ns;
    rec->pending_key = key;
    rec->have_pending = true;
    return 0;
}

static int flush_pending(ph_record *rec)
{
    int rc;

    if (!rec->have_pending)
        return 0;
    rc = write_access_unit(rec, rec->pending, rec->pending_len, rec->pending_pts, rec->pending_key);
    free(rec->pending);
    rec->pending = NULL;
    rec->pending_len = 0;
    rec->have_pending = false;
    return rc;
}

ph_record *ph_record_open(const char *path)
{
    ph_record *rec;
    int err;

    if (!path || !path[0])
        return NULL;
    rec = calloc(1, sizeof *rec);
    if (!rec)
        return NULL;
    err = avformat_alloc_output_context2(&rec->fmt, NULL, "mp4", path);
    if (err < 0 || !rec->fmt) {
        if (err < 0)
            log_av("couldn't create an mp4 muxer", err);
        free(rec);
        return NULL;
    }
    rec->stream = avformat_new_stream(rec->fmt, NULL);
    if (!rec->stream)
        goto fail;
    rec->stream->time_base = (AVRational){1, TICKS_PER_SEC};
    rec->stream->avg_frame_rate = (AVRational){60, 1};
    rec->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    rec->stream->codecpar->codec_id = AV_CODEC_ID_H264;
    rec->stream->codecpar->codec_tag = MKTAG('a', 'v', 'c', '1');
    rec->stream->codecpar->format = AV_PIX_FMT_YUV420P;
    if (!(rec->fmt->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&rec->fmt->pb, path, AVIO_FLAG_WRITE);
        if (err < 0) {
            log_av("couldn't open mp4 output", err);
            goto fail;
        }
    }
    return rec;
fail:
    if (rec->fmt) {
        if (rec->fmt->pb)
            avio_closep(&rec->fmt->pb);
        avformat_free_context(rec->fmt);
    }
    free(rec);
    return NULL;
}

int ph_record_video(ph_record *rec, const uint8_t *annexb, size_t len, uint64_t pts_ns,
                    bool keyframe)
{
    struct buf payload = {0};
    bool saw_nal = false;
    bool saw_vcl = false;
    bool idr = false;
    size_t sc = 0;
    size_t at;
    int rc = -1;

    if (!rec || !rec->fmt || !annexb || !len || len > INT_MAX)
        return -1;
    at = find_start(annexb, len, 0, &sc);
    while (at < len) {
        size_t next_sc = 0;
        size_t nal_off = at + sc;
        size_t next = find_start(annexb, len, nal_off, &next_sc);
        if (next > nal_off) {
            const uint8_t *nal = annexb + nal_off;
            size_t nal_len = next - nal_off;
            int type = nal[0] & 0x1f;
            saw_nal = true;
            if (type == 7) {
                if (save_param(&rec->sps, &rec->sps_len, nal, nal_len))
                    goto done;
            } else if (type == 8) {
                if (save_param(&rec->pps, &rec->pps_len, nal, nal_len))
                    goto done;
            } else if (type != 9 && type != 10 && type != 11 && type != 12) {
                if (type >= 1 && type <= 5)
                    saw_vcl = true;
                if (type == 5)
                    idr = true;
                if (append_avcc_nal(&payload, nal, nal_len))
                    goto done;
            }
        }
        if (next >= len)
            break;
        at = next;
        sc = next_sc;
    }
    if (!saw_vcl || !payload.len) {
        rc = saw_nal ? 0 : -1;
        goto done;
    }
    if (!rec->header_written) {
        if (!rec->sps || !rec->pps) {
            rc = stash(rec, payload.data, payload.len, pts_ns, keyframe || idr);
            goto done;
        }
        if (write_header(rec) || flush_pending(rec))
            goto done;
    }
    if (write_access_unit(rec, payload.data, payload.len, pts_ns, keyframe || idr))
        goto done;
    rc = 0;
done:
    buf_free(&payload);
    return rc;
}

int ph_record_close(ph_record *rec)
{
    int rc = 0;

    if (!rec)
        return 0;
    if (rec->fmt) {
        if (!rec->header_written && rec->sps && rec->pps && write_header(rec))
            rc = -1;
        if (rc == 0 && rec->header_written && flush_pending(rec))
            rc = -1;
        if (rec->header_written) {
            if (rec->have_out)
                rec->stream->duration = rec->last_out + rec->last_duration;
            if (av_write_trailer(rec->fmt) < 0)
                rc = -1;
        }
        if (rec->fmt->pb)
            avio_closep(&rec->fmt->pb);
        avformat_free_context(rec->fmt);
    }
    free(rec->sps);
    free(rec->pps);
    free(rec->pending);
    free(rec);
    return rc;
}
