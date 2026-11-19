/* Checks that black side bars of TV-shaped frames are cropped, and nothing else is. */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/crop.h"
#include "video/photos_ui.h"

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

    {
        /* Photos' Select mode: a grid of thumbnails, two carrying the blue badge with a
         * white tick in their bottom-right corner; a blue sky alone is not a badge. */
        frame f = make_frame(500, 1080, 0);
        ph_rect sel[8];
        int n;

        for (int row = 0; row < 2; row++)
            for (int col = 0; col < 3; col++) {
                int x0 = col * 166, y0 = 200 + row * 166;
                paint_box(&f, x0, x0 + 164, y0, y0 + 164, (uint8_t)(70 + 20 * col + 10 * row));
            }
        /* thumbnail (0,0) is sky blue all over: no badge */
        for (int y = 200; y < 364; y++)
            for (int x = 0; x < 164; x++) {
                f.y[y * 500 + x] = 120;
                f.u[(y / 2) * 250 + x / 2] = 200;
                f.v[(y / 2) * 250 + x / 2] = 90;
            }
        /* badges on (1,0) and (2,1): a 26-pixel disc of iOS blue with a white tick */
        int badges[2][2] = {{166 + 164 - 20, 200 + 164 - 20}, {332 + 164 - 20, 366 + 164 - 20}};
        for (int b = 0; b < 2; b++) {
            int cx = badges[b][0], cy = badges[b][1];
            for (int y = cy - 13; y <= cy + 13; y++)
                for (int x = cx - 13; x <= cx + 13; x++) {
                    int dx = x - cx, dy = y - cy;
                    if (dx * dx + dy * dy > 13 * 13)
                        continue;
                    bool tick = (abs(dy - dx / 2) <= 1 && dx >= -6 && dx <= 7);
                    f.y[y * 500 + x] = tick ? 240 : 110;
                    f.u[(y / 2) * 250 + x / 2] = tick ? 128 : 210;
                    f.v[(y / 2) * 250 + x / 2] = tick ? 128 : 60;
                }
        }
        n = ph_video_selected_items(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080}, NULL, sel, 8);
        CHECK(n == 2);
        CHECK(n >= 1 && abs(sel[0].x - 166) <= 2 && abs(sel[0].y - 200) <= 2 && abs(sel[0].w - 164) <= 4);
        CHECK(n >= 2 && abs(sel[1].x - 332) <= 2 && abs(sel[1].y - 366) <= 2 && abs(sel[1].h - 164) <= 4);
        free_frame(&f);
    }

    {
        /* Photos' grid at several zooms, scrolled part way, in dark and light mode, with
         * textured, dark and flat thumbnails. A textured photo is not a grid. */
        int columns[] = {3, 5, 7, 13};
        unsigned seed = 7;
        for (int mode = 0; mode < 2; mode++) {
            uint8_t page = mode ? 255 : 0;
            for (size_t c = 0; c < sizeof columns / sizeof columns[0]; c++) {
                int n = columns[c];
                double pitch = (500.0 + 2) / n;
                int scroll = 37;
                frame f = make_frame(500, 1080, page);
                ph_grid g;
                ph_rect cell;

                for (int row = -1; row * pitch < 1080; row++)
                    for (int col = 0; col < n; col++) {
                        int x0 = (int)ceil(col * pitch), x1 = (int)floor((col + 1) * pitch - 2);
                        int y0 = (int)ceil(scroll + row * pitch), y1 = (int)floor(scroll + (row + 1) * pitch - 2);
                        int base = 20 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 200;
                        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < 1080; y++)
                            for (int x = x0; x < x1 && x < 500; x++) {
                                int texture = ((x * 7 + y * 3) % 11) * ((col + row) % 3);
                                f.y[y * 500 + x] = (uint8_t)(base + texture);
                            }
                    }
                CHECK(ph_video_grid(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080}, 250, 540, &g));
                CHECK(g.columns == n);
                cell = ph_grid_cell(&g, 250, 540, 500, 1080);
                {
                    int col = (int)(250 / pitch);
                    int row = (int)floor((540 - scroll) / pitch);
                    CHECK(abs(cell.x - (int)ceil(col * pitch)) <= 2);
                    CHECK(abs(cell.y - (int)ceil(scroll + row * pitch)) <= 2);
                    /* The whole thumbnail: a cut further in reads as a zoom to the phone. */
                    CHECK(abs(cell.w - (int)(pitch - 2)) <= 1 && abs(cell.h - (int)(pitch - 2)) <= 1);
                }
                free_frame(&f);
            }
        }
        {
            frame f = make_frame(500, 1080, 0);
            ph_grid g;
            for (int y = 0; y < 1080; y++)
                for (int x = 0; x < 500; x++)
                    f.y[y * 500 + x] = (uint8_t)(60 + (x * 13 + y * 7) % 90 + (x / 50) * 5);
            CHECK(!ph_video_grid(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080}, 250, 540, &g));
            free_frame(&f);
        }
        {
            /* Badges map to their own cells in a small grid. */
            frame f = make_frame(500, 1080, 0);
            double pitch = 502.0 / 5;
            ph_grid g;
            ph_rect sel[8];
            int n;

            for (int row = 0; row * pitch < 1080; row++)
                for (int col = 0; col < 5; col++)
                    paint_box(&f, (int)ceil(col * pitch), (int)floor((col + 1) * pitch - 2),
                              (int)ceil(row * pitch), (int)floor((row + 1) * pitch - 2),
                              (uint8_t)(40 + 30 * ((row + col) % 5)));
            for (int b = 0; b < 2; b++) {
                int cx = (int)((b ? 3 : 1) * pitch + pitch - 16), cy = (int)((b ? 6 : 4) * pitch + pitch - 16);
                for (int y = cy - 9; y <= cy + 9; y++)
                    for (int x = cx - 9; x <= cx + 9; x++) {
                        int dx = x - cx, dy = y - cy;
                        if (dx * dx + dy * dy > 81)
                            continue;
                        bool tick = abs(dy - dx / 2) <= 1 && dx >= -4 && dx <= 5;
                        f.y[y * 500 + x] = tick ? 240 : 110;
                        f.u[(y / 2) * 250 + x / 2] = tick ? 128 : 210;
                        f.v[(y / 2) * 250 + x / 2] = tick ? 128 : 60;
                    }
            }
            CHECK(ph_video_grid(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080}, 250, 540, &g));
            n = ph_video_selected_items(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080}, &g, sel, 8);
            CHECK(n == 2);
            CHECK(n == 2 && abs(sel[0].x - (int)ceil(pitch)) <= 2 && abs(sel[0].y - (int)ceil(4 * pitch)) <= 2);
            CHECK(n == 2 && abs(sel[1].x - (int)ceil(3 * pitch)) <= 2 && abs(sel[1].y - (int)ceil(6 * pitch)) <= 2);
            /* The badge to paint out: the disc, its ring and shadow, inside the cell only. */
            if (n == 2) {
                ph_rect b[4];
                int nb = ph_video_badges_in(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080}, sel[0], b, 4);
                int cx = (int)(pitch + pitch - 16), cy = (int)(4 * pitch + pitch - 16);
                CHECK(nb == 1);
                CHECK(nb == 1 && b[0].x <= cx - 11 && b[0].y <= cy - 11 && b[0].x + b[0].w >= cx + 11
                      && b[0].y + b[0].h >= cy + 11);
                CHECK(nb == 1 && b[0].x >= sel[0].x && b[0].y >= sel[0].y
                      && b[0].x + b[0].w <= sel[0].x + sel[0].w && b[0].y + b[0].h <= sel[0].y + sel[0].h);
                CHECK(ph_video_badges_in(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080},
                                         ph_grid_cell(&g, 250, 50, 500, 1080), b, 4) == 0);
            }
            free_frame(&f);
        }
    }

    {
        /* Photos' Share button: a round button bottom left (Select mode), not the end
         * of the library's tab bar there. */
        ph_rect screen = {0, 0, 500, 1080};
        int x, y;

        for (int bar = 0; bar < 2; bar++) {
            frame f = make_frame(500, 1080, 0);
            for (int py = 960; py < 1080; py++)
                for (int px = 0; px < 500; px++) {
                    bool inside;
                    if (bar) { /* a capsule from x 25 to 245 */
                        int cx = px < 53 ? 53 : px > 217 ? 217 : px;
                        inside = (px - cx) * (px - cx) + (py - 1020) * (py - 1020) <= 28 * 28;
                    } else {
                        inside = (px - 60) * (px - 60) + (py - 1020) * (py - 1020) <= 28 * 28;
                    }
                    if (inside)
                        f.y[py * 500 + px] = 26;
                }
            for (int py = 1010; py < 1030; py++) /* the glyph */
                for (int px = 52; px < 68; px++)
                    f.y[py * 500 + px] = 240;
            bool found = ph_photos_share_button(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080},
                                                screen, &x, &y);
            CHECK(found == !bar);
            CHECK(bar || (abs(x - 60) <= 2 && abs(y - 1020) <= 2));
            free_frame(&f);
        }
    }

    {
        /* Porthole's purple icon in a Share sheet row; a purple-ish photo is not it. */
        frame f = make_frame(500, 1080, 200);
        int x = 0, y = 0;
        for (int i = 0; i < 2; i++) {
            int x0 = 40 + i * 110;
            for (int py = 700; py < 776; py++)
                for (int px = x0; px < x0 + 76; px++) {
                    f.y[py * 500 + px] = 87;
                    f.u[(py / 2) * 250 + px / 2] = i ? 223 : 177;
                    f.v[(py / 2) * 250 + px / 2] = i ? 203 : 155;
                }
        }
        for (int py = 725; py < 751; py++) /* the white ring's middle */
            for (int px = 175; px < 201; px++) {
                f.u[(py / 2) * 250 + px / 2] = 128;
                f.v[(py / 2) * 250 + px / 2] = 128;
            }
        CHECK(ph_photos_porthole_icon(&(ph_yuv_planes){f.y, f.u, f.v, 500, 250, 500, 1080},
                                      (ph_rect){0, 0, 500, 1080}, &x, &y));
        CHECK(abs(x - 188) <= 2 && abs(y - 738) <= 2);
        free_frame(&f);
    }

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all crop checks passed\n");
    return 0;
}
