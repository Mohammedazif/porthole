#include "audio/player.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include "core/log.h"

#define RATE 44100
#define CHANNELS 2
#define QUEUE_BYTES (RATE * CHANNELS * sizeof(float) / 5) /* at most 200 ms */

struct ph_audio {
    SDL_mutex *lock;
    SDL_AudioDeviceID device;
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;
    SwrContext *resample;
    int input_format, input_rate, input_channels;
    unsigned char compression;
    unsigned short samples;
    float gain;
    bool warned, playing;
};

static const char *format_name(unsigned char compression)
{
    switch (compression) {
    case 2: return "ALAC";
    case 4: return "AAC-LC";
    case 8: return "AAC-ELD";
    default: return "unsupported";
    }
}

static void flush_locked(ph_audio *a)
{
    if (a->device)
        SDL_ClearQueuedAudio(a->device);
    if (a->codec)
        avcodec_flush_buffers(a->codec);
    swr_free(&a->resample);
    a->playing = false;
}

static bool configure_locked(ph_audio *a, unsigned char compression, unsigned short samples)
{
    if (a->codec && compression == a->compression && samples == a->samples)
        return true;
    flush_locked(a);
    avcodec_free_context(&a->codec);
    a->compression = compression;
    a->samples = samples;
    a->warned = false;

    enum AVCodecID id = compression == 2 ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC;
    if (compression != 2 && compression != 4 && compression != 8) {
        PH_LOGW("unsupported AirPlay audio compression %u", compression);
        return false;
    }
    const AVCodec *decoder = avcodec_find_decoder(id);
    a->codec = avcodec_alloc_context3(decoder);
    if (!decoder || !a->codec)
        goto fail;

    /* The same 44.1 kHz stereo codec configuration as the vendored UxPlay
     * renderer. ALAC's magic cookie includes its negotiated frame length. */
    uint8_t alac[] = {0,0,0,36, 'a','l','a','c', 0,0,0,0,
                     0,0,1,96, 0,16,40,10,14,2,0,255,
                     0,0,0,0, 0,0,0,0, 0,0,172,68};
    uint8_t eld[] = {0xf8, 0xe8, samples == 512 ? 0x40 : 0x50, 0};
    const uint8_t lc[] = {0x12, 0x10};
    if (compression == 2 && samples) {
        alac[12] = 0;
        alac[13] = 0;
        alac[14] = (uint8_t)(samples >> 8);
        alac[15] = (uint8_t)samples;
    }
    const uint8_t *extra = compression == 2 ? alac : compression == 8 ? eld : lc;
    int extra_len = compression == 2 ? sizeof alac : compression == 8 ? sizeof eld : sizeof lc;
    a->codec->extradata = av_mallocz((size_t)extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!a->codec->extradata)
        goto fail;
    memcpy(a->codec->extradata, extra, (size_t)extra_len);
    a->codec->extradata_size = extra_len;
    a->codec->sample_rate = RATE;
    av_channel_layout_default(&a->codec->ch_layout, CHANNELS);
    if (avcodec_open2(a->codec, decoder, NULL) < 0)
        goto fail;

    if (!a->device) {
        SDL_AudioSpec wanted = {.freq = RATE, .format = AUDIO_F32SYS,
                                .channels = CHANNELS, .samples = 512};
        a->device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
        if (!a->device) {
            PH_LOGW("couldn't open PC audio output: %s", SDL_GetError());
            goto fail;
        }
        SDL_PauseAudioDevice(a->device, 0);
    }
    PH_LOGI("audio ready: %s, %u samples, PC speakers", format_name(compression), samples);
    return true;
fail:
    PH_LOGW("couldn't initialise %s audio playback", format_name(compression));
    avcodec_free_context(&a->codec);
    return false;
}

ph_audio *ph_audio_create(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        PH_LOGW("couldn't initialise PC audio: %s", SDL_GetError());
        return NULL;
    }
    ph_audio *a = calloc(1, sizeof *a);
    if (!a) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }
    a->gain = 1;
    a->lock = SDL_CreateMutex();
    a->packet = av_packet_alloc();
    a->frame = av_frame_alloc();
    if (!a->lock || !a->packet || !a->frame) {
        ph_audio_destroy(a);
        return NULL;
    }
    return a;
}

bool ph_audio_configure(ph_audio *a, unsigned char compression, unsigned short samples)
{
    if (!a)
        return false;
    SDL_LockMutex(a->lock);
    bool ok = configure_locked(a, compression, samples);
    SDL_UnlockMutex(a->lock);
    return ok;
}

static int queue_frame(ph_audio *a)
{
    AVFrame *f = a->frame;
    if (!a->resample || a->input_format != f->format || a->input_rate != f->sample_rate
        || a->input_channels != f->ch_layout.nb_channels) {
        swr_free(&a->resample);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&a->resample, &stereo, AV_SAMPLE_FMT_FLT, RATE,
                               &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0
            || swr_init(a->resample) < 0)
            return -1;
        a->input_format = f->format;
        a->input_rate = f->sample_rate;
        a->input_channels = f->ch_layout.nb_channels;
    }
    int capacity = swr_get_out_samples(a->resample, f->nb_samples);
    uint8_t *pcm = NULL;
    if (capacity <= 0 || capacity > 16384
        || av_samples_alloc(&pcm, NULL, CHANNELS, capacity, AV_SAMPLE_FMT_FLT, 0) < 0)
        return -1;
    int count = swr_convert(a->resample, &pcm, capacity,
                            (const uint8_t **)f->extended_data, f->nb_samples);
    if (count > 0) {
        float *values = (float *)pcm;
        for (int i = 0; i < count * CHANNELS; i++)
            values[i] *= a->gain;
        Uint32 bytes = (Uint32)(count * CHANNELS * sizeof(float));
        if (SDL_GetQueuedAudioSize(a->device) + bytes > QUEUE_BYTES)
            SDL_ClearQueuedAudio(a->device); /* recover instead of accumulating audible lag */
        if (SDL_QueueAudio(a->device, pcm, bytes) != 0)
            count = -1;
        else if (!a->playing) {
            PH_LOGI("audio playing: %s, %d Hz stereo", format_name(a->compression), RATE);
            a->playing = true;
        }
    }
    av_free(pcm);
    return count;
}

int ph_audio_push(ph_audio *a, unsigned char compression, const uint8_t *data, size_t len)
{
    if (!a || !data || !len || len > 1024 * 1024)
        return -1;
    SDL_LockMutex(a->lock);
    int result = -1;
    if ((!a->codec || compression != a->compression)
        && !configure_locked(a, compression, compression == 2 ? 352 : compression == 8 ? 480 : 1024))
        goto done;
    if (av_new_packet(a->packet, (int)len) < 0)
        goto done;
    memcpy(a->packet->data, data, len);
    int error = avcodec_send_packet(a->codec, a->packet);
    av_packet_unref(a->packet);
    if (error < 0)
        goto done;
    result = 0;
    while ((error = avcodec_receive_frame(a->codec, a->frame)) == 0) {
        int samples = queue_frame(a);
        av_frame_unref(a->frame);
        if (samples < 0) {
            result = -1;
            goto done;
        }
        result += samples;
    }
    if (error != AVERROR(EAGAIN) && error != AVERROR_EOF)
        result = -1;
done:
    if (result < 0 && !a->warned) {
        PH_LOGW("audio packet could not be played (%s)", format_name(compression));
        a->warned = true;
    }
    SDL_UnlockMutex(a->lock);
    return result;
}

void ph_audio_set_volume(ph_audio *a, float decibels)
{
    if (!a || !isfinite(decibels))
        return;
    SDL_LockMutex(a->lock);
    a->gain = decibels <= -144 ? 0 : powf(10, fminf(0, decibels) / 20);
    SDL_UnlockMutex(a->lock);
}

void ph_audio_flush(ph_audio *a)
{
    if (!a)
        return;
    SDL_LockMutex(a->lock);
    flush_locked(a);
    SDL_UnlockMutex(a->lock);
}

uint32_t ph_audio_queued_ms(ph_audio *a)
{
    if (!a)
        return 0;
    SDL_LockMutex(a->lock);
    uint32_t ms = a->device ? SDL_GetQueuedAudioSize(a->device) * 1000U
                                / (RATE * CHANNELS * sizeof(float)) : 0;
    SDL_UnlockMutex(a->lock);
    return ms;
}

void ph_audio_destroy(ph_audio *a)
{
    if (!a)
        return;
    if (a->device)
        SDL_CloseAudioDevice(a->device);
    swr_free(&a->resample);
    avcodec_free_context(&a->codec);
    av_frame_free(&a->frame);
    av_packet_free(&a->packet);
    SDL_DestroyMutex(a->lock);
    free(a);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
