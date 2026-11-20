#include "core/bonjour.h"

#include "core/log.h"

#include <stdlib.h>

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
