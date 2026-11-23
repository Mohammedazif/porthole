#include "ui/clipboard_preview.h"
#include "files/clip.h"

#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <libswscale/swscale.h>
#include <string.h>

bool ph_clipboard_png_size(const void *data, size_t len, uint32_t *width, uint32_t *height)
{
    const unsigned char *bytes = data;
    if (!bytes || len < 33 || memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) ||
        AV_RB32(bytes + 8) != 13 || memcmp(bytes + 12, "IHDR", 4))
        return false;
    *width = AV_RB32(bytes + 16);
    *height = AV_RB32(bytes + 20);
    return *width && *height;
}

SDL_Texture *ph_clipboard_thumbnail(SDL_Renderer *renderer, const void *data, size_t len)
{
    enum { MAX_PIXELS = 32 * 1024 * 1024, MAX_SIDE = 16384, THUMB_SIDE = 144 };
    uint32_t width, height;
    if (len > PH_CLIP_IMAGE_MAX || !ph_clipboard_png_size(data, len, &width, &height) ||
        width > MAX_SIDE || height > MAX_SIDE || (uint64_t)width * height > MAX_PIXELS)
        return NULL;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (!codec)
        return NULL;
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();
    AVFrame *thumbnail = av_frame_alloc();
    struct SwsContext *scale = NULL;
    SDL_Texture *texture = NULL;
    if (!decoder || !packet || !decoded || !thumbnail)
        goto done;

    decoder->max_pixels = MAX_PIXELS;
    decoder->thread_count = 1;
    if (avcodec_open2(decoder, codec, NULL) < 0 || av_new_packet(packet, (int)len) < 0)
        goto done;
    memcpy(packet->data, data, len);
    if (avcodec_send_packet(decoder, packet) < 0 || avcodec_receive_frame(decoder, decoded) < 0 ||
        decoded->width != (int)width || decoded->height != (int)height)
        goto done;

    unsigned longest = SDL_max(width, height);
    unsigned target = SDL_min(longest, (unsigned)THUMB_SIDE);
    thumbnail->width = (int)SDL_max(1u, width * target / longest);
    thumbnail->height = (int)SDL_max(1u, height * target / longest);
    thumbnail->format = AV_PIX_FMT_RGBA;
    if (av_frame_get_buffer(thumbnail, 32) < 0)
        goto done;
    scale = sws_getContext(decoded->width, decoded->height, decoded->format,
                           thumbnail->width, thumbnail->height, thumbnail->format,
                           SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale || sws_scale(scale, (const uint8_t *const *)decoded->data, decoded->linesize,
                            0, decoded->height, thumbnail->data,
                            thumbnail->linesize) != thumbnail->height)
        goto done;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                thumbnail->width, thumbnail->height);
    if (texture) {
        if (SDL_UpdateTexture(texture, NULL, thumbnail->data[0], thumbnail->linesize[0]) != 0) {
            SDL_DestroyTexture(texture);
            texture = NULL;
        } else {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
        }
    }

done:
    sws_freeContext(scale);
    av_frame_free(&thumbnail);
    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    return texture;
}
