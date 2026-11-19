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

static void paint_box(frame *f, int x0, int x1, int y0, int y1, uint8_t luma)
{
    for (int y = y0; y < y1 && y < f->h; y++)
        memset(f->y + (size_t)y * f->w + x0, luma, (size_t)(x1 - x0));
}

static ph_rect detect(const frame *f, double aspect)
{
    ph_yuv_planes p = {f->y, f->u, f->v, f->w, f->w / 2, f->w, f->h};
    return ph_video_content_rect(&p, aspect);
}

static ph_rect picture(const frame *f)
{
    ph_yuv_planes p = {f->y, f->u, f->v, f->w, f->w / 2, f->w, f->h};
    return ph_video_picture_rect(&p);
}

static ph_rect item_at(const frame *f, int x, int y)
{
    ph_yuv_planes p = {f->y, f->u, f->v, f->w, f->w / 2, f->w, f->h};
    return ph_video_item_rect(&p, x, y);
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
    /* A portrait photo wider than the phone, centred in a 1920x1080 canvas. */
    {
        frame f = make_frame(1920, 1080, 16);
        int content_w = 808; /* about 3:4, still portrait */
        int bar = (1920 - content_w) / 2;
        paint(&f, bar, bar + content_w, 180);
        ph_rect r = detect(&f, 0.463);
        CHECK(r.x == bar && r.w == content_w && r.y == 0 && r.h == 1080);
        r = detect(&f, 0);
        CHECK(r.x == bar && r.w == content_w);
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
    /* The same off-centre photo is the picture a drag must hash. A 4:3 photo
     * shifted in a 1920x1080 frame stays 4:3, and the window crop stays full. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint_box(&f, 438, 1248, 0, 1080, 180);
        ph_rect shown = detect(&f, 500.0 / 1080);
        ph_rect photo = picture(&f);
        CHECK(is_full(shown, &f));
        CHECK(photo.x == 438 && photo.y == 0 && photo.w == 810 && photo.h == 1080);
        CHECK(fabs((double)photo.w / photo.h - 0.75) < 0.001);
        free_frame(&f);
    }
    /* Landscape photo between black bars above and below. */
    {
        frame f = make_frame(500, 1080, 16);
        paint_box(&f, 0, 500, 200, 880, 180);
        ph_rect photo = picture(&f);
        CHECK(photo.x == 0 && photo.y == 200 && photo.w == 500 && photo.h == 680);
        free_frame(&f);
    }
    /* A thin black edge is part of the photo. A speck is not a picture. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 4, 1916, 180);
        CHECK(is_full(picture(&f), &f));
        paint(&f, 0, f.w, 16);
        paint_box(&f, 100, 120, 100, 120, 220);
        CHECK(is_full(picture(&f), &f));
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
    /* A library grid is separate photos. One-pixel black gaps must not merge,
     * and a press on the gap belongs to one thumbnail. The page does not. */
    {
        frame f = make_frame(3 * 80 + 2, 200, 16);
        ph_rect a, b, gap, page;
        paint_box(&f, 0, 80, 5, 85, 180);
        paint_box(&f, 81, 161, 5, 85, 200);
        paint_box(&f, 162, 242, 5, 85, 140);
        a = item_at(&f, 40, 40);
        b = item_at(&f, 120, 40);
        gap = item_at(&f, 80, 40);
        page = item_at(&f, 40, 150);
        CHECK(a.w >= 64 && a.h >= 64 && a.x + a.w <= 80);
        CHECK(b.x >= 81 && b.x + b.w <= 161 && b.w >= 64 && b.h >= 64);
        CHECK(gap.w >= 64 && (gap.x + gap.w <= 80 || gap.x >= 81));
        CHECK(gap.w < 160);
        CHECK(page.w == 0 && page.h == 0);
        f.y[(size_t)40 * f.w + 120] = 16; /* a dark pixel inside the thumbnail */
        b = item_at(&f, 120, 40);
        CHECK(b.x >= 81 && b.x + b.w <= 161 && b.w >= 64);
        free_frame(&f);
    }
    /* Wider gaps, as on the Photos search cards, also stay apart. */
    {
        frame f = make_frame(300, 100, 16);
        ph_rect a, b;
        paint_box(&f, 10, 90, 10, 90, 180);
        paint_box(&f, 110, 190, 10, 90, 160);
        a = item_at(&f, 40, 50);
        b = item_at(&f, 140, 50);
        CHECK(a.x >= 10 && a.x + a.w <= 90 && a.w >= 64);
        CHECK(b.x >= 110 && b.x + b.w <= 190 && b.w >= 64);
        free_frame(&f);
    }
    /* A photo between black bars is that photo, not the whole frame. */
    {
        frame f = make_frame(200, 180, 16);
        ph_rect photo;
        paint_box(&f, 0, 200, 40, 140, 170);
        photo = item_at(&f, 100, 80);
        CHECK(photo.w >= 64 && photo.h >= 64);
        CHECK(photo.y >= 40 && photo.y + photo.h <= 140);
        free_frame(&f);
    }

    /* A dark edge with a small amount of detail belongs to the picture. It is
     * not the empty black separator above it, or a gap between library cells. */
    {
        frame f = make_frame(200, 240, 16);
        ph_rect photo;
        paint_box(&f, 0, 200, 80, 200, 170);
        paint_box(&f, 90, 110, 40, 80, 170);
        photo = item_at(&f, 100, 120);
        CHECK(photo.x == 0 && photo.w == 200);
        CHECK(photo.y == 40 && photo.h == 160);
        /* A sparse floating control touches the top of the photo. Its text is
         * not enough to join the picture to the surrounding Photos interface. */
        paint_box(&f, 96, 104, 20, 40, 230);
        photo = item_at(&f, 100, 120);
        CHECK(photo.y == 40 && photo.h == 160);
        free_frame(&f);
    }

    /* Photos' thin enclosing frame identifies a dark screenshot even when the
     * press is on its black padding or on one disconnected card inside it. */
    {
        frame f = make_frame(500, 1080, 16);
        ph_rect photo, padding;
        paint_box(&f, 68, 432, 128, 129, 30);
        paint_box(&f, 68, 432, 913, 914, 30);
        paint_box(&f, 68, 69, 128, 914, 36);
        paint_box(&f, 431, 432, 128, 914, 36);
        paint_box(&f, 86, 414, 294, 430, 42);
        paint_box(&f, 86, 414, 482, 574, 42);
        photo = item_at(&f, 250, 520);
        padding = item_at(&f, 250, 460);
        CHECK(abs(photo.x - 68) <= 2 && abs(photo.w - 364) <= 4);
        CHECK(abs(photo.y - 128) <= 2 && abs(photo.h - 786) <= 4);
        CHECK(padding.x == photo.x && padding.y == photo.y && padding.h == photo.h);
        CHECK(item_at(&f, 20, 460).w == 0);
        CHECK(item_at(&f, 250, 40).w == 0);
        /* The phone's broadcast uses full-range black; AirPlay can use 16. */
        for (int i = 0; i < f.w * f.h; i++)
            f.y[i] = (uint8_t)(f.y[i] - 16);
        photo = item_at(&f, 250, 520);
        CHECK(abs(photo.x - 68) <= 2 && abs(photo.w - 364) <= 4);
        CHECK(abs(photo.y - 128) <= 2 && abs(photo.h - 786) <= 4);
        /* Unframed cards in a real app are not an enclosing screenshot. */
        paint_box(&f, 65, 72, 124, 918, 0);
        paint_box(&f, 428, 435, 124, 918, 0);
        photo = item_at(&f, 250, 460);
        CHECK(photo.w == 0);
        free_frame(&f);
    }

    {
        /* From a live broadcast frame: Photos' black is 0, a dark screenshot is about
         * 17, and the screenshot's own status bar is black, crossed only by Photos'
         * thin frame lines on both sides with rounded top corners. */
        frame f = make_frame(500, 1080, 0);
        ph_rect shot, a, b;

        paint_box(&f, 68, 432, 128, 129, 24);   /* top frame line */
        for (int y = 129; y < 168; y++) {
            int inset = y < 140 ? 6 : 0;        /* rounded corner */
            f.y[y * 500 + 68 + inset] = 24;
            f.y[y * 500 + 431 - inset] = 24;
        }
        paint_box(&f, 150, 210, 143, 155, 240); /* the time in the status bar */
        paint_box(&f, 68, 432, 168, 914, 17);
        paint_box(&f, 76, 424, 560, 900, 60);   /* a share sheet inside it */
        shot = item_at(&f, 250, 500);
        CHECK(abs(shot.x - 68) <= 2 && abs(shot.w - 364) <= 4);
        CHECK(abs(shot.y - 128) <= 2 && abs(shot.h - 786) <= 4);
        CHECK(item_at(&f, 250, 1000).w == 0);
        free_frame(&f);

        /* Thumbnails a few black rows apart, with nothing down their sides, stay apart. */
        f = make_frame(500, 1080, 0);
        paint_box(&f, 20, 240, 100, 300, 17);
        paint_box(&f, 20, 240, 306, 506, 17);
        a = item_at(&f, 100, 200);
        b = item_at(&f, 100, 400);
        CHECK(a.w > 0 && a.y + a.h <= 302 && b.y >= 304);
        free_frame(&f);
    }

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all crop checks passed\n");
    return 0;
}
