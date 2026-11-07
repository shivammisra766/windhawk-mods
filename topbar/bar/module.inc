// ---- Module interface: FROZEN at the end of P4. See specs/module-api.md. ----
// Feature modules (media, alerts, dashboards) implement IModule and are listed in g_modules.
// Everything here runs on the UI thread. A module's own workers hand results over with Services::Post.

struct Size2 { float w, h; };  // DIP

enum class View { Pill, Alert, Dashboard };

struct DrawCtx {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* brush;  // Shared. Set its colour before each use; multiply alpha by `alpha`.
    D2D1_RECT_F rect;             // Pixels. Exactly Measure(view) scaled by `scale`, in the island's stage.
    float alpha;                  // Content fade, 0..1.
    float scale;                  // DPI scale: DIP * scale = pixels.
    double now, dt;               // QPC seconds.
    View view;
};

struct PointerEvent {
    enum Kind { Move, Down, Up, Leave } kind;
    D2D1_POINT_2F pt;  // Pixels relative to rect.left, rect.top.
    bool left;
};

enum class PanelKind { Notifications, Calendar };

class Services;

class IModule {
public:
    virtual ~IModule() = default;
    virtual const wchar_t* Id() const = 0;
    virtual void Start(Services&) {}
    virtual void Stop() {}
    virtual void OnSettings(const Settings&) {}

    // Dashboards appear as pages in the expanded island, in DashboardOrder().
    virtual bool HasDashboard() const { return false; }
    virtual int DashboardOrder() const { return 100; }
    // Collapsed: the module with the highest PillPriority() above 0 shows; otherwise the clock.
    virtual int PillPriority() const { return 0; }

    virtual Size2 Measure(View) = 0;  // Called when state changes, not per frame.
    virtual void Draw(const DrawCtx&) = 0;
    virtual bool OnPointer(const PointerEvent&, View) { return false; }  // true: handled
    virtual bool OnWheel(int steps) { return false; }  // Dashboard only; steps > 0 is wheel up. true: consumed, the page does not change.
    virtual void OnClickAlert() {}                                        // The alert was tapped.

    virtual bool Animating(View) const { return false; }  // True keeps the frame clock running.
    virtual void OnVisibility(View, bool shown) {}        // Start or stop costly sampling.
};

class Services {
public:
    // Alerts queue by priority (higher first), then arrival. ms == 0: stays until ClearAlert.
    // An alert that arrives while the island is expanded waits; it does not close the island.
    void ShowAlert(IModule*, int priority, int ms);
    void ClearAlert(IModule*);

    void Invalidate();       // Redraw the island once.
    void Refresh();          // Measure() or PillPriority() changed: recompute the island's state.
    void WakeAnimation();    // Re-check Animating() and run the frame clock.
    void Post(std::function<void()>);  // From any thread to the UI thread.
    void SetTimer(IModule*, UINT ms, std::function<void()>);  // One timer per module; repeats until KillTimer.
    void KillTimer(IModule*);
    void Relayout();         // Re-measure the views after a module's size or presence changed.
    void Expand(IModule*, int ms);  // Open the island on that module's dashboard for at least ms (then hover rules).
    void OpenPanel(PanelKind, int anchorX);  // The native notification or calendar panel.
    const Settings& GetSettings() const;
    const wchar_t* Loc(const wchar_t* key) const;
    TextService& Text();
    // Reserved, added by the module that first needs them: Images() for decoded art, PersistInt.
};

// ---- Stub modules: what P4 ships so the shell can be seen. Later agents replace or add to these. ----

float ToDip(float px) { return px / Scale(); }

// The clock string from the settings: 12/24 hours, seconds, a custom date pattern, date above or beside.
std::wstring ClockText() {
    wchar_t timeText[64] = {};
    wchar_t dateText[128] = {};
    const Settings& st = g_settings;
    const wchar_t* pattern = nullptr;
    DWORD flags = st.clockSeconds ? 0 : TIME_NOSECONDS;
    if (st.clockHours == 24) { pattern = st.clockSeconds ? L"HH':'mm':'ss" : L"HH':'mm"; flags = 0; }
    else if (st.clockHours == 12) { pattern = st.clockSeconds ? L"h':'mm':'ss tt" : L"h':'mm tt"; flags = 0; }
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags, nullptr, pattern, timeText, 64)) wcscpy_s(timeText, L"--:--");
    bool ok = false;
    if (!st.dateFormat.empty()) ok = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, st.dateFormat.c_str(), dateText, 128, nullptr) > 0;
    if (!ok) GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"ddd d MMM", dateText, 128, nullptr);
    return std::wstring(dateText) + (st.dateAbove ? L"\n" : L", ") + timeText;
}

// Width of text with every digit set as the widest one, so a ticking clock never changes the notch's size.
float StableWidth(const std::wstring& text, float size) {
    wchar_t widest = L'0';
    float best = 0;
    for (wchar_t d = L'0'; d <= L'9'; d++) {
        const float w = g_text.Measure(std::wstring(1, d), size);
        if (w > best) { best = w; widest = d; }
    }
    std::wstring t = text;
    for (wchar_t& ch : t) if (ch >= L'0' && ch <= L'9') ch = widest;
    return g_text.Measure(t, size);
}

void DrawCentredText(const DrawCtx& c, const std::wstring& text, D2D1_RECT_F r, float size = 0) {
    D2D1_COLOR_F col = kFg;
    col.a *= c.alpha;
    c.brush->SetColor(col);
    TextOpts o;
    o.maxW = r.right - r.left;
    o.maxH = r.bottom - r.top;
    o.center = true;
    o.size = size;
    g_text.Draw(c.rt, text, r.left, r.top, c.brush, o);
}

void AlertsFullscreenChanged();  // topbar/alerts/alerts.inc
float AlertsDotsReserve();  // Extra pill width (DIP, both sides) while a privacy dot is shown.

void IslandOpenCalendar();  // island.inc: the native calendar, under the island.

// The collapsed content: date and time. A click opens the calendar.
class ClockModule : public IModule {
public:
    const wchar_t* Id() const override { return L"clock"; }
    bool OnPointer(const PointerEvent& e, View v) override {
        if (v != View::Pill) return false;
        if (e.kind == PointerEvent::Leave) down_ = false;
        if (!e.left) return false;
        if (e.kind == PointerEvent::Down) down_ = true;
        if (e.kind == PointerEvent::Up && down_) {
            down_ = false;
            IslandOpenCalendar();
        }
        return true;
    }
    Size2 Measure(View) override {
        const float size = g_settings.dateAbove ? g_text.DefaultSize() * 0.85f : 0.0f;  // Two lines need to be smaller.
        return {ToDip(StableWidth(ClockText(), size)) + 2 * kPillPadX + AlertsDotsReserve(), kNotchH};
    }
    void Draw(const DrawCtx& c) override {
        DrawCentredText(c, ClockText(), c.rect, g_settings.dateAbove ? g_text.DefaultSize() * 0.85f : 0.0f);
    }
private:
    bool down_ = false;
};

ClockModule g_clockModule;
