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
void bare__media_thread_idle(void);    /* nothing to decode for now: let go of what can be made again */
/* The picture the system already keeps for a file (Explorer's thumbnail), about `size` pixels on its long
 * side, only if it is cached: fast, for something to show while the file is read. */
bool bare__media_thumb(const char *path, int size, BareMediaFrame frame, void *user);
/* A still picture in a format stb_image can't read (WebP, HEIC, TIFF, ...). */
bool bare__media_image(const char *path, BareMediaFrame frame, void *user);
/* Up to `frames` frames of a video, `fps` a second, starting a tenth of the way in, decoded on the GPU when
 * it can and shrunk there to about want_w x want_h (0: the video's size). */
bool bare__media_video(const char *path, int frames, float fps, int want_w, int want_h, BareMediaFrame frame,
                       void *user);

/* All of a file's sound (an audio file, or a video's soundtrack), decoded to 32-bit float, interleaved, one or two
 * channels at the file's rate. *samples is malloc'd (the caller frees it). Any thread that called
 * bare__media_thread_begin. */
bool bare__media_audio(const char *path, float **samples, int *frames, int *channels, int *rate);

/* A video read a frame at a time, from anywhere in it: for playing it (src/video.c). gpu: decode on the GPU and
 * shrink there to about want_w x want_h (0: the video's size); without, the CPU at the video's size. */
typedef struct BareMediaReader BareMediaReader;
BareMediaReader *bare__media_reader_open(const char *path, bool gpu, int want_w, int want_h);
/* The video's length in seconds (0 if unknown), its picture's size and frame rate (0 if unknown). */
void bare__media_reader_info(BareMediaReader *r, double *duration, int *width, int *height, float *fps);
bool bare__media_reader_seek(BareMediaReader *r, double seconds);   /* to the key frame at or before it */
/* The next frame, given to `frame` with its time in seconds: 1, 0 at the end, -1 on an error. */
int  bare__media_reader_next(BareMediaReader *r, BareMediaFrame frame, void *user, double *time);
void bare__media_reader_close(BareMediaReader *r);
