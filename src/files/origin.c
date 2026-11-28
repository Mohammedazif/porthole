#include "files/origin.h"

#include "core/host.h"
#include "core/config.h"
#include "core/log.h"
#include "core/mdns_name.h"
#include "core/metrics.h"
#include "files/clip.h"
#include "files/digest.h"
#include "files/picture_hash.h"
#include "files/pull.h"
#include "files/paired_phones.h"
#include "session/devices.h"

#include <ctype.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>

#include "core/sock.h"
#include "core/secure_link.h"
#include "core/upath.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#endif

#define PH_PUSH_QUEUE 16

struct transfer_result {
    struct transfer_result *next;
    ph_origin_send_state state;
    ph_origin_send send;
};

struct ph_origin {
    int listen_fd;
    int client_fd;
    pthread_t thread;
    pthread_t send_thread; /* answers Send to on 127.0.0.1 even mid-transfer */
    bool send_thread_started;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    atomic_bool stop;
    bool have_client, disconnect_requested;
    bool channels_paused;
    bool screen_live;
    unsigned pairing_generation, client_generation;
    time_t code_expiry;
    struct {
        char address[64];
        unsigned attempts;
        time_t since;
    } failures[16];
    ph_control_policy control;
    /* Where the phone's sound plays while sharing (ph_sound_mode), chosen on
     * either device: audio_push sends the PC's choice to the phone once it says
     * it understands SOUND, and the phone's own choices bump audio_serial for
     * the app to apply. */
    int phone_audio;
    bool audio_capable, audio_push;
    unsigned audio_serial;
    /* Volume button presses on the phone while its sound plays here, not yet
     * applied by the app: positive is louder. */
    int volume_steps;
    char verification_id[37], verification_code[17];
    uint64_t verification_deadline;
    bool verification_requested, controls_verified, verification_reset;
    bool ask;
    uint64_t hash;
    double aspect;
    uint8_t thumb[PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    bool have_thumb;
    uint8_t detail[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL];
    bool have_detail;
    ph_origin_screen screen;
    bool have_screen;
    unsigned ask_id, canceled_pull;
    char lan_ip[64];
    unsigned ready_id;
    ph_origin_result ready;
    char dir[512];
    char path[768];
    char token[16], reconnect_key[33], channel_ticket[65];
    char screen_key[65], share_key[65];
    ph_paired_phones phones;
    int push_fd;
    ph_origin_send push_queue[PH_PUSH_QUEUE]; /* files waiting, oldest first */
    char active_push_key[80];
    char active_push_id[33]; /* the file going to the iPhone now */
    int push_count;
    bool push_cancel;
    bool push_stopped_by_phone; /* the person cancelled it on the iPhone */
    struct transfer_result *results, *last_result;
    bool files_ok, resumable, chunked;
    bool push_busy;
    char push_busy_name[96];
    unsigned long long push_size, push_done;
    char peer[64]; /* address of the connection being served */
    char known[64]; /* last phone that gave the right code */
    bool code_open; /* false after the pairing key has been committed */
    char phone_name[64];
    bool clip_ready;
    bool ask_clip;     /* send CLIP? once the phone has enabled clipboard sync */
    bool clip_wait;    /* Ctrl+C is waiting for that answer */
    double ask_clip_ms;
    bool out_pending;
    char out_kind;
    uint8_t *out_data;
    size_t out_len;
    bool in_ready;
    char in_kind;
    uint8_t *in_data;
    size_t in_len;
    bool note_sent, note_got;
};

static bool make_token(char *out, size_t len)
{
    unsigned char bytes[4];
    if (RAND_bytes(bytes, sizeof bytes) != 1)
        return false;
    return snprintf(out, len, "%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3]) == 8;
}

static void fill_hint(char *hint, size_t hint_len, const char *ip, const char *token)
{
    if (hint && hint_len)
        snprintf(hint, hint_len, "%s:%d  %s", ip, PH_PULL_PORT, token);
}

#ifdef _WIN32
/* The IPv4 address of the first connected adapter with a default gateway: the Wi-Fi
 * or Ethernet the iPhone shares, not a VPN or Hyper-V switch without one. */
static void lan_address(char *out, size_t len)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL, *ad;
    char fallback[64] = "";
    ULONG result = ERROR_BUFFER_OVERFLOW;

    snprintf(out, len, "this-pc");
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = malloc(size);
        if (!list)
            return;
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST
                                                   | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      NULL, list, &size);
    }
    if (result != NO_ERROR) {
        free(list);
        return;
    }
    for (ad = list; ad; ad = ad->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (u = ad->FirstUnicastAddress; u; u = u->Next) {
            struct sockaddr_in *in = (struct sockaddr_in *)u->Address.lpSockaddr;
            char ip[64];
            if (!in || in->sin_family != AF_INET || !inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip))
                continue;
            if (!strncmp(ip, "169.254.", 8))
                continue;
            if (ad->FirstGatewayAddress) {
                snprintf(out, len, "%s", ip);
                free(list);
                return;
            }
            if (!fallback[0])
                snprintf(fallback, sizeof fallback, "%s", ip);
        }
    }
    if (fallback[0])
        snprintf(out, len, "%s", fallback);
    free(list);
}
#else
static void lan_address(char *out, size_t len)
{
    struct ifaddrs *list = NULL, *ifa;

    snprintf(out, len, "this-pc");
    if (getifaddrs(&list) != 0)
        return;
    for (ifa = list; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in *in;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        in = (struct sockaddr_in *)ifa->ifa_addr;
        if (in->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
            continue;
        if (!inet_ntop(AF_INET, &in->sin_addr, out, (socklen_t)len))
            continue;
        break;
    }
    freeifaddrs(list);
}
#endif

static bool read_line_wait(int fd, char *buf, size_t len, uint64_t wait_ms)
{
    size_t n = 0;

    if (len < 2)
        return false;
    uint64_t deadline = ph_metrics_now_ms() + wait_ms;
    while (n + 1 < len) {
        char c;
        int got = ph_tls_read_until(fd, &c, 1, deadline);

        if (got <= 0)
            return false;
        if (n == 0)
            deadline = ph_metrics_now_ms() + 5000;
        if (c == '\n') {
            buf[n] = 0;
            if (n && buf[n - 1] == '\r')
                buf[n - 1] = 0;
            return true;
        }
        if (c == 0)
            return false;
        buf[n++] = c;
    }
    return false;
}

static bool read_line(int fd, char *buf, size_t len)
{
    return read_line_wait(fd, buf, len, 5000);
}

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;

    while (len) {
        int n = (int)ph_tls_write(fd, p, (int)(len > (1u << 20) ? (1u << 20) : len));

        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool pull_available(ph_origin *o, unsigned id)
{
    pthread_mutex_lock(&o->lock);
    bool ok = o->have_client && o->client_generation == o->pairing_generation &&
              o->canceled_pull != id;
    pthread_mutex_unlock(&o->lock);
    return ok;
}

static bool save_body(ph_origin *o, unsigned id, int fd, const char *dir, const char *name, unsigned long long size,
                      char *path, size_t path_len)
{
    char part[820];
    FILE *out;
    unsigned long long left = size;
    bool store = pull_available(o, id);

    if (snprintf(path, path_len, "%s/%s", dir, name) >= (int)path_len)
        return false;
    for (unsigned suffix = 2; ph_path_exists(path); suffix++) {
        if (suffix > 10000 ||
            snprintf(path, path_len, "%s/%u-%s", dir, suffix, name) >= (int)path_len)
            return false;
    }
    unsigned char random[8];
    if (RAND_bytes(random, sizeof random) != 1 ||
        snprintf(part, sizeof part, "%s.%02x%02x%02x%02x%02x%02x%02x%02x.part", path, random[0],
                 random[1], random[2], random[3], random[4], random[5], random[6],
                 random[7]) >= (int)sizeof part)
        return false;
    out = store ? ph_fopen(part, "wbx") : NULL;
    if (store && !out)
        return false;
    while (left) {
        char chunk[65536];
        size_t want = left > sizeof chunk ? sizeof chunk : (size_t)left;
        int got = (int)ph_tls_read(fd, chunk, (int)want);

        if (got <= 0)
            break;
        store = store && pull_available(o, id);
        if (store && fwrite(chunk, 1, (size_t)got, out) != (size_t)got)
            break;
        left -= (unsigned long long)got;
    }
    if (!out)
        return false;
    bool ok = !left && store && ph_flush_file(out);
    if (fclose(out) != 0)
        ok = false;
    pthread_mutex_lock(&o->lock);
    ok = ok && o->have_client && id != o->canceled_pull &&
         o->client_generation == o->pairing_generation && ph_publish_new(part, path);
    pthread_mutex_unlock(&o->lock);
    if (!ok) {
        ph_remove(part);
        return false;
    }
    return true;
}

static bool read_exact(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len) {
        int n = (int)ph_tls_read(fd, p, (int)(len > (1u << 20) ? (1u << 20) : len));

        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* SYNC and CLIP ride the photo link. *consumed is set when this function
 * took the line (and, for CLIP, the bytes after it). */
static bool request_id(const char *s)
{
    if (strlen(s) != 36)
        return false;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return false;
        } else if (!isxdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

static void hex_string(const unsigned char *bytes, size_t n, char *out);

static void derive_channel_key(const ph_paired_phone *phone, const char *purpose, char out[65])
{
    OPENSSL_cleanse(out, 65);
    if (phone) {
        unsigned char digest[32];
        unsigned int size = 0;
        if (HMAC(EVP_sha256(), phone->key, 32, (const unsigned char *)purpose, strlen(purpose),
                 digest, &size) && size == sizeof digest)
            hex_string(digest, sizeof digest, out);
        OPENSSL_cleanse(digest, sizeof digest);
    }
}

static void use_selected_phone(ph_origin *o)
{
    const ph_paired_phone *phone = ph_paired_phones_selected(&o->phones);
    snprintf(o->reconnect_key, sizeof o->reconnect_key, "%s", phone ? phone->key : "");
    derive_channel_key(phone, "Porthole screen broadcast v1", o->screen_key);
    derive_channel_key(phone, "Porthole file inbox v1", o->share_key);
    if (phone) {
        snprintf(o->token, sizeof o->token, "%s", phone->seed);
        snprintf(o->phone_name, sizeof o->phone_name, "%s",
                 phone->alias[0] ? phone->alias : phone->remote);
    } else {
        o->phone_name[0] = 0;
    }
}

static bool save_phone_key(ph_origin *o, const char *key)
{
    const ph_paired_phone *current = ph_paired_phones_selected(&o->phones);
    if (current)
        return !CRYPTO_memcmp(current->key, key, 32);
    if (o->phones.count == PH_PAIRED_PHONE_LIMIT)
        return false;
    ph_paired_phones next = o->phones;
    ph_paired_phone *phone = &next.items[next.count];
    *phone = (ph_paired_phone){0};
    if (!ph_paired_phone_identity(key, phone->id) || ph_paired_phones_find(&next, phone->id) >= 0 ||
        ph_paired_phones_is_revoked(&next, phone->id))
        return false;
    memcpy(phone->key, key, sizeof phone->key);
    memcpy(phone->seed, o->token, sizeof phone->seed);
    snprintf(phone->remote, sizeof phone->remote, "%s", o->phone_name);
    next.selected = next.count++;
    if (!ph_paired_phones_save(&next))
        return false;
    o->phones = next;
    use_selected_phone(o);
    return true;
}

static void clear_verification(ph_origin *o, bool notify)
{
    o->verification_reset |= notify &&
        (o->controls_verified || o->verification_code[0] || o->verification_requested);
    o->verification_id[0] = 0;
    OPENSSL_cleanse(o->verification_code, sizeof o->verification_code);
    o->verification_requested = false;
    o->controls_verified = false;
    o->verification_deadline = 0;
}

static bool verify_input_line(ph_origin *o, int fd, const char *line)
{
    if (!strcmp(line, "IV?"))
        return send_all(fd, "IV READY\n", 9);
    if (!strncmp(line, "IV BEGIN ", 9) || !strncmp(line, "IV CANCEL ", 10)) {
        bool begin = line[3] == 'B';
        const char *id = line + (begin ? 9 : 10);
        if (!request_id(id))
            return false;
        pthread_mutex_lock(&o->lock);
        bool ready = o->control.connected && o->control.enabled && o->reconnect_key[0];
        if (begin && ready) {
            clear_verification(o, false);
            o->verification_reset = false;
            memcpy(o->verification_id, id, 37);
            o->verification_deadline = ph_metrics_now_ms() + 30000;
            o->verification_requested = true;
        } else if (!begin && !strcmp(o->verification_id, id)) {
            clear_verification(o, true);
        }
        pthread_mutex_unlock(&o->lock);
        PH_LOGI("controls verification: iPhone %s%s", begin ? "began" : "cancelled",
                begin && !ready ? " (refused: controls off or not paired)" : "");
        if (!begin || ready)
            return true;
        char reply[64];
        int length = snprintf(reply, sizeof reply, "IV FAILED %s\n", id);
        return send_all(fd, reply, (size_t)length);
    }
    if (!strncmp(line, "IV PROOF ", 9)) {
        char reference[37], proof[17], extra;
        if (sscanf(line + 9, "%36s %16[0-9]%c", reference, proof, &extra) != 2 ||
            !request_id(reference) || strlen(proof) != 16)
            return false;
        pthread_mutex_lock(&o->lock);
        bool same_request = !strcmp(reference, o->verification_id);
        bool in_time = ph_metrics_now_ms() < o->verification_deadline;
        bool ok = same_request && o->control.connected && o->control.enabled &&
                  o->verification_code[0] && in_time &&
                  !CRYPTO_memcmp(proof, o->verification_code, 16);
        /* How many digits arrived right, for the log: a dropped key shows as
         * a shorter run. The digits themselves are never logged. */
        int matching = 0;
        while (o->verification_code[0] && matching < 16 && proof[matching] == o->verification_code[matching])
            matching++;
        PH_LOGI("controls verification: %s (%d of 16 digits right%s%s%s)", ok ? "OK" : "FAILED", matching,
                same_request ? "" : ", an older attempt", in_time ? "" : ", too late",
                o->verification_code[0] ? "" : ", no code typed yet");
        if (same_request) {
            clear_verification(o, false);
            o->controls_verified = ok;
            if (ok)
                memcpy(o->verification_id, reference, sizeof reference);
        }
        pthread_mutex_unlock(&o->lock);
        OPENSSL_cleanse(proof, sizeof proof);
        char reply[64];
        int length = snprintf(reply, sizeof reply, "IV %s %s\n", ok ? "OK" : "FAILED", reference);
        return send_all(fd, reply, (size_t)length);
    }
    return true;
}

static bool send_verification_reset(ph_origin *o, int fd)
{
    pthread_mutex_lock(&o->lock);
    bool notify = o->verification_reset;
    o->verification_reset = false;
    pthread_mutex_unlock(&o->lock);
    return !notify || send_all(fd, "IV RESET\n", 9);
}

static bool paired_client(const ph_origin *o)
{
    return o->have_client && o->client_generation == o->pairing_generation && o->reconnect_key[0];
}

bool ph_origin_companion_connected(ph_origin *o)
{
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    bool connected = paired_client(o);
    pthread_mutex_unlock(&o->lock);
    return connected;
}

static bool accept_clip_line(ph_origin *o, int fd, const char *line, bool *consumed)
{
    char kind;
    size_t size;
    uint8_t *body;

    *consumed = false;
    /* The person cancelled the arriving file on the iPhone. */
    if (!strncmp(line, "STOP ", 5)) {
        *consumed = true;
        pthread_mutex_lock(&o->lock);
        if (o->push_busy && o->active_push_id[0] && !strcmp(line + 5, o->active_push_id)) {
            o->push_cancel = true;
            o->push_stopped_by_phone = true;
        }
        pthread_mutex_unlock(&o->lock);
        return true;
    }
    if (!strcmp(line, "IV?") || !strncmp(line, "IV ", 3)) {
        *consumed = true;
        return verify_input_line(o, fd, line);
    }
    if (!strncmp(line, "INPUT ", 6)) {
        unsigned enabled, screen_only;
        char extra, id[37], reply[80];
        *consumed = true;
        if (sscanf(line, "INPUT %36s %u %u %c", id, &enabled, &screen_only, &extra) != 3 ||
            !request_id(id) || enabled > 1 || screen_only > 1)
            return false;
        pthread_mutex_lock(&o->lock);
        o->control = (ph_control_policy){.managed = true,
                                         .connected = true,
                                         .enabled = enabled != 0,
                                         .screen_only = screen_only != 0};
        if (!enabled)
            clear_verification(o, true);
        pthread_mutex_unlock(&o->lock);
        int len = snprintf(reply, sizeof reply, "INPUT OK %s %u %u\n", id, enabled, screen_only);
        return send_all(fd, reply, (size_t)len);
    }
    if (!strncmp(line, "SOUND ", 6)) {
        char id[37], word[8], extra, reply[80];
        *consumed = true;
        if (sscanf(line, "SOUND %36s %7s %c", id, word, &extra) != 2 || !request_id(id))
            return false;
        int mode = ph_sound_mode_parse(word);
        if (mode < 0)
            return false;
        pthread_mutex_lock(&o->lock);
        o->audio_capable = true;
        bool pc_wins = o->audio_push; /* chosen on the PC since the phone last heard */
        if (!pc_wins) {
            o->phone_audio = mode;
            o->audio_serial++;
        }
        pthread_mutex_unlock(&o->lock);
        if (!pc_wins)
            PH_LOGI("sound while sharing set on the iPhone: %s", ph_sound_mode_name(mode));
        int n = snprintf(reply, sizeof reply, "SOUND OK %s %s\n", id, ph_sound_mode_name(mode));
        return send_all(fd, reply, (size_t)n);
    }
    if (!strcmp(line, "VOLUME up") || !strcmp(line, "VOLUME down")) {
        *consumed = true;
        pthread_mutex_lock(&o->lock);
        if (o->volume_steps > -32 && o->volume_steps < 32)
            o->volume_steps += line[7] == 'u' ? 1 : -1;
        pthread_mutex_unlock(&o->lock);
        return true;
    }
    if (!strncmp(line, "VOLUME note ", 12)) {
        /* What the phone's volume button experiment saw, for this log. */
        char text[121];
        size_t n = 0;
        *consumed = true;
        for (const char *c = line + 12; *c && n < sizeof text - 1; c++)
            text[n++] = (*c >= 0x20 && *c < 0x7f) ? *c : '?';
        text[n] = 0;
        PH_LOGI("volume buttons (iPhone): %s", text);
        return true;
    }
    if (!strcmp(line, "VOLUME stuck")) {
        /* The phone could not put its own volume back after a press. */
        *consumed = true;
        PH_LOGW("volume buttons: the iPhone could not put its own volume back");
        return true;
    }
    if (!strncmp(line, "SYNC ", 5)) {
        unsigned enable;
        char id[37], extra, reply[64];
        *consumed = true;
        if (sscanf(line, "SYNC %36s %u %c", id, &enable, &extra) != 2 || !request_id(id) ||
            enable > 1)
            return false;
        pthread_mutex_lock(&o->lock);
        o->clip_ready = enable != 0;
        if (!enable) {
            o->out_pending = false;
            free(o->out_data);
            o->out_data = NULL;
            o->in_ready = false;
            free(o->in_data);
            o->in_data = NULL;
            o->in_len = 0;
            o->ask_clip = false;
            o->clip_wait = false;
        }
        pthread_mutex_unlock(&o->lock);
        int n = snprintf(reply, sizeof reply, "SYNC OK %s %u\n", id, enable);
        return send_all(fd, reply, (size_t)n);
    }
    if (!strncmp(line, "PAIR ", 5)) {
        const char *key = line + 5;
        *consumed = true;
        if (strlen(key) != 32 || strspn(key, "0123456789abcdef") != 32)
            return false;
        pthread_mutex_lock(&o->lock);
        /* Only the current authenticated TLS session may finish enrollment.
         * Leave the code valid within its original window until the key is saved. */
        bool ok = o->have_client && o->client_generation == o->pairing_generation &&
                  (*o->reconnect_key || (o->code_open && time(NULL) < o->code_expiry)) &&
                  save_phone_key(o, key);
        if (ok)
            o->code_open = false;
        pthread_mutex_unlock(&o->lock);
        return ok && send_all(fd, "PAIRED\n", 7);
    }
    if (strcmp(line, "RESUME?") == 0) {
        o->resumable = true;
        *consumed = true;
        return send_all(fd, "RESUME 2\n", 9);
    }
    if (!strcmp(line, "INBOX?")) {
        *consumed = true;
        return send_all(fd, "INBOX 3\n", 8);
    }
    /* Asked only by iPhones that send the checksum after the file. */
    if (!strcmp(line, "INBOX? 4")) {
        *consumed = true;
        return send_all(fd, "INBOX 4\n", 8);
    }
    if (!strcmp(line, "SHARE?") || !strcmp(line, "SCREEN? 2") || !strcmp(line, "SCREEN? 3") ||
        !strcmp(line, "SCREEN? 4")) {
        bool sharing = strcmp(line, "SHARE?") != 0;
        char reply[90];
        *consumed = true;
        if (sharing && !send_all(fd, !strcmp(line, "SCREEN? 4") ? "SCREEN 4\n"
                                     : !strcmp(line, "SCREEN? 3") ? "SCREEN 3\n" : "SCREEN 2\n", 9))
            return false;
        pthread_mutex_lock(&o->lock);
        const char *key = sharing ? o->screen_key : o->share_key;
        if (paired_client(o) && !o->channels_paused && *key)
            snprintf(reply, sizeof reply, "%s %s\n", sharing ? "SCREENKEY" : "SHAREKEY", key);
        else
            reply[0] = 0;
        pthread_mutex_unlock(&o->lock);
        bool sent = !reply[0] || send_all(fd, reply, strlen(reply));
        OPENSSL_cleanse(reply, sizeof reply);
        return sent;
    }
    if (!strcmp(line, "FILES 3") || !strcmp(line, "FILES 2") || !strcmp(line, "FILES")) {
        pthread_mutex_lock(&o->lock);
        o->files_ok = true;
        if (line[5])
            o->resumable = true;
        if (!strcmp(line, "FILES 3"))
            o->chunked = true;
        pthread_mutex_unlock(&o->lock);
        *consumed = true;
        return true;
    }
    if (strncmp(line, "NAME ", 5) == 0) {
        const char *raw = line + 5;
        /* An extra separator marks a user-chosen name. Older PCs trim it as usual. */
        bool chosen = *raw == ' ';
        char clean[64];
        size_t n = 0;
        while (*raw == ' ')
            raw++;
        for (; raw[n] && n + 1 < sizeof clean && raw[n] != '\r'; n++)
            clean[n] = (unsigned char)raw[n] < 32 ? ' ' : raw[n];
        while (n > 0 && clean[n - 1] == ' ')
            n--;
        clean[n] = 0;
        pthread_mutex_lock(&o->lock);
        if (clean[0] && paired_client(o)) {
            const ph_paired_phone *phone = ph_paired_phones_selected(&o->phones);
            if (phone && (chosen || !phone->remote[0] || strcmp(clean, "iPhone")) &&
                strcmp(phone->remote, clean)) {
                ph_paired_phones next = o->phones;
                ph_paired_phone *updated = &next.items[next.selected];
                snprintf(updated->remote, sizeof updated->remote, "%s", clean);
                if (ph_paired_phones_save(&next)) {
                    o->phones = next;
                    use_selected_phone(o);
                }
            }
        }
        pthread_mutex_unlock(&o->lock);
        *consumed = true;
        return true;
    }
    if (strncmp(line, "CLIP ", 5) != 0)
        return true;
    *consumed = true;
    if (!ph_clip_header(line, &kind, &size)) {
        PH_LOGW("clipboard: ignored a bad item from the iPhone");
        return false;
    }
    body = malloc(size);
    if (!body || !read_exact(fd, body, size)) {
        free(body);
        return false;
    }
    pthread_mutex_lock(&o->lock);
    if (!o->clip_ready) {
        pthread_mutex_unlock(&o->lock);
        free(body);
        return true;
    }
    free(o->in_data);
    o->in_data = body;
    o->in_len = size;
    o->in_kind = kind;
    o->in_ready = true;
    o->note_got = true;
    pthread_mutex_unlock(&o->lock);
    PH_LOGI("clipboard: received %s from the iPhone (%zu bytes)", kind == 'P' ? "an image" : "text",
            size);
    return true;
}

static bool send_outgoing(ph_origin *o, int fd)
{
    uint8_t *data = NULL;
    size_t len = 0;
    char kind = 0;
    char hdr[64];
    int hdr_len;
    bool ok;

    pthread_mutex_lock(&o->lock);
    if (o->clip_ready && o->out_pending && o->out_data) {
        data = o->out_data;
        len = o->out_len;
        kind = o->out_kind;
        o->out_data = NULL;
        o->out_len = 0;
        o->out_pending = false;
    }
    pthread_mutex_unlock(&o->lock);
    if (!data)
        return true;
    hdr_len = snprintf(hdr, sizeof hdr, "CLIP %c %zu\n", kind, len);
    ok = hdr_len > 0 && (size_t)hdr_len < sizeof hdr && send_all(fd, hdr, (size_t)hdr_len)
         && send_all(fd, data, len);
    if (!ok) {
        pthread_mutex_lock(&o->lock);
        if (!o->out_pending) {
            o->out_data = data;
            o->out_len = len;
            o->out_kind = kind;
            o->out_pending = true;
            data = NULL;
        }
        pthread_mutex_unlock(&o->lock);
        free(data);
        return false;
    }
    free(data);
    pthread_mutex_lock(&o->lock);
    o->note_sent = true;
    pthread_mutex_unlock(&o->lock);
    PH_LOGI("clipboard: sent %s to the iPhone (%zu bytes)", kind == 'P' ? "an image" : "text", len);
    return true;
}

#define PH_PUSH_PORT 47734
#define PH_PUSH_MAX PH_TRANSFER_MAX_BYTES

static bool safe_name(const char *path, char *out, size_t cap)
{
    const char *base = ph_path_base(path);
    size_t i, n = 0;

    if (!base || !base[0] || !out || cap < 2)
        return false;
    for (i = 0; base[i] && n + 1 < cap; i++) {
        unsigned char c = (unsigned char)base[i];
        if (c < 32 || c == '/' || c == '\\' || c == '"')
            c = '_';
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n > 0 && strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

static void push_progress(ph_origin *o, long long size, unsigned long long left)
{
    pthread_mutex_lock(&o->lock);
    o->push_done = (unsigned long long)size - left;
    pthread_mutex_unlock(&o->lock);
}

/* The network worker and Explorer can finish more than one queue before the UI polls. */
static bool append_result(ph_origin *o, ph_origin_send_state state, const ph_origin_send *send)
{
    if (o->client_generation != o->pairing_generation)
        return false;
    struct transfer_result *entry = calloc(1, sizeof *entry);
    if (!entry)
        return false;
    entry->state = state;
    entry->send = *send;
    if (o->last_result)
        o->last_result->next = entry;
    else
        o->results = entry;
    o->last_result = entry;
    return true;
}

static void push_result(ph_origin *o, ph_origin_send_state state, ph_origin_send *send)
{
    pthread_mutex_lock(&o->lock);
    if (state == PH_SEND_STORED)
        send->confirmed = send->bytes;
    append_result(o, state, send);
    o->active_push_key[0] = 0;
    o->push_busy = o->push_count > 0;
    pthread_mutex_unlock(&o->lock);
}

static double now_ms(void);
static void hex_string(const unsigned char *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = 0;
}
/* Caller holds the lock so a new pairing cannot inherit a queued send. */
static bool resume_key(ph_origin *o, const char *path, char key[80])
{
    char source[1100];
    unsigned char digest[32];
    unsigned n = 0;
    int len = snprintf(source, sizeof source, "%s/%s", o->token, path);
    if (len < 0 || len >= (int)sizeof source ||
        !EVP_Digest(source, (size_t)len, digest, &n, EVP_sha256(), NULL))
        return false;
    memcpy(key, "push-", 5);
    hex_string(digest, 32, key + 5);
    return true;
}
static bool valid_resume_key(const char *key)
{
    return strlen(key) == 69 && !strncmp(key, "push-", 5) &&
           strspn(key + 5, "0123456789abcdef") == 64;
}

static bool valid_digest(const char *digest)
{
    return strlen(digest) == 64 && strspn(digest, "0123456789abcdef") == 64;
}

static bool new_send_id(char out[33])
{
    unsigned char random[16];
    if (RAND_bytes(random, sizeof random) != 1)
        return false;
    hex_string(random, sizeof random, out);
    return true;
}
static bool read_pending_commands(ph_origin *o, int fd);
static bool send_clip_ask(ph_origin *o, int fd);
static bool send_audio_set(ph_origin *o, int fd);

struct hash_transfer {
    ph_origin *origin;
    int fd;
    bool canceled;
    bool connected;
};

static bool service_during_hash(void *user)
{
    struct hash_transfer *work = user;
    ph_origin *o = work->origin;
    pthread_mutex_lock(&o->lock);
    work->canceled = o->push_cancel;
    pthread_mutex_unlock(&o->lock);
    if (work->canceled || atomic_load(&o->stop))
        return false;
    work->connected = read_pending_commands(o, work->fd);
    return work->connected;
}

static ph_origin_send_state push_ticket(ph_origin_send *send, FILE *file,
                                        struct hash_transfer *work, bool resumable)
{
    char record[112], hash[65];
    if (!ph_file_digest(file, hash, service_during_hash, work))
        return work->canceled ? PH_SEND_CANCELED : PH_SEND_FAILED;
    if (send->digest[0] && strcmp(hash, send->digest))
        return PH_SEND_CHANGED;
    memcpy(send->digest, hash, sizeof send->digest);
    if (resumable) {
        snprintf(record, sizeof record, "%s %s", send->id, send->digest);
        if (!ph_config_write_line(send->resume_key, record))
            return PH_SEND_FAILED;
    }
    return PH_SEND_NONE;
}
/* Only the matching transfer can extend a receipt deadline or commit its ticket. */
/* `size` is the file's: the iPhone checks and saves a large file before it
 * answers (saying PENDING meanwhile), so the overall wait grows with it. */
static bool push_response(ph_origin *o, int fd, const char *ticket, char *line, size_t cap,
                          unsigned long long size)
{
    char pending[48];
    snprintf(pending, sizeof pending, "PENDING %s", ticket);
    double grace = 300000 + (double)size / 20e6 * 1000;
    double deadline = now_ms() + 15000, limit = now_ms() + (grace < 3600000 ? grace : 3600000);
    while (now_ms() < limit) {
        struct pollfd pollfd = {.fd = fd, .events = POLLIN};
        int remaining = (int)(deadline - now_ms());
        if (remaining <= 0 || (!ph_tls_pending(fd) && ph_poll(&pollfd, 1, remaining) <= 0) ||
            !read_line(fd, line, cap))
            return false;
        if (ticket[0] && !strcmp(line, pending)) {
            deadline = now_ms() + 15000;
            continue;
        }
        bool consumed = false;
        if (!accept_clip_line(o, fd, line, &consumed))
            return false;
        if (!consumed)
            return true;
    }
    return false;
}
static void finish_ticket(const char *key)
{
    char path[1100];
    if (*key && ph_config_path(key, path, sizeof path))
        ph_remove(path);
}

static bool send_push(ph_origin *o, int fd)
{
    ph_origin_send job;
    char hdr[300];
    FILE *file = NULL;
    long long size;
    int hdr_len;
    bool ok = true, cancel = false, ready, resumable, chunked;
    unsigned long long left;

    pthread_mutex_lock(&o->lock);
    if (!o->push_count) {
        pthread_mutex_unlock(&o->lock);
        return true;
    }
    job = o->push_queue[0];
    memcpy(o->active_push_key, job.resume_key, sizeof o->active_push_key);
    memmove(o->push_queue, o->push_queue + 1, sizeof o->push_queue[0] * (size_t)(o->push_count - 1));
    o->push_count--;
    /* Cancel empties the queue, so a file taken from it starts uncancelled. */
    o->push_cancel = false;
    o->push_stopped_by_phone = false;
    snprintf(o->active_push_id, sizeof o->active_push_id, "%s", job.id);
    ready = o->files_ok;
    resumable = o->resumable;
    chunked = o->chunked;
    snprintf(o->push_busy_name, sizeof o->push_busy_name, "%s", job.name);
    o->push_busy = true;
    o->push_size = 0;
    o->push_done = 0;
    pthread_mutex_unlock(&o->lock);
    if (!ready) {
        PH_LOGW("send: the connected iPhone cannot receive a file yet");
        push_result(o, PH_SEND_FAILED, &job);
        return true;
    }
    size = ph_file_size(job.path);
    if (size <= 0 || (unsigned long long)size > PH_PUSH_MAX) {
        push_result(o, PH_SEND_FAILED, &job);
        return true;
    }
    if ((uint64_t)size != job.bytes) {
        push_result(o, PH_SEND_CHANGED, &job);
        return true;
    }
    pthread_mutex_lock(&o->lock);
    o->push_size = (unsigned long long)size;
    pthread_mutex_unlock(&o->lock);
    file = ph_fopen(job.path, "rb");
    ph_origin_send_state problem = PH_SEND_FAILED;
    struct hash_transfer work = {.origin = o, .fd = fd, .connected = true};
    if (file)
        problem = push_ticket(&job, file, &work, resumable);
    if (problem) {
        if (file)
            fclose(file);
        push_result(o, problem, &job);
        return work.connected;
    }
    pthread_mutex_lock(&o->lock);
    append_result(o, PH_SEND_ACTIVE, &job);
    pthread_mutex_unlock(&o->lock);
    const char *ticket = resumable ? job.id : "";
    hdr_len = resumable
                  ? snprintf(hdr, sizeof hdr, "PUSH%d %s %s %llu %s\n", chunked ? 3 : 2, ticket, job.digest,
                             (unsigned long long)size, job.name)
                  : snprintf(hdr, sizeof hdr, "PUSH %llu %s\n", (unsigned long long)size, job.name);
    if (!file || hdr_len < 1 || (size_t)hdr_len >= sizeof hdr ||
        !send_all(fd, hdr, (size_t)hdr_len)) {
        if (file)
            fclose(file);
        push_result(o, PH_SEND_FAILED, &job);
        return false;
    }
    char saved_receipt[48] = "SAVED";
    if (resumable)
        snprintf(saved_receipt, sizeof saved_receipt, "SAVED %s", ticket);
    left = (unsigned long long)size;
    if (resumable) {
        char answer[256], extra, id[33];
        unsigned long long offset;
        if (!push_response(o, fd, ticket, answer, sizeof answer, (unsigned long long)size)) {
            fclose(file);
            push_result(o, PH_SEND_FAILED, &job);
            return false;
        }
        if (!strcmp(answer, saved_receipt)) {
            fclose(file);
            finish_ticket(job.resume_key);
            push_result(o, PH_SEND_STORED, &job);
            return true;
        }
        if (sscanf(answer, "OFFSET %32s %llu%c", id, &offset, &extra) != 2 || strcmp(id, ticket) ||
            offset > left || ph_fseek64(file, offset) != 0) {
            fclose(file);
            push_result(o, PH_SEND_FAILED, &job);
            return false;
        }
        left -= offset;
        job.confirmed = offset;
        push_progress(o, size, left);
    }
    while (left && ok && !cancel) {
        char chunk[65536];
        size_t want = left > sizeof chunk ? sizeof chunk : (size_t)left;
        size_t n = fread(chunk, 1, want, file);

        pthread_mutex_lock(&o->lock);
        cancel = o->push_cancel;
        pthread_mutex_unlock(&o->lock);
        if (cancel)
            break;
        if (n == 0) {
            ok = false;
            break;
        }
        if (chunked) {
            char chunk_header[32];
            int length = snprintf(chunk_header, sizeof chunk_header, "CHUNK %zu\n", n);
            ok = send_all(fd, chunk_header, (size_t)length);
        }
        ok = ok && send_all(fd, chunk, n);
        left -= (unsigned long long)n;
        push_progress(o, size, left);
        /* Process control commands at frame boundaries. Let the receipt reader consume
         * the final acknowledgement once the last chunk is written. */
        if (ok && chunked && left)
            ok = read_pending_commands(o, fd) && send_verification_reset(o, fd) &&
                 send_clip_ask(o, fd) && send_outgoing(o, fd);
    }
    fclose(file);
    pthread_mutex_lock(&o->lock);
    bool phone_stopped = o->push_stopped_by_phone;
    pthread_mutex_unlock(&o->lock);
    if (cancel && phone_stopped && chunked) {
        /* Cancelled on the iPhone: say so between segments, and the link
         * stays up. It isn't continued by itself later. */
        char ended[64];
        int length = snprintf(ended, sizeof ended, "ENDPUSH %s\n", ticket);
        PH_LOGI("send: %s cancelled on the iPhone", job.name);
        push_result(o, PH_SEND_CANCELED, &job);
        return send_all(fd, ended, (size_t)length);
    }
    if (cancel) {
        push_result(o, PH_SEND_CANCELED, &job);
        shutdown(fd, SHUT_RDWR);
        return false;
    }
    if (!ok || left) {
        push_result(o, PH_SEND_FAILED, &job);
        return false;
    }
    char receipt[128];
    if (push_response(o, fd, ticket, receipt, sizeof receipt, (unsigned long long)size) &&
        !strcmp(receipt, saved_receipt)) {
        finish_ticket(job.resume_key);
        push_result(o, PH_SEND_STORED, &job);
        return true;
    }
    pthread_mutex_lock(&o->lock);
    phone_stopped = o->push_stopped_by_phone;
    pthread_mutex_unlock(&o->lock);
    if (phone_stopped) {
        /* Cancelled on the iPhone just as the last bytes went. */
        PH_LOGI("send: %s cancelled on the iPhone", job.name);
        push_result(o, PH_SEND_CANCELED, &job);
        return true;
    }
    PH_LOGW("send: the iPhone did not accept %s", job.name);
    push_result(o, PH_SEND_FAILED, &job);
    return true;
}

static bool read_pending_commands(ph_origin *o, int fd)
{
    for (int i = 0; i < 8; i++) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        char line[160];
        bool consumed = false;
        int pr = ph_tls_pending(fd) ? 1 : ph_poll(&p, 1, 0);

        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (pr == 0)
            break;
        if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p.revents & POLLIN))
            return false;
        if (!read_line(fd, line, sizeof line))
            return false;
        if (!accept_clip_line(o, fd, line, &consumed))
            return false;
        (void)consumed;
    }
    return true;
}

static bool service_idle(ph_origin *o, int fd)
{
    return read_pending_commands(o, fd) && send_verification_reset(o, fd) &&
           send_push(o, fd) && send_clip_ask(o, fd) && send_audio_set(o, fd) &&
           send_outgoing(o, fd);
}

/* The sound choice changed on the PC: tell a phone that understands SOUND. */
static bool send_audio_set(ph_origin *o, int fd)
{
    int mode = -1;

    pthread_mutex_lock(&o->lock);
    if (o->audio_push && o->audio_capable && o->phone_audio >= 0) {
        o->audio_push = false;
        mode = o->phone_audio;
    }
    pthread_mutex_unlock(&o->lock);
    if (mode < 0)
        return true;
    char line[32];
    int n = snprintf(line, sizeof line, "SOUND SET %s\n", ph_sound_mode_name(mode));
    PH_LOGI("sound while sharing set on the PC: %s", ph_sound_mode_name(mode));
    return send_all(fd, line, (size_t)n);
}

/* One line. A phone that does not know CLIP? ignores it. */
static bool send_clip_ask(ph_origin *o, int fd)
{
    bool due = false;

    pthread_mutex_lock(&o->lock);
    if (o->ask_clip && o->clip_ready && now_ms() - o->ask_clip_ms >= 250.0) {
        o->ask_clip = false;
        due = true;
    }
    pthread_mutex_unlock(&o->lock);
    if (!due)
        return true;
    if (!send_all(fd, "CLIP?\n", 6))
        return false;
    PH_LOGI("clipboard: asked the iPhone for its copy");
    return true;
}

/* Waits for a pull request. Returns false when the phone left or a newer
 * connection is queued, so the caller drops this one. Called with o->lock held. */
static bool wait_for_ask(ph_origin *o, int fd)
{
    while (!o->ask && !o->stop) {
        struct timespec until;

        pthread_mutex_unlock(&o->lock);
        if (!service_idle(o, fd)) {
            PH_LOGI("photo pull: iPhone disconnected");
            pthread_mutex_lock(&o->lock);
            return false;
        }
        pthread_mutex_lock(&o->lock);
        if (o->ask || o->stop)
            break;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 250 * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        if (o->out_pending && o->clip_ready)
            continue;
        pthread_cond_timedwait(&o->cond, &o->lock, &until);
    }
    return true;
}

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static bool pull_reply(ph_origin *o, unsigned id, int fd, char *line, size_t capacity)
{
    uint64_t deadline = ph_metrics_now_ms() + 300000;
    bool sent_cancel = false;
    while (!atomic_load(&o->stop) && ph_metrics_now_ms() < deadline) {
        if (!sent_cancel && !pull_available(o, id)) {
            if (!send_all(fd, "CANCEL PULL\n", 12))
                return false;
            sent_cancel = true;
        }
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        int result = ph_tls_pending(fd) ? 1 : ph_poll(&ready, 1, 250);
        if (result < 0)
            return false;
        if (result == 0)
            continue;
        bool consumed;
        if (!read_line(fd, line, capacity) || !accept_clip_line(o, fd, line, &consumed))
            return false;
        if (!consumed)
            return true;
    }
    return false;
}

static void serve_client(ph_origin *o, int fd)
{
    char line[2048 + 2 * PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    ph_sock_keepalive(fd, 15, 5, 3);
    pthread_mutex_lock(&o->lock);
    o->client_fd = fd;
    pthread_mutex_unlock(&o->lock);
    if (!ph_tls_accept(fd) || !read_line(fd, line, sizeof line))
        goto done;
    unsigned char random[32];
    if (RAND_bytes(random, sizeof random) != 1)
        goto done;
    char ticket[65];
    hex_string(random, sizeof random, ticket);
    pthread_mutex_lock(&o->lock);
    const char *credential = *o->reconnect_key ? o->reconnect_key : o->token;
    time_t now = time(NULL);
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(o->failures[i].address, o->peer)) {
            slot = i;
            break;
        }
        if (o->failures[i].since < o->failures[slot].since)
            slot = i;
    }
    if (strcmp(o->failures[slot].address, o->peer) || now - o->failures[slot].since >= 60) {
        snprintf(o->failures[slot].address, sizeof o->failures[slot].address, "%s", o->peer);
        o->failures[slot].since = now;
        o->failures[slot].attempts = 0;
    }
    bool has_key = *o->reconnect_key != 0;
    bool code_open = o->code_open && now < o->code_expiry;
    bool code_attempt = !strncmp(line, "HELLO ", 6) && strlen(line + 6) == 8;
    bool locked = !has_key && code_open && code_attempt && o->failures[slot].attempts >= 5;
    bool accepted = !locked && (has_key || code_open) &&
                    !strncmp(line, "HELLO ", 6) && strlen(line + 6) == strlen(credential) &&
                    !CRYPTO_memcmp(line + 6, credential, strlen(credential));
    if (!accepted && !locked && !has_key && code_open && code_attempt)
        o->failures[slot].attempts++;
    if (accepted)
        o->failures[slot].attempts = 0;
    bool disconnected = accepted && o->disconnect_requested;
    if (disconnected)
        o->disconnect_requested = false;
    if (accepted && !disconnected) {
        memcpy(o->channel_ticket, ticket, sizeof ticket);
        o->have_client = true;
        o->channels_paused = false;
        o->client_generation = o->pairing_generation;
        if (!o->screen_live) {
            o->control = (ph_control_policy){.managed = true, .screen_only = true};
            if (!o->audio_push)
                o->phone_audio = -1;
        }
        o->audio_capable = false;
        o->control.connected = true;
        snprintf(o->known, sizeof o->known, "%s", o->peer);
    }
    pthread_mutex_unlock(&o->lock);
    if (!accepted || disconnected) {
        if (disconnected)
            send_all(fd, "DISCONNECTED\n", 13);
        else {
            const char *reply = locked ? "PAIR LOCKED\n" : has_key && code_attempt ? "PAIR SAVED\n" :
                                !code_attempt ? "PAIR REVOKED\n" :
                                code_open ? "PAIR CODE\n" : "PAIR CLOSED\n";
            send_all(fd, reply, strlen(reply));
        }
        goto done;
    }
    char hostname[64], encoded[96], welcome[320];
    ph_host_name(hostname, sizeof hostname);
    PH_LOGI("photo pull: this PC introduces itself as \"%s\"", hostname);
    EVP_EncodeBlock((unsigned char *)encoded, (const unsigned char *)hostname,
                    (int)strlen(hostname));
    int welcome_len =
        snprintf(welcome, sizeof welcome, "OK %s %s\nSYNC READY\nINPUT READY\nSOUND READY\nVOLUME READY\n", ticket,
                 encoded);
    if (!send_all(fd, welcome, (size_t)welcome_len))
        goto done;
    PH_LOGI("photo pull: iPhone connected");
    while (!o->stop) {
        uint64_t hash;
        double aspect;
        unsigned id;
        char dir[512], path[768];
        char ask_line[128 + 4 * PH_PICTURE_THUMB * PH_PICTURE_THUMB
                      + 4 * PH_PICTURE_DETAIL * PH_PICTURE_DETAIL];
        static const char hex[] = "0123456789abcdef";
        uint8_t thumb[PH_PICTURE_THUMB * PH_PICTURE_THUMB];
        uint8_t detail[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL];
        ph_origin_screen screen;
        bool asked, have_thumb, have_detail, have_screen;
        double sent_at, answer_at;
        size_t used, i;

        pthread_mutex_lock(&o->lock);
        if (!wait_for_ask(o, fd)) {
            pthread_mutex_unlock(&o->lock);
            break;
        }
        asked = o->ask && !o->stop;
        hash = o->hash;
        aspect = o->aspect;
        have_thumb = o->have_thumb;
        memcpy(thumb, o->thumb, sizeof thumb);
        have_detail = have_thumb && o->have_detail;
        memcpy(detail, o->detail, sizeof detail);
        have_screen = have_detail && o->have_screen;
        screen = o->screen;
        id = o->ask_id;
        snprintf(dir, sizeof dir, "%s", o->dir);
        o->ask = false;
        pthread_mutex_unlock(&o->lock);
        if (!asked)
            break;
        /* Phones before 1.6 answer a fourth field with MISS, so it is sent only with a thumb. */
        used = (size_t)snprintf(ask_line, sizeof ask_line, "PULL %016llx %.4f",
                                (unsigned long long)hash, aspect);
        if (have_thumb) {
            ask_line[used++] = ' ';
            for (i = 0; i < sizeof thumb; i++) {
                ask_line[used++] = hex[thumb[i] >> 4];
                ask_line[used++] = hex[thumb[i] & 15];
            }
        }
        if (have_detail) {
            ask_line[used++] = ' ';
            for (i = 0; i < sizeof detail; i++) {
                ask_line[used++] = hex[detail[i] >> 4];
                ask_line[used++] = hex[detail[i] & 15];
            }
        }
        /* Sixth field: S<aspect>:<32-cell hex>:<64-cell hex> of the whole screen. */
        if (have_screen) {
            used += (size_t)snprintf(ask_line + used, sizeof ask_line - used, " S%.4f:",
                                     screen.aspect);
            for (i = 0; i < sizeof screen.thumb; i++) {
                ask_line[used++] = hex[screen.thumb[i] >> 4];
                ask_line[used++] = hex[screen.thumb[i] & 15];
            }
            ask_line[used++] = ':';
            for (i = 0; i < sizeof screen.detail; i++) {
                ask_line[used++] = hex[screen.detail[i] >> 4];
                ask_line[used++] = hex[screen.detail[i] & 15];
            }
        }
        ask_line[used++] = '\n';
        sent_at = now_ms();
        if (!send_all(fd, ask_line, used)) {
            PH_LOGW("photo pull: iPhone did not send the file");
            pthread_mutex_lock(&o->lock);
            o->ready = PH_ORIGIN_MISS;
            o->ready_id = id;
            pthread_mutex_unlock(&o->lock);
            break;
        }
        if (!pull_reply(o, id, fd, line, sizeof line)) {
            pthread_mutex_lock(&o->lock);
            o->ready = PH_ORIGIN_MISS;
            o->ready_id = id;
            pthread_mutex_unlock(&o->lock);
            break;
        }
        answer_at = now_ms();
        if (strncmp(line, "MISS", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
            PH_LOGI("photo pull: iPhone searched %.1f s", (answer_at - sent_at) / 1000);
            /* The iPhone says why, or how close its best photo came. */
            PH_LOGI("photo pull: no original was exported%s%.120s%s", line[4] ? " (iPhone: " : "",
                    line[4] ? line + 5 : "", line[4] ? ")" : "");
            pthread_mutex_lock(&o->lock);
            o->ready = PH_ORIGIN_MISS;
            o->ready_id = id;
            o->path[0] = 0;
            pthread_mutex_unlock(&o->lock);
            continue;
        }
        char receipt_id[37] = "";
        {
            unsigned long long size = 0;
            char name[96];
            if (!strncmp(line, "FILE2 ", 6)) {
                if (strlen(line) < 44 || line[42] != ' ')
                    break;
                memcpy(receipt_id, line + 6, 36);
                if (!request_id(receipt_id))
                    break;
                memmove(line + 5, line + 43, strlen(line + 43) + 1);
                memcpy(line, "FILE ", 5);
            }
            bool saved = ph_pull_parse_file(line, PH_PULL_MAX_BYTES, &size, name, sizeof name) &&
                         save_body(o, id, fd, dir, name, size, path, sizeof path);
            if (!saved) {
                if (*receipt_id) {
                    char receipt[64];
                    int length = snprintf(receipt, sizeof receipt, "REJECTED %s\n", receipt_id);
                    send_all(fd, receipt, (size_t)length);
                }
                PH_LOGW("photo pull: the iPhone's file did not arrive");
                pthread_mutex_lock(&o->lock);
                o->ready = PH_ORIGIN_MISS;
                o->ready_id = id;
                bool canceled = o->canceled_pull == id;
                pthread_mutex_unlock(&o->lock);
                if (canceled)
                    continue;
                break;
            }
        }
        pthread_mutex_lock(&o->lock);
        o->ready = PH_ORIGIN_FILE;
        o->ready_id = id;
        snprintf(o->path, sizeof o->path, "%s", path);
        pthread_mutex_unlock(&o->lock);
        if (*receipt_id) {
            char receipt[64];
            int len = snprintf(receipt, sizeof receipt, "RECEIVED %s\n", receipt_id);
            send_all(fd, receipt, (size_t)len);
        }
        {
            /* Phones from 2.11 put the match's scores after the name. */
            const char *why = strchr(line, ' ');
            why = why ? strchr(why + 1, ' ') : NULL;
            why = why ? strchr(why + 1, ' ') : NULL;
            PH_LOGI("photo pull: original %s%s%s%s", path, why ? " (scores " : "", why ? why + 1 : "",
                    why ? ")" : "");
            PH_LOGI("photo pull: iPhone searched %.1f s, sent the file in %.1f s",
                    (answer_at - sent_at) / 1000, (now_ms() - answer_at) / 1000);
        }
    }
done:
    pthread_mutex_lock(&o->lock);
    /* Files still waiting cannot go anywhere now: report each as not sent. */
    for (int i = 0; i < o->push_count; i++)
        append_result(o, PH_SEND_FAILED, &o->push_queue[i]);
    o->push_count = 0;
    o->push_busy = false;
    o->channel_ticket[0] = 0;
    o->have_client = false;
    o->control.connected = false;
    if (!o->screen_live || !o->controls_verified)
        clear_verification(o, false);
    o->verification_reset = false;
    o->files_ok = false;
    o->resumable = false;
    o->chunked = false;
    o->client_fd = -1;
    o->clip_ready = false;
    o->ask_clip = false;
    o->clip_wait = false;
    free(o->out_data);
    o->out_data = NULL;
    o->out_pending = false;
    o->in_ready = false;
    free(o->in_data);
    o->in_data = NULL;
    o->in_len = 0;
    if (o->ask) {
        o->ask = false;
        o->ready = PH_ORIGIN_MISS;
        o->ready_id = o->ask_id;
    }
    pthread_mutex_unlock(&o->lock);
}

static void listen_send(ph_origin *o)
{
    int fd = ph_sock_listen(PH_PUSH_PORT, true, 2);

    o->push_fd = -1;
    if (fd < 0) {
        PH_LOGW("send: local port %d is unavailable", PH_PUSH_PORT);
        return;
    }
    o->push_fd = fd;
    PH_LOGI("send: drop a file on the window, or send a path to 127.0.0.1:%d", PH_PUSH_PORT);
}

static void take_local_send(ph_origin *o)
{
    char buf[1200];
    size_t n = 0;
    int fd;

    if (!o || o->push_fd < 0)
        return;
    fd = ph_sock_accept(o->push_fd, NULL);
    if (fd < 0)
        return;
    ph_sock_timeout(fd, 2);
    while (n + 1 < sizeof buf) {
        int got = (int)recv(fd, buf + n, 1, 0);
        if (got <= 0)
            break;
        if (buf[n] == '\n')
            break;
        n++;
    }
    buf[n] = 0;
    if (n && buf[n - 1] == '\r')
        buf[n - 1] = 0;
    if (strncmp(buf, "SEND ", 5) == 0 && buf[5]) {
        bool queued = ph_origin_put_file(o, buf + 5);
        send(fd, queued ? "OK\n" : "NO\n", 3, MSG_NOSIGNAL);
    }
    ph_sock_close(fd);
}

bool ph_origin_resume_ready(ph_origin *o)
{
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    bool ready = o->files_ok && o->resumable;
    pthread_mutex_unlock(&o->lock);
    return ready;
}

static bool queue_file(ph_origin *o, const char *path, const ph_origin_send *previous,
                       bool restart)
{
    ph_origin_send send = {0};
    if (!o || !path || !ph_path_absolute(path) || strlen(path) >= sizeof send.path)
        return false;
    long long size = ph_file_size(path);
    if (size <= 0 || (unsigned long long)size > PH_PUSH_MAX)
        return false;
    const char *name = previous && previous->name[0] ? previous->name : path;
    if (!safe_name(name, send.name, sizeof send.name))
        return false;
    memcpy(send.path, path, strlen(path) + 1);
    send.bytes = (uint64_t)size;
    if (previous) {
        send.history_id = previous->history_id;
        if (!restart) {
            send.bytes = previous->bytes ? previous->bytes : send.bytes;
            send.confirmed = previous->confirmed;
            if (valid_digest(previous->digest))
                memcpy(send.digest, previous->digest, sizeof send.digest);
            if (strlen(previous->id) == 32 && strspn(previous->id, "0123456789abcdef") == 32)
                memcpy(send.id, previous->id, sizeof send.id);
        }
    }
    pthread_mutex_lock(&o->lock);
    /* Nothing to send to: say so now rather than show a send that never moves. */
    if (!o->have_client || o->push_count == PH_PUSH_QUEUE)
        goto reject;
    if (previous && valid_resume_key(previous->resume_key))
        memcpy(send.resume_key, previous->resume_key, sizeof send.resume_key);
    else if (!resume_key(o, path, send.resume_key))
        goto reject;
    if (!strcmp(o->active_push_key, send.resume_key))
        goto reject;
    for (int i = 0; i < o->push_count; i++)
        if (!strcmp(o->push_queue[i].resume_key, send.resume_key) ||
            !strcmp(o->push_queue[i].path, path))
            goto reject;
    if (!restart && (!send.id[0] || !send.digest[0])) {
        char record[112], id[33], digest[65];
        if (ph_config_read_line(send.resume_key, record, sizeof record) &&
            sscanf(record, "%32[0-9a-f] %64[0-9a-f]", id, digest) == 2 &&
            strlen(id) == 32 && valid_digest(digest) &&
            (!send.id[0] || !strcmp(id, send.id))) {
            memcpy(send.id, id, sizeof send.id);
            memcpy(send.digest, digest, sizeof send.digest);
        }
    }
    if ((!send.id[0] && !new_send_id(send.id)) ||
        !append_result(o, PH_SEND_QUEUED, &send))
        goto reject;
    if (!o->push_busy) {
        snprintf(o->push_busy_name, sizeof o->push_busy_name, "%s", send.name);
        o->push_size = send.bytes;
        o->push_done = 0;
    }
    o->push_queue[o->push_count++] = send;
    o->push_busy = true;
    pthread_cond_signal(&o->cond);
    pthread_mutex_unlock(&o->lock);
    PH_LOGI("send: queued %s", path);
    return true;

reject:
    pthread_mutex_unlock(&o->lock);
    return false;
}

bool ph_origin_put_file(ph_origin *o, const char *path)
{
    return queue_file(o, path, NULL, false);
}

bool ph_origin_restart_file(ph_origin *o, const char *path)
{
    return queue_file(o, path, NULL, true);
}

bool ph_origin_retry_file(ph_origin *o, const ph_origin_send *previous, bool restart)
{
    return previous && queue_file(o, previous->path, previous, restart);
}

bool ph_origin_push_busy(ph_origin *o, char *name, size_t name_len)
{
    bool busy;

    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    busy = o->push_busy;
    if (name && name_len)
        snprintf(name, name_len, "%s", busy ? o->push_busy_name : "");
    pthread_mutex_unlock(&o->lock);
    return busy;
}

int ph_origin_push_percent(ph_origin *o)
{
    int percent = -1;

    if (!o)
        return -1;
    pthread_mutex_lock(&o->lock);
    if (o->push_busy && o->push_size)
        percent = (int)(o->push_done * 100 / o->push_size);
    pthread_mutex_unlock(&o->lock);
    return percent;
}

void ph_origin_cancel_push(ph_origin *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    /* Stops the file moving now and drops the ones waiting behind it. */
    o->push_cancel = true;
    for (int i = 0; i < o->push_count; i++)
        append_result(o, PH_SEND_CANCELED, &o->push_queue[i]);
    o->push_count = 0;
    pthread_mutex_unlock(&o->lock);
}

ph_origin_send_state ph_origin_take_send(ph_origin *o, ph_origin_send *send)
{
    if (!o)
        return PH_SEND_NONE;
    pthread_mutex_lock(&o->lock);
    struct transfer_result *entry = o->results;
    ph_origin_send_state state = entry ? entry->state : PH_SEND_NONE;
    if (entry) {
        if (send)
            *send = entry->send;
        o->results = entry->next;
        if (!o->results)
            o->last_result = NULL;
        free(entry);
    }
    pthread_mutex_unlock(&o->lock);
    return state;
}

int ph_origin_take_push_path(ph_origin *o, char *name, size_t name_len)
{
    ph_origin_send send;
    ph_origin_send_state state;
    do {
        state = ph_origin_take_send(o, &send);
    } while (state == PH_SEND_QUEUED || state == PH_SEND_ACTIVE);
    if (name && name_len)
        snprintf(name, name_len, "%s", state ? send.path : "");
    return state;
}

int ph_origin_take_push(ph_origin *o, char *name, size_t cap)
{
    char path[1024] = "";
    int note = ph_origin_take_push_path(o, path, sizeof path);
    if (name && cap)
        snprintf(name, cap, "%s", ph_path_base(path));
    return note;
}

void ph_origin_forget(ph_origin *o)
{
    int client = -1;

    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    o->disconnect_requested = o->have_client && o->reconnect_key[0];
    o->channels_paused = true;
    o->pairing_generation++;
    o->screen_live = false;
    clear_verification(o, false);
    o->channel_ticket[0] = 0;
    o->known[0] = 0;
    client = o->client_fd;
    if (client >= 0)
        shutdown(client, SHUT_RDWR);
    pthread_mutex_unlock(&o->lock);
    PH_LOGI("photo pull: forgot the connected iPhone");
}

static bool stopping(ph_origin *o)
{
    bool stop;

    pthread_mutex_lock(&o->lock);
    stop = o->stop;
    pthread_mutex_unlock(&o->lock);
    return stop;
}

static void *send_main(void *arg)
{
    ph_origin *o = arg;

    while (!stopping(o)) {
        struct pollfd p = {.fd = o->push_fd, .events = POLLIN};
        if (ph_poll(&p, 1, 200) > 0 && (p.revents & POLLIN))
            take_local_send(o);
    }
    return NULL;
}

static void *origin_main(void *arg)
{
    ph_origin *o = arg;

    while (!o->stop) {
        struct pollfd p[1];
        int pr, fd;
        struct sockaddr_in peer;

        p[0].fd = o->listen_fd;
        p[0].events = POLLIN;
        p[0].revents = 0;
        pr = ph_poll(p, 1, 200);
        if (pr <= 0)
            continue;
        if (!(p[0].revents & POLLIN))
            continue;
        fd = ph_sock_accept(o->listen_fd, &peer);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&o->lock);
        inet_ntop(AF_INET, &peer.sin_addr, o->peer, sizeof o->peer);
        pthread_mutex_unlock(&o->lock);
        serve_client(o, fd);
        ph_tls_end();
        ph_sock_close(fd);
    }
    return NULL;
}

ph_origin *ph_origin_start(char *hint, size_t hint_len, ph_origin_start_status *status)
{
    ph_origin *o;
    int fd;
    char ip[64];

    if (hint && hint_len)
        hint[0] = 0;
    if (status)
        *status = PH_ORIGIN_START_FAILED;
    if (!*ph_tls_fingerprint()) {
        PH_LOGW("pairing: could not load the desktop identity");
        if (status)
            *status = PH_ORIGIN_START_IDENTITY;
        return NULL;
    }
    o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->push_fd = -1;
    o->pairing_generation = 1;
    o->phone_audio = -1;
    atomic_init(&o->stop, false);
    ph_paired_phones_load(&o->phones);
    use_selected_phone(o);
    if (o->phones.selected < 0) {
        if (!make_token(o->token, sizeof o->token)) {
            free(o);
            return NULL;
        }
        o->code_open = true;
        o->code_expiry = time(NULL) + 600;
    }
    fd = ph_sock_listen(PH_PULL_PORT, false, 4);
    if (fd < 0) {
        PH_LOGW("pairing: port %d is unavailable", PH_PULL_PORT);
        if (status)
            *status = PH_ORIGIN_START_LISTENER;
        free(o);
        return NULL;
    }
    o->listen_fd = fd;
    o->client_fd = -1;
    listen_send(o);
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
    if (o->push_fd >= 0)
        o->send_thread_started = pthread_create(&o->send_thread, NULL, send_main, o) == 0;
    if (pthread_create(&o->thread, NULL, origin_main, o) != 0) {
        if (o->send_thread_started) {
            o->stop = true;
            pthread_join(o->send_thread, NULL);
        }
        ph_sock_close(fd);
        ph_sock_close(o->push_fd);
        pthread_mutex_destroy(&o->lock);
        pthread_cond_destroy(&o->cond);
        free(o);
        return NULL;
    }
    lan_address(ip, sizeof ip);
    snprintf(o->lan_ip, sizeof o->lan_ip, "%s", ip);
    fill_hint(hint, hint_len, ip, o->token);
    if (status)
        *status = PH_ORIGIN_START_OK;
    PH_LOGI("photo pull: listening on %s:%d", ip, PH_PULL_PORT);
    return o;
}

bool ph_origin_refresh_address(ph_origin *o, char *hint, size_t hint_len)
{
    if (!o)
        return false;
    char ip[64], token[16];
    lan_address(ip, sizeof ip);
    if (!strcmp(ip, o->lan_ip))
        return false;
    snprintf(o->lan_ip, sizeof o->lan_ip, "%s", ip);
    pthread_mutex_lock(&o->lock);
    snprintf(token, sizeof token, "%s", o->token);
    pthread_mutex_unlock(&o->lock);
    fill_hint(hint, hint_len, ip, token);
    return true;
}

/* Revoke active channels before switching profile. The lock prevents new work
 * from being queued against the previous phone while the socket is closing. */
static void invalidate_session(ph_origin *o)
{
    o->disconnect_requested = false;
    o->channels_paused = false;
    o->screen_live = false;
    o->channel_ticket[0] = 0;
    o->known[0] = 0;
    o->pairing_generation++;
    o->have_client = false;
    clear_verification(o, false);
    o->verification_reset = false;
    o->clip_ready = false;
    o->note_got = o->note_sent = false;
    o->push_count = 0;
    o->push_cancel = true;
    while (o->results) {
        struct transfer_result *entry = o->results;
        o->results = entry->next;
        free(entry);
    }
    o->last_result = NULL;
    free(o->in_data);
    o->in_data = NULL;
    o->in_ready = false;
    free(o->out_data);
    o->out_data = NULL;
    o->out_pending = false;
    if (o->client_fd >= 0)
        shutdown(o->client_fd, SHUT_RDWR);
}

static void remove_phone(ph_paired_phones *phones, int i)
{
    ph_paired_phones_revoke(phones, phones->items[i].id);
    memmove(phones->items + i, phones->items + i + 1,
            (size_t)(phones->count - i - 1) * sizeof phones->items[0]);
    phones->count--;
    if (phones->selected == i)
        phones->selected = -1;
    else if (phones->selected > i)
        phones->selected--;
}

static bool open_pairing_code(ph_origin *o, bool replace, char *hint, size_t hint_len)
{
    char token[16], ip[64];
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    if (!make_token(token, sizeof token)) {
        pthread_mutex_unlock(&o->lock);
        return false;
    }
    ph_paired_phones next = o->phones;
    if (replace && next.selected >= 0)
        remove_phone(&next, next.selected);
    if (next.count == PH_PAIRED_PHONE_LIMIT) {
        pthread_mutex_unlock(&o->lock);
        return false;
    }
    next.selected = -1;
    if (!ph_paired_phones_save(&next)) {
        pthread_mutex_unlock(&o->lock);
        return false;
    }
    o->phones = next;
    invalidate_session(o);
    use_selected_phone(o);
    snprintf(o->token, sizeof o->token, "%s", token);
    memset(o->failures, 0, sizeof o->failures);
    o->code_open = true;
    o->code_expiry = time(NULL) + 600;
    pthread_mutex_unlock(&o->lock);
    lan_address(ip, sizeof ip);
    fill_hint(hint, hint_len, ip, token);
    return true;
}

bool ph_origin_renew(ph_origin *o, char *hint, size_t hint_len)
{
    return open_pairing_code(o, true, hint, hint_len);
}

bool ph_origin_add_phone(ph_origin *o, char *hint, size_t hint_len)
{
    return open_pairing_code(o, false, hint, hint_len);
}

bool ph_origin_choose_phone(ph_origin *o, const char *id)
{
    if (!o || !id)
        return false;
    pthread_mutex_lock(&o->lock);
    ph_paired_phones next = o->phones;
    next.selected = ph_paired_phones_find(&next, id);
    bool ok = next.selected >= 0;
    if (ok && next.selected != o->phones.selected) {
        ok = ph_paired_phones_save(&next);
        if (ok) {
            invalidate_session(o);
            o->phones = next;
            use_selected_phone(o);
            o->code_open = false;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

bool ph_origin_rename_phone(ph_origin *o, const char *id, const char *name)
{
    if (!o || !id || !name || !*name || strlen(name) >= sizeof o->phones.items[0].alias ||
        strpbrk(name, "\r\n\t"))
        return false;
    pthread_mutex_lock(&o->lock);
    ph_paired_phones next = o->phones;
    int index = ph_paired_phones_find(&next, id);
    bool ok = index >= 0;
    if (ok) {
        snprintf(next.items[index].alias, sizeof next.items[index].alias, "%s", name);
        ok = ph_paired_phones_save(&next);
        if (ok) {
            o->phones = next;
            use_selected_phone(o);
        }
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

bool ph_origin_remove_saved_phone(ph_origin *o, const char *id)
{
    if (!o || !id)
        return false;
    pthread_mutex_lock(&o->lock);
    ph_paired_phones next = o->phones;
    int index = ph_paired_phones_find(&next, id);
    bool ok = index >= 0 && index != next.selected;
    if (ok) {
        remove_phone(&next, index);
        ok = ph_paired_phones_save(&next);
        if (ok)
            o->phones = next;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

void ph_origin_sessions(ph_origin *o, ph_session_state *state)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    for (int i = 0; i < o->phones.count && state->count < PH_SESSION_DEVICE_MAX; i++) {
        const ph_paired_phone *phone = &o->phones.items[i];
        ph_session_device *row = &state->devices[state->count++];
        bool selected = o->phones.selected == i;
        snprintf(row->id, sizeof row->id, "phone:%s", phone->id);
        snprintf(row->name, sizeof row->name, "%s",
                 phone->alias[0] ? phone->alias : phone->remote[0] ? phone->remote : "iPhone");
        row->companion = true;
        row->active = selected;
        row->connected = selected && paired_client(o);
        row->screen_sharing = selected && o->screen_live;
    }
    pthread_mutex_unlock(&o->lock);
}

static bool channel_authorized(const ph_origin *o, const char *ticket, const char *key)
{
    return !o->channels_paused && o->reconnect_key[0] &&
           ((key[0] && !CRYPTO_memcmp(key, ticket, 64)) ||
            (paired_client(o) && o->channel_ticket[0] && !CRYPTO_memcmp(o->channel_ticket, ticket, 64)));
}

unsigned ph_origin_generation(ph_origin *o, const char *ticket)
{
    if (!o || (ticket && strlen(ticket) != 64))
        return 0;
    pthread_mutex_lock(&o->lock);
    unsigned generation = (!ticket || channel_authorized(o, ticket, o->screen_key))
                              ? o->pairing_generation : 0;
    pthread_mutex_unlock(&o->lock);
    return generation;
}

unsigned ph_origin_inbox_generation(ph_origin *o, const char *ticket)
{
    if (!o || !ticket || strlen(ticket) != 64)
        return 0;
    pthread_mutex_lock(&o->lock);
    unsigned generation = channel_authorized(o, ticket, o->share_key) ? o->pairing_generation : 0;
    pthread_mutex_unlock(&o->lock);
    return generation;
}

void ph_origin_screen_live(ph_origin *o, unsigned generation, bool live)
{
    if (!o || !generation)
        return;
    pthread_mutex_lock(&o->lock);
    if (generation == o->pairing_generation) {
        o->screen_live = live;
        if (!live && !o->control.connected)
            clear_verification(o, false);
    }
    pthread_mutex_unlock(&o->lock);
}

bool ph_origin_selected_id(ph_origin *o, char id[65])
{
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    const ph_paired_phone *phone = ph_paired_phones_selected(&o->phones);
    if (phone)
        memcpy(id, phone->id, 65);
    pthread_mutex_unlock(&o->lock);
    return phone != NULL;
}

void ph_origin_stop(ph_origin *o)
{
    int client = -1;

    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    o->stop = true;
    client = o->client_fd;
    pthread_cond_signal(&o->cond);
    if (client >= 0)
        shutdown(client, SHUT_RDWR);
    pthread_mutex_unlock(&o->lock);
    shutdown(o->listen_fd, SHUT_RDWR);
    pthread_join(o->thread, NULL);
    if (o->send_thread_started)
        pthread_join(o->send_thread, NULL);
    ph_sock_close(o->listen_fd);
    ph_sock_close(o->push_fd);
    free(o->out_data);
    free(o->in_data);
    while (o->results) {
        struct transfer_result *e = o->results;
        o->results = e->next;
        free(e);
    }
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    free(o);
}

void ph_origin_pc_name(char *out, size_t len)
{
    ph_host_name(out, len);
}

bool ph_origin_code_open(ph_origin *o)
{
    bool open;
    if (!o)
        return true;
    pthread_mutex_lock(&o->lock);
    open = o->code_open;
    pthread_mutex_unlock(&o->lock);
    return open;
}

void ph_origin_phone_name(ph_origin *o, char *out, size_t len)
{
    if (!out || !len)
        return;
    out[0] = 0;
    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    snprintf(out, len, "%s", o->phone_name);
    pthread_mutex_unlock(&o->lock);
}

bool ph_origin_remember_verified_phone_name(ph_origin *o, const char *name)
{
    /* The phone's own Bluetooth name, exactly as it is: "Unknown", "-" and
     * the like are names too. It fills in a phone known only as "iPhone". */
    if (!o || !name || !*name || strlen(name) >= sizeof o->phone_name || strpbrk(name, "\t\r\n"))
        return false;
    pthread_mutex_lock(&o->lock);
    const ph_paired_phone *phone = ph_paired_phones_selected(&o->phones);
    bool saved = false;
    /* The Bluetooth name is exact, so it also replaces the network host name's
     * approximation of it ("Johns iPhone" for "John's iPhone"). */
    if (phone && paired_client(o) && o->controls_verified && strcmp(phone->remote, name) &&
        (!phone->remote[0] || !strcmp(phone->remote, "iPhone") ||
         ph_mdns_host_form_of(name, phone->remote))) {
        ph_paired_phones next = o->phones;
        snprintf(next.items[next.selected].remote, sizeof next.items[next.selected].remote, "%s", name);
        if (ph_paired_phones_save(&next)) {
            o->phones = next;
            use_selected_phone(o);
            saved = true;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return saved;
}

bool ph_origin_remember_network_phone_name(ph_origin *o, const char *name)
{
    /* From the phone's own answer on the network, at the address of its
     * authenticated companion connection. Like the Bluetooth name, it only
     * fills in a phone known as "iPhone"; a name the person chose stays. */
    if (!o || !name || !*name || strlen(name) >= sizeof o->phone_name || strpbrk(name, "\t\r\n"))
        return false;
    pthread_mutex_lock(&o->lock);
    const ph_paired_phone *phone = ph_paired_phones_selected(&o->phones);
    bool saved = false;
    if (phone && paired_client(o) && strcmp(phone->remote, name) &&
        (!phone->remote[0] || !strcmp(phone->remote, "iPhone"))) {
        ph_paired_phones next = o->phones;
        snprintf(next.items[next.selected].remote, sizeof next.items[next.selected].remote, "%s", name);
        if (ph_paired_phones_save(&next)) {
            o->phones = next;
            use_selected_phone(o);
            saved = true;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return saved;
}

bool ph_origin_peer(ph_origin *o, char *address, size_t len)
{
    bool ok;

    if (!o || !address || !len)
        return false;
    pthread_mutex_lock(&o->lock);
    ok = o->known[0] != '\0';
    snprintf(address, len, "%s", ok ? o->known : "");
    pthread_mutex_unlock(&o->lock);
    return ok;
}

bool ph_origin_ask(ph_origin *o, uint64_t hash, double aspect, const uint8_t *thumb,
                   const uint8_t *detail, const ph_origin_screen *screen, const char *dir)
{
    bool ok = false;

    if (!o || !dir || !dir[0])
        return false;
    pthread_mutex_lock(&o->lock);
    /* An ask the link has not sent yet is replaced: the newest press is the one that
     * counts, and refusing it would read as a lost iPhone. */
    if (o->have_client) {
        o->hash = hash;
        o->aspect = aspect;
        o->have_thumb = thumb != NULL;
        if (thumb)
            memcpy(o->thumb, thumb, sizeof o->thumb);
        o->have_detail = detail != NULL;
        if (detail)
            memcpy(o->detail, detail, sizeof o->detail);
        o->have_screen = screen != NULL;
        if (screen)
            o->screen = *screen;
        snprintf(o->dir, sizeof o->dir, "%s", dir);
        o->ask_id++;
        o->ready = PH_ORIGIN_PENDING;
        o->ready_id = 0;
        o->path[0] = 0;
        o->ask = true;
        pthread_cond_signal(&o->cond);
        ok = true;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

void ph_origin_cancel_pull(ph_origin *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    if (o->ask_id && (o->ask || o->ready == PH_ORIGIN_PENDING)) {
        o->canceled_pull = o->ask_id;
        o->ask = false;
        o->ready = PH_ORIGIN_IDLE;
    }
    pthread_mutex_unlock(&o->lock);
}

ph_origin_result ph_origin_take(ph_origin *o, char *path, size_t path_len)
{
    ph_origin_result result = PH_ORIGIN_IDLE;

    if (!o)
        return PH_ORIGIN_IDLE;
    pthread_mutex_lock(&o->lock);
    if (o->ready_id == o->ask_id && o->ask_id && !o->ask) {
        result = o->ready;
        if (result == PH_ORIGIN_FILE && path && path_len)
            snprintf(path, path_len, "%s", o->path);
        o->ready = PH_ORIGIN_IDLE;
        o->ready_id = 0;
    } else if (o->ask || (o->have_client && o->ask_id && o->ready == PH_ORIGIN_PENDING)) {
        result = PH_ORIGIN_PENDING;
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

bool ph_origin_put_clip(ph_origin *o, char kind, const void *data, size_t len)
{
    size_t max;
    uint8_t *copy;

    if (!o || !data || (kind != 'T' && kind != 'P'))
        return false;
    max = kind == 'T' ? PH_CLIP_TEXT_MAX : PH_CLIP_IMAGE_MAX;
    if (!len || len > max)
        return false;
    copy = malloc(len);
    if (!copy)
        return false;
    memcpy(copy, data, len);
    pthread_mutex_lock(&o->lock);
    if (!o->have_client || !o->clip_ready) {
        pthread_mutex_unlock(&o->lock);
        free(copy);
        return false;
    }
    free(o->out_data);
    o->out_data = copy;
    o->out_len = len;
    o->out_kind = kind;
    o->out_pending = true;
    pthread_cond_signal(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return true;
}

bool ph_origin_take_clip(ph_origin *o, char *kind, void **data, size_t *len)
{
    bool ok = false;

    if (!o || !kind || !data || !len)
        return false;
    *data = NULL;
    pthread_mutex_lock(&o->lock);
    if (o->in_ready && o->in_data) {
        *kind = o->in_kind;
        *data = o->in_data;
        *len = o->in_len;
        o->in_data = NULL;
        o->in_len = 0;
        o->in_ready = false;
        if (!o->ask_clip)
            o->clip_wait = false;
        ok = true;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

unsigned ph_origin_clip_flags(ph_origin *o)
{
    unsigned flags = 0;

    if (!o)
        return 0;
    pthread_mutex_lock(&o->lock);
    if (o->note_sent)
        flags |= PH_ORIGIN_CLIP_SENT;
    if (o->note_got)
        flags |= PH_ORIGIN_CLIP_GOT;
    if (o->clip_ready && paired_client(o))
        flags |= PH_ORIGIN_CLIP_READY;
    o->note_sent = false;
    o->note_got = false;
    pthread_mutex_unlock(&o->lock);
    return flags;
}

void ph_origin_ask_clip(ph_origin *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    o->ask_clip = true;
    o->clip_wait = true;
    o->ask_clip_ms = now_ms();
    pthread_cond_signal(&o->cond);
    pthread_mutex_unlock(&o->lock);
}

bool ph_origin_clip_waiting(ph_origin *o)
{
    bool waiting = false;

    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    if (o->clip_wait) {
        if (now_ms() - o->ask_clip_ms < 2000.0)
            waiting = true;
        else
            o->clip_wait = false;
    }
    pthread_mutex_unlock(&o->lock);
    return waiting;
}

ph_control_policy ph_origin_control_policy(ph_origin *o)
{
    ph_control_policy policy = {0};
    if (!o)
        return policy;
    pthread_mutex_lock(&o->lock);
    policy = o->control;
    policy.verified = policy.connected && o->controls_verified;
    pthread_mutex_unlock(&o->lock);
    return policy;
}

const char *ph_sound_mode_name(int mode)
{
    return mode == PH_SOUND_PC ? "pc" : mode == PH_SOUND_BOTH ? "both"
           : mode == PH_SOUND_IPHONE ? "iphone" : "unknown";
}

int ph_sound_mode_parse(const char *word)
{
    if (!word)
        return -1;
    return !strcmp(word, "pc") ? PH_SOUND_PC : !strcmp(word, "both") ? PH_SOUND_BOTH
           : !strcmp(word, "iphone") ? PH_SOUND_IPHONE : -1;
}

void ph_origin_set_sound_mode(ph_origin *o, int mode)
{
    if (!o || mode < PH_SOUND_IPHONE || mode > PH_SOUND_PC)
        return;
    pthread_mutex_lock(&o->lock);
    o->phone_audio = mode;
    o->audio_push = true;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
}

unsigned ph_origin_sound_serial(ph_origin *o)
{
    if (!o)
        return 0;
    pthread_mutex_lock(&o->lock);
    unsigned serial = o->audio_serial;
    pthread_mutex_unlock(&o->lock);
    return serial;
}

int ph_origin_take_volume_steps(ph_origin *o)
{
    if (!o)
        return 0;
    pthread_mutex_lock(&o->lock);
    int steps = o->volume_steps;
    o->volume_steps = 0;
    pthread_mutex_unlock(&o->lock);
    return steps;
}

int ph_origin_sound_mode(ph_origin *o)
{
    if (!o)
        return -1;
    pthread_mutex_lock(&o->lock);
    int mode = o->phone_audio;
    pthread_mutex_unlock(&o->lock);
    return mode;
}

ph_control_policy ph_origin_broadcast_control_policy(ph_origin *o)
{
    if (!o)
        return (ph_control_policy){0};
    pthread_mutex_lock(&o->lock);
    ph_control_policy policy = o->control;
    policy.connected |= o->screen_live && o->controls_verified;
    policy.verified = policy.connected && o->controls_verified;
    pthread_mutex_unlock(&o->lock);
    return policy;
}

bool ph_origin_verifying_controls(ph_origin *o)
{
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    bool waiting = o->verification_id[0] && ph_metrics_now_ms() < o->verification_deadline;
    pthread_mutex_unlock(&o->lock);
    return waiting;
}

bool ph_origin_issue_control_proof(ph_origin *o, const char code[17])
{
    if (!o || !code)
        return false;
    pthread_mutex_lock(&o->lock);
    bool pending = o->verification_requested && o->control.connected && o->control.enabled &&
                   ph_metrics_now_ms() < o->verification_deadline;
    if (pending) {
        memcpy(o->verification_code, code, sizeof o->verification_code);
        o->verification_requested = false;
        o->verification_deadline = ph_metrics_now_ms() + 12000;
    }
    pthread_mutex_unlock(&o->lock);
    return pending;
}

bool ph_origin_control_proof_current(ph_origin *o, const char code[17])
{
    if (!o || !code)
        return false;
    pthread_mutex_lock(&o->lock);
    bool current = o->verification_code[0] && ph_metrics_now_ms() < o->verification_deadline &&
                   !CRYPTO_memcmp(o->verification_code, code, 16);
    pthread_mutex_unlock(&o->lock);
    return current;
}

bool ph_origin_controls_verified(ph_origin *o)
{
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    bool verified = o->control.connected && o->controls_verified;
    pthread_mutex_unlock(&o->lock);
    return verified;
}

bool ph_origin_pairing_tag(ph_origin *o, char out[17])
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!o || !out)
        return false;
    pthread_mutex_lock(&o->lock);
    bool ok = o->reconnect_key[0] &&
              EVP_Digest(o->reconnect_key, strlen(o->reconnect_key), digest, &length, EVP_sha256(),
                         NULL) && length >= 8;
    pthread_mutex_unlock(&o->lock);
    if (!ok)
        return false;
    for (int i = 0; i < 8; i++)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    OPENSSL_cleanse(digest, sizeof digest);
    return true;
}

bool ph_origin_trust_controls(ph_origin *o)
{
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    bool ok = o->reconnect_key[0] && (!o->control.managed || o->control.enabled) &&
              !o->verification_requested;
    if (ok) {
        o->controls_verified = true;
        o->verification_reset = false;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

void ph_origin_reset_control_verification(ph_origin *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    clear_verification(o, true);
    pthread_mutex_unlock(&o->lock);
}

bool ph_origin_authorize(ph_origin *o, const char *ticket)
{
    if (!o || !ticket || strlen(ticket) != 64)
        return false;
    pthread_mutex_lock(&o->lock);
    bool allowed =
        o->have_client && o->channel_ticket[0] && !CRYPTO_memcmp(o->channel_ticket, ticket, 64);
    pthread_mutex_unlock(&o->lock);
    return allowed;
}

bool ph_origin_receive_profile(ph_origin *o, const char *ticket, char out[65])
{
    if (!o || !ticket || strlen(ticket) != 64 || !out)
        return false;
    unsigned char digest[32];
    unsigned n = 0;
    pthread_mutex_lock(&o->lock);
    bool allowed = channel_authorized(o, ticket, o->share_key);
    bool ok = allowed && EVP_Digest(o->reconnect_key, 32, digest, &n, EVP_sha256(), NULL) == 1 &&
              n == sizeof digest;
    pthread_mutex_unlock(&o->lock);
    if (ok)
        hex_string(digest, sizeof digest, out);
    return ok;
}

bool ph_origin_code_expired(ph_origin *o)
{
    if (!o)
        return false;
    pthread_mutex_lock(&o->lock);
    bool expired =
        !*o->reconnect_key && !o->have_client && (!o->code_open || time(NULL) >= o->code_expiry);
    pthread_mutex_unlock(&o->lock);
    return expired;
}
