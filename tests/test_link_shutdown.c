/* Main-loop recovery must not block rendering while a helper ignores EOF/TERM. */
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/metrics.h"
#include "hid/link.h"

typedef struct observed {
    atomic_int pid;
    atomic_bool ready, eof, term, exited, reaped;
} observed;

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; \
} } while (0)

static void pause_briefly(void)
{
    nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
}

static void on_status(void *user, const char *line)
{
    observed *events = user;
    int pid;
    if (sscanf(line, "pid %d", &pid) == 1)
        atomic_store(&events->pid, pid);
    else if (!strcmp(line, "ready"))
        atomic_store(&events->ready, true);
    else if (!strcmp(line, "eof-ignored"))
        atomic_store(&events->eof, true);
    else if (!strcmp(line, "term-ignored"))
        atomic_store(&events->term, true);
    else if (!strcmp(line, "exited")) {
        /* stdout EOF is insufficient: the actual child must already be reaped. */
        errno = 0;
        pid_t result = waitpid(atomic_load(&events->pid), NULL, WNOHANG);
        atomic_store(&events->reaped, result == -1 && errno == ECHILD);
        atomic_store(&events->exited, true);
    }
}

static ph_hid_link *start(observed *events, char *mode)
{
    char *args[] = {"python3", "-u", PORTHOLE_TEST_STUCK_HELPER, mode, NULL};
    ph_hid_link *link = ph_hid_link_start(args, on_status, events);
    CHECK(link);
    uint64_t deadline = ph_metrics_now_ms() + 1500;
    while (link && !atomic_load(&events->ready) && ph_metrics_now_ms() < deadline)
        pause_briefly();
    CHECK(atomic_load(&events->ready) && atomic_load(&events->pid) > 0);
    return link;
}

static void pump(ph_hid_link *link, observed *events, bool repeat_stop)
{
    uint64_t began = ph_metrics_now_ms();
    while (!atomic_load(&events->exited) && ph_metrics_now_ms() - began < 3500) {
        uint64_t call = ph_metrics_now_ms();
        if (repeat_stop)
            ph_hid_link_request_stop(link); /* idempotent: never extends the deadline */
        ph_hid_link_poll(link);
        CHECK(ph_metrics_now_ms() - call < 100);
        pause_briefly();
    }
    CHECK(atomic_load(&events->exited) && atomic_load(&events->reaped));
    if (!atomic_load(&events->exited)) {
        kill(atomic_load(&events->pid), SIGKILL);
        for (int i = 0; i < 100 && !atomic_load(&events->exited); i++) {
            ph_hid_link_poll(link);
            pause_briefly();
        }
    }
}

int main(void)
{
    observed events = {0};
    ph_hid_link *link = start(&events, "hung");
    if (!link)
        return 1;
    uint64_t began = ph_metrics_now_ms();
    ph_hid_link_request_stop(link);
    CHECK(ph_metrics_now_ms() - began < 100);
    CHECK(!ph_hid_link_sync(link, 1));
    CHECK(!ph_hid_link_consumer(link, 0));
    pump(link, &events, true);
    CHECK(ph_metrics_now_ms() - began < 3500);
    CHECK(atomic_load(&events.eof) && atomic_load(&events.term));
    CHECK(ph_hid_link_poll(link));
    began = ph_metrics_now_ms();
    ph_hid_link_stop(link);
    CHECK(ph_metrics_now_ms() - began < 100); /* exited-handler cleanup is immediate */

    /* Final shutdown has the same bounded escalation even without event-loop polling. */
    observed final = {0};
    link = start(&final, "hung");
    if (!link)
        return 1;
    began = ph_metrics_now_ms();
    ph_hid_link_stop(link);
    CHECK(ph_metrics_now_ms() - began < 3500);
    CHECK(atomic_load(&final.eof) && atomic_load(&final.term));
    CHECK(atomic_load(&final.exited) && atomic_load(&final.reaped));

    /* Another SIGCHLD owner may already have reaped the process. Never signal a
     * recycled PID or wait forever after waitpid reports ECHILD. */
    observed natural = {0};
    link = start(&natural, "exit");
    if (!link)
        return 1;
    pid_t result;
    do {
        result = waitpid(atomic_load(&natural.pid), NULL, 0);
    } while (result < 0 && errno == EINTR);
    CHECK(result == atomic_load(&natural.pid));
    pump(link, &natural, false);
    ph_hid_link_stop(link);

    /* A helper closing stdout while still alive must not emit exited early. */
    observed closed = {0};
    link = start(&closed, "closed-output");
    if (!link)
        return 1;
    CHECK(!atomic_load(&closed.exited));
    pump(link, &closed, false);
    ph_hid_link_stop(link);

    ph_hid_link_request_stop(NULL);
    CHECK(ph_hid_link_poll(NULL));
    ph_hid_link_stop(NULL);
    if (failures)
        return 1;
    puts("nonblocking stop, EOF/TERM/KILL escalation, reaping and bounded final shutdown passed");
    return 0;
}
