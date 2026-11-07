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
    void WakeAnimation();    // Re-check Animating() and run the frame clock.
    void Post(std::function<void()>);  // From any thread to the UI thread.
    void SetTimer(IModule*, UINT ms, std::function<void()>);  // One timer per module; repeats until KillTimer.
    void KillTimer(IModule*);
    void OpenPanel(PanelKind, int anchorX);  // The native notification or calendar panel.
    const Settings& GetSettings() const;
    const wchar_t* Loc(const wchar_t* key) const;
    TextService& Text();
    // Reserved, added by the module that first needs them: Images() for decoded art, PersistInt.
};

// ---- Stub modules: what P4 ships so the shell can be seen. Later agents replace or add to these. ----

float ToDip(float px) { return px / Scale(); }

std::wstring ClockText() {
    wchar_t timeText[32] = {};
    wchar_t dateText[32] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"hh':'mm tt", timeText, 32);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"ddd d MMM", dateText, 32, nullptr);
    return std::wstring(dateText) + L", " + timeText;
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

// The collapsed content: date and time.
class ClockModule : public IModule {
public:
    const wchar_t* Id() const override { return L"clock"; }
    Size2 Measure(View) override { return {ToDip(g_text.Measure(ClockText())) + 2 * kPillPadX + AlertsDotsReserve(), kNotchH}; }
    void Draw(const DrawCtx& c) override { DrawCentredText(c, ClockText(), c.rect); }
};

// Stub dashboard page. Replaced by the real dashboards (P6c).
class PlaceholderPage : public IModule {
public:
    PlaceholderPage(int index, int count) : index_(index), count_(count) {}
    const wchar_t* Id() const override { return index_ == 1 ? L"page-1" : L"page-2"; }
    bool HasDashboard() const override { return true; }
    int DashboardOrder() const override { return index_ * 10; }
    Size2 Measure(View) override { return {475.0f, 140.0f}; }
    void Draw(const DrawCtx& c) override {
        wchar_t line[64];
        swprintf_s(line, L"Placeholder page %d of %d", index_, count_);
        const float h = c.rect.bottom - c.rect.top;
        DrawCentredText(c, line, D2D1::RectF(c.rect.left, c.rect.top + h * 0.22f, c.rect.right, c.rect.top + h * 0.5f), g_text.DefaultSize() * 1.4f);
        DrawCentredText(c, L"Scroll to switch pages", D2D1::RectF(c.rect.left, c.rect.top + h * 0.5f, c.rect.right, c.rect.top + h * 0.7f));
    }
private:
    int index_, count_;
};

ClockModule g_clockModule;
PlaceholderPage g_page1(1, 2), g_page2(2, 2);
IModule* AlertsModuleInstance();  // topbar/alerts/alerts.inc
IModule* const g_modules[] = {&g_clockModule, AlertsModuleInstance(), &g_page1, &g_page2};
