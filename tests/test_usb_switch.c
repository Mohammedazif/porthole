#include "usb/switch.h"

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

static void test_wifi_until_usb_key(void)
{
    ph_src_switch sw;
    ph_src_switch_init(&sw);
    CHECK(sw.current == PH_SRC_NONE);
    CHECK(PH_SRC_USB_FALLBACK_MS == 500);

    CHECK(ph_src_switch_wifi_frame(&sw, false, 0) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&sw, false, 10) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&sw, true, 20) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_frame(&sw, false, 30) == PH_SRC_USB);
    CHECK(ph_src_switch_wifi_frame(&sw, true, 40) == PH_SRC_USB);
}

static void test_non_key_from_none(void)
{
    ph_src_switch sw;
    ph_src_switch_init(&sw);
    CHECK(ph_src_switch_usb_frame(&sw, false, 0) == PH_SRC_NONE);
    CHECK(ph_src_switch_usb_frame(&sw, true, 1) == PH_SRC_USB);
}

static void test_fallback_boundary(void)
{
    ph_src_switch at_500;
    ph_src_switch past;
    ph_src_switch none;
    ph_src_switch recorded_while_usb;

    ph_src_switch_init(&at_500);
    CHECK(ph_src_switch_wifi_frame(&at_500, false, 1000) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&at_500, true, 1000) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_unplugged(&at_500, 1500) == PH_SRC_WIFI);

    ph_src_switch_init(&past);
    CHECK(ph_src_switch_wifi_frame(&past, true, 1000) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&past, true, 1100) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_unplugged(&past, 1501) == PH_SRC_NONE);
    CHECK(ph_src_switch_wifi_frame(&past, false, 1600) == PH_SRC_WIFI);

    ph_src_switch_init(&none);
    CHECK(ph_src_switch_usb_frame(&none, true, 0) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_unplugged(&none, 0) == PH_SRC_NONE);

    ph_src_switch_init(&recorded_while_usb);
    CHECK(ph_src_switch_wifi_frame(&recorded_while_usb, false, 0) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&recorded_while_usb, true, 0) == PH_SRC_USB);
    CHECK(ph_src_switch_wifi_frame(&recorded_while_usb, false, 1000) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_unplugged(&recorded_while_usb, 1500) == PH_SRC_WIFI);
}

static void test_unplugged_ignores_usb(void)
{
    ph_src_switch sw;
    ph_src_switch_init(&sw);
    CHECK(ph_src_switch_wifi_frame(&sw, false, 10) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&sw, true, 20) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_unplugged(&sw, 2000) == PH_SRC_NONE);
    CHECK(ph_src_switch_usb_frame(&sw, true, 2010) == PH_SRC_NONE);
    CHECK(ph_src_switch_usb_frame(&sw, false, 2020) == PH_SRC_NONE);
    CHECK(ph_src_switch_wifi_frame(&sw, false, 2030) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&sw, true, 2040) == PH_SRC_WIFI);

    ph_src_switch_init(&sw);
    CHECK(ph_src_switch_usb_frame(&sw, true, 1) == PH_SRC_USB);
}

static void test_unplug_while_not_usb(void)
{
    ph_src_switch wifi;
    ph_src_switch none;

    ph_src_switch_init(&wifi);
    CHECK(ph_src_switch_wifi_frame(&wifi, false, 5) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_unplugged(&wifi, 5) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&wifi, true, 6) == PH_SRC_WIFI);

    ph_src_switch_init(&none);
    CHECK(ph_src_switch_usb_unplugged(&none, 1) == PH_SRC_NONE);
    CHECK(ph_src_switch_usb_frame(&none, true, 2) == PH_SRC_NONE);
}

static void test_wifi_lost(void)
{
    ph_src_switch sw;
    ph_src_switch_init(&sw);
    CHECK(ph_src_switch_wifi_frame(&sw, false, 100) == PH_SRC_WIFI);
    CHECK(ph_src_switch_wifi_lost(&sw, 200) == PH_SRC_NONE);
    CHECK(ph_src_switch_wifi_frame(&sw, false, 300) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&sw, true, 300) == PH_SRC_USB);
    CHECK(ph_src_switch_wifi_frame(&sw, false, 400) == PH_SRC_USB);
    CHECK(ph_src_switch_wifi_lost(&sw, 450) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_unplugged(&sw, 450) == PH_SRC_NONE);

    ph_src_switch_init(&sw);
    CHECK(ph_src_switch_wifi_lost(&sw, 0) == PH_SRC_NONE);
}

static void test_exact_zero_timestamp(void)
{
    ph_src_switch sw;
    ph_src_switch_init(&sw);
    CHECK(ph_src_switch_wifi_frame(&sw, false, 0) == PH_SRC_WIFI);
    CHECK(ph_src_switch_usb_frame(&sw, true, 0) == PH_SRC_USB);
    CHECK(ph_src_switch_usb_unplugged(&sw, 500) == PH_SRC_WIFI);
}

int main(void)
{
    test_wifi_until_usb_key();
    test_non_key_from_none();
    test_fallback_boundary();
    test_unplugged_ignores_usb();
    test_unplug_while_not_usb();
    test_wifi_lost();
    test_exact_zero_timestamp();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all usb switch checks passed\n");
    return 0;
}
