
// ---- LayeredSurface: a retained premultiplied DIB drawn with Direct2D and pushed to a layered window ----

// One software DC render target, kept for the life of the surface. Drawing is limited to a dirty
// rectangle, and only that rectangle is handed to UpdateLayeredWindowIndirect.
class LayeredSurface {
public:
    ~LayeredSurface() { Destroy(); }

    // Sizes the DIB. Contents are lost (all transparent) when the size changes.
    bool Resize(ID2D1Factory* factory, int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (mem_ && w == w_ && h == h_ && rt_) return true;
        Destroy();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        mem_ = CreateCompatibleDC(screen);
        dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!mem_ || !dib_) {
            Destroy();
            return false;
        }
        old_ = SelectObject(mem_, dib_);
        w_ = w;
        h_ = h;
        // The software target keeps the working set small: the default one loads the GPU driver.
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        if (FAILED(factory->CreateDCRenderTarget(&props, rt_.Put()))) {
            Destroy();
            return false;
        }
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType is invalid with transparency
        ++generation_;
        return true;
    }

    // Starts drawing: everything outside `dirty` is left alone, everything inside is cleared.
    ID2D1DCRenderTarget* Begin(const RECT& dirty) {
        if (!rt_) return nullptr;
        RECT all{0, 0, w_, h_};
        if (FAILED(rt_->BindDC(mem_, &all))) return nullptr;
        rt_->BeginDraw();
        rt_->PushAxisAlignedClip(D2D1::RectF((float)dirty.left, (float)dirty.top, (float)dirty.right, (float)dirty.bottom),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        rt_->Clear(D2D1::ColorF(0, 0.0f));
        return rt_.Get();
    }

    // Returns false if the target was lost and has to be rebuilt by the next Resize.
    bool End() {
        rt_->PopAxisAlignedClip();
        const HRESULT hr = rt_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            rt_.Reset();
            return false;
        }
        return true;
    }

    // `pos` is the window's top left on screen.
    void Present(HWND hwnd, POINT pos, const RECT& dirty, bool whole) {
        if (!mem_) return;
        HDC screen = GetDC(nullptr);
        POINT src{0, 0};
        SIZE size{w_, h_};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        RECT rc = dirty;
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.hdcDst = screen;
        info.pptDst = &pos;
        info.psize = &size;
        info.hdcSrc = mem_;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = whole ? nullptr : &rc;
        UpdateLayeredWindowIndirect(hwnd, &info);
        ReleaseDC(nullptr, screen);
    }

    // Pushes only the crop (srcX, 0, size) of the DIB, and moves and resizes the window to match. Used by the
    // island, whose window is just the current shape while the DIB is the largest stage.
    void PresentCrop(HWND hwnd, POINT pos, POINT src, SIZE size, BYTE alpha = 255) {
        if (!mem_) return;
        HDC screen = GetDC(nullptr);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.hdcDst = screen;
        info.pptDst = &pos;
        info.psize = &size;
        info.hdcSrc = mem_;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        UpdateLayeredWindowIndirect(hwnd, &info);
        ReleaseDC(nullptr, screen);
    }

    ID2D1DCRenderTarget* Target() const { return rt_.Get(); }
    unsigned Generation() const { return generation_; }
    int Width() const { return w_; }
    int Height() const { return h_; }

    void Destroy() {
        rt_.Reset();
        if (mem_) {
            if (old_) SelectObject(mem_, old_);
            DeleteDC(mem_);
        }
        if (dib_) DeleteObject(dib_);
        mem_ = nullptr;
        dib_ = nullptr;
        old_ = nullptr;
        bits_ = nullptr;
        w_ = h_ = 0;
    }

private:
    HDC mem_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    unsigned generation_ = 0;
    Com<ID2D1DCRenderTarget> rt_;
};
