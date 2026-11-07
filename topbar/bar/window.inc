void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW || !hwnd) {
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (IsQuickSettingsWindow(hwnd)) {
            g_quickHadFocus = true;
        } else if (hwnd != g_hwnd) {
            StartQuickClose(hwnd);
        }
        if (IsShellExperienceWindow(hwnd) || IsQuickSettingsWindow(hwnd)) {
            if (GetTickCount64() - g_anchorTick >= kPanelAnchorMs) {
                g_quickPage = Action::None;  // Opened from the keyboard, not from an icon.
            }
            if (IsShellExperienceWindow(hwnd)) {
                g_notificationPanel = hwnd;
            }
            if (GetTickCount64() - g_panelOpenTick > kPanelDropMs) {
                g_panelOpenTick = GetTickCount64();  // Unless it was already started before opening.
            }
            PlacePanel(hwnd);
            FollowPanel();
        }
        if (!IsShellWindow(hwnd) && !ForegroundCoversMonitor()) {
            g_abnFullscreen = false;  // Windows can miss telling us a fullscreen app has gone.
        }
        UpdateFullscreen();
        PostMessageW(g_hwnd, WM_REFRESH_FOREGROUND, 0, 0);
    } else if (event == EVENT_OBJECT_NAMECHANGE && hwnd == GetForegroundWindow()) {
        PostMessageW(g_hwnd, WM_REFRESH_TITLE, 0, 0);
    }
}

// Name changes are only watched for the foreground window's process, so the rest of the desktop
// does not wake this thread. Re-hooked on every foreground change.
void RehookTitle() {
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    if (pid == g_titlePid && g_titleHook) {
        return;
    }
    if (g_titleHook) {
        UnhookWinEvent(g_titleHook);
        g_titleHook = nullptr;
    }
    g_titlePid = pid;
    if (pid) {
        g_titleHook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, OnWinEvent, pid, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
}

void CALLBACK OnNetChange(void*, void*, int) {
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_REFRESH_NET, 0, 0);
    }
}

// One timer per minute, aimed at the next minute boundary, so the clock never polls.
void ArmClock() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    SetTimer(g_hwnd, kClockTimer, (60 - st.wSecond) * 1000 - st.wMilliseconds + 50, nullptr);
}

int HitIndexAt(POINT p) {
    for (size_t i = 0; i < g_hits.size(); i++) {
        if (PtInRect(&g_hits[i].rect, p)) {
            return (int)i;
        }
    }
    return -1;
}

int HitIdAt(POINT p) {
    const int index = HitIndexAt(p);
    return index < 0 ? H_none : g_hits[index].id;
}

RECT HitRectById(int id) {
    for (const Hit& h : g_hits) {
        if (h.id == id && id != H_none) {
            RECT r = h.rect;
            InflateRect(&r, 2, 2);
            return r;
        }
    }
    return RECT{};
}

// Hover follows stable ids, and only the old and new highlight are redrawn.
void SetHover(int id) {
    if (id == g_hoverId) {
        return;
    }
    Invalidate(HitRectById(g_hoverId));
    g_hoverId = id;
    Invalidate(HitRectById(id));
}

void SetPillTarget(bool expanded) {
    if (expanded == g_pillTarget) {
        return;
    }
    g_pillTarget = expanded;
    if (expanded) {
        g_pillSpring.Aim(1.0f, kSpringExpandK, kSpringExpandC);
    } else {
        g_pillSpring.Aim(0.0f, kSpringCollapseK, kSpringCollapseC);
    }
    g_clock.Wake();
}

// One display frame: the spring steps by the real time since the last one (QPC, at most 50 ms).
void StepPill() {
    g_clock.Acked();
    const double now = QpcSeconds();
    double dt = g_lastFrame ? now - g_lastFrame : 1.0 / 60.0;
    if (dt > 0.05) dt = 0.05;
    g_lastFrame = now;
    g_pillSpring.Step(dt);
    g_pillT = fminf(fmaxf(g_pillSpring.x, 0.0f), 1.0f);
    if (g_pillSpring.Settled()) {
        g_clock.Sleep();
        g_lastFrame = 0;
    }
    InvalidateLayout();  // Paint adds the pill and whatever it pushed aside.
    Paint();
    if (g_tracking) {
        // The hit list moved with the pill: resync hover against the new layout.
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
        SetHover(HitIdAt(p));
        SetPillTarget(PtInRect(&g_pillRect, p));
    }
}

void RunAction(Action action) {
    switch (action) {
        case Action::QuickSettings:
        case Action::Wifi:
        case Action::Bluetooth:
            OpenQuickPage(action);
            break;
        case Action::Notifications: TogglePanel(kPanelNotifications); break;
        case Action::Calendar: TogglePanel(kPanelCalendar); break;
        case Action::Profile:
            // Pressing the mouse can already have dismissed it: don't reopen it on release.
            if (!g_profileAtDown || g_profileOpen) {
                ToggleProfile();
            }
            break;
        default: break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        if (GetTickCount64() - g_trayBroadcastTick < 3000) {
            return 0;  // The bar's own request for tray icons, not a new taskbar.
        }
        g_registered = false;
        RegisterBar();
        InvalidateAll();
        return 0;
    }
    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                g_tracking = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
            }
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            SetPillTarget(PtInRect(&g_pillRect, p));
            SetHover(HitIdAt(p));
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            SetHover(H_none);
            SetPillTarget(false);
            return 0;
        case WM_APP_PAINT:
            Paint();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            g_profileAtDown = g_profileOpen;
            return 0;
        case WM_LBUTTONUP: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            const int index = HitIndexAt(p);
            if (index >= 0) {
                RECT hit = g_hits[index].rect;
                MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&hit), 2);
                g_anchorX = (hit.left + hit.right) / 2;
                g_anchorTick = GetTickCount64();
                // Read by panel-split inside the Quick Settings host.
                SetPropW(hwnd, L"TopBarAnchorX", reinterpret_cast<HANDLE>((INT_PTR)g_anchorX));
                SetPropW(hwnd, L"TopBarAnchorTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
                if (g_hits[index].action == Action::TrayIcon) {
                    ClickTrayIcon(g_hits[index].data, false);
                } else {
                    RunAction(g_hits[index].action);
                }
                InvalidateAll();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            const int index = HitIndexAt(p);
            if (index >= 0 && g_hits[index].action == Action::TrayIcon) {
                ClickTrayIcon(g_hits[index].data, true);
            }
            return 0;
        }
        case WM_COPYDATA: {
            auto cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == kTrayCopyData && cds->cbData >= sizeof(BarTrayMessage)) {
                OnTrayMessage(*static_cast<const BarTrayMessage*>(cds->lpData));
                return TRUE;
            }
            return FALSE;
        }
        case WM_PROFILE_DONE:
            if (wp == 1) {
                HWND fg = GetForegroundWindow();
                if (g_profileOpen && fg && IsStartWindow(fg)) {
                    SendWinKey(0);
                }
            }
            g_profileOpen = false;
            return 0;
        case WM_APPBAR_CB:
            if (wp == ABN_POSCHANGED) {
                PlaceBar();
                InvalidateAll();
            } else if (wp == ABN_FULLSCREENAPP) {
                g_abnFullscreen = lp != 0;
                UpdateFullscreen();
                // Apps often finish resizing just after Windows reports them as fullscreen.
                SetTimer(hwnd, kFullscreenTimer, 300, nullptr);
            }
            return 0;
        case WM_POWERBROADCAST:
            ReadBattery();
            ArmClock();
            InvalidateAll();
            return TRUE;
        case WM_TIMER:
            if (wp == kClockTimer) {
                ArmClock();
                InvalidateAll();
            } else if (wp == kTitleTimer) {
                KillTimer(hwnd, kTitleTimer);
                g_titlePending = false;
                ReadForeground();
                InvalidateAll();
                StepPill();
            } else if (wp == kPanelTimer) {
                StepPanelFollow();
            } else if (wp == kFullscreenTimer) {
                KillTimer(hwnd, kFullscreenTimer);
                UpdateFullscreen();
            } else if (wp == kReopenTimer) {
                HWND fg = GetForegroundWindow();
                const bool panelFocused = fg && IsShellExperienceWindow(fg);
                if (g_reopenStage == 0) {
                    // Only once it has really closed: while open, a toggle would close it again.
                    if (panelFocused && --g_reopenTries > 0) {
                        return 0;
                    }
                    // Windows ignores a toggle for a moment after the panel loses focus.
                    g_reopenStage = 1;
                    g_reopenOpens = 4;
                    SetTimer(hwnd, kReopenTimer, 300, nullptr);
                    return 0;
                }
                if (panelFocused && g_reopenStage == 2) {
                    g_reopenStage = 0;  // It opened.
                    KillTimer(hwnd, kReopenTimer);
                    return 0;
                }
                if (--g_reopenOpens < 0) {
                    g_reopenStage = 0;
                    KillTimer(hwnd, kReopenTimer);
                    return 0;
                }
                // Opening can still be ignored, so check it took, and try again if not.
                g_reopenStage = 2;
                g_anchorTick = GetTickCount64();  // A slow host start can outlast the click's anchor.
                TogglePanel(g_panelMode);
                SetTimer(hwnd, kReopenTimer, 500, nullptr);
            } else if (wp == kQuickCloseTimer) {
                StepQuickClose();
            } else if (wp == kQuickReopenTimer) {
                HWND fg = GetForegroundWindow();
                if (fg && IsQuickSettingsWindow(fg) && --g_quickReopenTries > 0) {
                    return 0;  // Still closing.
                }
                if (!g_quickReopenSettled) {
                    // Windows ignores the shortcut for a moment after the panel loses focus.
                    g_quickReopenSettled = true;
                    SetTimer(hwnd, kQuickReopenTimer, 250, nullptr);
                    return 0;
                }
                KillTimer(hwnd, kQuickReopenTimer);
                g_quickReopenSettled = false;
                g_anchorTick = GetTickCount64();
                SetPropW(hwnd, L"TopBarAnchorTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
                g_quickHadFocus = true;
                SendWinKey('A');
                FollowPanel();
            } else if (wp == kQuickPageTimer) {
                KillTimer(hwnd, kQuickPageTimer);
                if (g_quickPendingUri) {
                    ShellExecuteW(nullptr, L"open", g_quickPendingUri, nullptr, nullptr, SW_SHOWNORMAL);
                    g_quickPendingUri = nullptr;
                }
            }
            return 0;
        case WM_TIMECHANGE:
            ArmClock();
            InvalidateAll();
            return 0;
        case WM_REFRESH_NET:
            ReadNetwork();
            InvalidateAll();
            return 0;
        case WM_APP_FRAME:
            StepPill();
            return 0;
        case WM_APP_SETTINGS:
            g_settings = LoadSettings();
            InvalidateAll();
            return 0;
        case WM_REFRESH_TITLE:
            if (!g_titlePending) {
                g_titlePending = true;  // A busy title (a spinner in a terminal) costs one read per 100 ms.
                SetTimer(hwnd, kTitleTimer, 100, nullptr);
            }
            return 0;
        case WM_REFRESH_FOREGROUND:
            RehookTitle();
            ReadForeground();
            InvalidateAll();
            return 0;
        case WM_SETTINGCHANGE:
            ReadAccent();
            ReadForeground();
            InvalidateAll();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PlaceBar();
            InvalidateAll();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            RemovePropW(hwnd, L"TopBarAnchorX");
            RemovePropW(hwnd, L"TopBarAnchorTick");
            UnregisterBar();
            g_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI BarThread(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_panelMessage = RegisterWindowMessageW(L"Windhawk_TopBar_PanelMode");
    g_profileMessage = RegisterWindowMessageW(L"Windhawk_TopBar_Profile");
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                             kClassName, L"Top bar", WS_POPUP, 0, 0, 100, 40, nullptr, nullptr,
                             wc.hInstance, nullptr);
    if (!g_hwnd) {
        return 1;
    }
    ChangeWindowMessageFilterEx(g_hwnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, WM_PROFILE_DONE, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

    g_settings = LoadSettings();
    g_clock.Start(g_hwnd, WM_APP_FRAME);
    ReadAccent();
    LoadAvatar();
    ReadBattery();
    ReadNetwork();
    ReadForeground();

    g_powerA = RegisterPowerSettingNotification(g_hwnd, &kGuidBatteryPercent, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_powerB = RegisterPowerSettingNotification(g_hwnd, &kGuidAcDc, DEVICE_NOTIFY_WINDOW_HANDLE);
    NotifyIpInterfaceChange(0, reinterpret_cast<void*>(OnNetChange), nullptr, FALSE, &g_netNotify);
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    RehookTitle();

    RegisterBar();
    ArmClock();
    InvalidateAll();

    // Apps add their tray icons again when told the taskbar is new, and the bridge sees that.
    g_trayBroadcastTick = GetTickCount64();
    PostMessageW(HWND_BROADCAST, g_taskbarCreated, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_clock.Stop();
    if (g_foregroundHook) UnhookWinEvent(g_foregroundHook);
    if (g_titleHook) UnhookWinEvent(g_titleHook);
    if (g_netNotify) CancelMibChangeNotify2(g_netNotify);
    if (g_powerA) UnregisterPowerSettingNotification(g_powerA);
    if (g_powerB) UnregisterPowerSettingNotification(g_powerB);
    delete g_avatar;
    g_avatar = nullptr;
    delete g_appIcon;
    g_appIcon = nullptr;
    for (auto& icon : g_tray) {
        delete icon.pix;
    }
    g_tray.clear();
    ReleaseGfx();
    CoUninitialize();
    return 0;
}

