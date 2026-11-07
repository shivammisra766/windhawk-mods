// ---- Media module: the collapsed pill (art, clock, bars) and the player page. UI thread only. ----

// IAudioMeterInformation is only forward-declared by this compiler's headers, so it is declared here.
struct IMeterInfo : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* n) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 n, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};
const GUID kIidMeterInfo = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

// The pill keeps the clock exactly centred: both slots are the same width.
constexpr float kMediaSlot = 26.0f;
constexpr float kPlayerW = 380.0f, kPlayerH = 182.0f;
constexpr int kBarsHist = 7;

// Digits differ in width; measuring with the widest one stops the pill from shifting each minute.
std::wstring WidestDigitForm(const std::wstring& s) {
    wchar_t best = L'0';
    float bw = 0;
    for (wchar_t d = L'0'; d <= L'9'; d++) {
        const float w = g_text.Measure(std::wstring(1, d));
        if (w > bw) { bw = w; best = d; }
    }
    std::wstring o = s;
    for (wchar_t& c : o) if (c >= L'0' && c <= L'9') c = best;
    return o;
}

// Comma separated, case-insensitive substring match against name, app id and title.
bool MediaBlocked(const std::wstring& list, const MSnap& s) {
    if (list.empty()) return false;
    const std::wstring hay = LowerW(s.sourceName + L"\n" + s.aumid + L"\n" + s.title);
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(L',', i);
        if (j == std::wstring::npos) j = list.size();
        std::wstring e = LowerW(list.substr(i, j - i));
        const size_t a = e.find_first_not_of(L" \t"), b = e.find_last_not_of(L" \t");
        if (a != std::wstring::npos) {
            e = e.substr(a, b - a + 1);
            if (hay.find(e) != std::wstring::npos) return true;
        }
        i = j + 1;
    }
    return false;
}

std::wstring FormatMs(long long ms) {
    if (ms < 0) ms = 0;
    const long long t = ms / 1000;
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf_s(b, L"%lld:%02lld", t / 60, t % 60);
    return b;
}

class MediaModule : public IModule {
public:
    const wchar_t* Id() const override { return L"media"; }

    void Start(Services& s) override {
        services_ = &s;
        OnSettings(s.GetSettings());
        engine_.Start([this] { OnSnapshot(); });
    }
    void Stop() override {
        services_->KillTimer(this);
        engine_.Stop();
        meter_.Reset();
    }
    void OnSettings(const Settings& s) override {
        autoExpand_ = s.mediaAutoExpand;
        blocklist_ = s.mediaBlocklist;
    }

    int PillPriority() const override { return snap_ && snap_->has ? 10 : 0; }
    bool HasDashboard() const override { return snap_ && snap_->has; }
    int DashboardOrder() const override { return 5; }

    Size2 Measure(View v) override {
        if (v == View::Dashboard) return {kPlayerW, kPlayerH};
        const float clockW = ToDip(g_text.Measure(WidestDigitForm(ClockText())));
        return {clockW + 2 * kPillPadX + 2 * kMediaSlot, kNotchH};
    }

    bool Animating(View v) const override {
        return v == View::Dashboard && snap_ && (snap_->playing || morph_ != (snap_->playing ? 1.0f : 0.0f));
    }

    void OnVisibility(View v, bool shown) override {
        if (v == View::Pill) visPill_ = shown;
        else {
            visDash_ = shown;
            engine_.SetPanelOpen(shown);
            if (shown) { meter_.Reset(); lastSample_ = 0; dragging_ = false; pressed_ = Z_none; }
        }
        engine_.SetShown(visPill_ || visDash_);
        UpdateTimer();
    }

    void Draw(const DrawCtx& c) override {
        if (!snap_ || !snap_->has) return;
        if (c.view == View::Dashboard) DrawPlayer(c);
        else DrawPill(c);
    }

    bool OnPointer(const PointerEvent& e, View v) override;

private:
    enum Zone { Z_none, Z_prev, Z_play, Z_next, Z_scrub, Z_open, Z_dock0 = 10 };

    // ---- Snapshot handling ----
    void OnSnapshot() {
        auto old = snap_;
        snap_ = engine_.Snapshot();
        const bool presence = !old || old->has != snap_->has;
        const bool dockChange = !old || old->sources.size() != snap_->sources.size();
        if (snap_->has && snap_->trackSeq != lastSeq_) {
            const bool first = lastSeq_ == ~0u;
            lastSeq_ = snap_->trackSeq;
            if (!first && autoExpand_ && !g_fullscreen && !MediaBlocked(blocklist_, *snap_)) services_->Expand(this, 5000);
        }
        lastSeq_ = snap_->trackSeq;
        UpdateTimer();
        if (presence || dockChange || !old || old->title != snap_->title || old->durMs != snap_->durMs) services_->Relayout();
        services_->Invalidate();
        services_->WakeAnimation();
    }

    // Collapsed: bars move at about 12 fps, only while the pill shows and music plays. Otherwise no timer.
    void UpdateTimer() {
        const bool want = visPill_ && snap_ && snap_->has && snap_->playing;
        if (want == timerOn_) return;
        timerOn_ = want;
        if (want) {
            meter_.Reset();
            services_->SetTimer(this, 80, [this] { Sample(); services_->Invalidate(); });
        } else {
            services_->KillTimer(this);
            services_->Invalidate();
        }
    }

    // ---- Audio peak ----
    bool EnsureMeter() {
        if (meter_) return true;
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(en.Put()))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.Put()))) return false;
        void* p = nullptr;
        if (FAILED(dev->Activate(kIidMeterInfo, CLSCTX_ALL, nullptr, &p)) || !p) return false;
        *meter_.Put() = static_cast<IMeterInfo*>(p);
        return true;
    }

    void Sample() {
        float peak = 0;
        if (EnsureMeter()) {
            if (FAILED(meter_->GetPeakValue(&peak))) { meter_.Reset(); peak = 0; }  // Default device changed: re-resolve.
        }
        float v = std::min(1.0f, sqrtf(peak) * 1.15f);
        if (!snap_ || !snap_->playing) v = 0;
        const float prev = hist_[(histPos_ + kBarsHist - 1) % kBarsHist];
        v = v > prev ? prev * 0.3f + v * 0.7f : prev * 0.15f + v * 0.85f;  // Attack 0.7, release 0.85, as the old one.
        hist_[histPos_] = v;
        histPos_ = (histPos_ + 1) % kBarsHist;
    }

    // ---- Images ----
    struct Img {
        std::shared_ptr<const MArt> src;
        Com<ID2D1Bitmap> bmp;
        unsigned gen = 0;
    };
    ID2D1Bitmap* BitmapFor(ID2D1RenderTarget* rt, Img& img, const std::shared_ptr<const MArt>& a) {
        if (!a) return nullptr;
        if (img.src != a || !img.bmp || img.gen != g_isSurface.Generation()) {
            img.bmp.Reset();
            D2D1_BITMAP_PROPERTIES p = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(rt->CreateBitmap(D2D1::SizeU(a->w, a->h), a->px.data(), a->w * 4, &p, img.bmp.Put()))) return nullptr;
            img.src = a;
            img.gen = g_isSurface.Generation();
        }
        return img.bmp.Get();
    }

    static void FillBitmapRounded(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp, D2D1_RECT_F r, float radius, float alpha) {
        Com<ID2D1BitmapBrush> bb;
        if (!bmp || FAILED(rt->CreateBitmapBrush(bmp, bb.Put()))) return;
        const D2D1_SIZE_F sz = bmp->GetSize();
        bb->SetOpacity(alpha);
        bb->SetTransform(D2D1::Matrix3x2F::Scale((r.right - r.left) / sz.width, (r.bottom - r.top) / sz.height) *
                         D2D1::Matrix3x2F::Translation(r.left, r.top));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), bb.Get());
    }

    D2D1_COLOR_F Accent(float a = 1.0f) const {
        float r = 0.298f, g = 0.788f, b = 0.941f;
        if (snap_ && snap_->art && snap_->art->hasAccent) { r = snap_->art->acc[0]; g = snap_->art->acc[1]; b = snap_->art->acc[2]; }
        return D2D1::ColorF(r, g, b, a);
    }

    static void SetCol(const DrawCtx& c, D2D1_COLOR_F col, float a = 1.0f) {
        col.a *= a * c.alpha;
        c.brush->SetColor(col);
    }

    void DrawArt(const DrawCtx& c, D2D1_RECT_F r, float radius) {
        const float s = c.scale;
        if (snap_->art) {
            if (ID2D1Bitmap* b = BitmapFor(c.rt, art_, snap_->art)) { FillBitmapRounded(c.rt, b, r, radius * s, c.alpha); return; }
        }
        SetCol(c, Accent(), 0.24f);
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius * s, radius * s), c.brush);
        const std::wstring letter = snap_->sourceName.empty() ? L"" : snap_->sourceName.substr(0, 1);
        SetCol(c, kFg, 0.9f);
        TextOpts o;
        o.center = true;
        o.maxW = r.right - r.left;
        o.maxH = r.bottom - r.top;
        g_text.Draw(c.rt, letter, r.left, r.top, c.brush, o);
    }

    // Bars from the history ring, oldest on the left. Paused: a row of dots.
    void DrawBars(const DrawCtx& c, float x, float y, float w, float h, int n) {
        const float s = c.scale;
        const float gap = 1.6f * s;
        const float bw = (w - (n - 1) * gap) / n;
        for (int i = 0; i < n; i++) {
            if (!snap_->playing) {
                SetCol(c, kFg, 0.5f);
                c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + i * (bw + gap) + bw / 2, y + h / 2), bw / 2, bw / 2), c.brush);
                continue;
            }
            const float amp = hist_[(histPos_ + kBarsHist - n + i + kBarsHist) % kBarsHist];
            const float bh = std::max(3.0f * s, amp * 0.86f * h);
            SetCol(c, Accent(), 0.45f + 0.5f * amp);
            const float bx = x + i * (bw + gap);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, y + (h - bh) / 2, bx + bw, y + (h + bh) / 2), bw / 2, bw / 2), c.brush);
        }
    }

    void DrawPill(const DrawCtx& c) {
        const float s = c.scale, cy = (c.rect.top + c.rect.bottom) / 2;
        const float slot = kMediaSlot * s;
        const float art = 18.0f * s;
        DrawArt(c, D2D1::RectF(c.rect.left + (slot - art) / 2 + 3 * s, cy - art / 2, c.rect.left + (slot + art) / 2 + 3 * s, cy + art / 2), 4.0f);
        DrawCentredText(c, ClockText(), D2D1::RectF(c.rect.left + slot, c.rect.top, c.rect.right - slot, c.rect.bottom));
        const float bw = 18.0f * s, bh = 14.0f * s;
        DrawBars(c, c.rect.right - slot + (slot - bw) / 2 - 3 * s, cy - bh / 2, bw, bh, 5);
    }

    // ---- Player page ----
    struct Geo { float x, y, w, h; };
    static D2D1_RECT_F R(const DrawCtx& c, float x, float y, float w, float h) {
        const float s = c.scale;
        return D2D1::RectF(c.rect.left + x * s, c.rect.top + y * s, c.rect.left + (x + w) * s, c.rect.top + (y + h) * s);
    }

    long long CurPos() const {
        const ULONGLONG now = GetTickCount64();
        if (dragging_ && snap_->durMs > 0) return (long long)(dragFrac_ * snap_->durMs);
        if (now < ovTick_ + 1500) return std::min<long long>(ovPos_ + (snap_->playing ? (long long)((now - ovTick_) * snap_->rate) : 0), snap_->durMs);
        long long p = snap_->posMs + (snap_->playing ? (long long)((now - snap_->baseTick) * snap_->rate) : 0);
        return std::max(0LL, snap_->durMs > 0 ? std::min(p, snap_->durMs) : p);
    }

    void Poly(const DrawCtx& c, const D2D1_POINT_2F* p, int n) {
        Com<ID2D1PathGeometry> g;
        Com<ID2D1GeometrySink> sk;
        if (FAILED(g_factory->CreatePathGeometry(g.Put())) || FAILED(g->Open(sk.Put()))) return;
        sk->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < n; i++) sk->AddLine(p[i]);
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        sk->Close();
        c.rt->FillGeometry(g.Get(), c.brush);
    }

    // The play/pause morph: two quads, each blending a half of the play triangle into a pause bar.
    void DrawPlayPause(const DrawCtx& c, float cx, float cy, float size, float t) {
        const float L = cx - size / 2, T = cy - size / 2;
        auto lerp = [](float a, float b, float k) { return a + (b - a) * k; };
        const float pl[2][4][2] = {{{0.15f, 0}, {0.55f, 0.25f}, {0.55f, 0.75f}, {0.15f, 1}},
                                   {{0.55f, 0.25f}, {1.0f, 0.5f}, {1.0f, 0.5f}, {0.55f, 0.75f}}};
        const float pa[2][4][2] = {{{0.12f, 0}, {0.40f, 0}, {0.40f, 1}, {0.12f, 1}},
                                   {{0.60f, 0}, {0.88f, 0}, {0.88f, 1}, {0.60f, 1}}};
        for (int q = 0; q < 2; q++) {
            D2D1_POINT_2F pts[4];
            for (int i = 0; i < 4; i++) pts[i] = D2D1::Point2F(L + lerp(pl[q][i][0], pa[q][i][0], t) * size, T + lerp(pl[q][i][1], pa[q][i][1], t) * size);
            Poly(c, pts, 4);
        }
    }

    void DrawSkip(const DrawCtx& c, float cx, float cy, float size, bool next) {
        const float d = next ? 1.0f : -1.0f;
        const float h = size / 2;
        D2D1_POINT_2F a[3] = {{cx - d * h * 0.9f, cy - h}, {cx - d * h * 0.9f, cy + h}, {cx + d * h * 0.5f, cy}};
        Poly(c, a, 3);
        c.rt->FillRectangle(D2D1::RectF(std::min(cx + d * h * 0.62f, cx + d * h * 0.98f), cy - h, std::max(cx + d * h * 0.62f, cx + d * h * 0.98f), cy + h), c.brush);
    }

    void DrawPlayer(const DrawCtx& c) {
        const MSnap& m = *snap_;
        const float s = c.scale;
        morph_ += (((m.playing ? 1.0f : 0.0f) - morph_) * std::min(1.0f, (float)c.dt * 14.0f));
        if (fabsf(morph_ - (m.playing ? 1.0f : 0.0f)) < 0.01f) morph_ = m.playing ? 1.0f : 0.0f;
        if (m.playing && c.now - lastSample_ >= 0.033) { Sample(); lastSample_ = c.now; }

        // Album wash: a soft radial of the accent from the top middle.
        {
            const float w = c.rect.right - c.rect.left, h = c.rect.bottom - c.rect.top;
            D2D1_GRADIENT_STOP st[2] = {{0.0f, Accent(0.115f * c.alpha)}, {1.0f, Accent(0.0f)}};
            Com<ID2D1GradientStopCollection> col;
            Com<ID2D1RadialGradientBrush> rb;
            if (SUCCEEDED(c.rt->CreateGradientStopCollection(st, 2, col.Put())) &&
                SUCCEEDED(c.rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(c.rect.left + w / 2, c.rect.top), D2D1::Point2F(0, 0), w * 0.75f, h * 1.15f), col.Get(), rb.Put()))) {
                c.rt->FillRectangle(c.rect, rb.Get());
            }
        }
        // Art with a source badge, text column, bars.
        const auto artR = R(c, 24, 18, 64, 64);
        DrawArt(c, artR, 16.0f);
        if (m.active >= 0 && (size_t)m.active < m.sources.size() && m.sources[m.active].icon) {
            const auto bad = D2D1::RectF(artR.right - 24 * s, artR.bottom - 22 * s, artR.right - 3 * s, artR.bottom - 1 * s);
            SetCol(c, kSurfaceSolid, 1.0f);
            c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((bad.left + bad.right) / 2, (bad.top + bad.bottom) / 2), 11 * s, 11 * s), c.brush);
            if (ID2D1Bitmap* b = BitmapFor(c.rt, srcImg_[m.sources[m.active].aumid], m.sources[m.active].icon))
                c.rt->DrawBitmap(b, D2D1::RectF(bad.left + 2.5f * s, bad.top + 2.5f * s, bad.right - 2.5f * s, bad.bottom - 2.5f * s), c.alpha);
        }
        const float textSize = g_text.DefaultSize();
        auto text = [&](const std::wstring& t, float y, float h, float size, float a) {
            SetCol(c, kFg, a);
            TextOpts o;
            o.maxW = 202 * s;
            o.maxH = h * s;
            o.ellipsis = true;
            o.size = size;
            const auto r = R(c, 106, y, 202, h);
            g_text.Draw(c.rt, t, r.left, r.top, c.brush, o);
        };
        text(m.title.empty() ? Loc(L"media.unknown") : m.title, 18, 22, textSize * 1.15f, 1.0f);
        text(m.artist, 40, 18, textSize, 0.8f);
        float srcY = 62;
        if (!m.album.empty()) { text(m.album, 58, 16, textSize * 0.92f, 0.7f); srcY = 76; }
        {
            wchar_t line[160];
            swprintf_s(line, Loc(m.playing ? L"media.playingfrom" : L"media.pausedin"), m.sourceName.c_str());
            text(line, srcY, 14, textSize * 0.85f, 0.55f);
        }
        DrawBars(c, R(c, 324, 24, 32, 20).left, R(c, 324, 24, 32, 20).top, 32 * s, 20 * s, 7);

        // Scrubber and time labels; hidden for live streams (duration 0).
        if (m.durMs > 0) {
            const long long pos = CurPos();
            const float frac = (float)pos / (float)m.durMs;
            const auto tr = R(c, 72, 111.5f, 236, 5);
            const float cx = tr.left + frac * (tr.right - tr.left), half = (dragging_ ? 3.5f : 2.5f) * s;
            SetCol(c, kTile, m.canSeek ? 1.0f : 0.5f);  // Track.
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, (tr.top + tr.bottom) / 2 - half, tr.right, (tr.top + tr.bottom) / 2 + half), half, half), c.brush);
            SetCol(c, Accent(), m.canSeek ? 1.0f : 0.5f);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, (tr.top + tr.bottom) / 2 - half, cx, (tr.top + tr.bottom) / 2 + half), half, half), c.brush);
            if (m.canSeek) {
                const float rad = (dragging_ ? 6.5f : 4.5f) * s, my = (tr.top + tr.bottom) / 2;
                if (dragging_) { SetCol(c, Accent(), 0.22f); c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, my), rad * 2.2f, rad * 2.2f), c.brush); }
                SetCol(c, Accent());
                c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, my), rad + 1.6f * s, rad + 1.6f * s), c.brush);
                SetCol(c, Rgb(255, 255, 255));
                c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, my), rad, rad), c.brush);
            }
            auto label = [&](const std::wstring& t, float x, float w, bool right) {
                SetCol(c, kFg, 0.6f);
                TextOpts o;
                o.maxW = w * s;
                o.maxH = 16 * s;
                o.size = textSize * 0.85f;
                const auto r = R(c, x, 106, w, 16);
                const float tw = g_text.Measure(t) * (o.size / textSize);
                g_text.Draw(c.rt, t, right ? r.right - tw : r.left, r.top, c.brush, o);
            };
            label(FormatMs(pos), 24, 40, true);
            label(L"-" + FormatMs(m.durMs - pos), 316, 40, false);
        }

        // Transport.
        const float cxm = c.rect.left + (kPlayerW / 2) * s, cyb = c.rect.top + 140 * s;
        struct Btn { Zone z; float dx, r; bool enabled; };
        const Btn btns[3] = {{Z_prev, -64, 16, m.canPrev}, {Z_play, 0, 22, m.canToggle}, {Z_next, 64, 16, m.canNext}};
        for (const Btn& b : btns) {
            const bool hot = hover_ == b.z, down = pressed_ == b.z;
            const float rad = b.r * s * (down ? 0.87f : 1.0f);
            const bool big = b.z == Z_play;
            if (down) SetCol(c, Accent(), 0.5f);
            else if (big) SetCol(c, hot ? kTileHover : kTile, b.enabled ? 1.0f : 0.5f);
            else SetCol(c, kTile, (hot ? 1.0f : 0.5f) * (b.enabled ? 1.0f : 0.5f));  // Skip buttons: half a tile until hovered.
            c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cxm + b.dx * s, cyb), rad, rad), c.brush);
            SetCol(c, kFg, (big ? (hot ? 1.0f : 0.88f) : (hot ? 0.92f : 0.62f)) * (b.enabled ? 1.0f : 0.35f));
            if (big) DrawPlayPause(c, cxm, cyb, 18 * s * (down ? 0.87f : 1.0f), morph_);
            else DrawSkip(c, cxm + b.dx * s, cyb, 12 * s * (down ? 0.87f : 1.0f), b.z == Z_next);
        }

        // Source dock, only with two or more sources.
        const std::vector<int> dock = DockSlots();
        if (dock.size() >= 2 || (dock.size() == 1 && m.sources.size() >= 2)) {
            const auto pill = R(c, 20, 125, 2 + (float)dock.size() * 28, 30);
            SetCol(c, kTile);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 15 * s, 15 * s), c.brush);
            for (size_t i = 0; i < dock.size(); i++) {
                const int si = dock[i];
                const auto sr = R(c, 22 + (float)i * 28, 127, 26, 26);
                const float scx = (sr.left + sr.right) / 2, scy = (sr.top + sr.bottom) / 2;
                const bool active = si == m.active, hot = hover_ == Z_dock0 + (int)i;
                if (si < 0) {  // "+N"
                    SetCol(c, kFg, hot ? 1.0f : 0.6f);
                    wchar_t t[16];
                    swprintf_s(t, L"+%d", (int)m.sources.size() - 2);
                    TextOpts o; o.center = true; o.maxW = sr.right - sr.left; o.maxH = sr.bottom - sr.top; o.size = textSize * 0.9f;
                    g_text.Draw(c.rt, t, sr.left, sr.top, c.brush, o);
                    continue;
                }
                if (active) {
                    SetCol(c, Accent());
                    c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(scx, scy), 12.25f * s, 12.25f * s), c.brush, 1.5f * s);
                }
                const MSource& src = m.sources[si];
                const float ia = active || hot ? 1.0f : 0.5f;
                if (ID2D1Bitmap* b = src.icon ? BitmapFor(c.rt, srcImg_[src.aumid], src.icon) : nullptr) {
                    c.rt->DrawBitmap(b, D2D1::RectF(scx - 8 * s, scy - 8 * s, scx + 8 * s, scy + 8 * s), ia * c.alpha);
                } else {
                    SetCol(c, kFg, ia);
                    TextOpts o; o.center = true; o.maxW = sr.right - sr.left; o.maxH = sr.bottom - sr.top; o.size = textSize;
                    g_text.Draw(c.rt, src.name.substr(0, 1), sr.left, sr.top, c.brush, o);
                }
                if (src.playing) {
                    SetCol(c, kSurfaceSolid);
                    c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sr.right - 4 * s, sr.bottom - 4 * s), 4.5f * s, 4.5f * s), c.brush);
                    SetCol(c, Rgb(0x3a, 0xd0, 0x6c));
                    c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sr.right - 4 * s, sr.bottom - 4 * s), 3.5f * s, 3.5f * s), c.brush);
                }
            }
        }
    }

    // Up to 3 sources: one slot each. More: first, active, "+N" (index -1).
    std::vector<int> DockSlots() const {
        std::vector<int> out;
        const int n = (int)snap_->sources.size();
        if (n < 2) return out;
        if (n <= 3) { for (int i = 0; i < n; i++) out.push_back(i); return out; }
        out.push_back(0);
        out.push_back(snap_->active > 0 ? snap_->active : 1);
        out.push_back(-1);
        return out;
    }

    Zone HitTest(D2D1_POINT_2F p) const {  // p in DIP
        auto in = [&](float x, float y, float w, float h) { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; };
        const std::vector<int> dock = DockSlots();
        for (size_t i = 0; i < dock.size(); i++) if (in(22 + (float)i * 28, 127, 26, 26)) return (Zone)(Z_dock0 + (int)i);
        auto circ = [&](float cx, float r) { const float dx = p.x - cx, dy = p.y - 140; return dx * dx + dy * dy <= (r + 6) * (r + 6); };
        if (circ(190, 22)) return Z_play;
        if (circ(126, 16)) return Z_prev;
        if (circ(254, 16)) return Z_next;
        if (snap_->durMs > 0 && in(66, 104, 248, 22)) return Z_scrub;
        if (in(24, 18, 64, 64) || in(106, 18, 202, 62)) return Z_open;
        return Z_none;
    }

    Services* services_ = nullptr;
    MediaEngine engine_;
    std::shared_ptr<const MSnap> snap_;
    unsigned lastSeq_ = ~0u;
    bool autoExpand_ = true, visPill_ = false, visDash_ = false, timerOn_ = false;
    std::wstring blocklist_;
    Img art_;
    std::unordered_map<std::wstring, Img> srcImg_;
    Com<IMeterInfo> meter_;
    float hist_[kBarsHist] = {};
    int histPos_ = 0;
    double lastSample_ = 0;
    float morph_ = 0;
    Zone hover_ = Z_none, pressed_ = Z_none;
    bool dragging_ = false;
    float dragFrac_ = 0;
    long long ovPos_ = 0;
    ULONGLONG ovTick_ = 0;
    bool wasLeft_ = false;
};

bool MediaModule::OnPointer(const PointerEvent& e, View v) {
    if (v != View::Dashboard || !snap_ || !snap_->has) return false;
    const float s = Scale();
    const D2D1_POINT_2F p = D2D1::Point2F(e.pt.x / s, e.pt.y / s);
    auto fracAt = [&](float x) { return std::max(0.0f, std::min(1.0f, (x - 72.0f) / 236.0f)); };
    auto finishScrub = [&] {
        if (!dragging_) return;
        dragging_ = false;
        const long long target = (long long)(dragFrac_ * snap_->durMs);
        ovPos_ = target;
        ovTick_ = GetTickCount64();
        engine_.Command({MCmdKind::Seek, target});
    };
    switch (e.kind) {
        case PointerEvent::Move: {
            if (dragging_) { dragFrac_ = fracAt(p.x); services_->Invalidate(); return true; }
            const Zone z = HitTest(p);
            if (z != hover_) { hover_ = z; services_->Invalidate(); }
            return true;
        }
        case PointerEvent::Leave:
            finishScrub();
            hover_ = pressed_ = Z_none;
            services_->Invalidate();
            return true;
        case PointerEvent::Down: {
            const Zone z = HitTest(p);
            pressed_ = z;
            if (z == Z_scrub && snap_->canSeek) { dragging_ = true; dragFrac_ = fracAt(p.x); }
            services_->Invalidate();
            return true;
        }
        case PointerEvent::Up: {
            const Zone z = HitTest(p);
            const Zone was = pressed_;
            pressed_ = Z_none;
            if (dragging_) { dragFrac_ = fracAt(p.x); finishScrub(); services_->Invalidate(); return true; }
            if (z == was) {
                if (z == Z_prev) engine_.Command({MCmdKind::Prev});
                else if (z == Z_next) engine_.Command({MCmdKind::Next});
                else if (z == Z_play) engine_.Command({MCmdKind::Toggle});
                else if (z == Z_open) engine_.Command({MCmdKind::Open});
                else if (z >= Z_dock0) {
                    const std::vector<int> dock = DockSlots();
                    const size_t i = z - Z_dock0;
                    if (i < dock.size()) {
                        int target = dock[i];
                        if (target < 0) target = (snap_->active + 1) % (int)snap_->sources.size();  // "+N" cycles.
                        if (target != snap_->active) engine_.Command({MCmdKind::Pick, 0, snap_->sources[target].aumid});
                    }
                }
            }
            services_->Invalidate();
            return true;
        }
    }
    return false;
}

MediaModule g_mediaModule;
