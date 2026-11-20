#include <stdio.h>
#include <string.h>

#include "files/clip.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

int main(void)
{
    char kind = 0;
    size_t size = 0;

    CHECK(ph_clip_header("CLIP T 1", &kind, &size));
    CHECK(kind == 'T' && size == 1);
    CHECK(ph_clip_header("CLIP P 8", &kind, &size));
    CHECK(kind == 'P' && size == 8);
    CHECK(ph_clip_header("CLIP T 262144", &kind, &size) && size == PH_CLIP_TEXT_MAX);
    CHECK(!ph_clip_header("CLIP T 262145", &kind, &size));
    CHECK(ph_clip_header("CLIP P 8388608", &kind, &size) && size == PH_CLIP_IMAGE_MAX);
    CHECK(!ph_clip_header("CLIP P 8388609", &kind, &size));
    CHECK(!ph_clip_header("CLIP T 0", &kind, &size));
    CHECK(!ph_clip_header("CLIP X 1", &kind, &size));
    CHECK(!ph_clip_header("CLIP T", &kind, &size));
    CHECK(!ph_clip_header("CLIP T 5 ", &kind, &size));
    CHECK(!ph_clip_header("CLIP T 5 extra", &kind, &size));
    CHECK(!ph_clip_header("FILE 1 a.jpg", &kind, &size));
    CHECK(!ph_clip_header("SYNC", &kind, &size));
    CHECK(!ph_clip_header("", &kind, &size));
    CHECK(!ph_clip_header(NULL, &kind, &size));
    CHECK(!ph_clip_header("CLIP T 1", NULL, &size));
    CHECK(!ph_clip_header("CLIP T 1", &kind, NULL));
    return g_failures ? 1 : 0;
}
