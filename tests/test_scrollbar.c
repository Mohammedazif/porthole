#include <stdio.h>
#include <string.h>

#include "video/scrollbar.h"

static uint8_t image[544 * 1100];
static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void indicator(int x, int first, int last, uint8_t value)
{
    for (int y = first; y <= last; y++)
        for (int dx = 0; dx < 3; dx++)
            image[y * 544 + x + dx] = value;
}

int main(void)
{
    /* A cropped phone inside a larger padded frame, with the native indicator
     * positions measured in the live Settings reproduction. */
    ph_yuv_planes p = {.y = image, .y_stride = 544, .width = 540, .height = 1100};
    ph_rect screen = {20, 10, 500, 1080}, bar;
    memset(image, 16, sizeof image);
    CHECK(!ph_video_scrollbar(&p, &screen, &bar));
    indicator(20 + 493, 10 + 121, 10 + 451, 110);
    /* A longer stripe inside content must not be mistaken for a scrollbar. */
    indicator(300, 120, 1000, 200);
    CHECK(ph_video_scrollbar(&p, &screen, &bar));
    CHECK(bar.y == 121 && bar.h == 331 && bar.x >= 493 && bar.x <= 495);

    memset(image, 230, sizeof image); /* light appearance: dark indicator */
    indicator(20 + 493, 10 + 684, 10 + 1014, 100);
    CHECK(ph_video_scrollbar(&p, &screen, &bar));
    CHECK(bar.y == 684 && bar.h == 331);

    memset(image, 16, sizeof image);
    indicator(20 + 493, 10 + 1006, 10 + 1014, 110); /* compressed past bottom */
    CHECK(ph_video_scrollbar(&p, &screen, &bar));
    CHECK(bar.y == 1006 && bar.h == 9);
    memset(image, 16, sizeof image);
    indicator(20 + 493, 10 + 121, 10 + 129, 110); /* compressed past top */
    CHECK(ph_video_scrollbar(&p, &screen, &bar));
    CHECK(bar.y == 121 && bar.h == 9);
    memset(image, 16, sizeof image);
    indicator(20 + 493, 10 + 121, 10 + 451, 25); /* faded: no confident feedback */
    CHECK(!ph_video_scrollbar(&p, &screen, &bar));
    screen.x = 50; /* rectangle would extend outside the decoded image */
    CHECK(!ph_video_scrollbar(&p, &screen, &bar));
    CHECK(!ph_video_scrollbar(NULL, &screen, &bar));
    return failures ? 1 : 0;
}
