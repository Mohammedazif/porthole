#ifndef PORTHOLE_FILES_PICTURE_HASH_H
#define PORTHOLE_FILES_PICTURE_HASH_H

#include <stddef.h>
#include <stdint.h>

/* 64-bit difference hash of one plane. Bit 63 is the top-left pair: the sample
 * in column 0 is strictly greater than the sample in column 1. The iPhone app
 * uses this same bit order. */
uint64_t ph_picture_dhash(const uint8_t *pixels, int width, int height, int stride);

/* Box average of a whole picture into PH_PICTURE_THUMB x PH_PICTURE_THUMB cells, top
 * row first. Cell (r, c) covers rows [r*h/N, max(that+1, (r+1)*h/N)) and the same for
 * columns; the iPhone app samples the same cells. A dHash cannot tell burst shots
 * apart; the phone compares the gradients of this picture instead. */
#define PH_PICTURE_THUMB 32
int ph_picture_thumb(const uint8_t *pixels, int width, int height, int stride,
                     uint8_t out[PH_PICTURE_THUMB * PH_PICTURE_THUMB]);

/* The same box average at PH_PICTURE_DETAIL cells a side. The phone compares it only
 * between its few closest candidates, such as two shots of a burst or two moments
 * of one video, which the 32-cell thumb cannot tell apart. */
#define PH_PICTURE_DETAIL 64
int ph_picture_cells(const uint8_t *pixels, int width, int height, int stride, int side,
                     uint8_t *out);

int ph_picture_hamming(uint64_t a, uint64_t b);

/* A match must be at most this many bits away, and the next candidate must be
 * at least this many bits worse. Closer pairs are treated as two different photos. */
#define PH_PICTURE_MATCH_MAX 10
#define PH_PICTURE_MATCH_GAP 6
#define PH_PICTURE_ASPECT_TOL 0.08

/* Index of the one close picture, or -1. aspects may be NULL. */
int ph_picture_pick(uint64_t query, double query_aspect, const uint64_t *hashes,
                    const double *aspects, int count);

#endif
