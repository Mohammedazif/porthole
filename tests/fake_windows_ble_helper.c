/* Native Windows pipe peer; never opens Bluetooth or changes system settings. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "hid/link_proto.h"

static bool exact(HANDLE stream, uint8_t *data, DWORD count)
{
    DWORD used = 0;
    while (used < count) {
        DWORD got = 0;
        if (!ReadFile(stream, data + used, count - used, &got, NULL) || !got)
            return false;
        used += got;
    }
    return true;
}

static void print_arg(int index, const wchar_t *arg)
{
    int count = WideCharToMultiByte(CP_UTF8, 0, arg, -1, NULL, 0, NULL, NULL);
    char *utf8 = malloc((size_t)count);
    if (!utf8 || !WideCharToMultiByte(CP_UTF8, 0, arg, -1, utf8, count, NULL, NULL))
        exit(2);
    printf("arg %d ", index);
    for (int i = 0; i < count - 1; i++)
        printf("%02x", (unsigned char)utf8[i]);
    printf("\n");
    free(utf8);
}

int wmain(int argc, wchar_t **argv);
int wmain(int argc, wchar_t **argv)
{
    const wchar_t *mode = argc > 1 ? argv[1] : L"echo";
    if (!wcscmp(mode, L"hold-output")) {
        Sleep(10000);
        return 0;
    }
    puts("ready");
    for (int i = 2; i < argc; i++) {
        print_arg(i - 2, argv[i]);
        if (i > 2 && !wcscmp(argv[i - 1], L"--probe-handle")) {
            uintptr_t value = (uintptr_t)wcstoull(argv[i], NULL, 16);
            /* If inherited, this resets the parent's sentinel event. */
            ResetEvent((HANDLE)value);
        }
    }
    fprintf(stderr, "helper stderr diagnostic\n");
    fflush(stderr);
    fflush(stdout);
    if (!wcscmp(mode, L"hung")) {
        Sleep(INFINITE);
        return 0;
    }
    if (!wcscmp(mode, L"closed-output")) {
        CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));
        Sleep(INFINITE);
        return 0;
    }
    if (!wcscmp(mode, L"exit")) {
        printf("last-partial");
        return 0;
    }
    if (!wcscmp(mode, L"descendant")) {
        wchar_t path[32768], command[32768];
        if (!GetModuleFileNameW(NULL, path, sizeof path / sizeof path[0]))
            return 3;
        if (swprintf(command, sizeof command / sizeof command[0], L"\"%ls\" hold-output", path) < 0)
            return 4;
        STARTUPINFOW startup = {0};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process;
        if (!CreateProcessW(path, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &startup, &process))
            return 5;
        printf("descendant %lu\n", process.dwProcessId);
        fflush(stdout);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    }
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    uint8_t header[2], payload[255];
    while (exact(input, header, sizeof header) && exact(input, payload, header[1])) {
        if (header[0] == PH_LINK_SYNC && header[1] == 4) {
            uint32_t id = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
                          | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
            printf("sync %lu\n", (unsigned long)id);
        } else {
            printf("report %u ", header[0]);
            for (unsigned int i = 0; i < header[1]; i++)
                printf("%02x", payload[i]);
            printf("\n");
        }
        fflush(stdout);
    }
    puts("eof");
    return 0;
}
