#include "usb/packet.h"
#include "usb/winusb.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static ph_usb_iface iface_of(int number, int cls, int subclass, int in_ep, bool bulk_in)
{
    ph_usb_iface iface;
    memset(&iface, 0, sizeof iface);
    iface.number = number;
    iface.cls = cls;
    iface.subclass = subclass;
    iface.in_ep = in_ep;
    iface.out_ep = 0x02;
    iface.bulk_in = bulk_in;
    return iface;
}

static void test_select(void)
{
    ph_usb_iface list[6];

    list[0] = iface_of(0, PH_USB_VENDOR_CLASS, 0xFE, 0x81, true);
    list[1] = iface_of(1, PH_USB_VENDOR_CLASS, 0xFE, PH_USB_NOT_SCREEN_EP, true);
    list[2] = iface_of(2, PH_USB_VENDOR_CLASS, 0xFE, 0x83, true);
    list[3] = iface_of(4, PH_USB_VENDOR_CLASS, 0x2A, 0x85, true);
    list[4] = iface_of(6, 0x01, 0, 0x87, true);
    list[5] = iface_of(7, PH_USB_VENDOR_CLASS, 0, 0x89, false);

    CHECK(ph_usb_select_capture(list, 6) == 4);
    CHECK(ph_usb_select_capture(list, 0) == -1);
    CHECK(ph_usb_select_capture(list, -1) == -1);
    CHECK(ph_usb_select_capture(NULL, 6) == -1);

    {
        ph_usb_iface only_skipped[2];
        ph_usb_iface subclass_ok[1];
        only_skipped[0] = iface_of(0, PH_USB_VENDOR_CLASS, 0x2A, 0x81, true);
        only_skipped[1] = iface_of(3, PH_USB_VENDOR_CLASS, 0xFE, PH_USB_NOT_SCREEN_EP, true);
        CHECK(ph_usb_select_capture(only_skipped, 2) == -1);
        subclass_ok[0] = iface_of(3, PH_USB_VENDOR_CLASS, 0x2A, 0x81, true);
        CHECK(ph_usb_select_capture(subclass_ok, 1) == 3);
    }
}

static int line_has_bare_vid_pid(const char *text, const char *needle)
{
    const char *p = text;
    while ((p = strstr(p, needle)) != NULL) {
        const char *start = p;
        const char *end;
        const char *q;
        int has_mi = 0;
        while (start > text && start[-1] != '\n')
            start--;
        end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        for (q = start; q + 3 <= end; q++) {
            if (q[0] == 'M' && q[1] == 'I' && q[2] == '_')
                has_mi = 1;
        }
        if (!has_mi)
            return 1;
        p += strlen(needle);
    }
    return 0;
}

static void test_inf(void)
{
    char buf[4096];
    const char *needle = "VID_05AC&PID_12AB";

    CHECK(ph_usb_winusb_inf(buf, sizeof buf, 0x05AC, 0x12AB, 2) == 0);
    CHECK(strstr(buf, "WinUSB") != NULL);
    CHECK(strstr(buf, "MI_02") != NULL);
    CHECK(strstr(buf, "USB\\VID_05AC&PID_12AB&MI_02") != NULL);
    CHECK(!line_has_bare_vid_pid(buf, needle));
    CHECK(ph_usb_winusb_inf(buf, 8, 0x05AC, 0x12AB, 2) != 0);
    CHECK(ph_usb_winusb_inf(NULL, sizeof buf, 0x05AC, 0x12AB, 2) != 0);
    CHECK(ph_usb_winusb_inf(buf, sizeof buf, 0x05AC, 0x12AB, 256) != 0);
    CHECK(ph_usb_winusb_inf(buf, sizeof buf, -1, 1, 1) != 0);
    CHECK(ph_usb_winusb_inf(buf, sizeof buf, 0x05AC, 0x10000, 1) != 0);
    CHECK(ph_usb_winusb_inf(buf, sizeof buf, PH_USB_APPLE_VID, 0x12AB, 0) == 0);
    CHECK(strstr(buf, "MI_00") != NULL);
    CHECK(strstr(buf, "WinUSB") != NULL);
}

int main(void)
{
    test_select();
    test_inf();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all usb winusb checks passed\n");
    return 0;
}
