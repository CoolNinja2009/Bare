/* record.h: encoding frames to a video file, for BARE_RECORD (src/bare.c).
 * Windows: H.264 in an MP4 through Media Foundation. Elsewhere open fails. */
#pragma once
#include <stdbool.h>

typedef struct BareRecorder BareRecorder;

/* width and height must be even (H.264 works in 2x2 blocks). NULL on failure (logged). */
BareRecorder *bare__record_open(const char *path, int width, int height, int fps);
/* One frame, the recorder's size: rows top first, `pitch` bytes apart; BGRA or RGBA. */
bool bare__record_frame(BareRecorder *r, const void *pixels, int pitch, bool bgra);
/* Finishes the file; returns how many frames it holds. */
long bare__record_close(BareRecorder *r);
