#include "touch/wda_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ph_sock;
#define PH_SOCK_BAD INVALID_SOCKET
#define ph_sock_close closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
typedef int ph_sock;
#define PH_SOCK_BAD (-1)
#define ph_sock_close close
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#include "core/log.h"
#include "touch/wda.h"

struct ph_wda_tcp {
    ph_wda *wda;
    ph_sock fd;
};

static int set_timeout(ph_sock fd)
{
#ifdef _WIN32
    DWORD ms = 800;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms) == 0 ? 0 : -1;
#else
    struct timeval tv = {.tv_sec = 0, .tv_usec = 800000};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0 ? 0 : -1;
#endif
}

static int connect_host(const char *host, int port, ph_sock *out)
{
    char service[16];
    struct addrinfo hints, *list = NULL, *it;
    ph_sock fd = PH_SOCK_BAD;

    snprintf(service, sizeof service, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, service, &hints, &list) != 0)
        return -1;
    for (it = list; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == PH_SOCK_BAD)
            continue;
#ifndef _WIN32
        {
            int flags = fcntl(fd, F_GETFL, 0);
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            int connected = 0;
            if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
                int rc = connect(fd, it->ai_addr, it->ai_addrlen);
                if (rc == 0 || errno == EINPROGRESS) {
                    if (poll(&pfd, 1, 1500) == 1 && (pfd.revents & POLLOUT)) {
                        int err = 0;
                        socklen_t len = sizeof err;
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                            connected = fcntl(fd, F_SETFL, flags) == 0;
                    }
                }
            }
            if (connected)
                break;
        }
#else
        if (connect(fd, it->ai_addr, (int)it->ai_addrlen) == 0)
            break;
#endif
        ph_sock_close(fd);
        fd = PH_SOCK_BAD;
    }
    freeaddrinfo(list);
    if (fd == PH_SOCK_BAD)
        return -1;
    if (set_timeout(fd) != 0) {
        ph_sock_close(fd);
        return -1;
    }
    *out = fd;
    return 0;
}

static int tcp_send(void *user, const uint8_t *data, size_t len)
{
    struct ph_wda_tcp *tcp = user;
    size_t sent = 0;
    char response[8192];
    size_t got = 0;

    if (!tcp || tcp->fd == PH_SOCK_BAD || !data)
        return -1;
    while (sent < len) {
#ifdef _WIN32
        int n = send(tcp->fd, (const char *)data + sent, (int)(len - sent), 0);
#else
        ssize_t n = send(tcp->fd, data + sent, len - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    while (got + 1 < sizeof response) {
#ifdef _WIN32
        int n = recv(tcp->fd, response + got, (int)(sizeof response - 1 - got), 0);
#else
        ssize_t n = recv(tcp->fd, response + got, sizeof response - 1 - got, 0);
#endif
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        got += (size_t)n;
        response[got] = '\0';
        if (strstr(response, "\r\n\r\n"))
            break;
    }
    response[got] = '\0';
    if (got && strstr(response, "\"sessionId\""))
        ph_wda_note_response(tcp->wda, response);
    return 0;
}

ph_wda_tcp *ph_wda_tcp_open(ph_wda *wda, const char *host, int port)
{
    ph_wda_tcp *tcp;

    if (!wda || !host)
        return NULL;
#ifdef _WIN32
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return NULL;
    }
#endif
    tcp = calloc(1, sizeof *tcp);
    if (!tcp)
        return NULL;
    tcp->wda = wda;
    tcp->fd = PH_SOCK_BAD;
    if (connect_host(host, port, &tcp->fd) != 0) {
        PH_LOGW("precise touch: couldn't reach WebDriverAgent at %s:%d", host, port);
        free(tcp);
        return NULL;
    }
    ph_wda_set_send(wda, tcp_send, tcp);
    return tcp;
}

void ph_wda_tcp_close(ph_wda_tcp *tcp)
{
    if (!tcp)
        return;
    if (tcp->fd != PH_SOCK_BAD)
        ph_sock_close(tcp->fd);
    free(tcp);
}
