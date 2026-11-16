/* Synthetic phone pictures keep cursor recognition regressions independent of
 * personal phone screenshots and Bluetooth hardware. */
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "video/cursor.h"

#define WIDTH 540
#define HEIGHT 1100
#define STRIDE 544

static uint8_t image[STRIDE * HEIGHT];
static uint8_t baseline[STRIDE * HEIGHT];
static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static ph_yuv_planes planes(void)
{
    return (ph_yuv_planes){.y = image, .y_stride = STRIDE, .width = WIDTH, .height = HEIGHT};
}

static void dot(ph_rect screen, double cx, double cy, double radius, int value, double alpha)
{
    for (int y = 0; y < screen.h; y++) {
        for (int x = 0; x < screen.w; x++) {
            double distance = hypot(x - cx, y - cy);
            double coverage = fmax(0, fmin(1, radius + .5 - distance));
            uint8_t *pixel = &image[(screen.y + y) * STRIDE + screen.x + x];
            double blend = coverage * alpha;
            *pixel = (uint8_t)lround(*pixel * (1 - blend) + value * blend);
        }
    }
}

static void located(ph_rect screen, double cx, double cy, double hint_x, double hint_y,
                    double search_radius)
{
    ph_yuv_planes p = planes();
    double x = -1, y = -1;
    CHECK(ph_video_cursor(&p, &screen, hint_x, hint_y, search_radius, &x, &y));
    CHECK(hypot(x - cx, y - cy) <= 2.5);
}

static void mixed_background(ph_rect screen, bool checker)
{
    for (int y = 0; y < screen.h; y++)
        for (int x = 0; x < screen.w; x++) {
            bool bright = checker ? ((x / 8 + y / 8) % 2 != 0) : x >= 250;
            image[(screen.y + y) * STRIDE + screen.x + x] = (uint8_t)(bright ? 235 : 20);
        }
}

int main(void)
{
    ph_rect screen = {20, 10, 500, 1080};
    ph_yuv_planes p = planes();
    double x, y;

    memset(image, 16, sizeof image);
    dot(screen, 250, 700, 11, 90, 1);
    located(screen, 250, 700, 250, 700, 50); /* dark appearance */

    memset(image, 238, sizeof image);
    dot(screen, 295, 645, 11, 145, 1);
    located(screen, 295, 645, 280, 660, 50); /* light appearance, imprecise prediction */

    /* Coordinate mapping uses the cropped picture, never the padded frame. */
    memset(image, 16, sizeof image);
    dot(screen, 17, 17, 11, 90, 1);
    located(screen, 17, 17, 16, 16, 30);
    dot(screen, 475, 1045, 11, 90, 1);
    located(screen, 475, 1045, 490, 1060, 35);

    /* Font-like dark strokes beneath a translucent dot must not pull its centre
     * toward the changed letters instead of the actual pointer. */
    memset(image, 225, sizeof image);
    for (int cy = 600; cy < 730; cy++)
        for (int cx = 180; cx < 340; cx++)
            image[(screen.y + cy) * STRIDE + screen.x + cx] =
                (uint8_t)((cy % 19 < 3 && cx % 13 < 8) ? 65 : 225);
    dot(screen, 250, 667, 11, 90, .75);
    located(screen, 250, 667, 260, 660, 50);

    /* Radius follows phone picture width after a stream resize. */
    screen = (ph_rect){20, 10, 250, 540};
    memset(image, 16, sizeof image);
    dot(screen, 125, 350, 5.5, 110, 1);
    located(screen, 125, 350, 130, 340, 30);

    /* Rotation changes the picture aspect, not the cursor's diameter relative
     * to its shorter edge. */
    screen = (ph_rect){20, 10, 500, 250};
    memset(image, 16, sizeof image);
    dot(screen, 350, 125, 5.5, 110, 1);
    located(screen, 350, 125, 340, 130, 30);

    screen = (ph_rect){20, 10, 500, 1080};
    memset(image, 16, sizeof image);
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, 100, &x, &y));
    /* Crossing straight content edges do not resemble the circular cursor. */
    for (int row = 400; row < 800; row++)
        image[(screen.y + row) * STRIDE + screen.x + 250] = 220;
    for (int col = 100; col < 400; col++)
        image[(screen.y + 600) * STRIDE + screen.x + col] = 220;
    CHECK(!ph_video_cursor(&p, &screen, 250, 600, 70, &x, &y));

    dot(screen, 250, 700, 11, 100, 1);
    CHECK(!ph_video_cursor(&p, &screen, 50, 50, 20, &x, &y)); /* cursor outside search */
    CHECK(!ph_video_cursor(NULL, &screen, 250, 700, 30, &x, &y));
    CHECK(!ph_video_cursor(&p, NULL, 250, 700, 30, &x, &y));
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, 30, NULL, &y));
    CHECK(!ph_video_cursor(&p, &screen, NAN, 700, 30, &x, &y));
    CHECK(!ph_video_cursor(&p, &screen, 250, INFINITY, 30, &x, &y));
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, -1, &x, &y));
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, NAN, &x, &y));
    screen.x = 50;
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, 30, &x, &y));
    screen = (ph_rect){20, 10, 500, 1080};
    p.y_stride = WIDTH - 1;
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, 30, &x, &y));
    p = planes();
    /* Finite values outside the image still need safe bounds checking before
     * converting the search rectangle to integer pixel positions. */
    CHECK(!ph_video_cursor(&p, &screen, DBL_MAX, 700, 0, &x, &y));
    CHECK(!ph_video_cursor(&p, &screen, 250, -DBL_MAX, 0, &x, &y));
    p.width = INT_MIN;
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, 30, &x, &y));
    p = planes();
    p.height = INT_MIN;
    CHECK(!ph_video_cursor(&p, &screen, 250, 700, 30, &x, &y));
    p = planes();

    /* A stationary circular UI element is not proof that the mouse arrived.
     * Only the actual newly moved pointer may authorize a click. */
    memset(image, 238, sizeof image);
    dot(screen, 250, 600, 11, 140, 1);
    memcpy(baseline, image, sizeof image);
    ph_yuv_planes before = planes();
    before.y = baseline;
    CHECK(!ph_video_cursor_changed(&p, &screen, &before, &screen,
                                   250, 600, 100, &x, &y));
    dot(screen, 314, 557, 11, 140, 1);
    CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                  270, 590, 100, &x, &y));
    CHECK(hypot(x - 314, y - 557) <= 2.5);

    /* Feedback also works when a correction overlaps the previous dot. */
    memset(image, 16, sizeof image);
    dot(screen, 250, 700, 11, 90, 1);
    memcpy(baseline, image, sizeof image);
    memset(image, 16, sizeof image);
    dot(screen, 253, 700, 11, 90, 1);
    CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                  253, 700, 40, &x, &y));
    CHECK(hypot(x - 253, y - 700) <= 2.5);

    /* The visible alpha edge can end just inside the nominal 11px radius.
     * Looking only at that outer ring misses the movement even though the
     * pointer's interior changed strongly (seen during a real drag). */
    memset(image, 230, sizeof image);
    memcpy(baseline, image, sizeof image);
    dot(screen, 250, 450, 10.25, 130, .8);
    static const double ring[16][2] = {
        {1, 0}, {.924, .383}, {.707, .707}, {.383, .924}, {0, 1}, {-.383, .924},
        {-.707, .707}, {-.924, .383}, {-1, 0}, {-.924, -.383}, {-.707, -.707},
        {-.383, -.924}, {0, -1}, {.383, -.924}, {.707, -.707}, {.924, -.383},
    };
    for (int i = 0; i < 16; i++) {
        int ix = screen.x + 250 + (int)lround(11 * ring[i][0]);
        int iy = screen.y + 450 + (int)lround(11 * ring[i][1]);
        CHECK(image[iy * STRIDE + ix] == baseline[iy * STRIDE + ix]);
    }
    CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                  250, 450, 40, &x, &y));
    CHECK(hypot(x - 250, y - 450) <= 2.5);

    /* iOS shrinks the dot while the primary button is held. Coarse-search
     * parity must not hide it before the one-pixel refinement can run. */
    for (int half = 0; half < 2; half++) {
        for (int dx = 0; dx < 2; dx++) {
            for (int dy = 0; dy < 2; dy++) {
                memset(image, 230, sizeof image);
                memcpy(baseline, image, sizeof image);
                double cx = 250 + dx, cy = 450 + dy;
                dot(screen, cx, cy, 9 + half * .5, 130, .8);
                CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                              250, 450, 40, &x, &y));
                CHECK(hypot(x - cx, y - cy) <= 2.5);
            }
        }
    }

    /* A translucent dot crossing an icon or text can be darker than half its
     * surroundings and lighter than the other half. Its changed circular mask,
     * rather than a single edge polarity, identifies the newly arrived pointer. */
    for (int checker = 0; checker < 2; checker++) {
        mixed_background(screen, checker != 0);
        memcpy(baseline, image, sizeof image);
        dot(screen, 250, 700, 11, 140, .65);
        CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                      250, 700, 45, &x, &y));
        CHECK(hypot(x - 250, y - 700) <= 2.5);

        /* The same circular change in the opposite direction is the OLD dot
         * disappearing. Restored background texture cannot confirm arrival. */
        memcpy(baseline, image, sizeof image);
        mixed_background(screen, checker != 0);
        CHECK(!ph_video_cursor_changed(&p, &screen, &before, &screen,
                                       250, 700, 45, &x, &y));
    }

    /* With both an old removal and a new arrival in the search window, only
     * the destination pointer can confirm the mouse's updated coordinates. */
    mixed_background(screen, true);
    dot(screen, 220, 700, 11, 140, .65);
    memcpy(baseline, image, sizeof image);
    mixed_background(screen, true);
    dot(screen, 280, 700, 11, 140, .65);
    CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                  260, 700, 80, &x, &y));
    CHECK(hypot(x - 280, y - 700) <= 2.5);

    /* A video transition with two equally convincing moving circles is
     * ambiguous; it must not select whichever happens to be scanned first. */
    memset(image, 16, sizeof image);
    memcpy(baseline, image, sizeof image);
    dot(screen, 220, 700, 11, 90, 1);
    dot(screen, 280, 700, 11, 90, 1);
    CHECK(!ph_video_cursor_changed(&p, &screen, &before, &screen,
                                   250, 700, 80, &x, &y));
    CHECK(!ph_video_cursor_changed(&p, &screen, NULL, &screen,
                                   250, 700, 80, &x, &y));
    CHECK(!ph_video_cursor_changed(&p, &screen, &before, NULL,
                                   250, 700, 80, &x, &y));
    ph_rect mismatch = screen;
    mismatch.w--;
    CHECK(!ph_video_cursor_changed(&p, &screen, &before, &mismatch,
                                   250, 700, 80, &x, &y));

    /* Even valid but clipped hints at picture edges must stay inside the luma
     * allocation during coarse search and subpixel-radius refinement. */
    for (int width = 80; width <= 500; width += 70) {
        ph_rect small = {0, 0, width, width};
        memset(image, 16, sizeof image);
        dot(small, width / 2.0, width / 2.0, width * 11.0 / 500, 110, 1);
        for (int edge = 0; edge < 2; edge++) {
            ph_video_cursor(&p, &small, edge ? width - 1 : 0,
                            edge ? width - 1 : 0, width, &x, &y);
        }
    }

    if (failures)
        return 1;
    puts("all cursor recognition checks passed");
    return 0;
}
