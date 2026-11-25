/* media_win32.c: WIC for still pictures, Media Foundation for video frames
 * (bare_picture). Both come with Windows; codecs the user installed (HEVC,
 * AV1, WebP, HEIC) work too. */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdint.h>
#include <stdlib.h>
#include "media.h"

static bool com_ready, mf_ready;

void bare__media_thread_begin(void) {
    com_ready = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    mf_ready = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

void bare__media_thread_end(void) {
    if (mf_ready) MFShutdown();
    if (com_ready) CoUninitialize();
    mf_ready = com_ready = false;
}

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void release(IUnknown *p) { if (p) p->lpVtbl->Release(p); }

bool bare__media_image(const char *path, BareMediaFrame frame, void *user) {
    wchar_t *wpath = widen(path);
    if (!wpath) return false;
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *conv = NULL;
    unsigned char *px = NULL;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                   (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, wpath, NULL, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppRGBA,
                                                 WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h)) && w && h && w <= 16384 && h <= 16384 &&
        (px = malloc((size_t)w * h * 4)) != NULL &&
        SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, px))) {
        frame(user, px, (int)w, (int)h, (int)w * 4, false, 0);
        ok = true;
    }
    free(px);
    release((IUnknown *)conv);
    release((IUnknown *)fr);
    release((IUnknown *)dec);
    release((IUnknown *)f);
    free(wpath);
    return ok;
}

/* The picture's size and where it sits in the decoded frame (decoders pad to 16 rows). */
static bool frame_size(IMFSourceReader *r, int *x, int *y, int *w, int *h, int *rows, LONG *stride) {
    IMFMediaType *t = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t))) return false;
    UINT64 size = 0;
    bool ok = SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &size));
    *x = *y = 0;
    *w = (int)(size >> 32), *h = *rows = (int)(size & 0xffffffffu);
    MFVideoArea area;
    if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, NULL)) &&
        area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value + area.Area.cx <= *w &&
        area.OffsetY.value + area.Area.cy <= *h) {
        *x = area.OffsetX.value, *y = area.OffsetY.value, *w = area.Area.cx, *h = area.Area.cy;
    }
    UINT32 s = 0;
    *stride = SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &s)) ? (LONG)s : 0;
    release((IUnknown *)t);
    return ok && *w > 0 && *h > 0;
}

bool bare__media_video(const char *path, int frames, float fps, BareMediaFrame frame, void *user) {
    if (!mf_ready || frames < 1) return false;
    wchar_t *wpath = widen(path);
    if (!wpath) return false;
    IMFAttributes *attr = NULL;
    IMFSourceReader *r = NULL;
    IMFMediaType *want = NULL;
    int got = 0;
    if (SUCCEEDED(MFCreateAttributes(&attr, 1))) {
        IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   /* any codec to RGB32 */
        if (FAILED(MFCreateSourceReaderFromURL(wpath, attr, &r))) r = NULL;
    }
    int x = 0, y = 0, w = 0, h = 0, rows = 0;
    LONG stride = 0;
    if (r && SUCCEEDED(MFCreateMediaType(&want)) &&
        SUCCEEDED(IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)) &&
        SUCCEEDED(IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32)) &&
        SUCCEEDED(IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) &&
        SUCCEEDED(IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) &&
        SUCCEEDED(IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, want)) &&
        frame_size(r, &x, &y, &w, &h, &rows, &stride)) {
        PROPVARIANT dur;
        PropVariantInit(&dur);
        if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(r, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION,
                                                               &dur)) && dur.vt == VT_UI8 && dur.uhVal.QuadPart > 0) {
            PROPVARIANT at;
            PropVariantInit(&at);
            at.vt = VT_I8;
            at.hVal.QuadPart = (LONGLONG)(dur.uhVal.QuadPart / 10);   /* past intros and fades */
            IMFSourceReader_SetCurrentPosition(r, &GUID_NULL, &at);
        }
        PropVariantClear(&dur);
        LONGLONG step = (LONGLONG)(1e7 / (fps > 0 ? fps : 8)), next = -1;
        int delay = (int)(1000 / (fps > 0 ? fps : 8));
        for (int reads = 0; got < frames && reads < frames * 40; reads++) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample *sample = NULL;
            if (FAILED(IMFSourceReader_ReadSample(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &ts,
                                                  &sample))) break;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) frame_size(r, &x, &y, &w, &h, &rows, &stride);
            if (!sample) {
                if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
                continue;
            }
            if (next >= 0 && ts < next) { release((IUnknown *)sample); continue; }
            next = (next < 0 ? ts : next) + step;
            IMFMediaBuffer *buf = NULL;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) {
                IMF2DBuffer *b2 = NULL;
                BYTE *scan = NULL;
                LONG pitch = 0;
                bool locked = false;
                if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
                    SUCCEEDED(IMF2DBuffer_Lock2D(b2, &scan, &pitch))) {
                    locked = true;
                } else {
                    DWORD len = 0;
                    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &scan, NULL, &len))) {
                        pitch = stride ? stride : w * 4;
                        if (pitch < 0) scan += (size_t)(-pitch) * (size_t)(rows - 1);   /* bottom-up: the top row */
                        if ((DWORD)labs(pitch) * (DWORD)rows > len) { IMFMediaBuffer_Unlock(buf); scan = NULL; }
                    }
                }
                if (scan) {
                    const unsigned char *top = scan + (ptrdiff_t)pitch * y + (ptrdiff_t)x * 4;
                    bool more = frame(user, top, w, h, (int)pitch, true, delay);
                    got++;
                    if (!more) got = frames;
                }
                if (locked) IMF2DBuffer_Unlock2D(b2);
                else if (scan) IMFMediaBuffer_Unlock(buf);
                release((IUnknown *)b2);
                release((IUnknown *)buf);
            }
            release((IUnknown *)sample);
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
    }
    release((IUnknown *)want);
    release((IUnknown *)r);
    release((IUnknown *)attr);
    free(wpath);
    return got > 0;
}
#else
#include "media.h"
void bare__media_thread_begin(void) {}
void bare__media_thread_end(void) {}
bool bare__media_image(const char *path, BareMediaFrame frame, void *user) {
    (void)path, (void)frame, (void)user;
    return false;
}
bool bare__media_video(const char *path, int frames, float fps, BareMediaFrame frame, void *user) {
    (void)path, (void)frames, (void)fps, (void)frame, (void)user;
    return false;
}
#endif
