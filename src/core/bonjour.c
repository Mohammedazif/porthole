#include "core/bonjour.h"

#include "core/log.h"
#include "core/sock.h"

#include <stdio.h>
#include <string.h>

#include <stdbool.h>
#include <stdlib.h>

void ph_bonjour_host_name(char *out, size_t len)
{
    char *dot;

    if (!out || len < 2)
        return;
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

#if defined(__linux__)

#include <arpa/inet.h>
#include <dlfcn.h>

typedef void *dns_ref;
typedef int (*register_fn)(dns_ref *ref, uint32_t flags, uint32_t interface, const char *name,
                           const char *type, const char *domain, const char *host,
                           uint16_t port_be, uint16_t txt_len, const void *txt, void *reply,
                           void *context);
typedef void (*deallocate_fn)(dns_ref ref);

struct ph_bonjour {
    void *module;
    dns_ref ref;
    deallocate_fn release;
};

ph_bonjour *ph_bonjour_advertise(const char *name, const char *type, uint16_t port,
                                 const void *txt, uint16_t txt_len)
{
    static const char *const names[] = {"libdns_sd.so.1", "libdns_sd.so"};
    ph_bonjour *b = calloc(1, sizeof *b);
    register_fn reg;

    if (!b)
        return NULL;
    if (txt_len && !txt)
        txt_len = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !b->module; i++)
        b->module = dlopen(names[i], RTLD_LAZY);
    if (!b->module) {
        PH_LOGW("Bonjour library not found; the iPhone cannot find %s", type);
        free(b);
        return NULL;
    }
    reg = (register_fn)dlsym(b->module, "DNSServiceRegister");
    b->release = (deallocate_fn)dlsym(b->module, "DNSServiceRefDeallocate");
    if (!reg || !b->release
        || reg(&b->ref, 0, 0, name, type, NULL, NULL, htons(port), txt_len, txt, NULL, NULL) != 0) {
        PH_LOGW("could not advertise %s", type);
        dlclose(b->module);
        free(b);
        return NULL;
    }
    return b;
}

void ph_bonjour_stop(ph_bonjour *b)
{
    if (!b)
        return;
    if (b->ref && b->release)
        b->release(b->ref);
    dlclose(b->module);
    free(b);
}

#elif defined(_WIN32)

/* Windows 10's own DNS-SD (DnsServiceRegister): no Bonjour service or dnssd.dll. */
#include <winsock2.h>
#include <windows.h>
#include <windns.h>

#include <stdio.h>
#include <string.h>

#define REGISTER_TIMEOUT_MS 5000
#define MAX_TXT 8

struct ph_bonjour {
    LONG references;
    HANDLE completed;
    DWORD status;
    bool deregistering;
    DNS_SERVICE_REGISTER_REQUEST request;
    DNS_SERVICE_CANCEL cancel;
    PDNS_SERVICE_INSTANCE input, registered;
};

static void release(ph_bonjour *b);

static void WINAPI registered(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    ph_bonjour *b = context;

    if (!b->deregistering)
        b->registered = instance;
    else if (instance && instance != b->input && instance != b->registered)
        DnsServiceFreeInstance(instance);
    b->status = status;
    SetEvent(b->completed);
    release(b);
}

/* The last reference of a live registration takes it off the network first. */
static void release(ph_bonjour *b)
{
    if (InterlockedDecrement(&b->references))
        return;
    if (!b->deregistering && b->status == ERROR_SUCCESS) {
        DWORD status;
        b->deregistering = true;
        b->references = 2;
        b->request.pServiceInstance = b->registered ? b->registered : b->input;
        status = DnsServiceDeRegister(&b->request, NULL);
        if (status != DNS_REQUEST_PENDING) {
            b->status = status;
            release(b);
        }
        release(b);
        return;
    }
    if (b->registered && b->registered != b->input)
        DnsServiceFreeInstance(b->registered);
    if (b->input)
        DnsServiceFreeInstance(b->input);
    if (b->completed)
        CloseHandle(b->completed);
    free(b);
}

static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = n > 0 ? calloc((size_t)n, sizeof *w) : NULL;

    if (w && !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

ph_bonjour *ph_bonjour_advertise(const char *name, const char *type, uint16_t port,
                                 const void *txt, uint16_t txt_len)
{
    char label[128], service[256];
    wchar_t host[256], *wservice = NULL, *keys[MAX_TXT] = {0}, *values[MAX_TXT] = {0};
    DWORD host_len = 240, count = 0, status;
    ph_bonjour *b = NULL;
    const uint8_t *t = txt;
    size_t at = 0;

    if (!name || !type)
        return NULL;
    /* An instance name is one DNS label: '.' and '\' inside it are escaped. */
    for (const char *c = name; *c && at + 2 < sizeof label; c++) {
        if (*c == '.' || *c == '\\')
            label[at++] = '\\';
        label[at++] = *c;
    }
    label[at] = 0;
    snprintf(service, sizeof service, "%s.%s.local", label, type);
    wservice = wide(service);
    if (!wservice || !GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &host_len))
        goto fail;
    wcscat(host, L".local");
    for (size_t pos = 0; t && pos < txt_len && count < MAX_TXT;) {
        unsigned len = t[pos++];
        char field[256], *eq;
        if (len > txt_len - pos)
            break;
        memcpy(field, t + pos, len);
        field[len] = 0;
        pos += len;
        eq = strchr(field, '=');
        if (!eq)
            continue;
        *eq = 0;
        keys[count] = wide(field);
        values[count] = wide(eq + 1);
        if (!keys[count] || !values[count])
            goto fail;
        count++;
    }
    b = calloc(1, sizeof *b);
    if (!b)
        goto fail;
    b->input = DnsServiceConstructInstance(wservice, host, NULL, NULL, port, 0, 0, count,
                                           (PCWSTR *)keys, (PCWSTR *)values);
    b->completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!b->input || !b->completed) {
        b->references = 1;
        b->status = ERROR_NOT_ENOUGH_MEMORY;
        release(b);
        b = NULL;
        goto fail;
    }
    b->references = 2;
    b->status = DNS_REQUEST_PENDING;
    b->request.Version = DNS_QUERY_REQUEST_VERSION1;
    b->request.InterfaceIndex = 0;
    b->request.pServiceInstance = b->input;
    b->request.pRegisterCompletionCallback = registered;
    b->request.pQueryContext = b;
    b->request.unicastEnabled = FALSE;
    status = DnsServiceRegister(&b->request, &b->cancel);
    if (status != DNS_REQUEST_PENDING) {
        b->status = status;
        release(b);
        release(b);
        b = NULL;
        PH_LOGW("could not advertise %s (%lu)", type, (unsigned long)status);
        goto fail;
    }
    if (WaitForSingleObject(b->completed, REGISTER_TIMEOUT_MS) != WAIT_OBJECT_0) {
        DnsServiceRegisterCancel(&b->cancel);
        release(b);
        b = NULL;
        PH_LOGW("advertising %s timed out", type);
        goto fail;
    }
    if (b->status != ERROR_SUCCESS) {
        PH_LOGW("could not advertise %s (%lu)", type, (unsigned long)b->status);
        release(b);
        b = NULL;
    }
fail:
    free(wservice);
    for (DWORD i = 0; i < MAX_TXT; i++) {
        free(keys[i]);
        free(values[i]);
    }
    return b;
}

void ph_bonjour_stop(ph_bonjour *b)
{
    if (b)
        release(b);
}

#else

ph_bonjour *ph_bonjour_advertise(const char *name, const char *type, uint16_t port,
                                 const void *txt, uint16_t txt_len)
{
    (void)name;
    (void)type;
    (void)port;
    (void)txt;
    (void)txt_len;
    return NULL;
}

void ph_bonjour_stop(ph_bonjour *bonjour)
{
    (void)bonjour;
}

#endif
