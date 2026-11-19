// ---- Bar placement and panels ----

void IslandMoved();
void IslandReset();
void IslandRecompute();
void SetExpanded(bool expanded);
void RegisterBar();
void UnregisterBar();

struct MonitorList { std::vector<HMONITOR> list; };

BOOL CALLBACK CollectMonitor(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<MonitorList*>(lp)->list.push_back(m);
    return TRUE;
}

// The monitor the bar and its strip live on: the chosen one, else the primary. Follow-mouse keeps the bar on the primary.
HMONITOR BarMonitor() {
    if (g_settings.monitor > 0) {
        MonitorList all;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&all));
        if ((size_t)g_settings.monitor <= all.list.size()) return all.list[g_settings.monitor - 1];
    }
    return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

// Where the island hangs: with the bar, or under the pointer in follow mode.
void UpdateIslandMonitor() {
    RECT want = g_monitor;
    if (g_settings.monitor == -1) {
        POINT p;
        GetCursorPos(&p);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) want = mi.rcMonitor;
    }
    g_isMon = want;
}

bool IslandOnBar() { return EqualRect(&g_isMon, &g_monitor) != FALSE; }

void PlaceBar() {
    if (!g_hwnd || g_hidden) {
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(BarMonitor(), &mi);
    if (!EqualRect(&g_monitor, &mi.rcMonitor)) {
        // Another monitor may have another DPI: move there first, so the height is worked out at its scale.
        SetWindowPos(g_hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, 100, 10, SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    g_monitor = mi.rcMonitor;
    UpdateIslandMonitor();
    const int height = (int)(kBarH * Scale() + 0.5f);

    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = g_hwnd;
    abd.uEdge = ABE_TOP;
    abd.rc = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.top + height};
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.bottom = abd.rc.top + height;
    SHAppBarMessage(ABM_SETPOS, &abd);
    // Placement must never bring the bar back over a fullscreen app.
    SetWindowPos(g_hwnd, HWND_TOPMOST, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left,
                 abd.rc.bottom - abd.rc.top, SWP_NOACTIVATE | (g_fullscreen ? 0 : SWP_SHOWWINDOW));
    IslandMoved();
}

bool ForegroundCoversMonitor() {
    HWND fg = GetForegroundWindow();
    if (!fg || IsShellWindow(fg)) {
        return false;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r;
    GetWindowRect(fg, &r);
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

bool IsDesktopWindow(HWND hwnd) {
    wchar_t cls[32] = {};
    GetClassNameW(hwnd, cls, 32);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
}

// Show Desktop (Win+D, the three-finger swipe down) minimizes, hides or un-topmosts windows, the bar's
// too. The bar refuses those in WM_WINDOWPOSCHANGING; this repairs whatever got through anyway.
void KeepBarUp() {
    if (!g_hwnd || g_fullscreen || g_hidden) return;
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    for (HWND h : {g_hwnd, g_isHwnd}) {
        if (h && !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    if (!IsWindowVisible(g_hwnd)) PlaceBar();
}

// Our own windows only: forced on top, and the bar not hidden unless it is hiding itself.
void GuardBarPos(HWND hwnd, WINDOWPOS* p) {
    if (g_fullscreen || g_hidden) return;
    if (!(p->flags & SWP_NOZORDER) && p->hwndInsertAfter != HWND_TOPMOST) p->hwndInsertAfter = HWND_TOPMOST;
    if (hwnd == g_hwnd) p->flags &= ~SWP_HIDEWINDOW;
}

void UpdateFullscreen() {
    // The desktop in front (Show Desktop) is never a fullscreen app, whatever the last ABN_FULLSCREENAPP said.
    HWND fgNow = GetForegroundWindow();
    g_fsRaw = (g_abnFullscreen && !(fgNow && IsDesktopWindow(fgNow))) || ForegroundCoversMonitor();
    if (!g_fsRaw) g_fsOverride = false;
    const bool fullscreen = g_fsRaw && !g_fsOverride;  // The hotkey can bring the bar back over the app.
    if (fullscreen == g_fullscreen) {
        return;
    }
    g_fullscreen = fullscreen;
    AlertsFullscreenChanged();
    if (g_hidden) {
        return;  // Nothing is shown; the state is kept for when the bar comes back.
    }
    if (fullscreen) {
        ShowWindow(g_hwnd, SW_HIDE);
        // Forget the pointer and a half-open island, so it does not come back expanded.
        g_tracking = false;
        g_hoverId = H_none;
        IslandReset();
        g_clock.Sleep();
        g_lastFrame = 0;
    } else {
        PlaceBar();
        IslandRecompute();
        if (g_pinned) SetExpanded(true);
        InvalidateAll();
    }
}

// Hide releases the reserved strip, so windows get the space back; show takes it again.
void SetHidden(bool hide) {
    if (hide == g_hidden || !g_hwnd) {
        return;
    }
    g_hidden = hide;
    if (hide) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_tracking = false;
        g_hoverId = H_none;
        IslandReset();
        g_clock.Sleep();
        g_lastFrame = 0;
        UnregisterBar();
    } else {
        g_fsOverride = false;
        g_fullscreen = false;
        UpdateFullscreen();
        RegisterBar();
        IslandRecompute();
        if (g_pinned) SetExpanded(true);
        InvalidateAll();
    }
}

// Ctrl+Alt+D. Over a fullscreen app it only shows or hides the bar for that app and keeps the strip as it is.
void ToggleHotkey() {
    if (g_fsRaw && !g_hidden) {
        g_fsOverride = !g_fsOverride;
        UpdateFullscreen();
        return;
    }
    SetHidden(!g_hidden);
}

void RegisterBar() {
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = g_hwnd;
    abd.uCallbackMessage = WM_APPBAR_CB;
    if (SHAppBarMessage(ABM_NEW, &abd)) {
        g_registered = true;
    }
    PlaceBar();
}

void UnregisterBar() {
    if (g_registered) {
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = g_hwnd;
        SHAppBarMessage(ABM_REMOVE, &abd);
        g_registered = false;
    }
}


// ---- Right-click menu: one for every bubble. Pin and hide only; the look has no options. ----

void SetPinned(bool pin) {
    g_pinned = pin;
    Wh_SetIntValue(L"pinned", pin ? 1 : 0);
    if (pin) SetExpanded(true);  // Does nothing while covered or in fullscreen; the pin still stands.
    IslandRecompute();  // Starts or stops the zone poll, which only an unpinned island needs.
}

void ShowBarMenu() {
    if (!g_hwnd) return;
    constexpr UINT kPin = 1, kHide = 2;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (g_pinned ? MF_CHECKED : 0), kPin, Loc(g_pinned ? L"menu.unpin" : L"menu.pin"));
    AppendMenuW(menu, MF_STRING, kHide, Loc(L"menu.hide"));
    POINT p;
    GetCursorPos(&p);
    HWND previous = GetForegroundWindow();  // Handing focus to the menu must not leave the user's app without it.
    SetForegroundWindow(g_hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);  // Lets the menu loop finish cleanly.
    DestroyMenu(menu);
    if (previous && previous != g_hwnd && previous != g_isHwnd && IsWindow(previous)) SetForegroundWindow(previous);
    if (cmd == kPin) SetPinned(!g_pinned);
    else if (cmd == kHide) SetHidden(true);
}
