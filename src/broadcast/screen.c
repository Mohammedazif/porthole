#include "broadcast/screen.h"

#include "core/log.h"
#include "core/metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t ph_screen_parse_header(const uint8_t header[5], uint32_t max_len, int *type)
{
    uint32_t len;

    if (!header || !type)
        return 0;
    len = (uint32_t)header[0] << 24 | (uint32_t)header[1] << 16 | (uint32_t)header[2] << 8
          | header[3];
    *type = header[4];
    if (len == 0 || len > max_len || (*type != 1 && *type != 2))
        return 0;
    return len;
}

#include "core/sock.h"
#include "core/secure_link.h"

#include <pthread.h>
#include <stdatomic.h>

struct ph_screen {
    ph_screen_callbacks cb;
    int listen_fd, client_fd;
    pthread_t thread;
    pthread_mutex_t client_lock;
    atomic_bool stop, disconnect;
};

/* Allow the sender's initial frame deadline to expire before closing an idle stream. */
static const uint64_t frame_header_timeout_ms = 12000;

static bool read_all(ph_screen *s, int fd, void *buf, size_t len, uint64_t deadline)
{
    uint8_t *p = buf;

    while (len > 0) {
        uint64_t now = ph_metrics_now_ms();
        if (now >= deadline)
            return false;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = ph_tls_pending(fd) ? 1 : ph_poll(&pfd, 1, 250);
        int got;

        if (atomic_load(&s->stop) || atomic_load(&s->disconnect) || !ph_tls_authorized())
            return false;
        if (pr < 0)
            return false;
        if (pr == 0)
            continue;
        got = ph_tls_read_until(fd, p, (int)(len > 65536 ? 65536 : len), deadline);
        if (got <= 0)
            return false;
        p += got;
        len -= (size_t)got;
    }
    return true;
}

static bool read_hello(ph_screen *s, int fd)
{
    char line[32];
    size_t n = 0;
    uint64_t deadline = ph_metrics_now_ms() + 5000;

    while (n + 1 < sizeof line) {
        char c;
        if (!read_all(s, fd, &c, 1, deadline))
            return false;
        if (c == '\n')
            break;
        line[n++] = c;
    }
    line[n] = 0;
    return !strcmp(line, "PSCR 1");
}

static void serve(ph_screen *s, int fd, const char *address)
{
    uint8_t header[5];
    uint8_t *frame = malloc(PH_SCREEN_FRAME_MAX);
    unsigned long frames = 0;

    if (!frame)
        return;
    ph_sock_nodelay(fd);
    ph_sock_keepalive(fd, 10, 3, 3);
    if (!read_hello(s, fd)) {
        PH_LOGW("screen broadcast: %s did not say hello", address);
        free(frame);
        return;
    }
    PH_LOGI("screen broadcast: %s connected", address);
    if (s->cb.connected)
        s->cb.connected(s->cb.user, address);
    while (!atomic_load(&s->stop)) {
        int type = 0;
        uint32_t len;

        if (!read_all(s, fd, header, sizeof header, ph_metrics_now_ms() + frame_header_timeout_ms))
            break;
        len = ph_screen_parse_header(header, PH_SCREEN_FRAME_MAX, &type);
        if (!len) {
            PH_LOGW("screen broadcast: bad frame header, closing");
            break;
        }
        if (!read_all(s, fd, frame, len, ph_metrics_now_ms() + 5000))
            break;
        if (s->cb.video)
            s->cb.video(s->cb.user, frame, len, type == 2);
        frames++;
        /* One byte per frame taken: the phone keeps at most a couple of frames
         * unanswered and skips the rest, so Wi-Fi never queues seconds of video. */
        if (ph_tls_write(fd, "A", 1) != 1)
            break;
    }
    PH_LOGI("screen broadcast: %s stopped after %lu frames", address, frames);
    if (s->cb.disconnected)
        s->cb.disconnected(s->cb.user);
    free(frame);
}

static void *screen_main(void *arg)
{
    ph_screen *s = arg;

    while (!atomic_load(&s->stop)) {
        struct pollfd p = {.fd = s->listen_fd, .events = POLLIN};
        struct sockaddr_in peer;
        char address[INET_ADDRSTRLEN] = "";
        int fd;

        if (ph_poll(&p, 1, 250) <= 0)
            continue;
        fd = ph_sock_accept(s->listen_fd, &peer);
        if (fd < 0)
            continue;
        inet_ntop(AF_INET, &peer.sin_addr, address, sizeof address);
        pthread_mutex_lock(&s->client_lock);
        if (atomic_load(&s->stop)) {
            pthread_mutex_unlock(&s->client_lock);
            ph_sock_close(fd);
            break;
        }
        s->client_fd = fd;
        atomic_store(&s->disconnect, false);
        pthread_mutex_unlock(&s->client_lock);
        if (s->cb.allow && !s->cb.allow(s->cb.user, address)) {
            PH_LOGW("screen broadcast: refused %s (open the Porthole app on that iPhone first)",
                    address);
        } else {
            if (ph_tls_accept(fd) && ph_tls_authorize(fd, s->cb.authorize, s->cb.user))
                serve(s, fd, address);
            ph_tls_end();
        }
        pthread_mutex_lock(&s->client_lock);
        s->client_fd = -1;
        ph_sock_close(fd);
        pthread_mutex_unlock(&s->client_lock);
    }
    return NULL;
}

ph_screen *ph_screen_start(const ph_screen_callbacks *callbacks)
{
    ph_screen *s = calloc(1, sizeof *s);

    if (!s || !callbacks)
        goto fail;
    s->cb = *callbacks;
    s->listen_fd = ph_sock_listen(PH_SCREEN_PORT, false, 2);
    if (s->listen_fd < 0) {
        PH_LOGW("screen broadcast: port %d is unavailable", PH_SCREEN_PORT);
        goto fail;
    }
    atomic_init(&s->stop, false);
    atomic_init(&s->disconnect, false);
    pthread_mutex_init(&s->client_lock, NULL);
    s->client_fd = -1;
    if (pthread_create(&s->thread, NULL, screen_main, s) != 0) {
        pthread_mutex_destroy(&s->client_lock);
        ph_sock_close(s->listen_fd);
        goto fail;
    }
    PH_LOGI("screen broadcast: listening on port %d", PH_SCREEN_PORT);
    return s;
fail:
    free(s);
    return NULL;
}

void ph_screen_stop(ph_screen *s)
{
    if (!s)
        return;
    atomic_store(&s->stop, true);
    ph_screen_disconnect(s);
    shutdown(s->listen_fd, SHUT_RDWR);
    pthread_join(s->thread, NULL);
    ph_sock_close(s->listen_fd);
    pthread_mutex_destroy(&s->client_lock);
    free(s);
}

void ph_screen_disconnect(ph_screen *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->client_lock);
    atomic_store(&s->disconnect, true);
    if (s->client_fd >= 0)
        shutdown(s->client_fd, SHUT_RDWR);
    pthread_mutex_unlock(&s->client_lock);
}
