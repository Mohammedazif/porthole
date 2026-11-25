#include "core/host.h"

#include "core/sock.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <wchar.h>

static bool copy_windows_name(const wchar_t *name, char *out, size_t len)
{
    char utf8[768];
    /* Any name the PC has is its name, "UNKNOWN" included. */
    if (!name[0] ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, utf8, sizeof utf8, NULL, NULL))
        return false;
    size_t count = strlen(utf8);
    if (count >= len) {
        count = len - 1;
        while (count && ((unsigned char)utf8[count] & 0xc0) == 0x80)
            count--;
    }
    memcpy(out, utf8, count);
    out[count] = 0;
    return count != 0;
}
#endif

void ph_host_name(char *out, size_t len)
{
    char *dot;

    if (!out || len < 2)
        return;
#ifdef _WIN32
    wchar_t name[256];
    const COMPUTER_NAME_FORMAT formats[] = {ComputerNamePhysicalDnsHostname, ComputerNameDnsHostname,
                                             ComputerNamePhysicalNetBIOS, ComputerNameNetBIOS};
    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        DWORD count = (DWORD)(sizeof name / sizeof name[0]);
        if (GetComputerNameExW(formats[i], name, &count) && copy_windows_name(name, out, len))
            return;
    }
    DWORD count = (DWORD)(sizeof name / sizeof name[0]);
    if (GetComputerNameW(name, &count) && copy_windows_name(name, out, len))
        return;
    count = GetEnvironmentVariableW(L"COMPUTERNAME", name, (DWORD)(sizeof name / sizeof name[0]));
    if (count && count < sizeof name / sizeof name[0] && copy_windows_name(name, out, len))
        return;
#endif
    ph_sock_init(); /* gethostname needs Winsock on Windows */
    memset(out, 0, len);
    if (gethostname(out, (int)len - 1) != 0 || !out[0])
        snprintf(out, len, "Porthole PC");
    dot = strchr(out, '.');
    if (dot == out || !out[0])
        snprintf(out, len, "Porthole PC");
    else if (dot)
        *dot = 0;
}
