/* media.h: decoders the system has (Windows: the shell's thumbnail cache, WIC
 * images, Media Foundation video), for bare_picture (src/picture.c).
 * Elsewhere they return false. */
#pragma once
#include <stdbool.h>

/* One decoded frame: `pitch` bytes between rows (negative: bottom-up rows,
 * `pixels` is the top one), RGBA or BGRA (alpha ignored for BGRA). Return
 * false to stop. */
typedef bool (*BareMediaFrame)(void *user, const unsigned char *pixels, int width, int height, int pitch,
                               bool bgra, int delay_ms);

void bare__media_thread_begin(void);   /* each decoding thread, before the calls below */
void bare__media_thread_end(void);
/* The picture the system already keeps for a file (Explorer's thumbnail), about `size` pixels on its long
 * side, only if it is cached: fast, for something to show while the file is read. */
bool bare__media_thumb(const char *path, int size, BareMediaFrame frame, void *user);
/* A still picture in a format stb_image can't read (WebP, HEIC, TIFF, ...). */
bool bare__media_image(const char *path, BareMediaFrame frame, void *user);
/* Up to `frames` frames of a video, `fps` a second, starting a tenth of the way in, decoded on the GPU when
 * it can and shrunk there to about want_w x want_h (0: the video's size). */
bool bare__media_video(const char *path, int frames, float fps, int want_w, int want_h, BareMediaFrame frame,
                       void *user);
