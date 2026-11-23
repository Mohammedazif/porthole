/* Native portable bootstrap. The verified ZIP is resource 101, at its root.
 * Does not install, elevate, register services, or alter the machine environment.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PAYLOAD_RESOURCE 101
#define WM_PORTABLE_DONE (WM_APP + 1)
#define PATH_CAP 32768

typedef struct {
    HWND window;
    HINSTANCE instance;
    wchar_t *base, *archive, *appdir, *log, *extractlog, *cache;
    wchar_t error[2048];
    BOOL ok;
} portable;

static wchar_t *join(const wchar_t *base, const wchar_t *name)
{
    size_t a = wcslen(base), b = wcslen(name);
    wchar_t *s = calloc(a + b + 2, sizeof(*s));
    if (!s) return NULL;
    memcpy(s, base, a * sizeof(*s));
    if (a && base[a - 1] != L'\\') s[a++] = L'\\';
    memcpy(s + a, name, (b + 1) * sizeof(*s));
    return s;
}

static void fail(portable *p, const wchar_t *message, DWORD code)
{
    wchar_t reason[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, reason, 512, NULL);
    swprintf(p->error, 2048, L"%ls\n\nCode: %lu %ls\n\nLog: %ls\nExtraction log: %ls\n\nKeep this folder when reporting the problem. Check available disk space, then retry the portable file.",
             message, (unsigned long)code, reason,
             p->log ? p->log : L"Not created; Windows TEMP directory unavailable",
             p->extractlog ? p->extractlog : L"Not created");
}

static BOOL regular_file(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* Keep the connection log easy to find. Read-only launch locations retain the
 * private TEMP log selected by create_base instead. Existing logs are appended. */
static void prefer_adjacent_log(portable *p)
{
    wchar_t path[PATH_CAP];
    DWORD length = GetModuleFileNameW(p->instance, path, PATH_CAP);
    if (!length || length >= PATH_CAP - 5) return;
    wchar_t *slash = wcsrchr(path, L'\\'), *dot = wcsrchr(path, L'.');
    if (dot && (!slash || dot > slash)) *dot = L'\0';
    wcscat(path, L".log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    CloseHandle(file);
    wchar_t *copy = _wcsdup(path);
    if (copy) { free(p->log); p->log = copy; }
}

static BOOL create_base(portable *p)
{
    wchar_t tmp[PATH_CAP], suffix[96], guidstr[40];
    DWORD n = GetTempPathW(PATH_CAP, tmp);
    if (!n || n >= PATH_CAP) return FALSE;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        GUID guid;
        if (FAILED(CoCreateGuid(&guid))) return FALSE;
        StringFromGUID2(&guid, guidstr, 40);
        swprintf(suffix, 96, L"Porthole-%ls", guidstr);
        p->base = join(tmp, suffix);
        if (!p->base) return FALSE;
        if (CreateDirectoryW(p->base, NULL)) break;
        DWORD error = GetLastError();
        free(p->base); p->base = NULL;
        if (error != ERROR_ALREADY_EXISTS) { SetLastError(error); return FALSE; }
    }
    if (!p->base) return FALSE;
    p->archive = join(p->base, L"payload.zip");
    p->appdir = join(p->base, L"app");
    p->log = join(p->base, L"portable.log");
    p->extractlog = join(p->base, L"extractor.log");
    p->cache = join(p->base, L"dotnet-cache");
    if (!p->archive || !p->appdir || !p->log || !p->extractlog || !p->cache) return FALSE;
    prefer_adjacent_log(p);
    return CreateDirectoryW(p->appdir, NULL) && CreateDirectoryW(p->cache, NULL);
}

static BOOL save_payload(portable *p)
{
    HRSRC resource = FindResourceW(p->instance, MAKEINTRESOURCEW(PAYLOAD_RESOURCE), RT_RCDATA);
    if (!resource) return FALSE;
    DWORD size = SizeofResource(p->instance, resource);
    HGLOBAL loaded = LoadResource(p->instance, resource);
    const BYTE *bytes = loaded ? LockResource(loaded) : NULL;
    if (!bytes || !size) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
    HANDLE file = CreateFileW(p->archive, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD offset = 0;
    BOOL ok = TRUE;
    while (offset < size) {
        DWORD written = 0, chunk = size - offset;
        if (chunk > 1024 * 1024) chunk = 1024 * 1024;
        if (!WriteFile(file, bytes + offset, chunk, &written, NULL) || !written) { ok = FALSE; break; }
        offset += written;
    }
    DWORD error = GetLastError();
    if (!CloseHandle(file) && ok) { ok = FALSE; error = GetLastError(); }
    SetLastError(error);
    return ok;
}

static const wchar_t *const envkeys[] = {
    L"PORTHOLE_PORTABLE_ARCHIVE", L"PORTHOLE_PORTABLE_DEST",
    L"PORTHOLE_PORTABLE_EXTRACT_LOG", L"DOTNET_BUNDLE_EXTRACT_BASE_DIR",
    L"DOTNET_HOST_TRACE", L"DOTNET_HOST_TRACEFILE"
};

static int compare_environment(const void *left, const void *right)
{
    const wchar_t *const *a = left, *const *b = right;
    return _wcsicmp(*a, *b);
}

/* Clone, rather than mutate, the process environment. Paths stay literal data. */
static wchar_t *child_environment(const portable *p)
{
    const wchar_t *values[] = {p->archive, p->appdir, p->extractlog, p->cache, L"1", NULL};
    wchar_t *trace = join(p->base, L"dotnet-host.log");
    if (!trace) return NULL;
    values[5] = trace;
    LPWCH original = GetEnvironmentStringsW();
    if (!original) { free(trace); return NULL; }
    size_t total = 2;
    for (const wchar_t *v = original; *v; v += wcslen(v) + 1) total += wcslen(v) + 1;
    for (size_t i = 0; i < sizeof(envkeys) / sizeof(envkeys[0]); ++i)
        total += wcslen(envkeys[i]) + wcslen(values[i]) + 2;
    wchar_t *block = calloc(total, sizeof(*block)), *out = block;
    if (!block) { FreeEnvironmentStringsW(original); free(trace); return NULL; }
    for (const wchar_t *v = original; *v; v += wcslen(v) + 1) {
        BOOL replace = FALSE;
        for (size_t i = 0; i < sizeof(envkeys) / sizeof(envkeys[0]); ++i) {
            size_t len = wcslen(envkeys[i]);
            if (_wcsnicmp(v, envkeys[i], len) == 0 && v[len] == L'=') replace = TRUE;
        }
        if (!replace) { size_t len = wcslen(v) + 1; memcpy(out, v, len * sizeof(*out)); out += len; }
    }
    for (size_t i = 0; i < sizeof(envkeys) / sizeof(envkeys[0]); ++i) {
        size_t len = wcslen(envkeys[i]);
        memcpy(out, envkeys[i], len * sizeof(*out)); out += len; *out++ = L'=';
        len = wcslen(values[i]); memcpy(out, values[i], len * sizeof(*out)); out += len; *out++ = 0;
    }
    *out = 0;
    FreeEnvironmentStringsW(original); free(trace);
    /* Windows specifies a case-insensitively sorted Unicode environment. */
    size_t count = 0;
    for (const wchar_t *v = block; *v; v += wcslen(v) + 1) ++count;
    wchar_t **entries = calloc(count, sizeof(*entries));
    wchar_t *sorted = calloc(total, sizeof(*sorted));
    if (!entries || !sorted) { free(entries); free(sorted); free(block); return NULL; }
    size_t index = 0;
    for (wchar_t *v = block; *v; v += wcslen(v) + 1) entries[index++] = v;
    qsort(entries, count, sizeof(*entries), compare_environment);
    out = sorted;
    for (size_t i = 0; i < count; ++i) {
        size_t len = wcslen(entries[i]) + 1;
        memcpy(out, entries[i], len * sizeof(*out)); out += len;
    }
    free(entries); free(block); return sorted;
}

static wchar_t *base64_utf16(const wchar_t *input)
{
    static const wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *bytes = (const unsigned char *)input;
    size_t size = wcslen(input) * sizeof(*input), len = ((size + 2) / 3) * 4;
    wchar_t *result = calloc(len + 1, sizeof(*result));
    if (!result) return NULL;
    for (size_t i = 0, j = 0; i < size; i += 3) {
        uint32_t n = (uint32_t)bytes[i] << 16;
        if (i + 1 < size) n |= (uint32_t)bytes[i + 1] << 8;
        if (i + 2 < size) n |= bytes[i + 2];
        result[j++] = alphabet[(n >> 18) & 63]; result[j++] = alphabet[(n >> 12) & 63];
        result[j++] = i + 1 < size ? alphabet[(n >> 6) & 63] : L'=';
        result[j++] = i + 2 < size ? alphabet[n & 63] : L'=';
    }
    return result;
}

static BOOL extract(portable *p, wchar_t *environment)
{
    wchar_t system[PATH_CAP], *exe = NULL, *encoded = NULL, *command = NULL;
    DWORD count = GetSystemDirectoryW(system, PATH_CAP);
    if (!count || count >= PATH_CAP) return FALSE;
#ifdef PORTHOLE_PORTABLE_TEST
    wchar_t test_extractor[PATH_CAP];
    DWORD test_len = GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_TEST_EXTRACTOR", test_extractor, PATH_CAP);
    if (test_len && test_len < PATH_CAP) exe = _wcsdup(test_extractor);
#endif
    if (!exe) exe = join(system, L"WindowsPowerShell\\v1.0\\powershell.exe");
    /* Fixed UTF-16 script: no filename or environment value becomes PS code. */
    static const wchar_t script[] =
        L"$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';try {"
        L"Import-Module ($PSHOME+'\\Modules\\Microsoft.PowerShell.Archive\\Microsoft.PowerShell.Archive.psd1') -ErrorAction Stop;"
        L"Expand-Archive -LiteralPath $env:PORTHOLE_PORTABLE_ARCHIVE -DestinationPath $env:PORTHOLE_PORTABLE_DEST -ErrorAction Stop;"
        L"[IO.File]::AppendAllText($env:PORTHOLE_PORTABLE_EXTRACT_LOG,'Extraction completed'+[Environment]::NewLine);exit 0"
        L"}catch{[IO.File]::AppendAllText($env:PORTHOLE_PORTABLE_EXTRACT_LOG,($_|Out-String));exit 1}";
    encoded = base64_utf16(script);
    if (!exe || !encoded) { free(exe); free(encoded); return FALSE; }
    size_t capacity = wcslen(exe) + wcslen(encoded) + 128;
    command = calloc(capacity, sizeof(*command));
    if (!command) { free(exe); free(encoded); return FALSE; }
    swprintf(command, capacity, L"\"%ls\" -NoLogo -NoProfile -NonInteractive -EncodedCommand %ls", exe, encoded);
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(exe, command, NULL, NULL, FALSE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                             environment, p->base, &si, &pi);
    DWORD error = GetLastError();
    if (ok) {
        CloseHandle(pi.hThread);
        DWORD waited = WaitForSingleObject(pi.hProcess, 180000), status = 1;
        ok = waited == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &status) && status == 0;
        error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : (ok ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE);
        if (waited == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(pi.hProcess, 3000);
        }
        CloseHandle(pi.hProcess);
    }
    free(command); free(encoded); free(exe); SetLastError(error);
    return ok;
}

static BOOL launch(portable *p, wchar_t *environment)
{
    wchar_t *exe = join(p->appdir, L"porthole.exe");
    wchar_t *helper = join(p->appdir, L"ble-winrt\\porthole-ble-winrt.exe");
    if (!exe || !helper || !regular_file(exe) || !regular_file(helper)) {
        free(exe); free(helper); SetLastError(ERROR_FILE_NOT_FOUND); return FALSE;
    }
    free(helper);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(p->log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        free(exe); SetLastError(error); return FALSE;
    }
    static const char header[] = "Porthole portable: extracted application starting.\r\n";
    DWORD written; WriteFile(log, header, sizeof(header) - 1, &written, NULL);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = malloc(bytes);
    BOOL initialized = attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &bytes);
    HANDLE handles[] = {log, input};
    BOOL ok = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                       handles, sizeof(handles), NULL, NULL);
    size_t capacity = wcslen(exe) + 128;
    wchar_t *command = calloc(capacity, sizeof(*command));
    if (!command) ok = FALSE;
    if (ok) swprintf(command, capacity, L"\"%ls\" --name \"Porthole Windows\"", exe);
    STARTUPINFOEXW si = {0}; si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = input; si.StartupInfo.hStdOutput = log; si.StartupInfo.hStdError = log;
    si.lpAttributeList = attributes;
    PROCESS_INFORMATION pi = {0};
    if (ok) ok = CreateProcessW(exe, command, NULL, NULL, TRUE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                               environment, p->appdir, &si.StartupInfo, &pi);
    DWORD error = GetLastError();
    if (ok) {
        CloseHandle(pi.hThread);
#ifdef PORTHOLE_PORTABLE_TEST
        DWORD status = 1;
        ok = WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0 &&
             GetExitCodeProcess(pi.hProcess, &status) && status == 0;
        if (!ok) error = ERROR_PROCESS_ABORTED;
#else
        /* Surface loader/startup failures while retaining the application log. */
        DWORD status = 0;
        if (WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(pi.hProcess, &status) && status != 0) { ok = FALSE; error = status; }
#endif
        CloseHandle(pi.hProcess);
    }
    if (initialized) DeleteProcThreadAttributeList(attributes);
    free(attributes); free(command); free(exe); CloseHandle(log); CloseHandle(input);
    SetLastError(error); return ok;
}

static void persist_failure(const portable *p)
{
    if (!p->log || !p->error[0]) return;
    HANDLE file = CreateFileW(p->log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, p->error, -1, NULL, 0, NULL, NULL);
    char *utf8 = bytes > 0 ? malloc((size_t)bytes) : NULL;
    DWORD written;
    if (utf8 && WideCharToMultiByte(CP_UTF8, 0, p->error, -1, utf8, bytes, NULL, NULL)) {
        WriteFile(file, utf8, (DWORD)bytes - 1, &written, NULL);
        WriteFile(file, "\r\n", 2, &written, NULL);
    }
    free(utf8); CloseHandle(file);
}

#ifdef PORTHOLE_PORTABLE_TEST
static void test_result(const portable *p)
{
    wchar_t path[PATH_CAP];
    DWORD n = GetEnvironmentVariableW(L"PORTHOLE_PORTABLE_TEST_RESULT", path, PATH_CAP);
    if (!n || n >= PATH_CAP) return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    const wchar_t *message = p->ok ? p->log : p->error;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, message, -1, NULL, 0, NULL, NULL);
    char *utf8 = malloc((size_t)bytes);
    DWORD written;
    const char *status = p->ok ? "OK\n" : "ERROR\n";
    WriteFile(file, status, (DWORD)strlen(status), &written, NULL);
    if (utf8 && WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, bytes, NULL, NULL))
        WriteFile(file, utf8, (DWORD)bytes - 1, &written, NULL);
    free(utf8); CloseHandle(file);
}
#endif

static DWORD WINAPI prepare(void *arg)
{
    portable *p = arg;
    if (!create_base(p)) fail(p, L"Could not create a private folder in Windows TEMP.", GetLastError());
    else if (!save_payload(p)) fail(p, L"Could not unpack the embedded archive to disk.", GetLastError());
    else {
        wchar_t *environment = child_environment(p);
        if (!environment) fail(p, L"Could not prepare the application environment.", ERROR_NOT_ENOUGH_MEMORY);
        else if (!extract(p, environment)) fail(p, L"Windows could not extract Porthole. Windows PowerShell and its Microsoft.PowerShell.Archive module are required (included in Windows 10/11).", GetLastError());
        else if (!launch(p, environment)) fail(p, L"The extracted Porthole application could not start. Its application or Bluetooth helper file may be missing or blocked.", GetLastError());
        else p->ok = TRUE;
        free(environment);
    }
    if (!p->ok) persist_failure(p);
#ifdef PORTHOLE_PORTABLE_TEST
    test_result(p);
#endif
    PostMessageW(p->window, WM_PORTABLE_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    portable *p = (portable *)GetWindowLongPtrW(window, GWLP_USERDATA);
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams);
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint); RECT rect;
        GetClientRect(window, &rect); rect.left += 24; rect.right -= 24;
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Preparing Porthole...\n\nExtracting the complete application to your TEMP folder.\nThe mirroring window will open automatically.",
                  -1, &rect, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
        EndPaint(window, &paint); return 0;
    }
    case WM_CLOSE:
        /* Close is disabled until the worker finishes: no hidden later launch. */
        return 0;
    case WM_PORTABLE_DONE:
#ifndef PORTHOLE_PORTABLE_TEST
        if (p && !p->ok) MessageBoxW(window, p->error, L"Porthole portable could not start", MB_OK | MB_ICONERROR);
#else
        (void)p;
#endif
        DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    (void)previous; (void)command; (void)show;
    portable p = {.instance = instance};
    WNDCLASSW cls = {0}; cls.hInstance = instance; cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"PortholePortableBootstrap"; cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) return 1;
    p.window = CreateWindowExW(0, cls.lpszClassName, L"Porthole portable", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, 480, 170, NULL, NULL, instance, &p);
    if (!p.window) return 1;
    EnableMenuItem(GetSystemMenu(p.window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    ShowWindow(p.window, SW_SHOW); UpdateWindow(p.window);
    HANDLE worker = CreateThread(NULL, 0, prepare, &p, 0, NULL);
    if (!worker) {
        fail(&p, L"Could not start the extraction worker.", GetLastError());
        PostMessageW(p.window, WM_PORTABLE_DONE, 0, 0);
    }
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); }
    free(p.base); free(p.archive); free(p.appdir); free(p.log); free(p.extractlog); free(p.cache);
    return p.ok ? 0 : 1;
}
