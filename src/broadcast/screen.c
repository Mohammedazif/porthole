#include "broadcast/screen.h"

#include "core/bonjour.h"
#include "core/log.h"

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

#if defined(__linux__)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

struct ph_screen {
    ph_screen_callbacks cb;
    int listen_fd;
    pthread_t thread;
    atomic_bool stop;
    ph_bonjour *bonjour;
};

static bool read_all(ph_screen *s, int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len > 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 250);
        ssize_t got;

        if (atomic_load(&s->stop))
            return false;
        if (pr < 0)
            return false;
        if (pr == 0)
            continue;
        got = recv(fd, p, len, 0);
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

    while (n + 1 < sizeof line) {
        char c;
        if (!read_all(s, fd, &c, 1))
            return false;
        if (c == '\n')
            break;
        line[n++] = c;
    }
    line[n] = 0;
    return strncmp(line, "PSCR ", 5) == 0;
}

static void serve(ph_screen *s, int fd, const char *address)
{
    uint8_t header[5];
    uint8_t *frame = malloc(PH_SCREEN_FRAME_MAX);
    int on = 1, idle = 10, interval = 3, count = 3;
    unsigned long frames = 0;

    if (!frame)
        return;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
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

        if (!read_all(s, fd, header, sizeof header))
            break;
        len = ph_screen_parse_header(header, PH_SCREEN_FRAME_MAX, &type);
        if (!len) {
            PH_LOGW("screen broadcast: bad frame header, closing");
            break;
        }
        if (!read_all(s, fd, frame, len))
            break;
        if (s->cb.video)
            s->cb.video(s->cb.user, frame, len, type == 2);
        frames++;
        /* One byte per frame taken: the phone keeps at most a couple of frames
         * unanswered and skips the rest, so Wi-Fi never queues seconds of video. */
        if (send(fd, "A", 1, MSG_NOSIGNAL) != 1)
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
        socklen_t peer_len = sizeof peer;
        char address[INET_ADDRSTRLEN] = "";
        int fd;

        if (poll(&p, 1, 250) <= 0)
            continue;
        fd = accept(s->listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0)
            continue;
        inet_ntop(AF_INET, &peer.sin_addr, address, sizeof address);
        if (s->cb.allow && !s->cb.allow(s->cb.user, address)) {
            PH_LOGW("screen broadcast: refused %s (open the Porthole app on that iPhone first)",
                    address);
            close(fd);
            continue;
        }
        serve(s, fd, address);
        close(fd);
    }
    return NULL;
}

ph_screen *ph_screen_start(const ph_screen_callbacks *callbacks)
{
    ph_screen *s = calloc(1, sizeof *s);
    struct sockaddr_in addr;
    int on = 1;

    if (!s || !callbacks)
        goto fail;
    s->cb = *callbacks;
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0)
        goto fail;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PH_SCREEN_PORT);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0
        || listen(s->listen_fd, 2) != 0) {
        PH_LOGW("screen broadcast: port %d is unavailable", PH_SCREEN_PORT);
        close(s->listen_fd);
        goto fail;
    }
    atomic_init(&s->stop, false);
    if (pthread_create(&s->thread, NULL, screen_main, s) != 0) {
        close(s->listen_fd);
        goto fail;
    }
    s->bonjour = ph_bonjour_advertise("Porthole", PH_SCREEN_SERVICE, PH_SCREEN_PORT);
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
    shutdown(s->listen_fd, SHUT_RDWR);
    pthread_join(s->thread, NULL);
    close(s->listen_fd);
    ph_bonjour_stop(s->bonjour);
    free(s);
}

#else

ph_screen *ph_screen_start(const ph_screen_callbacks *callbacks)
{
    (void)callbacks;
    return NULL;
}

void ph_screen_stop(ph_screen *screen)
{
    (void)screen;
}

#endif
