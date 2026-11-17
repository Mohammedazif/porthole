#ifndef PORTHOLE_MEDIA_SHOT_H
#define PORTHOLE_MEDIA_SHOT_H

struct AVFrame;

/* One YUV 4:2:0 picture (YUV420P or YUVJ420P) to a PNG file. 0 on success, -1 on error. */
int ph_shot_write_png(const struct AVFrame *frame, const char *path);

#endif
