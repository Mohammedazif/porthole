#include "files/inbox.h"

#include "core/bonjour.h"
#include "core/log.h"
#include "files/pull.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ph_inbox_parse_hello(const char *line)
{
    int version = 0, count = -1;
    char extra;

    if (!line || sscanf(line, "PSHR %d %d%c", &version, &count, &extra) != 2)
        return -1;
    if (version != 1 || count < 1 || count > PH_INBOX_MAX_FILES)
        return -1;
    return count;
}

#include "core/sock.h"
#include "core/upath.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* The largest single file accepted, the same as a pulled video. */
#define INBOX_FILE_MAX (16ULL * 1024 * 1024 * 1024)
/* A phone that stops sending for this long has gone. */
#define INBOX_IDLE_MS 60000

struct ph_inbox {
    ph_inbox_callbacks cb;
    char dir[512];
    int listen_fd;
    pthread_t thread;
    atomic_bool stop;
    ph_bonjour *bonjour;
};

static bool wait_readable(ph_inbox *in, int fd)
{
    int waited = 0;

    while (!atomic_load(&in->stop) && waited < INBOX_IDLE_MS) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = ph_poll(&p, 1, 250);

        if (r > 0)
            return true;
        if (r < 0 && errno != EINTR)
            return false;
        waited += 250;
    }
    return false;
}

static bool read_line(ph_inbox *in, int fd, char *buf, size_t len)
{
    size_t n = 0;

    while (n + 1 < len) {
        char c;

        if (!wait_readable(in, fd) || recv(fd, &c, 1, 0) != 1)
            return false;
        if (c == '\n') {
            buf[n] = 0;
            if (n && buf[n - 1] == '\r')
                buf[n - 1] = 0;
            return true;
        }
        buf[n++] = c;
    }
    return false;
}

static bool send_text(int fd, const char *text)
{
    int len = (int)strlen(text);
    return send(fd, text, len, MSG_NOSIGNAL) == len;
}

/* A file name not yet used in folder: IMG_1.JPG, then 2-IMG_1.JPG and so on. */
static bool free_path(const char *folder, const char *name, char *out, size_t len)
{
    if (snprintf(out, len, "%s/%s", folder, name) >= (int)len)
        return false;
    for (int n = 2; ph_path_exists(out); n++) {
        if (n > 999 || snprintf(out, len, "%s/%d-%s", folder, n, name) >= (int)len)
            return false;
    }
    return true;
}

static bool receive_file(ph_inbox *in, int fd, const char *path, unsigned long long size)
{
    static char chunk[1 << 16];
    FILE *out = ph_fopen(path, "wb");
    bool ok = out != NULL;

    while (ok && size > 0) {
        size_t want = size < sizeof chunk ? (size_t)size : sizeof chunk;
        int got;

        if (!wait_readable(in, fd))
            ok = false;
        else if ((got = (int)recv(fd, chunk, (int)want, 0)) <= 0)
            ok = false;
        else if (fwrite(chunk, 1, (size_t)got, out) != (size_t)got)
            ok = false;
        else
            size -= (unsigned long long)got;
    }
    if (out && fclose(out) != 0)
        ok = false;
    if (!ok)
        ph_remove(path);
    return ok;
}

static void serve(ph_inbox *in, int fd, const char *address)
{
    char line[256], folder[640], path[800], name[96];
    char *paths = NULL;
    size_t used = 0, cap = 0;
    int total, done = 0;
    time_t now = time(NULL);
    struct tm tm;
    unsigned long long size;

    if (!read_line(in, fd, line, sizeof line) || (total = ph_inbox_parse_hello(line)) < 0) {
        PH_LOGW("share: %s did not say hello", address);
        return;
    }
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    snprintf(folder, sizeof folder, "%s/shared-%04d%02d%02d-%02d%02d%02d", in->dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (ph_mkdir(in->dir) != 0 && errno != EEXIST)
        return;
    if (ph_mkdir(folder) != 0 && errno != EEXIST) {
        PH_LOGW("share: cannot create %s", folder);
        return;
    }
    PH_LOGI("share: %s is sending %d item(s)", address, total);
    if (in->cb.progress)
        in->cb.progress(in->cb.user, 0, total);
    while (!atomic_load(&in->stop) && read_line(in, fd, line, sizeof line)) {
        if (!strcmp(line, "DONE")) {
            send_text(fd, "BYE\n");
            break;
        }
        if (done >= total || !ph_pull_parse_file(line, INBOX_FILE_MAX, &size, name, sizeof name)
            || !free_path(folder, name, path, sizeof path)) {
            PH_LOGW("share: bad file line, closing");
            break;
        }
        if (!receive_file(in, fd, path, size)) {
            PH_LOGW("share: %s stopped during %s", address, name);
            break;
        }
        if (used + strlen(path) + 2 > cap) {
            size_t want = (cap ? cap * 2 : 4096) + strlen(path);
            char *grown = realloc(paths, want);
            if (!grown)
                break;
            paths = grown;
            cap = want;
        }
        used += (size_t)sprintf(paths + used, "%s%s", used ? "\n" : "", path);
        done++;
        if (!send_text(fd, "OK\n"))
            break;
        if (in->cb.progress)
            in->cb.progress(in->cb.user, done, total);
    }
    PH_LOGI("share: received %d of %d item(s) into %s", done, total, folder);
    if (done > 0 && in->cb.received)
        in->cb.received(in->cb.user, paths, done);
    else
        ph_rmdir(folder);
    free(paths);
}

static void *inbox_main(void *arg)
{
    ph_inbox *in = arg;

    while (!atomic_load(&in->stop)) {
        struct pollfd p = {.fd = in->listen_fd, .events = POLLIN};
        struct sockaddr_in peer;
        char address[INET_ADDRSTRLEN] = "";
        int fd;

        if (ph_poll(&p, 1, 250) <= 0)
            continue;
        fd = ph_sock_accept(in->listen_fd, &peer);
        if (fd < 0)
            continue;
        inet_ntop(AF_INET, &peer.sin_addr, address, sizeof address);
        if (in->cb.allow && !in->cb.allow(in->cb.user, address)) {
            PH_LOGW("share: refused %s (open the Porthole app on that iPhone first)", address);
            ph_sock_close(fd);
            continue;
        }
        ph_sock_nodelay(fd);
        serve(in, fd, address);
        ph_sock_close(fd);
    }
    return NULL;
}

/* "dir=<folder>" as one DNS-SD TXT field, so the share sheet can name the folder. */
static void advertise(ph_inbox *in, const char *label)
{
    char name[64], field[96], clean[80];
    unsigned char txt[98];
    uint16_t len = 0;
    size_t i = 0;
    int n;

    ph_bonjour_stop(in->bonjour);
    in->bonjour = NULL;
    ph_bonjour_host_name(name, sizeof name);
    if (!label || !label[0])
        label = "Downloads";
    for (; label[i] && i + 1 < sizeof clean; i++)
        clean[i] = (label[i] == '\n' || label[i] == '\r') ? ' ' : label[i];
    clean[i] = 0;
    n = snprintf(field, sizeof field, "dir=%s", clean);
    if (n > 0 && n < 256) {
        txt[0] = (unsigned char)n;
        memcpy(txt + 1, field, (size_t)n);
        len = (uint16_t)(n + 1);
    }
    in->bonjour = ph_bonjour_advertise(name, PH_INBOX_SERVICE, PH_INBOX_PORT,
                                       len ? txt : NULL, len);
}

void ph_inbox_set_save_label(ph_inbox *inbox, const char *save_label)
{
    if (inbox)
        advertise(inbox, save_label);
}

ph_inbox *ph_inbox_start(const char *dir, const char *save_label, const ph_inbox_callbacks *callbacks)
{
    ph_inbox *in = calloc(1, sizeof *in);

    if (!in || !callbacks || !dir || !dir[0])
        goto fail;
    in->cb = *callbacks;
    snprintf(in->dir, sizeof in->dir, "%s", dir);
    in->listen_fd = ph_sock_listen(PH_INBOX_PORT, false, 2);
    if (in->listen_fd < 0) {
        PH_LOGW("share: port %d is unavailable", PH_INBOX_PORT);
        goto fail;
    }
    atomic_init(&in->stop, false);
    if (pthread_create(&in->thread, NULL, inbox_main, in) != 0) {
        ph_sock_close(in->listen_fd);
        goto fail;
    }
    ph_inbox_set_save_label(in, save_label);
    PH_LOGI("share: listening on port %d", PH_INBOX_PORT);
    return in;
fail:
    free(in);
    return NULL;
}

void ph_inbox_stop(ph_inbox *in)
{
    if (!in)
        return;
    atomic_store(&in->stop, true);
    shutdown(in->listen_fd, SHUT_RDWR);
    pthread_join(in->thread, NULL);
    ph_sock_close(in->listen_fd);
    ph_bonjour_stop(in->bonjour);
    free(in);
}
