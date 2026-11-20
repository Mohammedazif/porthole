#include "files/clip.h"

#include <string.h>

bool ph_clip_header(const char *line, char *kind, size_t *size)
{
    const char *p;
    unsigned long long n = 0;
    size_t max;

    if (!line || !kind || !size || strncmp(line, "CLIP ", 5) != 0)
        return false;
    p = line + 5;
    if ((p[0] != 'T' && p[0] != 'P') || p[1] != ' ')
        return false;
    for (p += 2; *p >= '0' && *p <= '9'; p++) {
        n = n * 10u + (unsigned)(*p - '0');
        if (n > PH_CLIP_IMAGE_MAX)
            return false;
    }
    if (*p != '\0' || n == 0)
        return false;
    max = line[5] == 'T' ? PH_CLIP_TEXT_MAX : PH_CLIP_IMAGE_MAX;
    if (n > max)
        return false;
    *kind = line[5];
    *size = (size_t)n;
    return true;
}
