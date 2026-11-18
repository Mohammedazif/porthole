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
static uint8_t reference_image[STRIDE * HEIGHT];
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

static void title_background(ph_rect screen)
{
    static const char title[7][18] = {
        "00100001000000000", "00100001000000000", "11111001011001110",
        "00100001100110001", "00100001000110001", "00100001000110001",
        "00011001000101110",
    };
    memset(image, 0, sizeof image);
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 17; col++)
            if (title[row][col] == '1')
                for (int dy = 0; dy < 3; dy++)
                    for (int dx = 0; dx < 3; dx++)
                        image[(screen.y + 125 + row * 3 + dy) * STRIDE
                              + screen.x + 75 + col * 3 + dx] = 215;
}

/* A moving texture plus a moving dot-shaped distractor, standing in for a
 * playing video under the AssistiveTouch dot. */
static void video_frame(ph_rect screen, int phase)
{
    for (int y = 0; y < screen.h; y++)
        for (int x = 0; x < screen.w; x++)
            image[(screen.y + y) * STRIDE + screen.x + x] = (uint8_t)lround(
                60 + 25 * sin((x + phase) / 23.0) * cos((y - phase) / 29.0));
    dot(screen, 318 + phase * 14, 545 + phase * 11, 11, 230, .9);
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

    /* Bright title text on black already contains circular edges. A light
     * translucent hover dot over the letters must not authorize one of those
     * unchanged edges as its centre (observed in the native Notes title). */
    title_background(screen);
    memcpy(baseline, image, sizeof image);
    dot(screen, 109, 138, 11, 255, .2);
    CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                  100.5, 150.5, 40, &x, &y));
    CHECK(hypot(x - 109, y - 138) <= 2.5);
    /* Unchanged text, and the old dot disappearing, cannot confirm arrival. */
    memcpy(image, baseline, sizeof image);
    CHECK(!ph_video_cursor_changed(&p, &screen, &before, &screen,
                                   100.5, 150.5, 40, &x, &y));
    dot(screen, 109, 138, 11, 255, .2);
    memcpy(baseline, image, sizeof image);
    title_background(screen);
    CHECK(!ph_video_cursor_changed(&p, &screen, &before, &screen,
                                   100.5, 150.5, 40, &x, &y));

    title_background(screen);
    memcpy(baseline, image, sizeof image);
    dot(screen, 103, 142, 11, 255, .2);
    CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                  100.5, 150.5, 40, &x, &y));
    CHECK(hypot(x - 103, y - 142) <= 2.5);

    /* Small corrections and press contraction can leave most of the old dot
     * visible. If change evidence is insufficient, fail safely; a nearby text
     * ring must never replace the actual new centre. */
    for (int press = 0; press < 2; press++) {
        for (int distance = 1; distance <= 4; distance++) {
            for (int axis = 0; axis < 2; axis++) {
                title_background(screen);
                dot(screen, 103, 142, 11, 255, .2);
                memcpy(baseline, image, sizeof image);
                title_background(screen);
                double cx = 103 + (axis == 0 ? distance : 0);
                double cy = 142 + (axis == 1 ? distance : 0);
                dot(screen, cx, cy, press ? 9 : 11, 255, .2);
                if (ph_video_cursor_changed(&p, &screen, &before, &screen,
                                            cx, cy, 40, &x, &y))
                    CHECK(hypot(x - cx, y - cy) <= 2.5);
            }
        }
    }

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

    /* Every search-grid parity must see the changed circle over text. Its
     * strongest centre can be a single pixel that a two-pixel scan skips. */
    title_background(screen);
    memcpy(baseline, image, sizeof image);
    dot(screen, 84, 131, 11, 255, .2);
    for (int hx = 84; hx <= 85; hx++) {
        for (int hy = 131; hy <= 132; hy++) {
            CHECK(ph_video_cursor_changed(&p, &screen, &before, &screen,
                                          hx, hy, 40, &x, &y));
            CHECK(hypot(x - 84, y - 131) <= 2.5);
        }
    }

    /* A short correction leaves most of the old dot in the dispatch baseline.
     * A recent background reference can still locate the complete destination
     * circle, provided the nearby scene and the new movement both check out. */
    ph_yuv_planes reference = planes();
    reference.y = reference_image;
    title_background(screen);
    memcpy(reference_image, image, sizeof image);
    dot(screen, 109, 142, 11, 255, .2);
    memcpy(baseline, image, sizeof image);
    title_background(screen);
    dot(screen, 106, 142, 11, 255, .2);
    CHECK(ph_video_cursor_referenced(&p, &screen, &before, &screen, &reference, &screen,
                                     109, 142, 106, 142, 40, &x, &y));
    CHECK(hypot(x - 106, y - 142) <= 2.5);

    memcpy(image, baseline, sizeof image);
    CHECK(!ph_video_cursor_referenced(&p, &screen, &before, &screen, &reference, &screen,
                                      109, 142, 106, 142, 40, &x, &y)); /* unchanged */
    title_background(screen);
    CHECK(!ph_video_cursor_referenced(&p, &screen, &before, &screen, &reference, &screen,
                                      109, 142, 106, 142, 40, &x, &y)); /* disappeared */

    title_background(screen);
    dot(screen, 109, 142, 11, 255, .2);
    for (int cy = 148; cy < 152; cy++)
        for (int cx = 124; cx < 128; cx++)
            image[(screen.y + cy) * STRIDE + screen.x + cx] = 100;
    memcpy(baseline, image, sizeof image);
    title_background(screen);
    dot(screen, 106, 142, 11, 255, .2);
    for (int cy = 148; cy < 152; cy++)
        for (int cx = 124; cx < 128; cx++)
            image[(screen.y + cy) * STRIDE + screen.x + cx] = 100;
    CHECK(!ph_video_cursor_referenced(&p, &screen, &before, &screen, &reference, &screen,
                                      109, 142, 106, 142, 40, &x, &y)); /* changed scene */
    CHECK(!ph_video_cursor_referenced(&p, &screen, &before, &screen, NULL, &screen,
                                      109, 142, 106, 142, 40, &x, &y));
    CHECK(!ph_video_cursor_referenced(&p, &screen, &before, &screen, &reference, &screen,
                                      NAN, 142, 106, 142, 40, &x, &y));

    /* Playing video: the stopped dot is found once motion between two pictures
     * taken after the movement is masked out; the changed search alone may pick
     * the moving distractor or reject the result as ambiguous. */
    {
        static uint8_t later_image[STRIDE * HEIGHT];
        ph_yuv_planes current = planes(), later = planes(), before_video = planes();
        later.y = later_image;
        before_video.y = baseline;
        video_frame(screen, 0);
        dot(screen, 120, 300, 11, 113, .62);
        memcpy(baseline, image, sizeof image);
        video_frame(screen, 2);
        dot(screen, 300, 520, 11, 113, .62);
        memcpy(later_image, image, sizeof image);
        video_frame(screen, 1);
        dot(screen, 300, 520, 11, 113, .62);
        {
            double cx = -1, cy = -1;
            bool plain = ph_video_cursor_changed(&current, &screen, &before_video, &screen,
                                                 306, 528, 200, &cx, &cy);
            /* The bright moving distractor wins or makes the plain search ambiguous. */
            CHECK(!plain || hypot(cx - 300, cy - 520) > 4);
        }
        for (int dilate = 1; dilate <= 2; dilate++) {
            x = y = -1;
            CHECK(ph_video_cursor_settled(&current, &screen, &later, NULL, &before_video, &screen,
                                          dilate, 306, 528, 200, &x, &y));
            CHECK(hypot(x - 300, y - 520) <= 2.5);
        }
        /* A dot still moving between the two later pictures is not settled. */
        video_frame(screen, 2);
        dot(screen, 318, 520, 11, 113, .62);
        memcpy(later_image, image, sizeof image);
        video_frame(screen, 1);
        dot(screen, 300, 520, 11, 113, .62);
        x = y = -1;
        bool moving = ph_video_cursor_settled(&current, &screen, &later, NULL, &before_video,
                                              &screen, 2, 306, 528, 200, &x, &y);
        CHECK(!moving || hypot(x - 300, y - 520) > 4);
        /* A still screen (identical later picture) behaves like the changed search. */
        title_background(screen);
        memcpy(baseline, image, sizeof image);
        dot(screen, 84, 131, 11, 255, .2);
        memcpy(later_image, image, sizeof image);
        double cx = -1, cy = -1;
        CHECK(ph_video_cursor_changed(&current, &screen, &before_video, &screen, 85, 131, 40,
                                      &cx, &cy));
        x = y = -1;
        CHECK(ph_video_cursor_settled(&current, &screen, &later, NULL, &before_video, &screen,
                                      2, 85, 131, 40, &x, &y));
        CHECK(x == cx && y == cy);
        CHECK(!ph_video_cursor_settled(NULL, &screen, &later, NULL, &before_video, &screen,
                                       2, 85, 131, 40, &x, &y));
        CHECK(!ph_video_cursor_settled(&current, &screen, &later, NULL, &before_video, &screen,
                                       9, 85, 131, 40, &x, &y));
    }

    if (failures)
        return 1;
    puts("all cursor recognition checks passed");
    return 0;
}
