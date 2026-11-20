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

#if defined(__linux__)

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct ph_origin {
    int listen_fd;
    int client_fd;
    pthread_t thread;
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
    FILE *f = fopen("/dev/urandom", "rb");

    if (!f || fread(bytes, 1, sizeof bytes, f) != sizeof bytes) {
        bytes[0] = 0xA1;
        bytes[1] = 0xB2;
        bytes[2] = 0xC3;
        bytes[3] = 0xD4;
    }
    if (f)
        fclose(f);
    snprintf(out, len, "%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3]);
}

static void fill_hint(char *hint, size_t hint_len, const char *ip, const char *token)
{
    if (hint && hint_len)
        snprintf(hint, hint_len, "%s:%d  %s", ip, PH_PULL_PORT, token);
}

static void service_name(char *out, size_t len)
{
    char *dot;

    if (!out || len < 2)
        return;
    memset(out, 0, len);
    if (gethostname(out, len - 1) != 0 || !out[0])
        snprintf(out, len, "Porthole");
    dot = strchr(out, '.');
    if (dot == out || !out[0])
        snprintf(out, len, "Porthole");
    else if (dot)
        *dot = 0;
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
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

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
    out = fopen(part, "wb");
    if (!out)
        return false;
    while (left) {
        char chunk[65536];
        size_t want = left > sizeof chunk ? sizeof chunk : (size_t)left;
        ssize_t got = recv(fd, chunk, want, 0);

        if (got <= 0) {
            fclose(out);
            remove(part);
            return false;
        }
        if (fwrite(chunk, 1, (size_t)got, out) != (size_t)got) {
            fclose(out);
            remove(part);
            return false;
        }
        left -= (unsigned long long)got;
    }
    if (fclose(out) != 0) {
        remove(part);
        return false;
    }
    if (rename(part, path) != 0) {
        remove(part);
        return false;
    }
    return true;
}

static bool read_exact(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len) {
        ssize_t n = recv(fd, p, len, 0);

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

/* Reads clipboard messages that arrived while no photo request is in flight,
 * then sends one waiting PC clipboard item. */
static bool service_idle(ph_origin *o, int fd)
{
    int i;

    for (i = 0; i < 8; i++) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        char line[160];
        bool consumed = false;
        int pr = poll(&p, 1, 0);

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

    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
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
    struct timeval tv = {.tv_sec = 300, .tv_usec = 0};
    int on = 1, idle = 15, interval = 5, count = 3;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    /* A phone that leaves Wi-Fi sends no FIN; keepalive turns that into an error. */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
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
    o->have_client = false;
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

static void *origin_main(void *arg)
{
    ph_origin *o = arg;

    while (!o->stop) {
        struct pollfd p = {.fd = o->listen_fd, .events = POLLIN};
        int pr = poll(&p, 1, 200);
        int fd;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;

        if (pr <= 0)
            continue;
        fd = accept(o->listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&o->lock);
        inet_ntop(AF_INET, &peer.sin_addr, o->peer, sizeof o->peer);
        pthread_mutex_unlock(&o->lock);
        serve_client(o, fd);
        close(fd);
    }
    return NULL;
}

ph_origin *ph_origin_start(char *hint, size_t hint_len)
{
    ph_origin *o;
    struct sockaddr_in addr;
    int fd, on = 1;
    char ip[64], token[16];

    o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    if (!ph_config_read_line("pull-token", token, sizeof token) || !token_ok(token)) {
        make_token(token, sizeof token);
        ph_config_write_line("pull-token", token);
    }
    snprintf(o->token, sizeof o->token, "%s", token);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        free(o);
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PH_PULL_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        PH_LOGW("photo pull: port %d is unavailable", PH_PULL_PORT);
        close(fd);
        free(o);
        return NULL;
    }
    o->listen_fd = fd;
    o->client_fd = -1;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
    if (pthread_create(&o->thread, NULL, origin_main, o) != 0) {
        close(fd);
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
    close(o->listen_fd);
    ph_bonjour_stop(o->bonjour);
    free(o->out_data);
    free(o->in_data);
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    free(o);
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

#else

struct ph_origin {
    int unused;
};

ph_origin *ph_origin_start(char *hint, size_t hint_len)
{
    (void)hint;
    (void)hint_len;
    return NULL;
}

void ph_origin_stop(ph_origin *origin)
{
    (void)origin;
}

bool ph_origin_renew(ph_origin *origin, char *hint, size_t hint_len)
{
    (void)origin;
    if (hint && hint_len)
        hint[0] = 0;
    return false;
}

bool ph_origin_ask(ph_origin *origin, uint64_t hash, double aspect, const uint8_t *thumb,
                   const uint8_t *detail, const ph_origin_screen *screen, const char *dir)
{
    (void)screen;
    (void)origin;
    (void)hash;
    (void)aspect;
    (void)thumb;
    (void)detail;
    (void)dir;
    return false;
}

bool ph_origin_peer(ph_origin *origin, char *address, size_t len)
{
    (void)origin;
    if (address && len)
        address[0] = 0;
    return false;
}

ph_origin_result ph_origin_take(ph_origin *origin, char *path, size_t path_len)
{
    (void)origin;
    (void)path;
    (void)path_len;
    return PH_ORIGIN_IDLE;
}

bool ph_origin_put_clip(ph_origin *origin, char kind, const void *data, size_t len)
{
    (void)origin;
    (void)kind;
    (void)data;
    (void)len;
    return false;
}

bool ph_origin_take_clip(ph_origin *origin, char *kind, void **data, size_t *len)
{
    (void)origin;
    (void)kind;
    if (data)
        *data = NULL;
    if (len)
        *len = 0;
    return false;
}

unsigned ph_origin_clip_flags(ph_origin *origin)
{
    (void)origin;
    return 0;
}

#endif
