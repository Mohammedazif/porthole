#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "usb/capture.h"

#include "core/log.h"
#include "usb/packet.h"
#include "usb/winusb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#ifdef interface
#undef interface
#endif
#else
#include <time.h>
#endif

#ifdef PH_USB_HAVE_LIBUSB
#include <libusb.h>
#endif

struct ph_usb_capture {
    ph_usb_transport io;
    ph_usb_reasm reasm;
    void (*release)(void *user);
    uint64_t local_audio;
    uint64_t local_video;
    uint64_t local_clok;
};

static uint64_t mono_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency) ||
        ticks.QuadPart < 0 || frequency.QuadPart <= 0)
        return 0;
    uint64_t counter = (uint64_t)ticks.QuadPart;
    uint64_t rate = (uint64_t)frequency.QuadPart;
    return counter / rate * 1000000000ull + (counter % rate) * 1000000000ull / rate;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
#endif
}

ph_usb_capture *ph_usb_capture_open(const ph_usb_transport *io)
{
    ph_usb_capture *cap;
    if (!io || !io->read || !io->write)
        return NULL;
    cap = calloc(1, sizeof *cap);
    if (!cap)
        return NULL;
    cap->io = *io;
    ph_usb_reasm_init(&cap->reasm);
    cap->local_audio = 0x00007FA66CE20CB0ULL;
    cap->local_video = 0x00007FA66CD10250ULL;
    cap->local_clok = 0x00007FA67CC17980ULL;
    return cap;
}

void ph_usb_capture_close(ph_usb_capture *cap)
{
    if (!cap)
        return;
    if (cap->release)
        cap->release(cap->io.user);
    ph_usb_reasm_free(&cap->reasm);
    free(cap);
}

static int io_write(ph_usb_capture *cap, const uint8_t *src, size_t n)
{
    int wrote;
    if (n > 0x7fffffff)
        return -1;
    wrote = cap->io.write(cap->io.user, src, (int)n, 1000);
    if (wrote != (int)n)
        return -1;
    return 0;
}

static int handle_frame(ph_usb_capture *cap, const uint8_t *frame, size_t len, ph_usb_sample *out,
                        uint8_t *copy_buf, size_t copy_cap)
{
    ph_usb_frame parsed;
    uint8_t built[128];
    size_t n;

    if (!ph_usb_frame_parse(frame, len, &parsed))
        return -1;

    if (parsed.kind == PH_USB_FRAME_PING) {
        n = ph_usb_build_ping(built, sizeof built);
        if (n == 0 || io_write(cap, built, n) != 0)
            return -1;
        return 0;
    }

    if (parsed.kind == PH_USB_FRAME_SYNC) {
        uint64_t clock = 0;
        if (parsed.subtype == PH_USB_CVRP && parsed.has_device_clock && parsed.device_clock != 0) {
            n = ph_usb_build_need(built, sizeof built, parsed.device_clock);
            if (n == 0 || io_write(cap, built, n) != 0)
                return -1;
        }
        if (parsed.subtype == PH_USB_CWPA)
            clock = cap->local_audio;
        else if (parsed.subtype == PH_USB_CVRP)
            clock = cap->local_video;
        else if (parsed.subtype == PH_USB_CLOK)
            clock = cap->local_clok;
        else if (parsed.subtype == PH_USB_TIME)
            clock = mono_ns();
        n = ph_usb_build_sync_reply(built, sizeof built, parsed.subtype, parsed.correlation, clock);
        if (n == 0)
            return 0;
        if (io_write(cap, built, n) != 0)
            return -1;
        return 0;
    }

    if (parsed.kind == PH_USB_FRAME_ASYN &&
        (parsed.subtype == PH_USB_FEED || parsed.subtype == PH_USB_EAT) && parsed.sdat) {
        if (parsed.sdat_len > copy_cap)
            return -1;
        if (parsed.sdat_len > 0) {
            if (!copy_buf)
                return -1;
            memcpy(copy_buf, parsed.sdat, parsed.sdat_len);
        }
        out->video = parsed.subtype == PH_USB_FEED;
        out->data = copy_buf;
        out->len = parsed.sdat_len;
        return 1;
    }
    return 0;
}

static int drain(ph_usb_capture *cap, ph_usb_sample *out, uint8_t *copy_buf, size_t copy_cap)
{
    for (;;) {
        const uint8_t *frame = NULL;
        size_t frame_len = 0;
        int pop = ph_usb_reasm_pop(&cap->reasm, &frame, &frame_len);
        int handled;
        if (pop < 0)
            return -1;
        if (pop == 0)
            return 0;
        handled = handle_frame(cap, frame, frame_len, out, copy_buf, copy_cap);
        if (handled != 0)
            return handled;
    }
}

int ph_usb_capture_poll(ph_usb_capture *cap, ph_usb_sample *out, uint8_t *copy_buf, size_t copy_cap)
{
    int spin;
    if (!cap || !out)
        return -1;
    for (spin = 0; spin < 64; spin++) {
        uint8_t chunk[8192];
        int pending = drain(cap, out, copy_buf, copy_cap);
        int n;
        if (pending != 0)
            return pending;
        n = cap->io.read(cap->io.user, chunk, (int)sizeof chunk, 1000);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        if (ph_usb_reasm_push(&cap->reasm, chunk, (size_t)n) != 0)
            return -1;
    }
    return 0;
}

#ifdef PH_USB_HAVE_LIBUSB

enum { PH_USB_IFACE_CAP = 32 };

typedef struct ph_usb_dev {
    libusb_context *ctx;
    libusb_device_handle *handle;
    int iface;
    uint8_t in_ep;
    uint8_t out_ep;
} ph_usb_dev;

static void usb_pause(void)
{
#ifdef _WIN32
    Sleep(200);
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 200000000L;
    nanosleep(&ts, NULL);
#endif
}

static void usb_dev_release(void *user)
{
    ph_usb_dev *dev = user;
    if (!dev)
        return;
    if (dev->handle) {
        libusb_release_interface(dev->handle, dev->iface);
        libusb_close(dev->handle);
    }
    if (dev->ctx)
        libusb_exit(dev->ctx);
    free(dev);
}

static int usb_dev_read(void *user, uint8_t *dst, int cap, int timeout_ms)
{
    ph_usb_dev *dev = user;
    int transferred = 0;
    unsigned int timeout;
    int rc;
    if (!dev || !dev->handle || !dst || cap < 0)
        return -1;
    if (cap == 0)
        return 0;
    timeout = timeout_ms > 0 ? (unsigned int)timeout_ms : 0;
    rc = libusb_bulk_transfer(dev->handle, dev->in_ep, dst, cap, &transferred, timeout);
    if (rc == LIBUSB_ERROR_TIMEOUT)
        return transferred > 0 ? transferred : 0;
    if (rc != 0)
        return -1;
    return transferred > 0 ? transferred : 0;
}

static int usb_dev_write(void *user, const uint8_t *src, int len, int timeout_ms)
{
    ph_usb_dev *dev = user;
    int sent = 0;
    unsigned int timeout;
    if (!dev || !dev->handle || (!src && len > 0) || len < 0)
        return -1;
    timeout = timeout_ms > 0 ? (unsigned int)timeout_ms : 0;
    while (sent < len) {
        int transferred = 0;
        int rc = libusb_bulk_transfer(dev->handle, dev->out_ep, (uint8_t *)(src + sent), len - sent,
                                      &transferred, timeout);
        if (rc != 0 || transferred <= 0)
            return -1;
        sent += transferred;
    }
    return sent;
}

static bool ep_is_bulk_in(uint8_t attr, uint8_t addr)
{
    return (attr & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK &&
           (addr & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
}

static bool ep_is_bulk_out(uint8_t attr, uint8_t addr)
{
    return (attr & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK &&
           (addr & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT;
}

static ph_usb_dev *claim_apple(libusb_context *ctx, libusb_device *device)
{
    struct libusb_device_descriptor desc;
    ph_usb_iface ifaces[PH_USB_IFACE_CAP];
    int cfg_values[PH_USB_IFACE_CAP];
    int count = 0;
    int selected;
    int chosen = -1;
    libusb_device_handle *handle = NULL;
    ph_usb_dev *dev;
    uint8_t c;

    if (libusb_get_device_descriptor(device, &desc) != 0)
        return NULL;
    if (desc.idVendor != PH_USB_APPLE_VID)
        return NULL;

    for (c = 0; c < desc.bNumConfigurations && count < PH_USB_IFACE_CAP; c++) {
        struct libusb_config_descriptor *cfg = NULL;
        int i;
        if (libusb_get_config_descriptor(device, c, &cfg) != 0)
            continue;
        for (i = 0; i < cfg->bNumInterfaces && count < PH_USB_IFACE_CAP; i++) {
            const struct libusb_interface_descriptor *alt;
            ph_usb_iface *iface;
            int e;
            if (cfg->interface[i].num_altsetting < 1)
                continue;
            alt = &cfg->interface[i].altsetting[0];
            iface = &ifaces[count];
            memset(iface, 0, sizeof *iface);
            iface->number = alt->bInterfaceNumber;
            iface->cls = alt->bInterfaceClass;
            iface->subclass = alt->bInterfaceSubClass;
            iface->protocol = alt->bInterfaceProtocol;
            for (e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if (iface->in_ep == 0 && ep_is_bulk_in(ep->bmAttributes, ep->bEndpointAddress)) {
                    iface->in_ep = ep->bEndpointAddress;
                    iface->bulk_in = true;
                } else if (iface->out_ep == 0 &&
                           ep_is_bulk_out(ep->bmAttributes, ep->bEndpointAddress)) {
                    iface->out_ep = ep->bEndpointAddress;
                }
            }
            cfg_values[count] = cfg->bConfigurationValue;
            count++;
        }
        libusb_free_config_descriptor(cfg);
    }

    selected = ph_usb_select_capture(ifaces, count);
    if (selected < 0)
        return NULL;
    for (c = 0; c < count; c++) {
        if (ifaces[c].number == selected && ifaces[c].cls == PH_USB_VENDOR_CLASS &&
            ifaces[c].bulk_in && ifaces[c].in_ep != PH_USB_NOT_SCREEN_EP && ifaces[c].number > 0)
            chosen = (int)c;
    }
    if (chosen < 0 || ifaces[chosen].out_ep == 0)
        return NULL;
    if (libusb_open(device, &handle) != 0)
        return NULL;

    (void)libusb_set_auto_detach_kernel_driver(handle, 1);
    {
        int active = -1;
        if (libusb_get_configuration(handle, &active) != 0 || active != cfg_values[chosen]) {
            if (libusb_set_configuration(handle, cfg_values[chosen]) != 0) {
                libusb_close(handle);
                return NULL;
            }
        }
    }
    if (libusb_claim_interface(handle, ifaces[chosen].number) != 0) {
        (void)libusb_detach_kernel_driver(handle, ifaces[chosen].number);
        if (libusb_claim_interface(handle, ifaces[chosen].number) != 0) {
            libusb_close(handle);
            return NULL;
        }
    }

    dev = calloc(1, sizeof *dev);
    if (!dev) {
        libusb_release_interface(handle, ifaces[chosen].number);
        libusb_close(handle);
        return NULL;
    }
    dev->ctx = ctx;
    dev->handle = handle;
    dev->iface = ifaces[chosen].number;
    dev->in_ep = (uint8_t)ifaces[chosen].in_ep;
    dev->out_ep = (uint8_t)ifaces[chosen].out_ep;
    return dev;
}

static ph_usb_dev *try_claim(libusb_context *ctx)
{
    libusb_device **list = NULL;
    ssize_t n;
    ssize_t i;
    ph_usb_dev *found = NULL;

    n = libusb_get_device_list(ctx, &list);
    if (n < 0)
        return NULL;
    for (i = 0; i < n && !found; i++)
        found = claim_apple(ctx, list[i]);
    libusb_free_device_list(list, 1);
    return found;
}

static void send_enable(libusb_context *ctx)
{
    libusb_device **list = NULL;
    ssize_t n;
    ssize_t i;

    n = libusb_get_device_list(ctx, &list);
    if (n < 0)
        return;
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        libusb_device_handle *handle = NULL;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != PH_USB_APPLE_VID)
            continue;
        if (libusb_open(list[i], &handle) != 0)
            continue;
        (void)libusb_control_transfer(handle, PH_USB_ENABLE_REQUEST_TYPE, PH_USB_ENABLE_REQUEST,
                                      PH_USB_ENABLE_VALUE, PH_USB_ENABLE_INDEX, NULL, 0, 1000);
        libusb_close(handle);
    }
    libusb_free_device_list(list, 1);
}

ph_usb_capture *ph_usb_libusb_open(void)
{
    libusb_context *ctx = NULL;
    ph_usb_dev *dev;
    ph_usb_transport io;
    ph_usb_capture *cap;
    int attempt;

    if (libusb_init(&ctx) != 0)
        return NULL;
    dev = try_claim(ctx);
    if (!dev) {
        send_enable(ctx);
        for (attempt = 0; attempt < 15 && !dev; attempt++) {
            usb_pause();
            dev = try_claim(ctx);
        }
    }
    if (!dev) {
        libusb_exit(ctx);
        return NULL;
    }

    io.read = usb_dev_read;
    io.write = usb_dev_write;
    io.user = dev;
    cap = ph_usb_capture_open(&io);
    if (!cap) {
        usb_dev_release(dev);
        return NULL;
    }
    cap->release = usb_dev_release;
    PH_LOGI("usb screen iface %d in %02x out %02x", dev->iface, (unsigned)dev->in_ep,
            (unsigned)dev->out_ep);
    return cap;
}

int ph_usb_write_winusb_inf(const char *path)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    ssize_t n;
    ssize_t i;
    int wrote = -1;

    if (!path || !path[0])
        return -1;
    if (libusb_init(&ctx) != 0)
        return -1;
    n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        libusb_exit(ctx);
        return -1;
    }
    for (i = 0; i < n && wrote != 0; i++) {
        struct libusb_device_descriptor desc;
        ph_usb_iface ifaces[PH_USB_IFACE_CAP];
        int count = 0;
        int selected;
        uint8_t c;

        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != PH_USB_APPLE_VID)
            continue;
        for (c = 0; c < desc.bNumConfigurations && count < PH_USB_IFACE_CAP; c++) {
            struct libusb_config_descriptor *cfg = NULL;
            int iface_i;
            if (libusb_get_config_descriptor(list[i], c, &cfg) != 0)
                continue;
            for (iface_i = 0; iface_i < cfg->bNumInterfaces && count < PH_USB_IFACE_CAP; iface_i++) {
                const struct libusb_interface_descriptor *alt;
                ph_usb_iface *iface;
                int e;
                if (cfg->interface[iface_i].num_altsetting < 1)
                    continue;
                alt = &cfg->interface[iface_i].altsetting[0];
                iface = &ifaces[count];
                memset(iface, 0, sizeof *iface);
                iface->number = alt->bInterfaceNumber;
                iface->cls = alt->bInterfaceClass;
                for (e = 0; e < alt->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                    if (iface->in_ep == 0 && ep_is_bulk_in(ep->bmAttributes, ep->bEndpointAddress)) {
                        iface->in_ep = ep->bEndpointAddress;
                        iface->bulk_in = true;
                    }
                }
                count++;
            }
            libusb_free_config_descriptor(cfg);
        }
        selected = ph_usb_select_capture(ifaces, count);
        if (selected < 0)
            continue;
        {
            char text[2048];
            FILE *file;
            if (ph_usb_winusb_inf(text, sizeof text, desc.idVendor, desc.idProduct, selected) != 0)
                continue;
            file = fopen(path, "w");
            if (!file)
                continue;
            if (fputs(text, file) < 0) {
                fclose(file);
                continue;
            }
            if (fclose(file) == 0)
                wrote = 0;
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return wrote;
}

#endif
