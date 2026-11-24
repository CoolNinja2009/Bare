/* backdrop.h: the desktop behind the window, for bare_window_backdrop
 * (backdrop_win32.c; other platforms have none yet). */
#ifndef BARE_BACKDROP_H
#define BARE_BACKDROP_H

typedef struct BareBackdrop BareBackdrop;

/* Starts capturing what's under `hwnd` on a thread; wake(user) is called
 * (from that thread) when a new picture is ready. NULL if unsupported. */
BareBackdrop *bare__backdrop_start(void *hwnd, void (*wake)(void *user), void *user);
/* The newest picture (RGBA8, *width x *height physical pixels) if one arrived
 * since the last call, else NULL. Valid until the next call. */
const void   *bare__backdrop_take(BareBackdrop *b, int *width, int *height);
void          bare__backdrop_stop(BareBackdrop *b);

#endif
