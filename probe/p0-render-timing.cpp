// P0: headless timing of GDI+ vs Direct2D (software, default) on a mock 475x230 frame.
// Usage: p0 gdiplus|d2d-sw|d2d-def [frames]. One backend per process so working sets are clean.
#include <windows.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <psapi.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <cstdlib>
using namespace Gdiplus;

static const int W = 475, H = 230;
static const wchar_t* kLines[] = {L"Thursday 7 Nov, 06:55 PM", L"Now playing - A long track title here", L"Battery 84%  Wi-Fi  Bluetooth", L"Calendar  Bell  Settings", L"Hello world 1234567890"};
static const char* kPath = "M12 2 C 6 2 2 6 2 12 s 4 10 10 10 s 10 -4 10 -10 S 18 2 12 2 z";

static double Now() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
static SIZE_T WS(SIZE_T* priv) { PROCESS_MEMORY_COUNTERS_EX m{}; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m)); *priv = m.PrivateUsage; return m.WorkingSetSize; }

static void GdiFrame(Graphics& g, int f) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    float w = 300 + 150 * (f % 40) / 40.0f;
    GraphicsPath p; float r = 18, d = 2 * r, x = 10, y = 0, h = 220;
    p.AddArc(x, y, d, d, 180, 90); p.AddArc(x + w - d, y, d, d, 270, 90); p.AddArc(x + w - d, y + h - d, d, d, 0, 90); p.AddArc(x, y + h - d, d, d, 90, 90); p.CloseFigure();
    SolidBrush b(Color(255, 16, 16, 16)); g.FillPath(&b, &p);
    FontFamily ff(L"Segoe UI"); Font font(&ff, 16, FontStyleRegular, UnitPixel); SolidBrush fg(Color(255, 228, 228, 228));
    for (int i = 0; i < 5; i++) g.DrawString(kLines[i], -1, &font, PointF(24, 20.0f + i * 34), &fg);
    Pen pen(Color(255, 228, 228, 228), 1.5f);
    for (int i = 0; i < 12; i++) { GraphicsPath ic; ic.AddEllipse(300.0f + (i % 4) * 30, 40.0f + (i / 4) * 40, 20.0f, 20.0f); g.DrawPath(&pen, &ic); }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const wchar_t* mode = argv[1];
    int frames = argc > 2 ? _wtoi(argv[2]) : 200;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SIZE_T priv0, priv1; SIZE_T ws0 = WS(&priv0);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HDC screen = GetDC(nullptr); HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0); SelectObject(mem, dib);
    double first = 0, total = 0, worst = 0;
    if (!wcscmp(mode, L"gdiplus")) {
        ULONG_PTR tok; GdiplusStartupInput in; GdiplusStartup(&tok, &in, nullptr);
        for (int f = 0; f < frames; f++) {
            double t0 = Now();
            { Bitmap bm(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits); Graphics g(&bm); GdiFrame(g, f); }
            double dt = Now() - t0; if (!f) first = dt; else { total += dt; if (dt > worst) worst = dt; }
        }
    } else {
        ID2D1Factory* fac = nullptr; D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &fac);
        IDWriteFactory* dw = nullptr; DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
        IDWriteTextFormat* tf = nullptr; dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"en-us", &tf);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(!wcscmp(mode, L"d2d-sw") ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        ID2D1DCRenderTarget* rt = nullptr; HRESULT hr = fac->CreateDCRenderTarget(&props, &rt);
        if (FAILED(hr)) { wprintf(L"create target failed %08x\n", (unsigned)hr); return 1; }
        ID2D1SolidColorBrush *bBg, *bFg; rt->CreateSolidColorBrush(D2D1::ColorF(0x101010), &bBg); rt->CreateSolidColorBrush(D2D1::ColorF(0xE4E4E4), &bFg);
        RECT rc{0, 0, W, H};
        for (int f = 0; f < frames; f++) {
            double t0 = Now();
            rt->BindDC(mem, &rc); rt->BeginDraw(); rt->Clear(D2D1::ColorF(0, 0.0f));
            float w = 300 + 150 * (f % 40) / 40.0f;
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, 0, 10 + w, 220), 18, 18), bBg);
            for (int i = 0; i < 5; i++) rt->DrawText(kLines[i], (UINT32)wcslen(kLines[i]), tf, D2D1::RectF(24, 20.0f + i * 34, 300, 50.0f + i * 34), bFg);
            for (int i = 0; i < 12; i++) rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(310.0f + (i % 4) * 30, 50.0f + (i / 4) * 40), 10, 10), bFg, 1.5f);
            rt->EndDraw();
            double dt = Now() - t0; if (!f) first = dt; else { total += dt; if (dt > worst) worst = dt; }
        }
    }
    SIZE_T ws1 = WS(&priv1);
    wprintf(L"%s: first %.2f ms, mean %.3f ms, worst %.2f ms | WS %.1f -> %.1f MB (+%.1f), private %.1f -> %.1f MB\n", mode, first, total / (frames - 1), worst,
        ws0 / 1048576.0, ws1 / 1048576.0, (ws1 - ws0) / 1048576.0, priv0 / 1048576.0, priv1 / 1048576.0);
    return 0;
}
