#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "security/gate.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static unsigned seq_rng(void *user)
{
    unsigned *n = user;
    unsigned cur = *n;

    *n = cur + 1;
    return cur;
}

static void copy_pin(char *dst, size_t len, const char *pin)
{
    size_t i = 0;

    if (!pin)
        pin = "";
    while (pin[i] != '\0' && i + 1 < len) {
        dst[i] = pin[i];
        i++;
    }
    dst[i] = '\0';
}

static void temp_path(char *out, size_t len, const char *name)
{
    const char *dir = getenv("TMPDIR");

    if (!dir || !dir[0])
        dir = getenv("TEMP");
    if (!dir || !dir[0])
        dir = "/tmp";
    snprintf(out, len, "%s/porthole-%s-%ld", dir, name, (long)getpid());
}

static void expect_line(const char *path, const char *line)
{
    FILE *f = fopen(path, "r");
    char buf[256];

    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fgets(buf, (int)sizeof buf, f) != NULL);
    CHECK(strcmp(buf, line) == 0);
    fclose(f);
}

int main(void)
{
    unsigned rng = 7;
    ph_gate *gate;
    ph_gate *loaded;
    char pin[8];
    char path[256];
    char path_blank[256];
    int i;
    FILE *crafted;

    gate = ph_gate_create();
    CHECK(gate != NULL);
    ph_gate_set_rng(gate, seq_rng, &rng);

    CHECK(ph_gate_decide(gate, NULL, "n", "m") == PH_GATE_REJECT);
    CHECK(ph_gate_decide(gate, "", "n", "m") == PH_GATE_REJECT);

    CHECK(ph_gate_decide(gate, "dev-a", "Ada", "iPhone") == PH_GATE_PIN);
    CHECK(ph_gate_pending_pin(gate) != NULL);
    CHECK(strcmp(ph_gate_pending_pin(gate), "0007") == 0);
    CHECK(strcmp(ph_gate_pending_id(gate), "dev-a") == 0);
    CHECK(rng == 8);
    CHECK(ph_gate_decide(gate, "dev-a", "Ada", "iPhone") == PH_GATE_PIN);
    CHECK(strcmp(ph_gate_pending_pin(gate), "0007") == 0);
    CHECK(rng == 8);

    CHECK(ph_gate_decide(gate, "dev-b", "Bea", "iPhone") == PH_GATE_PIN);
    CHECK(strcmp(ph_gate_pending_id(gate), "dev-b") == 0);
    CHECK(strcmp(ph_gate_pending_pin(gate), "0008") == 0);
    copy_pin(pin, sizeof pin, ph_gate_pending_pin(gate));
    CHECK(!ph_gate_submit_pin(gate, "dev-a", "0007"));
    CHECK(!ph_gate_submit_pin(gate, "dev-b", "0000"));
    CHECK(ph_gate_decide(gate, "dev-b", "Bea", "iPhone") == PH_GATE_PIN);
    CHECK(strcmp(ph_gate_pending_pin(gate), pin) == 0);
    CHECK(ph_gate_submit_pin(gate, "dev-b", pin));
    CHECK(ph_gate_pending_pin(gate) == NULL);
    CHECK(ph_gate_decide(gate, "dev-b", "Bea", "iPhone") == PH_GATE_ADMIT);
    ph_gate_destroy(gate);

    rng = 100;
    gate = ph_gate_create();
    CHECK(gate != NULL);
    ph_gate_set_rng(gate, seq_rng, &rng);
    CHECK(ph_gate_decide(gate, "bad", "N", "M") == PH_GATE_PIN);
    copy_pin(pin, sizeof pin, ph_gate_pending_pin(gate));
    for (i = 0; i < 4; i++)
        CHECK(!ph_gate_submit_pin(gate, "bad", "9999"));
    CHECK(ph_gate_decide(gate, "bad", "N", "M") == PH_GATE_PIN);
    CHECK(!ph_gate_submit_pin(gate, "bad", "9999"));
    CHECK(ph_gate_decide(gate, "bad", "N", "M") == PH_GATE_REJECT);
    CHECK(!ph_gate_submit_pin(gate, "bad", pin));
    CHECK(ph_gate_decide(gate, "other", "O", "M") == PH_GATE_PIN);
    ph_gate_reset_failures(gate, "bad");
    CHECK(ph_gate_decide(gate, "bad", "N", "M") == PH_GATE_PIN);
    copy_pin(pin, sizeof pin, ph_gate_pending_pin(gate));
    CHECK(ph_gate_submit_pin(gate, "bad", pin));
    CHECK(ph_gate_decide(gate, "bad", "N", "M") == PH_GATE_ADMIT);
    ph_gate_destroy(gate);

    temp_path(path, sizeof path, "gate");
    temp_path(path_blank, sizeof path_blank, "gate-blank");
    remove(path);
    remove(path_blank);

    gate = ph_gate_create();
    CHECK(gate != NULL);
    ph_gate_set_open(gate, true);
    CHECK(ph_gate_decide(gate, "", "n", "m") == PH_GATE_REJECT);
    CHECK(ph_gate_decide(gate, NULL, "n", "m") == PH_GATE_REJECT);
    CHECK(ph_gate_decide(gate, "dev-1", "Ada Lovelace", "iPhone") == PH_GATE_ADMIT);
    CHECK(ph_gate_pending_pin(gate) == NULL);
    CHECK(ph_gate_save(gate, path));
    expect_line(path, "dev-1\tAda Lovelace\n");
    ph_gate_set_open(gate, false);
    CHECK(ph_gate_decide(gate, "dev-1", "other", "m") == PH_GATE_ADMIT);

    loaded = ph_gate_create();
    CHECK(loaded != NULL);
    CHECK(!ph_gate_load(loaded, "/no/such/porthole-gate-file"));
    CHECK(ph_gate_load(loaded, path));
    CHECK(ph_gate_decide(loaded, "dev-1", "x", "y") == PH_GATE_ADMIT);
    CHECK(ph_gate_decide(loaded, "dev-2", "x", "y") == PH_GATE_PIN);
    ph_gate_destroy(loaded);
    ph_gate_destroy(gate);

    crafted = fopen(path_blank, "w");
    CHECK(crafted != NULL);
    if (crafted) {
        fputs("\n", crafted);
        fputs("id-1\tOne\n", crafted);
        fputs("\n", crafted);
        fputs("id-2\tTwo\n", crafted);
        fclose(crafted);
    }
    loaded = ph_gate_create();
    CHECK(loaded != NULL);
    CHECK(ph_gate_load(loaded, path_blank));
    CHECK(ph_gate_decide(loaded, "id-1", "x", "y") == PH_GATE_ADMIT);
    CHECK(ph_gate_decide(loaded, "id-2", "x", "y") == PH_GATE_ADMIT);
    CHECK(ph_gate_decide(loaded, "id-3", "x", "y") == PH_GATE_PIN);
    ph_gate_destroy(loaded);

    remove(path);
    remove(path_blank);
    ph_gate_destroy(NULL);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all gate checks passed\n");
    return 0;
}
