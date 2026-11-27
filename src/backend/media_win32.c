/* media_win32.c: the shell's thumbnail cache, WIC for still pictures, Media
 * Foundation for video frames (bare_picture). All come with Windows; codecs
 * the user installed (HEVC, AV1, WebP, HEIC) work too. Video decodes on the
 * GPU (DXVA) and is shrunk there, so only a small frame comes back. */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdint.h>
#include <stdlib.h>
#include "media.h"

#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif

/* each decoding thread starts COM and Media Foundation; they all share one D3D11 device for video, kept
 * while any of them is busy (a device costs the driver's memory, about 10 MB each) */
static THREAD_LOCAL bool com_ready, mf_ready, gpu_using;
static SRWLOCK gpu_lock = SRWLOCK_INIT;
static ID3D11Device *gpu_device;
static IMFDXGIDeviceManager *gpu_manager;
static int gpu_users;
static bool gpu_failed;

static const GUID k_iid_multithread = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };

static void release(IUnknown *p) { if (p) p->lpVtbl->Release(p); }

void bare__media_thread_begin(void) {
    com_ready = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    mf_ready = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

/* This thread is done with video for now; the last one to be lets the device go. */
void bare__media_thread_idle(void) {
    if (!gpu_using) return;
    gpu_using = false;
    AcquireSRWLockExclusive(&gpu_lock);
    if (--gpu_users == 0) {
        release((IUnknown *)gpu_manager);
        release((IUnknown *)gpu_device);
        gpu_manager = NULL, gpu_device = NULL;
    }
    ReleaseSRWLockExclusive(&gpu_lock);
}

void bare__media_thread_end(void) {
    bare__media_thread_idle();
    if (mf_ready) MFShutdown();
    if (com_ready) CoUninitialize();
    mf_ready = com_ready = false;
}

/* The shared D3D11 device for Media Foundation, made the first time a video is read (NULL: no GPU). */
static IMFDXGIDeviceManager *gpu(void) {
    AcquireSRWLockExclusive(&gpu_lock);
    if (!gpu_using) gpu_using = true, gpu_users++;
    if (!gpu_manager && !gpu_failed) {
        ID3D11Device *dev = NULL;
        ID3D10Multithread *mt = NULL;
        IMFDXGIDeviceManager *mgr = NULL;
        UINT token = 0;
        if (SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                        D3D11_SDK_VERSION, &dev, NULL, NULL))) {
            if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &k_iid_multithread, (void **)&mt))) {
                ID3D10Multithread_SetMultithreadProtected(mt, TRUE);   /* every decoding thread uses it */
                release((IUnknown *)mt);
            }
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr)) &&
                SUCCEEDED(IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev, token))) {
                gpu_device = dev, gpu_manager = mgr;
                dev = NULL, mgr = NULL;
            }
        }
        release((IUnknown *)mgr);
        release((IUnknown *)dev);
        gpu_failed = !gpu_manager;   /* no GPU decoding here: the CPU from now on */
    }
    IMFDXGIDeviceManager *m = gpu_manager;
    ReleaseSRWLockExclusive(&gpu_lock);
    return m;
}

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

bool bare__media_thumb(const char *path, int size, BareMediaFrame frame, void *user) {
    if (!com_ready || size <= 0) return false;
    wchar_t *wpath = widen(path);
    if (!wpath) return false;
    for (wchar_t *c = wpath; *c; c++) if (*c == L'/') *c = L'\\';   /* the shell wants Windows paths */
    IShellItemImageFactory *f = NULL;
    HBITMAP bmp = NULL;
    unsigned char *px = NULL;
    bool ok = false;
    SIZE want = { size, size };
    BITMAP bm;
    if (SUCCEEDED(SHCreateItemFromParsingName(wpath, NULL, &IID_IShellItemImageFactory, (void **)&f)) &&
        SUCCEEDED(IShellItemImageFactory_GetImage(f, want, SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY, &bmp)) &&
        GetObjectW(bmp, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0 &&
        (px = malloc((size_t)bm.bmWidth * bm.bmHeight * 4)) != NULL) {
        BITMAPINFO bi = { 0 };
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;   /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(NULL);
        if (dc && GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, px, &bi, DIB_RGB_COLORS) == bm.bmHeight) {
            frame(user, px, bm.bmWidth, bm.bmHeight, bm.bmWidth * 4, true, 0);
            ok = true;
        }
        if (dc) ReleaseDC(NULL, dc);
    }
    free(px);
    if (bmp) DeleteObject(bmp);
    release((IUnknown *)f);
    free(wpath);
    return ok;
}

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

/* A reader giving RGB32 frames: with `mgr`, decoded on that GPU and shrunk there to cover want_w x want_h
 * (with some room for the box filter); without, on the CPU at the video's size. NULL if it can't. */
static IMFSourceReader *open_reader(const wchar_t *wpath, IMFDXGIDeviceManager *mgr, int want_w, int want_h) {
    IMFAttributes *attr = NULL;
    IMFSourceReader *r = NULL;
    IMFMediaType *native = NULL, *want = NULL;
    bool ok = false;
    if (FAILED(MFCreateAttributes(&attr, 3))) return NULL;
    if (mgr) {
        IMFAttributes_SetUnknown(attr, &MF_SOURCE_READER_D3D_MANAGER, (IUnknown *)mgr);
        IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        IMFAttributes_SetUINT32(attr, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    } else {
        IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   /* any codec to RGB32 */
    }
    if (SUCCEEDED(MFCreateSourceReaderFromURL(wpath, attr, &r)) &&
        SUCCEEDED(IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) &&
        SUCCEEDED(IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) &&
        SUCCEEDED(MFCreateMediaType(&want)) &&
        SUCCEEDED(IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)) &&
        SUCCEEDED(IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32))) {
        UINT64 size = 0;
        if (mgr && (want_w > 0 || want_h > 0) &&
            SUCCEEDED(IMFSourceReader_GetNativeMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)) &&
            SUCCEEDED(IMFMediaType_GetUINT64(native, &MF_MT_FRAME_SIZE, &size))) {
            double sw = (double)(size >> 32), sh = (double)(size & 0xffffffffu);
            double s = sw > 0 && sh > 0 ? 1.5 * (want_w / sw > want_h / sh ? want_w / sw : want_h / sh) : 1;
            if (s < 1) {
                UINT32 w = ((UINT32)(sw * s) + 1) & ~1u, h = ((UINT32)(sh * s) + 1) & ~1u;
                IMFMediaType_SetUINT64(want, &MF_MT_FRAME_SIZE, (UINT64)w << 32 | h);
                IMFMediaType_SetUINT64(want, &MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
                IMFMediaType_SetUINT32(want, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            }
        }
        ok = SUCCEEDED(IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, want));
        if (!ok && size) {   /* the processor wouldn't shrink it: the video's own size */
            IMFMediaType_DeleteItem(want, &MF_MT_FRAME_SIZE);
            IMFMediaType_DeleteItem(want, &MF_MT_PIXEL_ASPECT_RATIO);
            IMFMediaType_DeleteItem(want, &MF_MT_INTERLACE_MODE);
            ok = SUCCEEDED(IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL,
                                                               want));
        }
    }
    release((IUnknown *)native);
    release((IUnknown *)want);
    release((IUnknown *)attr);
    if (!ok) {
        release((IUnknown *)r);
        return NULL;
    }
    return r;
}

/* Reads the frames; -1 if the first sample failed (so the CPU can try), else how many were given. */
static int read_frames(IMFSourceReader *r, int frames, float fps, BareMediaFrame frame, void *user) {
    int x = 0, y = 0, w = 0, h = 0, rows = 0, got = 0;
    LONG stride = 0;
    if (!frame_size(r, &x, &y, &w, &h, &rows, &stride)) return -1;
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
    bool any = false;
    for (int reads = 0; got < frames && reads < frames * 40; reads++) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample *sample = NULL;
        if (FAILED(IMFSourceReader_ReadSample(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &ts,
                                              &sample))) {
            if (!any) return -1;
            break;
        }
        any = true;
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
                if (!more) frames = got;
            }
            if (locked) IMF2DBuffer_Unlock2D(b2);
            else if (scan) IMFMediaBuffer_Unlock(buf);
            release((IUnknown *)b2);
            release((IUnknown *)buf);
        }
        release((IUnknown *)sample);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    return got;
}

bool bare__media_video(const char *path, int frames, float fps, int want_w, int want_h, BareMediaFrame frame,
                       void *user) {
    if (!mf_ready || frames < 1) return false;
    wchar_t *wpath = widen(path);
    if (!wpath) return false;
    int got = -1;
    IMFDXGIDeviceManager *mgr = gpu();
    IMFSourceReader *r = mgr ? open_reader(wpath, mgr, want_w, want_h) : NULL;
    if (r) got = read_frames(r, frames, fps, frame, user);
    release((IUnknown *)r);
    if (got <= 0 && (r = open_reader(wpath, NULL, 0, 0)) != NULL) {   /* no GPU decoder for it: the CPU */
        got = read_frames(r, frames, fps, frame, user);
        release((IUnknown *)r);
    }
    free(wpath);
    return got > 0;
}
/* ---- sound ------------------------------------------------------------------- */

static bool grow(float **data, size_t *cap, size_t need) {
    if (need <= *cap) return true;
    size_t ncap = *cap ? *cap * 2 : 1u << 20;
    while (ncap < need) ncap *= 2;
    float *g = realloc(*data, ncap * sizeof *g);
    if (!g) return false;
    *data = g, *cap = ncap;
    return true;
}

bool bare__media_audio(const char *path, float **samples, int *frames, int *channels, int *rate) {
    *samples = NULL, *frames = 0, *channels = 0, *rate = 0;
    if (!mf_ready) return false;
    wchar_t *wpath = widen(path);
    if (!wpath) return false;
    IMFSourceReader *r = NULL;
    IMFMediaType *native = NULL, *want = NULL, *got = NULL;
    float *data = NULL;
    size_t len = 0, cap = 0;
    UINT32 ch = 0, hz = 0, nch = 0;
    int out_ch = 0;
    bool ok = false;
    const DWORD audio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(wpath, NULL, &r)) &&
        SUCCEEDED(IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) &&
        SUCCEEDED(IMFSourceReader_SetStreamSelection(r, audio, TRUE)) &&
        SUCCEEDED(IMFSourceReader_GetNativeMediaType(r, audio, 0, &native)) &&
        SUCCEEDED(MFCreateMediaType(&want)) &&
        SUCCEEDED(IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio)) &&
        SUCCEEDED(IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_Float))) {
        /* Surround to stereo where the reader can; otherwise it's mixed down below. */
        if (SUCCEEDED(IMFMediaType_GetUINT32(native, &MF_MT_AUDIO_NUM_CHANNELS, &nch)) && nch > 2)
            IMFMediaType_SetUINT32(want, &MF_MT_AUDIO_NUM_CHANNELS, 2);
        HRESULT hr = IMFSourceReader_SetCurrentMediaType(r, audio, NULL, want);
        if (FAILED(hr) && nch > 2) {
            IMFMediaType_DeleteItem(want, &MF_MT_AUDIO_NUM_CHANNELS);
            hr = IMFSourceReader_SetCurrentMediaType(r, audio, NULL, want);
        }
        if (SUCCEEDED(hr) && SUCCEEDED(IMFSourceReader_GetCurrentMediaType(r, audio, &got)) &&
            SUCCEEDED(IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_NUM_CHANNELS, &ch)) &&
            SUCCEEDED(IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &hz)) && ch > 0 && hz > 0) {
            ok = true;
            out_ch = ch > 1 ? 2 : 1;
            for (;;) {
                DWORD flags = 0;
                IMFSample *sample = NULL;
                if (FAILED(IMFSourceReader_ReadSample(r, audio, 0, NULL, &flags, NULL, &sample))) break;
                IMFMediaBuffer *buf = NULL;
                BYTE *p = NULL;
                DWORD bytes = 0;
                if (sample && SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) &&
                    SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &bytes))) {
                    size_t n = bytes / (sizeof(float) * ch);   /* frames in this sample */
                    if (!grow(&data, &cap, len + n * (size_t)out_ch)) ok = false;
                    const float *in = (const float *)p;
                    for (size_t f = 0; ok && f < n; f++, in += ch) {
                        if (ch <= 2) {
                            for (int c = 0; c < out_ch; c++) data[len++] = in[c];
                        } else {   /* the reader wouldn't mix it: odd channels left, even right */
                            float lr[2] = { 0, 0 };
                            for (UINT32 c = 0; c < ch; c++) lr[c & 1] += in[c];
                            data[len++] = lr[0] * 2.0f / (float)ch;
                            data[len++] = lr[1] * 2.0f / (float)ch;
                        }
                    }
                    IMFMediaBuffer_Unlock(buf);
                }
                release((IUnknown *)buf);
                release((IUnknown *)sample);
                if (!ok || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) break;
            }
        }
    }
    release((IUnknown *)got);
    release((IUnknown *)want);
    release((IUnknown *)native);
    release((IUnknown *)r);
    free(wpath);
    if (!ok || !len) { free(data); return false; }
    *samples = data, *frames = (int)(len / (size_t)out_ch), *channels = out_ch, *rate = (int)hz;
    return true;
}

/* ---- video, a frame at a time ------------------------------------------------ */

struct BareMediaReader {
    IMFSourceReader *r;
    int x, y, w, h, rows;
    LONG stride;
    double duration;
    float fps;
};

BareMediaReader *bare__media_reader_open(const char *path, bool use_gpu, int want_w, int want_h) {
    if (!mf_ready) return NULL;
    IMFDXGIDeviceManager *mgr = use_gpu ? gpu() : NULL;
    if (use_gpu && !mgr) return NULL;
    wchar_t *wpath = widen(path);
    if (!wpath) return NULL;
    IMFSourceReader *r = open_reader(wpath, mgr, use_gpu ? want_w : 0, use_gpu ? want_h : 0);
    free(wpath);
    if (!r) return NULL;
    BareMediaReader *m = calloc(1, sizeof *m);
    if (!m || !frame_size(r, &m->x, &m->y, &m->w, &m->h, &m->rows, &m->stride)) {
        free(m);
        release((IUnknown *)r);
        return NULL;
    }
    m->r = r;
    PROPVARIANT dur;
    PropVariantInit(&dur);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(r, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION,
                                                           &dur)) && dur.vt == VT_UI8)
        m->duration = (double)dur.uhVal.QuadPart / 1e7;
    PropVariantClear(&dur);
    IMFMediaType *t = NULL;
    UINT64 rate = 0;
    if (SUCCEEDED(IMFSourceReader_GetNativeMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &t)) &&
        SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_RATE, &rate)) && (rate & 0xffffffffu))
        m->fps = (float)((double)(rate >> 32) / (double)(rate & 0xffffffffu));
    release((IUnknown *)t);
    return m;
}

void bare__media_reader_info(BareMediaReader *m, double *duration, int *width, int *height, float *fps) {
    if (duration) *duration = m ? m->duration : 0;
    if (width) *width = m ? m->w : 0;
    if (height) *height = m ? m->h : 0;
    if (fps) *fps = m ? m->fps : 0;
}

bool bare__media_reader_seek(BareMediaReader *m, double seconds) {
    if (!m) return false;
    PROPVARIANT at;
    PropVariantInit(&at);
    at.vt = VT_I8;
    at.hVal.QuadPart = (LONGLONG)((seconds > 0 ? seconds : 0) * 1e7);
    return SUCCEEDED(IMFSourceReader_SetCurrentPosition(m->r, &GUID_NULL, &at));
}

int bare__media_reader_next(BareMediaReader *m, BareMediaFrame frame, void *user, double *time) {
    if (!m) return -1;
    for (int tries = 0; tries < 64; tries++) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample *sample = NULL;
        if (FAILED(IMFSourceReader_ReadSample(m->r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &ts,
                                              &sample)))
            return -1;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            frame_size(m->r, &m->x, &m->y, &m->w, &m->h, &m->rows, &m->stride);
        if (!sample) {
            if (flags & MF_SOURCE_READERF_ERROR) return -1;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return 0;
            continue;
        }
        int given = -1;
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
                    pitch = m->stride ? m->stride : m->w * 4;
                    if (pitch < 0) scan += (size_t)(-pitch) * (size_t)(m->rows - 1);   /* bottom-up: the top row */
                    if ((DWORD)labs(pitch) * (DWORD)m->rows > len) { IMFMediaBuffer_Unlock(buf); scan = NULL; }
                }
            }
            if (scan) {
                if (time) *time = (double)ts / 1e7;
                frame(user, scan + (ptrdiff_t)pitch * m->y + (ptrdiff_t)m->x * 4, m->w, m->h, (int)pitch, true, 0);
                given = 1;
            }
            if (locked) IMF2DBuffer_Unlock2D(b2);
            else if (scan) IMFMediaBuffer_Unlock(buf);
            release((IUnknown *)b2);
            release((IUnknown *)buf);
        }
        release((IUnknown *)sample);
        return given;
    }
    return -1;
}

void bare__media_reader_close(BareMediaReader *m) {
    if (!m) return;
    release((IUnknown *)m->r);
    free(m);
}

#else
#include "media.h"
void bare__media_thread_begin(void) {}
void bare__media_thread_end(void) {}
void bare__media_thread_idle(void) {}
bool bare__media_thumb(const char *path, int size, BareMediaFrame frame, void *user) {
    (void)path, (void)size, (void)frame, (void)user;
    return false;
}
bool bare__media_image(const char *path, BareMediaFrame frame, void *user) {
    (void)path, (void)frame, (void)user;
    return false;
}
bool bare__media_video(const char *path, int frames, float fps, int want_w, int want_h, BareMediaFrame frame,
                       void *user) {
    (void)path, (void)frames, (void)fps, (void)want_w, (void)want_h, (void)frame, (void)user;
    return false;
}
bool bare__media_audio(const char *path, float **samples, int *frames, int *channels, int *rate) {
    (void)path;
    *samples = NULL, *frames = *channels = *rate = 0;
    return false;
}
BareMediaReader *bare__media_reader_open(const char *path, bool gpu, int want_w, int want_h) {
    (void)path, (void)gpu, (void)want_w, (void)want_h;
    return NULL;
}
void bare__media_reader_info(BareMediaReader *r, double *duration, int *width, int *height, float *fps) {
    (void)r;
    if (duration) *duration = 0;
    if (width) *width = 0;
    if (height) *height = 0;
    if (fps) *fps = 0;
}
bool bare__media_reader_seek(BareMediaReader *r, double seconds) { (void)r, (void)seconds; return false; }
int  bare__media_reader_next(BareMediaReader *r, BareMediaFrame frame, void *user, double *time) {
    (void)r, (void)frame, (void)user, (void)time;
    return -1;
}
void bare__media_reader_close(BareMediaReader *r) { (void)r; }
#endif
