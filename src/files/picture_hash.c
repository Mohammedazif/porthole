#include "files/picture_hash.h"

#include <math.h>

static int at_axis(int numer, int denom, int limit)
{
    int v;

    if (limit < 1)
        return 0;
    v = numer / denom;
    if (v < 0)
        return 0;
    if (v >= limit)
        return limit - 1;
    return v;
}

uint64_t ph_picture_dhash(const uint8_t *pixels, int width, int height, int stride)
{
    uint64_t hash = 0;
    int r, c;

    if (!pixels || width < 2 || height < 2 || stride < width)
        return 0;
    for (r = 0; r < 8; r++) {
        int y = at_axis((r * 2 + 1) * height, 16, height);
        const uint8_t *row = pixels + (size_t)y * (size_t)stride;

        for (c = 0; c < 8; c++) {
            int x0 = at_axis((c * 2 + 1) * width, 18, width);
            int x1 = at_axis(((c + 1) * 2 + 1) * width, 18, width);

            if (row[x0] > row[x1])
                hash |= (uint64_t)1 << (63 - (r * 8 + c));
        }
    }
    return hash;
}

int ph_picture_thumb(const uint8_t *pixels, int width, int height, int stride,
                     uint8_t out[PH_PICTURE_THUMB * PH_PICTURE_THUMB])
{
    const int n = PH_PICTURE_THUMB;
    int r, c;

    if (!pixels || !out || width < 1 || height < 1 || stride < width)
        return 0;
    for (r = 0; r < n; r++) {
        int y0 = r * height / n, y1 = (r + 1) * height / n;

        if (y1 <= y0)
            y1 = y0 + 1;
        for (c = 0; c < n; c++) {
            int x0 = c * width / n, x1 = (c + 1) * width / n;
            unsigned long sum = 0;
            int x, y;

            if (x1 <= x0)
                x1 = x0 + 1;
            for (y = y0; y < y1; y++) {
                const uint8_t *row = pixels + (size_t)y * (size_t)stride;
                for (x = x0; x < x1; x++)
                    sum += row[x];
            }
            out[r * n + c] = (uint8_t)((sum + (unsigned long)((y1 - y0) * (x1 - x0)) / 2)
                                       / (unsigned long)((y1 - y0) * (x1 - x0)));
        }
    }
    return 1;
}

int ph_picture_hamming(uint64_t a, uint64_t b)
{
    uint64_t x = a ^ b;
    int n = 0;

    while (x) {
        n += (int)(x & 1);
        x >>= 1;
    }
    return n;
}

int ph_picture_pick(uint64_t query, double query_aspect, const uint64_t *hashes,
                    const double *aspects, int count)
{
    int i, best = -1, best_d = 65, second = 65;

    if (!hashes || count < 1)
        return -1;
    for (i = 0; i < count; i++) {
        int d;

        if (aspects && query_aspect > 0 && aspects[i] > 0) {
            double diff = fabs(aspects[i] - query_aspect) / query_aspect;
            if (diff > PH_PICTURE_ASPECT_TOL)
                continue;
        }
        d = ph_picture_hamming(query, hashes[i]);
        if (d < best_d) {
            second = best_d;
            best_d = d;
            best = i;
        } else if (d < second) {
            second = d;
        }
    }
    if (best < 0 || best_d > PH_PICTURE_MATCH_MAX)
        return -1;
    if (second < best_d + PH_PICTURE_MATCH_GAP)
        return -1;
    return best;
}
