#include "security/gate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PH_GATE_TEXT_MAX 128
#define PH_GATE_LOCKOUT 5

typedef struct ph_gate_device {
    char id[PH_GATE_TEXT_MAX + 1];
    char name[PH_GATE_TEXT_MAX + 1];
} ph_gate_device;

typedef struct ph_gate_fail {
    char id[PH_GATE_TEXT_MAX + 1];
    int count;
} ph_gate_fail;

struct ph_gate {
    ph_gate_device *allowed;
    size_t allowed_count;
    size_t allowed_cap;
    ph_gate_fail *fails;
    size_t fail_count;
    size_t fail_cap;
    bool open_lan;
    bool has_pending;
    char pending_id[PH_GATE_TEXT_MAX + 1];
    char pending_pin[8];
    char pending_name[PH_GATE_TEXT_MAX + 1];
    unsigned (*rng)(void *);
    void *rng_user;
    unsigned rng_state;
};

static bool copy_text(char *dst, const char *src)
{
    size_t n;

    if (!src)
        src = "";
    n = strlen(src);
    if (n > PH_GATE_TEXT_MAX)
        return false;
    memcpy(dst, src, n + 1);
    return true;
}

static void copy_name(char *dst, const char *name)
{
    size_t i, n = 0;

    if (!name)
        name = "";
    for (i = 0; name[i] != '\0' && n < PH_GATE_TEXT_MAX; i++) {
        char c = name[i];
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
        dst[n++] = c;
    }
    dst[n] = '\0';
}

static bool id_ok(const char *id)
{
    size_t n;

    if (!id || id[0] == '\0')
        return false;
    n = strlen(id);
    if (n == 0 || n > PH_GATE_TEXT_MAX)
        return false;
    if (strchr(id, '\t') || strchr(id, '\n') || strchr(id, '\r'))
        return false;
    return true;
}

static bool is_allowed(const ph_gate *gate, const char *id)
{
    size_t i;

    for (i = 0; i < gate->allowed_count; i++) {
        if (strcmp(gate->allowed[i].id, id) == 0)
            return true;
    }
    return false;
}

static bool remember(ph_gate *gate, const char *id, const char *name)
{
    size_t i;
    ph_gate_device *grown;
    size_t cap;

    for (i = 0; i < gate->allowed_count; i++) {
        if (strcmp(gate->allowed[i].id, id) == 0) {
            copy_name(gate->allowed[i].name, name);
            return true;
        }
    }
    if (gate->allowed_count == gate->allowed_cap) {
        cap = gate->allowed_cap ? gate->allowed_cap * 2 : 8;
        grown = realloc(gate->allowed, cap * sizeof *grown);
        if (!grown)
            return false;
        gate->allowed = grown;
        gate->allowed_cap = cap;
    }
    if (!copy_text(gate->allowed[gate->allowed_count].id, id))
        return false;
    copy_name(gate->allowed[gate->allowed_count].name, name);
    gate->allowed_count++;
    return true;
}

static ph_gate_fail *find_fail(ph_gate *gate, const char *id)
{
    size_t i;

    for (i = 0; i < gate->fail_count; i++) {
        if (strcmp(gate->fails[i].id, id) == 0)
            return &gate->fails[i];
    }
    return NULL;
}

static int fail_count_of(const ph_gate *gate, const char *id)
{
    size_t i;

    for (i = 0; i < gate->fail_count; i++) {
        if (strcmp(gate->fails[i].id, id) == 0)
            return gate->fails[i].count;
    }
    return 0;
}

static void clear_fail(ph_gate *gate, const char *id)
{
    size_t i;

    for (i = 0; i < gate->fail_count; i++) {
        if (strcmp(gate->fails[i].id, id) == 0) {
            gate->fails[i] = gate->fails[gate->fail_count - 1];
            gate->fail_count--;
            return;
        }
    }
}

static void bump_fail(ph_gate *gate, const char *id)
{
    ph_gate_fail *slot = find_fail(gate, id);
    ph_gate_fail *grown;
    size_t cap;

    if (slot) {
        if (slot->count < 1000000)
            slot->count++;
        return;
    }
    if (gate->fail_count == gate->fail_cap) {
        cap = gate->fail_cap ? gate->fail_cap * 2 : 8;
        grown = realloc(gate->fails, cap * sizeof *grown);
        if (!grown)
            return;
        gate->fails = grown;
        gate->fail_cap = cap;
    }
    if (!copy_text(gate->fails[gate->fail_count].id, id))
        return;
    gate->fails[gate->fail_count].count = 1;
    gate->fail_count++;
}

static unsigned next_rng(ph_gate *gate)
{
    if (gate->rng)
        return gate->rng(gate->rng_user);
    gate->rng_state = gate->rng_state * 1664525u + 1013904223u;
    return gate->rng_state;
}

static void format_pin(char out[8], unsigned value)
{
    unsigned pin = value % 10000u;

    out[0] = (char)('0' + (pin / 1000u) % 10u);
    out[1] = (char)('0' + (pin / 100u) % 10u);
    out[2] = (char)('0' + (pin / 10u) % 10u);
    out[3] = (char)('0' + pin % 10u);
    out[4] = '\0';
}

static void clear_pending(ph_gate *gate)
{
    gate->has_pending = false;
    gate->pending_id[0] = '\0';
    gate->pending_pin[0] = '\0';
    gate->pending_name[0] = '\0';
}

ph_gate *ph_gate_create(void)
{
    ph_gate *gate = calloc(1, sizeof *gate);

    if (!gate)
        return NULL;
    gate->rng_state = (unsigned)time(NULL);
    if (gate->rng_state == 0)
        gate->rng_state = 0x6d2b79f5u;
    return gate;
}

void ph_gate_destroy(ph_gate *gate)
{
    if (!gate)
        return;
    free(gate->allowed);
    free(gate->fails);
    free(gate);
}

void ph_gate_set_rng(ph_gate *gate, unsigned (*rng)(void *), void *user)
{
    if (!gate)
        return;
    gate->rng = rng;
    gate->rng_user = user;
}

void ph_gate_set_open(ph_gate *gate, bool open_lan)
{
    if (!gate)
        return;
    gate->open_lan = open_lan;
}

ph_gate_decision ph_gate_decide(ph_gate *gate, const char *device_id, const char *name,
                                const char *model)
{
    (void)model;
    if (!gate || !id_ok(device_id))
        return PH_GATE_REJECT;
    if (is_allowed(gate, device_id))
        return PH_GATE_ADMIT;
    if (fail_count_of(gate, device_id) >= PH_GATE_LOCKOUT)
        return PH_GATE_REJECT;
    if (gate->open_lan) {
        if (!remember(gate, device_id, name))
            return PH_GATE_REJECT;
        if (gate->has_pending && strcmp(gate->pending_id, device_id) == 0)
            clear_pending(gate);
        return PH_GATE_ADMIT;
    }
    if (!gate->has_pending || strcmp(gate->pending_id, device_id) != 0) {
        if (!copy_text(gate->pending_id, device_id))
            return PH_GATE_REJECT;
        format_pin(gate->pending_pin, next_rng(gate));
        copy_name(gate->pending_name, name);
        gate->has_pending = true;
    } else if (name) {
        copy_name(gate->pending_name, name);
    }
    return PH_GATE_PIN;
}

const char *ph_gate_pending_pin(const ph_gate *gate)
{
    if (!gate || !gate->has_pending)
        return NULL;
    return gate->pending_pin;
}

const char *ph_gate_pending_id(const ph_gate *gate)
{
    if (!gate || !gate->has_pending)
        return NULL;
    return gate->pending_id;
}

bool ph_gate_submit_pin(ph_gate *gate, const char *device_id, const char *pin)
{
    if (!gate || !id_ok(device_id) || !pin)
        return false;
    if (is_allowed(gate, device_id))
        return true;
    if (fail_count_of(gate, device_id) >= PH_GATE_LOCKOUT)
        return false;
    if (!gate->has_pending || strcmp(gate->pending_id, device_id) != 0 ||
        strcmp(gate->pending_pin, pin) != 0) {
        bump_fail(gate, device_id);
        return false;
    }
    if (!remember(gate, device_id, gate->pending_name))
        return false;
    clear_fail(gate, device_id);
    clear_pending(gate);
    return true;
}

void ph_gate_reset_failures(ph_gate *gate, const char *device_id)
{
    if (!gate)
        return;
    if (!device_id) {
        gate->fail_count = 0;
        return;
    }
    clear_fail(gate, device_id);
}

bool ph_gate_save(ph_gate *gate, const char *path)
{
    FILE *f;
    size_t i;

    if (!gate || !path)
        return false;
    f = fopen(path, "w");
    if (!f)
        return false;
    for (i = 0; i < gate->allowed_count; i++) {
        if (fprintf(f, "%s\t%s\n", gate->allowed[i].id, gate->allowed[i].name) < 0) {
            fclose(f);
            return false;
        }
    }
    return fclose(f) == 0;
}

static void rstrip(char *line)
{
    size_t n = strlen(line);

    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        n--;
        line[n] = '\0';
    }
}

bool ph_gate_load(ph_gate *gate, const char *path)
{
    FILE *f;
    char line[PH_GATE_TEXT_MAX * 2 + 8];

    if (!gate || !path)
        return false;
    f = fopen(path, "r");
    if (!f)
        return false;
    free(gate->allowed);
    gate->allowed = NULL;
    gate->allowed_count = 0;
    gate->allowed_cap = 0;
    clear_pending(gate);
    while (fgets(line, (int)sizeof line, f)) {
        char *tab;

        rstrip(line);
        if (line[0] == '\0')
            continue;
        tab = strchr(line, '\t');
        if (!tab || tab == line)
            continue;
        *tab = '\0';
        if (!id_ok(line))
            continue;
        if (!remember(gate, line, tab + 1)) {
            fclose(f);
            return false;
        }
    }
    return fclose(f) == 0;
}
