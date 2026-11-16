#include "hid/link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/metrics.h"
#include "hid/link_proto.h"

#ifdef _WIN32

/* The Windows helper (WinRT Bluetooth) arrives with the Windows port. */
ph_hid_link *ph_hid_link_start(char *const argv[], ph_hid_link_status_fn on_status, void *user)
{
    (void)argv, (void)on_status, (void)user;
    PH_LOGW("Bluetooth control isn't available on Windows yet");
    return NULL;
}

static bool send_frame(ph_hid_link *l, uint8_t type, const uint8_t *payload, size_t len)
{
    (void)l, (void)type, (void)payload, (void)len;
    return false;
}

void ph_hid_link_stop(ph_hid_link *l)
{
    (void)l;
}

void ph_hid_link_request_stop(ph_hid_link *l) { (void)l; }
bool ph_hid_link_poll(ph_hid_link *l) { (void)l; return true; }

#else

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EOF_GRACE_MS 1000
#define TERM_GRACE_MS 1000

struct ph_hid_link {
    pid_t pid;
    int to_helper;   /* write end of the helper's stdin */
    int from_helper; /* read end of the helper's stdout */
    pthread_t reader;
    ph_hid_link_status_fn on_status;
    void *user;
    bool dead;
    bool reader_started, stop_requested;
    unsigned int stop_phase;
    uint64_t stop_deadline;
    atomic_bool child_reaped, output_closed, reader_finished;
};

static void sleep_briefly(void)
{
    struct timespec pause = {0, 10 * 1000 * 1000};
    while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
}

static void status_line(ph_hid_link *l, char line[512], size_t *used)
{
    line[*used] = '\0';
    if (*used && l->on_status)
        l->on_status(l->user, line);
    *used = 0;
}

static void *reader_thread(void *arg)
{
    ph_hid_link *l = arg;
    char line[512], data[512];
    size_t used = 0;
    uint64_t drain_until = 0;
    for (;;) {
        if (atomic_load(&l->child_reaped)) {
            if (!drain_until)
                drain_until = ph_metrics_now_ms() + 100;
            else if (ph_metrics_now_ms() >= drain_until)
                break;
        }
        struct pollfd fd = {l->from_helper, POLLIN, 0};
        int ready = poll(&fd, 1, 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!ready) {
            /* A descendant could retain stdout after the direct child exits.
             * Do not let that descriptor hold the reader join forever. */
            if (atomic_load(&l->child_reaped))
                break;
            continue;
        }
        ssize_t count = read(l->from_helper, data, sizeof data);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        for (ssize_t i = 0; i < count; i++) {
            if (data[i] == '\n')
                status_line(l, line, &used);
            else if (data[i] != '\r') {
                line[used++] = data[i];
                if (used == sizeof line - 1)
                    status_line(l, line, &used);
            }
        }
    }
    status_line(l, line, &used);
    close(l->from_helper);
    atomic_store(&l->output_closed, true);
    /* EOF alone is not proof of process exit. A stuck helper may close stdout
     * before hanging; poll() must first finish the bounded process shutdown. */
    while (!atomic_load(&l->child_reaped))
        sleep_briefly();
    if (l->on_status)
        l->on_status(l->user, "exited");
    atomic_store(&l->reader_finished, true);
    return NULL;
}

ph_hid_link *ph_hid_link_start(char *const argv[], ph_hid_link_status_fn on_status, void *user)
{
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    ph_hid_link *l = calloc(1, sizeof *l);

    if (!l)
        return NULL;
    l->to_helper = -1;
    l->from_helper = -1;
    atomic_init(&l->child_reaped, false);
    atomic_init(&l->output_closed, false);
    atomic_init(&l->reader_finished, false);
    /* A dead helper must make writes fail with EPIPE, not kill the app. */
    signal(SIGPIPE, SIG_IGN);
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0)
        goto fail;

    l->pid = fork();
    if (l->pid < 0)
        goto fail;
    if (l->pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "porthole: couldn't run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    l->to_helper = in_pipe[1];
    l->from_helper = out_pipe[0];
    fcntl(l->to_helper, F_SETFD, FD_CLOEXEC);
    fcntl(l->from_helper, F_SETFD, FD_CLOEXEC);
    l->on_status = on_status;
    l->user = user;
    int error = pthread_create(&l->reader, NULL, reader_thread, l);
    if (error != 0) {
        PH_LOGE("couldn't start Bluetooth status reader: %s", strerror(error));
        close(l->from_helper);
        l->from_helper = -1;
        atomic_store(&l->reader_finished, true);
        ph_hid_link_stop(l);
        return NULL;
    }
    l->reader_started = true;
    return l;

fail:
    PH_LOGE("couldn't start the Bluetooth helper: %s", strerror(errno));
    for (int i = 0; i < 2; i++) {
        if (in_pipe[i] >= 0)
            close(in_pipe[i]);
        if (out_pipe[i] >= 0)
            close(out_pipe[i]);
    }
    free(l);
    return NULL;
}

static bool send_frame(ph_hid_link *l, uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t buf[PH_LINK_MAX_FRAME];
    size_t n = ph_link_frame(type, payload, len, buf), off = 0;

    if (!l || l->dead || l->to_helper < 0 || n == 0)
        return false;
    if (ph_log_get_level() <= PH_LOG_DEBUG) {
        char hex[3 * PH_LINK_MAX_FRAME + 1] = "";
        for (size_t i = 0; i < len; i++)
            snprintf(hex + 3 * i, sizeof hex - 3 * i, "%02x ", payload[i]);
        PH_LOGD("hid -> %s: %s", type == PH_LINK_KEYBOARD   ? "keyboard"
                                 : type == PH_LINK_MOUSE    ? "mouse"
                                 : type == PH_LINK_CONSUMER ? "consumer"
                                                            : "control",
                hex);
    }
    while (off < n) {
        ssize_t w = write(l->to_helper, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            PH_LOGE("the Bluetooth helper stopped (%s)", strerror(errno));
            l->dead = true;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

void ph_hid_link_request_stop(ph_hid_link *l)
{
    if (!l || l->stop_requested)
        return;
    l->stop_requested = true;
    l->dead = true;
    if (l->to_helper >= 0) {
        close(l->to_helper); /* allow normal unregister/queue drain first */
        l->to_helper = -1;
    }
    l->stop_phase = 1;
    l->stop_deadline = ph_metrics_now_ms() + EOF_GRACE_MS;
}

bool ph_hid_link_poll(ph_hid_link *l)
{
    if (!l || atomic_load(&l->child_reaped))
        return true;
    pid_t result;
    do {
        result = waitpid(l->pid, NULL, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == l->pid || (result < 0 && errno == ECHILD)) {
        if (l->to_helper >= 0) {
            close(l->to_helper);
            l->to_helper = -1;
        }
        l->dead = true;
        atomic_store(&l->child_reaped, true);
        return true;
    }
    if (atomic_load(&l->output_closed) && !l->stop_requested)
        ph_hid_link_request_stop(l);
    if (l->stop_requested && ph_metrics_now_ms() >= l->stop_deadline) {
        if (l->stop_phase == 1) {
            kill(l->pid, SIGTERM);
            l->stop_phase = 2;
            l->stop_deadline = ph_metrics_now_ms() + TERM_GRACE_MS;
        } else if (l->stop_phase == 2) {
            kill(l->pid, SIGKILL);
            l->stop_phase = 3;
        }
    }
    return false;
}

void ph_hid_link_stop(ph_hid_link *l)
{
    if (!l)
        return;
    ph_hid_link_request_stop(l);
    while (!ph_hid_link_poll(l) || !atomic_load(&l->reader_finished))
        sleep_briefly();
    if (l->reader_started)
        pthread_join(l->reader, NULL);
    free(l);
}

#endif

bool ph_hid_link_keyboard(ph_hid_link *l, const ph_hid_keyboard *k)
{
    uint8_t r[PH_HID_KEYBOARD_REPORT_LEN];
    ph_hid_build_keyboard(k, r);
    return send_frame(l, PH_LINK_KEYBOARD, r, sizeof r);
}

bool ph_hid_link_mouse(ph_hid_link *l, const ph_hid_mouse *m)
{
    uint8_t r[PH_HID_MOUSE_REPORT_LEN];
    ph_hid_build_mouse(m, r);
    return send_frame(l, PH_LINK_MOUSE, r, sizeof r);
}

bool ph_hid_link_consumer(ph_hid_link *l, uint16_t usage)
{
    uint8_t r[PH_HID_CONSUMER_REPORT_LEN];
    ph_hid_build_consumer(usage, r);
    return send_frame(l, PH_LINK_CONSUMER, r, sizeof r);
}

bool ph_hid_link_sync(ph_hid_link *l, uint32_t id)
{
    uint8_t r[4] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24)};
    return send_frame(l, PH_LINK_SYNC, r, sizeof r);
}

bool ph_hid_link_delay(ph_hid_link *l, uint16_t milliseconds)
{
    uint8_t r[2] = {(uint8_t)milliseconds, (uint8_t)(milliseconds >> 8)};
    return send_frame(l, PH_LINK_DELAY, r, sizeof r);
}
