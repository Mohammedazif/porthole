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
#include "core/metrics.h"

#define RATE 44100
#define CHANNELS 2
#define QUEUE_BYTES (RATE * CHANNELS * sizeof(float) / 5) /* at most 200 ms */
/* PC output backlog. AirPlay delivers a burst when a stream (re)starts, e.g.
 * scrolling to a new Reel; a queue only cleared at 200 ms then stayed that late
 * for good. Drop arriving audio above this instead, so playback catches up. */
#define BACKLOG_MAX_MS 100
#define STATS_EVERY_MS 2000
/* Scheduled sound (AirPlay beside the shared screen, and ReplayKit audio)
 * waits in its own queue until it is nearly due. The device queue stays short
 * so a flush or a new picture delay takes effect at once. */
#define TIMED_FILL_US 80000
/* A chunk this early starts after silence. Smaller errors play on directly. */
#define TIMED_EARLY_US 25000
/* A chunk this late is skipped, so playback catches up with the picture. */
#define TIMED_LATE_US 45000
/* SDL's 512-frame buffer plus a typical system mixer. */
#define OUTPUT_LATENCY_US 25000
/* AirPlay sends sound about two seconds ahead; keep room for it and the picture. */
#define TIMED_MAX_BYTES ((size_t)RATE * CHANNELS * sizeof(float) * 4)
/* Quickest capture-to-arrival trip of a shared-screen frame: ReplayKit hands
 * over the frame, it is scaled and encoded, and crosses the LAN. */
#define FASTEST_TRIP_MS 30
/* ReplayKit audio waits this long past its slowest recent arrival, covering
 * the output latency and Wi-Fi jitter. */
#define BROADCAST_AUDIO_MARGIN_MS 60

struct ph_held_pcm {
    struct ph_held_pcm *next;
    uint8_t *pcm;
    int samples;
    uint32_t hold_ms;
    uint32_t bytes;
    uint64_t release_ms;
    bool anchored;
};

struct ph_timed_pcm {
    struct ph_timed_pcm *next;
    uint8_t *pcm; /* stereo float, gain not yet applied */
    int samples;
    int64_t due_us; /* when the first sample should be heard, ph_metrics_now_us clock */
};

struct ph_audio {
    SDL_mutex *lock;
    SDL_AudioDeviceID device;
    uint64_t retry_output_at;
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;
    SwrContext *resample;
    int input_format, input_rate, input_channels;
    unsigned char compression;
    unsigned short samples;
    unsigned char airplay_compression;
    unsigned short airplay_samples;
    float gain;
    float airplay_gain;
    float volume_db;
    bool muted;
    int sync_offset_ms;
    bool warned, playing;
    ph_sync *sync;
    struct ph_held_pcm *hold_head, *hold_tail;
    size_t hold_bytes;
    bool clock_valid;
    uint64_t clock_ms;
    struct ph_timed_pcm *timed_head, *timed_tail;
    size_t timed_bytes;
    ph_av_delay av; /* shared-screen capture-to-display timing */
    bool unscheduled_warned;
    uint8_t *pcm_scratch;
    int pcm_capacity;
    struct {
        uint64_t since_ms;
        unsigned int played, held, trimmed;
        int lead_min, lead_max;
        int64_t lead_sum;
        unsigned int leads;
        uint32_t backlog_max;
    } stats;
    struct {
        uint64_t since_ms;
        unsigned int chunks, late, gaps;
        int64_t gap_us;
        int64_t due_in_min, due_in_max; /* AirPlay due time on arrival, ms */
        bool airplay;
        float peak; /* loudest ReplayKit sample, 0..1 */
    } tstats;
    struct {
        uint64_t since_ms, last_capture_ns;
        unsigned int arrived, drawn;
        int64_t capture_gap_max, behind_max; /* ms */
    } vstats;
    /* Every fifth window's worst also goes to the normal log, which is all a
     * Windows run keeps, so a slow picture shows without --verbose. */
    unsigned int vstats_windows;
    int64_t vstats_worst_behind, vstats_worst_gap;
};

static const char *format_name(unsigned char compression)
{
    switch (compression) {
    case 1: return "broadcast PCM";
    case 2: return "ALAC";
    case 4: return "AAC-LC";
    case 8: return "AAC-ELD";
    default: return "unsupported";
    }
}

static bool open_output(ph_audio *a)
{
    if (a->device)
        return true;
    uint64_t now = ph_metrics_now_ms();
    if (now < a->retry_output_at)
        return false;
    SDL_AudioSpec wanted = {.freq = RATE, .format = AUDIO_F32SYS,
                            .channels = CHANNELS, .samples = 512};
    a->device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
    if (!a->device) {
        PH_LOGW("couldn't open PC audio output: %s", SDL_GetError());
        a->retry_output_at = now + 2000;
        return false;
    }
    SDL_PauseAudioDevice(a->device, 0);
    return true;
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

static struct ph_timed_pcm *pop_timed(ph_audio *a)
{
    struct ph_timed_pcm *node = a->timed_head;

    if (!node)
        return NULL;
    a->timed_head = node->next;
    if (!a->timed_head)
        a->timed_tail = NULL;
    size_t bytes = (size_t)node->samples * CHANNELS * sizeof(float);
    a->timed_bytes = a->timed_bytes >= bytes ? a->timed_bytes - bytes : 0;
    return node;
}

static void drop_timed(ph_audio *a)
{
    struct ph_timed_pcm *node = pop_timed(a);

    if (node) {
        av_free(node->pcm);
        free(node);
    }
}

static void free_timed(ph_audio *a)
{
    while (a->timed_head)
        drop_timed(a);
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
    free_timed(a);
    ph_sync_reset(a->sync);
    a->clock_valid = false;
}

void ph_audio_output_removed(ph_audio *a, uint32_t device)
{
    if (!a || !device)
        return;
    SDL_LockMutex(a->lock);
    if (a->device == device) {
        SDL_CloseAudioDevice(a->device);
        a->device = 0;
        a->retry_output_at = 0;
        flush_locked(a);
        PH_LOGI("PC audio output disconnected; will reopen the default device");
    }
    SDL_UnlockMutex(a->lock);
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

    if (!open_output(a))
        goto fail;
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

static int convert_frame(ph_audio *a, int *count_out)
{
    AVFrame *f = a->frame;
    int capacity;
    uint8_t *pcm;
    int count;

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
    if (capacity <= 0 || capacity > 16384)
        return -1;
    if (a->pcm_capacity < capacity) {
        uint8_t *grown = NULL;
        if (av_samples_alloc(&grown, NULL, CHANNELS, capacity, AV_SAMPLE_FMT_FLT, 0) < 0)
            return -1;
        av_free(a->pcm_scratch);
        a->pcm_scratch = grown;
        a->pcm_capacity = capacity;
    }
    pcm = a->pcm_scratch;
    count = swr_convert(a->resample, &pcm, capacity,
                        (const uint8_t **)f->extended_data, f->nb_samples);
    if (count <= 0) {
        *count_out = count;
        return count < 0 ? -1 : 0;
    }
    *count_out = count;
    return 0;
}

/* Applies the volume in place and queues to the device. */
static int queue_device(ph_audio *a, uint8_t *pcm, int count)
{
    float *values = (float *)pcm;
    float gain = a->gain * (a->compression == 1 ? 1.f : a->airplay_gain);

    if (!a->device)
        return -1;
    for (int i = 0; i < count * CHANNELS; i++)
        values[i] *= gain;
    if (SDL_QueueAudio(a->device, pcm, (Uint32)(count * CHANNELS * sizeof(float))) != 0)
        return -1;
    if (!a->playing) {
        PH_LOGI("audio playing: %s, %d Hz stereo", format_name(a->compression), RATE);
        a->playing = true;
    }
    return count;
}

static int queue_pcm(ph_audio *a, uint8_t *pcm, int count, bool own)
{
    if (count <= 0) {
        if (own)
            av_free(pcm);
        return count;
    }
    if (!a->device) {
        if (own)
            av_free(pcm);
        return -1;
    }
    uint32_t backlog = device_queued_ms_locked(a);
    if (backlog > a->stats.backlog_max)
        a->stats.backlog_max = backlog;
    if (a->playing && backlog >= BACKLOG_MAX_MS) {
        a->stats.trimmed++; /* catch up: the device already has enough to play */
        if (own)
            av_free(pcm);
        return count;
    }
    count = queue_device(a, pcm, count);
    if (own)
        av_free(pcm);
    return count;
}

static int64_t device_queued_us_locked(const ph_audio *a)
{
    if (!a->device)
        return 0;
    return (int64_t)SDL_GetQueuedAudioSize(a->device) * 1000000
           / (int64_t)(RATE * CHANNELS * sizeof(float));
}

static void queue_silence(ph_audio *a, int64_t us)
{
    static const float zeros[1024 * CHANNELS];
    int64_t frames = us * RATE / 1000000;

    while (frames > 0) {
        int n = frames > 1024 ? 1024 : (int)frames;
        if (SDL_QueueAudio(a->device, zeros, (Uint32)(n * CHANNELS * sizeof(float))) != 0)
            return;
        frames -= n;
    }
}

/* Moves due sound into the device queue. Each chunk is heard when the sound
 * already queued has played, plus the output latency. A chunk that would be
 * clearly early waits, with silence before it once it is close; one that would
 * be clearly late is skipped. */
static void release_timed_locked(ph_audio *a, int64_t now)
{
    while (a->timed_head) {
        if (!open_output(a))
            return;
        struct ph_timed_pcm *node = a->timed_head;
        int64_t queued = device_queued_us_locked(a);
        if (queued >= TIMED_FILL_US)
            return;
        int64_t early = node->due_us - (now + queued + OUTPUT_LATENCY_US);
        if (early < -TIMED_LATE_US) {
            a->tstats.late++;
            drop_timed(a);
            continue;
        }
        if (early > 0 && (queued == 0 || early > TIMED_EARLY_US)) {
            if (early > TIMED_FILL_US - queued)
                return;
            queue_silence(a, early);
            a->tstats.gaps++;
            a->tstats.gap_us += early;
        }
        node = pop_timed(a);
        queue_device(a, node->pcm, node->samples);
        a->tstats.chunks++;
        av_free(node->pcm);
        free(node);
    }
}

static int schedule_pcm(ph_audio *a, const uint8_t *pcm, int count, int64_t due_us)
{
    size_t bytes = (size_t)count * CHANNELS * sizeof(float);
    struct ph_timed_pcm *node;

    if (count <= 0)
        return 0;
    while (a->timed_head && a->timed_bytes + bytes > TIMED_MAX_BYTES)
        drop_timed(a);
    node = calloc(1, sizeof *node);
    if (!node)
        return -1;
    node->pcm = av_memdup(pcm, bytes);
    if (!node->pcm) {
        free(node);
        return -1;
    }
    node->samples = count;
    node->due_us = due_us;
    if (a->timed_tail)
        a->timed_tail->next = node;
    else
        a->timed_head = node;
    a->timed_tail = node;
    a->timed_bytes += bytes;
    return count;
}

static void log_timed_stats(ph_audio *a, uint64_t now_ms)
{
    if (!a->tstats.since_ms) {
        a->tstats.since_ms = now_ms;
        return;
    }
    if (now_ms - a->tstats.since_ms < STATS_EVERY_MS)
        return;
    int picture = -1;
    int64_t shown = 0;
    bool known = ph_av_delay_video_ms(&a->av, FASTEST_TRIP_MS, &picture);
    if (a->av.have_applied)
        shown = a->av.applied;
    uint32_t held = (uint32_t)(a->timed_bytes * 1000 / (RATE * CHANNELS * sizeof(float)));
    if (a->tstats.airplay)
        PH_LOGD("audio schedule: AirPlay due in %lld..%lld ms on arrival; picture delay %s%d ms; "
                "waiting %u ms, device %u ms; played %u, late %u, gaps %u (%lld ms)",
                (long long)a->tstats.due_in_min, (long long)a->tstats.due_in_max,
                known ? "" : "unknown ", known ? picture : 0, held,
                device_queued_ms_locked(a), a->tstats.chunks, a->tstats.late, a->tstats.gaps,
                (long long)(a->tstats.gap_us / 1000));
    else
    {
        int64_t lag = 0;
        int64_t sound_us = 0;
        uint64_t probe = a->av.last_capture_ns ? a->av.last_capture_ns : 1000000000ULL;
        if (a->av.have_applied &&
            ph_av_delay_audio_us(&a->av, probe, BROADCAST_AUDIO_MARGIN_MS, &sound_us))
            lag = sound_us / 1000 - (int64_t)(probe / 1000000ULL) - shown;
        PH_LOGD("audio schedule: broadcast sound %lld ms after its picture (picture delay %s%d ms); "
                "loudest %.0f dBFS; waiting %u ms, device %u ms; played %u, late %u, gaps %u (%lld ms)",
                (long long)lag, known ? "" : "unknown ", known ? picture : 0,
                a->tstats.peak > 0 ? 20.0 * log10(a->tstats.peak) : -144.0, held,
                device_queued_ms_locked(a), a->tstats.chunks, a->tstats.late, a->tstats.gaps,
                (long long)(a->tstats.gap_us / 1000));
    }
    memset(&a->tstats, 0, sizeof a->tstats);
    a->tstats.since_ms = now_ms;
}

int ph_audio_push_broadcast(ph_audio *a, const uint8_t *pcm, size_t len)
{
    return ph_audio_push_broadcast_timed(a, pcm, len, 0);
}

int ph_audio_push_broadcast_timed(ph_audio *a, const uint8_t *pcm, size_t len, uint64_t pts_ns)
{
    if (!a || !pcm || !len || len > 8192 || len % 4)
        return -1;
    int frames = (int)(len / 4);
    float samples[4096];
    for (int i = 0; i < frames * CHANNELS; i++) {
        int value = pcm[2 * i] | (int)pcm[2 * i + 1] << 8;
        samples[i] = (value < 32768 ? value : value - 65536) / 32768.0f;
    }
    float peak = 0;
    for (int i = 0; i < frames * CHANNELS; i++)
        peak = fmaxf(peak, fabsf(samples[i]));
    SDL_LockMutex(a->lock);
    if (a->muted) {
        SDL_UnlockMutex(a->lock);
        return 0;
    }
    if (a->compression != 1) {
        flush_locked(a);
        avcodec_free_context(&a->codec);
        a->compression = 1;
        a->warned = false;
    }
    bool had_output = a->device != 0;
    if (!open_output(a)) {
        a->warned = true;
        SDL_UnlockMutex(a->lock);
        return -1;
    }
    if (!had_output || !a->playing)
        PH_LOGI("audio ready: broadcast PCM, PC speakers");
    int result;
    if (!pts_ns) {
        result = queue_pcm(a, (uint8_t *)samples, frames, false);
    } else {
        /* Heard when a frame captured at the same moment is drawn, or a little
         * later when the sound itself arrives later than that. */
        int64_t now = (int64_t)ph_metrics_now_us();
        int64_t due;
        ph_av_delay_audio_arrived(&a->av, pts_ns, (uint64_t)now / 1000);
        if (!ph_av_delay_audio_us(&a->av, pts_ns, BROADCAST_AUDIO_MARGIN_MS, &due))
            due = now;
        a->tstats.peak = fmaxf(a->tstats.peak, peak);
        due += (int64_t)a->sync_offset_ms * 1000;
        result = schedule_pcm(a, (const uint8_t *)samples, frames, due);
        release_timed_locked(a, now);
        a->tstats.airplay = false;
        log_timed_stats(a, (uint64_t)now / 1000);
    }
    SDL_UnlockMutex(a->lock);
    return result;
}

int ph_audio_push_scheduled(ph_audio *a, unsigned char compression, const uint8_t *data, size_t len,
                            int64_t due_in_us)
{
    int result = -1;
    int error;

    if (!a || !data || !len || len > 1024 * 1024)
        return -1;
    SDL_LockMutex(a->lock);
    int64_t now = (int64_t)ph_metrics_now_us();
    bool negotiated = compression == a->airplay_compression && a->airplay_samples;
    unsigned short packet_samples = negotiated ? a->airplay_samples
                            : compression == 2 ? 352 : compression == 8 ? 480 : 1024;
    if ((!a->codec || compression != a->compression || (negotiated && packet_samples != a->samples))
        && !configure_locked(a, compression, packet_samples))
        goto done;
    if (!open_output(a))
        goto done;
    /* The iPhone delays its own picture until this sound is due. With its
     * screen shown here, the sound waits for that picture to arrive too. */
    int64_t due = now;
    if (due_in_us >= -1000000 && due_in_us <= 3500000) {
        due += due_in_us;
        int picture;
        if (ph_av_delay_video_ms(&a->av, FASTEST_TRIP_MS, &picture))
            due += (int64_t)picture * 1000;
        int64_t due_ms = due_in_us / 1000;
        if (!a->tstats.airplay || due_ms < a->tstats.due_in_min)
            a->tstats.due_in_min = due_ms;
        if (!a->tstats.airplay || due_ms > a->tstats.due_in_max)
            a->tstats.due_in_max = due_ms;
        a->tstats.airplay = true;
    } else if (!a->unscheduled_warned) {
        PH_LOGW("AirPlay audio has no usable play time (%lld ms); playing it on arrival",
                (long long)(due_in_us / 1000));
        a->unscheduled_warned = true;
    }
    due += (int64_t)a->sync_offset_ms * 1000;
    if (av_new_packet(a->packet, (int)len) < 0)
        goto done;
    memcpy(a->packet->data, data, len);
    error = avcodec_send_packet(a->codec, a->packet);
    av_packet_unref(a->packet);
    if (error < 0)
        goto done;
    result = 0;
    while ((error = avcodec_receive_frame(a->codec, a->frame)) == 0) {
        int samples = 0;
        int conv = convert_frame(a, &samples);
        av_frame_unref(a->frame);
        if (conv < 0) {
            result = -1;
            goto done;
        }
        if (samples <= 0)
            continue;
        if (!a->muted && schedule_pcm(a, a->pcm_scratch, samples, due) < 0) {
            result = -1;
            goto done;
        }
        due += (int64_t)samples * 1000000 / RATE;
        result += samples;
    }
    if (error != AVERROR(EAGAIN) && error != AVERROR_EOF)
        result = -1;
    release_timed_locked(a, now);
    log_timed_stats(a, (uint64_t)now / 1000);
done:
    if (result < 0 && !a->warned) {
        PH_LOGW("audio packet could not be played (%s)", format_name(compression));
        a->warned = true;
    }
    SDL_UnlockMutex(a->lock);
    return result;
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
    a->airplay_gain = 1;
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

void ph_audio_set_airplay_format(ph_audio *a, unsigned char compression, unsigned short samples)
{
    if (!a)
        return;
    SDL_LockMutex(a->lock);
    a->airplay_compression = compression;
    a->airplay_samples = samples;
    SDL_UnlockMutex(a->lock);
}

void ph_audio_set_airplay_volume(ph_audio *a, float decibels)
{
    if (!a || !isfinite(decibels))
        return;
    SDL_LockMutex(a->lock);
    a->airplay_gain = decibels <= -144.f ? 0.f : powf(10.f, fminf(0.f, decibels) / 20.f);
    SDL_UnlockMutex(a->lock);
}

void ph_audio_flush_airplay(ph_audio *a)
{
    if (!a)
        return;
    SDL_LockMutex(a->lock);
    if (a->compression != 1)
        flush_locked(a);
    SDL_UnlockMutex(a->lock);
}

static int push_locked(ph_audio *a, unsigned char compression, const uint8_t *data, size_t len,
                       bool timed, uint64_t pts_ns)
{
    int result = -1;
    ph_sync_action action = {PH_SYNC_PLAY, 0, 0, false};
    int error;

    bool negotiated = compression == a->airplay_compression && a->airplay_samples;
    unsigned short packet_samples = negotiated ? a->airplay_samples
                            : compression == 2 ? 352 : compression == 8 ? 480 : 1024;
    if ((!a->codec || compression != a->compression || (negotiated && packet_samples != a->samples))
        && !configure_locked(a, compression, packet_samples))
        goto done;
    if (!open_output(a))
        goto done;
    if (timed) {
        action = ph_sync_audio_at(a->sync, pts_ns, 0, ph_metrics_now_ms());
        if (action.kind == PH_SYNC_DROP) {
            result = 0;
            goto done;
        }
        if (action.have_lead) {
            if (!a->stats.leads || action.lead_ms < a->stats.lead_min)
                a->stats.lead_min = action.lead_ms;
            if (!a->stats.leads || action.lead_ms > a->stats.lead_max)
                a->stats.lead_max = action.lead_ms;
            a->stats.lead_sum += action.lead_ms;
            a->stats.leads++;
        }
        if (action.kind == PH_SYNC_HOLD) {
            /* Audio already queued for the device plays first: hold only the rest. */
            int net = action.hold_or_drop_ms - (int)device_queued_ms_locked(a);
            if (net <= 10)
                action.kind = PH_SYNC_PLAY;
            else
                action.hold_or_drop_ms = net;
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
        int samples = 0;
        int conv = convert_frame(a, &samples);
        av_frame_unref(a->frame);
        if (conv < 0) {
            result = -1;
            goto done;
        }
        if (samples <= 0)
            continue;
        if (!a->muted && (a->hold_head || (timed && action.kind == PH_SYNC_HOLD))) {
            int delay = timed && action.kind == PH_SYNC_HOLD ? action.hold_or_drop_ms : 0;
            size_t bytes = (size_t)samples * CHANNELS * sizeof(float);
            uint8_t *copy = av_malloc(bytes);
            if (!copy) {
                result = -1;
                goto done;
            }
            memcpy(copy, a->pcm_scratch, bytes);
            if (hold_pcm(a, copy, samples, delay) < 0) {
                result = -1;
                goto done;
            }
            a->stats.held++;
        } else if (!a->muted) {
            if (queue_pcm(a, a->pcm_scratch, samples, false) < 0) {
                result = -1;
                goto done;
            }
            a->stats.played++;
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
    uint64_t now = ph_metrics_now_ms();
    if (!a->stats.since_ms)
        a->stats.since_ms = now;
    else if (now - a->stats.since_ms >= STATS_EVERY_MS) {
        uint32_t held_ms = (uint32_t)(a->hold_bytes * 1000 / (RATE * CHANNELS * sizeof(float)));
        if (a->stats.leads)
            PH_LOGD("audio: lead vs picture avg %lld ms (min %d, max %d); queued %u ms "
                    "(max %u), held %u ms; played %u, held %u, trimmed %u",
                    (long long)(a->stats.lead_sum / a->stats.leads), a->stats.lead_min,
                    a->stats.lead_max, device_queued_ms_locked(a), a->stats.backlog_max,
                    held_ms, a->stats.played, a->stats.held, a->stats.trimmed);
        else
            PH_LOGD("audio: no video timing; queued %u ms (max %u); played %u, trimmed %u",
                    device_queued_ms_locked(a), a->stats.backlog_max, a->stats.played,
                    a->stats.trimmed);
        memset(&a->stats, 0, sizeof a->stats);
        a->stats.since_ms = now;
    }
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

void ph_audio_note_broadcast_video_received(ph_audio *a, uint64_t pts_ns)
{
    if (!a || !pts_ns)
        return;
    SDL_LockMutex(a->lock);
    ph_av_delay_arrived(&a->av, pts_ns, ph_metrics_now_ms());
    /* A long gap between capture times while the screen moves means the
     * iPhone skipped frames; it sends nothing while the screen is still. */
    if (a->vstats.last_capture_ns && pts_ns > a->vstats.last_capture_ns) {
        int64_t gap = (int64_t)((pts_ns - a->vstats.last_capture_ns) / 1000000ULL);
        if (gap > a->vstats.capture_gap_max)
            a->vstats.capture_gap_max = gap;
    }
    a->vstats.last_capture_ns = pts_ns;
    a->vstats.arrived++;
    SDL_UnlockMutex(a->lock);
}

void ph_audio_note_broadcast_video(ph_audio *a, uint64_t pts_ns)
{
    if (!a)
        return;
    SDL_LockMutex(a->lock);
    if (!pts_ns) {
        ph_av_delay_reset(&a->av);
        memset(&a->vstats, 0, sizeof a->vstats);
    } else {
        bool had = a->av.have_applied;
        int64_t before = a->av.applied;
        uint64_t now_ms = ph_metrics_now_ms();
        ph_av_delay_presented(&a->av, pts_ns, now_ms);
        /* How far this frame was drawn behind the quickest frame's pace. */
        int fastest = -1;
        if (ph_av_delay_video_ms(&a->av, 0, &fastest) && a->av.count) {
            int64_t last = a->av.shown[(a->av.next + PH_AV_DELAY_FRAMES - 1) % PH_AV_DELAY_FRAMES];
            int64_t behind = last - a->av.applied + fastest;
            if (behind > a->vstats.behind_max)
                a->vstats.behind_max = behind;
        }
        a->vstats.drawn++;
        if (!a->vstats.since_ms) {
            a->vstats.since_ms = now_ms;
        } else if (now_ms - a->vstats.since_ms >= STATS_EVERY_MS) {
            PH_LOGD("broadcast video: %u frames arrived, %u drawn in %llu ms; longest capture "
                    "gap %lld ms; slowest capture-to-display %lld ms beyond the quickest trip",
                    a->vstats.arrived, a->vstats.drawn,
                    (unsigned long long)(now_ms - a->vstats.since_ms),
                    (long long)a->vstats.capture_gap_max, (long long)a->vstats.behind_max);
            if (a->vstats.behind_max > a->vstats_worst_behind)
                a->vstats_worst_behind = a->vstats.behind_max;
            if (a->vstats.capture_gap_max > a->vstats_worst_gap)
                a->vstats_worst_gap = a->vstats.capture_gap_max;
            if (++a->vstats_windows % 5 == 0) {
                PH_LOGI("screen health (10 s): %u frames drawn in the last 2 s; worst capture gap %lld ms; "
                        "worst capture-to-display %lld ms beyond the quickest trip",
                        a->vstats.drawn, (long long)a->vstats_worst_gap, (long long)a->vstats_worst_behind);
                a->vstats_worst_behind = a->vstats_worst_gap = 0;
            }
            uint64_t keep = a->vstats.last_capture_ns;
            memset(&a->vstats, 0, sizeof a->vstats);
            a->vstats.last_capture_ns = keep;
            a->vstats.since_ms = now_ms;
        }
        int picture;
        if ((!had || a->av.applied != before) &&
            ph_av_delay_video_ms(&a->av, FASTEST_TRIP_MS, &picture))
            PH_LOGD("audio schedule: shared screen picture delay now %d ms", picture);
    }
    SDL_UnlockMutex(a->lock);
}

void ph_audio_note_video_pts(ph_audio *a, uint64_t pts_ns)
{
    if (!a)
        return;
    SDL_LockMutex(a->lock);
    ph_sync_note_video_at(a->sync, pts_ns, ph_metrics_now_ms());
    SDL_UnlockMutex(a->lock);
}

void ph_audio_pump(ph_audio *a, uint64_t now_ms)
{
    uint64_t prev = 0;
    bool have_prev = false;

    if (!a)
        return;
    SDL_LockMutex(a->lock);
    release_timed_locked(a, (int64_t)ph_metrics_now_us());
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
            queue_pcm(a, node->pcm, node->samples, true);
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
    if (muted && !a->muted) {
        if (a->device)
            SDL_ClearQueuedAudio(a->device);
        free_holds(a);
        free_timed(a);
        a->playing = false;
    }
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
    held = (uint32_t)((a->hold_bytes + a->timed_bytes) * 1000U / (RATE * CHANNELS * sizeof(float)));
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
    free_timed(a);
    if (a->device)
        SDL_CloseAudioDevice(a->device);
    swr_free(&a->resample);
    av_free(a->pcm_scratch);
    avcodec_free_context(&a->codec);
    av_frame_free(&a->frame);
    av_packet_free(&a->packet);
    ph_sync_destroy(a->sync);
    if (a->lock)
        SDL_DestroyMutex(a->lock);
    free(a);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
