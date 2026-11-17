#include "audio/player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include "audio/sync.h"
#include "core/log.h"

#define RATE 44100
#define CHANNELS 2
#define QUEUE_BYTES (RATE * CHANNELS * sizeof(float) / 5) /* at most 200 ms */

struct ph_held_pcm {
    struct ph_held_pcm *next;
    uint8_t *pcm;
    int samples;
    uint32_t hold_ms;
    uint32_t bytes;
    uint64_t release_ms;
    bool anchored;
};

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
    float volume_db;
    bool muted;
    int sync_offset_ms;
    bool warned, playing;
    ph_sync *sync;
    struct ph_held_pcm *hold_head, *hold_tail;
    size_t hold_bytes;
    bool clock_valid;
    uint64_t clock_ms;
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

static void drop_oldest_hold(ph_audio *a)
{
    struct ph_held_pcm *node = a->hold_head;

    if (!node)
        return;
    a->hold_head = node->next;
    if (!a->hold_head)
        a->hold_tail = NULL;
    if (a->hold_bytes >= node->bytes)
        a->hold_bytes -= node->bytes;
    else
        a->hold_bytes = 0;
    av_free(node->pcm);
    free(node);
}

static void free_holds(ph_audio *a)
{
    while (a->hold_head)
        drop_oldest_hold(a);
}

static void flush_locked(ph_audio *a)
{
    if (a->device)
        SDL_ClearQueuedAudio(a->device);
    if (a->codec)
        avcodec_flush_buffers(a->codec);
    swr_free(&a->resample);
    a->playing = false;
    free_holds(a);
    ph_sync_reset(a->sync);
    a->clock_valid = false;
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

static uint32_t device_queued_ms_locked(const ph_audio *a)
{
    if (!a->device)
        return 0;
    return SDL_GetQueuedAudioSize(a->device) * 1000U / (RATE * CHANNELS * sizeof(float));
}

static int convert_frame(ph_audio *a, uint8_t **out, int *count_out)
{
    AVFrame *f = a->frame;
    int capacity;
    uint8_t *pcm = NULL;
    int count;

    *out = NULL;
    *count_out = 0;
    if (!a->resample || a->input_format != f->format || a->input_rate != f->sample_rate
        || a->input_channels != f->ch_layout.nb_channels) {
        swr_free(&a->resample);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&a->resample, &stereo, AV_SAMPLE_FMT_FLT, RATE,
                               &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0
            || !a->resample || swr_init(a->resample) < 0) {
            swr_free(&a->resample);
            return -1;
        }
        a->input_format = f->format;
        a->input_rate = f->sample_rate;
        a->input_channels = f->ch_layout.nb_channels;
    }
    capacity = swr_get_out_samples(a->resample, f->nb_samples);
    if (capacity <= 0 || capacity > 16384
        || av_samples_alloc(&pcm, NULL, CHANNELS, capacity, AV_SAMPLE_FMT_FLT, 0) < 0)
        return -1;
    count = swr_convert(a->resample, &pcm, capacity,
                        (const uint8_t **)f->extended_data, f->nb_samples);
    if (count <= 0) {
        av_free(pcm);
        *count_out = count;
        return count < 0 ? -1 : 0;
    }
    *out = pcm;
    *count_out = count;
    return 0;
}

static int queue_pcm(ph_audio *a, uint8_t *pcm, int count)
{
    float *values;
    Uint32 bytes;

    if (count <= 0) {
        av_free(pcm);
        return count;
    }
    if (!a->device) {
        av_free(pcm);
        return -1;
    }
    values = (float *)pcm;
    for (int i = 0; i < count * CHANNELS; i++)
        values[i] *= a->gain;
    bytes = (Uint32)(count * CHANNELS * sizeof(float));
    if (SDL_GetQueuedAudioSize(a->device) + bytes > QUEUE_BYTES)
        SDL_ClearQueuedAudio(a->device); /* recover instead of accumulating audible lag */
    if (SDL_QueueAudio(a->device, pcm, bytes) != 0)
        count = -1;
    else if (!a->playing) {
        PH_LOGI("audio playing: %s, %d Hz stereo", format_name(a->compression), RATE);
        a->playing = true;
    }
    av_free(pcm);
    return count;
}

static int hold_pcm(ph_audio *a, uint8_t *pcm, int count, int hold_ms)
{
    size_t bytes;
    struct ph_held_pcm *node;

    if (count <= 0) {
        av_free(pcm);
        return 0;
    }
    if (hold_ms < 0)
        hold_ms = 0;
    if (hold_ms > 200)
        hold_ms = 200;
    bytes = (size_t)count * CHANNELS * sizeof(float);
    if (bytes > UINT32_MAX) {
        av_free(pcm);
        return -1;
    }
    while (a->hold_head && a->hold_bytes + bytes > (size_t)QUEUE_BYTES)
        drop_oldest_hold(a);
    node = calloc(1, sizeof *node);
    if (!node) {
        av_free(pcm);
        return -1;
    }
    node->pcm = pcm;
    node->samples = count;
    node->bytes = (uint32_t)bytes;
    node->hold_ms = (uint32_t)hold_ms;
    if (a->clock_valid) {
        uint64_t release = a->clock_ms + (uint64_t)hold_ms;
        if (a->hold_tail && a->hold_tail->anchored && a->hold_tail->release_ms > release)
            release = a->hold_tail->release_ms;
        node->anchored = true;
        node->release_ms = release;
    }
    if (!a->hold_head)
        a->hold_head = node;
    else
        a->hold_tail->next = node;
    a->hold_tail = node;
    a->hold_bytes += bytes;
    return 0;
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
    a->sync = ph_sync_create();
    if (!a->lock || !a->packet || !a->frame || !a->sync) {
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

static int push_locked(ph_audio *a, unsigned char compression, const uint8_t *data, size_t len,
                       bool timed, uint64_t pts_ns)
{
    int result = -1;
    ph_sync_action action = {PH_SYNC_PLAY, 0};
    int error;

    if ((!a->codec || compression != a->compression)
        && !configure_locked(a, compression, compression == 2 ? 352 : compression == 8 ? 480 : 1024))
        goto done;
    if (timed) {
        action = ph_sync_audio(a->sync, pts_ns, 0);
        if (action.kind == PH_SYNC_DROP) {
            result = 0;
            goto done;
        }
    }
    if (av_new_packet(a->packet, (int)len) < 0)
        goto done;
    memcpy(a->packet->data, data, len);
    error = avcodec_send_packet(a->codec, a->packet);
    av_packet_unref(a->packet);
    if (error < 0)
        goto done;
    result = 0;
    while ((error = avcodec_receive_frame(a->codec, a->frame)) == 0) {
        uint8_t *pcm = NULL;
        int samples = 0;
        int conv = convert_frame(a, &pcm, &samples);
        av_frame_unref(a->frame);
        if (conv < 0) {
            result = -1;
            goto done;
        }
        if (samples <= 0)
            continue;
        if (a->muted) {
            av_free(pcm);
        } else if (a->hold_head || (timed && action.kind == PH_SYNC_HOLD)) {
            int delay = timed && action.kind == PH_SYNC_HOLD ? action.hold_or_drop_ms : 0;
            if (hold_pcm(a, pcm, samples, delay) < 0) {
                result = -1;
                goto done;
            }
        } else if (queue_pcm(a, pcm, samples) < 0) {
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
    return result;
}

static int push_outer(ph_audio *a, unsigned char compression, const uint8_t *data, size_t len,
                      bool timed, uint64_t pts_ns)
{
    int result;

    if (!a || !data || !len || len > 1024 * 1024)
        return -1;
    SDL_LockMutex(a->lock);
    result = push_locked(a, compression, data, len, timed, pts_ns);
    SDL_UnlockMutex(a->lock);
    return result;
}

int ph_audio_push(ph_audio *a, unsigned char compression, const uint8_t *data, size_t len)
{
    return push_outer(a, compression, data, len, false, 0);
}

int ph_audio_push_timed(ph_audio *a, unsigned char compression, const uint8_t *data, size_t len,
                        uint64_t pts_ns)
{
    return push_outer(a, compression, data, len, true, pts_ns);
}

void ph_audio_note_video_pts(ph_audio *a, uint64_t pts_ns)
{
    if (!a)
        return;
    SDL_LockMutex(a->lock);
    ph_sync_note_video(a->sync, pts_ns);
    SDL_UnlockMutex(a->lock);
}

void ph_audio_pump(ph_audio *a, uint64_t now_ms)
{
    uint64_t prev = 0;
    bool have_prev = false;

    if (!a)
        return;
    SDL_LockMutex(a->lock);
    a->clock_ms = now_ms;
    a->clock_valid = true;
    for (struct ph_held_pcm *node = a->hold_head; node; node = node->next) {
        if (!node->anchored) {
            uint64_t release = now_ms + node->hold_ms;
            if (have_prev && release < prev)
                release = prev;
            node->release_ms = release;
            node->anchored = true;
        }
        prev = node->release_ms;
        have_prev = true;
    }
    while (a->hold_head && a->hold_head->release_ms <= now_ms) {
        struct ph_held_pcm *node = a->hold_head;
        a->hold_head = node->next;
        if (!a->hold_head)
            a->hold_tail = NULL;
        if (a->hold_bytes >= node->bytes)
            a->hold_bytes -= node->bytes;
        else
            a->hold_bytes = 0;
        if (a->muted)
            av_free(node->pcm);
        else
            queue_pcm(a, node->pcm, node->samples);
        free(node);
    }
    SDL_UnlockMutex(a->lock);
}

void ph_audio_set_volume(ph_audio *a, float decibels)
{
    if (!a || !isfinite(decibels))
        return;
    SDL_LockMutex(a->lock);
    if (decibels <= -144.f) {
        a->volume_db = decibels;
        a->gain = 0.f;
    } else {
        a->volume_db = fminf(0.f, decibels);
        a->gain = powf(10.f, a->volume_db / 20.f);
    }
    SDL_UnlockMutex(a->lock);
}

void ph_audio_adjust_volume_db(ph_audio *a, float delta)
{
    float next;

    if (!a || !isfinite(delta))
        return;
    SDL_LockMutex(a->lock);
    next = a->volume_db + delta;
    if (!isfinite(next)) {
        SDL_UnlockMutex(a->lock);
        return;
    }
    if (next < -30.f)
        next = -30.f;
    if (next > 0.f)
        next = 0.f;
    a->volume_db = next;
    a->gain = powf(10.f, next / 20.f);
    SDL_UnlockMutex(a->lock);
}

void ph_audio_set_muted(ph_audio *a, bool muted)
{
    if (!a)
        return;
    SDL_LockMutex(a->lock);
    a->muted = muted;
    SDL_UnlockMutex(a->lock);
}

void ph_audio_set_sync_offset_ms(ph_audio *a, int offset_ms)
{
    if (!a)
        return;
    if (offset_ms > 500)
        offset_ms = 500;
    if (offset_ms < -500)
        offset_ms = -500;
    SDL_LockMutex(a->lock);
    a->sync_offset_ms = offset_ms;
    ph_sync_set_offset_ms(a->sync, offset_ms);
    SDL_UnlockMutex(a->lock);
}

bool ph_audio_settings(const ph_audio *audio, struct ph_audio_settings *out)
{
    ph_audio *a = (ph_audio *)audio;
    uint32_t held;

    if (!a || !out)
        return false;
    SDL_LockMutex(a->lock);
    held = (uint32_t)(a->hold_bytes * 1000U / (RATE * CHANNELS * sizeof(float)));
    out->muted = a->muted;
    out->volume_db = a->volume_db;
    out->sync_offset_ms = a->sync_offset_ms;
    out->queued_ms = device_queued_ms_locked(a) + held;
    SDL_UnlockMutex(a->lock);
    return true;
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
    uint32_t ms;

    if (!a)
        return 0;
    SDL_LockMutex(a->lock);
    ms = device_queued_ms_locked(a);
    SDL_UnlockMutex(a->lock);
    return ms;
}

void ph_audio_destroy(ph_audio *a)
{
    if (!a)
        return;
    free_holds(a);
    if (a->device)
        SDL_CloseAudioDevice(a->device);
    swr_free(&a->resample);
    avcodec_free_context(&a->codec);
    av_frame_free(&a->frame);
    av_packet_free(&a->packet);
    ph_sync_destroy(a->sync);
    if (a->lock)
        SDL_DestroyMutex(a->lock);
    free(a);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
