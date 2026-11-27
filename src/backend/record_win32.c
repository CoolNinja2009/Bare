/* record_win32.c: BARE_RECORD's encoder. Frames go to Media Foundation's sink
 * writer as RGB32 and come out as H.264 in an MP4; the writer converts the
 * colours and picks the encoder (the GPU's when there is one). Nothing to
 * install: it comes with Windows. */
#include "record.h"

#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct BareRecorder {
    IMFSinkWriter *writer;
    DWORD stream;
    int width, height, fps;
    long frames;
    bool com, mf;
};

static void fail(BareRecorder *r, const char *what, HRESULT hr) {
    fprintf(stderr, "bare: recording: %s failed (0x%08lx)\n", what, (unsigned long)hr);
    bare__record_close(r);
}

static HRESULT set_size(IMFMediaType *t, const GUID *key, UINT32 a, UINT32 b) {
    return IMFMediaType_SetUINT64(t, key, ((UINT64)a << 32) | b);
}

BareRecorder *bare__record_open(const char *path, int width, int height, int fps) {
    BareRecorder *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->width = width, r->height = height, r->fps = fps;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    r->com = SUCCEEDED(hr);   /* RPC_E_CHANGED_MODE: the thread already has COM, which is fine */
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { fail(r, "MFStartup", hr); return NULL; }
    r->mf = true;

    wchar_t wpath[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0]))) {
        fail(r, "the path", E_INVALIDARG);
        return NULL;
    }
    IMFAttributes *attrs = NULL;
    hr = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(wpath, NULL, attrs, &r->writer);
    if (attrs) IMFAttributes_Release(attrs);
    if (FAILED(hr)) { fail(r, "creating the file", hr); return NULL; }

    /* Out: H.264 High, at a bitrate that keeps fine UI detail (thin text, small dots) sharp. */
    IMFMediaType *out = NULL;
    hr = MFCreateMediaType(&out);
    UINT32 bitrate = (UINT32)((double)width * height * fps * 0.4);
    if (bitrate < 2000000) bitrate = 2000000;
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(out, &MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(out, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(out, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = set_size(out, &MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    if (SUCCEEDED(hr)) hr = set_size(out, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    if (SUCCEEDED(hr)) hr = set_size(out, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(r->writer, out, &r->stream);
    if (out) IMFMediaType_Release(out);
    if (FAILED(hr)) { fail(r, "the H.264 stream", hr); return NULL; }

    /* In: BGRA rows, top row first (a positive stride; RGB32 is bottom-up unless told). */
    IMFMediaType *in = NULL;
    hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_DEFAULT_STRIDE, (UINT32)(width * 4));
    if (SUCCEEDED(hr)) hr = set_size(in, &MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    if (SUCCEEDED(hr)) hr = set_size(in, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    if (SUCCEEDED(hr)) hr = set_size(in, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(r->writer, r->stream, in, NULL);
    if (in) IMFMediaType_Release(in);
    if (FAILED(hr)) { fail(r, "the encoder's input", hr); return NULL; }

    hr = IMFSinkWriter_BeginWriting(r->writer);
    if (FAILED(hr)) { fail(r, "BeginWriting", hr); return NULL; }
    return r;
}

bool bare__record_frame(BareRecorder *r, const void *pixels, int pitch, bool bgra) {
    if (!r || !r->writer) return false;
    DWORD row = (DWORD)r->width * 4, size = row * (DWORD)r->height;
    IMFMediaBuffer *buf = NULL;
    IMFSample *sample = NULL;
    BYTE *dst = NULL;
    HRESULT hr = MFCreateMemoryBuffer(size, &buf);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buf, &dst, NULL, NULL);
    if (SUCCEEDED(hr)) {
        for (int y = 0; y < r->height; y++) {
            const unsigned char *s = (const unsigned char *)pixels + (size_t)y * (size_t)pitch;
            BYTE *d = dst + (size_t)y * row;
            if (bgra) {
                memcpy(d, s, row);
            } else {
                for (int x = 0; x < r->width; x++, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
            }
        }
        IMFMediaBuffer_Unlock(buf);
        hr = IMFMediaBuffer_SetCurrentLength(buf, size);
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(sample, buf);
    /* Times in 100 ns units, from the frame count, so the video's clock is exact. */
    LONGLONG t0 = r->frames * 10000000LL / r->fps, t1 = (r->frames + 1) * 10000000LL / r->fps;
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(sample, t0);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(sample, t1 - t0);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(r->writer, r->stream, sample);
    if (sample) IMFSample_Release(sample);
    if (buf) IMFMediaBuffer_Release(buf);
    if (FAILED(hr)) {
        fprintf(stderr, "bare: recording: frame %ld failed (0x%08lx)\n", r->frames, (unsigned long)hr);
        return false;
    }
    r->frames++;
    return true;
}

long bare__record_close(BareRecorder *r) {
    if (!r) return 0;
    long frames = r->frames;
    if (r->writer) {
        HRESULT hr = IMFSinkWriter_Finalize(r->writer);   /* writes the MP4's index: without it the file won't play */
        if (FAILED(hr)) fprintf(stderr, "bare: recording: finishing the file failed (0x%08lx)\n", (unsigned long)hr);
        IMFSinkWriter_Release(r->writer);
    }
    if (r->mf) MFShutdown();
    if (r->com) CoUninitialize();
    free(r);
    return frames;
}

#else
#include <stdio.h>
BareRecorder *bare__record_open(const char *path, int width, int height, int fps) {
    (void)path, (void)width, (void)height, (void)fps;
    fprintf(stderr, "bare: BARE_RECORD isn't supported on this system yet (Windows only)\n");
    return NULL;
}
bool bare__record_frame(BareRecorder *r, const void *pixels, int pitch, bool bgra) {
    (void)r, (void)pixels, (void)pitch, (void)bgra;
    return false;
}
long bare__record_close(BareRecorder *r) { (void)r; return 0; }
#endif
