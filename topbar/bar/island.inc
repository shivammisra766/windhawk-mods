// ---- Island: the centre bubble, a layered window owned by the bar, on the bar's thread ----
// The window is only as big as the notch at that moment. Drawing goes to a retained stage-sized DIB;
// each frame presents the crop that holds the shape, so a settled notch is a small window and an
// idle one costs nothing. Hover, alerts and tabs are decided here; modules draw the content.

LayeredSurface g_isSurface;
Com<ID2D1SolidColorBrush> g_isBrush;
unsigned g_isBrushGen = 0;

enum class IState { Collapsed, Alert, Expanded };
struct ViewSel {
    IModule* m = nullptr;
    View v = View::Pill;
    bool operator==(const ViewSel& o) const { return m == o.m && v == o.v; }
    bool operator!=(const ViewSel& o) const { return !(*this == o); }
};
struct AlertItem {
    IModule* m;
    int priority;
    ULONGLONG expires;  // 0: until cleared
    ULONGLONG seq;
};

// Provided by the alerts module (topbar/alerts), which is listed after this file.
void AlertsDrawOverlay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float L, float R, float B, float s);
void AlertsPublishGeometry();
bool AlertsHoldsHover();  // The alert on show is a slider: hovering it must not expand the island over it.

IState g_isState = IState::Collapsed;
Spring g_cover;         // 0 island shown, 1 faded out under the native volume popup (P5b).
Spring g_isW, g_isH;    // Body size, pixels.
Spring g_wing;          // 0 bell and calendar parked, 1 out.
Spring g_mix;           // Cross-fade between g_viewPrev and g_viewCur.
ViewSel g_viewCur, g_viewPrev;
Size2 g_curSize{}, g_prevSize{};  // Measure() of each, taken when state changes.
double g_frameDt = 0;
bool g_isInit = false;
bool g_expanded = false;   // Hover (after the dwell) or pinned.
bool g_covered = false;    // Reserved for the native volume popup (P5b): blocks hover expansion.
bool g_isTracking = false;
bool g_dwelling = false;
int g_tab = 0;
int g_tabDir = 1;  // Direction of the last page change, for the slide.
int g_wheelAcc = 0;
ULONGLONG g_leaveSince = 0;
ULONGLONG g_wingHold = 0;
ULONGLONG g_autoUntil = 0;  // Services::Expand: stays open until then even without the pointer.
ULONGLONG g_alertSeq = 0;
std::vector<AlertItem> g_alerts;
RECT g_isBody{};        // Screen rect of the body as drawn.
RECT g_isTarget{};      // Screen rect of the body when the springs have settled (no shoulders).
D2D1_RECT_F g_isView{}; // Stage rect of the view last drawn, for pointer events.
int g_isCropX = 0;

struct ModuleTimer { IModule* m; std::function<void()> fn; };
std::vector<ModuleTimer> g_moduleTimers;

float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

std::vector<IModule*> g_lastDashes;  // The page list as of the last IslandRecompute.

std::vector<IModule*> Dashboards() {
    std::vector<IModule*> out;
    for (IModule* m : g_modules) if (m->HasDashboard()) out.push_back(m);
    std::stable_sort(out.begin(), out.end(), [](IModule* a, IModule* b) { return a->DashboardOrder() < b->DashboardOrder(); });
    return out;
}

IModule* PillModule() {
    IModule* best = &g_clockModule;
    int priority = 0;
    for (IModule* m : g_modules) {
        if (m->PillPriority() > priority) { priority = m->PillPriority(); best = m; }
    }
    return best;
}

IModule* TopAlert() {
    const AlertItem* best = nullptr;
    for (const AlertItem& a : g_alerts) {
        if (!best || a.priority > best->priority || (a.priority == best->priority && a.seq < best->seq)) best = &a;
    }
    return best ? best->m : nullptr;
}

// Widest dashboard any module declares: the bar caps its left bubble by it, so nothing reflows.
float IslandMaxExpandedW() {
    float w = 0;
    for (IModule* m : g_modules) if (m->HasDashboard()) w = std::max(w, m->Measure(View::Dashboard).w);
    return w > 0 ? std::min(w, kIslandMaxW) : 475.0f;
}

bool PanelWingsWanted() {
    HWND fg = GetForegroundWindow();
    return fg && IsShellExperienceWindow(fg) && (g_panelMode == kPanelNotifications || g_panelMode == kPanelCalendar);
}

void AimIfChanged(Spring& sp, float target, float k, float c) {
    if (fabsf(sp.target - target) > 0.01f) sp.Aim(target, k, c);
}

bool IslandSettled() {
    return g_isW.Settled() && g_isH.Settled() && g_wing.Settled() && g_mix.Settled() && g_cover.Settled();
}

// Derives everything from the inputs: alerts, hover, tab, panel. Called when an input changes, never per frame.
void IslandRecompute() {
    if (!g_isHwnd || g_fullscreen || g_hidden || !EnsureGfx()) return;
    PrepareText();
    const ULONGLONG now = GetTickCount64();
    g_alerts.erase(std::remove_if(g_alerts.begin(), g_alerts.end(), [&](const AlertItem& a) { return a.expires && a.expires <= now; }),
                   g_alerts.end());
    const std::vector<IModule*> dashes = Dashboards();
    // Pages come and go (media only has one while something plays): keep the tab on the same page, not the same index.
    if (g_expanded && g_tab < (int)g_lastDashes.size()) {
        const auto it = std::find(dashes.begin(), dashes.end(), g_lastDashes[g_tab]);
        if (it != dashes.end()) g_tab = (int)(it - dashes.begin());
    }
    g_lastDashes = dashes;
    if (g_tab >= (int)dashes.size()) g_tab = std::max(0, (int)dashes.size() - 1);

    IState state = IState::Collapsed;
    if (g_expanded && !dashes.empty()) state = IState::Expanded;
    else if (!g_alerts.empty()) state = IState::Alert;
    if (g_covered && state == IState::Alert) state = IState::Collapsed;  // Alerts wait under the popup.
    ViewSel want{PillModule(), View::Pill};
    if (state == IState::Expanded) want = {dashes[g_tab], View::Dashboard};
    else if (state == IState::Alert) want = {TopAlert(), View::Alert};

    const float s = Scale();
    const Size2 size = want.m->Measure(want.v);
    const float tw = std::min(std::max(size.w, 40.0f), kIslandMaxW) * s;
    const float th = std::min(std::max(size.h, 20.0f), kIslandMaxH) * s;

    const bool wings = IslandOnBar() && (state == IState::Expanded || PanelWingsWanted() || now < g_wingHold);  // The buttons belong to the bar's monitor.
    if (!g_isInit) {
        g_isInit = true;
        g_isW.eps = g_isH.eps = 0.25f;  // Pixels.
        g_isW.epsV = g_isH.epsV = 5.0f;
        g_isW.Snap(tw);
        g_isH.Snap(th);
        g_wing.Snap(wings ? 1.0f : 0.0f);
        g_wingT = g_wing.x;
        g_mix.Snap(1.0f);
        g_viewCur = want;
        g_curSize = size;
        g_viewPrev = ViewSel{};
    } else {
        AimIfChanged(g_isW, tw, tw > g_isW.x ? kSpringExpandK : kSpringCollapseK, tw > g_isW.x ? kSpringExpandC : kSpringCollapseC);
        AimIfChanged(g_isH, th, th > g_isH.x ? kSpringExpandK : kSpringCollapseK, th > g_isH.x ? kSpringExpandC : kSpringCollapseC);
        AimIfChanged(g_wing, wings ? 1.0f : 0.0f, 320.0f, 34.0f);
        if (want != g_viewCur) {
            if (g_viewCur.m) g_viewCur.m->OnVisibility(g_viewCur.v, false);
            g_viewPrev = g_viewCur;
            g_prevSize = g_curSize;
            g_viewCur = want;
            g_mix.x = 0;
            g_mix.v = 0;
            g_mix.Aim(1.0f, 250.0f, 31.0f);
            if (want.m) want.m->OnVisibility(want.v, true);
        }
    }
    g_curSize = size;
    g_isState = state;
    AimIfChanged(g_cover, (g_covered && state != IState::Expanded) ? 1.0f : 0.0f, 900.0f, 60.0f);  // About 80 ms.

    // Where the body will stand once settled, for the pointer tests.
    const float cx = (g_isMon.left + g_isMon.right) / 2.0f;
    g_isTarget = {(LONG)roundf(cx - tw / 2.0f), g_isMon.top, (LONG)roundf(cx + tw / 2.0f), g_isMon.top + (LONG)roundf(th)};

    if (state == IState::Expanded) SetTimer(g_hwnd, kZoneTimer, kZonePollMs, nullptr);
    else KillTimer(g_hwnd, kZoneTimer);

    ULONGLONG next = 0;
    for (const AlertItem& a : g_alerts) if (a.expires && (!next || a.expires < next)) next = a.expires;
    if (next) SetTimer(g_hwnd, kAlertTimer, (UINT)std::max<ULONGLONG>(next - now, 10) + 5, nullptr);
    else KillTimer(g_hwnd, kAlertTimer);
    if (now < g_wingHold) SetTimer(g_hwnd, kWingTimer, (UINT)(g_wingHold - now) + 30, nullptr);
    else KillTimer(g_hwnd, kWingTimer);

    g_isDirty = true;
    if (!IslandSettled()) {
        g_lastFrame = 0;
        g_clock.Wake();
    }
    RequestPaint();
    AlertsPublishGeometry();
}

void IslandRescale() {  // DPI changed: sizes are pixels, so start over from the new scale.
    g_isInit = false;
    IslandRecompute();
}

void IslandMoved() {  // The bar moved (monitor, work area): the notch follows rcMonitor.
    g_isDirty = true;
    RequestPaint();
}

bool ShelfHoldsOpen();  // shelf.inc: a drag is over the bar or the island, or just left it.
// The native volume or brightness popup is up: fade the island out, pause hover and alerts. If it is
// already expanded it stays (the popup sits above it).
void SetCovered(bool covered) {
    if (g_covered == covered) return;
    g_covered = covered;
    if (covered && g_dwelling) {
        g_dwelling = false;
        KillTimer(g_hwnd, kDwellTimer);
    }
    IslandRecompute();
}

void SetExpanded(bool expanded) {
    if (g_expanded == expanded) return;
    g_expanded = expanded;
    if (!expanded) g_tab = 0;  // Always open on the first page.
    g_leaveSince = 0;
    IslandRecompute();
}

// Fullscreen: forget the pointer and any open state, and hide. IslandRecompute brings it back.
void IslandReset() {
    g_expanded = false;
    g_dwelling = false;
    g_isTracking = false;
    g_leaveSince = 0;
    g_wingHold = 0;
    g_autoUntil = 0;
    if (g_hwnd) {
        KillTimer(g_hwnd, kDwellTimer);
        KillTimer(g_hwnd, kZoneTimer);
        KillTimer(g_hwnd, kWingTimer);
    }
    g_isInit = false;
    g_wing.Snap(0.0f);
    g_wingT = 0.0f;
    if (g_isHwnd) ShowWindow(g_isHwnd, SW_HIDE);
}

// Every 50 ms while expanded: is the pointer in the island, or in the strip band the buttons run along?
void IslandZoneTick() {
    if (g_isState != IState::Expanded) return;
    POINT p;
    GetCursorPos(&p);
    const float s = Scale();
    RECT zone = g_isTarget;
    InflateRect(&zone, (int)ceilf(kNotchShoulder * s), 0);
    bool in = PtInRect(&zone, p) != FALSE;
    // The strip band from the left edge of the bell to the right edge of the calendar keeps hover alive across the gaps.
    int lo = zone.left, hi = zone.right;
    if (IslandOnBar() && g_wingOk[0]) lo = std::min<int>(lo, g_monitor.left + (int)g_wingX[0]);
    if (IslandOnBar() && g_wingOk[1]) hi = std::max<int>(hi, g_monitor.left + (int)(g_wingX[1] + g_wingSize));
    const RECT band{lo, g_isMon.top, hi, g_isMon.top + (LONG)(kBarH * s + 0.5f)};
    if (PtInRect(&band, p)) in = true;
    // Over the settled body, a window that covers it (a panel, a popup) counts as leaving.
    if (in && IslandSettled() && PtInRect(&g_isTarget, p)) {
        HWND at = WindowFromPoint(p);
        if (at != g_isHwnd && at != g_hwnd) in = false;
    }
    if (in || g_pinned || GetTickCount64() < g_autoUntil || ShelfHoldsOpen()) {
        g_leaveSince = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_leaveSince) g_leaveSince = now;
    else if (now - g_leaveSince >= kLeaveGraceMs) SetExpanded(false);
}

void IslandDwellTick() {
    KillTimer(g_hwnd, kDwellTimer);
    g_dwelling = false;
    POINT p;
    GetCursorPos(&p);
    if (g_isState == IState::Alert && AlertsHoldsHover()) return;  // A slider came up during the dwell.
    if (!g_covered && !g_fullscreen && PtInRect(&g_isBody, p)) SetExpanded(true);
}

// Notch outline from the shell spec: flat top at the screen edge, concave shoulders, convex bottom corners.
Com<ID2D1PathGeometry> NotchGeometry(float L, float R, float B, float r, float rs, bool edgeOnly) {
    Com<ID2D1PathGeometry> geo;
    if (FAILED(g_factory->CreatePathGeometry(geo.Put()))) return {};
    Com<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(sink.Put()))) return {};
    const auto cw = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    const auto ccw = D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    if (edgeOnly) {  // Open path for the faint outline: no top edge, no shoulders.
        sink->BeginFigure(D2D1::Point2F(L, rs), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(L, B - r));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(L + r, B), D2D1::SizeF(r, r), 0, ccw, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(R - r, B));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(R, B - r), D2D1::SizeF(r, r), 0, ccw, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(R, rs));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
    } else {
        sink->BeginFigure(D2D1::Point2F(L - rs, 0), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(L, rs), D2D1::SizeF(rs, rs), 0, cw, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(L, B - r));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(L + r, B), D2D1::SizeF(r, r), 0, ccw, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(R - r, B));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(R, B - r), D2D1::SizeF(r, r), 0, ccw, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(R, rs));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(R + rs, 0), D2D1::SizeF(rs, rs), 0, cw, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }
    sink->Close();
    return geo;
}

void IslandPaint() {
    if (!g_isDirty || !g_isHwnd) return;
    g_isDirty = false;
    if (g_fullscreen || g_hidden || !g_isInit || !EnsureGfx()) return;
    PrepareText();
    g_isDirty = false;

    const float s = Scale();
    const float rs = kNotchShoulder * s;
    int stageW = (int)ceilf((kIslandMaxW + 2 * kNotchShoulder) * s) + 8;
    stageW += stageW & 1;  // Even, so the centre is a whole pixel.
    const int stageH = (int)ceilf(kIslandMaxH * s) + 4;
    if (!g_isSurface.Resize(g_factory.Get(), stageW, stageH)) return;
    ID2D1DCRenderTarget* rt = g_isSurface.Target();
    if (!rt) return;
    if (!g_isBrush || g_isBrushGen != g_isSurface.Generation()) {
        g_isBrush.Reset();
        rt->CreateSolidColorBrush(kFg, g_isBrush.Put());
        g_isBrushGen = g_isSurface.Generation();
    }
    ID2D1SolidColorBrush* brush = g_isBrush.Get();
    if (!brush) return;

    const float w = roundf(fmaxf(g_isW.x, 2.0f)), h = roundf(fmaxf(g_isH.x, 2.0f));
    const float cx = stageW / 2.0f;
    const float L = cx - floorf(w / 2.0f), R = L + w, B = h;
    const float grow = Clamp01((h - kNotchH * s) / (40.0f * s));
    const float r = std::min((kNotchRadius + (kNotchRadiusOpen - kNotchRadius) * grow) * s, std::min(w, h) / 2.0f);
    const int cl = std::max(0, (int)floorf(L - rs) - 1);
    const int cr = std::min(stageW, (int)ceilf(R + rs) + 1);
    const int cb = std::min(stageH, (int)ceilf(B) + 2);
    const RECT crop{cl, 0, cr, cb};

    if (!g_isSurface.Begin(crop)) return;
    brush->SetColor(kCentreBg);
    if (Com<ID2D1PathGeometry> body = NotchGeometry(L, R, B, r, rs, false)) rt->FillGeometry(body.Get(), brush);
    if (grow > 0.02f) {  // Faint edge once open, so a dark overlay stays apart from dark apps. Never along the top.
        if (Com<ID2D1PathGeometry> edge = NotchGeometry(L + 0.5f, R - 0.5f, B - 0.5f, r, rs, true)) {
            D2D1_COLOR_F c = Rgb(255, 255, 255, 0x30);
            c.a *= grow;
            brush->SetColor(c);
            rt->DrawGeometry(edge.Get(), brush, 1.0f);
        }
    }

    // Content, laid out at its own size and clipped to the body, inset so the corners never touch it.
    const double now = QpcSeconds();
    rt->PushAxisAlignedClip(D2D1::RectF(L + r * 0.5f, 0, R - r * 0.5f, B - r * 0.25f), D2D1_ANTIALIAS_MODE_ALIASED);
    auto drawView = [&](const ViewSel& v, const Size2& sz, float alpha, float slide) {
        if (!v.m || alpha <= 0.01f) return;
        const float vw = sz.w * s, vh = sz.h * s;
        const bool pages = v.v == View::Dashboard && g_viewCur.v == View::Dashboard && g_viewPrev.v == View::Dashboard;
        const float dx = pages ? slide * s : 0.0f;  // Page change: travel 34 px, in the direction of the scroll.
        DrawCtx c{rt, brush, D2D1::RectF(cx - vw / 2.0f + dx, 0, cx + vw / 2.0f + dx, vh), alpha, s, now, g_frameDt, v.v};
        v.m->Draw(c);
        if (v == g_viewCur) g_isView = D2D1::RectF(cx - vw / 2.0f, 0, cx + vw / 2.0f, vh);
    };
    const float mix = Clamp01(g_mix.x);
    if (mix < 1.0f) drawView(g_viewPrev, g_prevSize, 1.0f - mix, -g_tabDir * 34.0f * mix);
    drawView(g_viewCur, g_curSize, mix, g_tabDir * 34.0f * (1.0f - mix));
    if (g_viewCur.v == View::Dashboard) {
        const int n = (int)Dashboards().size();
        if (n > 1) {  // A column at the right edge, so rows and cards keep the bottom.
            const float step = 9.0f * s, x = R - 10.0f * s, cyDots = B / 2.0f;
            for (int i = 0; i < n; i++) {
                D2D1_COLOR_F c = kFg;
                c.a *= mix * (i == g_tab ? 0.9f : 0.22f);
                brush->SetColor(c);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cyDots + (i - (n - 1) / 2.0f) * step), 2.5f * s, 2.5f * s), brush);
            }
        }
    }
    rt->PopAxisAlignedClip();
    AlertsDrawOverlay(rt, brush, L, R, B, s);

    if (!g_isSurface.End()) {
        g_isBrush.Reset();
        g_isDirty = true;
        RequestPaint();
        return;
    }
    const int x0 = (g_isMon.left + g_isMon.right) / 2 - stageW / 2;
    g_isCropX = cl;
    g_isBody = {x0 + (LONG)L, g_isMon.top, x0 + (LONG)R, g_isMon.top + (LONG)B};
    g_isSurface.PresentCrop(g_isHwnd, POINT{x0 + cl, g_isMon.top}, POINT{cl, 0}, SIZE{cr - cl, cb},
                            (BYTE)(255.0f * (1.0f - Clamp01(g_cover.x)) + 0.5f));
    if (!IsWindowVisible(g_isHwnd)) ShowWindow(g_isHwnd, SW_SHOWNOACTIVATE);
}

void IslandPointer(PointerEvent::Kind kind, LPARAM lp, bool left) {
    if (!g_viewCur.m) return;
    PointerEvent e{kind, D2D1::Point2F((float)(short)LOWORD(lp) + g_isCropX - g_isView.left, (float)(short)HIWORD(lp) - g_isView.top), left};
    g_viewCur.m->OnPointer(e, g_viewCur.v);
}

LRESULT CALLBACK IslandProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!g_isTracking) {
                g_isTracking = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
            }
            // Event-driven while collapsed: the pixels of the notch start the dwell, nothing polls.
            // Not over a slider card: expanding would replace it with a dashboard under the pointer.
            if (!g_expanded && !g_covered && !g_dwelling && !g_fullscreen && !(g_isState == IState::Alert && AlertsHoldsHover())) {
                g_dwelling = true;
                SetTimer(g_hwnd, kDwellTimer, kDwellMs, nullptr);
            }
            IslandPointer(PointerEvent::Move, lp, false);
            return 0;
        case WM_MOUSELEAVE:
            g_isTracking = false;
            if (g_dwelling) {  // Brushed past: no expansion.
                g_dwelling = false;
                KillTimer(g_hwnd, kDwellTimer);
            }
            IslandPointer(PointerEvent::Leave, 0, false);
            return 0;
        case WM_MOUSEWHEEL:
            if (g_expanded) {
                g_wheelAcc += GET_WHEEL_DELTA_WPARAM(wp);
                const int steps = g_wheelAcc / WHEEL_DELTA;
                if (steps) {
                    g_wheelAcc -= steps * WHEEL_DELTA;
                    const int count = (int)Dashboards().size();
                    if (g_viewCur.m && g_viewCur.v == View::Dashboard && g_viewCur.m->OnWheel(steps)) return 0;
                    const int tab = std::min(std::max(g_tab - steps, 0), std::max(count - 1, 0));
                    if (tab != g_tab) {
                        g_tabDir = tab > g_tab ? 1 : -1;
                        g_tab = tab;
                        IslandRecompute();
                    }
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
            IslandPointer(PointerEvent::Down, lp, true);
            return 0;
        case WM_LBUTTONUP:
            if (g_isState == IState::Alert) {
                IslandPointer(PointerEvent::Up, lp, true);  // Ends a slider drag; other cards ignore it.
                if (IModule* m = TopAlert()) m->OnClickAlert();
            } else {
                IslandPointer(PointerEvent::Up, lp, true);
            }
            return 0;
        case WM_RBUTTONUP:
            ShowBarMenu();
            return 0;
        case WM_DESTROY:
            ShelfDropRevoke(hwnd);
            g_isHwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Services: what modules call ----

Services g_services;

void Services::ShowAlert(IModule* m, int priority, int ms) {
    g_alerts.erase(std::remove_if(g_alerts.begin(), g_alerts.end(), [&](const AlertItem& a) { return a.m == m; }), g_alerts.end());
    g_alerts.push_back({m, priority, ms > 0 ? GetTickCount64() + ms : 0, ++g_alertSeq});
    IslandRecompute();
}

void Services::ClearAlert(IModule* m) {
    g_alerts.erase(std::remove_if(g_alerts.begin(), g_alerts.end(), [&](const AlertItem& a) { return a.m == m; }), g_alerts.end());
    IslandRecompute();
}

void Services::Invalidate() {
    g_isDirty = true;
    RequestPaint();
}

void Services::Refresh() { IslandRecompute(); }
unsigned IslandSurfaceGen() { return g_isSurface.Generation(); }
void IslandRepaint() {
    g_isDirty = true;
    RequestPaint();
}

// A drag reached the bar: show the shelf (the island opens over apps; nothing resizes).
void IslandOpenShelf() {
    const std::vector<IModule*> dashes = Dashboards();
    for (size_t i = 0; i < dashes.size(); i++) {
        if (dashes[i] == ShelfModule()) {
            if (g_tab != (int)i) g_tabDir = (int)i > g_tab ? 1 : -1;
            g_tab = (int)i;
        }
    }
    g_expanded = true;
    g_leaveSince = 0;
    IslandRecompute();
}

void Services::WakeAnimation() {
    g_lastFrame = 0;
    g_clock.Wake();
}

void Services::Post(std::function<void()> fn) {
    auto* heap = new std::function<void()>(std::move(fn));
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_POST, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
}

void Services::SetTimer(IModule* m, UINT ms, std::function<void()> fn) {
    size_t slot = 0;
    while (slot < g_moduleTimers.size() && g_moduleTimers[slot].m != m) slot++;
    if (slot == g_moduleTimers.size()) g_moduleTimers.push_back({m, nullptr});
    g_moduleTimers[slot].fn = std::move(fn);
    ::SetTimer(g_hwnd, kModuleTimerBase + slot, ms, nullptr);
}

void Services::KillTimer(IModule* m) {
    for (size_t i = 0; i < g_moduleTimers.size(); i++) {
        if (g_moduleTimers[i].m == m) {
            ::KillTimer(g_hwnd, kModuleTimerBase + i);
            g_moduleTimers[i].fn = nullptr;
        }
    }
}

void Services::Relayout() { IslandRecompute(); }

void Services::Expand(IModule* m, int ms) {
    if (!g_isHwnd || g_fullscreen || g_covered || !g_alerts.empty()) return;  // An alert owns the island; do not cover it.
    const std::vector<IModule*> dashes = Dashboards();
    g_lastDashes = dashes;
    int idx = -1;
    for (size_t i = 0; i < dashes.size(); i++) if (dashes[i] == m) idx = (int)i;
    if (idx < 0) return;
    g_tab = idx;
    g_autoUntil = GetTickCount64() + ms;
    g_expanded = true;
    g_leaveSince = 0;
    IslandRecompute();
}

void Services::OpenPanel(PanelKind kind, int anchorX) {
    g_anchorX = anchorX;
    g_anchorTick = GetTickCount64();
    SetPropW(g_hwnd, L"TopBarAnchorX", reinterpret_cast<HANDLE>((INT_PTR)anchorX));
    SetPropW(g_hwnd, L"TopBarAnchorTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
    TogglePanel(kind == PanelKind::Notifications ? kPanelNotifications : kPanelCalendar);
}

const Settings& Services::GetSettings() const { return g_settings; }
const wchar_t* Services::Loc(const wchar_t* key) const { return ::Loc(key); }
TextService& Services::Text() { return g_text; }

void RunModuleTimer(UINT_PTR id) {
    const size_t slot = id - kModuleTimerBase;
    if (slot < g_moduleTimers.size() && g_moduleTimers[slot].fn) {
        auto fn = g_moduleTimers[slot].fn;
        fn();
    }
}

bool ModulesAnimating() {
    return g_viewCur.m && g_viewCur.m->Animating(g_viewCur.v);
}

bool IslandInit() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = IslandProc;
    wc.hInstance = inst;
    wc.lpszClassName = kIslandClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // Owned by the bar: always above it, no z-order upkeep, and it is hidden with it.
    g_isHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kIslandClass,
                               L"Top bar island", WS_POPUP, 0, 0, 10, 10, g_hwnd, nullptr, inst, nullptr);
    if (!g_isHwnd) return false;
    for (IModule* m : g_modules) m->Start(g_services);
    IslandRecompute();
    if (g_pinned) SetExpanded(true);
    return true;
}

void IslandShutdown() {
    for (IModule* m : g_modules) m->Stop();
    g_moduleTimers.clear();
    g_isBrush.Reset();
    g_isSurface.Destroy();
    if (g_isHwnd) DestroyWindow(g_isHwnd);
    UnregisterClassW(kIslandClass, GetModuleHandleW(nullptr));
}
