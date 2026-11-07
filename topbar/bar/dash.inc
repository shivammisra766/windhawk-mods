// ---- Dashboards (P6c): shared drawing helpers, page base, idle, calendar, hardware, weather, timer ----
// Pages are modules with HasDashboard(). The island owns switching, the dot column and the slide;
// a page does work only between OnVisibility(Dashboard, true) and (…, false).

// Fixed look (specs/dashboards.md section 2). Cards are the theme's tiles (kTile, kTileHover in types.inc).
const D2D1_COLOR_F kHairline = Rgb(255, 255, 255, 26);
const D2D1_COLOR_F kMuted = Rgb(228, 228, 228, 153);
const D2D1_COLOR_F kAccent = Rgb(228, 228, 228);
const D2D1_COLOR_F kAmber = Rgb(255, 176, 33);
const D2D1_COLOR_F kRed = Rgb(255, 89, 82);
const D2D1_COLOR_F kInk = Rgb(16, 16, 16);

constexpr float kPageW = 380.0f, kPageH = 184.0f;
enum TextFlag { TF_Center = 1, TF_Right = 2, TF_Ellipsis = 4, TF_Wrap = 8 };

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string out(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], (int)out.size(), nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], (int)out.size());
    return out;
}

// Where the bar keeps content that must survive a reformat: D: when it is a fixed disk, else the user profile.
// Never the Windhawk mod storage, which every process can write.
const std::wstring& DataRoot() {
    static std::wstring root;
    if (root.empty()) {
        auto usable = [](const std::wstring& p) {
            SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
            const DWORD a = GetFileAttributesW(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        };
        if (GetDriveTypeW(L"D:\\") == DRIVE_FIXED && usable(L"D:\\windhawk-data\\top-bar")) {
            root = L"D:\\windhawk-data\\top-bar";
        } else {
            wchar_t buf[MAX_PATH] = {};
            GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
            root = std::wstring(buf) + L"\\top-bar";
            usable(root);
        }
    }
    return root;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &n, nullptr) && n == data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool ReadWholeFile(const std::wstring& path, std::string& out, DWORD cap = 16u << 20) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart <= cap;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD n = 0;
        ok = ReadFile(f, out.empty() ? nullptr : &out[0], (DWORD)out.size(), &n, nullptr) && n == out.size();
    }
    CloseHandle(f);
    return ok;
}

ULONGLONG WallSeconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL;
}

// Drawing in DIP relative to the page's top left; everything scales and fades with the DrawCtx.
struct Pg {
    const DrawCtx& c;
    float s;
    explicit Pg(const DrawCtx& ctx) : c(ctx), s(ctx.scale) {}
    D2D1_RECT_F R(float l, float t, float r, float b) const {
        return D2D1::RectF(c.rect.left + l * s, c.rect.top + t * s, c.rect.left + r * s, c.rect.top + b * s);
    }
    float X(float x) const { return c.rect.left + x * s; }
    float Y(float y) const { return c.rect.top + y * s; }
    void Col(D2D1_COLOR_F k, float a = 1.0f) const {
        k.a *= c.alpha * a;
        c.brush->SetColor(k);
    }
    void Fill(float l, float t, float r, float b, float rad, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(R(l, t, r, b), rad * s, rad * s), c.brush);
    }
    void Outline(float l, float t, float r, float b, float rad, float w, D2D1_COLOR_F k, float a = 1.0f,
                 ID2D1StrokeStyle* style = nullptr) const {
        Col(k, a);
        c.rt->DrawRoundedRectangle(D2D1::RoundedRect(R(l, t, r, b), rad * s, rad * s), c.brush, w * s, style);
    }
    void Dot(float cx, float cy, float r, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(X(cx), Y(cy)), r * s, r * s), c.brush);
    }
    void Ring(float cx, float cy, float r, float w, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(X(cx), Y(cy)), r * s, r * s), c.brush, w * s);
    }
    void Line(float x1, float y1, float x2, float y2, float w, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->DrawLine(D2D1::Point2F(X(x1), Y(y1)), D2D1::Point2F(X(x2), Y(y2)), c.brush, w * s);
    }
    void Text(const std::wstring& t, float l, float tp, float r, float b, float size, D2D1_COLOR_F k, int flags = 0,
              int weight = 400) const {
        if (t.empty()) return;
        Col(k);
        TextOpts o;
        o.size = size * s;
        o.maxW = (r - l) * s;
        o.maxH = (b - tp) * s;
        o.center = flags & TF_Center;
        o.right = flags & TF_Right;
        o.ellipsis = flags & TF_Ellipsis;
        o.wrap = flags & TF_Wrap;
        o.weight = weight;
        g_text.Draw(c.rt, t, X(l), Y(tp), c.brush, o);
    }
};

// Width in DIP of a text at a size and weight.
float TextWidth(const std::wstring& t, float size, int weight = 400) {
    TextOpts o;
    o.size = size * Scale();
    o.weight = weight;
    Com<IDWriteTextLayout> layout = g_text.Layout(t, o);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace / Scale();
}

// Arc from 12 o'clock, clockwise, `frac` of a turn. Round caps.
void DrawArcFrac(const Pg& p, float cx, float cy, float r, float frac, float w, D2D1_COLOR_F k) {
    if (frac <= 0.002f) return;
    static Com<ID2D1StrokeStyle> round;
    if (!round) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        g_factory->CreateStrokeStyle(sp, nullptr, 0, round.Put());
    }
    p.Col(k);
    if (frac >= 0.999f) {
        p.c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(p.X(cx), p.Y(cy)), r * p.s, r * p.s), p.c.brush, w * p.s);
        return;
    }
    const float a = frac * 6.2831853f;
    Com<ID2D1PathGeometry> geo;
    Com<ID2D1GeometrySink> sink;
    if (FAILED(g_factory->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
    const float R = r * p.s;
    sink->BeginFigure(D2D1::Point2F(p.X(cx), p.Y(cy) - R), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(p.X(cx) + R * sinf(a), p.Y(cy) - R * cosf(a)), D2D1::SizeF(R, R), 0,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    p.c.rt->DrawGeometry(geo.Get(), p.c.brush, w * p.s, round.Get());
}

// ---- Page base: pointer hit rects in DIP, rebuilt by each Draw ----
struct HitRect {
    float l, t, r, b;
    int id;
};

class PageBase : public IModule {
public:
    bool HasDashboard() const override { return true; }
    Size2 Measure(View) override { return {kPageW, kPageH}; }
    void Start(Services& s) override { sv_ = &s; }
    void Stop() override { if (sv_) sv_->KillTimer(this); }
    void OnVisibility(View v, bool shown) override {
        if (v != View::Dashboard || shown == shown_) return;
        shown_ = shown;
        hover_ = down_ = 0;
        if (shown) Shown(); else Hidden();
    }
    bool OnPointer(const PointerEvent& e, View) override {
        const float x = e.pt.x / Scale(), y = e.pt.y / Scale();
        int id = 0;
        if (e.kind != PointerEvent::Leave) {
            for (const HitRect& h : hits_) if (x >= h.l && x < h.r && y >= h.t && y < h.b) id = h.id;
        }
        if (e.kind == PointerEvent::Down) down_ = id;
        bool changed = id != hover_;
        hover_ = id;
        if (e.kind == PointerEvent::Up) {
            const int was = down_;
            down_ = 0;
            if (was && was == id) { Click(id); changed = true; }
        }
        if (e.kind == PointerEvent::Leave) down_ = 0;
        if (changed && sv_) sv_->Invalidate();
        return id != 0;
    }
protected:
    virtual void Shown() {}
    virtual void Hidden() {}
    virtual void Click(int) {}
    void Hit(float l, float t, float r, float b, int id) { hits_.push_back({l, t, r, b, id}); }
    Services* sv_ = nullptr;
    bool shown_ = false;
    int hover_ = 0, down_ = 0;
    std::vector<HitRect> hits_;
};

// ---- Idle ----
class IdlePage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-idle"; }
    int DashboardOrder() const override { return 10; }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        hits_.clear();
        Hit(0, 0, kPageW, kPageH, 1);  // The whole page: a click opens the calendar.
        wchar_t tm[40] = {}, dt[64] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, tm, 40);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"dddd, MMMM d", dt, 64, nullptr);
        p.Text(tm, 0, 44, 360, 112, 56, kFg, TF_Center, 700);
        p.Text(dt, 0, 114, 360, 138, 16, kMuted, TF_Center, 600);
        if (g_battery >= 0) {
            wchar_t line[64];
            if (!g_charging && life_ != 0xFFFFFFFFu && life_ > 0) {
                swprintf_s(line, L"%d%%   %uh %02um left", g_battery, life_ / 3600, (life_ / 60) % 60);
            } else {
                swprintf_s(line, L"%d%%", g_battery);
            }
            const float tw = TextWidth(line, 11);
            const float x0 = 180.0f - (tw + 22.0f) / 2.0f;
            const bool low = !g_charging && g_battery <= 20;
            IconId icon = g_charging ? I_bolt : g_battery >= 90 ? I_batfull : g_battery >= 60 ? I_bathigh : g_battery >= 35 ? I_batmed : g_battery >= 15 ? I_batlow : I_batwarn;
            p.Col(low ? kRed : kFg);
            g_icons.Draw(c.rt, c.brush, icon, p.X(x0 + 7), p.Y(160), 14.0f * c.scale);
            p.Text(line, x0 + 22, 150, x0 + 22 + tw + 4, 170, 11, low ? kRed : kMuted);
        }
    }
protected:
    void Click(int) override { IslandOpenCalendar(); }
    void Shown() override {
        SYSTEM_POWER_STATUS st{};
        life_ = GetSystemPowerStatus(&st) ? st.BatteryLifeTime : 0xFFFFFFFFu;
    }
private:
    DWORD life_ = 0xFFFFFFFFu;
};

// ---- Calendar (read-only; a click on the clock or the idle page opens the native calendar) ----
int DaysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

class CalendarPage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-calendar"; }
    int DashboardOrder() const override { return 20; }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t month[48] = {}, wday[48] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"MMMM", month, 48, nullptr);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"dddd", wday, 48, nullptr);
        DWORD first = 0;  // 0 = Monday
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, (LPWSTR)&first, 2);
        first %= 7;
        SYSTEMTIME f = now;
        f.wDay = 1;
        FILETIME ft;
        SystemTimeToFileTime(&f, &ft);
        FileTimeToSystemTime(&ft, &f);
        const int firstMon = (f.wDayOfWeek + 6) % 7;
        const int startIdx = (firstMon - (int)first + 7) % 7;
        const int dim = DaysInMonth(now.wYear, now.wMonth);
        const int rows = (startIdx + dim + 6) / 7;

        p.Fill(22, 16, 115, 164, 12, kTile);
        p.Text(month, 28, 22, 112, 38, 12, kAccent, TF_Ellipsis, 700);
        wchar_t year[8];
        swprintf_s(year, L"%d", now.wYear);
        p.Text(year, 28, 37, 112, 52, 11, kMuted);
        wchar_t day[8];
        swprintf_s(day, L"%d", now.wDay);
        p.Text(day, 22, 50, 115, 138, 63, kFg, TF_Center, 700);
        p.Text(wday, 26, 140, 112, 158, 12, kMuted, TF_Center | TF_Ellipsis, 700);

        const float gx = 134, gy = 16, col = 28, headH = 18;
        for (int i = 0; i < 7; i++) {
            const int mon = ((int)first + i) % 7;
            wchar_t nm[8] = {};
            std::wstring ch = L"?";
            if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, 0x31 + mon, nm, 8) && nm[0]) ch.assign(nm, 1);
            p.Text(ch, gx + i * col, gy, gx + (i + 1) * col, gy + headH, 11, kMuted, TF_Center, 600);
        }
        p.Line(gx, gy + headH + 3, gx + 7 * col, gy + headH + 3, 1, kHairline);
        const float top = gy + headH + 7, rowH = (kPageH - 14 - top) / rows;
        const float rad = std::min(col, rowH) / 2.0f - 1.5f;
        for (int d = 1; d <= dim; d++) {
            const int idx = startIdx + d - 1, r = idx / 7, cc = idx % 7;
            const float cx = gx + cc * col + col / 2, cy = top + r * rowH + rowH / 2;
            const bool today = d == now.wDay;
            const int mon = ((int)first + cc) % 7;
            wchar_t n[4];
            swprintf_s(n, L"%d", d);
            if (today) p.Dot(cx, cy, rad, kAccent);
            D2D1_COLOR_F dc = today ? kInk : kFg;
            if (!today && mon >= 5) dc.a *= 0.62f;  // weekends recede
            p.Text(n, cx - col / 2, cy - rowH / 2, cx + col / 2, cy + rowH / 2, 12, dc, TF_Center, today ? 700 : 400);
        }
    }
};

// ---- Hardware ----
// CPU, RAM and disk are cheap and sampled on the bar thread at 1 Hz. GPU and network come from PDH: the
// first PDH use blocks for about half a second, so a worker owns it and only runs while the page shows.
struct HwShared {
    std::atomic<int> gpu{-1};          // percent, -1 unavailable
    std::atomic<long long> up{-1};     // bytes per second
    std::atomic<long long> down{-1};
    std::atomic<bool> run{false};
    std::atomic<bool> alive{false};
    Services* sv = nullptr;
};
HwShared g_hw;
HANDLE g_hwEvent = nullptr;
PDH_HQUERY g_pdhQuery = nullptr;
PDH_HCOUNTER g_pdhGpu = nullptr, g_pdhUp = nullptr, g_pdhDown = nullptr;

// Sum of every item that matches `filter`, grouped by `keyOf`; mode 0 = max of the groups, 1 = sum.
double PdhArray(PDH_HCOUNTER counter, bool gpu) {
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA) return -1;
    std::vector<BYTE> buf(bytes + 64);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    std::unordered_map<std::wstring, double> groups;
    double sum = 0;
    for (DWORD i = 0; i < count; i++) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        const std::wstring name = items[i].szName;
        const double v = items[i].FmtValue.doubleValue;
        if (gpu) {  // per engine, summed over processes; the busiest engine is the load
            const size_t at = name.find(L"_luid_");
            groups[at == std::wstring::npos ? name : name.substr(at)] += v;
        } else {
            if (name.find(L"Loopback") != std::wstring::npos || name.find(L"vEthernet") != std::wstring::npos ||
                name.find(L"isatap") != std::wstring::npos || name.find(L"Teredo") != std::wstring::npos) continue;
            sum += v;
        }
    }
    if (!gpu) return sum;
    double best = 0;
    for (auto& g : groups) best = std::max(best, g.second);
    return best;
}

DWORD WINAPI HwWorker(void*) {
    for (;;) {
        if (!g_pdhQuery && PdhOpenQueryW(nullptr, 0, &g_pdhQuery) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(g_pdhQuery, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g_pdhGpu);
            PdhAddEnglishCounterW(g_pdhQuery, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &g_pdhUp);
            PdhAddEnglishCounterW(g_pdhQuery, L"\\Network Interface(*)\\Bytes Received/sec", 0, &g_pdhDown);
        }
        if (g_pdhQuery) {
            PdhCollectQueryData(g_pdhQuery);  // Baseline: rates need two samples.
            while (g_hw.run) {
                WaitForSingleObject(g_hwEvent, 1000);
                if (!g_hw.run) break;
                if (PdhCollectQueryData(g_pdhQuery) != ERROR_SUCCESS) continue;
                if (g_pdhGpu) {
                    const double v = PdhArray(g_pdhGpu, true);
                    g_hw.gpu = v < 0 ? -1 : (int)(std::min(v, 100.0) + 0.5);
                }
                if (g_pdhUp) g_hw.up = (long long)std::max(PdhArray(g_pdhUp, false), -1.0);
                if (g_pdhDown) g_hw.down = (long long)std::max(PdhArray(g_pdhDown, false), -1.0);
                if (g_hw.sv) g_hw.sv->Post([] { if (g_hw.sv) g_hw.sv->Invalidate(); });
            }
        } else {
            g_hw.run = false;
        }
        g_hw.alive = false;
        if (!(g_hw.run && !g_hw.alive.exchange(true))) break;
    }
    return 0;
}

class HardwarePage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-hardware"; }
    int DashboardOrder() const override { return 40; }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        struct Card { const wchar_t* label; std::wstring value; float frac; int glyph; };
        wchar_t b[48];
        Card cards[6];
        swprintf_s(b, L"%d%%", cpu_);
        cards[0] = {L"CPU", cpu_ < 0 ? L"--" : b, cpu_ < 0 ? -1.0f : cpu_ / 100.0f, 0};
        wchar_t ram[48];
        swprintf_s(ram, L"%.1f / %.1f GB", ramUsed_, ramTotal_);
        cards[1] = {L"RAM", ramTotal_ > 0.1 ? ram : L"--", ramTotal_ > 0.1 ? (float)(ramUsed_ / ramTotal_) : -1.0f, 1};
        wchar_t gp[16];
        swprintf_s(gp, L"%d%%", g_hw.gpu.load());
        cards[2] = {L"GPU", g_hw.gpu < 0 ? L"--" : gp, g_hw.gpu < 0 ? -1.0f : g_hw.gpu / 100.0f, 2};
        wchar_t nu[32], nd[32];
        swprintf_s(nu, L"%.1f Mbps", g_hw.up.load() * 8.0 / 1e6);
        swprintf_s(nd, L"%.1f Mbps", g_hw.down.load() * 8.0 / 1e6);
        cards[3] = {L"NET UP", g_hw.up < 0 ? L"--" : nu, -1.0f, 3};
        cards[4] = {L"NET DOWN", g_hw.down < 0 ? L"--" : nd, -1.0f, 4};
        wchar_t dk[48];
        swprintf_s(dk, L"%.0f / %.0f GB", diskUsed_, diskTotal_);
        cards[5] = {L"DISK C:", diskTotal_ > 0.1 ? dk : L"--", diskTotal_ > 0.1 ? (float)(diskUsed_ / diskTotal_) : -1.0f, 5};
        // Reading order: CPU, NET UP / RAM, NET DOWN / GPU, DISK.
        static const int order[6] = {0, 3, 1, 4, 2, 5};
        const float cw = (kPageW - 48 - 9 - 12) / 2.0f;
        for (int i = 0; i < 6; i++) {
            const Card& k = cards[order[i]];
            const float l = 24 + (i % 2) * (cw + 9), t = 22 + (i / 2) * 44, r = l + cw, bt = t + 38;
            p.Fill(l, t, r, bt, 9, kTile);
            D2D1_COLOR_F tint = k.frac >= 0.90f ? kRed : k.frac >= 0.75f ? kAmber : kAccent;
            Glyph(p, k.glyph, l + 16, t + 19, tint);
            p.Text(k.label, l + 30, t + 2, r - 9, t + 15, 11, kMuted);
            p.Text(k.value, l + 30, t + 15, r - 9, k.frac >= 0 ? t + 31 : bt - 4, 13.5f, kFg, TF_Ellipsis, 600);
            if (k.frac >= 0) {
                p.Fill(l + 30, bt - 6, r - 9, bt - 3.5f, 1.25f, kHairline);
                const float w = (r - 9 - l - 30) * std::min(k.frac, 1.0f);
                if (w > 0.5f) p.Fill(l + 30, bt - 6, l + 30 + w, bt - 3.5f, 1.25f, tint);
            }
        }
    }
protected:
    void Shown() override {
        cpu_ = -1;
        kPrev_ = 0;
        g_hw.gpu = -1;
        g_hw.up = g_hw.down = -1;
        g_hw.sv = sv_;
        tick_ = 0;
        Sample();
        sv_->SetTimer(this, 1000, [this] { Sample(); });
        if (!g_hwEvent) g_hwEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_hw.run = true;
        if (!g_hw.alive.exchange(true)) {
            if (HANDLE h = CreateThread(nullptr, 0, HwWorker, nullptr, 0, nullptr)) CloseHandle(h);
            else g_hw.alive = false;
        }
    }
    void Hidden() override {
        sv_->KillTimer(this);
        g_hw.run = false;
        if (g_hwEvent) SetEvent(g_hwEvent);
    }
private:
    void Sample() {
        FILETIME idle, kern, user;
        if (GetSystemTimes(&idle, &kern, &user)) {
            auto u = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
            const ULONGLONG i = u(idle), k = u(kern) + u(user);  // kernel time includes idle
            if (kPrev_) {
                const ULONGLONG dk = k - kPrev_, di = i - iPrev_;
                if (dk > 0) cpu_ = (int)(100.0 * (double)(dk - std::min(di, dk)) / (double)dk + 0.5);
            }
            kPrev_ = k;
            iPrev_ = i;
        }
        MEMORYSTATUSEX m{sizeof(m)};
        if (GlobalMemoryStatusEx(&m)) {
            ramTotal_ = m.ullTotalPhys / 1073741824.0;
            ramUsed_ = (m.ullTotalPhys - m.ullAvailPhys) / 1073741824.0;
        }
        if (tick_++ % 30 == 0) {
            ULARGE_INTEGER avail{}, total{}, freeB{};
            if (GetDiskFreeSpaceExW(L"C:\\", &avail, &total, &freeB) && total.QuadPart) {
                diskTotal_ = total.QuadPart / 1073741824.0;
                diskUsed_ = (total.QuadPart - freeB.QuadPart) / 1073741824.0;
            }
        }
        sv_->Invalidate();
    }
    static void Glyph(const Pg& p, int id, float x, float y, D2D1_COLOR_F k) {
        switch (id) {
            case 0: p.Outline(x - 6, y - 6, x + 6, y + 6, 2, 1.3f, k); p.Fill(x - 2.5f, y - 2.5f, x + 2.5f, y + 2.5f, 1, k); break;
            case 1: for (int i = -1; i <= 1; i++) p.Fill(x + i * 4.5f - 1.5f, y - 6, x + i * 4.5f + 1.5f, y + 6, 1, k); break;
            case 2: p.Outline(x - 7, y - 5, x + 7, y + 5, 2, 1.3f, k); p.Ring(x, y, 2.5f, 1.3f, k); break;
            case 3: p.Line(x, y + 6, x, y - 5, 1.6f, k); p.Line(x - 4, y - 1, x, y - 5, 1.6f, k); p.Line(x + 4, y - 1, x, y - 5, 1.6f, k); break;
            case 4: p.Line(x, y - 6, x, y + 5, 1.6f, k); p.Line(x - 4, y + 1, x, y + 5, 1.6f, k); p.Line(x + 4, y + 1, x, y + 5, 1.6f, k); break;
            default: p.Outline(x - 7, y - 4, x + 7, y + 4, 2.5f, 1.3f, k); p.Dot(x + 3.5f, y, 1.2f, k); break;
        }
    }
    int cpu_ = -1, tick_ = 0;
    ULONGLONG kPrev_ = 0, iPrev_ = 0;
    double ramUsed_ = 0, ramTotal_ = 0, diskUsed_ = 0, diskTotal_ = 0;
};

// ---- Weather ----
struct WeatherData {
    bool valid = false;
    int code = 0, temp = 0, feels = 0, humidity = 0, wind = 0;
    bool fahrenheit = true;
    std::wstring desc, area, dir, city;
    ULONGLONG when = 0;  // WallSeconds of the reading
};

// Value after "key": up to the closing quote, or to the next separator for numbers.
bool JsonValue(const std::string& j, const char* key, size_t from, std::string& out, size_t* at = nullptr) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return false;
    p += k.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == ':')) p++;
    if (p >= j.size()) return false;
    out.clear();
    if (j[p] == '"') {
        for (p++; p < j.size() && j[p] != '"'; p++) {
            if (j[p] == '\\' && p + 1 < j.size()) p++;
            out += j[p];
        }
    } else {
        while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != ']' && j[p] != '\r' && j[p] != '\n') out += j[p++];
    }
    if (at) *at = p;
    return true;
}

std::string PercentEncode(const std::wstring& city) {
    const std::string u = Utf8(city);
    bool encoded = false;  // already %XX: pass through
    for (size_t i = 0; i + 2 < u.size(); i++) {
        if (u[i] == '%' && isxdigit((unsigned char)u[i + 1]) && isxdigit((unsigned char)u[i + 2])) encoded = true;
    }
    if (encoded) return u;
    std::string out;
    char buf[4];
    for (unsigned char ch : u) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += (char)ch;
        else { snprintf(buf, sizeof(buf), "%%%02X", ch); out += buf; }
    }
    return out;
}

// Runs on a worker. Fills `w` only for a real reading (HTTP 200 and a code or a description).
bool FetchWeather(const std::wstring& city, bool fahrenheit, WeatherData& w) {
    const std::wstring url = L"https://wttr.in/" + Wide(PercentEncode(city)) + L"?format=j2";
    HINTERNET h = InternetOpenW(L"top-bar", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!h) return false;
    DWORD to = 10000;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_SEND_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    HINTERNET u = InternetOpenUrlW(h, url.c_str(), L"Accept: application/json\r\n", (DWORD)-1,
                                   INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    bool ok = false;
    if (u) {
        DWORD status = 0, len = sizeof(status);
        if (HttpQueryInfoW(u, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr) && status == 200) {
            std::string body;
            char buf[4096];
            DWORD n = 0;
            while (body.size() < (256u << 10) && InternetReadFile(u, buf, sizeof(buf), &n) && n) body.append(buf, n);
            const size_t cc = body.find("\"current_condition\"");
            std::string v;
            if (cc != std::string::npos) {
                WeatherData r;
                r.fahrenheit = fahrenheit;
                if (JsonValue(body, fahrenheit ? "temp_F" : "temp_C", cc, v)) r.temp = atoi(v.c_str());
                if (JsonValue(body, fahrenheit ? "FeelsLikeF" : "FeelsLikeC", cc, v)) r.feels = atoi(v.c_str());
                if (JsonValue(body, "humidity", cc, v)) r.humidity = atoi(v.c_str());
                if (JsonValue(body, fahrenheit ? "windspeedMiles" : "windspeedKmph", cc, v)) r.wind = atoi(v.c_str());
                if (JsonValue(body, "winddir16Point", cc, v)) r.dir = Wide(v);
                if (JsonValue(body, "weatherCode", cc, v)) r.code = atoi(v.c_str());
                const size_t wd = body.find("\"weatherDesc\"", cc);
                if (wd != std::string::npos && JsonValue(body, "value", wd, v)) r.desc = Wide(v);
                while (!r.desc.empty() && r.desc.back() == L' ') r.desc.pop_back();
                const size_t na = body.find("\"areaName\"");
                if (na != std::string::npos && JsonValue(body, "value", na, v)) r.area = Wide(v);
                if (r.code != 0 || !r.desc.empty()) {
                    r.valid = true;
                    r.when = WallSeconds();
                    w = r;
                    ok = true;
                }
            }
        }
        InternetCloseHandle(u);
    }
    InternetCloseHandle(h);
    return ok;
}

int WeatherKind(int code) {  // 0 sun, 1 partly, 2 cloud, 3 rain, 4 snow, 5 storm, 6 fog
    switch (code) {
        case 113: return 0;
        case 116: return 1;
        case 119: case 122: return 2;
        case 143: case 248: case 260: return 6;
        case 200: case 386: case 389: case 392: case 395: return code >= 392 ? 4 : 5;
        case 179: case 182: case 185: case 227: case 230: case 323: case 326: case 329: case 332: case 335: case 338:
        case 350: case 368: case 371: case 374: case 377: return 4;
        default: return code >= 176 ? 3 : 2;
    }
}

const wchar_t* WindArrow(const std::wstring& dir) {  // flow direction, not bearing
    static const wchar_t* names[] = {L"N", L"NNE", L"NE", L"ENE", L"E", L"ESE", L"SE", L"SSE", L"S", L"SSW", L"SW", L"WSW", L"W", L"WNW", L"NW", L"NNW"};
    static const wchar_t* arrows[] = {L"\u2193", L"\u2193", L"\u2199", L"\u2190", L"\u2190", L"\u2190", L"\u2196", L"\u2191", L"\u2191", L"\u2191", L"\u2197", L"\u2192", L"\u2192", L"\u2192", L"\u2198", L"\u2193"};
    for (int i = 0; i < 16; i++) if (dir == names[i]) return arrows[i];
    return L"";
}

class WeatherPage;
struct WeatherJob { std::wstring city; bool f; Services* sv; WeatherPage* self; };
DWORD WINAPI WeatherThread(void* arg);

class WeatherPage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-weather"; }
    int DashboardOrder() const override { return 30; }
    void Start(Services& s) override {
        PageBase::Start(s);
        city_ = s.GetSettings().weatherCity;
        fahrenheit_ = s.GetSettings().fahrenheit;
    }
    void OnSettings(const Settings& s) override {
        if (s.weatherCity == city_ && s.fahrenheit == fahrenheit_) return;
        city_ = s.weatherCity;
        fahrenheit_ = s.fahrenheit;
        fails_ = 0;
        nextTry_ = 0;
        data_.valid = false;  // another unit or place: the old reading no longer applies
        if (shown_ && sv_) Consider();
    }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        const bool have = data_.valid;
        const ULONGLONG age = have ? WallSeconds() - data_.when : 0;
        const float dim = have && age > 3600 ? 0.55f : 1.0f;
        std::wstring place = !city_.empty() ? city_ : (have ? data_.area : L"");
        if (!have) place = fetching_ ? L"Locating..." : (fails_ ? L"Offline" : L"");
        p.Text(place, 26, 24, 188, 44, 11, kMuted, TF_Ellipsis);
        wchar_t t[16];
        if (have) swprintf_s(t, L"%d\u00B0", data_.temp); else wcscpy_s(t, L"--");
        if (have) WeatherIcon(p, WeatherKind(data_.code), 48, 78, 44, dim);
        p.Text(t, 80, 46, 190, 110, 42, kFg, 0, 700);
        if (have) {
            std::wstring d = data_.desc;
            if (age > 3600) { wchar_t a[32]; swprintf_s(a, L" (%llu h old)", age / 3600); d += a; }
            const size_t n = d.size();
            const float sz = n > 58 ? 9.8f : n > 44 ? 10.5f : n > 32 ? 11.5f : n > 22 ? 12.5f : 13.5f;
            p.Text(d, 26, 118, 188, 162, sz, kFg, TF_Wrap, 600);
        }
        p.Line(200, 34, 200, 150, 1, kHairline);
        wchar_t wind[40] = L"--", feels[16] = L"--", hum[16] = L"--";
        if (have) {
            swprintf_s(wind, L"%d %ls %ls", data_.wind, data_.fahrenheit ? L"mph" : L"km/h", WindArrow(data_.dir));
            swprintf_s(feels, L"%d\u00B0", data_.feels);
            swprintf_s(hum, L"%d%%", data_.humidity);
        }
        const wchar_t* labels[3] = {L"Wind", L"Feels like", L"Humidity"};
        const wchar_t* values[3] = {wind, feels, hum};
        for (int i = 0; i < 3; i++) {
            const float top = 25 + i * 47;
            p.Fill(214, top, 358, top + 40, 10, kTile, dim);
            if (i == 0) { p.Line(222, top + 15, 235, top + 15, 1.4f, kAccent, dim); p.Line(222, top + 20, 232, top + 20, 1.4f, kAccent, dim); p.Line(222, top + 25, 236, top + 25, 1.4f, kAccent, dim); }
            else if (i == 1) { p.Line(229, top + 12, 229, top + 24, 1.6f, kAccent, dim); p.Dot(229, top + 27, 3, kAccent, dim); }
            else { p.Dot(229, top + 24, 4, kAccent, dim); p.Line(229, top + 12, 225.5f, top + 21, 1.4f, kAccent, dim); p.Line(229, top + 12, 232.5f, top + 21, 1.4f, kAccent, dim); }
            p.Text(labels[i], 244, top + 3, 354, top + 19, 11, kMuted, TF_Ellipsis);
            p.Text(values[i], 244, top + 18, 354, top + 37, 13.5f, kFg, TF_Ellipsis, 600);
        }
    }
protected:
    void Shown() override {
        if (!loaded_) { loaded_ = true; LoadCache(); }
        Consider();
    }
    void Hidden() override { sv_->KillTimer(this); }
private:
    static void WeatherIcon(const Pg& p, int kind, float cx, float cy, float size, float a) {
        const float u = size;
        auto cloud = [&](float ox, float oy, D2D1_COLOR_F k) {
            p.Dot(cx + ox - 0.20f * u, cy + oy + 0.06f * u, 0.17f * u, k, a);
            p.Dot(cx + ox + 0.00f * u, cy + oy - 0.08f * u, 0.22f * u, k, a);
            p.Dot(cx + ox + 0.21f * u, cy + oy + 0.06f * u, 0.16f * u, k, a);
            p.Fill(cx + ox - 0.20f * u, cy + oy + 0.0f * u, cx + ox + 0.21f * u, cy + oy + 0.22f * u, 0.1f * u, k, a);
        };
        const D2D1_COLOR_F white = Rgb(228, 228, 228);
        if (kind == 0 || kind == 1) {
            const float sx = kind == 0 ? 0 : -0.14f * u, sy = kind == 0 ? 0 : -0.14f * u;
            p.Dot(cx + sx, cy + sy, 0.2f * u, kAmber, a);
            for (int i = 0; i < 8; i++) {
                const float an = i * 0.7853982f, c1 = cosf(an), s1 = sinf(an);
                p.Line(cx + sx + c1 * 0.29f * u, cy + sy + s1 * 0.29f * u, cx + sx + c1 * 0.4f * u, cy + sy + s1 * 0.4f * u, 1.6f, kAmber, a);
            }
        }
        if (kind >= 1) cloud(kind == 1 ? 0.08f * u : 0, kind == 1 ? 0.08f * u : -0.06f * u, white);
        if (kind == 3) for (int i = -1; i <= 1; i++) p.Line(cx + i * 0.2f * u, cy + 0.24f * u, cx + i * 0.2f * u - 0.06f * u, cy + 0.4f * u, 1.6f, Rgb(90, 170, 255), a);
        if (kind == 4) for (int i = -1; i <= 1; i++) p.Dot(cx + i * 0.2f * u, cy + 0.33f * u, 0.04f * u, white, a);
        if (kind == 5) {
            p.Line(cx + 0.05f * u, cy + 0.2f * u, cx - 0.05f * u, cy + 0.33f * u, 1.8f, kAmber, a);
            p.Line(cx - 0.05f * u, cy + 0.33f * u, cx + 0.06f * u, cy + 0.33f * u, 1.8f, kAmber, a);
            p.Line(cx + 0.06f * u, cy + 0.33f * u, cx - 0.03f * u, cy + 0.46f * u, 1.8f, kAmber, a);
        }
        if (kind == 6) for (int i = -1; i <= 1; i++) p.Line(cx - 0.3f * u, cy + i * 0.14f * u, cx + 0.3f * u, cy + i * 0.14f * u, 2.0f, white, a);
    }
    std::wstring CachePath() { return DataRoot() + L"\\weather.txt"; }
    void LoadCache() {
        std::string txt;
        if (!ReadWholeFile(CachePath(), txt, 65536)) return;
        WeatherData d;
        size_t pos = 0;
        std::wstring city;
        while (pos < txt.size()) {
            size_t e = txt.find('\n', pos);
            if (e == std::string::npos) e = txt.size();
            std::string line = txt.substr(pos, e - pos);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            pos = e + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "when") d.when = strtoull(v.c_str(), nullptr, 10);
            else if (k == "code") d.code = atoi(v.c_str());
            else if (k == "temp") d.temp = atoi(v.c_str());
            else if (k == "feels") d.feels = atoi(v.c_str());
            else if (k == "humidity") d.humidity = atoi(v.c_str());
            else if (k == "wind") d.wind = atoi(v.c_str());
            else if (k == "f") d.fahrenheit = v == "1";
            else if (k == "desc") d.desc = Wide(v);
            else if (k == "area") d.area = Wide(v);
            else if (k == "dir") d.dir = Wide(v);
            else if (k == "city") city = Wide(v);
        }
        d.valid = d.when != 0 && d.fahrenheit == fahrenheit_ && city == city_ && (d.code != 0 || !d.desc.empty());
        if (d.valid) { data_ = d; lastOk_ = d.when; }
    }
    void SaveCache() {
        char b[64];
        std::string s = "when=" + std::to_string(data_.when) + "\ncode=" + std::to_string(data_.code) + "\ntemp=" + std::to_string(data_.temp) +
                        "\nfeels=" + std::to_string(data_.feels) + "\nhumidity=" + std::to_string(data_.humidity) + "\nwind=" + std::to_string(data_.wind) +
                        "\nf=" + (data_.fahrenheit ? "1" : "0") + "\ndesc=" + Utf8(data_.desc) + "\narea=" + Utf8(data_.area) + "\ndir=" + Utf8(data_.dir) +
                        "\ncity=" + Utf8(city_) + "\n";
        (void)b;
        WriteFileAtomic(CachePath(), s);
    }
    // Fetch now if the reading is stale and backoff allows; otherwise a one-shot for the next due time.
    void Consider() {
        sv_->KillTimer(this);
        if (!shown_ || fetching_) return;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG wall = WallSeconds();
        const bool stale = !data_.valid || wall - lastOk_ >= 900;
        ULONGLONG due = stale ? 0 : (900 - (wall - lastOk_)) * 1000;
        if (stale && nextTry_ > now) due = nextTry_ - now;
        if (due == 0) { Start(); return; }
        sv_->SetTimer(this, (UINT)std::min<ULONGLONG>(due + 50, 3600000), [this] { sv_->KillTimer(this); Consider(); });
    }
    void Start() {
        fetching_ = true;
        const std::wstring city = city_;
        const bool f = fahrenheit_;
        Services* sv = sv_;
        WeatherPage* self = this;
        auto* job = new WeatherJob{city, f, sv, self};
        HANDLE h = CreateThread(nullptr, 0, WeatherThread, job, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete job; fetching_ = false; }
        sv_->Invalidate();
    }
    friend DWORD WINAPI WeatherThread(void*);
    void Done(bool ok, const WeatherData& w, const std::wstring& city, bool f) {
        fetching_ = false;
        if (city != city_ || f != fahrenheit_) { Consider(); return; }  // settings changed meanwhile
        if (ok) {
            data_ = w;
            lastOk_ = w.when;
            fails_ = 0;
            nextTry_ = 0;
            SaveCache();
        } else {
            static const ULONGLONG back[] = {60000, 120000, 300000, 900000};
            nextTry_ = GetTickCount64() + back[std::min(fails_, 3)];
            fails_++;
        }
        sv_->Invalidate();
        if (shown_) Consider();
    }
    std::wstring city_;
    bool fahrenheit_ = true, loaded_ = false, fetching_ = false;
    int fails_ = 0;
    ULONGLONG nextTry_ = 0, lastOk_ = 0;
    WeatherData data_;
};

DWORD WINAPI WeatherThread(void* arg) {
    std::unique_ptr<WeatherJob> j(static_cast<WeatherJob*>(arg));
    WeatherData w;
    const bool ok = FetchWeather(j->city, j->f, w);
    WeatherPage* page = j->self;
    const std::wstring city = j->city;
    const bool f = j->f;
    j->sv->Post([page, ok, w, city, f] { page->Done(ok, w, city, f); });
    return 0;
}

// ---- Focus timer: page and collapsed pill ----
class TimerModule : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-timer"; }
    int DashboardOrder() const override { return 50; }
    int PillPriority() const override { return active_ || finished_ ? 50 : 0; }
    Size2 Measure(View v) override { return v == View::Pill ? Size2{180.0f, kNotchH} : Size2{kPageW, kPageH}; }
    void OnVisibility(View v, bool shown) override {
        if (v == View::Alert) return;
        vis_[v == View::Pill ? 0 : 1] = shown;
        shown_ = vis_[1];
        hover_ = down_ = 0;
        Arm();
    }
    void Draw(const DrawCtx& c) override {
        const float rem = Remaining() / 1000.0f;
        const float frac = total_ > 0 && (active_ || finished_) ? (finished_ ? 1.0f : std::min(rem / total_, 1.0f)) : 0.0f;
        D2D1_COLOR_F col = finished_ ? kRed : isBreak_ ? Rgb(48, 212, 97) : Rgb(255, 148, 0);
        wchar_t tm[16];
        const int secs = (int)ceilf(rem);
        swprintf_s(tm, L"%d:%02d", secs / 60, secs % 60);
        if (c.view == View::Pill) {
            Pg p(c);
            const float h = (c.rect.bottom - c.rect.top) / c.scale;
            p.Ring(24, h / 2, 8, 2, kHairline);
            DrawArcFrac(p, 24, h / 2, 8, frac, 2, col);
            p.Text(finished_ ? L"Done" : tm, 40, 0, 170, h, 13, kFg, 0, 600);
            return;
        }
        Pg p(c);
        hits_.clear();
        p.Ring(86, 92, 44, 5, kTileHover);
        DrawArcFrac(p, 86, 92, 44, frac, 5, col);
        p.Text(active_ || finished_ ? tm : L"Ready", 46, 72, 126, 112, 18, kFg, TF_Center, 600);
        if (!running_ && active_) p.Text(L"Paused", 46, 108, 126, 124, 10, kMuted, TF_Center);
        const wchar_t* label = finished_ ? (isBreak_ ? L"Break Complete" : L"Focus Complete") : active_ ? (isBreak_ ? L"Break Timer" : L"Focus Timer") : L"Focus Timer";
        p.Text(label, 160, 26, 350, 48, 13.5f, finished_ ? kRed : kFg, TF_Ellipsis, 600);
        auto button = [&](int id, const wchar_t* text, float t) {
            p.Fill(160, t, 340, t + 28, 8, hover_ == id ? kTileHover : kTile);
            p.Text(text, 160, t, 340, t + 28, 12, kFg, TF_Center, 600);
            Hit(160, t, 340, t + 28, id);
        };
        if (active_) {
            button(4, running_ ? L"Pause" : L"Resume", 66);
            button(5, L"Stop", 102);
        } else {
            button(1, L"25 min focus", 56);
            button(2, L"50 min focus", 90);
            button(3, L"5 min break", 124);
        }
    }
protected:
    void Click(int id) override {
        const ULONGLONG now = GetTickCount64();
        if (id >= 1 && id <= 3) {
            static const int mins[] = {0, 25, 50, 5};
            total_ = mins[id] * 60;
            isBreak_ = id == 3;
            endsAt_ = now + (ULONGLONG)total_ * 1000;
            active_ = running_ = true;
            finished_ = false;
        } else if (id == 4 && active_) {
            if (running_) { remaining_ = endsAt_ > now ? endsAt_ - now : 0; running_ = false; }
            else { endsAt_ = now + remaining_; running_ = true; }
        } else if (id == 5) {
            active_ = running_ = finished_ = false;
        }
        Arm();
        sv_->Refresh();
    }
private:
    ULONGLONG Remaining() const {
        if (!active_) return 0;
        if (!running_) return remaining_;
        const ULONGLONG now = GetTickCount64();
        return endsAt_ > now ? endsAt_ - now : 0;
    }
    void Tick() {
        const ULONGLONG now = GetTickCount64();
        bool changed = false;
        if (active_ && running_ && Remaining() == 0) {  // computed from endsAt, never by counting ticks
            active_ = running_ = false;
            finished_ = true;
            finishedUntil_ = now + 6000;
            MessageBeep(MB_ICONASTERISK);
            changed = true;
        } else if (finished_ && now >= finishedUntil_) {
            finished_ = false;
            changed = true;
        }
        Arm();
        if (changed) sv_->Refresh(); else sv_->Invalidate();
    }
    // Visible: one paint per displayed second. Hidden: a single one-shot at the deadline. Idle: nothing.
    void Arm() {
        if (!sv_) return;
        const bool visible = vis_[0] || vis_[1];
        const ULONGLONG now = GetTickCount64();
        UINT ms = 0;
        if (finished_) ms = (UINT)(finishedUntil_ > now ? finishedUntil_ - now : 0) + 20;
        else if (active_ && running_) {
            const ULONGLONG rem = Remaining();
            ms = visible ? (UINT)(rem % 1000) + 20 : (UINT)rem + 20;
            if (ms < 20) ms = 20;
        }
        if (ms) sv_->SetTimer(this, ms, [this] { Tick(); });
        else sv_->KillTimer(this);
    }
    bool active_ = false, running_ = false, isBreak_ = false, finished_ = false;
    bool vis_[2] = {false, false};
    int total_ = 0;
    ULONGLONG endsAt_ = 0, remaining_ = 0, finishedUntil_ = 0;
};

IdlePage g_idlePage;
CalendarPage g_calendarPage;
WeatherPage g_weatherPage;
HardwarePage g_hardwarePage;
TimerModule g_timerModule;
