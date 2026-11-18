// ==WindhawkMod==
// @id              edge-fullscreen-zoom
// @name            Edge fullscreen zoom
// @description     Animates Edge entering and leaving fullscreen (video fullscreen, F11) with a zoom and crossfade
// @version         1.2
// @author          local
// @include         msedge.exe
// @architecture    x86-64
// @compilerOptions -ldwmapi -luser32 -lgdi32
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- duration: 360
  $name: Duration (ms)
*/
// ==/WindhawkModSettings==

// Chromium enters fullscreen by removing WS_CAPTION | WS_THICKFRAME with SetWindowLong and
// then calling SetWindowPos(monitor rect, SWP_FRAMECHANGED). Leaving does the reverse. Both are
// one-step resizes, so Windows never animates them.
//
// v1.1 design: everything that moves is a DWM thumbnail, scaled by the GPU.
//  - Before the style change, a screenshot of the window as it looks now.
//  - In the SetWindowPos call, on the browser's UI thread and without waiting on any other
//    thread: a cloaked layered "still" window at the old place holding the screenshot, and a
//    full-monitor host with no redirection bitmap (fully see-through) carrying two thumbnails,
//    the still below and the live browser window above at opacity 0. The browser window is
//    cloaked and resized.
//  - A worker thread only calls DwmUpdateThumbnailProperties and DwmFlush: both thumbnails
//    travel from the old rect to the new one while the live one fades in over the still.
//    At the end it uncloaks the browser and posts cleanup to the host.
//  - A timer on the host forces the same ending if the worker is ever late, so the browser
//    can never stay hidden.
// The helper windows are created already visible and are tool windows, so they never go
// through ShowWindow or SetWindowPos (other mods hook those).

#include <windows.h>
#include <dwmapi.h>
#include <atomic>
#include <mutex>
#include <cmath>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

constexpr LONG kFrameBits = WS_CAPTION | WS_THICKFRAME;
constexpr wchar_t kHostClass[] = L"EdgeFullscreenZoomHost";
constexpr wchar_t kStillClass[] = L"EdgeFullscreenZoomStill";
constexpr UINT kMsgCleanup = WM_APP + 0x51;
constexpr UINT_PTR kSafetyTimer = 1;

std::atomic<int> g_duration{360};
thread_local bool t_inHook = false;
HINSTANCE g_inst = nullptr;

struct Pending {
    HWND hwnd = nullptr;
    bool enter = false;
    RECT from{};
    HBITMAP snap = nullptr;
    DWORD tick = 0;
};

struct Anim {
    HWND target = nullptr;
    bool enter = false;
    RECT from{};      // screen coords
    RECT monitor{};   // host rect, screen coords
    HBITMAP snap = nullptr;
    HWND host = nullptr;
    HWND still = nullptr;
    HTHUMBNAIL thStill = nullptr;
    HTHUMBNAIL thLive = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> abort{false};
    std::atomic<bool> finished{false};  // worker reached its end
    bool cleaned = false;
    LARGE_INTEGER t0{};
};

std::mutex g_mx;
Pending g_pending;
Anim* g_anim = nullptr;

int W(const RECT& r) { return r.right - r.left; }
int H(const RECT& r) { return r.bottom - r.top; }

double Ms(const LARGE_INTEGER& since) {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return (n.QuadPart - since.QuadPart) * 1000.0 / f.QuadPart;
}

RECT VisibleRect(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
        W(r) <= 0) {
        GetWindowRect(h, &r);
    }
    return r;
}

bool MonitorRect(HWND h, RECT* out) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    *out = mi.rcMonitor;
    return true;
}

bool IsOurBrowserFrame(HWND h) {
    if (!h || GetAncestor(h, GA_ROOT) != h) return false;
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || tid != GetCurrentThreadId()) return false;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return false;
    return wcscmp(cls, L"Chrome_WidgetWin_1") == 0;
}

HBITMAP Screenshot(const RECT& r) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    HBITMAP bmp = CreateCompatibleBitmap(screen, W(r), H(r));
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, W(r), H(r), screen, r.left, r.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    ReleaseDC(nullptr, screen);
    return bmp;
}

void SetCloak(HWND h, BOOL on) { DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof(on)); }

void PaintStill(HWND still, HBITMAP snap) {
    if (!still || !snap) return;
    HDC dc = GetDC(still);
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, snap);
    BITMAP bm{};
    GetObjectW(snap, sizeof(bm), &bm);
    BitBlt(dc, 0, 0, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(still, dc);
    GdiFlush();
}

// UI thread only (owner of the helper windows). Idempotent.
void Cleanup(Anim* a) {
    if (a->cleaned) return;
    a->cleaned = true;
    SetCloak(a->target, FALSE);
    if (a->thLive) DwmUnregisterThumbnail(a->thLive);
    if (a->thStill) DwmUnregisterThumbnail(a->thStill);
    a->thLive = a->thStill = nullptr;
    if (a->host) {
        KillTimer(a->host, kSafetyTimer);
        SetWindowLongPtrW(a->host, GWLP_USERDATA, 0);
        DestroyWindow(a->host);
    }
    if (a->still) DestroyWindow(a->still);
    a->host = a->still = nullptr;
    if (a->snap) DeleteObject(a->snap);
    a->snap = nullptr;
}

// Detaches and frees an animation. UI thread only.
void Release(Anim* a, DWORD waitMs = 1000) {
    {
        std::lock_guard<std::mutex> lock(g_mx);
        if (g_anim == a) g_anim = nullptr;
    }
    a->abort = true;
    // The worker only talks to DWM; it never waits on this thread.
    bool joined = !a->thread || WaitForSingleObject(a->thread, waitMs) == WAIT_OBJECT_0;
    Cleanup(a);
    if (!joined) {
        Wh_Log(L"worker did not stop; leaking its state");
        return;
    }
    if (a->thread) CloseHandle(a->thread);
    delete a;
}

LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgCleanup || (msg == WM_TIMER && wp == kSafetyTimer)) {
        Anim* a = reinterpret_cast<Anim*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (a) {
            if (msg == WM_TIMER) Wh_Log(L"safety timer ended the animation");
            Release(a, msg == WM_TIMER ? 100 : 1000);
        }
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK StillProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

void RegisterClasses() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSW wc{};
        wc.hInstance = g_inst;
        wc.lpfnWndProc = HostProc;
        wc.lpszClassName = kHostClass;
        RegisterClassW(&wc);
        wc.lpfnWndProc = StillProc;
        wc.lpszClassName = kStillClass;
        RegisterClassW(&wc);
    });
}

// Ease out with a long, soft landing (close to cubic-bezier(0.22, 1, 0.36, 1)).
double Ease(double t) {
    double u = 1.0 - t;
    return 1.0 - u * u * u * u * u * 0.35 - u * u * u * 0.65;
}

double SmoothStep(double a, double b, double x) {
    if (x <= a) return 0;
    if (x >= b) return 1;
    double t = (x - a) / (b - a);
    return t * t * (3 - 2 * t);
}

RECT Lerp(const RECT& a, const RECT& b, double e) {
    auto l = [e](LONG x, LONG y) { return (LONG)std::lround(x + (y - x) * e); };
    return RECT{l(a.left, b.left), l(a.top, b.top), l(a.right, b.right), l(a.bottom, b.bottom)};
}

RECT ToHost(const Anim* a, RECT r) {
    OffsetRect(&r, -a->monitor.left, -a->monitor.top);
    return r;
}

void ShowThumb(HTHUMBNAIL th, const RECT& dest, const RECT* src, BYTE opacity) {
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE |
                DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcDestination = dest;
    if (src) {
        p.dwFlags |= DWM_TNP_RECTSOURCE;
        p.rcSource = *src;
    }
    p.opacity = opacity;
    p.fVisible = opacity > 0;
    p.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(th, &p);
}

// Live thumbnail of the visible part of the browser window (no invisible resize borders).
void ShowLive(Anim* a, const RECT& destScreen, BYTE opacity) {
    RECT wr{};
    GetWindowRect(a->target, &wr);
    RECT vis = VisibleRect(a->target);
    RECT src{vis.left - wr.left, vis.top - wr.top, vis.right - wr.left, vis.bottom - wr.top};
    ShowThumb(a->thLive, ToHost(a, destScreen), &src, opacity);
}

DWORD WINAPI Worker(void* pv) {
    Anim* a = static_cast<Anim*>(pv);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const double duration = (double)g_duration.load();
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int frames = 0;
    while (!a->abort) {
        QueryPerformanceCounter(&now);
        double t = (now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart / duration;
        if (t >= 1.0) break;
        RECT dst = VisibleRect(a->target);
        RECT r = Lerp(a->from, dst, Ease(t));
        // The browser needs a few frames to draw at its new size; keep the still on top of
        // that, then let the live window take over.
        BYTE live = (BYTE)std::lround(255.0 * SmoothStep(0.18, 0.62, t));
        ShowThumb(a->thStill, ToHost(a, r), nullptr, live == 255 ? 0 : 255);
        ShowLive(a, r, live);
        DwmFlush();
        frames++;
    }
    if (!a->abort) {
        ShowThumb(a->thStill, RECT{}, nullptr, 0);
        ShowLive(a, VisibleRect(a->target), 255);
        DwmFlush();
        // The thumbnail now matches the real window pixel for pixel: swap them.
        SetCloak(a->target, FALSE);
        DwmFlush();
        DwmFlush();
    }
    Wh_Log(L"%s done: %d frames in %.0f ms (since hook %.0f ms)", a->enter ? L"enter" : L"leave",
           frames, (now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart, Ms(a->t0));
    a->finished = true;
    PostMessageW(a->host, kMsgCleanup, 0, 0);
    return 0;
}

// UI thread. Builds the helpers and thumbnails, showing the still at the old place.
Anim* Prepare(const Pending& p) {
    RegisterClasses();
    Anim* a = new Anim();
    QueryPerformanceCounter(&a->t0);
    a->target = p.hwnd;
    a->enter = p.enter;
    a->from = p.from;
    a->snap = p.snap;
    MonitorRect(p.hwnd, &a->monitor);
    UnionRect(&a->monitor, &a->monitor, &a->from);

    const DWORD toolEx = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    // Layered without attributes: not drawn until SetLayeredWindowAttributes, by which time
    // it is cloaked, so it never shows itself.
    a->still = CreateWindowExW(toolEx | WS_EX_LAYERED, kStillClass, L"", WS_POPUP | WS_VISIBLE,
                               a->from.left, a->from.top, W(a->from), H(a->from), nullptr,
                               nullptr, g_inst, nullptr);
    if (a->still) {
        SetCloak(a->still, TRUE);
        BOOL off = TRUE;
        DwmSetWindowAttribute(a->still, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        SetLayeredWindowAttributes(a->still, 0, 255, LWA_ALPHA);
        PaintStill(a->still, a->snap);
    }
    a->host = CreateWindowExW(toolEx | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kHostClass, L"",
                              WS_POPUP | WS_VISIBLE, a->monitor.left, a->monitor.top,
                              W(a->monitor), H(a->monitor), nullptr, nullptr, g_inst, nullptr);
    if (a->host) {
        BOOL off = TRUE;
        DwmSetWindowAttribute(a->host, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        SetWindowLongPtrW(a->host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
    }
    bool ok = a->still && a->host &&
              SUCCEEDED(DwmRegisterThumbnail(a->host, a->still, &a->thStill)) &&
              SUCCEEDED(DwmRegisterThumbnail(a->host, a->target, &a->thLive));
    if (!ok) {
        Wh_Log(L"helpers failed: still=%p host=%p", a->still, a->host);
        Cleanup(a);
        delete a;
        return nullptr;
    }
    ShowThumb(a->thStill, ToHost(a, a->from), nullptr, 255);
    ShowLive(a, a->from, 0);
    SetTimer(a->host, kSafetyTimer, g_duration.load() + 700, nullptr);
    return a;
}

void ClearPending() {
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_pending.snap) DeleteObject(g_pending.snap);
    g_pending = Pending{};
}

void EndCurrentAnim() {
    Anim* a;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        a = g_anim;
    }
    if (a) Release(a);
}

void OnStyleChange(HWND h, LONG newStyle) {
    if (!IsOurBrowserFrame(h) || !IsWindowVisible(h) || IsIconic(h)) return;
    LONG oldStyle = GetWindowLongW(h, GWL_STYLE);
    bool enter = (oldStyle & kFrameBits) == kFrameBits && (newStyle & kFrameBits) == 0;
    bool leave = (oldStyle & kFrameBits) == 0 && (newStyle & kFrameBits) == kFrameBits;
    if (!enter && !leave) return;
    if (leave) {
        RECT wr, mr;
        GetWindowRect(h, &wr);
        if (!MonitorRect(h, &mr) || wr.left > mr.left || wr.top > mr.top ||
            wr.right < mr.right || wr.bottom < mr.bottom) {
            Wh_Log(L"leave ignored: window %d,%d,%d,%d monitor %d,%d,%d,%d", wr.left, wr.top,
                   wr.right, wr.bottom, mr.left, mr.top, mr.right, mr.bottom);
            return;
        }
    }
    // A second press during an animation: end it first so the screenshot shows the real window.
    EndCurrentAnim();
    ClearPending();
    Pending p;
    p.hwnd = h;
    p.enter = enter;
    p.from = VisibleRect(h);
    p.snap = Screenshot(p.from);
    p.tick = GetTickCount();
    if (!p.snap) return;
    std::lock_guard<std::mutex> lock(g_mx);
    g_pending = p;
}

using SetWindowLongW_t = LONG(WINAPI*)(HWND, int, LONG);
SetWindowLongW_t SetWindowLongW_Orig;
LONG WINAPI SetWindowLongW_Hook(HWND h, int index, LONG value) {
    if (index == GWL_STYLE && !t_inHook) {
        t_inHook = true;
        OnStyleChange(h, value);
        LONG r = SetWindowLongW_Orig(h, index, value);
        t_inHook = false;
        return r;
    }
    return SetWindowLongW_Orig(h, index, value);
}

using SetWindowLongPtrW_t = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
SetWindowLongPtrW_t SetWindowLongPtrW_Orig;
LONG_PTR WINAPI SetWindowLongPtrW_Hook(HWND h, int index, LONG_PTR value) {
    if (index == GWL_STYLE && !t_inHook) {
        t_inHook = true;
        OnStyleChange(h, (LONG)value);
        LONG_PTR r = SetWindowLongPtrW_Orig(h, index, value);
        t_inHook = false;
        return r;
    }
    return SetWindowLongPtrW_Orig(h, index, value);
}

using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
SetWindowPos_t SetWindowPos_Orig;
BOOL WINAPI SetWindowPos_Hook(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    // Never hold the lock across the real SetWindowPos: it calls back into the browser's
    // window procedure, which calls SetWindowPos again (v1.0 and v1.1 deadlocked there).
    bool mine;
    bool enter = false;
    DWORD tick = 0;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        mine = g_pending.hwnd && g_pending.hwnd == h;
        if (mine) {
            enter = g_pending.enter;
            tick = g_pending.tick;
        }
    }
    if (!mine) return SetWindowPos_Orig(h, after, x, y, cx, cy, flags);

    if (GetTickCount() - tick > 1000) {
        Wh_Log(L"pending %s expired", enter ? L"enter" : L"leave");
        ClearPending();
        return SetWindowPos_Orig(h, after, x, y, cx, cy, flags);
    }
    // Other calls can come between the style change and the real resize; let them through
    // and keep waiting for the resize itself.
    bool match = (flags & SWP_FRAMECHANGED) && !(flags & (SWP_NOSIZE | SWP_NOMOVE)) && cx > 0 &&
                 cy > 0 && GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId();
    if (match && enter) {
        RECT mr;
        match = MonitorRect(h, &mr) && x == mr.left && y == mr.top && cx == W(mr) && cy == H(mr);
    }
    if (!match) {
        Wh_Log(L"%s: passing other SetWindowPos flags=%X %d,%d %dx%d", enter ? L"enter" : L"leave",
               flags, x, y, cx, cy);
        return SetWindowPos_Orig(h, after, x, y, cx, cy, flags);
    }
    Pending p;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        p = g_pending;
        g_pending = Pending{};
    }
    if (p.hwnd != h) return SetWindowPos_Orig(h, after, x, y, cx, cy, flags);
    Anim* a = Prepare(p);  // owns the screenshot from here
    if (!a) return SetWindowPos_Orig(h, after, x, y, cx, cy, flags);

    SetCloak(h, TRUE);
    BOOL r = SetWindowPos_Orig(h, after, x, y, cx, cy, flags);
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_anim = a;
    }
    a->thread = CreateThread(nullptr, 0, Worker, a, 0, nullptr);
    if (!a->thread) Release(a);
    Wh_Log(L"%s started, prepare took %.1f ms", p.enter ? L"enter" : L"leave", Ms(a->t0));
    return r;
}

void LoadSettings() {
    int d = Wh_GetIntSetting(L"duration");
    if (d < 80) d = 80;
    if (d > 2000) d = 2000;
    g_duration = d;
}

}  // namespace

BOOL Wh_ModInit() {
    g_inst = reinterpret_cast<HINSTANCE>(&__ImageBase);
    LoadSettings();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return FALSE;
    Wh_SetFunctionHook((void*)GetProcAddress(user32, "SetWindowLongW"), (void*)SetWindowLongW_Hook,
                       (void**)&SetWindowLongW_Orig);
    Wh_SetFunctionHook((void*)GetProcAddress(user32, "SetWindowLongPtrW"),
                       (void*)SetWindowLongPtrW_Hook, (void**)&SetWindowLongPtrW_Orig);
    Wh_SetFunctionHook((void*)GetProcAddress(user32, "SetWindowPos"), (void*)SetWindowPos_Hook,
                       (void**)&SetWindowPos_Orig);
    return TRUE;
}

void Wh_ModSettingsChanged() { LoadSettings(); }

void Wh_ModUninit() {
    Anim* a;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        a = g_anim;
    }
    if (a) {
        a->abort = true;
        if (a->thread) WaitForSingleObject(a->thread, 1000);
        SetCloak(a->target, FALSE);
        // Helper windows belong to the browser's UI thread; let it destroy them.
        if (a->host) SendMessageTimeoutW(a->host, kMsgCleanup, 0, 0, SMTO_BLOCK, 1000, nullptr);
    }
    ClearPending();
    UnregisterClassW(kHostClass, g_inst);
    UnregisterClassW(kStillClass, g_inst);
}
