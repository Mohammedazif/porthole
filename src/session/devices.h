#ifndef PORTHOLE_SESSION_DEVICES_H
#define PORTHOLE_SESSION_DEVICES_H

#include <stdbool.h>

#include "video/codecs.h"

#define PH_DEVICES_MAX 8
#define PH_SESSION_DEVICE_MAX 16

typedef enum ph_transport {
    PH_TRANSPORT_WIFI = 0,
    PH_TRANSPORT_USB
} ph_transport;

typedef struct ph_devices ph_devices;

typedef struct ph_device_view {
    const char *id;
    const char *name;
    const char *model;
    ph_device_kind kind;
    ph_transport transport;
    bool connected;
    bool active;
} ph_device_view;

typedef struct ph_session_device {
    char id[129], name[129];
    bool connected, active, usb, companion, screen_sharing;
} ph_session_device;
typedef struct ph_session_state {
    ph_session_device devices[PH_SESSION_DEVICE_MAX];
    int count;
    bool pin_required;
    char request_id[129], request_name[129], pin[8];
} ph_session_state;
void ph_devices_remove(ph_devices *devices, const char *id);
ph_devices *ph_devices_create(void);
void ph_devices_destroy(ph_devices *devices);

/* Index of the stored device, or -1 if id is NULL/empty or every slot holds another id.
 * An existing id is refreshed and marked connected. The first connected device is active. */
int ph_devices_upsert(ph_devices *devices, const char *id, const char *name, const char *model,
                      ph_transport transport);

void ph_devices_disconnect(ph_devices *devices, const char *id);
bool ph_devices_rename(ph_devices *devices, const char *id, const char *name);

/* The connected device becomes the only active one. */
bool ph_devices_select(ph_devices *devices, const char *id);

const char *ph_devices_active_id(const ph_devices *devices);

/* True only for the active connected device. */
bool ph_devices_accepts_video(const ph_devices *devices, const char *id);

/* Stored devices, including disconnected ones. */
int ph_devices_count(const ph_devices *devices);

bool ph_devices_at(const ph_devices *devices, int index, ph_device_view *out);

#endif
