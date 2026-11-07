// Checks that ID2D1RenderTarget::Clear honours an axis-aligned clip on a software DC target.
#include <windows.h>
#include <d2d1.h>
#include <cstdio>
int wmain() {
    CoInitialize(nullptr);
    const int W = 64, H = 32;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits; HDC s = GetDC(nullptr); HDC mem = CreateCompatibleDC(s); HBITMAP dib = CreateDIBSection(s, &bi, DIB_RGB_COLORS, &bits, nullptr, 0); SelectObject(mem, dib);
    ID2D1Factory* f; D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    ID2D1DCRenderTarget* rt; f->CreateDCRenderTarget(&p, &rt);
    RECT all{0, 0, W, H};
    rt->BindDC(mem, &all); rt->BeginDraw(); rt->Clear(D2D1::ColorF(1, 0, 0, 1)); rt->EndDraw();
    rt->BindDC(mem, &all); rt->BeginDraw();
    rt->PushAxisAlignedClip(D2D1::RectF(16, 0, 32, 32), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->Clear(D2D1::ColorF(0, 0.0f)); rt->PopAxisAlignedClip(); rt->EndDraw();
    DWORD* px = (DWORD*)bits;
    printf("outside=%08x inside=%08x\n", (unsigned)px[5], (unsigned)px[20]);
    return 0;
}
