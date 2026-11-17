/* Checks that black side bars of TV-shaped frames are cropped, and nothing else is. */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/crop.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct frame {
    int w, h;
    uint8_t *y, *u, *v;
} frame;

static frame make_frame(int w, int h, uint8_t luma)
{
    frame f = {w, h, malloc((size_t)w * h), malloc((size_t)w * h / 4), malloc((size_t)w * h / 4)};
    memset(f.y, luma, (size_t)w * h);
    memset(f.u, 128, (size_t)w * h / 4);
    memset(f.v, 128, (size_t)w * h / 4);
    return f;
}

/* Paints columns [x0, x1) with a luma value (chroma stays neutral). */
static void paint(frame *f, int x0, int x1, uint8_t luma)
{
    for (int y = 0; y < f->h; y++)
        memset(f->y + (size_t)y * f->w + x0, luma, (size_t)(x1 - x0));
}

static ph_rect detect(const frame *f, double aspect)
{
    ph_yuv_planes p = {f->y, f->u, f->v, f->w, f->w / 2, f->w, f->h};
    return ph_video_content_rect(&p, aspect);
}

static ph_rect detect_stable(const frame *f, double aspect, ph_video_crop_state *state)
{
    ph_yuv_planes p = {f->y, f->u, f->v, f->w, f->w / 2, f->w, f->h};
    return ph_video_content_rect_stable(&p, aspect, state);
}

static void free_frame(frame *f)
{
    free(f->y);
    free(f->u);
    free(f->v);
}

static int is_full(ph_rect r, const frame *f)
{
    return r.x == 0 && r.y == 0 && r.w == f->w && r.h == f->h;
}

int main(void)
{
    /* AirPlay media output can be 16:9 while the physical phone remains tall.
     * Only the native shape, its reciprocal, or a verified pillarbox crop may
     * drive its pointer. Chroma/codec rounding gets two short-edge pixels. */
    {
        double aspect = 500.0 / 1080;
        CHECK(ph_video_matches_phone_screen((ph_rect){0, 0, 500, 1080}, aspect));
        CHECK(ph_video_matches_phone_screen((ph_rect){0, 0, 1920, 888}, aspect));
        CHECK(ph_video_matches_phone_screen((ph_rect){710, 0, 500, 1080}, aspect));
        CHECK(ph_video_matches_phone_screen((ph_rect){0, 0, 502, 1080}, aspect));
        CHECK(!ph_video_matches_phone_screen((ph_rect){0, 0, 504, 1080}, aspect));
        CHECK(!ph_video_matches_phone_screen((ph_rect){0, 0, 1920, 1080}, aspect));
        CHECK(!ph_video_matches_phone_screen((ph_rect){0, 0, 0, 1080}, aspect));
        CHECK(ph_video_matches_phone_screen((ph_rect){0, 0, 1920, 1080}, 0));
        CHECK(ph_video_matches_phone_screen((ph_rect){0, 0, 1920, 1080}, NAN));
        /* A native 16:9 phone cannot be distinguished from 16:9 media by shape. */
        CHECK(ph_video_matches_phone_screen((ph_rect){0, 0, 1920, 1080}, .5625));
    }
    /* Portrait frames are never cropped, even if their edges are black (dark mode). */
    {
        frame f = make_frame(500, 1080, 16);
        paint(&f, 100, 400, 200);
        CHECK(is_full(detect(&f, 0.463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }
    /* Pillarboxed 1920x1080: black bars, phone screen in the middle. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 710, 1210, 180);
        ph_rect r = detect(&f, 0.463);
        CHECK(r.x == 710 && r.w == 500 && r.y == 0 && r.h == 1080);
        r = detect(&f, 0); /* shape unknown: found by scanning */
        CHECK(r.x == 710 && r.w == 500);
        free_frame(&f);
    }
    /* Pillarboxed, phone in dark mode (screen mostly black): known shape still crops. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 900, 1000, 220); /* a little bright content in the middle only */
        ph_rect r = detect(&f, 0.463);
        CHECK(r.x == 710 && r.w == 500);
        free_frame(&f);
    }
    /* Real landscape content filling the frame: no crop. */
    {
        frame f = make_frame(1920, 1080, 90);
        CHECK(is_full(detect(&f, 0.463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }
    /* Landscape dark-mode app: black on the left, content on the right: no crop. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 1300, 1900, 200);
        CHECK(is_full(detect(&f, 0.463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }
    /* Off-centre content with unknown shape: not treated as pillarboxing. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 200, 700, 200);
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }

    /* Reproduce the live 1920x888 landscape stream. Its reciprocal phone shape
     * proves orientation even when black or containing only centred text. */
    {
        frame f = make_frame(1920, 888, 16);
        const double aspects[] = {0.463, 500.0 / 1080};
        for (size_t i = 0; i < sizeof aspects / sizeof aspects[0]; i++) {
            CHECK(is_full(detect(&f, aspects[i]), &f));
            paint(&f, 910, 1010, 220); /* bright subtitle/overlay in the centre */
            CHECK(is_full(detect(&f, aspects[i]), &f));
            paint(&f, 0, f.w, 100);
            CHECK(is_full(detect(&f, aspects[i]), &f));
            paint(&f, 0, f.w, 16);
        }
        free_frame(&f);
    }
    /* A black TV canvas cannot establish portrait bars from a remembered
     * aspect alone. Sparse bright pixels (including the pointer) are ambiguous. */
    {
        frame f = make_frame(1920, 1080, 16);
        CHECK(is_full(detect(&f, .463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        for (int y = 500; y < 520; y++)
            memset(f.y + (size_t)y * f.w + 950, 220, 20);
        CHECK(is_full(detect(&f, .463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }
    /* Same-size fades retain whichever orientation was established. Real
     * pillarboxed portrait and full landscape transitions remain immediate. */
    {
        frame f = make_frame(1920, 1080, 90);
        ph_video_crop_state state = {0};
        CHECK(is_full(detect_stable(&f, .463, &state), &f));
        for (int i = 0; i < 50; i++) {
            paint(&f, 0, f.w, i % 2 ? 16 : 32);
            CHECK(is_full(detect_stable(&f, .463, &state), &f));
        }
        paint(&f, 0, f.w, 16);
        paint(&f, 710, 1210, 180);
        ph_rect r = detect_stable(&f, .463, &state);
        CHECK(r.x == 710 && r.w == 500 && r.h == 1080);
        for (int i = 0; i < 50; i++) {
            paint(&f, 0, f.w, i % 2 ? 16 : 32);
            r = detect_stable(&f, .463, &state);
            CHECK(r.x == 710 && r.w == 500 && r.h == 1080);
        }
        paint(&f, 1300, 1900, 200); /* visible landscape outside the old crop */
        CHECK(is_full(detect_stable(&f, .463, &state), &f));
        paint(&f, 0, f.w, 16);
        CHECK(is_full(detect_stable(&f, .463, &state), &f));
        ph_video_crop_reset(&state);
        CHECK(!state.frame_width && !state.frame_height && !state.screen.w);
        CHECK(is_full(detect_stable(&f, .463, &state), &f));
        CHECK(is_full(detect_stable(&f, .463, NULL), &f));
        free_frame(&f);
    }
    /* Unknown aspect still discovers real bars and retains them during a fade;
     * stream-size changes and native portrait frames never reuse an old crop. */
    {
        frame f = make_frame(1920, 1080, 16);
        ph_video_crop_state state = {0};
        paint(&f, 710, 1210, 180);
        ph_rect r = detect_stable(&f, 0, &state);
        CHECK(r.x == 710 && r.w == 500);
        paint(&f, 0, f.w, 16);
        r = detect_stable(&f, 0, &state);
        CHECK(r.x == 710 && r.w == 500);
        free_frame(&f);
        f = make_frame(1920, 888, 16);
        CHECK(is_full(detect_stable(&f, .463, &state), &f));
        free_frame(&f);
        f = make_frame(500, 1080, 16);
        CHECK(is_full(detect_stable(&f, .463, &state), &f));
        free_frame(&f);
        f = make_frame(1920, 1080, 16);
        CHECK(is_full(detect_stable(&f, .463, &state), &f));
        free_frame(&f);
    }
    /* Authoritative native geometry overrides a prior pixel-derived crop even
     * if the remembered phone aspect changes while the TV canvas stays black. */
    {
        frame f = make_frame(1920, 1080, 16);
        ph_video_crop_state state = {0};
        paint(&f, 710, 1210, 180);
        CHECK(detect_stable(&f, .463, &state).w == 500);
        paint(&f, 0, f.w, 16);
        CHECK(is_full(detect_stable(&f, 1080.0 / 1920, &state), &f));
        free_frame(&f);
    }

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all crop checks passed\n");
    return 0;
}
