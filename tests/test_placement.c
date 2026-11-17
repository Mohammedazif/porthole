#include <stdio.h>
#include <string.h>

#include "ui/placement.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static void check_round_trip(const ph_placement *placement)
{
    char line[128];
    char again[128];
    ph_placement parsed;

    CHECK(ph_placement_format(placement, line, sizeof line));
    CHECK(ph_placement_parse(line, &parsed));
    CHECK(parsed.valid);
    CHECK(parsed.x == placement->x && parsed.y == placement->y);
    CHECK(parsed.w == placement->w && parsed.h == placement->h);
    CHECK(parsed.fullscreen == placement->fullscreen);
    CHECK(parsed.always_on_top == placement->always_on_top);
    CHECK(parsed.one_to_one == placement->one_to_one);
    CHECK(ph_placement_format(&parsed, again, sizeof again));
    CHECK(strcmp(line, again) == 0);
}

static void check_fit(int video_w, int video_h, int screen_w, int screen_h, int expect_w,
                      int expect_h)
{
    int w = 0, h = 0;

    ph_chrome_one_to_one(video_w, video_h, screen_w, screen_h, &w, &h);
    CHECK(w == expect_w && h == expect_h);
    CHECK(w > 0 && h > 0);
    if (screen_w > 0 && screen_h > 0)
        CHECK(w <= screen_w && h <= screen_h);
}

int main(void)
{
    ph_placement placement;
    char line[128];
    int flags;

    for (flags = 0; flags < 8; flags++) {
        ph_placement sample;

        memset(&sample, 0, sizeof sample);
        sample.x = -40 + flags;
        sample.y = 15;
        sample.w = 800;
        sample.h = 600;
        sample.fullscreen = (flags & 1) != 0;
        sample.always_on_top = (flags & 2) != 0;
        sample.one_to_one = (flags & 4) != 0;
        sample.valid = true;
        check_round_trip(&sample);
    }

    memset(&placement, 0, sizeof placement);
    placement.valid = true;
    CHECK(!ph_placement_format(&placement, line, 4));
    CHECK(!ph_placement_format(NULL, line, sizeof line));
    placement.valid = false;
    CHECK(!ph_placement_format(&placement, line, sizeof line));

    placement.valid = true;
    CHECK(!ph_placement_parse("", &placement));
    CHECK(!placement.valid);
    CHECK(!ph_placement_parse(NULL, &placement));
    CHECK(!placement.valid);
    CHECK(!ph_placement_parse("   ", &placement));
    CHECK(!ph_placement_parse("nope", &placement));
    CHECK(!ph_placement_parse("1 2 3 4 1 1", &placement));
    CHECK(!ph_placement_parse("1 2 3 4 1 1 1 trailing", &placement));
    CHECK(!ph_placement_parse("1 2 0 4 0 0 0", &placement));
    CHECK(!ph_placement_parse("1 2 3 4 2 0 0", &placement));
    CHECK(!ph_placement_parse("1 2 -3 4 0 0 0", &placement));
    CHECK(!placement.valid);

    check_fit(1920, 1080, 2560, 1440, 1920, 1080);
    check_fit(1920, 1080, 1280, 720, 1280, 720);
    check_fit(200, 100, 100, 100, 100, 50);
    check_fit(200, 100, 99, 100, 99, 50);
    check_fit(100, 200, 100, 80, 40, 80);
    check_fit(1919, 1080, 1366, 768, 1365, 768);
    check_fit(10000, 1, 5, 5, 5, 1);
    check_fit(1, 10000, 5, 5, 1, 5);
    check_fit(0, 10, 100, 100, 1, 1);
    check_fit(10, 10, 0, 0, 1, 1);
    ph_chrome_one_to_one(10, 10, 10, 10, NULL, NULL);

    {
        ph_window_insets frame = {32, 8, 8, 8};
        int w = 0, h = 0, x = 0, y = 0;

        /* 1920x1080 desktop. Portrait is limited by height, landscape by width. */
        ph_chrome_fit_client(1170, 2532, 1920, 1080, &frame, &w, &h);
        CHECK(w == 433 && h == 936);
        ph_chrome_place_client(w, h, 0, 0, 1920, 1080, &frame, 0, 0, 0, 0, true, &x, &y);
        CHECK(x == 743 && y == 84);
        CHECK(x >= frame.left && y >= frame.top);
        CHECK(x + w + frame.right <= 1920 && y + h + frame.bottom <= 1080);

        ph_chrome_fit_client(2532, 1170, 1920, 1080, &frame, &w, &h);
        CHECK(w == 1713 && h == 792);
        ph_chrome_place_client(w, h, 0, 0, 1920, 1080, &frame, 0, 0, 0, 0, true, &x, &y);
        CHECK(x == 103 && y == 156);
        CHECK(x + w + frame.right <= 1920 && y + h + frame.bottom <= 1080);

        /* A wide window parked past the right edge comes back on screen, center kept. */
        ph_chrome_place_client(800, 400, 0, 0, 1920, 1080, NULL, 1700, 100, 800, 400, false, &x,
                               &y);
        CHECK(x == 1120 && y == 100);

        ph_chrome_fit_client(1170, 2532, 10, 10, &frame, &w, &h);
        CHECK(w >= 1 && h >= 1);
        ph_chrome_place_client(w, h, 40, 50, 10, 10, &frame, 0, 0, 0, 0, true, &x, &y);
        CHECK(x == 40 + frame.left && y == 50 + frame.top);
        ph_chrome_fit_client(100, 200, 800, 600, NULL, NULL, NULL);
        ph_chrome_place_client(100, 200, 0, 0, 800, 600, NULL, 0, 0, 0, 0, true, NULL, NULL);
    }

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all placement checks passed\n");
    return 0;
}
