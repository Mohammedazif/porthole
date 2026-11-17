/* Roster of up to eight phones and iPads, with one active video source. */
#include <stdio.h>
#include <string.h>

#include "session/devices.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static void check_kind(const ph_devices *devices, const char *id, ph_device_kind kind)
{
    ph_device_view view;
    int i;

    for (i = 0; i < ph_devices_count(devices); i++) {
        CHECK(ph_devices_at(devices, i, &view));
        if (view.id && strcmp(view.id, id) == 0) {
            CHECK(view.kind == kind);
            return;
        }
    }
    fprintf(stderr, "missing device %s\n", id);
    g_failures++;
}

int main(void)
{
    ph_devices *devices;
    ph_device_view view;
    const char *active;
    const char *kept_name;
    static const char *extra[] = {"e0", "e1", "e2", "e3", "e4"};
    int i;

    CHECK(ph_devices_upsert(NULL, "a", "n", "iPhone", PH_TRANSPORT_WIFI) == -1);
    CHECK(ph_devices_count(NULL) == 0);
    CHECK(ph_devices_active_id(NULL) == NULL);
    CHECK(!ph_devices_accepts_video(NULL, "a"));
    CHECK(!ph_devices_select(NULL, "a"));
    ph_devices_disconnect(NULL, "a");
    ph_devices_destroy(NULL);

    devices = ph_devices_create();
    CHECK(devices);
    CHECK(ph_devices_count(devices) == 0);
    CHECK(ph_devices_active_id(devices) == NULL);
    CHECK(!ph_devices_accepts_video(devices, "p0"));
    CHECK(ph_devices_upsert(devices, NULL, "n", "iPhone", PH_TRANSPORT_WIFI) == -1);
    CHECK(ph_devices_upsert(devices, "", "n", "iPhone", PH_TRANSPORT_WIFI) == -1);
    CHECK(ph_devices_count(devices) == 0);

    CHECK(ph_devices_upsert(devices, "p0", "Phone A", "iPhone14,2", PH_TRANSPORT_WIFI) == 0);
    CHECK(ph_devices_upsert(devices, "p1", "Phone B", "iPhone15,3", PH_TRANSPORT_USB) == 1);
    CHECK(ph_devices_count(devices) == 2);
    CHECK(ph_devices_active_id(devices) && strcmp(ph_devices_active_id(devices), "p0") == 0);
    CHECK(ph_devices_accepts_video(devices, "p0"));
    CHECK(!ph_devices_accepts_video(devices, "p1"));
    check_kind(devices, "p0", PH_DEVICE_IPHONE);
    check_kind(devices, "p1", PH_DEVICE_IPHONE);
    CHECK(ph_devices_at(devices, 1, &view));
    CHECK(view.transport == PH_TRANSPORT_USB && view.connected && !view.active);
    CHECK(strcmp(view.model, "iPhone15,3") == 0);

    CHECK(ph_devices_upsert(devices, "pad", "Pad", "iPad13,4", PH_TRANSPORT_WIFI) == 2);
    check_kind(devices, "pad", PH_DEVICE_IPAD);
    CHECK(ph_devices_at(devices, 2, &view));
    CHECK(view.kind == PH_DEVICE_IPAD && strcmp(view.model, "iPad13,4") == 0);
    CHECK(strcmp(ph_devices_active_id(devices), "p0") == 0);

    CHECK(ph_devices_select(devices, "p1"));
    CHECK(ph_devices_accepts_video(devices, "p1"));
    CHECK(!ph_devices_accepts_video(devices, "p0"));
    CHECK(!ph_devices_accepts_video(devices, "pad"));
    CHECK(ph_devices_at(devices, 0, &view) && !view.active);
    CHECK(ph_devices_at(devices, 1, &view) && view.active);
    CHECK(!ph_devices_select(devices, "missing"));
    CHECK(!ph_devices_select(devices, NULL));
    CHECK(strcmp(ph_devices_active_id(devices), "p1") == 0);

    ph_devices_disconnect(devices, "pad");
    CHECK(strcmp(ph_devices_active_id(devices), "p1") == 0);
    CHECK(!ph_devices_accepts_video(devices, "pad"));
    CHECK(!ph_devices_select(devices, "pad"));

    ph_devices_disconnect(devices, "p1");
    active = ph_devices_active_id(devices);
    CHECK(active && strcmp(active, "p0") == 0);
    CHECK(ph_devices_accepts_video(devices, "p0"));
    CHECK(!ph_devices_accepts_video(devices, "p1"));
    CHECK(ph_devices_count(devices) == 3);

    ph_devices_disconnect(devices, "p0");
    CHECK(ph_devices_active_id(devices) == NULL);
    CHECK(!ph_devices_accepts_video(devices, "p0"));
    CHECK(ph_devices_upsert(devices, "p0", "Phone A2", "iPad16,1", PH_TRANSPORT_USB) == 0);
    check_kind(devices, "p0", PH_DEVICE_IPAD);
    CHECK(ph_devices_at(devices, 0, &view));
    CHECK(view.connected && view.active && view.transport == PH_TRANSPORT_USB);
    CHECK(strcmp(view.name, "Phone A2") == 0);
    CHECK(strcmp(ph_devices_active_id(devices), "p0") == 0);

    CHECK(ph_devices_upsert(devices, "p1", "Phone B", "iPhone15,3", PH_TRANSPORT_WIFI) == 1);
    CHECK(ph_devices_upsert(devices, "pad", "Pad", "iPad13,4", PH_TRANSPORT_WIFI) == 2);
    CHECK(strcmp(ph_devices_active_id(devices), "p0") == 0);
    CHECK(ph_devices_count(devices) == 3);

    for (i = 0; i < 5; i++)
        CHECK(ph_devices_upsert(devices, extra[i], extra[i], "iPhone12,1", PH_TRANSPORT_WIFI) == 3 + i);
    CHECK(ph_devices_count(devices) == PH_DEVICES_MAX);
    kept_name = NULL;
    CHECK(ph_devices_at(devices, 0, &view));
    kept_name = view.name;
    CHECK(ph_devices_upsert(devices, "ninth", "Nope", "iPhone1,1", PH_TRANSPORT_USB) == -1);
    CHECK(ph_devices_upsert(devices, "", "Nope", "iPhone1,1", PH_TRANSPORT_USB) == -1);
    CHECK(ph_devices_count(devices) == PH_DEVICES_MAX);
    CHECK(ph_devices_at(devices, 0, &view));
    CHECK(view.name == kept_name && strcmp(view.name, "Phone A2") == 0);
    CHECK(strcmp(ph_devices_active_id(devices), "p0") == 0);
    CHECK(ph_devices_upsert(devices, "p1", "Renamed", "iPod", PH_TRANSPORT_USB) == 1);
    check_kind(devices, "p1", PH_DEVICE_UNKNOWN);
    CHECK(ph_devices_at(devices, 1, &view));
    CHECK(strcmp(view.name, "Renamed") == 0 && view.transport == PH_TRANSPORT_USB && view.connected);
    CHECK(ph_devices_upsert(devices, "tenth", "Nope", "iPad", PH_TRANSPORT_WIFI) == -1);
    CHECK(ph_devices_count(devices) == PH_DEVICES_MAX);
    CHECK(!ph_devices_at(devices, PH_DEVICES_MAX, &view));

    ph_devices_destroy(devices);
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all device roster checks passed\n");
    return 0;
}
