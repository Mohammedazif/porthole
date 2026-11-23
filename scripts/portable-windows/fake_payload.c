/* Native fixture: TEST launcher extractor and benign application in one PE. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CAP 32768

static BOOL path(wchar_t *out, const wchar_t *base, const wchar_t *name)
{
    return swprintf(out, CAP, L"%ls\\%ls", base, name) > 0;
}

static BOOL write_marker(const wchar_t *file, const char *text)
{
    HANDLE h = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD n;
    BOOL ok = WriteFile(h, text, (DWORD)strlen(text), &n, NULL) && n == strlen(text);
    CloseHandle(h); return ok;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t self[CAP], dest[CAP], archive[CAP], file[CAP], mode[80];
    GetModuleFileNameW(NULL, self, CAP);
    GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_TEST_MODE", mode, 80);
    if (argc > 1 && wcscmp(argv[1], L"-NoLogo") == 0) {
        if (wcscmp(mode, L"extract-fail") == 0) return 17;
        if (!GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_DEST", dest, CAP) ||
            !GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_ARCHIVE", archive, CAP)) return 18;
        HANDLE zip = CreateFileW(archive, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char magic[4] = {0}; DWORD n = 0;
        if (zip == INVALID_HANDLE_VALUE) return 19;
        BOOL valid = ReadFile(zip, magic, 4, &n, NULL) && n == 4 && memcmp(magic, "PK\003\004", 4) == 0;
        CloseHandle(zip);
        if (!valid) return 20;
        path(file, dest, L"porthole.exe");
        if (!CopyFileW(self, file, TRUE)) return 21;
        if (wcscmp(mode, L"missing-helper") != 0) {
            path(file, dest, L"ble-winrt");
            if (!CreateDirectoryW(file, NULL)) return 22;
            path(file, dest, L"ble-winrt\\porthole-ble-winrt.exe");
            if (!CopyFileW(self, file, TRUE)) return 23;
        }
        /* Parent must not leak arbitrary inheritable handles into extraction. */
        wchar_t raw[80];
        if (GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_TEST_HANDLE", raw, 80)) {
            HANDLE foreign = (HANDLE)(uintptr_t)wcstoull(raw, NULL, 10);
            DWORD flags;
            if (GetHandleInformation(foreign, &flags)) return 24;
        }
        return 0;
    }
    if (argc != 3 || wcscmp(argv[1], L"--name") || wcscmp(argv[2], L"Porthole Windows")) return 30;
    wchar_t current[CAP], cache[CAP], marker[CAP], trace[CAP];
    GetCurrentDirectoryW(CAP, current);
    if (!GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_DEST", dest, CAP) || wcscmp(current, dest)) return 31;
    if (!GetEnvironmentVariableW(L"DOTNET_BUNDLE_EXTRACT_BASE_DIR", cache, CAP) ||
        GetFileAttributesW(cache) == INVALID_FILE_ATTRIBUTES) return 32;
    if (!GetEnvironmentVariableW(L"DOTNET_HOST_TRACEFILE", trace, CAP)) return 33;
    HANDLE log = GetStdHandle(STD_OUTPUT_HANDLE);
    static const char logged[] = "FAKE APP STDOUT\r\n"; DWORD n;
    if (!WriteFile(log, logged, sizeof(logged) - 1, &n, NULL)) return 34;
    log = GetStdHandle(STD_ERROR_HANDLE);
    static const char error[] = "FAKE APP STDERR\r\n";
    if (!WriteFile(log, error, sizeof(error) - 1, &n, NULL)) return 35;
    if (!GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_TEST_MARKER", marker, CAP) ||
        !write_marker(marker, "NATIVE_APP_RAN\n")) return 36;
    return wcscmp(mode, L"app-fail") == 0 ? 37 : 0;
}
