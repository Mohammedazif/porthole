#ifndef PORTHOLE_CORE_SOCK_H
#define PORTHOLE_CORE_SOCK_H

/* TCP sockets that work the same on Linux and Windows. A socket is an int: Windows
 * socket handles are kernel handles, which always fit in 32 bits. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

typedef int ph_socklen;
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#define ph_poll(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))

static inline bool ph_sock_init(void)
{
    static bool ready;
    WSADATA data;

    if (!ready)
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return ready;
}

static inline int ph_sock_open(void)
{
    SOCKET s;

    if (!ph_sock_init())
        return -1;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return -1;
    /* Not inherited by helper processes Porthole starts. */
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    return (int)s;
}

static inline int ph_sock_accept(int fd, struct sockaddr_in *peer)
{
    int len = sizeof *peer;
    SOCKET s = accept((SOCKET)fd, (struct sockaddr *)peer, peer ? &len : NULL);
    return s == INVALID_SOCKET ? -1 : (int)s;
}

static inline void ph_sock_close(int fd)
{
    if (fd >= 0)
        closesocket((SOCKET)fd);
}

/* Seconds a blocking recv waits before it fails. */
static inline void ph_sock_timeout(int fd, int seconds)
{
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
}

/* Windows refuses a second listener on the port with SO_EXCLUSIVEADDRUSE, the same
 * guarantee SO_REUSEADDR gives on Linux without letting another app steal it. */
static inline void ph_sock_reuse(int fd)
{
    BOOL on = TRUE;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
}

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef socklen_t ph_socklen;
#define ph_poll(fds, n, ms) poll((fds), (nfds_t)(n), (ms))

static inline bool ph_sock_init(void)
{
    return true;
}

static inline int ph_sock_open(void)
{
    return socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
}

static inline int ph_sock_accept(int fd, struct sockaddr_in *peer)
{
    socklen_t len = sizeof *peer;
    int s = accept(fd, (struct sockaddr *)peer, peer ? &len : NULL);
    if (s >= 0)
        fcntl(s, F_SETFD, FD_CLOEXEC);
    return s;
}

static inline void ph_sock_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

static inline void ph_sock_timeout(int fd, int seconds)
{
    struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static inline void ph_sock_reuse(int fd)
{
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
}

#endif

static inline void ph_sock_nodelay(int fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const void *)&on, sizeof on);
}

/* A phone that leaves Wi-Fi sends no FIN; keepalive turns that into an error. */
static inline void ph_sock_keepalive(int fd, int idle_s, int interval_s, int count)
{
    int on = 1;

    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const void *)&on, sizeof on);
#ifdef TCP_KEEPIDLE
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (const void *)&idle_s, sizeof idle_s);
#endif
#ifdef TCP_KEEPINTVL
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (const void *)&interval_s, sizeof interval_s);
#endif
#ifdef TCP_KEEPCNT
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, (const void *)&count, sizeof count);
#endif
#if defined(_WIN32) && !defined(TCP_KEEPIDLE)
    {
        struct tcp_keepalive ka = {1, (ULONG)idle_s * 1000, (ULONG)interval_s * 1000};
        DWORD out = 0;
        WSAIoctl((SOCKET)fd, SIO_KEEPALIVE_VALS, &ka, sizeof ka, NULL, 0, &out, NULL, NULL);
    }
#endif
    (void)count;
}

/* An IPv4 listener on port. loopback_only keeps it to this PC. -1 on failure. */
static inline int ph_sock_listen(unsigned short port, bool loopback_only, int backlog)
{
    struct sockaddr_in addr;
    int fd = ph_sock_open();

    if (fd < 0)
        return -1;
    ph_sock_reuse(fd);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, backlog) != 0) {
        ph_sock_close(fd);
        return -1;
    }
    return fd;
}

#endif
