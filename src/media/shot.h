#ifndef PORTHOLE_MEDIA_SHOT_H
#define PORTHOLE_MEDIA_SHOT_H

struct AVFrame;

/* One YUV 4:2:0 picture (YUV420P or YUVJ420P) to a PNG file. 0 on success, -1 on error. */
int ph_shot_write_png(const struct AVFrame *frame, const char *path);

/* Writes the even-aligned rectangle of that picture. */
int ph_shot_write_png_rect(const struct AVFrame *frame, int x, int y, int w, int h,
                           const char *path);

#endif
