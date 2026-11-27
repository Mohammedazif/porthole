#include "files/inbox.h"

#include "core/log.h"
#include "files/pull.h"
#include "files/digest.h"
#include "files/places.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ph_inbox_parse_hello(const char *line)
{
    int count = -1;
    char extra;

    if (!line || strncmp(line, "PSHR ", 5) || strlen(line) < 8 || line[5] < '1' ||
        line[5] > '3' || line[6] != ' ' || sscanf(line + 7, "%d%c", &count, &extra) != 1 ||
        count < 1 || count > PH_INBOX_MAX_FILES)
        return -1;
    return count;
}

#include "core/sock.h"
#include "core/secure_link.h"
#include "core/metrics.h"
#include "core/upath.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/statvfs.h>
#endif

#include <openssl/rand.h>

#define INBOX_FILE_MAX PH_TRANSFER_MAX_BYTES
/* Unfinished files left behind by earlier transfers may use this much; the
 * file arriving now needs only room on the disk. */
#define INBOX_STAGING_MAX (8ULL * 1024 * 1024 * 1024)
#define INBOX_PAIR_STAGING_MAX (4ULL * 1024 * 1024 * 1024)
#define INBOX_FREE_RESERVE (32ULL * 1024 * 1024)
#define INBOX_PART_MAX_AGE (7 * 24 * 60 * 60)
#define INBOX_RECEIPT_MAX_AGE (30 * 24 * 60 * 60)
/* A phone that stops sending for this long has gone. */
#define INBOX_IDLE_MS 60000

struct ph_inbox {
    ph_inbox_callbacks cb;
    char dir[512];
    int listen_fd, client_fd;
    pthread_t thread;
    pthread_mutex_t client_lock;
    atomic_bool stop, disconnect;
    ph_inbox_item item;
};

static bool wait_readable(ph_inbox *in, int fd, uint64_t deadline)
{
    while (!atomic_load(&in->stop) && !atomic_load(&in->disconnect)) {
        uint64_t now = ph_metrics_now_ms();
        if (now >= deadline || !ph_tls_authorized())
            break;
        int remaining = (int)(deadline - now);
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int n = ph_tls_pending(fd) ? 1 : ph_poll(&p, 1, remaining > 250 ? 250 : remaining);
        if (n > 0)
            return true;
        if (n < 0 && errno != EINTR)
            break;
    }
    return false;
}

static bool read_line(ph_inbox *in, int fd, char *buf, size_t len)
{
    size_t n = 0;
    uint64_t deadline = ph_metrics_now_ms() + 10000;

    while (n + 1 < len) {
        char c;
        if (!wait_readable(in, fd, deadline) || ph_tls_read_until(fd, &c, 1, deadline) != 1 || !c)
            return false;
        if (c == '\n') {
            if (n && buf[n - 1] == '\r')
                n--;
            buf[n] = 0;
            return true;
        }
        buf[n++] = c;
    }
    return false;
}

static bool send_text(int fd, const char *text)
{
    int len = (int)strlen(text);
    return ph_tls_write(fd, text, len) == len;
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

static bool receive_file(ph_inbox *in, int fd, const char *path, unsigned long long size,
                         bool resume)
{
    char chunk[1 << 16];
    FILE *out = ph_fopen(path, resume ? "ab" : "wbx");
    bool ok = out != NULL;
    double next_update = ph_metrics_now_ms() + 200;
    while (ok && size > 0) {
        size_t want = size < sizeof chunk ? (size_t)size : sizeof chunk;
        int got;

        if (!wait_readable(in, fd, ph_metrics_now_ms() + INBOX_IDLE_MS))
            ok = false;
        else if ((got = (int)ph_tls_read(fd, chunk, (int)want)) <= 0)
            ok = false;
        else if (fwrite(chunk, 1, (size_t)got, out) != (size_t)got)
            ok = false;
        else {
            size -= (unsigned long long)got;
            if (resume) {
                in->item.bytes = in->item.total - size;
                double now = ph_metrics_now_ms();
                if (in->cb.item && now >= next_update) {
                    in->cb.item(in->cb.user, &in->item);
                    next_update = now + 200;
                }
            }
        }
    }
    if (out) {
        if (!ph_flush_file(out))
            ok = false;
        if (fclose(out) != 0)
            ok = false;
    }
    if (out && !ok && !resume)
        ph_remove(path);
    return ok;
}

/* A UUID identifies the sender's private, immutable snapshot. The accepted name and
 * length are bound to it before any bytes are written. A receipt survives a lost OK. */
static bool write_line_file(const char *path, const char *line)
{
    char temporary[1200];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary)
        return false;
    FILE *f = ph_fopen(temporary, "wb");
    if (!f)
        return false;
    bool ok = fprintf(f, "%s\n", line) > 0 && ph_flush_file(f);
    if (fclose(f) != 0)
        ok = false;
    if (ok && ph_rename(temporary, path) == 0)
        return true;
    ph_remove(temporary);
    return false;
}
static bool read_line_file(const char *path, char *line, size_t length)
{
    FILE *f = ph_fopen(path, "rb");
    if (!f)
        return false;
    bool ok = fgets(line, (int)length, f) != NULL;
    fclose(f);
    if (ok)
        line[strcspn(line, "\r\n")] = 0;
    return ok;
}
static bool copy_complete(const char *source, const char *target)
{
    FILE *in = ph_fopen(source, "rb");
    if (!in)
        return false;
    FILE *out = ph_fopen(target, "wbx");
    bool ok = out != NULL;
    char buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in) && ph_flush_file(out);
    fclose(in);
    if (out && fclose(out) != 0)
        ok = false;
    if (out && !ok)
        ph_remove(target);
    return ok;
}
static bool volume_has_space(const char *folder, uint64_t required)
{
#ifdef _WIN32
    wchar_t *wide = ph_wide(folder);
    ULARGE_INTEGER available;
    bool enough = wide && GetDiskFreeSpaceExW(wide, &available, NULL, NULL) &&
                  available.QuadPart >= required;
    free(wide);
    return enough;
#else
    struct statvfs space;
    return statvfs(folder, &space) == 0 && space.f_frsize &&
           space.f_bavail >= (required + space.f_frsize - 1) / space.f_frsize;
#endif
}

/* Only our private journal filenames match this shape. Never clean the receive folder. */
static void count_staging(const char *folder, const char *name, const char *current,
                          const char *profile, uint64_t *used, uint64_t *pair_used)
{
    if (strncmp(name, "transfer-", 9))
        return;
    size_t scope = strspn(name + 9, "0123456789abcdef");
    const char *suffix = name + 9 + scope;
    if (scope == 64 && *suffix == '-') {
        if (strspn(suffix + 1, "0123456789abcdef") != 32)
            return;
        suffix += 33;
    } else if (scope != 32) {
        return;
    }
    bool partial = !strcmp(suffix, ".part");
    if (!partial && strcmp(suffix, ".saved") && strcmp(suffix, ".info") &&
        strcmp(suffix, ".target") && !strstr(suffix, ".tmp"))
        return;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", folder, name);
    if (n < 0 || (size_t)n >= sizeof path)
        return;
    if (current && !strcmp(ph_path_base(current), name))
        return;
    time_t modified = ph_file_mtime(path);
    time_t now = time(NULL);
    int age = !strcmp(suffix, ".saved") || !strcmp(suffix, ".info")
                  ? INBOX_RECEIPT_MAX_AGE
                  : INBOX_PART_MAX_AGE;
    if (modified > 0 && now > modified && now - modified >= age && ph_remove(path) == 0)
        return;
    long long size = ph_file_size(path);
    if (partial && size > 0) {
        *used += (uint64_t)size;
        if (scope == 64 && !memcmp(name + 9, profile, 64))
            *pair_used += (uint64_t)size;
    }
}

static bool staging_has_space(const char *folder, const char *current, const char *profile,
                              uint64_t size)
{
    uint64_t used = 0, pair_used = 0;
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/transfer-*", folder);
    wchar_t *wide = ph_wide(pattern);
    if (!wide)
        return false;
    WIN32_FIND_DATAW item;
    HANDLE scan = FindFirstFileW(wide, &item);
    free(wide);
    if (scan != INVALID_HANDLE_VALUE) {
        do {
            char name[512];
            if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                ph_narrow(item.cFileName, name, sizeof name))
                count_staging(folder, name, current, profile, &used, &pair_used);
        } while (FindNextFileW(scan, &item));
        FindClose(scan);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return false;
    }
#else
    DIR *scan = opendir(folder);
    if (!scan)
        return false;
    struct dirent *item;
    while ((item = readdir(scan)))
        count_staging(folder, item->d_name, current, profile, &used, &pair_used);
    closedir(scan);
#endif
    (void)size; /* the arriving file is checked against free space instead */
    return pair_used <= INBOX_PAIR_STAGING_MAX && used <= INBOX_STAGING_MAX;
}

static bool file_matches(ph_inbox *in, const char *path, const char *hash, uint64_t size)
{
    if (ph_file_size(path) != (long long)size)
        return false;
    FILE *file = ph_fopen(path, "rb");
    if (!file)
        return false;
    char digest[65];
    bool ok = !atomic_load(&in->stop) && ph_file_digest(file, digest, NULL, NULL) &&
              !strcmp(digest, hash);
    fclose(file);
    return ok;
}

static bool receive_resumable(ph_inbox *in, int fd, const char *header, bool verify_digest)
{
    char id[33], name[96], meta[256], info[256], location[1024], digest[65] = "";
    char prefix[65], file_header[256];
    const char *file_info = header + 38;
    unsigned long long size = 0;
    if (strncmp(header, "ITEM ", 5) || strlen(header) < 39 || header[37] != ' ')
        return false;
    memcpy(id, header + 5, 32);
    id[32] = 0;
    if (strspn(id, "0123456789abcdef") != 32)
        return false;
    if (verify_digest) {
        if (strlen(file_info) < 67 || file_info[64] != ' ')
            return false;
        memcpy(digest, file_info, 64);
        if (strspn(digest, "0123456789abcdef") != 64)
            return false;
        snprintf(file_header, sizeof file_header, "FILE %s", file_info + 65);
        file_info = file_header;
    }
    if (!ph_pull_parse_file(file_info, INBOX_FILE_MAX, &size, name, sizeof name))
        return false;
    if (!in->cb.profile || !in->cb.profile(in->cb.user, ph_tls_ticket(), prefix) ||
        strlen(prefix) != 64 || strspn(prefix, "0123456789abcdef") != 64)
        return false;
    snprintf(meta, sizeof meta, "%s %llu %s", digest, size, name);
    char base[640], part[660], receipt[660], descriptor[660], plan[660];
    snprintf(base, sizeof base, "%s/transfer-%s-%s", in->dir, prefix, id);
    snprintf(part, sizeof part, "%s.part", base);
    snprintf(receipt, sizeof receipt, "%s.saved", base);
    snprintf(descriptor, sizeof descriptor, "%s.info", base);
    snprintf(plan, sizeof plan, "%s.target", base);
    in->item = (ph_inbox_item){.total = size, .state = PH_INBOX_RECEIVING};
    memcpy(in->item.id, id, sizeof id);
    memcpy(in->item.digest, digest, sizeof digest);
    snprintf(in->item.name, sizeof in->item.name, "%s", name);
    bool described = ph_path_exists(descriptor);
    if (described) {
        if (!read_line_file(descriptor, info, sizeof info) || strcmp(info, meta))
            return false;
    }
    if (described && read_line_file(receipt, location, sizeof location)) {
        in->item.state = PH_INBOX_STORED;
        in->item.bytes = size;
        snprintf(in->item.path, sizeof in->item.path, "%s", location);
        if (in->cb.item)
            in->cb.item(in->cb.user, &in->item);
        return send_text(fd, "SAVED\n");
    }
    long long existing = ph_file_size(part);
    unsigned long long offset = existing < 0 ? 0 : (unsigned long long)existing;
    if (offset > size)
        return false;
    if (!staging_has_space(in->dir, part, prefix, size) ||
        !volume_has_space(in->dir, size - offset + INBOX_FREE_RESERVE)) {
        snprintf(in->item.message, sizeof in->item.message,
                 "Receive storage limit reached. Free space and retry from your iPhone.");
        return false;
    }
    if (!described && !write_line_file(descriptor, meta))
        return false;
    snprintf(info, sizeof info, "OFFSET %llu\n", offset);
    in->item.bytes = offset;
    if (in->cb.item)
        in->cb.item(in->cb.user, &in->item);
    if (!send_text(fd, info) || !receive_file(in, fd, part, size - offset, true))
        return false;
    if (verify_digest && !file_matches(in, part, digest, size)) {
        ph_remove(part);
        in->item.bytes = 0;
        snprintf(in->item.message, sizeof in->item.message,
                 "File integrity check failed. Retry from your iPhone.");
        return false;
    }
    char folder[1024], temporary[1100];
    if (!ph_place_save_dir(folder, sizeof folder)) {
        snprintf(in->item.message, sizeof in->item.message,
                 "Receive folder unavailable. Choose a folder and retry from your iPhone.");
        return false;
    }
    if (snprintf(temporary, sizeof temporary, "%s/.porthole-%.12s-%s.download", folder, prefix, id) >=
        (int)sizeof temporary)
        return false;
    bool planned = read_line_file(plan, location, sizeof location);
    /* The private journal is written before publication. If publication succeeded
     * before a crash, comparing the saved bytes avoids publishing a second copy. */
    bool published = false;
    if (planned && verify_digest)
        published = file_matches(in, location, digest, size);
    if (planned && !verify_digest && ph_file_size(location) == (long long)size) {
        FILE *a = ph_fopen(part, "rb"), *b = ph_fopen(location, "rb");
        char x[16384], y[16384];
        published = a && b;
        while (published) {
            size_t n = fread(x, 1, sizeof x, a);
            if (fread(y, 1, n, b) != n || memcmp(x, y, n)) {
                published = false;
                break;
            }
            if (n < sizeof x) {
                published = !ferror(a) && !ferror(b);
                break;
            }
        }
        if (a)
            fclose(a);
        if (b)
            fclose(b);
    }
    if (!published) {
        if (!volume_has_space(folder, size + INBOX_FREE_RESERVE)) {
            snprintf(in->item.message, sizeof in->item.message,
                     "Receive folder storage is full. Free space or choose another folder.");
            return false;
        }
        /* O_EXCL avoids truncating any file that was already in the destination. */
        for (int i = 2; ph_path_exists(temporary); i++) {
            if (i > 999 || snprintf(temporary, sizeof temporary, "%s/.porthole-%.12s-%s-%d.download",
                                    folder, prefix, id, i) >= (int)sizeof temporary)
                return false;
        }
        if (!free_path(folder, name, location, sizeof location) ||
            !write_line_file(plan, location) || !copy_complete(part, temporary))
            return false;
        if (!ph_tls_authorized() || !ph_publish_new(temporary, location)) {
            ph_remove(temporary);
            return false;
        }
    }
    if (!ph_tls_authorized() || !write_line_file(receipt, location))
        return false;
    ph_remove(part);
    ph_remove(plan);
    in->item.state = PH_INBOX_STORED;
    snprintf(in->item.path, sizeof in->item.path, "%s", location);
    if (in->cb.item)
        in->cb.item(in->cb.user, &in->item);
    if (in->cb.saved)
        in->cb.saved(in->cb.user, location);
    return send_text(fd, "OK\n");
}

static void serve(ph_inbox *in, int fd, const char *address)
{
    char line[352], path[1024], name[96];
    char *paths = NULL;
    size_t used = 0, cap = 0;
    int total, done = 0;
    unsigned long long size;

    if (!read_line(in, fd, line, sizeof line) || (total = ph_inbox_parse_hello(line)) < 0) {
        PH_LOGW("share: %s did not say hello", address);
        return;
    }
    if (ph_mkdir(in->dir) != 0 && errno != EEXIST)
        return;
    if (line[5] == '2' || line[5] == '3') {
        bool verify_digest = line[5] == '3';
        memset(&in->item, 0, sizeof in->item);
        if (in->cb.progress)
            in->cb.progress(in->cb.user, 0, total);
        while (done < total && read_line(in, fd, line, sizeof line) &&
               receive_resumable(in, fd, line, verify_digest)) {
            done++;
            memset(&in->item, 0, sizeof in->item);
            if (in->cb.progress)
                in->cb.progress(in->cb.user, done, total);
        }
        if (done == total && read_line(in, fd, line, sizeof line) && !strcmp(line, "DONE"))
            send_text(fd, "BYE\n");
        if (done < total) {
            if (in->item.id[0] && in->item.state != PH_INBOX_STORED) {
                in->item.state = PH_INBOX_INTERRUPTED;
                if (!in->item.message[0])
                    snprintf(in->item.message, sizeof in->item.message, "%s",
                             errno == ENOSPC
                                 ? "Storage full. Free space and retry from your iPhone."
                                 : "Transfer interrupted. Retry from your iPhone.");
                if (in->cb.item)
                    in->cb.item(in->cb.user, &in->item);
            }
            send_text(fd, "ERROR\n");
            if (in->cb.progress && (!in->cb.item || !in->item.id[0]))
                in->cb.progress(in->cb.user, -1, total);
        }
        return;
    }
    char folder[1024];
    if (!ph_place_save_dir(folder, sizeof folder))
        return;
    if (in->cb.progress)
        in->cb.progress(in->cb.user, 0, total);
    while (done < total && read_line(in, fd, line, sizeof line)) {
        if (!ph_pull_parse_file(line, INBOX_FILE_MAX, &size, name, sizeof name) ||
            !volume_has_space(in->dir, size + INBOX_FREE_RESERVE) ||
            !volume_has_space(folder, size + INBOX_FREE_RESERVE))
            break;
        unsigned char nonce[16];
        if (RAND_bytes(nonce, sizeof nonce) != 1)
            break;
        ph_inbox_item item = {.total = size, .bytes = size, .state = PH_INBOX_STORED};
        for (size_t i = 0; i < sizeof nonce; i++)
            snprintf(item.id + i * 2, 3, "%02x", nonce[i]);
        char staged[640];
        snprintf(staged, sizeof staged, "%s/transfer-%s.part", in->dir, item.id);
        if (!receive_file(in, fd, staged, size, false))
            break;
        bool saved = ph_tls_authorized() &&
                     ph_place_copy_unique(staged, folder, name, path, sizeof path);
        ph_remove(staged);
        if (!saved)
            break;
        done++;
        if (in->cb.item) {
            snprintf(item.name, sizeof item.name, "%s", name);
            snprintf(item.path, sizeof item.path, "%s", path);
            in->cb.item(in->cb.user, &item);
        }
        if (in->cb.saved)
            in->cb.saved(in->cb.user, path);
        if (in->cb.received) {
            if (used + strlen(path) + 2 > cap) {
                size_t want = (cap ? cap * 2 : 4096) + strlen(path);
                char *grown = realloc(paths, want);
                if (!grown)
                    break;
                paths = grown;
                cap = want;
            }
            used += (size_t)sprintf(paths + used, "%s%s", used ? "\n" : "", path);
        }
        if (!send_text(fd, "OK\n"))
            break;
        if (in->cb.progress)
            in->cb.progress(in->cb.user, done, total);
    }
    if (done == total && read_line(in, fd, line, sizeof line) && !strcmp(line, "DONE"))
        send_text(fd, "BYE\n");
    if (paths && in->cb.received)
        in->cb.received(in->cb.user, paths, done);
    if (done < total) {
        send_text(fd, "ERROR\n");
        if (in->cb.progress)
            in->cb.progress(in->cb.user, -1, total);
    }
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
        pthread_mutex_lock(&in->client_lock);
        if (atomic_load(&in->stop)) {
            pthread_mutex_unlock(&in->client_lock);
            ph_sock_close(fd);
            break;
        }
        in->client_fd = fd;
        atomic_store(&in->disconnect, false);
        pthread_mutex_unlock(&in->client_lock);
        if (in->cb.allow && !in->cb.allow(in->cb.user, address)) {
            PH_LOGW("share: refused %s (open the Porthole app on that iPhone first)", address);
        } else {
            ph_sock_nodelay(fd);
            if (ph_tls_accept(fd) && ph_tls_authorize(fd, in->cb.authorize, in->cb.user))
                serve(in, fd, address);
            ph_tls_end();
        }
        pthread_mutex_lock(&in->client_lock);
        in->client_fd = -1;
        ph_sock_close(fd);
        pthread_mutex_unlock(&in->client_lock);
    }
    return NULL;
}

ph_inbox *ph_inbox_start(const char *dir, const ph_inbox_callbacks *callbacks)
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
    atomic_init(&in->disconnect, false);
    pthread_mutex_init(&in->client_lock, NULL);
    in->client_fd = -1;
    if (pthread_create(&in->thread, NULL, inbox_main, in) != 0) {
        pthread_mutex_destroy(&in->client_lock);
        ph_sock_close(in->listen_fd);
        goto fail;
    }
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
    ph_inbox_disconnect(in);
    shutdown(in->listen_fd, SHUT_RDWR);
    pthread_join(in->thread, NULL);
    ph_sock_close(in->listen_fd);
    pthread_mutex_destroy(&in->client_lock);
    free(in);
}

void ph_inbox_disconnect(ph_inbox *in)
{
    if (!in)
        return;
    pthread_mutex_lock(&in->client_lock);
    atomic_store(&in->disconnect, true);
    if (in->client_fd >= 0)
        shutdown(in->client_fd, SHUT_RDWR);
    pthread_mutex_unlock(&in->client_lock);
}
