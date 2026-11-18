#include "hid/link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/metrics.h"
#include "hid/link_proto.h"

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include <windows.h>
#include <process.h>
#include <stdatomic.h>
#include <wchar.h>

#define WIN_QUEUE_FRAMES 256
#define WIN_PIPE_BYTES 4096
#define WIN_EOF_GRACE_MS 2000
#define WIN_COMMAND_CHARS 32767

typedef struct win_frame {
    uint8_t data[PH_LINK_MAX_FRAME];
    DWORD len;
} win_frame;

struct ph_hid_link {
    HANDLE process, reader, to_helper, from_helper, write_event, read_event;
    OVERLAPPED write_io;
    bool write_pending, dead, stop_requested, terminated;
    DWORD write_offset;
    win_frame queue[WIN_QUEUE_FRAMES]; /* main-thread owned, including pending I/O storage */
    size_t head, queued;
    uint64_t stop_deadline;
    atomic_bool child_reaped, output_closed, reader_finished;
    ph_hid_link_status_fn on_status;
    void *user;
};

static void win_close(HANDLE *handle)
{
    if (*handle && *handle != INVALID_HANDLE_VALUE)
        CloseHandle(*handle);
    *handle = NULL;
}

static wchar_t *win_utf8(const char *text)
{
    if (!text || strlen(text) > 4U * WIN_COMMAND_CHARS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!length)
        return NULL;
    if (length > WIN_COMMAND_CHARS) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    wchar_t *wide = malloc((size_t)length * sizeof *wide);
    if (!wide) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static bool win_append(wchar_t *command, size_t *used, wchar_t ch)
{
    if (*used >= WIN_COMMAND_CHARS - 1) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    command[(*used)++] = ch;
    return true;
}

/* MS CRT quoting: double trailing backslashes and those before a literal quote.
 * Always quote each argument; never use cmd.exe or interpolate a shell command. */
static wchar_t *win_command(char *const argv[])
{
    wchar_t *command = calloc(WIN_COMMAND_CHARS, sizeof *command);
    if (!command) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; argv[i]; i++) {
        wchar_t *arg = win_utf8(argv[i]);
        if (!arg)
            goto fail;
        bool ok = (!i || win_append(command, &used, L' '))
                  && win_append(command, &used, L'"');
        const wchar_t *cursor = arg;
        while (ok && *cursor) {
            size_t slashes = 0;
            while (*cursor == L'\\') {
                slashes++;
                cursor++;
            }
            size_t copies = (*cursor == L'"' || !*cursor) ? slashes * 2 : slashes;
            for (size_t j = 0; ok && j < copies; j++)
                ok = win_append(command, &used, L'\\');
            if (*cursor == L'"')
                ok = ok && win_append(command, &used, L'\\');
            if (*cursor)
                ok = ok && win_append(command, &used, *cursor++);
        }
        ok = ok && win_append(command, &used, L'"');
        free(arg);
        if (!ok)
            goto fail;
    }
    command[used] = L'\0';
    return command;
fail:
    free(command);
    return NULL;
}

/* Parent endpoints are overlapped and noninheritable; only synchronous child
 * endpoints go in CreateProcess's explicit handle inheritance list. */
static bool win_pipe(HANDLE *parent, HANDLE *child, HANDLE event, bool input)
{
    static LONG serial;
    wchar_t name[128];
    swprintf(name, sizeof name / sizeof name[0], L"\\\\.\\pipe\\porthole-%lu-%ld",
             GetCurrentProcessId(), InterlockedIncrement(&serial));
    DWORD access = input ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;
    *parent = CreateNamedPipeW(name, access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                               WIN_PIPE_BYTES, WIN_PIPE_BYTES, 0, NULL);
    if (*parent == INVALID_HANDLE_VALUE) {
        *parent = NULL;
        return false;
    }
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    *child = CreateFileW(name, input ? GENERIC_READ : GENERIC_WRITE, 0, &inherit,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (*child == INVALID_HANDLE_VALUE) {
        *child = NULL;
        return false;
    }
    OVERLAPPED connected = {.hEvent = event};
    if (!ConnectNamedPipe(*parent, &connected)) {
        DWORD error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            if (error != ERROR_IO_PENDING)
                return false;
            /* Our client handle is already open, so the connection is established.
             * Retain the OVERLAPPED until even an asynchronous connect completes. */
            DWORD count;
            if (!GetOverlappedResult(*parent, &connected, &count, TRUE))
                return false;
        }
    }
    ResetEvent(event);
    return true;
}

static void win_status(ph_hid_link *l, char line[512], size_t *used)
{
    line[*used] = '\0';
    if (*used && strcmp(line, "exited") && l->on_status)
        l->on_status(l->user, line);
    *used = 0;
}

static unsigned __stdcall win_reader(void *arg)
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
        OVERLAPPED io = {.hEvent = l->read_event};
        ResetEvent(l->read_event);
        BOOL ready = ReadFile(l->from_helper, data, sizeof data, NULL, &io);
        DWORD error = ready ? ERROR_SUCCESS : GetLastError(), count = 0;
        if (!ready && error == ERROR_IO_PENDING) {
            HANDLE events[] = {l->read_event, l->process};
            DWORD signaled = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (signaled != WAIT_OBJECT_0)
                CancelIoEx(l->from_helper, &io);
            ready = GetOverlappedResult(l->from_helper, &io, &count, TRUE);
        } else if (ready) {
            ready = GetOverlappedResult(l->from_helper, &io, &count, FALSE);
        }
        if (!ready || !count)
            break;
        for (DWORD i = 0; i < count; i++) {
            if (data[i] == '\n')
                win_status(l, line, &used);
            else if (data[i] != '\r') {
                line[used++] = data[i];
                if (used == sizeof line - 1)
                    win_status(l, line, &used);
            }
        }
    }
    win_status(l, line, &used);
    atomic_store(&l->output_closed, true);
    while (!atomic_load(&l->child_reaped))
        Sleep(10);
    if (l->on_status)
        l->on_status(l->user, "exited");
    atomic_store(&l->reader_finished, true);
    return 0;
}

static void win_io_failed(ph_hid_link *l, DWORD error)
{
    if (!l->stop_requested)
        PH_LOGW("Bluetooth helper pipe failed (Windows error %lu); reconnecting", error);
    l->dead = true;
    l->queued = 0;
    l->write_offset = 0;
    ph_hid_link_request_stop(l);
}

static void win_pump(ph_hid_link *l)
{
    for (unsigned int work = 0; work < 16 && l->to_helper; work++) {
        if (l->write_pending) {
            DWORD written = 0;
            if (!GetOverlappedResult(l->to_helper, &l->write_io, &written, FALSE)) {
                DWORD error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE)
                    return;
                l->write_pending = false;
                win_io_failed(l, error);
            } else {
                l->write_pending = false;
                if (!written || !l->queued
                    || written > l->queue[l->head].len - l->write_offset) {
                    win_io_failed(l, ERROR_INVALID_DATA);
                } else {
                    l->write_offset += written;
                    if (l->write_offset == l->queue[l->head].len) {
                        l->head = (l->head + 1) % WIN_QUEUE_FRAMES;
                        l->queued--;
                        l->write_offset = 0;
                    }
                }
            }
        }
        if (atomic_load(&l->child_reaped)) {
            l->queued = 0;
            l->write_offset = 0;
        }
        if (!l->queued) {
            if (l->stop_requested || atomic_load(&l->child_reaped))
                win_close(&l->to_helper);
            return;
        }
        memset(&l->write_io, 0, sizeof l->write_io);
        l->write_io.hEvent = l->write_event;
        ResetEvent(l->write_event);
        win_frame *frame = &l->queue[l->head];
        BOOL ok = WriteFile(l->to_helper, frame->data + l->write_offset,
                             frame->len - l->write_offset, NULL, &l->write_io);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            win_io_failed(l, GetLastError());
            continue;
        }
        l->write_pending = true; /* persistent frame/event storage until completion */
    }
}

void ph_hid_link_request_stop(ph_hid_link *l)
{
    if (!l || l->stop_requested)
        return;
    l->dead = true;
    l->stop_requested = true;
    l->stop_deadline = ph_metrics_now_ms() + WIN_EOF_GRACE_MS;
    if (!l->write_pending && !l->queued)
        win_close(&l->to_helper);
}

bool ph_hid_link_poll(ph_hid_link *l)
{
    if (!l)
        return true;
    if (!atomic_load(&l->child_reaped) && WaitForSingleObject(l->process, 0) == WAIT_OBJECT_0) {
        l->dead = true;
        atomic_store(&l->child_reaped, true);
        if (l->write_pending)
            CancelIoEx(l->to_helper, &l->write_io);
    }
    if (atomic_load(&l->output_closed) && !l->stop_requested)
        ph_hid_link_request_stop(l);
    if (l->stop_requested && !l->terminated && !atomic_load(&l->child_reaped)
        && ph_metrics_now_ms() >= l->stop_deadline) {
        if (l->write_pending)
            CancelIoEx(l->to_helper, &l->write_io);
        TerminateProcess(l->process, 1);
        l->terminated = true;
    }
    win_pump(l);
    return atomic_load(&l->child_reaped);
}

void ph_hid_link_stop(ph_hid_link *l)
{
    if (!l)
        return;
    ph_hid_link_request_stop(l);
    while (!ph_hid_link_poll(l) || l->write_pending || !atomic_load(&l->reader_finished))
        Sleep(10);
    if (l->reader)
        WaitForSingleObject(l->reader, INFINITE);
    win_close(&l->reader);
    win_close(&l->to_helper);
    win_close(&l->from_helper);
    win_close(&l->write_event);
    win_close(&l->read_event);
    win_close(&l->process);
    free(l);
}

ph_hid_link *ph_hid_link_start(char *const argv[], ph_hid_link_status_fn on_status, void *user)
{
    if (!argv || !argv[0] || !argv[0][0])
        return NULL;
    wchar_t *command = win_command(argv);
    wchar_t *program = win_utf8(argv[0]);
    if (!command || !program) {
        PH_LOGE("invalid Bluetooth helper command (Windows error %lu)", GetLastError());
        free(command);
        free(program);
        return NULL;
    }
    /* Search explicitly, then pass lpApplicationName to avoid ambiguous paths. */
    DWORD needed = SearchPathW(NULL, program, L".exe", 0, NULL, NULL);
    wchar_t *executable = needed ? malloc(((size_t)needed + 1) * sizeof *executable) : NULL;
    if (!executable || !SearchPathW(NULL, program, L".exe", needed + 1, executable, NULL)) {
        PH_LOGE("couldn't find Bluetooth helper (Windows error %lu)", GetLastError());
        free(command); free(program); free(executable);
        return NULL;
    }
    free(program);
    ph_hid_link *l = calloc(1, sizeof *l);
    HANDLE input = NULL, output = NULL, error_output = NULL;
    STARTUPINFOEXW startup = {0};
    bool attributes_ready = false;
    SIZE_T attributes_size = 0;
    if (!l)
        goto fail;
    l->on_status = on_status;
    l->user = user;
    atomic_init(&l->child_reaped, false);
    atomic_init(&l->output_closed, false);
    atomic_init(&l->reader_finished, true);
    l->write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    l->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!l->write_event || !l->read_event
        || !win_pipe(&l->to_helper, &input, l->write_event, true)
        || !win_pipe(&l->from_helper, &output, l->read_event, false))
        goto fail;

    HANDLE parent_error = GetStdHandle(STD_ERROR_HANDLE);
    if (!parent_error || parent_error == INVALID_HANDLE_VALUE) {
        SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
        error_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &inherit, OPEN_EXISTING, 0, NULL);
        if (error_output == INVALID_HANDLE_VALUE) {
            error_output = NULL;
            goto fail;
        }
    } else if (!DuplicateHandle(GetCurrentProcess(), parent_error, GetCurrentProcess(),
                                 &error_output, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        goto fail;
    }
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    startup.lpAttributeList = malloc(attributes_size);
    if (!startup.lpAttributeList
        || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size))
        goto fail;
    attributes_ready = true;
    HANDLE inherited[] = {input, output, error_output};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof inherited, NULL, NULL))
        goto fail;
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = error_output;
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(executable, command, NULL, NULL, TRUE,
                         CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                         NULL, NULL, &startup.StartupInfo, &process))
        goto fail;
    l->process = process.hProcess;
    CloseHandle(process.hThread);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    startup.lpAttributeList = NULL;
    attributes_ready = false;
    win_close(&input); win_close(&output); win_close(&error_output);
    free(command); free(executable);
    command = executable = NULL;
    atomic_store(&l->reader_finished, false);
    l->reader = (HANDLE)_beginthreadex(NULL, 0, win_reader, l, 0, NULL);
    if (!l->reader) {
        PH_LOGE("couldn't start the Windows Bluetooth status reader");
        atomic_store(&l->reader_finished, true);
        ph_hid_link_stop(l);
        return NULL;
    }
    return l;
fail:
    PH_LOGE("couldn't start Bluetooth helper (Windows error %lu)", GetLastError());
    if (attributes_ready)
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    win_close(&input); win_close(&output); win_close(&error_output);
    if (l) {
        win_close(&l->to_helper); win_close(&l->from_helper);
        win_close(&l->write_event); win_close(&l->read_event);
        free(l);
    }
    free(command); free(executable);
    return NULL;
}

static bool send_frame(ph_hid_link *l, uint8_t type, const uint8_t *payload, size_t len)
{
    if (!l || l->dead || atomic_load(&l->child_reaped))
        return false;
    win_frame frame;
    frame.len = (DWORD)ph_link_frame(type, payload, len, frame.data);
    if (!frame.len)
        return false;
    win_pump(l);
    if (l->dead)
        return false;
    if (l->queued == WIN_QUEUE_FRAMES) {
        PH_LOGW("Bluetooth helper input queue is full; reconnecting instead of blocking input");
        ph_hid_link_request_stop(l);
        return false;
    }
    l->queue[(l->head + l->queued) % WIN_QUEUE_FRAMES] = frame;
    l->queued++;
    win_pump(l);
    return !l->dead;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

/* A nonblocking pipe accepts these whole frames atomically or returns EAGAIN. */
_Static_assert(PH_LINK_MAX_FRAME <= PIPE_BUF, "HID frames must fit an atomic pipe write");

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
    /* Input runs on the rendering thread. A helper that stops reading must not
     * prevent that thread from polling its bounded shutdown/recovery sequence. */
    int flags = fcntl(in_pipe[1], F_GETFL);
    if (flags < 0 || fcntl(in_pipe[1], F_SETFL, flags | O_NONBLOCK) < 0)
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
        /* AirPlay's listening/data sockets are not close-on-exec. A helper
         * (re)started during mirroring would otherwise keep the mirror port
         * bound after the session ends, and the next session fails to bind. */
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
        closefrom(STDERR_FILENO + 1);
#else
        for (long fd = STDERR_FILENO + 1, max = sysconf(_SC_OPEN_MAX); fd < max && fd < 65536; fd++)
            close((int)fd);
#endif
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
    size_t n = ph_link_frame(type, payload, len, buf);

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
    ssize_t written;
    do {
        written = write(l->to_helper, buf, n);
    } while (written < 0 && errno == EINTR);
    if (written == (ssize_t)n)
        return true;
    /* Never wait for space or retry an incomplete frame: dropped input can
     * include a key/button release, so retire this helper and reconnect. */
    if (written < 0)
        PH_LOGW("Bluetooth helper input failed (%s); reconnecting", strerror(errno));
    else
        PH_LOGW("Bluetooth helper input write was incomplete; reconnecting");
    ph_hid_link_request_stop(l);
    return false;
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
