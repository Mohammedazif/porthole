#include "airplay/receiver.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/rand.h>

#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"

#include "core/config.h"
#include "core/log.h"

#define DEVICE_ID_FILE "airplay-device-id"
#define KEY_FILE "airplay-key.pem"

struct ph_receiver {
    raop_t *raop;
    dnssd_t *dnssd;
    ph_receiver_callbacks cb;
    atomic_int open_connections;
    atomic_bool reset_requested;
};

/* ------------------------------------------------------------------ device identity */

/* The iPhone identifies a receiver by a MAC-style device ID; keep it stable across runs.
 * A random, locally administered address avoids per-platform MAC lookups. */
static bool load_device_id(char out[18], unsigned char hw_addr[6])
{
    unsigned int b[6];

    if (!ph_config_read_line(DEVICE_ID_FILE, out, 18) ||
        sscanf(out, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        unsigned char rnd[6];
        if (RAND_bytes(rnd, sizeof rnd) != 1)
            return false;
        rnd[0] = (unsigned char)((rnd[0] & ~0x01) | 0x02); /* unicast, locally administered */
        snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", rnd[0], rnd[1], rnd[2], rnd[3],
                 rnd[4], rnd[5]);
        if (!ph_config_write_line(DEVICE_ID_FILE, out))
            PH_LOGW("couldn't save the AirPlay device ID; the iPhone may see a new receiver "
                    "next time");
        for (int i = 0; i < 6; i++)
            b[i] = rnd[i];
    }
    for (int i = 0; i < 6; i++)
        hw_addr[i] = (unsigned char)b[i];
    return true;
}

/* ------------------------------------------------------------------ UxPlay callbacks */

static ph_receiver *self(void *cls)
{
    return (ph_receiver *)cls;
}

static void cb_conn_init(void *cls)
{
    atomic_fetch_add(&self(cls)->open_connections, 1);
}

static void cb_conn_destroy(void *cls)
{
    ph_receiver *r = self(cls);
    if (atomic_fetch_sub(&r->open_connections, 1) == 1 && r->cb.disconnected)
        r->cb.disconnected(r->cb.user);
}

static void cb_conn_reset(void *cls, int reason)
{
    ph_receiver *r = self(cls);
    PH_LOGW("mirroring connection lost (reason %d); restarting the receiver", reason);
    atomic_store(&r->reset_requested, true);
    if (r->cb.disconnected)
        r->cb.disconnected(r->cb.user);
}

static void cb_conn_feedback(void *cls)
{
    (void)cls; /* client heartbeat */
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data)
{
    ph_receiver *r = self(cls);
    (void)ntp;
    if (r->cb.video && data->data_len > 0)
        r->cb.video(r->cb.user, data->data, (size_t)data->data_len, data->ntp_time_remote);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data)
{
    ph_receiver *r = self(cls);
    (void)ntp;
    if (r->cb.audio && data->data_len > 0)
        r->cb.audio(r->cb.user, data->ct, data->data, (size_t)data->data_len, data->ntp_time_remote);
}

static void cb_audio_flush(void *cls)
{
    ph_receiver *r = self(cls);
    if (r->cb.audio_flush)
        r->cb.audio_flush(r->cb.user);
}

static void cb_noop(void *cls)
{
    (void)cls;
}

static void cb_video_reset(void *cls, reset_type_t type)
{
    ph_receiver *r = self(cls);
    PH_LOGD("video reset (type %d)", (int)type);
    if ((type == RESET_TYPE_RTP_SHUTDOWN || type == RESET_TYPE_NOHOLD) && r->cb.disconnected)
        r->cb.disconnected(r->cb.user);
}

static int cb_video_set_codec(void *cls, video_codec_t codec)
{
    (void)cls;
    if (codec == VIDEO_CODEC_H264 || codec == VIDEO_CODEC_H265)
        return 0;
    PH_LOGE("the iPhone offered a video codec other than H.264 or H.265; not supported yet");
    return -1;
}

static double cb_audio_set_client_volume(void *cls)
{
    (void)cls;
    return 0.0; /* AirPlay volume in dB: 0 = full */
}

static void cb_audio_set_volume(void *cls, float volume)
{
    ph_receiver *r = self(cls);
    if (r->cb.audio_volume)
        r->cb.audio_volume(r->cb.user, volume);
}

static void cb_audio_buffer(void *cls, const void *buffer, int buflen)
{
    (void)cls, (void)buffer, (void)buflen;
}

static void cb_audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end)
{
    (void)cls, (void)start, (void)curr, (void)end;
}

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf,
                                bool *using_screen, bool *is_media, uint64_t *audio_format)
{
    ph_receiver *r = self(cls);
    (void)using_screen, (void)is_media, (void)audio_format;
    if (r->cb.audio_format)
        r->cb.audio_format(r->cb.user, *ct, *spf);
}

static void cb_video_report_size(void *cls, float *width_source, float *height_source,
                                 float *width, float *height)
{
    (void)cls;
    PH_LOGI("iPhone screen %.0fx%.0f, streamed at %.0fx%.0f", *width_source, *height_source,
            *width, *height);
}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name,
                                     bool *admit)
{
    ph_receiver *r = self(cls);
    PH_LOGI("connection from \"%s\" (%s)", name ? name : "?", model ? model : "?");
    PH_LOGD("client device ID %s", deviceid ? deviceid : "?");
    *admit = !r->cb.admit || r->cb.admit(r->cb.user, deviceid ? deviceid : "",
                                        name ? name : "", model ? model : "");
    if (*admit && r->cb.connected)
        r->cb.connected(r->cb.user, name ? name : "", model ? model : "");
}

static void cb_display_pin(void *cls, char *pin)
{
    (void)cls;
    PH_LOGI("AirPlay PIN: %s", pin);
}

static void cb_register_client(void *cls, const char *device_id, const char *pk,
                               const char *name)
{
    (void)cls, (void)device_id, (void)pk, (void)name;
}

static bool cb_check_register(void *cls, const char *pk)
{
    (void)cls, (void)pk;
    return true;
}

static const char *cb_passwd(void *cls, int *len)
{
    (void)cls;
    *len = 0; /* no password */
    return NULL;
}

static void cb_export_dacp(void *cls, const char *active_remote, const char *dacp_id)
{
    (void)cls, (void)active_remote, (void)dacp_id;
}

/* HLS (AirPlay video URLs) isn't advertised, so these are never used. */
static void cb_on_video_play(void *cls, const char *location, const float start)
{
    (void)cls, (void)location, (void)start;
}

static void cb_on_video_float(void *cls, const float value)
{
    (void)cls, (void)value;
}

static void cb_on_video_info(void *cls, playback_info_t *info)
{
    (void)cls, (void)info;
}

static float cb_on_video_playlist_remove(void *cls)
{
    (void)cls;
    return 0.0f;
}

static void cb_log(void *cls, int level, const char *msg)
{
    char line[512];
    size_t n;
    ph_log_level lvl;

    (void)cls;
    snprintf(line, sizeof line, "%s", msg);
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (level <= LOGGER_ERR)
        lvl = PH_LOG_ERROR;
    else if (level == LOGGER_WARNING)
        lvl = PH_LOG_WARN;
    else
        lvl = PH_LOG_DEBUG; /* UxPlay's info messages are protocol detail */
    ph_log(lvl, "airplay: %s", line);
}

/* ------------------------------------------------------------------ setup */

/* Same feature bits as UxPlay's front end (mirroring + audio, no HLS, H.264 only).
 * Clearing Photo (1) and Slideshow (5) was tried on iOS 26.6.1: Photos still replaced
 * the mirrored screen with the bare photo, so they stay as UxPlay sets them. */
static void set_features(dnssd_t *d)
{
    static const int on[] = {1, 2, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                             18, 19, 20, 21, 22, 25, 27, 28, 30};
    static const int off[] = {0, 3, 4, 8, 23, 24, 26, 29, 31};

    for (size_t i = 0; i < sizeof on / sizeof on[0]; i++)
        dnssd_set_airplay_features(d, on[i], 1);
    for (size_t i = 0; i < sizeof off / sizeof off[0]; i++)
        dnssd_set_airplay_features(d, off[i], 0);
}

static int start_httpd(ph_receiver *r)
{
    unsigned short port = raop_get_port(r->raop);
    if (raop_start_httpd(r->raop, &port) < 0)
        return -1;
    raop_set_port(r->raop, port);
    return port;
}

ph_receiver *ph_receiver_start(const ph_receiver_options *opt, const ph_receiver_callbacks *cb)
{
    raop_callbacks_t cbs;
    char device_id[18], key_path[1100];
    unsigned char hw_addr[6];
    int err = 0, port;
    ph_receiver *r = calloc(1, sizeof *r);

    if (!r)
        return NULL;
    r->cb = *cb;

    if (!load_device_id(device_id, hw_addr)) {
        PH_LOGE("couldn't create an AirPlay device ID");
        goto fail;
    }
    if (!ph_config_path(KEY_FILE, key_path, sizeof key_path))
        key_path[0] = '\0'; /* no persistence: a new pairing key each run */

    memset(&cbs, 0, sizeof cbs);
    cbs.cls = r;
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.video_pause = cb_noop;
    cbs.video_resume = cb_noop;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_noop;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec = cb_video_set_codec;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_set_metadata = cb_audio_buffer;
    cbs.audio_set_coverart = cb_audio_buffer;
    cbs.audio_stop_coverart_rendering = cb_noop;
    cbs.audio_set_progress = cb_audio_set_progress;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.video_report_size = cb_video_report_size;
    cbs.report_client_request = cb_report_client_request;
    cbs.display_pin = cb_display_pin;
    cbs.register_client = cb_register_client;
    cbs.check_register = cb_check_register;
    cbs.passwd = cb_passwd;
    cbs.export_dacp = cb_export_dacp;
    cbs.on_video_play = cb_on_video_play;
    cbs.on_video_scrub = cb_on_video_float;
    cbs.on_video_rate = cb_on_video_float;
    cbs.on_video_stop = cb_noop;
    cbs.on_video_acquire_playback_info = cb_on_video_info;
    cbs.on_video_playlist_remove = cb_on_video_playlist_remove;

#if defined(__linux__)
    /* Avahi's Bonjour compatibility layer prints a warning to stderr unless told not to. */
    setenv("AVAHI_COMPAT_NOWARN", "1", 0);
#endif
    r->dnssd = dnssd_init(opt->name, (int)strlen(opt->name), (const char *)hw_addr, 6, &err, 0);
    if (err || !r->dnssd) {
        PH_LOGE("couldn't start mDNS (error %d). Is avahi-daemon running?", err);
        goto fail;
    }
    set_features(r->dnssd);

    r->raop = raop_init(&cbs);
    if (!r->raop) {
        PH_LOGE("couldn't initialise the AirPlay receiver");
        goto fail;
    }
    raop_set_log_callback(r->raop, cb_log, NULL);
    raop_set_log_level(r->raop, opt->debug ? LOGGER_DEBUG : LOGGER_INFO);
    if (raop_init2(r->raop, 1, device_id, key_path)) {
        PH_LOGE("couldn't initialise the AirPlay receiver (pairing keys)");
        goto fail;
    }
    raop_set_plist(r->raop, "width", opt->width ? opt->width : 1920);
    raop_set_plist(r->raop, "height", opt->height ? opt->height : 1080);
    raop_set_plist(r->raop, "refreshRate", 60);
    raop_set_plist(r->raop, "maxFPS", opt->max_fps ? opt->max_fps : 60);
    raop_set_plist(r->raop, "overscanned", 0);

    if (opt->fixed_ports) {
        unsigned short tcp[2] = {7100, 7000}; /* mirror data, control */
        unsigned short udp[3] = {7011, 6001, 6000}; /* timing, control, audio data */
        raop_set_tcp_ports(r->raop, tcp);
        raop_set_udp_ports(r->raop, udp);
    }
    port = start_httpd(r);
    if (port < 0) {
        PH_LOGE("couldn't open the AirPlay port (is another receiver such as uxplay running?)");
        goto fail;
    }
    raop_set_dnssd(r->raop, r->dnssd);
    if ((err = dnssd_register_raop(r->dnssd, (unsigned short)port)) != 0 ||
        (err = dnssd_register_airplay(r->dnssd, (unsigned short)port)) != 0) {
        PH_LOGE("couldn't advertise the receiver over mDNS (error %d)", err);
        goto fail;
    }
    PH_LOGI("receiver \"%s\" ready on port %d (device ID %s)", opt->name, port, device_id);
    return r;

fail:
    ph_receiver_stop(r);
    return NULL;
}

void ph_receiver_poll(ph_receiver *r)
{
    if (!r || !atomic_exchange(&r->reset_requested, false))
        return;
    raop_stop_httpd(r->raop);
    if (start_httpd(r) < 0)
        PH_LOGE("couldn't restart the AirPlay receiver after a dropped connection");
}

void ph_receiver_stop(ph_receiver *r)
{
    if (!r)
        return;
    if (r->dnssd) {
        dnssd_unregister_raop(r->dnssd);
        dnssd_unregister_airplay(r->dnssd);
    }
    if (r->raop)
        raop_destroy(r->raop);
    if (r->dnssd)
        dnssd_destroy(r->dnssd);
    free(r);
}
