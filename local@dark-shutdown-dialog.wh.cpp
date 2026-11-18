// ==WindhawkMod==
// @id              dark-shutdown-dialog
// @name            Dark shutdown dialog banner (local)
// @description     The Alt+F4 "Shut Down Windows" dialog paints its Windows 11 banner on a fixed white strip, which stays white when the rest of the dialog is dark. This repaints the strip in the dialog's own background colour and redraws the logo with its transparency.
// @version         1.0.2
// @author          local
// @include         explorer.exe
// @compilerOptions -luser32 -lgdi32 -lmsimg32 -lole32 -lwindowscodecs -luuid -lcomctl32
// ==/WindhawkMod==

#include <windhawk_utils.h>
#include <commctrl.h>
#include <wincodec.h>
#include <vector>

namespace {

// Control ids of shell32's shutdown dialog: the icon, the action combo and the hidden event tracker group.
constexpr int kIdIcon = 2027;
constexpr int kIdAction = 2001;
constexpr int kIdTracker = 2010;

HANDLE g_stopEvent = nullptr;
HANDLE g_thread = nullptr;

struct Logo {
    int w = 0, h = 0;
    std::vector<BYTE> pbgra;  // premultiplied, top-down
    RECT box{};                // bounding box of pixels with alpha
};

bool IsShutdownDialog(HWND hwnd) {
    wchar_t cls[16];
    if (!GetClassNameW(hwnd, cls, 16) || wcscmp(cls, L"#32770") != 0) return false;
    return GetDlgItem(hwnd, kIdIcon) && GetDlgItem(hwnd, kIdAction) && GetDlgItem(hwnd, kIdTracker);
}

// basebrd.dll IMAGE 123 / 1123 / 2123 are the banner logo for 100 / 125 / 150 % and up, with alpha.
bool LoadLogo(UINT dpi, Logo& out) {
    const WORD id = dpi >= 144 ? 2123 : dpi >= 120 ? 1123 : 123;
    wchar_t path[MAX_PATH];
    if (!GetSystemWindowsDirectoryW(path, MAX_PATH)) return false;
    wcscat_s(path, L"\\Branding\\Basebrd\\basebrd.dll");
    HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) return false;
    bool ok = false;
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), L"IMAGE");
    HGLOBAL data = res ? LoadResource(mod, res) : nullptr;
    const BYTE* bytes = data ? static_cast<const BYTE*>(LockResource(data)) : nullptr;
    const DWORD size = res ? SizeofResource(mod, res) : 0;
    if (bytes && size) {
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), size)) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            UINT w = 0, h = 0;
            conv->GetSize(&w, &h);
            out.w = (int)w;
            out.h = (int)h;
            out.pbgra.resize((size_t)w * h * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.pbgra.size(), out.pbgra.data()))) {
                RECT b{out.w, out.h, -1, -1};
                for (int y = 0; y < out.h; y++)
                    for (int x = 0; x < out.w; x++)
                        if (out.pbgra[((size_t)y * out.w + x) * 4 + 3] > 8) {
                            if (x < b.left) b.left = x;
                            if (y < b.top) b.top = y;
                            if (x > b.right) b.right = x;
                            if (y > b.bottom) b.bottom = y;
                        }
                out.box = b;
                ok = b.right >= 0;
            }
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        if (SUCCEEDED(init)) CoUninitialize();
    }
    FreeLibrary(mod);
    return ok;
}

inline bool NearWhite(const BYTE* p) { return p[0] >= 0xF0 && p[1] >= 0xF0 && p[2] >= 0xF0; }

void FixBanner(HWND hwnd) {
    RECT client;
    GetClientRect(hwnd, &client);
    HWND icon = GetDlgItem(hwnd, kIdIcon);
    RECT ir;
    if (!icon || !GetWindowRect(icon, &ir)) return;
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&ir), 2);
    const int w = client.right, scanH = ir.top;  // the banner sits above the icon
    if (w <= 8 || scanH <= 8) return;

    HDC dc = GetDC(hwnd);
    if (!dc) return;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -scanH, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = dib ? CreateCompatibleDC(dc) : nullptr;
    if (!mem) {
        if (dib) DeleteObject(dib);
        ReleaseDC(hwnd, dc);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, scanH, dc, 0, 0, SRCCOPY);
    GdiFlush();
    auto px = [&](int x, int y) { return static_cast<BYTE*>(bits) + ((size_t)y * w + x) * 4; };

    // The white strip is inset from the dialog edges. Its top row is the first one with a long white run; the
    // logo has padding, so that row is all strip. The strip ends where its own edge columns stop being white.
    int top = -1, sl = -1, sr = -1;
    for (int y = 0; y < scanH && top < 0; y++) {
        int l = -1, r = -1;
        for (int x = 0; x < w; x++)
            if (NearWhite(px(x, y))) {
                if (l < 0) l = x;
                r = x;
            }
        if (l >= 0 && r - l > w / 2) {
            top = y;
            sl = l;
            sr = r;
        }
    }
    int bottom = top;
    if (top >= 0)
        while (bottom < scanH && NearWhite(px(sl + 1, bottom)) && NearWhite(px(sr - 1, bottom))) bottom++;
    const int band = top >= 0 ? bottom - top : 0;
    // Dialog background beside the strip; only act when it is dark (light mode keeps the original).
    const BYTE* bg = px(sl > 2 ? sl / 2 : 1, top >= 0 ? top : 0);
    const int luma = (bg[2] * 3 + bg[1] * 6 + bg[0]) / 10;
    Wh_Log(L"strip x %d-%d y %d-%d, background luma %d", sl, sr, top, bottom, luma);
    if (band >= 8 && luma < 110) {
        RECT found{w, bottom, -1, -1};
        for (int y = top; y < bottom; y++)
            for (int x = sl; x <= sr; x++)
                if (!NearWhite(px(x, y))) {
                    if (x < found.left) found.left = x;
                    if (y < found.top) found.top = y;
                    if (x > found.right) found.right = x;
                    if (y > found.bottom) found.bottom = y;
                }
        const COLORREF bgColor = RGB(bg[2], bg[1], bg[0]);
        HBRUSH brush = CreateSolidBrush(bgColor);
        RECT strip{sl, top, sr + 1, bottom};
        FillRect(mem, &strip, brush);
        DeleteObject(brush);

        Logo logo;
        if (found.right >= 0 && LoadLogo(GetDpiForWindow(hwnd), logo)) {
            // Put the logo's visible pixels exactly where the white-backed ones were.
            const int foundW = found.right - found.left + 1, boxW = logo.box.right - logo.box.left + 1;
            const double scale = boxW > 0 ? (double)foundW / boxW : 1.0;
            const int dw = (int)(logo.w * scale + 0.5), dh = (int)(logo.h * scale + 0.5);
            const int dx = found.left - (int)(logo.box.left * scale + 0.5);
            const int dy = found.top - (int)(logo.box.top * scale + 0.5);
            BITMAPINFO li{};
            li.bmiHeader = {sizeof(BITMAPINFOHEADER), logo.w, -logo.h, 1, 32, BI_RGB};
            void* lbits = nullptr;
            HBITMAP ldib = CreateDIBSection(mem, &li, DIB_RGB_COLORS, &lbits, nullptr, 0);
            if (ldib) {
                memcpy(lbits, logo.pbgra.data(), logo.pbgra.size());
                HDC lmem = CreateCompatibleDC(mem);
                HGDIOBJ lold = SelectObject(lmem, ldib);
                BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                SetStretchBltMode(mem, HALFTONE);
                AlphaBlend(mem, dx, dy, dw, dh, lmem, 0, 0, logo.w, logo.h, bf);
                SelectObject(lmem, lold);
                DeleteDC(lmem);
                DeleteObject(ldib);
            }
        }
        BitBlt(dc, sl, top, sr + 1 - sl, band, mem, sl, top, SRCCOPY);
        Wh_Log(L"Banner %d px repainted on %p, logo box %d,%d-%d,%d", band, hwnd, found.left, found.top, found.right,
               found.bottom);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(hwnd, dc);
}

LRESULT CALLBACK DialogSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, DWORD_PTR) {
    if (msg == WM_PAINT) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        FixBanner(hwnd);
        return r;
    }
    // WindhawkUtils' wrapper removes the subclass itself on WM_NCDESTROY.
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void Attach(HWND hwnd) {
    if (!IsShutdownDialog(hwnd)) return;
    if (WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, DialogSubclassProc, 0)) {
        InvalidateRect(hwnd, nullptr, TRUE);
        Wh_Log(L"Shutdown dialog %p subclassed", hwnd);
    }
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd) Attach(hwnd);
}

DWORD WINAPI WatchThread(void*) {
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, GetCurrentProcessId(),
                                         0, WINEVENT_OUTOFCONTEXT);
    for (;;) {  // Out-of-context events arrive through this thread's message queue.
        const DWORD r = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    if (hook) UnhookWinEvent(hook);
    return 0;
}

BOOL CALLBACK DetachEnum(HWND hwnd, LPARAM) {
    if (IsShutdownDialog(hwnd)) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(hwnd, DialogSubclassProc);
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    return TRUE;
}

}  // namespace

BOOL Wh_ModInit() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
    }
    EnumWindows(DetachEnum, 0);
    if (g_stopEvent) CloseHandle(g_stopEvent);
}
