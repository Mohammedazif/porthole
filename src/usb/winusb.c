#include "usb/winusb.h"

#include "usb/packet.h"

#include <stdio.h>

int ph_usb_select_capture(const ph_usb_iface *list, int count)
{
    if (!list || count <= 0)
        return -1;
    int best = -1;
    for (int i = 0; i < count; i++) {
        const ph_usb_iface *iface = &list[i];
        if (iface->number <= 0)
            continue;
        if (iface->cls != PH_USB_VENDOR_CLASS)
            continue;
        if (!iface->bulk_in)
            continue;
        if (iface->in_ep == PH_USB_NOT_SCREEN_EP)
            continue;
        if (iface->number >= best)
            best = iface->number;
    }
    return best;
}

int ph_usb_winusb_inf(char *out, size_t out_len, int vid, int pid, int iface)
{
    char hwid[48];
    int hw;
    int n;

    if (!out || out_len == 0)
        return -1;
    if (vid < 0 || vid > 0xFFFF || pid < 0 || pid > 0xFFFF || iface < 0 || iface > 255)
        return -1;

    hw = snprintf(hwid, sizeof hwid, "USB\\VID_%04X&PID_%04X&MI_%02X", vid, pid, iface);
    if (hw < 0 || (size_t)hw >= sizeof hwid)
        return -1;

    n = snprintf(out, out_len,
                 "[Version]\n"
                 "Signature   = \"$Windows NT$\"\n"
                 "Class       = USBDevice\n"
                 "ClassGuid   = {88BAE032-5A81-49f0-BC3D-A4FF138216D6}\n"
                 "Provider    = %%ManufacturerName%%\n"
                 "DriverVer   = 11/17/2026,1.0.0.0\n"
                 "\n"
                 "[Manufacturer]\n"
                 "%%ManufacturerName%% = Devices,NTamd64\n"
                 "\n"
                 "[Devices.NTamd64]\n"
                 "%%DeviceName%% = USB_Install, %s\n"
                 "\n"
                 "[USB_Install.NT]\n"
                 "Include = winusb.inf\n"
                 "Needs   = WINUSB.NT\n"
                 "\n"
                 "[USB_Install.NT.Services]\n"
                 "Include = winusb.inf\n"
                 "Needs   = WINUSB.NT.Services\n"
                 "\n"
                 "[USB_Install.NT.HW]\n"
                 "AddReg = Dev_AddReg\n"
                 "\n"
                 "[Dev_AddReg]\n"
                 "HKR,,DeviceInterfaceGUIDs,0x10000,\"{A5B4C3D2-1E0F-4A6B-8C7D-9E0F1A2B3C4D}\"\n"
                 "\n"
                 "[Strings]\n"
                 "ManufacturerName = \"Porthole\"\n"
                 "DeviceName = \"Porthole WinUSB screen\"\n",
                 hwid);
    if (n < 0 || (size_t)n >= out_len)
        return -1;
    return 0;
}
