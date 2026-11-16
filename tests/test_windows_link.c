/* Native Windows process/pipe integration. No radio, Python, SDL or desktop needed. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include <windows.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "core/metrics.h"
#include "hid/link.h"

typedef struct observed {
    CRITICAL_SECTION lock;
    char lines[64][512];
    size_t used;
    atomic_uint exited;
} observed;

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s (Windows error %lu)\n", __FILE__, __LINE__, #condition, GetLastError()); \
    failures++; \
} } while (0)

static void result_marker(const char *text)
{
    wchar_t path[32768];
    DWORD length = GetEnvironmentVariableW(L"PORTHOLE_WIN_LINK_RESULT", path,
                                           sizeof path / sizeof path[0]);
    if (!length || length >= sizeof path / sizeof path[0])
        return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(file);
    }
}

static void init(observed *events)
{
    memset(events, 0, sizeof *events);
    InitializeCriticalSection(&events->lock);
    atomic_init(&events->exited, 0);
}

static void on_status(void *user, const char *line)
{
    observed *events = user;
    EnterCriticalSection(&events->lock);
    if (events->used < sizeof events->lines / sizeof events->lines[0])
        snprintf(events->lines[events->used++], sizeof events->lines[0], "%s", line);
    LeaveCriticalSection(&events->lock);
    if (!strcmp(line, "exited"))
        atomic_fetch_add(&events->exited, 1);
}

static bool seen(observed *events, const char *line)
{
    bool found = false;
    EnterCriticalSection(&events->lock);
    for (size_t i = 0; i < events->used; i++)
        found |= !strcmp(events->lines[i], line);
    LeaveCriticalSection(&events->lock);
    return found;
}

static bool wait_line(ph_hid_link *link, observed *events, const char *line, unsigned int timeout)
{
    uint64_t deadline = ph_metrics_now_ms() + timeout;
    do {
        uint64_t began = ph_metrics_now_ms();
        ph_hid_link_poll(link);
        CHECK(ph_metrics_now_ms() - began < 100);
        if (seen(events, line))
            return true;
        Sleep(5);
    } while (ph_metrics_now_ms() < deadline);
    return false;
}

static char *utf8(const wchar_t *wide)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = malloc((size_t)length);
    if (!text || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, length, NULL, NULL)) {
        free(text);
        return NULL;
    }
    return text;
}

static void expected_arg(char line[512], unsigned int index, const char *arg)
{
    int used = snprintf(line, 512, "arg %u ", index);
    for (size_t i = 0; arg[i] && used + 2 < 512; i++)
        used += snprintf(line + used, (size_t)(512 - used), "%02x", (unsigned char)arg[i]);
}

static void close_link(ph_hid_link *link, observed *events)
{
    uint64_t began = ph_metrics_now_ms();
    ph_hid_link_request_stop(link);
    CHECK(ph_metrics_now_ms() - began < 100);
    CHECK(!ph_hid_link_sync(link, 99));
    CHECK(wait_line(link, events, "exited", 3500));
    CHECK(atomic_load(&events->exited) == 1 && ph_hid_link_poll(link));
    began = ph_metrics_now_ms();
    ph_hid_link_stop(link);
    CHECK(ph_metrics_now_ms() - began < 100);
    DeleteCriticalSection(&events->lock);
}

static void simple_case(char *helper, char *mode, bool request)
{
    observed events;
    init(&events);
    char *args[] = {helper, mode, NULL};
    ph_hid_link *link = ph_hid_link_start(args, on_status, &events);
    CHECK(link);
    if (!link) {
        DeleteCriticalSection(&events.lock);
        return;
    }
    CHECK(wait_line(link, &events, "ready", 2000));
    if (!request)
        CHECK(wait_line(link, &events, "exited", 3500));
    if (!strcmp(mode, "exit"))
        CHECK(seen(&events, "last-partial"));
    if (!strcmp(mode, "descendant")) {
        DWORD descendant = 0;
        EnterCriticalSection(&events.lock);
        for (size_t i = 0; i < events.used; i++)
            if (sscanf(events.lines[i], "descendant %lu", &descendant) == 1)
                break;
        LeaveCriticalSection(&events.lock);
        CHECK(descendant);
        if (descendant) {
            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, descendant);
            CHECK(process);
            if (process) {
                TerminateProcess(process, 0);
                WaitForSingleObject(process, 1000);
                CloseHandle(process);
            }
        }
    }
    close_link(link, &events);
}

int wmain(int argc, wchar_t **argv);
int wmain(int argc, wchar_t **argv)
{
    result_marker("STARTED\n");
    CHECK(argc == 2);
    if (argc != 2)
        return 1;
    wchar_t temporary[MAX_PATH], directory[MAX_PATH], copied[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, temporary);
    CHECK(length > 0 && length < MAX_PATH - 80);
    if (!length || length >= MAX_PATH - 80)
        return 1;
    swprintf(directory, MAX_PATH, L"%lsporthole-link-%lu-\u7a7a \u683c", temporary, GetCurrentProcessId());
    swprintf(copied, MAX_PATH, L"%ls\\helper \u03bb.exe", directory);
    CHECK(CreateDirectoryW(directory, NULL));
    CHECK(CopyFileW(argv[1], copied, FALSE));
    char *helper = utf8(copied), *unicode = utf8(L"\u03bb \u4e2d\u6587 \U0001f600");
    CHECK(helper && unicode);
    if (!helper || !unicode)
        return 1;

    /* A handle marked inheritable must still be excluded by the allow-list. */
    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, NULL, TRUE};
    HANDLE sentinel = CreateEventW(&inheritable, TRUE, TRUE, NULL);
    CHECK(sentinel);
    char handle_text[32];
    snprintf(handle_text, sizeof handle_text, "%llx", (unsigned long long)(uintptr_t)sentinel);
    char *args[] = {helper, "echo", "", "two words", "quote\"in\"word", "C:\\ends\\",
                    "\\\\\\\"", unicode, "&|<>^%PATH%!\t", "--probe-handle", handle_text, NULL};
    HANDLE error_read = NULL, error_write = NULL, previous_error = GetStdHandle(STD_ERROR_HANDLE);
    CHECK(CreatePipe(&error_read, &error_write, NULL, 0));
    CHECK(SetStdHandle(STD_ERROR_HANDLE, error_write));
    observed events;
    init(&events);
    ph_hid_link *link = ph_hid_link_start(args, on_status, &events);
    CHECK(SetStdHandle(STD_ERROR_HANDLE, previous_error));
    CloseHandle(error_write);
    CHECK(link);
    if (!link)
        return 1;
    CHECK(wait_line(link, &events, "ready", 2000));
    for (unsigned int i = 2; args[i]; i++) {
        char line[512];
        expected_arg(line, i - 2, args[i]);
        CHECK(wait_line(link, &events, line, 2000));
    }
    CHECK(WaitForSingleObject(sentinel, 0) == WAIT_OBJECT_0);
    CloseHandle(sentinel);
    ph_hid_keyboard keyboard = {.keys = {4}};
    ph_hid_mouse mouse = {.dx = -123, .dy = 456, .wheel = 1, .pan = -1};
    CHECK(ph_hid_link_keyboard(link, &keyboard));
    CHECK(ph_hid_link_mouse(link, &mouse));
    CHECK(ph_hid_link_consumer(link, 0x223));
    CHECK(ph_hid_link_sync(link, 0x80000011));
    CHECK(wait_line(link, &events, "report 1 0000040000000000", 2000));
    CHECK(wait_line(link, &events, "report 2 0085ffc80101ff", 2000));
    CHECK(wait_line(link, &events, "report 3 2302", 2000));
    CHECK(wait_line(link, &events, "sync 2147483665", 2000));
    DWORD available = 0, count = 0;
    char error_text[128] = "";
    CHECK(PeekNamedPipe(error_read, NULL, 0, NULL, &available, NULL) && available > 0);
    CHECK(ReadFile(error_read, error_text, sizeof error_text - 1, &count, NULL));
    CHECK(strstr(error_text, "helper stderr diagnostic"));
    CloseHandle(error_read);
    close_link(link, &events);

    simple_case(helper, "exit", false);
    simple_case(helper, "closed-output", false);
    simple_case(helper, "descendant", false);

    /* No-reader stdin forces pending overlapped I/O and then saturates the FIFO.
     * Render-thread enqueue and poll must stay responsive; cancellation must
     * retain its OVERLAPPED/buffer storage through completion. */
    observed stuck;
    init(&stuck);
    char *hung[] = {helper, "hung", NULL};
    link = ph_hid_link_start(hung, on_status, &stuck);
    CHECK(link);
    if (!link)
        return 1;
    CHECK(wait_line(link, &stuck, "ready", 2000));
    bool full = false;
    uint64_t began = ph_metrics_now_ms();
    for (unsigned int i = 0; i < 100000; i++) {
        if (!ph_hid_link_mouse(link, &mouse)) {
            full = true;
            break;
        }
    }
    CHECK(full && ph_metrics_now_ms() - began < 1000);
    close_link(link, &stuck);

    /* Warmed-up repeated start/stop and invalid commands must leak no handles. */
    DWORD before = 0, after = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 6; i++)
        simple_case(helper, "exit", false);
    char invalid_utf8[] = {'x', (char)0xff, 0};
    char *invalid[] = {helper, invalid_utf8, NULL};
    CHECK(!ph_hid_link_start(invalid, NULL, NULL));
    char *missing[] = {"Z:\\definitely-not-a-porthole-helper.exe", NULL};
    CHECK(!ph_hid_link_start(missing, NULL, NULL));
    char *long_arg = malloc(33000);
    CHECK(long_arg);
    if (long_arg) {
        memset(long_arg, 'x', 32999); long_arg[32999] = 0;
        char *too_long[] = {helper, long_arg, NULL};
        CHECK(!ph_hid_link_start(too_long, NULL, NULL));
        free(long_arg);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    CHECK(after == before);
    CHECK(DeleteFileW(copied));
    CHECK(RemoveDirectoryW(directory));
    free(unicode); free(helper);
    ph_hid_link_request_stop(NULL);
    CHECK(ph_hid_link_poll(NULL));
    ph_hid_link_stop(NULL);
    if (failures) {
        result_marker("FAIL\n");
        return 1;
    }
    result_marker("PASS\n");
    puts("Windows Unicode argv, restricted inheritance, framing, async saturation, cancellation and lifecycle passed");
    return 0;
}
