/* Pictures on the Windows clipboard. Browsers and Office put a "PNG" format there;
 * screenshots and Paint put only a bitmap (CF_DIB), which is turned into PNG here.
 * A picture from the iPhone goes on as both, so any app can paste it. */
#include "files/winclip.h"

#ifdef _WIN32

#include "core/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static UINT png_format(void)
{
    static UINT format;
    if (!format)
        format = RegisterClipboardFormatW(L"PNG");
    return format;
}

static bool open_board(void)
{
    /* Another app may hold the clipboard for a moment. */
    for (int i = 0; i < 5; i++) {
        if (OpenClipboard(NULL))
            return true;
        Sleep(10);
    }
    return false;
}

/* RGBA pixels to PNG bytes. */
static bool encode_png(const uint8_t *rgba, int w, int h, int stride, uint8_t **out, size_t *len)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    bool ok = false;

    if (!ctx || !frame || !pkt)
        goto done;
    ctx->width = w;
    ctx->height = h;
    ctx->pix_fmt = AV_PIX_FMT_RGBA;
    ctx->time_base = (AVRational){1, 25};
    if (avcodec_open2(ctx, codec, NULL) < 0)
        goto done;
    frame->format = AV_PIX_FMT_RGBA;
    frame->width = w;
    frame->height = h;
    if (av_frame_get_buffer(frame, 32) < 0)
        goto done;
    for (int y = 0; y < h; y++)
        memcpy(frame->data[0] + (size_t)y * (size_t)frame->linesize[0], rgba + (size_t)y * (size_t)stride,
               (size_t)w * 4);
    if (avcodec_send_frame(ctx, frame) < 0 || avcodec_send_frame(ctx, NULL) < 0
        || avcodec_receive_packet(ctx, pkt) < 0)
        goto done;
    *out = malloc((size_t)pkt->size);
    if (*out) {
        memcpy(*out, pkt->data, (size_t)pkt->size);
        *len = (size_t)pkt->size;
        ok = true;
    }
done:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ok;
}

/* PNG bytes to top-down BGRA pixels. */
static uint8_t *decode_png(const void *png, size_t len, int *w, int *h)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    struct SwsContext *sws = NULL;
    uint8_t *bgra = NULL;

    if (!ctx || !frame || !pkt || avcodec_open2(ctx, codec, NULL) < 0
        || av_new_packet(pkt, (int)len) < 0)
        goto done;
    memcpy(pkt->data, png, len);
    if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, frame) < 0)
        goto done;
    sws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                         frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_POINT, NULL, NULL, NULL);
    bgra = sws ? malloc((size_t)frame->width * (size_t)frame->height * 4) : NULL;
    if (bgra) {
        uint8_t *dst[4] = {bgra, NULL, NULL, NULL};
        int dst_stride[4] = {frame->width * 4, 0, 0, 0};
        sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst,
                  dst_stride);
        *w = frame->width;
        *h = frame->height;
    }
done:
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return bgra;
}

/* A 24- or 32-bit CF_DIB to PNG. Other bitmap layouts are left alone. */
static bool dib_to_png(const BITMAPINFOHEADER *bi, size_t size, uint8_t **out, size_t *len)
{
    const uint8_t *bits;
    int w = bi->biWidth, h = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
    int bpp = bi->biBitCount, stride;
    size_t header = bi->biSize;
    uint8_t *rgba;
    bool ok;

    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (bpp != 24 && bpp != 32))
        return false;
    if (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER))
        header += 12; /* three colour masks follow the header */
    else if (bi->biCompression != BI_RGB && bi->biCompression != BI_BITFIELDS)
        return false;
    stride = ((w * bpp + 31) / 32) * 4;
    if (header + (size_t)stride * (size_t)h > size)
        return false;
    bits = (const uint8_t *)bi + header;
    rgba = malloc((size_t)w * (size_t)h * 4);
    if (!rgba)
        return false;
    {
        bool any_alpha = false;
        for (int y = 0; y < h; y++) {
            const uint8_t *row = bits + (size_t)stride * (size_t)(bi->biHeight < 0 ? y : h - 1 - y);
            uint8_t *dst = rgba + (size_t)y * (size_t)w * 4;
            for (int x = 0; x < w; x++) {
                const uint8_t *px = row + x * (bpp / 8);
                dst[x * 4 + 0] = px[2];
                dst[x * 4 + 1] = px[1];
                dst[x * 4 + 2] = px[0];
                dst[x * 4 + 3] = bpp == 32 ? px[3] : 255;
                any_alpha |= bpp == 32 && px[3] != 0;
            }
        }
        /* Most 32-bit screenshots leave alpha at zero: treat that as opaque. */
        if (bpp == 32 && !any_alpha)
            for (size_t i = 3; i < (size_t)w * (size_t)h * 4; i += 4)
                rgba[i] = 255;
    }
    ok = encode_png(rgba, w, h, w * 4, out, len);
    free(rgba);
    return ok;
}

bool ph_winclip_read_png(uint8_t **out, size_t *len, size_t max)
{
    static DWORD last_seq;
    DWORD seq = GetClipboardSequenceNumber();
    HANDLE data;
    bool ok = false;

    *out = NULL;
    *len = 0;
    /* Only look again after the clipboard changed: reading a big bitmap is not free. */
    if (seq == last_seq)
        return false;
    last_seq = seq;
    if (!IsClipboardFormatAvailable(png_format()) && !IsClipboardFormatAvailable(CF_DIB))
        return false;
    if (!open_board())
        return false;
    if ((data = GetClipboardData(png_format()))) {
        size_t size = GlobalSize(data);
        const void *bytes = GlobalLock(data);
        if (bytes && size > 8 && size <= max && (*out = malloc(size))) {
            memcpy(*out, bytes, size);
            *len = size;
            ok = true;
        }
        if (bytes)
            GlobalUnlock(data);
    } else if ((data = GetClipboardData(CF_DIB))) {
        size_t size = GlobalSize(data);
        const BITMAPINFOHEADER *bi = GlobalLock(data);
        if (bi && size >= sizeof *bi)
            ok = dib_to_png(bi, size, out, len) && *len <= max;
        if (bi)
            GlobalUnlock(data);
        if (!ok) {
            free(*out);
            *out = NULL;
        }
    }
    CloseClipboard();
    return ok;
}

static HGLOBAL global_copy(const void *bytes, size_t len)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, len);
    void *p = g ? GlobalLock(g) : NULL;

    if (!p) {
        if (g)
            GlobalFree(g);
        return NULL;
    }
    memcpy(p, bytes, len);
    GlobalUnlock(g);
    return g;
}

bool ph_winclip_write_png(const void *png, size_t len)
{
    int w = 0, h = 0;
    uint8_t *bgra = decode_png(png, len, &w, &h);
    HGLOBAL png_mem = global_copy(png, len), dib_mem = NULL;
    bool ok = false;

    if (bgra) {
        BITMAPINFOHEADER bi = {0};
        size_t pixels = (size_t)w * (size_t)h * 4;
        uint8_t *dib = malloc(sizeof bi + pixels);
        bi.biSize = sizeof bi;
        bi.biWidth = w;
        bi.biHeight = -h; /* top-down rows */
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        bi.biSizeImage = (DWORD)pixels;
        if (dib) {
            memcpy(dib, &bi, sizeof bi);
            memcpy(dib + sizeof bi, bgra, pixels);
            dib_mem = global_copy(dib, sizeof bi + pixels);
            free(dib);
        }
        free(bgra);
    }
    if ((png_mem || dib_mem) && open_board()) {
        EmptyClipboard();
        if (png_mem && SetClipboardData(png_format(), png_mem)) {
            png_mem = NULL; /* the clipboard owns it now */
            ok = true;
        }
        if (dib_mem && SetClipboardData(CF_DIB, dib_mem)) {
            dib_mem = NULL;
            ok = true;
        }
        CloseClipboard();
    }
    if (png_mem)
        GlobalFree(png_mem);
    if (dib_mem)
        GlobalFree(dib_mem);
    if (!ok)
        PH_LOGW("clipboard image was not accepted");
    return ok;
}

/* OpenClipboard(NULL): a window handle can fail while the caller still reports success. */
bool ph_winclip_read_text(char **out, size_t *len, size_t max)
{
    HANDLE data;
    const wchar_t *wide;
    char *utf8, *norm;
    int bytes;
    size_t n, j, k;

    if (out)
        *out = NULL;
    if (len)
        *len = 0;
    if (!out || !len || !max || !open_board())
        return false;
    data = GetClipboardData(CF_UNICODETEXT);
    wide = data ? GlobalLock(data) : NULL;
    if (!wide || !wide[0]) {
        if (wide)
            GlobalUnlock(data);
        CloseClipboard();
        return false;
    }
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (bytes <= 1 || (size_t)bytes > max + 1) {
        GlobalUnlock(data);
        CloseClipboard();
        return false;
    }
    utf8 = malloc((size_t)bytes);
    if (!utf8 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, bytes, NULL, NULL)
                   != bytes) {
        free(utf8);
        GlobalUnlock(data);
        CloseClipboard();
        return false;
    }
    GlobalUnlock(data);
    CloseClipboard();
    n = (size_t)bytes - 1;
    norm = malloc(n + 1);
    if (!norm) {
        free(utf8);
        return false;
    }
    k = 0;
    for (j = 0; j < n; j++) {
        if (utf8[j] == '\r') {
            norm[k++] = '\n';
            if (j + 1 < n && utf8[j + 1] == '\n')
                j++;
            continue;
        }
        norm[k++] = utf8[j];
    }
    norm[k] = 0;
    free(utf8);
    if (k == 0 || k > max) {
        free(norm);
        return false;
    }
    *out = norm;
    *len = k;
    return true;
}

bool ph_winclip_write_text(const char *utf8, size_t len)
{
    char *crlf;
    wchar_t *wide = NULL;
    HGLOBAL mem = NULL;
    size_t i, out = 0;
    int wchars;
    bool ok = false;

    if (!utf8 || !len || len > (SIZE_MAX / 2) - 1)
        return false;
    crlf = malloc(len * 2 + 1);
    if (!crlf)
        return false;
    for (i = 0; i < len; i++) {
        if (utf8[i] == '\r') {
            crlf[out++] = '\r';
            crlf[out++] = '\n';
            if (i + 1 < len && utf8[i + 1] == '\n')
                i++;
            continue;
        }
        if (utf8[i] == '\n') {
            crlf[out++] = '\r';
            crlf[out++] = '\n';
            continue;
        }
        crlf[out++] = utf8[i];
    }
    crlf[out] = 0;
    wchars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, crlf, -1, NULL, 0);
    if (wchars > 1)
        wide = calloc((size_t)wchars, sizeof *wide);
    if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, crlf, -1, wide, wchars) == wchars)
        mem = global_copy(wide, (size_t)wchars * sizeof *wide);
    if (mem && open_board()) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, mem)) {
            mem = NULL;
            ok = true;
        }
        CloseClipboard();
    }
    if (mem)
        GlobalFree(mem);
    free(wide);
    free(crlf);
    return ok;
}

#else

bool ph_winclip_read_png(uint8_t **out, size_t *len, size_t max)
{
    (void)max;
    if (out)
        *out = NULL;
    if (len)
        *len = 0;
    return false;
}

bool ph_winclip_write_png(const void *png, size_t len)
{
    (void)png;
    (void)len;
    return false;
}

bool ph_winclip_read_text(char **out, size_t *len, size_t max)
{
    (void)max;
    if (out)
        *out = NULL;
    if (len)
        *len = 0;
    return false;
}

bool ph_winclip_write_text(const char *utf8, size_t len)
{
    (void)utf8;
    (void)len;
    return false;
}

#endif
