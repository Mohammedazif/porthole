#include <stdio.h>
#include <string.h>

#include "files/picture_hash.h"
#include "files/inbox.h"
#include "files/pull.h"
#include "broadcast/screen.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static void test_hash(void)
{
    uint8_t px[8][9];
    uint64_t hashes[3];
    double aspects[3];
    int r, c;

    memset(px, 10, sizeof px);
    px[0][0] = 20;
    CHECK(ph_picture_dhash(&px[0][0], 9, 8, 9) == ((uint64_t)1 << 63));

    for (c = 0; c < 9; c++)
        px[0][c] = (uint8_t)(90 - c * 10);
    CHECK(ph_picture_dhash(&px[0][0], 9, 8, 9) == 0xFF00000000000000ull);

    memset(px, 40, sizeof px);
    CHECK(ph_picture_dhash(&px[0][0], 9, 8, 9) == 0);
    CHECK(ph_picture_hamming(0xF0, 0x0F) == 8);
    (void)r;

    hashes[0] = 0;
    hashes[1] = 0xFF;
    hashes[2] = 0xFFFF;
    aspects[0] = 0.5;
    aspects[1] = 0.5;
    aspects[2] = 2.0;
    CHECK(ph_picture_pick(0, 0.5, hashes, aspects, 3) == 0);
    CHECK(ph_picture_pick(0x7FF, 0.5, hashes, aspects, 1) == -1);
    hashes[1] = 0x0F;
    CHECK(ph_picture_pick(0, 0.5, hashes, aspects, 2) == -1);
    CHECK(ph_picture_pick(0, 0.5, hashes, NULL, 1) == 0);
}

static void test_thumb(void)
{
    static uint8_t px[64 * 48];
    uint8_t out[PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    int x, y;

    /* 48 wide, 64 tall: each cell is 1.5 columns by 2 rows; integer bounds give
     * columns of width 1 and 2 alternating, exactly as the phone samples. */
    for (y = 0; y < 64; y++)
        for (x = 0; x < 48; x++)
            px[y * 48 + x] = (uint8_t)(y < 32 ? 200 : 20);
    CHECK(ph_picture_thumb(px, 48, 64, 48, out) == 1);
    CHECK(out[0] == 200 && out[15 * 32 + 31] == 200);
    CHECK(out[16 * 32] == 20 && out[31 * 32 + 31] == 20);

    px[0] = 0;
    px[1] = 100; /* cell 0 covers column 0 only; cell 1 covers column 1 */
    px[48] = 0;
    px[49] = 100;
    CHECK(ph_picture_thumb(px, 48, 64, 48, out) == 1);
    CHECK(out[0] == 0);
    CHECK(out[1] == 150); /* columns 1 and 2: (100 + 200 + 100 + 200) / 4 */

    /* Smaller than the grid: cells repeat the nearest pixel instead of dividing by zero. */
    CHECK(ph_picture_thumb(px, 4, 4, 48, out) == 1);
    CHECK(ph_picture_thumb(NULL, 4, 4, 4, out) == 0);

    /* The detail grid splits the same picture into 64 cells a side: on a 128-wide
     * picture each cell is a 2 x 2 block, so a 2-pixel stripe stays visible. */
    {
        static uint8_t big[128 * 128], detail[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL];
        for (y = 0; y < 128; y++)
            for (x = 0; x < 128; x++)
                big[y * 128 + x] = (uint8_t)((x / 2) % 2 ? 250 : 10);
        CHECK(ph_picture_cells(big, 128, 128, 128, PH_PICTURE_DETAIL, detail) == 1);
        CHECK(detail[0] == 10 && detail[1] == 250 && detail[63 * 64 + 63] == 250);
        CHECK(ph_picture_thumb(big, 128, 128, 128, out) == 1);
        CHECK(out[0] == 130); /* the 32 grid averages the stripes away */
        CHECK(ph_picture_cells(big, 128, 128, 128, 0, detail) == 0);
    }
}

static void test_inbox_hello(void)
{
    CHECK(ph_inbox_parse_hello("PSHR 1 3") == 3);
    CHECK(ph_inbox_parse_hello("PSHR 1 500") == 500);
    CHECK(ph_inbox_parse_hello("PSHR 1 501") == -1);
    CHECK(ph_inbox_parse_hello("PSHR 1 0") == -1);
    CHECK(ph_inbox_parse_hello("PSHR 2 3") == 3);
    CHECK(ph_inbox_parse_hello("PSHR 1 3 x") == -1);
    CHECK(ph_inbox_parse_hello("PULL 1 3") == -1);
    CHECK(ph_inbox_parse_hello(NULL) == -1);
}

static void test_screen_header(void)
{
    const uint8_t video[5] = {0, 0, 1, 0, 1}, key[5] = {0, 0, 0, 9, 2};
    const uint8_t empty[5] = {0, 0, 0, 0, 1}, odd[5] = {0, 0, 0, 9, 7};
    const uint8_t huge[5] = {0x7f, 0, 0, 0, 1};
    const uint8_t audio[5] = {0, 0, 0x20, 0, 3}, short_audio[5] = {0, 0, 0, 3, 3};
    const uint8_t oversized_audio[5] = {0, 0, 0x20, 4, 3};
    int type = 0;

    CHECK(ph_screen_parse_header(video, PH_SCREEN_FRAME_MAX, &type) == 256 && type == 1);
    CHECK(ph_screen_parse_header(key, PH_SCREEN_FRAME_MAX, &type) == 9 && type == 2);
    CHECK(ph_screen_parse_header(audio, PH_SCREEN_FRAME_MAX, &type) == PH_SCREEN_AUDIO_MAX && type == 3);
    CHECK(ph_screen_parse_header(short_audio, PH_SCREEN_FRAME_MAX, &type) == 0);
    CHECK(ph_screen_parse_header(oversized_audio, PH_SCREEN_FRAME_MAX, &type) == 0);
    CHECK(ph_screen_parse_header(empty, PH_SCREEN_FRAME_MAX, &type) == 0);
    CHECK(ph_screen_parse_header(odd, PH_SCREEN_FRAME_MAX, &type) == 0);
    CHECK(ph_screen_parse_header(huge, PH_SCREEN_FRAME_MAX, &type) == 0);
    /* PSCR 4: one byte saying how the phone turned its screen upright. */
    const uint8_t turn[5] = {0, 0, 0, 1, 5}, long_turn[5] = {0, 0, 0, 2, 5};
    CHECK(ph_screen_parse_header(turn, PH_SCREEN_FRAME_MAX, &type) == 1 && type == 5);
    CHECK(ph_screen_parse_header(long_turn, PH_SCREEN_FRAME_MAX, &type) == 0);
}

static void test_gesture(void)
{
    ph_pull pull;

    ph_pull_reset(&pull);
    ph_pull_press(&pull, false, true, 0, 0);
    CHECK(!ph_pull_motion(&pull, false, 80, 0));

    ph_pull_press(&pull, true, false, 0, 0);
    CHECK(!ph_pull_motion(&pull, false, 80, 0));

    ph_pull_press(&pull, true, true, 10, 10);
    CHECK(!ph_pull_motion(&pull, true, 90, 10));
    CHECK(!ph_pull_motion(&pull, false, 20, 10));
    CHECK(ph_pull_motion(&pull, false, 10 + PH_PULL_TRAVEL_PX, 10));
    CHECK(pull.pulling);
    CHECK(!ph_pull_motion(&pull, false, 400, 10));
    ph_pull_release(&pull);
    CHECK(!pull.pulling && !pull.armed);

    ph_pull_allow_inside(&pull);
    CHECK(!ph_pull_lifts(&pull));
    ph_pull_press(&pull, true, true, 10, 10);
    ph_pull_allow_inside(&pull);
    CHECK(ph_pull_lifts(&pull));
    CHECK(!ph_pull_motion(&pull, true, 10 + 8, 10));
    CHECK(ph_pull_motion(&pull, true, 10 + PH_PULL_LIFT_PX, 10));
    CHECK(pull.pulling);
    ph_pull_release(&pull);
    ph_pull_press(&pull, true, true, 10, 10);
    CHECK(!ph_pull_lifts(&pull));
    CHECK(!ph_pull_motion(&pull, true, 10 + 40, 10));
}

static void test_name(void)
{
    unsigned long long size = 0;
    char name[96];

    CHECK(ph_pull_parse_file("FILE 12 photo.HEIC", 1000, &size, name, sizeof name));
    CHECK(size == 12 && strcmp(name, "photo.HEIC") == 0);
    CHECK(!ph_pull_parse_file("FILE 0 photo.HEIC", 1000, &size, name, sizeof name));
    CHECK(!ph_pull_parse_file("FILE 12 ../x.jpg", 1000, &size, name, sizeof name));
    CHECK(!ph_pull_parse_file("FILE 5000 photo.HEIC", 1000, &size, name, sizeof name));
    CHECK(!ph_pull_parse_file("MISS", 1000, &size, name, sizeof name));
    CHECK(!ph_pull_parse_file("FILE 4 my photo.jpg", 1000, &size, name, sizeof name));
    CHECK(ph_pull_parse_file("FILE 4 my_photo.jpg", 1000, &size, name, sizeof name));
    CHECK(size == 4 && strcmp(name, "my_photo.jpg") == 0);
}

int main(void)
{
    test_hash();
    test_thumb();
    test_screen_header();
    test_inbox_hello();
    test_gesture();
    test_name();
    if (g_failures) {
        fprintf(stderr, "%d pull checks failed\n", g_failures);
        return 1;
    }
    puts("pull checks passed");
    return 0;
}
