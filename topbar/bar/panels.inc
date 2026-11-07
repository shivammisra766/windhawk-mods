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

// These panels are tall windows clipped to a region around their content, and they
// only paint inside it. Moving the window is what puts the content under the bar;
// moving the content inside the window would push it out of the region.
void PlacePanel(HWND hwnd) {
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
        const int edge = (int)(kPanelEdge * s + 0.5f);
        x = g_anchorX - (box.left + box.right) / 2;
        if (x + box.right > mi.rcMonitor.right - edge) x = mi.rcMonitor.right - edge - box.right;
        if (x + box.left < mi.rcMonitor.left + edge) x = mi.rcMonitor.left + edge - box.left;
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
    if (x != r.left || y != r.top) {
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
        RemovePropW(g_hwnd, L"TopBarPanelHideTick");  // The reopen is the one to show.
        SendWinKey('N');
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

// Shows one half of the native notification and calendar panel.
void TogglePanel(WPARAM mode) {
    HWND fg = GetForegroundWindow();
    const bool open = fg && IsShellExperienceWindow(fg);
    if (open && g_panelMode == mode) {
        SendWinKey('N');
        return;
    }
    g_panelMode = mode;
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
    g_panelResend = 6;
    SendPanelMode(mode);
    // This panel opens wherever it was left, so put it in place first. The clip only updates
    // once it opens, so use its top from the last time this mode was shown.
    if (g_notificationPanel && IsWindow(g_notificationPanel) && g_regionTop[mode]) {
        RECT box, bar, r;
        const int kind = GetWindowRgnBox(g_notificationPanel, &box);
        if (kind != ERROR && kind != NULLREGION) {
            GetWindowRect(g_hwnd, &bar);
            GetWindowRect(g_notificationPanel, &r);
            const float s = Scale();
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            const int edge = (int)(kPanelEdge * s + 0.5f);
            int x = g_anchorX - (box.left + box.right) / 2;
            if (x + box.right > mi.rcMonitor.right - edge) x = mi.rcMonitor.right - edge - box.right;
            if (x + box.left < mi.rcMonitor.left + edge) x = mi.rcMonitor.left + edge - box.left;
            const int y = bar.bottom + (int)(kPanelGap * s + 0.5f) - g_regionTop[mode] - (int)(kPanelDrop * s);
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

// Opened from the bar, Quick Settings can appear without focus, and then doesn't close when the
// user moves to another app. Win+A closes it, though it may first only bring it into focus.
// Focus then goes back to the app the user moved to.
//
// Only once the bar has opened it or seen it focused: its window can count as visible while
// closed, and Win+A on a closed one would open it, dismissing whatever just took focus.
void StartQuickClose(HWND movedTo) {
    if (!g_quickHadFocus) {
        return;
    }
    g_quickHadFocus = false;
    HWND quick = FindWindowW(L"ControlCenterWindow", nullptr);
    if (quick && IsWindowVisible(quick)) {
        g_quickReturnTo = movedTo;
        g_quickCloseStage = 0;
        SetTimer(g_hwnd, kQuickCloseTimer, 120, nullptr);
    }
}

void StepQuickClose() {
    HWND quick = FindWindowW(L"ControlCenterWindow", nullptr);
    HWND fg = GetForegroundWindow();
    const bool focused = fg && IsQuickSettingsWindow(fg);
    if (g_quickCloseStage == 0) {
        if (focused || !quick || !IsWindowVisible(quick)) {
            KillTimer(g_hwnd, kQuickCloseTimer);  // The user went back to it, or it closed itself.
            return;
        }
        g_quickCloseStage = 1;
        SendWinKey('A');
        return;
    }
    if (g_quickCloseStage == 1 && focused) {
        // That only brought it into focus. This one closes it.
        g_quickCloseStage = 2;
        SendWinKey('A');
        return;
    }
    KillTimer(g_hwnd, kQuickCloseTimer);
    g_quickHadFocus = false;
    if (g_quickReturnTo && IsWindow(g_quickReturnTo) && fg != g_quickReturnTo) {
        SetForegroundWindow(g_quickReturnTo);
    }
}

