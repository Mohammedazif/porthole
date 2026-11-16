#include "hid/link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
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

#else

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct ph_hid_link {
    pid_t pid;
    int to_helper;   /* write end of the helper's stdin */
    int from_helper; /* read end of the helper's stdout */
    pthread_t reader;
    ph_hid_link_status_fn on_status;
    void *user;
    bool dead;
};

static void *reader_thread(void *arg)
{
    ph_hid_link *l = arg;
    FILE *in = fdopen(l->from_helper, "r");
    char line[512];

    if (in) {
        while (fgets(line, sizeof line, in)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] && l->on_status)
                l->on_status(l->user, line);
        }
        fclose(in); /* also closes from_helper */
    } else {
        close(l->from_helper);
    }
    if (l->on_status)
        l->on_status(l->user, "exited");
    return NULL;
}

ph_hid_link *ph_hid_link_start(char *const argv[], ph_hid_link_status_fn on_status, void *user)
{
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    ph_hid_link *l = calloc(1, sizeof *l);

    if (!l)
        return NULL;
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
    if (pthread_create(&l->reader, NULL, reader_thread, l) != 0) {
        close(l->to_helper);
        close(l->from_helper);
        waitpid(l->pid, NULL, 0);
        free(l);
        return NULL;
    }
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

    if (!l || l->dead || n == 0)
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

void ph_hid_link_stop(ph_hid_link *l)
{
    int status;

    if (!l)
        return;
    close(l->to_helper); /* EOF: the helper unregisters from BlueZ and exits */
    for (int i = 0; i < 40; i++) {
        if (waitpid(l->pid, &status, WNOHANG) == l->pid)
            goto reaped;
        nanosleep(&(struct timespec){0, 50 * 1000 * 1000}, NULL);
    }
    kill(l->pid, SIGTERM);
    waitpid(l->pid, &status, 0);
reaped:
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
