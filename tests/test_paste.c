#include <stdio.h>
#include <string.h>

#include "control/paste.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static int shift_only(uint8_t modifiers)
{
    return (modifiers & (uint8_t)~PH_HID_MOD_LSHIFT) == 0;
}

int main(void)
{
    ph_paste_event ev[4];
    const char non_ascii[] = {'A', (char)0xC3, (char)0xA9, 'b', '\0'};
    const char only_high[] = {(char)0xFF, '\0'};

    memset(ev, 0xFF, sizeof ev);
    CHECK(ph_paste_encode("Hi", ev, 4) == 2);
    CHECK(ev[0].usage == 0x0B && ev[0].modifiers == PH_HID_MOD_LSHIFT);
    CHECK(ev[1].usage == 0x0C && ev[1].modifiers == 0);
    CHECK(shift_only(ev[0].modifiers) && shift_only(ev[1].modifiers));

    CHECK(ph_paste_encode("A\n", ev, 4) == 2);
    CHECK(ev[0].usage == 0x04 && ev[0].modifiers == PH_HID_MOD_LSHIFT);
    CHECK(ev[1].usage == 0x28 && ev[1].modifiers == 0);

    CHECK(ph_paste_encode("1!", ev, 4) == 2);
    CHECK(ev[0].usage == 0x1E && ev[0].modifiers == 0);
    CHECK(ev[1].usage == 0x1E && ev[1].modifiers == PH_HID_MOD_LSHIFT);
    CHECK(shift_only(ev[1].modifiers));

    CHECK(ph_paste_encode("\t", ev, 1) == 1);
    CHECK(ev[0].usage == 0x2B && ev[0].modifiers == 0);

    CHECK(ph_paste_encode("", ev, 4) == 0);
    CHECK(ph_paste_encode(NULL, ev, 4) == 0);

    memset(ev, 0xFF, sizeof ev);
    CHECK(ph_paste_encode("Hi", ev, 1) == 1);
    CHECK(ev[0].usage == 0x0B && ev[0].modifiers == PH_HID_MOD_LSHIFT);
    CHECK(ev[1].usage == 0xFF && ev[1].modifiers == 0xFF);
    CHECK(ph_paste_encode("Hi", ev, 0) == 0);
    CHECK(ph_paste_encode("Hi", NULL, 4) == 0);

    CHECK(ph_paste_encode(non_ascii, ev, 4) == 2);
    CHECK(ev[0].usage == 0x04 && ev[0].modifiers == PH_HID_MOD_LSHIFT);
    CHECK(ev[1].usage == 0x05 && ev[1].modifiers == 0);
    CHECK(ph_paste_encode(only_high, ev, 4) == 0);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all paste checks passed\n");
    return 0;
}
