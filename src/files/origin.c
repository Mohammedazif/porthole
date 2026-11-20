#include "files/origin.h"

#include "core/bonjour.h"
#include "core/config.h"
#include "core/log.h"
#include "files/clip.h"
#include "files/picture_hash.h"
#include "files/pull.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>

#include "core/sock.h"
#include "core/upath.h"

#include <pthread.h>
#include <time.h>

#ifdef _WIN32
#include <iphlpapi.h>
#include <openssl/rand.h>
#else
#include <ifaddrs.h>
#endif

#define PH_PUSH_QUEUE 16

struct ph_origin {
    int listen_fd;
    int client_fd;
    pthread_t thread;
    pthread_t send_thread; /* answers Send to on 127.0.0.1 even mid-transfer */
    bool send_thread_started;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool stop;
    bool have_client;
    bool ask;
    uint64_t hash;
    double aspect;
    uint8_t thumb[PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    bool have_thumb;
    uint8_t detail[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL];
    bool have_detail;
    ph_origin_screen screen;
    bool have_screen;
    unsigned ask_id;
    unsigned ready_id;
    ph_origin_result ready;
    char dir[512];
    char path[768];
    char token[16];
    ph_bonjour *bonjour;
    int push_fd;
    char push_queue[PH_PUSH_QUEUE][1024]; /* files waiting, oldest first */
    int push_count;
    bool push_cancel;
    int notes[PH_PUSH_QUEUE];               /* results not yet taken */
    char note_names[PH_PUSH_QUEUE][96];
    int note_count;
    bool files_ok;
    bool push_busy;
    char push_busy_name[96];
    unsigned long long push_size, push_done;
    char peer[64]; /* address of the connection being served */
    char known[64]; /* last phone that gave the right code */
    bool clip_ready;
    bool out_pending;
    char out_kind;
    uint8_t *out_data;
    size_t out_len;
    bool in_ready;
    char in_kind;
    uint8_t *in_data;
    size_t in_len;
    bool note_sent, note_got, note_ready;
};

static bool token_ok(const char *s)
{
    size_t i;

    if (!s || strlen(s) != 8)
        return false;
    for (i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

static void make_token(char *out, size_t len)
{
    unsigned char bytes[4];
#ifdef _WIN32
    bool got = RAND_bytes(bytes, (int)sizeof bytes) == 1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    bool got = f && fread(bytes, 1, sizeof bytes, f) == sizeof bytes;

    if (f)
        fclose(f);
#endif
    if (!got) {
        bytes[0] = 0xA1;
        bytes[1] = 0xB2;
        bytes[2] = 0xC3;
        bytes[3] = 0xD4;
    }
    snprintf(out, len, "%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3]);
}

static void fill_hint(char *hint, size_t hint_len, const char *ip, const char *token)
{
    if (hint && hint_len)
        snprintf(hint, hint_len, "%s:%d  %s", ip, PH_PULL_PORT, token);
}

static void service_name(char *out, size_t len)
{
    ph_bonjour_host_name(out, len);
}

/* DNS-SD TXT: one length byte, then "addr=<ipv4>". Empty when there is no LAN address. */
static uint16_t txt_addr(uint8_t *out, size_t cap, const char *ip)
{
    char item[72];
    int n;

    if (!ip || !strcmp(ip, "this-pc"))
        return 0;
    n = snprintf(item, sizeof item, "addr=%s", ip);
    if (n < 1 || n > 255 || (size_t)n + 1 > cap)
        return 0;
    out[0] = (uint8_t)n;
    memcpy(out + 1, item, (size_t)n);
    return (uint16_t)(n + 1);
}

static void advertise_pc(ph_origin *o, const char *ip)
{
    uint8_t txt[80];
    uint16_t txt_len = txt_addr(txt, sizeof txt, ip);
    char name[64];

    service_name(name, sizeof name);
    o->bonjour = ph_bonjour_advertise(name, PH_PULL_SERVICE, PH_PULL_PORT,
                                      txt_len ? txt : NULL, txt_len);
}

#ifdef _WIN32
/* The IPv4 address of the first connected adapter with a default gateway: the Wi-Fi
 * or Ethernet the iPhone shares, not a VPN or Hyper-V switch without one. */
static void lan_address(char *out, size_t len)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL, *ad;
    char fallback[64] = "";

    snprintf(out, len, "this-pc");
    for (int tries = 0; tries < 3; tries++) {
        ULONG r;
        free(list);
        list = malloc(size);
        if (!list)
            return;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST
                                              | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, list, &size);
        if (r == NO_ERROR)
            break;
        if (r != ERROR_BUFFER_OVERFLOW) {
            free(list);
            return;
        }
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

static bool read_line(int fd, char *buf, size_t len)
{
    size_t n = 0;

    if (len < 2)
        return false;
    /* A line longer than buf keeps its start: the phone's notes grow over time, and
     * failing here dropped the whole connection after every refused drag. */
    for (size_t seen = 0; seen < 1024 * 1024; seen++) {
        char c;
        ssize_t got = recv(fd, &c, 1, 0);

        if (got <= 0)
            return false;
        if (c == '\n') {
            buf[n] = 0;
            if (n && buf[n - 1] == '\r')
                buf[n - 1] = 0;
            return true;
        }
        if (n + 1 < len)
            buf[n++] = c;
    }
    return false;
}

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;

    while (len) {
        int n = (int)send(fd, p, (int)(len > (1u << 20) ? (1u << 20) : len), MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool save_body(int fd, const char *dir, const char *name, unsigned long long size,
                      char *path, size_t path_len)
{
    char part[768];
    FILE *out;
    unsigned long long left = size;

    if (snprintf(path, path_len, "%s/%s", dir, name) >= (int)path_len)
        return false;
    if (snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part)
        return false;
    out = ph_fopen(part, "wb");
    if (!out)
        return false;
    while (left) {
        char chunk[65536];
        size_t want = left > sizeof chunk ? sizeof chunk : (size_t)left;
        int got = (int)recv(fd, chunk, (int)want, 0);

        if (got <= 0) {
            fclose(out);
            ph_remove(part);
            return false;
        }
        if (fwrite(chunk, 1, (size_t)got, out) != (size_t)got) {
            fclose(out);
            ph_remove(part);
            return false;
        }
        left -= (unsigned long long)got;
    }
    if (fclose(out) != 0) {
        ph_remove(part);
        return false;
    }
    if (ph_rename(part, path) != 0) {
        ph_remove(part);
        return false;
    }
    return true;
}

static bool read_exact(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len) {
        int n = (int)recv(fd, p, (int)(len > (1u << 20) ? (1u << 20) : len), 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* SYNC and CLIP ride the photo link. *consumed is set when this function
 * took the line (and, for CLIP, the bytes after it). */
static bool accept_clip_line(ph_origin *o, int fd, const char *line, bool *consumed)
{
    char kind;
    size_t size;
    uint8_t *body;

    *consumed = false;
    if (strcmp(line, "SYNC") == 0) {
        pthread_mutex_lock(&o->lock);
        if (!o->clip_ready)
            PH_LOGI("clipboard: iPhone will copy and paste");
        o->clip_ready = true;
        o->note_ready = true;
        pthread_mutex_unlock(&o->lock);
        *consumed = true;
        return true;
    }
    if (strcmp(line, "FILES") == 0) {
        pthread_mutex_lock(&o->lock);
        o->files_ok = true;
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
#define PH_PUSH_MAX (512ull * 1024ull * 1024ull)

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

static void push_result(ph_origin *o, int note, const char *name)
{
    pthread_mutex_lock(&o->lock);
    if (o->note_count == PH_PUSH_QUEUE) {
        memmove(o->notes, o->notes + 1, sizeof o->notes[0] * (PH_PUSH_QUEUE - 1));
        memmove(o->note_names, o->note_names + 1, sizeof o->note_names[0] * (PH_PUSH_QUEUE - 1));
        o->note_count--;
    }
    o->notes[o->note_count] = note;
    snprintf(o->note_names[o->note_count], sizeof o->note_names[0], "%s", name ? name : "");
    o->note_count++;
    o->push_busy = o->push_count > 0;
    pthread_mutex_unlock(&o->lock);
}

static bool send_push(ph_origin *o, int fd)
{
    char path[1024], name[96], hdr[160];
    FILE *file = NULL;
    long long size;
    int hdr_len;
    bool ok = true, cancel = false, ready;
    unsigned long long left;

    pthread_mutex_lock(&o->lock);
    if (!o->push_count) {
        pthread_mutex_unlock(&o->lock);
        return true;
    }
    snprintf(path, sizeof path, "%s", o->push_queue[0]);
    memmove(o->push_queue, o->push_queue + 1, sizeof o->push_queue[0] * (size_t)(o->push_count - 1));
    o->push_count--;
    /* Cancel empties the queue, so a file taken from it starts uncancelled. */
    o->push_cancel = false;
    ready = o->files_ok;
    if (!safe_name(path, name, sizeof name))
        name[0] = 0;
    snprintf(o->push_busy_name, sizeof o->push_busy_name, "%s", name);
    o->push_busy = true;
    o->push_size = 0;
    o->push_done = 0;
    pthread_mutex_unlock(&o->lock);
    if (cancel) {
        push_result(o, 3, name);
        return true;
    }
    if (!ready) {
        PH_LOGW("send: the connected iPhone cannot receive a file yet");
        push_result(o, 2, name);
        return true;
    }
    size = name[0] ? ph_file_size(path) : -1;
    if (size <= 0 || (unsigned long long)size > PH_PUSH_MAX) {
        push_result(o, 2, name);
        return true;
    }
    pthread_mutex_lock(&o->lock);
    o->push_size = (unsigned long long)size;
    pthread_mutex_unlock(&o->lock);
    file = ph_fopen(path, "rb");
    hdr_len = snprintf(hdr, sizeof hdr, "PUSH %llu %s\n", (unsigned long long)size, name);
    if (!file || hdr_len < 1 || (size_t)hdr_len >= sizeof hdr || !send_all(fd, hdr, (size_t)hdr_len)) {
        if (file)
            fclose(file);
        push_result(o, 2, name);
        return false;
    }
    left = (unsigned long long)size;
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
        ok = send_all(fd, chunk, n);
        left -= (unsigned long long)n;
        push_progress(o, size, left);
    }
    fclose(file);
    if (cancel) {
        push_result(o, 3, name);
        shutdown(fd, SHUT_RDWR);
        return false;
    }
    if (!ok || left) {
        push_result(o, 2, name);
        return false;
    }
    {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        char line[32];
        int pr = ph_poll(&p, 1, 15000);

        if (pr > 0 && read_line(fd, line, sizeof line) && strcmp(line, "SAVED") == 0) {
            PH_LOGI("send: %s arrived on the iPhone", name);
            push_result(o, 1, name);
            return true;
        }
    }
    PH_LOGW("send: the iPhone did not accept %s", name);
    push_result(o, 2, name);
    return true;
}

/* Reads clipboard messages that arrived while no photo request is in flight,
 * then sends one waiting PC clipboard item. */
static bool service_idle(ph_origin *o, int fd)
{
    int i;

    if (!send_push(o, fd))
        return false;

    for (i = 0; i < 8; i++) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        char line[160];
        bool consumed = false;
        int pr = ph_poll(&p, 1, 0);

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
    return send_outgoing(o, fd);
}

static bool client_waiting(int listen_fd)
{
    struct pollfd p = {.fd = listen_fd, .events = POLLIN};

    return ph_poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
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
        if (client_waiting(o->listen_fd)) {
            PH_LOGI("photo pull: iPhone reconnected");
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

static bool write_pgm(const char *dir, const char *name, const uint8_t *cells)
{
    char path[640];
    FILE *f;
    bool ok;

    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path)
        return false;
    f = fopen(path, "wb");
    if (!f)
        return false;
    fprintf(f, "P5\n%d %d\n255\n", PH_PICTURE_THUMB, PH_PICTURE_THUMB);
    ok = fwrite(cells, 1, (size_t)PH_PICTURE_THUMB * PH_PICTURE_THUMB, f)
         == (size_t)PH_PICTURE_THUMB * PH_PICTURE_THUMB;
    return fclose(f) == 0 && ok;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* A refusal from phone 1.8 ends with the phone's own copy of its closest photo.
 * Both copies are saved next to the pulled files so a mismatch can be seen, and the
 * hex is cut from the line before it is logged. */
static void save_comparison(const char *dir, const uint8_t *pc_thumb, bool have_thumb, char *reply)
{
    const size_t hex_len = 2 * PH_PICTURE_THUMB * PH_PICTURE_THUMB;
    uint8_t phone[PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    char *space = strrchr(reply, ' ');
    size_t i;

    if (!space || strlen(space + 1) != hex_len)
        return;
    for (i = 0; i < sizeof phone; i++) {
        int hi = hex_value(space[1 + 2 * i]), lo = hex_value(space[2 + 2 * i]);
        if (hi < 0 || lo < 0)
            return;
        phone[i] = (uint8_t)(hi * 16 + lo);
    }
    *space = 0;
    if (!dir[0] || !write_pgm(dir, "compare-phone.pgm", phone))
        return;
    if (have_thumb)
        write_pgm(dir, "compare-pc.pgm", pc_thumb);
    PH_LOGI("photo pull: saved both copies as %s/compare-{pc,phone}.pgm", dir);
}

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void serve_client(ph_origin *o, int fd)
{
    char line[2048 + 2 * PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    /* The phone may first download a video from iCloud before it answers. */
    ph_sock_timeout(fd, 300);
    ph_sock_keepalive(fd, 15, 5, 3);
    pthread_mutex_lock(&o->lock);
    o->client_fd = fd;
    pthread_mutex_unlock(&o->lock);
    if (!read_line(fd, line, sizeof line))
        goto done;
    {
        char expect[16];

        pthread_mutex_lock(&o->lock);
        snprintf(expect, sizeof expect, "%s", o->token);
        pthread_mutex_unlock(&o->lock);
        if (strncmp(line, "HELLO ", 6) != 0 || strcmp(line + 6, expect) != 0) {
            PH_LOGW("photo pull: rejected a connection");
            goto done;
        }
    }
    /* SYNC asks a current phone to exchange clipboards. An older phone ignores
     * the line. It must not send clipboard bytes first: an older PC treats any
     * idle input as a disconnect. */
    if (!send_all(fd, "OK\n", 3) || !send_all(fd, "SYNC\n", 5))
        goto done;
    pthread_mutex_lock(&o->lock);
    o->have_client = true;
    snprintf(o->known, sizeof o->known, "%s", o->peer);
    pthread_mutex_unlock(&o->lock);
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
        {
            bool consumed = true;
            bool ok = true;

            /* A clipboard item can arrive before FILE or MISS. */
            while (consumed) {
                if (!read_line(fd, line, sizeof line) || !accept_clip_line(o, fd, line, &consumed)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                PH_LOGW("photo pull: iPhone did not send the file");
                pthread_mutex_lock(&o->lock);
                o->ready = PH_ORIGIN_MISS;
                o->ready_id = id;
                pthread_mutex_unlock(&o->lock);
                break;
            }
        }
        answer_at = now_ms();
        if (strncmp(line, "MISS", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
            PH_LOGI("photo pull: iPhone searched %.1f s", (answer_at - sent_at) / 1000);
            save_comparison(dir, thumb, have_thumb, line);
            PH_LOGI("photo pull: no original matched (aspect %.4f%s)", aspect, line + 4);
            pthread_mutex_lock(&o->lock);
            o->ready = PH_ORIGIN_MISS;
            o->ready_id = id;
            o->path[0] = 0;
            pthread_mutex_unlock(&o->lock);
            continue;
        }
        {
            unsigned long long size = 0;
            char name[96];

            if (!ph_pull_parse_file(line, PH_PULL_MAX_BYTES, &size, name, sizeof name)
                || !save_body(fd, dir, name, size, path, sizeof path)) {
                PH_LOGW("photo pull: the iPhone's file did not arrive");
                pthread_mutex_lock(&o->lock);
                o->ready = PH_ORIGIN_MISS;
                o->ready_id = id;
                pthread_mutex_unlock(&o->lock);
                break;
            }
        }
        pthread_mutex_lock(&o->lock);
        o->ready = PH_ORIGIN_FILE;
        o->ready_id = id;
        snprintf(o->path, sizeof o->path, "%s", path);
        pthread_mutex_unlock(&o->lock);
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
    for (int i = 0; i < o->push_count && o->note_count < PH_PUSH_QUEUE; i++) {
        char waiting[96];
        o->notes[o->note_count] = 2;
        if (!safe_name(o->push_queue[i], waiting, sizeof waiting))
            waiting[0] = 0;
        snprintf(o->note_names[o->note_count++], sizeof o->note_names[0], "%s", waiting);
    }
    o->push_count = 0;
    o->push_busy = false;
    o->have_client = false;
    o->files_ok = false;
    o->client_fd = -1;
    o->clip_ready = false;
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

bool ph_origin_put_file(ph_origin *o, const char *path)
{
    long long size;
    char name[96];

    if (!o || !path || !ph_path_absolute(path) || strlen(path) >= sizeof o->push_queue[0])
        return false;
    size = ph_file_size(path);
    if (size <= 0 || (unsigned long long)size > PH_PUSH_MAX)
        return false;
    if (!safe_name(path, name, sizeof name))
        return false;
    pthread_mutex_lock(&o->lock);
    /* Nothing to send to: say so now rather than show a send that never moves. */
    if (!o->have_client || o->push_count == PH_PUSH_QUEUE) {
        pthread_mutex_unlock(&o->lock);
        return false;
    }
    if (!o->push_busy) {
        snprintf(o->push_busy_name, sizeof o->push_busy_name, "%s", name);
        o->push_size = (unsigned long long)size;
        o->push_done = 0;
    }
    snprintf(o->push_queue[o->push_count++], sizeof o->push_queue[0], "%s", path);
    o->push_busy = true;
    pthread_cond_signal(&o->cond);
    pthread_mutex_unlock(&o->lock);
    PH_LOGI("send: queued %s", path);
    return true;
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
    o->push_count = 0;
    pthread_mutex_unlock(&o->lock);
}

int ph_origin_take_push(ph_origin *o, char *name, size_t name_len)
{
    int note;

    if (!o)
        return 0;
    pthread_mutex_lock(&o->lock);
    note = o->note_count ? o->notes[0] : 0;
    if (name && name_len)
        snprintf(name, name_len, "%s", note ? o->note_names[0] : "");
    if (o->note_count) {
        o->note_count--;
        memmove(o->notes, o->notes + 1, sizeof o->notes[0] * (size_t)o->note_count);
        memmove(o->note_names, o->note_names + 1, sizeof o->note_names[0] * (size_t)o->note_count);
    }
    pthread_mutex_unlock(&o->lock);
    return note;
}

void ph_origin_forget(ph_origin *o)
{
    int client = -1;

    if (!o)
        return;
    pthread_mutex_lock(&o->lock);
    o->known[0] = 0;
    client = o->client_fd;
    pthread_mutex_unlock(&o->lock);
    if (client >= 0)
        shutdown(client, SHUT_RDWR);
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
        ph_sock_close(fd);
    }
    return NULL;
}

ph_origin *ph_origin_start(char *hint, size_t hint_len)
{
    ph_origin *o;
    int fd;
    char ip[64], token[16];

    o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->push_fd = -1;
    if (!ph_config_read_line("pull-token", token, sizeof token) || !token_ok(token)) {
        make_token(token, sizeof token);
        ph_config_write_line("pull-token", token);
    }
    snprintf(o->token, sizeof o->token, "%s", token);
    fd = ph_sock_listen(PH_PULL_PORT, false, 4);
    if (fd < 0) {
        PH_LOGW("photo pull: port %d is unavailable", PH_PULL_PORT);
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
    advertise_pc(o, ip);
    fill_hint(hint, hint_len, ip, o->token);
    PH_LOGI("photo pull: %s:%d code %s", ip, PH_PULL_PORT, o->token);
    return o;
}

bool ph_origin_renew(ph_origin *o, char *hint, size_t hint_len)
{
    char token[16], ip[64];
    int client = -1;

    if (!o)
        return false;
    make_token(token, sizeof token);
    if (!ph_config_write_line("pull-token", token))
        return false;
    pthread_mutex_lock(&o->lock);
    snprintf(o->token, sizeof o->token, "%s", token);
    o->known[0] = 0;
    client = o->client_fd;
    pthread_mutex_unlock(&o->lock);
    if (client >= 0)
        shutdown(client, SHUT_RDWR);
    lan_address(ip, sizeof ip);
    fill_hint(hint, hint_len, ip, token);
    PH_LOGI("photo pull: new code %s", token);
    return true;
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
    pthread_mutex_unlock(&o->lock);
    if (client >= 0)
        shutdown(client, SHUT_RDWR);
    shutdown(o->listen_fd, SHUT_RDWR);
    pthread_join(o->thread, NULL);
    if (o->send_thread_started)
        pthread_join(o->send_thread, NULL);
    ph_sock_close(o->listen_fd);
    ph_sock_close(o->push_fd);
    ph_bonjour_stop(o->bonjour);
    free(o->out_data);
    free(o->in_data);
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    free(o);
}

void ph_origin_pc_name(char *out, size_t len)
{
    service_name(out, len);
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
    if (o->note_ready)
        flags |= PH_ORIGIN_CLIP_READY;
    o->note_sent = false;
    o->note_got = false;
    o->note_ready = false;
    pthread_mutex_unlock(&o->lock);
    return flags;
}

