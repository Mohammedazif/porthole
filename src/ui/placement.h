#ifndef PORTHOLE_UI_PLACEMENT_H
#define PORTHOLE_UI_PLACEMENT_H

/* Saved mirror-window geometry. One line, no SDL. */

#include <stdbool.h>
#include <stddef.h>

typedef struct ph_placement {
    int x, y, w, h;
    bool fullscreen;
    bool always_on_top;
    bool one_to_one;
    bool valid;
} ph_placement;

/* "<x> <y> <w> <h> <fullscreen> <always_on_top> <one_to_one>", flags 0 or 1. */
bool ph_placement_format(const ph_placement *placement, char *out, size_t out_len);
bool ph_placement_parse(const char *line, ph_placement *out);

/* Video pixel size, scaled down uniformly when it does not fit. Never 0. */
void ph_chrome_one_to_one(int video_w, int video_h, int screen_w, int screen_h,
                          int *out_w, int *out_h);

/* Window frame outside the client area, in the same units as the client size. */
typedef struct ph_window_insets {
    int top, left, bottom, right;
} ph_window_insets;

/* Client size for a portrait or landscape picture. At most 90% of the usable
 * display after insets, never larger than the picture, never 0. */
void ph_chrome_fit_client(int pic_w, int pic_h, int usable_w, int usable_h,
                          const ph_window_insets *insets, int *out_w, int *out_h);

/* Client position whose outer frame stays inside the usable display.
 * center ignores the anchor and centers the frame. Otherwise the anchor's
 * center is kept, then clamped. */
void ph_chrome_place_client(int client_w, int client_h, int usable_x, int usable_y,
                            int usable_w, int usable_h, const ph_window_insets *insets,
                            int anchor_x, int anchor_y, int anchor_w, int anchor_h,
                            bool center, int *out_x, int *out_y);

#endif
