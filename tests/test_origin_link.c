/* The iPhone app's link, end to end over loopback: the code, clipboard sync, and
 * two files queued through the local Send to port arriving in order, with progress
 * and one result each. Skips when Porthole itself already holds the ports. */
#include <SDL.h>

#include "core/sock.h"
#include "files/origin.h"
#include "files/places.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#endif

#ifdef _WIN32
#include <windows.h>
static void nap(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void nap(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static int dial(unsigned short port)
{
    struct sockaddr_in addr;
    int fd = ph_sock_open();

    CHECK(fd >= 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    CHECK(connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    ph_sock_timeout(fd, 5);
    return fd;
}

static void line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        CHECK(recv(fd, &c, 1, 0) == 1);
        if (c == '\n')
            break;
        buf[n++] = c;
    }
    buf[n] = 0;
}

static void say(int fd, const char *text)
{
    CHECK(send(fd, text, (int)strlen(text), MSG_NOSIGNAL) == (int)strlen(text));
}

static void write_file(const char *path, size_t size)
{
    FILE *f = fopen(path, "wb");
    CHECK(f);
    for (size_t i = 0; i < size; i++)
        fputc((int)(i * 7 % 251), f);
    CHECK(fclose(f) == 0);
}

/* Reads one PUSH and its bytes, checks them, and answers SAVED. */
static void take_push(int fd, const char *name, size_t size)
{
    char head[256], want[256];
    size_t left = size, at = 0;

    line(fd, head, sizeof head);
    snprintf(want, sizeof want, "PUSH %zu %s", size, name);
    CHECK(!strcmp(head, want));
    while (left) {
        char chunk[4096];
        int got = (int)recv(fd, chunk, (int)(left < sizeof chunk ? left : sizeof chunk), 0);
        CHECK(got > 0);
        for (int i = 0; i < got; i++, at++)
            CHECK((unsigned char)chunk[i] == (unsigned char)(at * 7 % 251));
        left -= (size_t)got;
    }
    say(fd, "SAVED\n");
}

int main(int argc, char **argv)
{
    char dir[256], hint[96], token[16] = "", reply[64], name[96], path_a[300], path_b[300];
    char *paths[2] = {path_a, path_b};
    ph_origin *o;
    int phone, results[2], percent_seen = -1;

    (void)argc;
    (void)argv;

    snprintf(dir, sizeof dir, "%s/porthole-origin-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
             (int)getpid());
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0700);
    setenv("XDG_CONFIG_HOME", dir, 1);
#endif
    o = ph_origin_start(hint, sizeof hint);
    if (!o) {
        puts("origin link: ports busy (Porthole running?), skipped");
        return 0;
    }
    CHECK(sscanf(hint, "%*s %15s", token) == 1 && strlen(token) == 8);
    CHECK(!ph_origin_put_file(o, "/does/not/exist"));

    /* A wrong code is turned away; the right one is welcomed with OK and SYNC. */
    phone = dial(PH_PULL_PORT);
    say(phone, "HELLO 00000000\n");
    CHECK(recv(phone, reply, 1, 0) <= 0);
    ph_sock_close(phone);
    phone = dial(PH_PULL_PORT);
    snprintf(reply, sizeof reply, "HELLO %s\n", token);
    say(phone, reply);
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "OK"));
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "SYNC"));
    say(phone, "SYNC\nFILES\n");
    nap(400);
    CHECK(ph_origin_clip_flags(o) & PH_ORIGIN_CLIP_READY);

    /* Two files through the local port, as Explorer's Send to hands them over. */
    snprintf(path_a, sizeof path_a, "%s/Q3 report.pdf", dir);
    snprintf(path_b, sizeof path_b, "%s/Résumé.txt", dir);
    write_file(path_a, 300000);
    write_file(path_b, 1234);
    CHECK(ph_place_send_to_running(2, paths) == 2);
    CHECK(ph_origin_push_busy(o, name, sizeof name));
    take_push(phone, "Q3 report.pdf", 300000);
    take_push(phone, "Résumé.txt", 1234);
    for (int i = 0, got = 0; i < 50 && got < 2; i++) {
        int note = ph_origin_take_push(o, name, sizeof name);
        int pct = ph_origin_push_percent(o);
        if (pct > percent_seen)
            percent_seen = pct;
        if (note)
            results[got++] = note;
        else
            nap(50);
        if (got == 2)
            break;
    }
    CHECK(results[0] == 1 && results[1] == 1);
    CHECK(!ph_origin_push_busy(o, NULL, 0));

    /* Clipboard text the PC copies reaches the phone on the same link. */
    CHECK(ph_origin_put_clip(o, 'T', "hello", 5));
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "CLIP T 5"));
    CHECK(recv(phone, reply, 5, 0) == 5 && !memcmp(reply, "hello", 5));

    /* The phone leaving fails anything still queued instead of showing it forever. */
    ph_sock_close(phone);
    nap(400);
    ph_origin_stop(o);
    remove(path_a);
    remove(path_b);
    puts("origin link: code, sync, queued Send to files with results, clipboard passed");
    return 0;
}
