#ifndef PORTHOLE_FAKE_WINDOWS_DNS_H
#define PORTHOLE_FAKE_WINDOWS_DNS_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#define WINAPI
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef void *HANDLE;
typedef void *PVOID;
typedef const wchar_t *PCWSTR;
#define TRUE 1
#define FALSE 0
#define ERROR_SUCCESS 0
#define ERROR_INVALID_DATA 13
#define ERROR_INVALID_NAME 123
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_ALREADY_EXISTS 183
#define ERROR_TIMEOUT 1460
#define ERROR_ACCESS_DENIED 5
#define DNS_REQUEST_PENDING 9506
#define DNS_QUERY_REQUEST_VERSION1 1
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 8
#define ComputerNamePhysicalDnsHostname 5
/* Keep test doubles separate from the real kernel32 import symbols when the
 * same deterministic tests run on Windows itself. */
#define InterlockedDecrement ph_fake_InterlockedDecrement
#define CreateEventW ph_fake_CreateEventW
#define SetEvent ph_fake_SetEvent
#define CloseHandle ph_fake_CloseHandle
#define WaitForSingleObject ph_fake_WaitForSingleObject
#define MultiByteToWideChar ph_fake_MultiByteToWideChar
#define GetComputerNameExW ph_fake_GetComputerNameExW
#define DnsServiceConstructInstance ph_fake_DnsServiceConstructInstance
#define DnsServiceFreeInstance ph_fake_DnsServiceFreeInstance
#define DnsServiceRegister ph_fake_DnsServiceRegister
#define DnsServiceDeRegister ph_fake_DnsServiceDeRegister
#define DnsServiceRegisterCancel ph_fake_DnsServiceRegisterCancel
typedef struct instance { wchar_t *name; } DNS_SERVICE_INSTANCE, *PDNS_SERVICE_INSTANCE;
typedef struct { void *context; } DNS_SERVICE_CANCEL, *PDNS_SERVICE_CANCEL;
typedef void DNS_SERVICE_REGISTER_COMPLETE(DWORD, PVOID, PDNS_SERVICE_INSTANCE);
typedef struct {
    DWORD Version, InterfaceIndex;
    PDNS_SERVICE_INSTANCE pServiceInstance;
    DNS_SERVICE_REGISTER_COMPLETE *pRegisterCompletionCallback;
    PVOID pQueryContext;
    HANDLE hCredentials;
    int unicastEnabled;
} DNS_SERVICE_REGISTER_REQUEST, *PDNS_SERVICE_REGISTER_REQUEST;
LONG InterlockedDecrement(LONG *value);
HANDLE CreateEventW(void *, int, int, PCWSTR);
int SetEvent(HANDLE);
int CloseHandle(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD);
int MultiByteToWideChar(unsigned int, DWORD, const char *, int, wchar_t *, int);
int GetComputerNameExW(int, wchar_t *, DWORD *);
PDNS_SERVICE_INSTANCE DnsServiceConstructInstance(PCWSTR, PCWSTR, void *, void *,
                                                 uint16_t, uint16_t, uint16_t, DWORD,
                                                 PCWSTR *, PCWSTR *);
void DnsServiceFreeInstance(PDNS_SERVICE_INSTANCE);
DWORD DnsServiceRegister(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
DWORD DnsServiceDeRegister(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
DWORD DnsServiceRegisterCancel(PDNS_SERVICE_CANCEL);
#endif
