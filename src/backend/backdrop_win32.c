/* backdrop_win32.c: the live desktop behind the window (bare_window_backdrop).
 *
 * DWM can blur what's behind a window, but nothing can refract it: the pixels
 * never reach the app. So the window asks to be left out of screen capture
 * (WDA_EXCLUDEFROMCAPTURE, Windows 10 2004+), which makes DXGI desktop
 * duplication of its monitor show what's really under it, and a worker
 * thread copies the part under the client area out as RGBA. The app draws it
 * as the window's background and glass bends the real desktop.
 *
 * The thread keeps the last whole desktop on the GPU, so moving the window
 * over a still desktop only copies the new region; a live desktop (a video
 * wallpaper) gives new frames at up to MAX_FPS. Minimised or hidden windows
 * cost nothing.
 *
 * Screenshots: an excluded window is missing from them, so a keyboard hook
 * watches for the screenshot keys (PrtScn with any modifier, Win+Shift+S) and
 * lets the window be captured for a few seconds, holding the last backdrop so
 * the glass doesn't see itself. Screen sharing still leaves it out. */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "backdrop.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif

#define MAX_FPS 60          /* a live desktop */
#define MOVING_FPS 120      /* while the window moves */
#define SHOT_MS 4000        /* how long a screenshot key leaves the window capturable */

struct BareBackdrop {
    HWND hwnd;
    void (*wake)(void *user);
    void *user;
    HANDLE thread;
    volatile LONG stop;
    HANDLE shot_thread;           /* the screenshot-key hook's thread */
    DWORD shot_tid;
    volatile LONG visible;        /* capturable now (a screenshot key was pressed) */
    volatile LONG64 visible_until;
    SRWLOCK lock;
    unsigned char *buf[3];   /* back (worker), ready, front (app) */
    int w[3], h[3];
    int fresh;               /* ready holds a picture the app hasn't taken */
    /* worker only */
    ID3D11Device *dev;
    ID3D11DeviceContext *dc;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D *full, *staging;
    RECT output;             /* the duplicated monitor, desktop pixels */
    int full_w, full_h, stage_w, stage_h;
    DXGI_FORMAT format;
};

static void release(IUnknown *p) { if (p) p->lpVtbl->Release(p); }

static void drop_duplication(BareBackdrop *b) {
    release((IUnknown *)b->staging); b->staging = NULL;
    release((IUnknown *)b->full); b->full = NULL;
    release((IUnknown *)b->dup); b->dup = NULL;
    release((IUnknown *)b->dc); b->dc = NULL;
    release((IUnknown *)b->dev); b->dev = NULL;
    b->stage_w = b->stage_h = 0;
}

/* Duplicates the monitor the window is mostly on. */
static bool open_duplication(BareBackdrop *b) {
    drop_duplication(b);
    HMONITOR mon = MonitorFromWindow(b->hwnd, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return false;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIOutput *output = NULL;
    for (UINT a = 0; !output && SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, a, &adapter)); a++) {
        IDXGIOutput *o = NULL;
        for (UINT i = 0; SUCCEEDED(IDXGIAdapter1_EnumOutputs(adapter, i, &o)); i++) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(IDXGIOutput_GetDesc(o, &desc)) && desc.Monitor == mon) {
                output = o;
                b->output = desc.DesktopCoordinates;
                break;
            }
            release((IUnknown *)o);
        }
        if (!output) { release((IUnknown *)adapter); adapter = NULL; }
    }
    release((IUnknown *)factory);
    if (!output) return false;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &b->dev, &level, &b->dc);
    release((IUnknown *)adapter);
    IDXGIOutput1 *output1 = NULL;
    if (SUCCEEDED(hr)) hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
    release((IUnknown *)output);
    if (SUCCEEDED(hr)) hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)b->dev, &b->dup);
    release((IUnknown *)output1);
    if (FAILED(hr)) { drop_duplication(b); return false; }
    return true;
}

/* Takes the newest desktop frame into b->full (false: none, or lost). */
static bool next_frame(BareBackdrop *b, UINT wait_ms, bool *lost) {
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *res = NULL;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(b->dup, wait_ms, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) { *lost = true; return false; }
    bool got = false;
    ID3D11Texture2D *tex = NULL;
    if (info.LastPresentTime.QuadPart != 0 &&
        SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(tex, &d);
        if (!b->full || (int)d.Width != b->full_w || (int)d.Height != b->full_h || d.Format != b->format) {
            release((IUnknown *)b->full); b->full = NULL;
            release((IUnknown *)b->staging); b->staging = NULL;
            b->stage_w = b->stage_h = 0;
            D3D11_TEXTURE2D_DESC f = d;
            f.Usage = D3D11_USAGE_DEFAULT;
            f.BindFlags = 0;
            f.CPUAccessFlags = 0;
            f.MiscFlags = 0;
            f.MipLevels = f.ArraySize = 1;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(b->dev, &f, NULL, &b->full))) {
                b->full_w = (int)d.Width, b->full_h = (int)d.Height, b->format = d.Format;
            }
        }
        if (b->full) {
            ID3D11DeviceContext_CopyResource(b->dc, (ID3D11Resource *)b->full, (ID3D11Resource *)tex);
            got = true;
        }
        release((IUnknown *)tex);
    }
    release((IUnknown *)res);
    IDXGIOutputDuplication_ReleaseFrame(b->dup);
    return got;
}

/* Copies the client area's part of the desktop into the back buffer as RGBA. */
static bool copy_region(BareBackdrop *b, RECT client) {
    int w = client.right - client.left, h = client.bottom - client.top;
    if (w <= 0 || h <= 0 || !b->full) return false;
    if (w > 16384 || h > 16384) return false;
    if (!b->staging || w != b->stage_w || h != b->stage_h) {
        release((IUnknown *)b->staging); b->staging = NULL;
        D3D11_TEXTURE2D_DESC s = { 0 };
        s.Width = (UINT)w; s.Height = (UINT)h; s.MipLevels = 1; s.ArraySize = 1; s.Format = b->format;
        s.SampleDesc.Count = 1; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(b->dev, &s, NULL, &b->staging))) return false;
        b->stage_w = w, b->stage_h = h;
    }
    /* the window in the monitor's pixels, clipped to it */
    int x0 = client.left - b->output.left, y0 = client.top - b->output.top;
    int sx0 = x0 < 0 ? 0 : x0, sy0 = y0 < 0 ? 0 : y0;
    int sx1 = x0 + w > b->full_w ? b->full_w : x0 + w, sy1 = y0 + h > b->full_h ? b->full_h : y0 + h;
    if (sx1 > sx0 && sy1 > sy0) {
        D3D11_BOX box = { (UINT)sx0, (UINT)sy0, 0, (UINT)sx1, (UINT)sy1, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(b->dc, (ID3D11Resource *)b->staging, 0, (UINT)(sx0 - x0),
                                                  (UINT)(sy0 - y0), 0, (ID3D11Resource *)b->full, 0, &box);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(b->dc, (ID3D11Resource *)b->staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    size_t bytes = (size_t)w * (size_t)h * 4;
    if (!b->buf[0] || b->w[0] * b->h[0] < w * h) {
        free(b->buf[0]);
        b->buf[0] = malloc(bytes);
    }
    bool ok = b->buf[0] != NULL;
    if (ok) {
        bool bgra = b->format == DXGI_FORMAT_B8G8R8A8_UNORM || b->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (int y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)((const unsigned char *)m.pData + (size_t)y * m.RowPitch);
            uint32_t *dst = (uint32_t *)(b->buf[0] + (size_t)y * w * 4);
            bool inside = y + y0 >= sy0 && y + y0 < sy1;
            for (int x = 0; x < w; x++) {
                if (!inside || x + x0 < sx0 || x + x0 >= sx1) { dst[x] = 0xff000000u; continue; }
                uint32_t v = src[x];
                dst[x] = bgra ? ((v & 0x0000ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16) | 0xff000000u)
                              : (v | 0xff000000u);
            }
        }
        b->w[0] = w, b->h[0] = h;
    }
    ID3D11DeviceContext_Unmap(b->dc, (ID3D11Resource *)b->staging, 0);
    return ok;
}

static void publish(BareBackdrop *b) {
    AcquireSRWLockExclusive(&b->lock);
    unsigned char *p = b->buf[1]; int w = b->w[1], h = b->h[1];
    b->buf[1] = b->buf[0], b->w[1] = b->w[0], b->h[1] = b->h[0];
    b->buf[0] = p, b->w[0] = w, b->h[0] = h;
    b->fresh = 1;
    ReleaseSRWLockExclusive(&b->lock);
    if (b->wake) b->wake(b->user);
}

static DWORD WINAPI worker(void *arg) {
    BareBackdrop *b = arg;
    RECT last = { 0 };
    HMONITOR last_mon = NULL;
    ULONGLONG last_publish = 0, moved_at = 0;
    bool dirty = true, held = false;
    while (!b->stop) {
        if (!IsWindowVisible(b->hwnd) || IsIconic(b->hwnd)) { Sleep(100); dirty = true; continue; }
        /* While a screenshot may be taken the duplication shows this window: hold the last picture,
         * and afterwards start over from a whole new frame (the old one may contain the window). */
        if (b->visible) { held = true; Sleep(15); continue; }
        if (held) { held = false; drop_duplication(b); last_mon = NULL; dirty = true; Sleep(30); continue; }
        HMONITOR mon = MonitorFromWindow(b->hwnd, MONITOR_DEFAULTTONEAREST);
        if (!b->dup || mon != last_mon) {
            if (!open_duplication(b)) { Sleep(500); continue; }   /* lock screen, UAC, a mode change */
            last_mon = mon;
            dirty = true;
        }
        bool lost = false;
        bool moving = GetTickCount64() - moved_at < 500;
        if (next_frame(b, dirty ? 0 : moving ? 1 : 15, &lost)) dirty = true;
        if (lost) { drop_duplication(b); last_mon = NULL; Sleep(50); continue; }
        RECT client;
        GetClientRect(b->hwnd, &client);
        MapWindowPoints(b->hwnd, NULL, (POINT *)&client, 2);
        if (memcmp(&client, &last, sizeof client)) { dirty = true; moved_at = GetTickCount64(); moving = true; }
        if (!dirty) continue;
        ULONGLONG now = GetTickCount64(), gap = 1000 / (moving ? MOVING_FPS : MAX_FPS);
        if (now - last_publish < gap) Sleep((DWORD)(gap - (now - last_publish)));
        if (copy_region(b, client)) {
            publish(b);
            last = client;
            last_publish = GetTickCount64();
            dirty = false;
        }
    }
    drop_duplication(b);
    return 0;
}

/* ---- screenshots ---------------------------------------------------------- */

static BareBackdrop *volatile g_shot;   /* the window the hook serves (Bare has one) */

static LRESULT CALLBACK shot_key(int code, WPARAM wp, LPARAM lp) {
    BareBackdrop *b = g_shot;
    if (code == HC_ACTION && b && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        bool win = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if ((k->vkCode == VK_SNAPSHOT || (win && shift && k->vkCode == 'S')) && !IsIconic(b->hwnd)) {
            InterlockedExchange64(&b->visible_until, (LONG64)GetTickCount64() + SHOT_MS);
            if (!InterlockedExchange(&b->visible, 1)) {
                SetWindowDisplayAffinity(b->hwnd, WDA_NONE);
                DwmFlush();   /* composed capturable before the key reaches the screenshot tool */
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI shot_main(void *arg) {
    BareBackdrop *b = arg;
    MSG m;
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);   /* a queue for PostThreadMessage */
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, shot_key, GetModuleHandleW(NULL), 0);
    for (;;) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, b->visible ? 100 : INFINITE, QS_ALLINPUT);
        bool quit = false;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) quit = true;
            DispatchMessageW(&m);
        }
        if (b->visible && (quit || (LONG64)GetTickCount64() >= b->visible_until)) {
            if (!quit) SetWindowDisplayAffinity(b->hwnd, WDA_EXCLUDEFROMCAPTURE);
            InterlockedExchange(&b->visible, 0);
        }
        if (quit) break;
    }
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

BareBackdrop *bare__backdrop_start(void *hwnd, void (*wake)(void *user), void *user) {
    if (!hwnd) return NULL;
    /* Without the exclusion the window would capture itself, glass in glass. */
    if (!SetWindowDisplayAffinity((HWND)hwnd, WDA_EXCLUDEFROMCAPTURE)) return NULL;
    BareBackdrop *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->hwnd = (HWND)hwnd;
    b->wake = wake;
    b->user = user;
    InitializeSRWLock(&b->lock);
    b->thread = CreateThread(NULL, 0, worker, b, 0, NULL);
    if (!b->thread) {
        SetWindowDisplayAffinity((HWND)hwnd, WDA_NONE);
        free(b);
        return NULL;
    }
    g_shot = b;
    b->shot_thread = CreateThread(NULL, 0, shot_main, b, 0, &b->shot_tid);
    return b;
}

const void *bare__backdrop_take(BareBackdrop *b, int *width, int *height) {
    if (!b) return NULL;
    const void *out = NULL;
    AcquireSRWLockExclusive(&b->lock);
    if (b->fresh) {
        unsigned char *p = b->buf[2]; int w = b->w[2], h = b->h[2];
        b->buf[2] = b->buf[1], b->w[2] = b->w[1], b->h[2] = b->h[1];
        b->buf[1] = p, b->w[1] = w, b->h[1] = h;
        b->fresh = 0;
        out = b->buf[2];
        *width = b->w[2], *height = b->h[2];
    }
    ReleaseSRWLockExclusive(&b->lock);
    return out;
}

void bare__backdrop_stop(BareBackdrop *b) {
    if (!b) return;
    InterlockedExchange(&b->stop, 1);
    if (g_shot == b) g_shot = NULL;
    if (b->shot_thread) {
        PostThreadMessageW(b->shot_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(b->shot_thread, 2000);
        CloseHandle(b->shot_thread);
    }
    WaitForSingleObject(b->thread, 2000);
    CloseHandle(b->thread);
    if (IsWindow(b->hwnd)) SetWindowDisplayAffinity(b->hwnd, WDA_NONE);
    for (int i = 0; i < 3; i++) free(b->buf[i]);
    free(b);
}
#endif
