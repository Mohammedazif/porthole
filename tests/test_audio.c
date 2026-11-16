/* Decode real encoder output and exercise the SDL playback queue without hardware. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "audio/player.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; \
} } while (0)

static void encoded_tone(ph_audio *audio, enum AVCodecID id, unsigned char compression)
{
    const AVCodec *encoder = avcodec_find_encoder(id);
    AVCodecContext *ctx = avcodec_alloc_context3(encoder);
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc(), *repeat = NULL;
    CHECK(encoder && ctx && frame && packet);
    if (!encoder || !ctx || !frame || !packet)
        goto done;
    ctx->sample_rate = 44100;
    ctx->sample_fmt = compression == 2 ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_FLTP;
    ctx->bits_per_raw_sample = compression == 2 ? 16 : 0;
    ctx->bit_rate = 128000;
    ctx->time_base = (AVRational){1, 44100};
    av_channel_layout_default(&ctx->ch_layout, 2);
    int error = avcodec_open2(ctx, encoder, NULL);
    CHECK(error == 0);
    if (error < 0)
        goto done;
    CHECK(ph_audio_configure(audio, compression, (unsigned short)ctx->frame_size));
    frame->format = ctx->sample_fmt;
    frame->sample_rate = ctx->sample_rate;
    frame->nb_samples = ctx->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    error = av_frame_get_buffer(frame, 0);
    CHECK(error == 0);
    if (error < 0)
        goto done;
    int decoded = 0;
    for (int n = 0; n < 6; n++) {
        CHECK(av_frame_make_writable(frame) == 0);
        for (int ch = 0; ch < 2; ch++) {
            for (int i = 0; i < frame->nb_samples; i++) {
                double sample = .1 * sin(6.28318530718 * 440
                                         * (n * frame->nb_samples + i) / 44100);
                if (ctx->sample_fmt == AV_SAMPLE_FMT_S16P)
                    ((int16_t *)frame->data[ch])[i] = (int16_t)(sample * 32767);
                else
                    ((float *)frame->data[ch])[i] = (float)sample;
            }
        }
        frame->pts = (int64_t)n * frame->nb_samples;
        CHECK(avcodec_send_frame(ctx, frame) == 0);
        while (avcodec_receive_packet(ctx, packet) == 0) {
            int samples = ph_audio_push(audio, compression, packet->data, (size_t)packet->size);
            CHECK(samples > 0);
            decoded += samples > 0 ? samples : 0;
            av_packet_free(&repeat);
            repeat = av_packet_clone(packet);
            av_packet_unref(packet);
        }
    }
    CHECK(avcodec_send_frame(ctx, NULL) == 0);
    while (avcodec_receive_packet(ctx, packet) == 0) {
        int samples = ph_audio_push(audio, compression, packet->data, (size_t)packet->size);
        CHECK(samples > 0);
        decoded += samples > 0 ? samples : 0;
        av_packet_unref(packet);
    }
    CHECK(decoded > 0 && repeat);
    if (repeat) {
        /* A fast sender cannot accumulate seconds of audio behind the picture. */
        for (int i = 0; i < 50; i++)
            CHECK(ph_audio_push(audio, compression, repeat->data, (size_t)repeat->size) > 0);
        CHECK(ph_audio_queued_ms(audio) > 0 && ph_audio_queued_ms(audio) <= 200);
    }
    ph_audio_set_volume(audio, -144);
    ph_audio_set_volume(audio, -12);
    ph_audio_set_volume(audio, NAN);
    ph_audio_set_volume(audio, 0);
    ph_audio_flush(audio);
    CHECK(ph_audio_queued_ms(audio) == 0);
done:
    av_packet_free(&repeat);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
}

int main(void)
{
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    ph_audio *audio = ph_audio_create();
    CHECK(audio);
    if (!audio)
        return 1;
    CHECK(ph_audio_configure(audio, 8, 480));
    CHECK(ph_audio_configure(audio, 8, 512));
    CHECK(!ph_audio_configure(audio, 99, 0));
    CHECK(ph_audio_push(audio, 8, NULL, 0) == -1);
    CHECK(ph_audio_push(audio, 8, (const uint8_t *)"x", 1024 * 1024 + 1) == -1);
    encoded_tone(audio, AV_CODEC_ID_ALAC, 2);
    encoded_tone(audio, AV_CODEC_ID_AAC, 4);
    ph_audio_destroy(audio);
    ph_audio_destroy(NULL);
    if (failures)
        return 1;
    puts("audio formats, real packet decoding, queue bounds and flush passed");
    return 0;
}
