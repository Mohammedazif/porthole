/* Native Windows DNS-SD implementation of UxPlay's dnssd.h interface.
 * Windows 10 DNS API; no Bonjour SDK, daemon or dnssd.dll is required. */
#define WIN32_LEAN_AND_MEAN
#ifdef PORTHOLE_TEST_NATIVE_DNSSD
#include "fakes/windows_dns.h"
#else
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "airplay/dnssd_records.h"

#define REGISTER_TIMEOUT_MS 5000
#define MAX_PROPERTIES 24

typedef struct registration {
    LONG references;
    HANDLE completed;
    DWORD status;
    bool deregistering;
    DNS_SERVICE_REGISTER_REQUEST request;
    DNS_SERVICE_CANCEL cancel;
    PDNS_SERVICE_INSTANCE input, registered;
} registration;

static void release_registration(registration *r);

static void WINAPI registered(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    registration *r = context;
    if (!r->deregistering)
        r->registered = instance;
    else if (instance && instance != r->input && instance != r->registered)
        DnsServiceFreeInstance(instance);
    r->status = status;
    SetEvent(r->completed);
    release_registration(r);
}

static void release_registration(registration *r)
{
    if (InterlockedDecrement(&r->references))
        return;
    if (!r->deregistering && r->status == ERROR_SUCCESS) {
        /* The callback owns its request and copies until deregistration finishes,
         * even if the receiver itself has already been destroyed. */
        r->deregistering = true;
        r->references = 2; /* submitting call + asynchronous callback */
        r->request.pServiceInstance = r->registered ? r->registered : r->input;
        DWORD status = DnsServiceDeRegister(&r->request, NULL);
        if (status != DNS_REQUEST_PENDING) {
            r->status = status;
            release_registration(r); /* no callback will own this reference */
        }
        release_registration(r);
        return;
    }
    if (r->registered && r->registered != r->input)
        DnsServiceFreeInstance(r->registered);
    DnsServiceFreeInstance(r->input);
    CloseHandle(r->completed);
    free(r);
}

static wchar_t *wide(const char *utf8)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (!length)
        return NULL;
    wchar_t *result = calloc((size_t)length, sizeof *result);
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, result, length)) {
        free(result);
        return NULL;
    }
    return result;
}

static int register_service(dnssd_t *d, bool raop, unsigned short port, void **destination)
{
    if (*destination)
        return ERROR_ALREADY_EXISTS;
    if (!ph_dnssd_build_records(d))
        return ERROR_INVALID_DATA;
    /* Service names are one DNS label, including names containing '.' or '\\'. */
    char label[128], service[200];
    size_t at = 0;
    if (raop) {
        int length = snprintf(label, sizeof label, "%02X%02X%02X%02X%02X%02X@",
                              d->hw_addr[0], d->hw_addr[1], d->hw_addr[2], d->hw_addr[3],
                              d->hw_addr[4], d->hw_addr[5]);
        at = (size_t)length;
    }
    for (int i = 0; i < d->name_len; i++) {
        if (d->name[i] == '.' || d->name[i] == '\\')
            label[at++] = '\\';
        label[at++] = d->name[i];
    }
    label[at] = 0;
    snprintf(service, sizeof service, "%s.%s.local", label, raop ? "_raop._tcp" : "_airplay._tcp");
    wchar_t *name = wide(service);
    wchar_t hostname[256];
    DWORD hostname_length = 240;
    if (!name || !GetComputerNameExW(ComputerNamePhysicalDnsHostname, hostname, &hostname_length)) {
        free(name);
        return ERROR_INVALID_NAME;
    }
    wcscat(hostname, L".local");

    wchar_t *keys[MAX_PROPERTIES] = {0}, *values[MAX_PROPERTIES] = {0};
    ph_dnssd_record *record = raop ? &d->raop_txt : &d->airplay_txt;
    DWORD properties = 0, status = ERROR_NOT_ENOUGH_MEMORY;
    for (size_t pos = 0; pos < record->len;) {
        unsigned int length = record->data[pos++];
        char field[256];
        if (properties == MAX_PROPERTIES || length > record->len - pos) {
            status = ERROR_INVALID_DATA;
            goto done;
        }
        memcpy(field, record->data + pos, length);
        field[length] = 0;
        pos += length;
        char *value = strchr(field, '=');
        if (!value) {
            status = ERROR_INVALID_DATA;
            goto done;
        }
        *value++ = 0;
        keys[properties] = wide(field);
        values[properties] = wide(value);
        if (!keys[properties] || !values[properties])
            goto done;
        properties++;
    }

    registration *r = calloc(1, sizeof *r);
    if (!r)
        goto done;
    r->input = DnsServiceConstructInstance(name, hostname, NULL, NULL, port, 0, 0,
                                          properties, (PCWSTR *)keys, (PCWSTR *)values);
    r->completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!r->input || !r->completed) {
        if (r->input)
            DnsServiceFreeInstance(r->input);
        if (r->completed)
            CloseHandle(r->completed);
        free(r);
        goto done;
    }
    r->references = 2;
    r->status = DNS_REQUEST_PENDING;
    r->request.Version = DNS_QUERY_REQUEST_VERSION1;
    r->request.InterfaceIndex = 0; /* Windows selects eligible network interfaces */
    r->request.pServiceInstance = r->input;
    r->request.pRegisterCompletionCallback = registered;
    r->request.pQueryContext = r;
    r->request.unicastEnabled = FALSE; /* advertise over multicast DNS */
    status = DnsServiceRegister(&r->request, &r->cancel);
    if (status != DNS_REQUEST_PENDING) {
        r->status = status;
        release_registration(r); /* immediate error: no callback */
        release_registration(r);
        goto done;
    }
    if (WaitForSingleObject(r->completed, REGISTER_TIMEOUT_MS) != WAIT_OBJECT_0) {
        status = ERROR_TIMEOUT;
        DnsServiceRegisterCancel(&r->cancel);
        /* A late callback still owns the request. If registration won the race,
         * its last reference deregisters it instead of advertising a dead server. */
        release_registration(r);
        goto done;
    }
    status = r->status;
    if (status == ERROR_SUCCESS)
        *destination = r; /* receiver owns the successful registration */
    else
        release_registration(r);
done:
    free(name);
    for (size_t i = 0; i < MAX_PROPERTIES; i++) {
        free(keys[i]);
        free(values[i]);
    }
    return (int)status;
}

int dnssd_register_raop(dnssd_t *d, unsigned short port)
{
    return register_service(d, true, port, &d->raop_service);
}

int dnssd_register_airplay(dnssd_t *d, unsigned short port)
{
    return register_service(d, false, port, &d->airplay_service);
}

void dnssd_unregister_raop(dnssd_t *d)
{
    if (d->raop_service) {
        release_registration(d->raop_service);
        d->raop_service = NULL;
    }
}

void dnssd_unregister_airplay(dnssd_t *d)
{
    if (d->airplay_service) {
        release_registration(d->airplay_service);
        d->airplay_service = NULL;
    }
}
