/* Decode real encoder output and exercise the SDL playback queue without hardware. */
#include <math.h>
#include <stdint.h>
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

static AVPacket *one_aac_packet(unsigned short *frame_samples)
{
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext *ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL, *kept = NULL;

    if (!encoder)
        return NULL;
    ctx = avcodec_alloc_context3(encoder);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!ctx || !frame || !packet)
        goto done;
    ctx->sample_rate = 44100;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->bit_rate = 128000;
    ctx->time_base = (AVRational){1, 44100};
    av_channel_layout_default(&ctx->ch_layout, 2);
    if (avcodec_open2(ctx, encoder, NULL) < 0)
        goto done;
    frame->format = ctx->sample_fmt;
    frame->sample_rate = ctx->sample_rate;
    frame->nb_samples = ctx->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    if (av_frame_get_buffer(frame, 0) < 0)
        goto done;
    if (frame_samples)
        *frame_samples = (unsigned short)ctx->frame_size;
    for (int n = 0; n < 2 && !kept; n++) {
        if (av_frame_make_writable(frame) < 0)
            goto done;
        for (int ch = 0; ch < 2; ch++)
            memset(frame->data[ch], 0, (size_t)frame->nb_samples * sizeof(float));
        frame->pts = (int64_t)n * frame->nb_samples;
        if (avcodec_send_frame(ctx, frame) < 0)
            goto done;
        while (avcodec_receive_packet(ctx, packet) == 0) {
            if (!kept)
                kept = av_packet_clone(packet);
            av_packet_unref(packet);
        }
    }
    if (!kept && avcodec_send_frame(ctx, NULL) == 0) {
        while (avcodec_receive_packet(ctx, packet) == 0) {
            if (!kept)
                kept = av_packet_clone(packet);
            av_packet_unref(packet);
        }
    }
done:
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return kept;
}

static void settings_cover(ph_audio *audio)
{
    struct ph_audio_settings out;

    CHECK(ph_audio_settings(audio, &out));
    CHECK(!out.muted);
    ph_audio_set_volume(audio, -12);
    CHECK(ph_audio_settings(audio, &out) && out.volume_db == -12.f);
    ph_audio_adjust_volume_db(audio, -3);
    CHECK(ph_audio_settings(audio, &out) && out.volume_db == -15.f);
    ph_audio_adjust_volume_db(audio, -100);
    CHECK(ph_audio_settings(audio, &out) && out.volume_db == -30.f);
    ph_audio_adjust_volume_db(audio, 1000);
    CHECK(ph_audio_settings(audio, &out) && out.volume_db == 0.f);
    ph_audio_adjust_volume_db(audio, NAN);
    CHECK(ph_audio_settings(audio, &out) && out.volume_db == 0.f);
    ph_audio_set_volume(audio, -144);
    CHECK(ph_audio_settings(audio, &out) && out.volume_db == -144.f);
    ph_audio_set_volume(audio, 0);
    ph_audio_set_sync_offset_ms(audio, 800);
    CHECK(ph_audio_settings(audio, &out) && out.sync_offset_ms == 500);
    ph_audio_set_sync_offset_ms(audio, -900);
    CHECK(ph_audio_settings(audio, &out) && out.sync_offset_ms == -500);
    ph_audio_set_sync_offset_ms(audio, 0);
    ph_audio_set_muted(audio, true);
    CHECK(ph_audio_settings(audio, &out) && out.muted);
    ph_audio_set_muted(audio, false);
    CHECK(!ph_audio_settings(NULL, &out));
    CHECK(!ph_audio_settings(audio, NULL));
    ph_audio_set_muted(NULL, true);
    ph_audio_adjust_volume_db(NULL, -1);
    ph_audio_set_sync_offset_ms(NULL, 1);
    ph_audio_note_video_pts(NULL, 1);
    ph_audio_pump(NULL, 1);
    CHECK(ph_audio_push_timed(NULL, 4, (const uint8_t *)"x", 1, 1) == -1);
    CHECK(ph_audio_push_timed(audio, 4, NULL, 0, 1) == -1);
}

static void sync_queue(ph_audio *audio)
{
    const uint64_t ms = 1000000ULL;
    const uint64_t video = 8000000000000ULL;
    unsigned short samples = 0;
    AVPacket *packet = one_aac_packet(&samples);
    struct ph_audio_settings settings;
    uint32_t queued;
    int produced;

    CHECK(packet && samples);
    if (!packet)
        return;
    CHECK(ph_audio_configure(audio, 4, samples));
    ph_audio_set_volume(audio, 0);
    ph_audio_set_muted(audio, false);
    ph_audio_set_sync_offset_ms(audio, 0);
    ph_audio_flush(audio);
    ph_audio_pump(audio, 1000);
    ph_audio_note_video_pts(audio, video + 5000 * ms);
    produced = ph_audio_push(audio, 4, packet->data, (size_t)packet->size);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) > 0);

    ph_audio_flush(audio);
    ph_audio_pump(audio, 1000);
    ph_audio_note_video_pts(audio, video);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video);
    CHECK(produced > 0);
    queued = ph_audio_queued_ms(audio);
    CHECK(queued > 0);
    /* Late audio is never delayed further: it plays at once. */
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video - 80 * ms);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) > queued);
    queued = ph_audio_queued_ms(audio);
    /* Audio well ahead of its picture waits out the lead not already covered
     * by queued device audio (about 150 ms minus two queued AAC frames). */
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video + 150 * ms);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) == queued);
    CHECK(ph_audio_settings(audio, &settings));
    CHECK(settings.queued_ms > queued);
    ph_audio_pump(audio, 1010);
    CHECK(ph_audio_queued_ms(audio) == queued);
    ph_audio_pump(audio, 1200);
    CHECK(ph_audio_queued_ms(audio) > queued);

    ph_audio_set_muted(audio, true);
    queued = ph_audio_queued_ms(audio);
    CHECK(queued == 0);
    CHECK(ph_audio_settings(audio, &settings) && settings.queued_ms == 0);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) == queued);
    ph_audio_set_muted(audio, false);

    ph_audio_flush(audio);
    ph_audio_pump(audio, 2000);
    ph_audio_note_video_pts(audio, video);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video + 150 * ms);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) == 0); /* held, not yet queued */
    ph_audio_flush(audio);
    ph_audio_pump(audio, 12000);
    CHECK(ph_audio_queued_ms(audio) == 0);

    ph_audio_pump(audio, 3000);
    ph_audio_note_video_pts(audio, video);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, 0);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) > 0);

    ph_audio_flush(audio);
    ph_audio_pump(audio, 8000);
    ph_audio_note_video_pts(audio, video);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video);
    CHECK(produced > 0);
    queued = ph_audio_queued_ms(audio);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video - 600 * ms);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) > queued);

    ph_audio_set_sync_offset_ms(audio, 25);
    ph_audio_flush(audio);
    CHECK(ph_audio_settings(audio, &settings) && settings.sync_offset_ms == 25);
    ph_audio_pump(audio, 4000);
    ph_audio_note_video_pts(audio, video);
    ph_audio_set_sync_offset_ms(audio, 100);
    queued = ph_audio_queued_ms(audio);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) == queued);
    ph_audio_pump(audio, 4100);
    CHECK(ph_audio_queued_ms(audio) > queued);
    queued = ph_audio_queued_ms(audio);
    /* A negative offset cannot make late audio earlier: it simply plays. */
    ph_audio_set_sync_offset_ms(audio, -100);
    produced = ph_audio_push_timed(audio, 4, packet->data, (size_t)packet->size, video);
    CHECK(produced > 0);
    CHECK(ph_audio_queued_ms(audio) > queued);

    /* A burst (a new Reel/Short starting) cannot build a lasting backlog: once
     * about 100 ms is queued, further arriving audio is dropped to catch up. */
    ph_audio_set_sync_offset_ms(audio, 0);
    ph_audio_flush(audio);
    ph_audio_pump(audio, 9000);
    for (int i = 0; i < 40; i++)
        CHECK(ph_audio_push(audio, 4, packet->data, (size_t)packet->size) > 0);
    CHECK(ph_audio_queued_ms(audio) >= 90 && ph_audio_queued_ms(audio) < 100 + 30);
    ph_audio_set_sync_offset_ms(audio, 0);
    ph_audio_set_volume(audio, 0);
    av_packet_free(&packet);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
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
    settings_cover(audio);
    sync_queue(audio);
    ph_audio_destroy(audio);
    ph_audio_destroy(NULL);
    if (failures)
        return 1;
    puts("audio formats, real packet decoding, queue bounds and flush passed");
    return 0;
}
