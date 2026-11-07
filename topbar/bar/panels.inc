void SendWinKey(WORD vk) {
    INPUT in[4]{};
    int n = 0;
    in[n].type = INPUT_KEYBOARD;
    in[n++].ki.wVk = VK_LWIN;
    if (vk) {
        in[n].type = INPUT_KEYBOARD;
        in[n++].ki.wVk = vk;
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = vk;
        in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = VK_LWIN;
    in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(n, in, sizeof(INPUT));
}

bool IsShellExperienceWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow")) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(proc, 0, path, &size) && wcsstr(path, L"\\ShellExperienceHost.exe");
    CloseHandle(proc);
    return ok;
}

bool IsQuickSettingsWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return !wcscmp(cls, L"ControlCenterWindow");
}

// The bar opened the calendar a moment ago, so the panel host now taking focus is that calendar.
bool CalendarOpenWanted() {
    return g_panelMode == kPanelCalendar && GetTickCount64() - g_calendarTick < (ULONGLONG)kCalendarClaimMs;
}

// The notification panel opens wherever its window was left, so before the bar moves it for the
// calendar, remember the native place, and put it back before a native open. A place that does not
// end near the bottom of the work area was left by an older bar: use the native corner instead.
void SaveNativePanelRect(HWND hwnd) {
    if (g_panelMoved || IsQuickSettingsWindow(hwnd)) {
        return;
    }
    RECT r;
    if (!GetWindowRect(hwnd, &r)) {
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int slack = (int)(64.0f * Scale());
    if (r.bottom < mi.rcWork.bottom - slack || r.bottom > mi.rcMonitor.bottom + slack) {
        const int w = r.right - r.left, h = r.bottom - r.top;
        r.right = mi.rcMonitor.right;
        r.left = r.right - w;
        r.bottom = mi.rcWork.bottom;
        r.top = r.bottom - h;
    }
    g_nativePanelRect = r;
    g_panelMoved = true;
}

void RestoreNativePanel(HWND hwnd) {
    if (!g_panelMoved || !hwnd || !IsWindow(hwnd)) {
        return;
    }
    g_panelMoved = false;
    RECT r;
    if (GetWindowRect(hwnd, &r) && (r.left != g_nativePanelRect.left || r.top != g_nativePanelRect.top)) {
        SetWindowPos(hwnd, nullptr, g_nativePanelRect.left, g_nativePanelRect.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
}

// Centred under the clicked icon (the calendar: under the island).
int AnchoredPanelX(HWND, const RECT& box, const MONITORINFO& mi, float s) {
    const int edge = (int)(kPanelEdge * s + 0.5f);
    int x = g_anchorX - (box.left + box.right) / 2;
    if (x + box.right > mi.rcMonitor.right - edge) x = mi.rcMonitor.right - edge - box.right;
    if (x + box.left < mi.rcMonitor.left + edge) x = mi.rcMonitor.left + edge - box.left;
    return x;
}

// These panels are tall windows clipped to a region around their content, and they
// only paint inside it. Moving the window is what puts the content under the bar;
// moving the content inside the window would push it out of the region.
void PlacePanel(HWND hwnd) {
    // Only Quick Settings and the bar's calendar hang from the bar; the notifications stay native.
    if (!IsQuickSettingsWindow(hwnd) && g_panelMode != kPanelCalendar) {
        return;
    }
    RECT box;
    const int kind = GetWindowRgnBox(hwnd, &box);
    if (kind == ERROR || kind == NULLREGION) {
        return;
    }
    RECT bar, r;
    GetWindowRect(g_hwnd, &bar);
    GetWindowRect(hwnd, &r);
    const float s = Scale();
    const ULONGLONG now = GetTickCount64();

    // Centred under the icon that was clicked. Opened from the keyboard, it goes on its usual
    // side: Quick Settings is put back there by Windows, the notification panel is not.
    const bool anchored = now - g_anchorTick < kPanelAnchorMs;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = r.left;
    if (anchored) {
        x = AnchoredPanelX(hwnd, box, mi, s);
    } else if (!IsQuickSettingsWindow(hwnd)) {
        x = mi.rcMonitor.right - box.right;
    }
    if (anchored && !IsQuickSettingsWindow(hwnd) && g_panelMode < 3) {
        g_regionTop[g_panelMode] = box.top;
    }

    int y = bar.bottom + (int)(kPanelGap * s + 0.5f) - box.top;
    const ULONGLONG elapsed = now - g_panelOpenTick;
    if (elapsed < kPanelDropMs) {
        const float t = 1.0f - (float)elapsed / kPanelDropMs;
        y -= (int)(kPanelDrop * s * t * t * t);
    }
    // The drop (10) is taller than the gap (8), so it used to lift Quick Settings' top above the
    // work area, under the bar. Quick Settings sizes its content and clip to the room it has, so
    // that is the likely cause of its cut-off tiles. Its top now never goes above the bar's bottom.
    if (IsQuickSettingsWindow(hwnd) && y + box.top < mi.rcWork.top) {
        y = mi.rcWork.top - box.top;
    }
    if (x != r.left || y != r.top) {
        SaveNativePanelRect(hwnd);
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
}

// The region settles shortly after a panel opens or switches mode, and the drop
// runs during that time, so follow the panel at frame rate, briefly.
ULONGLONG g_panelDeadline = 0;  // The follow timer never runs past this (a Win+N that was ignored).

void FollowPanel() {
    g_panelDeadline = GetTickCount64() + 5000;
    g_panelTicks = 26;
    SetTimer(g_hwnd, kPanelTimer, 16, nullptr);
}

// EnumWindows from outside the host does not return its windows; FindWindowEx does.
void SendPanelMode(WPARAM mode) {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"Windows.UI.Core.CoreWindow", nullptr))) {
        if (IsShellExperienceWindow(hwnd)) {
            DWORD_PTR result = 0;
            SendMessageTimeoutW(hwnd, g_panelMessage, mode, 0, SMTO_ABORTIFHUNG, 50, &result);
        }
    }
}

void StepPanelFollow() {
    HWND fg = GetForegroundWindow();
    if (GetTickCount64() > g_panelDeadline) {
        KillTimer(g_hwnd, kPanelTimer);
        RemovePropW(g_hwnd, L"TopBarPanelHideTick");
        g_panelCold = false;
        return;
    }
    if (g_panelCold && g_panelTicks <= 1) {
        g_panelTicks = 26;  // A host starting from nothing can take far longer than usual.
    }
    if (fg && IsShellExperienceWindow(fg) && g_panelCold) {
        // First open since the host started: its clip was sized before the mode could arrive,
        // so reopen once, now that it can be set beforehand.
        g_panelCold = false;
        g_panelResend = 0;
        SendWinKey('N');  // The clip comes just after the focus, so hiding lasts until the reopen below.
        g_reopenStage = 0;
        g_reopenTries = 15;
        SetTimer(g_hwnd, kReopenTimer, 100, nullptr);
        KillTimer(g_hwnd, kPanelTimer);
        return;
    }
    if (fg && IsShellExperienceWindow(fg) && g_panelResend > 0) {
        // The host can create this window on demand and miss the first send, so repeat it
        // for a few frames once it's up.
        g_panelResend--;
        SendPanelMode(g_panelMode);
    }
    if (fg && (IsShellExperienceWindow(fg) || IsQuickSettingsWindow(fg))) {
        PlacePanel(fg);
    }
    if (--g_panelTicks <= 0) {
        KillTimer(g_hwnd, kPanelTimer);
    }
}

// Shows one half of the native notification and calendar panel: the calendar under the island, or
// the notifications at their native place (kPanelNative).
void TogglePanel(WPARAM mode) {
    HWND fg = GetForegroundWindow();
    const bool open = fg && IsShellExperienceWindow(fg);
    if (open && g_panelMode == mode) {
        SendWinKey('N');
        return;
    }
    g_panelMode = mode;
    if (mode == kPanelCalendar) g_calendarTick = GetTickCount64();
    if (open) {
        // Windows sizes the panel's clip region as it opens, so changing parts means reopening.
        SendWinKey('N');
        g_reopenStage = 0;
        g_reopenTries = 15;
        SetTimer(g_hwnd, kReopenTimer, 100, nullptr);
        return;
    }
    g_panelCold = !g_notificationPanel || !IsWindow(g_notificationPanel);
    // P7: the cold first cycle is not shown (panel-split replaces the clip with an empty one until the reopen).
    if (g_panelCold) SetPropW(g_hwnd, L"TopBarPanelHideTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
    else RemovePropW(g_hwnd, L"TopBarPanelHideTick");  // The reopen, or any warm open, is the one to show.
    g_panelResend = 6;
    SendPanelMode(mode);
    // This panel opens wherever it was left, so put it in place first. The clip only updates
    // once it opens, so use its top from the last time this mode was shown.
    if (mode == kPanelCalendar && g_notificationPanel && IsWindow(g_notificationPanel) && g_regionTop[mode]) {
        RECT box, bar, r;
        const int kind = GetWindowRgnBox(g_notificationPanel, &box);
        if (kind != ERROR && kind != NULLREGION) {
            GetWindowRect(g_hwnd, &bar);
            GetWindowRect(g_notificationPanel, &r);
            const float s = Scale();
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            const int x = AnchoredPanelX(g_notificationPanel, box, mi, s);
            const int y = bar.bottom + (int)(kPanelGap * s + 0.5f) - g_regionTop[mode] - (int)(kPanelDrop * s);
            SaveNativePanelRect(g_notificationPanel);
            SetWindowPos(g_notificationPanel, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            g_panelOpenTick = GetTickCount64();
        }
    }
    SendWinKey('N');
    FollowPanel();
}

void StartQuickClose(HWND movedTo);

// Opens one page of Quick Settings. The same icon again closes it.
void OpenQuickPage(Action page) {
    HWND fg = GetForegroundWindow();
    HWND quick = FindWindowW(L"ControlCenterWindow", nullptr);
    const bool focused = fg && IsQuickSettingsWindow(fg);
    const bool unfocusedOpen = !focused && g_quickHadFocus && quick && IsWindowVisible(quick);
    // Opened from the keyboard, it's on the main page.
    const Action current = g_quickPage == Action::None ? Action::QuickSettings : g_quickPage;
    if ((focused || unfocusedOpen) && current == page) {
        if (focused) {
            SendWinKey('A');
        } else {
            StartQuickClose(fg);
        }
        return;
    }
    g_quickPage = page;
    g_quickHadFocus = true;  // Opened by the bar, so it's the bar's to close even without focus.
    PCWSTR uri = L"ms-actioncenter:controlcenter/&showFooter=true";
    if (page == Action::Wifi) {
        uri = L"ms-availablenetworks:";
    } else if (page == Action::Bluetooth) {
        uri = L"ms-actioncenter:controlcenter/bluetooth";
    }
    if (focused || unfocusedOpen) {
        if (page == Action::QuickSettings) {
            // Windows ignores the main page's link while on a subpage, so close and reopen.
            g_quickHadFocus = false;  // This close is intended: nothing to clean up after it.
            g_quickReopenTries = 15;
            g_quickReopenSettled = false;
            SendWinKey('A');
            SetTimer(g_hwnd, kQuickReopenTimer, 80, nullptr);
            return;
        }
        ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);  // Already open: just change page.
    } else {
        // Opened by link alone it gets no focus, and then Windows doesn't close it when the user
        // moves to another app. Its own shortcut gives it focus; the page link follows.
        SendWinKey('A');
        if (page != Action::QuickSettings) {
            g_quickPendingUri = uri;
            SetTimer(g_hwnd, kQuickPageTimer, 150, nullptr);
        }
    }
    FollowPanel();
}

bool HasClass(HWND hwnd, PCWSTR name) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return !wcscmp(cls, name);
}

// ---- Closing Quick Settings and Search when the user moves to another app ----
// Opened from the bar, Quick Settings can appear without focus, and then doesn't close when the
// user moves to another app. Search stays open behind the app too, and the Escape that
// start-menu-pop posts to it is not acted on once it has lost focus. Both close on their own
// shortcut (Win+A, Win+S), though a press may first only bring the panel into focus, so it is
// pressed until the panel is gone. Focus then goes back to the app the user moved to.
//
// What used to keep them open:
// - The press went out 120 ms after the first focus change. During Alt+Tab that is the switcher,
//   with Alt still held, so Win+Alt+A reached nobody; clicking a dock icon passes the taskbar
//   first. The close was then marked done, and the real app's focus change was ignored.
// - Windows ignores the shortcut for a moment after the panel loses focus (see kQuickReopenTimer).
// - IsWindowVisible stays true for a closed shell window that is only cloaked.
// So: wait until focus has settled on a real window with no key or button held, check the panel
// is really up, and retry a few times.
//
// Only once the bar has opened the panel or seen it focused: Win+A or Win+S on a closed one
// would open it, dismissing whatever just took focus.

bool PathOfWindowHas(HWND hwnd, PCWSTR exe) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(proc, 0, path, &size) && wcsstr(path, exe);
    CloseHandle(proc);
    return ok;
}

bool IsSearchWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return !wcscmp(cls, L"Windows.UI.Core.CoreWindow") && PathOfWindowHas(hwnd, L"\\SearchHost.exe");
}

bool IsStartMenuWindow(HWND hwnd) {
    return PathOfWindowHas(hwnd, L"\\StartMenuExperienceHost.exe");
}

// Closed shell panels are often only cloaked, so visible alone doesn't mean open.
bool ShellWindowShown(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd)) {
        return false;
    }
    DWORD cloaked = 0;
    return !(SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked);
}

bool QuickSettingsShown() {
    return ShellWindowShown(FindWindowW(L"ControlCenterWindow", nullptr));
}

bool SearchShown() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"Windows.UI.Core.CoreWindow", nullptr))) {
        if (PathOfWindowHas(hwnd, L"\\SearchHost.exe") && ShellWindowShown(hwnd)) {
            return true;
        }
    }
    return false;
}

// A held key would turn the shortcut into another one (Alt+Tab still down), and a held button
// means a click or drag is still in progress.
bool UserInputHeld() {
    static const int kKeys[] = {VK_MENU, VK_CONTROL, VK_SHIFT, VK_LWIN, VK_RWIN, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int k : kKeys) {
        if (GetAsyncKeyState(k) & 0x8000) {
            return true;
        }
    }
    return false;
}

// Focus passes through these on the way to an app (Alt+Tab, Win+Tab).
bool IsTransientForeground(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return !wcscmp(cls, L"XamlExplorerHostIslandWindow") || !wcscmp(cls, L"MultitaskingViewFrame") ||
           !wcscmp(cls, L"TaskSwitcherWnd") || !wcscmp(cls, L"ForegroundStaging");
}

struct PanelCloser {
    UINT_PTR timer;
    WORD key;
    bool (*shown)();
    bool (*owns)(HWND);  // The panel's own windows, and any that work as a pair with it.
    bool* hadFocus;
    bool closing = false;
    int waits = 0;
    int presses = 0;
    HWND returnTo = nullptr;
};

bool QuickOwns(HWND hwnd) { return IsQuickSettingsWindow(hwnd); }
bool SearchOwns(HWND hwnd) { return IsSearchWindow(hwnd) || IsStartMenuWindow(hwnd); }  // Start and Search work as a pair.

PanelCloser g_quickCloser{kQuickCloseTimer, 'A', QuickSettingsShown, QuickOwns, &g_quickHadFocus};
PanelCloser g_searchCloser{kSearchCloseTimer, 'S', SearchShown, SearchOwns, &g_searchHadFocus};

constexpr UINT kCloseSettleMs = 350;  // After the last focus change; longer than Windows ignores the shortcut.
constexpr UINT kClosePollMs = 75;
constexpr int kCloseMaxWaits = 60;    // About 4.5 s of held keys or a switcher still up.
constexpr int kCloseMaxPresses = 4;

// Each focus change starts the wait again, so it runs once focus has settled.
void StartPanelClose(PanelCloser& c, HWND movedTo) {
    if (!*c.hadFocus || c.closing) {
        return;
    }
    c.returnTo = movedTo;
    c.waits = 0;
    SetTimer(g_hwnd, c.timer, kCloseSettleMs, nullptr);
}

void CancelPanelClose(PanelCloser& c) {
    if (!c.closing) {
        KillTimer(g_hwnd, c.timer);
    }
}

void FinishPanelClose(PanelCloser& c) {
    KillTimer(g_hwnd, c.timer);
    c.closing = false;
    *c.hadFocus = false;
    HWND fg = GetForegroundWindow();
    if (c.presses > 0 && c.returnTo && IsWindow(c.returnTo) && fg != c.returnTo && !(fg && c.owns(fg))) {
        SetForegroundWindow(c.returnTo);
    }
    c.presses = 0;
}

void StepPanelClose(PanelCloser& c) {
    HWND fg = GetForegroundWindow();
    if (!c.closing) {
        if (fg && c.owns(fg)) {
            KillTimer(g_hwnd, c.timer);  // The user went back to it.
            return;
        }
        if (!c.shown()) {
            KillTimer(g_hwnd, c.timer);  // It closed itself.
            *c.hadFocus = false;
            return;
        }
        if ((UserInputHeld() || !fg || IsTransientForeground(fg)) && ++c.waits < kCloseMaxWaits) {
            SetTimer(g_hwnd, c.timer, kClosePollMs, nullptr);
            return;
        }
        if (fg && fg != g_hwnd && fg != g_isHwnd) {
            c.returnTo = fg;  // Where focus really ended up.
        }
        c.closing = true;
        c.presses = 0;
        c.waits = 0;
    }
    if (!c.shown()) {
        FinishPanelClose(c);
        return;
    }
    if (c.presses >= kCloseMaxPresses) {
        FinishPanelClose(c);  // Give up rather than keep pressing a shortcut.
        return;
    }
    if (UserInputHeld() && ++c.waits < kCloseMaxWaits) {
        SetTimer(g_hwnd, c.timer, kClosePollMs, nullptr);
        return;
    }
    c.presses++;
    SendWinKey(c.key);
    SetTimer(g_hwnd, c.timer, 250, nullptr);
}

void StartQuickClose(HWND movedTo) { StartPanelClose(g_quickCloser, movedTo); }
void StepQuickClose() { StepPanelClose(g_quickCloser); }
void StepSearchClose() { StepPanelClose(g_searchCloser); }

// From the foreground hook.
void OnForegroundForPanels(HWND hwnd) {
    if (hwnd == g_hwnd || hwnd == g_isHwnd) {
        return;
    }
    if (IsQuickSettingsWindow(hwnd)) {
        g_quickHadFocus = true;
        CancelPanelClose(g_quickCloser);
    } else {
        StartPanelClose(g_quickCloser, hwnd);
    }
    if (IsSearchWindow(hwnd)) {
        g_searchHadFocus = true;
        CancelPanelClose(g_searchCloser);
    } else if (IsStartMenuWindow(hwnd)) {
        CancelPanelClose(g_searchCloser);  // Start and Search hand focus between them.
    } else {
        StartPanelClose(g_searchCloser, hwnd);
    }
}

