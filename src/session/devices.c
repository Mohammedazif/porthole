#include "session/devices.h"

#include <stdlib.h>
#include <string.h>

typedef struct ph_device_slot {
    char *id;
    char *name;
    char *model;
    ph_device_kind kind;
    ph_transport transport;
    bool connected;
    bool active;
} ph_device_slot;

struct ph_devices {
    ph_device_slot items[PH_DEVICES_MAX];
    int count;
};

static char *ph_dup(const char *text)
{
    size_t n;
    char *copy;

    if (!text)
        text = "";
    n = strlen(text);
    copy = malloc(n + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, n + 1);
    return copy;
}

static int find_id(const ph_devices *devices, const char *id)
{
    int i;

    if (!devices || !id)
        return -1;
    for (i = 0; i < devices->count; i++) {
        if (strcmp(devices->items[i].id, id) == 0)
            return i;
    }
    return -1;
}

static bool other_active(const ph_devices *devices, int self)
{
    int i;

    for (i = 0; i < devices->count; i++) {
        if (i == self)
            continue;
        if (devices->items[i].connected && devices->items[i].active)
            return true;
    }
    return false;
}

ph_devices *ph_devices_create(void)
{
    return calloc(1, sizeof(ph_devices));
}

void ph_devices_destroy(ph_devices *devices)
{
    int i;

    if (!devices)
        return;
    for (i = 0; i < devices->count; i++) {
        free(devices->items[i].id);
        free(devices->items[i].name);
        free(devices->items[i].model);
    }
    free(devices);
}

int ph_devices_upsert(ph_devices *devices, const char *id, const char *name, const char *model,
                      ph_transport transport)
{
    char *copy_name, *copy_model, *copy_id;
    ph_device_slot *slot;
    int index;

    if (!devices || !id || id[0] == '\0')
        return -1;
    index = find_id(devices, id);
    copy_name = ph_dup(name);
    copy_model = ph_dup(model);
    if (!copy_name || !copy_model) {
        free(copy_name);
        free(copy_model);
        return -1;
    }
    if (index >= 0) {
        slot = &devices->items[index];
        free(slot->name);
        free(slot->model);
    } else {
        if (devices->count >= PH_DEVICES_MAX) {
            free(copy_name);
            free(copy_model);
            return -1;
        }
        copy_id = ph_dup(id);
        if (!copy_id) {
            free(copy_name);
            free(copy_model);
            return -1;
        }
        index = devices->count++;
        slot = &devices->items[index];
        slot->id = copy_id;
        slot->active = false;
    }
    slot->name = copy_name;
    slot->model = copy_model;
    slot->kind = ph_device_kind_from_model(model);
    slot->transport = transport;
    slot->connected = true;
    if (!other_active(devices, index))
        slot->active = true;
    return index;
}

void ph_devices_disconnect(ph_devices *devices, const char *id)
{
    int index, i;
    bool was_active;

    index = find_id(devices, id);
    if (index < 0)
        return;
    was_active = devices->items[index].active && devices->items[index].connected;
    devices->items[index].connected = false;
    devices->items[index].active = false;
    if (!was_active)
        return;
    for (i = 0; i < devices->count; i++) {
        if (devices->items[i].connected) {
            devices->items[i].active = true;
            return;
        }
    }
}

bool ph_devices_rename(ph_devices *devices, const char *id, const char *name)
{
    int index = find_id(devices, id);
    if (index < 0 || !name || !*name)
        return false;
    char *copy = ph_dup(name);
    if (!copy)
        return false;
    free(devices->items[index].name);
    devices->items[index].name = copy;
    return true;
}

bool ph_devices_select(ph_devices *devices, const char *id)
{
    int index, i;

    index = find_id(devices, id);
    if (index < 0 || !devices->items[index].connected)
        return false;
    for (i = 0; i < devices->count; i++)
        devices->items[i].active = false;
    devices->items[index].active = true;
    return true;
}

const char *ph_devices_active_id(const ph_devices *devices)
{
    int i;

    if (!devices)
        return NULL;
    for (i = 0; i < devices->count; i++) {
        if (devices->items[i].active && devices->items[i].connected)
            return devices->items[i].id;
    }
    return NULL;
}

bool ph_devices_accepts_video(const ph_devices *devices, const char *id)
{
    int index = find_id(devices, id);

    if (index < 0)
        return false;
    return devices->items[index].connected && devices->items[index].active;
}

int ph_devices_count(const ph_devices *devices)
{
    return devices ? devices->count : 0;
}

bool ph_devices_at(const ph_devices *devices, int index, ph_device_view *out)
{
    const ph_device_slot *slot;

    if (!devices || !out || index < 0 || index >= devices->count)
        return false;
    slot = &devices->items[index];
    out->id = slot->id;
    out->name = slot->name;
    out->model = slot->model;
    out->kind = slot->kind;
    out->transport = slot->transport;
    out->connected = slot->connected;
    out->active = slot->active;
    return true;
}

void ph_devices_remove(ph_devices *devices, const char *id)
{
    int i = find_id(devices, id);
    if (i < 0)
        return;
    ph_devices_disconnect(devices, id);
    free(devices->items[i].id);
    free(devices->items[i].name);
    free(devices->items[i].model);
    devices->items[i] = devices->items[--devices->count];
}
