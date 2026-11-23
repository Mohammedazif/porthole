#ifndef PORTHOLE_MEDIA_RECORD_H
#define PORTHOLE_MEDIA_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ph_record ph_record;

#define PH_RECORD_FORMAT_CHANGED (-2)

/* Mux Annex-B H.264 access units to MP4 (avc1) without re-encoding. */
ph_record *ph_record_open(const char *path);
int ph_record_video(ph_record *rec, const uint8_t *annexb, size_t len, uint64_t pts_ns,
                    bool keyframe);
/* Writes the trailer. NULL is safe and returns 0. */
int ph_record_close(ph_record *rec);

#endif
