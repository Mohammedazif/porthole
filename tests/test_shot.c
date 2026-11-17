/* YUV 4:2:0 still -> PNG signature and IHDR size. */
#ifdef main
#undef main
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavutil/frame.h>
#include <libavutil/imgutils.h>

#include "media/shot.h"
#include "temp_path.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static AVFrame *yuv_frame(int format)
{
    AVFrame *frame = av_frame_alloc();
    int y, x;

    if (!frame)
        return NULL;
    frame->format = format;
    frame->width = 16;
    frame->height = 16;
    if (av_frame_get_buffer(frame, 16) < 0) {
        av_frame_free(&frame);
        return NULL;
    }
    for (y = 0; y < 16; y++) {
        uint8_t *row = frame->data[0] + y * frame->linesize[0];
        for (x = 0; x < 16; x++)
            row[x] = (uint8_t)(16 + x + y);
    }
    for (y = 0; y < 8; y++) {
        memset(frame->data[1] + y * frame->linesize[1], 90, 8);
        memset(frame->data[2] + y * frame->linesize[2], 160, 8);
    }
    return frame;
}

static void expect_png(const char *path)
{
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t buf[24];
    FILE *file = fopen(path, "rb");

    CHECK(file);
    if (!file)
        return;
    CHECK(fread(buf, 1, sizeof buf, file) == sizeof buf);
    fclose(file);
    CHECK(memcmp(buf, sig, 8) == 0);
    CHECK(memcmp(buf + 12, "IHDR", 4) == 0);
    CHECK(be32(buf + 16) == 16 && be32(buf + 20) == 16);
}

static void write_one(int format)
{
    char path[1024];
    AVFrame *frame = yuv_frame(format);
    int fd = ph_test_temp_file(path, sizeof path);

    CHECK(frame && fd >= 0);
    if (fd >= 0)
        close(fd);
    if (frame && fd >= 0) {
        CHECK(ph_shot_write_png(frame, path) == 0);
        expect_png(path);
    }
    if (fd >= 0)
        remove(path);
    av_frame_free(&frame);
}

int main(void)
{
    AVFrame *bad = av_frame_alloc();

    CHECK(bad);
    if (bad) {
        bad->format = AV_PIX_FMT_RGB24;
        bad->width = 16;
        bad->height = 16;
        CHECK(ph_shot_write_png(bad, "/tmp/porthole-shot-unused.png") == -1);
        CHECK(ph_shot_write_png(NULL, "/tmp/porthole-shot-unused.png") == -1);
        CHECK(ph_shot_write_png(bad, NULL) == -1);
    }
    av_frame_free(&bad);
    write_one(AV_PIX_FMT_YUV420P);
    write_one(AV_PIX_FMT_YUVJ420P);

    if (g_failures) {
        fprintf(stderr, "%d shot check(s) failed\n", g_failures);
        return 1;
    }
    printf("png signature and 16x16 IHDR passed\n");
    return 0;
}
