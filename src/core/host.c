#include "core/host.h"

#include "core/sock.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <wchar.h>

static bool windows_host_name(wchar_t *host, char *out, size_t len)
{
    return host[0] && _wcsicmp(host, L"unknown") && _wcsicmp(host, L"localhost") &&
           WideCharToMultiByte(CP_UTF8, 0, host, -1, out, (int)len, NULL, NULL) > 0;
}
#endif

void ph_host_name(char *out, size_t len)
{
    char *dot;

    if (!out || len < 2)
        return;
#ifdef _WIN32
    wchar_t host[256];
    DWORD size = sizeof host / sizeof *host;
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &size) &&
        windows_host_name(host, out, len))
        return;
    size = sizeof host / sizeof *host;
    if (GetComputerNameW(host, &size) && windows_host_name(host, out, len))
        return;
#endif
    ph_sock_init(); /* gethostname needs Winsock on Windows */
    memset(out, 0, len);
    if (gethostname(out, (int)len - 1) != 0 || !out[0])
        snprintf(out, len, "Porthole");
    dot = strchr(out, '.');
    if (dot == out || !out[0])
        snprintf(out, len, "Porthole");
    else if (dot)
        *dot = 0;
}
