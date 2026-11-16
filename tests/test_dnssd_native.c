/* Exercise the actual native adapter through deterministic Windows API fakes:
 * no advertising, Windows machine, timing sleeps or admin rights are needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "fakes/windows_dns.h"
#include "airplay/dnssd_records.h"

static int failures, instances, events, deregistrations, cancellations;
static int mode; /* 0 success, 1 immediate failure, 2 callback failure, 3 timeout */
static DNS_SERVICE_REGISTER_REQUEST pending;
static wchar_t last_name[200];
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

LONG InterlockedDecrement(LONG *value) { return --*value; }
HANDLE CreateEventW(void *attributes, int manual, int initial, PCWSTR name)
{
    (void)attributes; (void)manual; (void)name;
    int *event = malloc(sizeof *event);
    *event = initial;
    events++;
    return event;
}
int SetEvent(HANDLE event) { *(int *)event = 1; return 1; }
int CloseHandle(HANDLE event) { free(event); events--; return 1; }
int MultiByteToWideChar(unsigned int cp, DWORD flags, const char *s, int count,
                        wchar_t *out, int capacity)
{
    (void)cp; (void)flags; (void)count;
    int n = (int)strlen(s) + 1;
    if (out && capacity >= n)
        for (int i = 0; i < n; i++) out[i] = (unsigned char)s[i];
    return n;
}
int GetComputerNameExW(int kind, wchar_t *name, DWORD *length)
{
    (void)kind;
    wcscpy(name, L"pc"); *length = 2;
    return 1;
}
PDNS_SERVICE_INSTANCE DnsServiceConstructInstance(PCWSTR name, PCWSTR host, void *ip4,
        void *ip6, uint16_t port, uint16_t priority, uint16_t weight, DWORD count,
        PCWSTR *keys, PCWSTR *values)
{
    (void)priority; (void)weight;
    CHECK(!wcscmp(host, L"pc.local") && !ip4 && !ip6 && port == 7000);
    CHECK(count == 19 || count == 9);
    CHECK(keys && values && keys[0] && values[0]);
    wcscpy(last_name, name);
    PDNS_SERVICE_INSTANCE instance = malloc(sizeof *instance);
    instance->name = malloc((wcslen(name) + 1) * sizeof(wchar_t));
    wcscpy(instance->name, name);
    instances++;
    return instance;
}
void DnsServiceFreeInstance(PDNS_SERVICE_INSTANCE instance)
{
    if (instance) { free(instance->name); free(instance); instances--; }
}
static PDNS_SERVICE_INSTANCE copy_instance(PDNS_SERVICE_INSTANCE input)
{
    PDNS_SERVICE_INSTANCE copy = malloc(sizeof *copy);
    copy->name = malloc((wcslen(input->name) + 1) * sizeof(wchar_t));
    wcscpy(copy->name, input->name);
    instances++;
    return copy;
}
DWORD DnsServiceRegister(PDNS_SERVICE_REGISTER_REQUEST request, PDNS_SERVICE_CANCEL cancel)
{
    CHECK(request->Version == 1 && request->InterfaceIndex == 0 && !request->unicastEnabled);
    CHECK(cancel && request->pQueryContext && request->pRegisterCompletionCallback);
    pending = *request;
    return mode == 1 ? ERROR_ACCESS_DENIED : DNS_REQUEST_PENDING;
}
DWORD WaitForSingleObject(HANDLE event, DWORD timeout)
{
    CHECK(timeout == 5000);
    if (mode == 3)
        return WAIT_TIMEOUT;
    pending.pRegisterCompletionCallback(mode == 2 ? ERROR_ACCESS_DENIED : ERROR_SUCCESS,
        pending.pQueryContext, copy_instance(pending.pServiceInstance));
    CHECK(*(int *)event);
    return WAIT_OBJECT_0;
}
DWORD DnsServiceDeRegister(PDNS_SERVICE_REGISTER_REQUEST request, PDNS_SERVICE_CANCEL cancel)
{
    CHECK(!cancel);
    deregistrations++;
    /* Completion before the API call returns exercises callback ownership. */
    request->pRegisterCompletionCallback(ERROR_SUCCESS, request->pQueryContext,
                                        copy_instance(request->pServiceInstance));
    return DNS_REQUEST_PENDING;
}
DWORD DnsServiceRegisterCancel(PDNS_SERVICE_CANCEL cancel)
{
    CHECK(cancel); cancellations++;
    return ERROR_SUCCESS;
}

int main(void)
{
    const char address[6] = {1,2,3,4,5,6};
    int error;
    dnssd_t *d = dnssd_init("Porthole.Lab", 12, address, 6, &error, 0);
    CHECK(d);
    CHECK(dnssd_register_raop(d, 7000) == 0);
    CHECK(!wcscmp(last_name, L"010203040506@Porthole\\.Lab._raop._tcp.local"));
    CHECK(dnssd_register_airplay(d, 7000) == 0);
    CHECK(!wcscmp(last_name, L"Porthole\\.Lab._airplay._tcp.local"));
    CHECK(dnssd_register_airplay(d, 7000) == ERROR_ALREADY_EXISTS);
    dnssd_destroy(d);
    CHECK(deregistrations == 2 && instances == 0 && events == 0);
    for (mode = 1; mode <= 2; mode++) {
        d = dnssd_init("Phone", 5, address, 6, &error, 0);
        CHECK(dnssd_register_airplay(d, 7000) == ERROR_ACCESS_DENIED);
        dnssd_destroy(d);
        CHECK(instances == 0 && events == 0 && deregistrations == 2);
    }
    mode = 3;
    d = dnssd_init("Phone", 5, address, 6, &error, 0);
    CHECK(dnssd_register_airplay(d, 7000) == ERROR_TIMEOUT && cancellations == 1);
    dnssd_destroy(d);
    CHECK(instances == 1 && events == 1); /* late callback still owns its inputs */
    pending.pRegisterCompletionCallback(ERROR_SUCCESS, pending.pQueryContext,
                                        copy_instance(pending.pServiceInstance));
    CHECK(instances == 0 && events == 0 && deregistrations == 3);
    if (failures)
        return 1;
    puts("native Windows registration, errors, teardown and late callbacks passed");
    return 0;
}
