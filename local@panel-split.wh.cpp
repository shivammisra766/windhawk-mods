// ==WindhawkMod==
// @id              panel-split
// @name            Panel split
// @description     Lets the top bar open the native notifications or calendar on their own, hides toasts for the bar's island, and hides a cold first panel open
// @version         1.10
// @author          local
// @include         ShellExperienceHost.exe
// @include         ShellHost.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lshcore -lwindowsapp -luser32 -lgdi32
// ==/WindhawkMod==

// Both are parts of one native panel. The top bar sends a mode just before opening it,
// and this collapses the other part. Windows re-clips the panel to what is left.
// Opening the panel any other way, such as Win+N, shows both parts as usual.
// Event driven: nothing runs here until the panel opens or the bar sends a mode.
//
// These hosts also get "no client area animation" reported to them alone, which removes
// the native slide up from the bottom of the screen. It suits a panel above a bottom
// taskbar, not one hanging from a top bar. Other apps keep their animations.

#include <windows.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <atomic>
#include <mutex>
#include <vector>

using namespace winrt::Windows::UI::Xaml;

namespace {

constexpr WPARAM kModeBoth = 0;
constexpr WPARAM kModeNotifications = 1;
constexpr WPARAM kModeCalendar = 2;
constexpr WPARAM kAttach = 10;
constexpr WPARAM kDetach = 11;

UINT g_message = 0;
HANDLE g_setupThread = nullptr;
HANDLE g_stopEvent = nullptr;
std::atomic<WPARAM> g_mode{kModeBoth};
constexpr float kNotificationsLift = -28.0f;

using SystemParametersInfoW_t = decltype(&SystemParametersInfoW);
SystemParametersInfoW_t g_originalSpi = nullptr;

BOOL WINAPI SystemParametersInfoW_Hook(UINT action, UINT param, PVOID value, UINT flags) {
    if (action == SPI_GETCLIENTAREAANIMATION && value) {
        *static_cast<BOOL*>(value) = FALSE;
        return TRUE;
    }
    return g_originalSpi(action, param, value, flags);
}

// Quick Settings positions and shows itself in one SetWindowPos call on every open.
// Moving it from outside would show it at that position first, so the top bar's
// position goes straight into the call. The bar then finishes the drop into place.
constexpr float kPanelGap = 8.0f;
constexpr float kPanelEdge = 8.0f;
constexpr float kPanelDrop = 10.0f;
constexpr DWORD kPanelAnchorMs = 1500;

bool IsQuickSettings(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return !wcscmp(cls, L"ControlCenterWindow");
}

// Works out the position under the bar from the panel's clip region. The anchor is what
// the bar left on its window when an icon was clicked.
bool QuickSettingsPosition(HWND hwnd, const RECT& box, int* x, int* y) {
    HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
    if (!bar || !IsWindowVisible(bar)) {
        return false;
    }
    RECT barRect;
    GetWindowRect(bar, &barRect);
    const float s = GetDpiForWindow(bar) / 96.0f;
    *y = barRect.bottom + (int)(kPanelGap * s + 0.5f) - box.top - (int)(kPanelDrop * s);
    const DWORD tick = (DWORD)(UINT_PTR)GetPropW(bar, L"TopBarAnchorTick");
    if (GetTickCount() - tick < kPanelAnchorMs) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTOPRIMARY), &mi);
        const int edge = (int)(kPanelEdge * s + 0.5f);
        int px = (int)(INT_PTR)GetPropW(bar, L"TopBarAnchorX") - (box.left + box.right) / 2;
        if (px + box.right > mi.rcMonitor.right - edge) px = mi.rcMonitor.right - edge - box.right;
        if (px + box.left < mi.rcMonitor.left + edge) px = mi.rcMonitor.left + edge - box.left;
        *x = px;
    }
    return true;
}

// ---- Toasts: island only ----
// A toast is a CoreWindow in this host whose title says "New notification". While the bar is alive,
// answering, flagged TopBarToastsOk and refreshing TopBarToastsTick, the toast is placed off screen
// (it keeps its life cycle: timeout, sound, move to the bell) and the bar is told with a registered
// message so it reads the new notification. Any doubt shows the toast normally: no bar, a hung bar,
// a stale tick, the flag cleared (fullscreen, or no listener permission).
const wchar_t* const kToastTitles[] = {
    L"New notification", L"Neue Benachrichtigung", L"Nouvelle notification", L"Nueva notificaci",
    L"Nuova notifica", L"Nova notifica", L"Nieuwe melding", L"Nowe powiadomienie", L"Yeni bildirim",
    L"Новое уведомление", L"Нове сповіщення", L"新しい通知", L"新通知", L"새 알림", L"إعلام جديد", L"नई सूचना",
    L"Ny avisering", L"Ny meddelelse", L"Uusi ilmoitus", L"Nová oznámení", L"Új értesítés", L"Notificare nouă",
};
constexpr DWORD kToastTickMaxMs = 45000;
UINT g_toastMessage = 0;
int g_toastLogs = 0;

bool IsToastWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return false;
    wchar_t title[128] = {};
    GetWindowTextW(hwnd, title, 128);
    if (!title[0]) return false;
    for (const wchar_t* t : kToastTitles) {
        if (wcsstr(title, t)) return true;
    }
    return false;
}

bool BarWantsToastsHidden(HWND bar) {
    if (!bar || IsHungAppWindow(bar)) return false;
    if (!GetPropW(bar, L"TopBarToastsOk")) return false;
    const DWORD tick = (DWORD)(UINT_PTR)GetPropW(bar, L"TopBarToastsTick");
    return GetTickCount() - tick < kToastTickMaxMs;
}

// ---- Cold first open of the panel ----
// The bar sets TopBarPanelHideTick before a first open (host not yet warm) and clears it when it
// reopens. Until then the panel's clip is replaced by an empty one, so the first cycle is not seen.
constexpr DWORD kPanelHideMaxMs = 2500;
bool PanelHiddenByBar(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return false;
    HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
    if (!bar) return false;
    const DWORD tick = (DWORD)(UINT_PTR)GetPropW(bar, L"TopBarPanelHideTick");
    if (!tick) return false;
    if (GetTickCount() - tick >= kPanelHideMaxMs) return false;
    return !IsToastWindow(hwnd);
}

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t g_originalSetWindowPos = nullptr;

BOOL WINAPI SetWindowPos_Hook(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(flags & SWP_HIDEWINDOW) && IsToastWindow(hwnd)) {
        HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
        const bool hide = BarWantsToastsHidden(bar);
        if (g_toastLogs++ < 20) Wh_Log(L"toast window %p pos (%d,%d) flags %x hide=%d", hwnd, x, y, flags, hide);
        if (hide) {
            x = -32000;
            y = -32000;
            flags &= ~SWP_NOMOVE;
        }
        if (HWND alerts = FindWindowW(L"WindhawkTopBarAlerts", nullptr)) PostMessageW(alerts, g_toastMessage, 0, 0);
        return g_originalSetWindowPos(hwnd, after, x, y, cx, cy, flags);
    }
    if (!(flags & SWP_NOMOVE) && IsQuickSettings(hwnd)) {
        RECT box;
        const int kind = GetWindowRgnBox(hwnd, &box);
        if (kind != ERROR && kind != NULLREGION) {
            QuickSettingsPosition(hwnd, box, &x, &y);
        }
    }
    return g_originalSetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

using SetWindowRgn_t = decltype(&SetWindowRgn);
SetWindowRgn_t g_originalSetWindowRgn = nullptr;

// The clip arrives just after the show. If its size changed, the position has to follow.
int WINAPI SetWindowRgn_Hook(HWND hwnd, HRGN region, BOOL redraw) {
    if (region && PanelHiddenByBar(hwnd)) {
        if (HRGN empty = CreateRectRgn(0, 0, 0, 0)) {
            DeleteObject(region);
            region = empty;
            Wh_Log(L"cold panel clip hidden");
            return g_originalSetWindowRgn(hwnd, region, redraw);
        }
    }
    const int result = g_originalSetWindowRgn(hwnd, region, redraw);
    if (result && region && IsQuickSettings(hwnd)) {
        RECT box, r;
        const int kind = GetWindowRgnBox(hwnd, &box);
        if (kind != ERROR && kind != NULLREGION && GetWindowRect(hwnd, &r)) {
            int x = r.left, y = r.top;
            if (QuickSettingsPosition(hwnd, box, &x, &y) && (x != r.left || y != r.top)) {
                g_originalSetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    return result;
}

struct HookedThread {
    DWORD thread;
    HWND window;
    HHOOK hook;
};
std::mutex g_hooksLock;
std::vector<HookedThread> g_hooks;

thread_local bool t_attached = false;
thread_local winrt::event_token t_visibilityToken{};

FrameworkElement FindNamed(FrameworkElement const& root, PCWSTR name) {
    if (!root) {
        return nullptr;
    }
    if (root.Name() == name) {
        return root;
    }
    int count = Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; i++) {
        auto child = Media::VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();
        if (auto found = FindNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

// Runs on the XAML thread that owns the current window.
void ApplyMode(WPARAM mode) {
    auto window = Window::Current();
    if (!window) {
        return;
    }
    auto root = window.Content().try_as<FrameworkElement>();
    auto notifications = FindNamed(root, L"NotificationCenterGrid");
    auto calendar = FindNamed(root, L"CalendarCenterGrid");
    if (notifications) {
        notifications.Visibility(mode == kModeCalendar ? Visibility::Collapsed : Visibility::Visible);
        // Alone, the card sits at the bottom of its clip with a constant empty strip above it,
        // leaving a gap under the bar. Sliding it up through that strip stays inside the clip.
        notifications.Translation({0.0f, mode == kModeNotifications ? kNotificationsLift : 0.0f, 0.0f});
    }
    if (calendar) {
        calendar.Visibility(mode == kModeNotifications ? Visibility::Collapsed : Visibility::Visible);
    }
}

void Attach() {
    if (t_attached) {
        return;
    }
    auto window = Window::Current();
    if (!window) {
        return;
    }
    t_visibilityToken = window.VisibilityChanged(
        [](winrt::Windows::Foundation::IInspectable const&,
           winrt::Windows::UI::Core::VisibilityChangedEventArgs const& args) {
            try {
                if (!args.Visible()) {
                    // Closed: the next open is both parts unless the bar says otherwise.
                    // Activation can't be used for this, since it also changes while open.
                    g_mode = kModeBoth;
                }
                ApplyMode(g_mode);
            } catch (...) {
            }
        });
    t_attached = true;
}

void Detach() {
    if (!t_attached) {
        return;
    }
    if (auto window = Window::Current()) {
        window.VisibilityChanged(t_visibilityToken);
    }
    ApplyMode(kModeBoth);
    t_attached = false;
}

LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto msg = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (msg->message == g_message) {
            try {
                if (msg->wParam == kAttach) {
                    Attach();
                } else if (msg->wParam == kDetach) {
                    Detach();
                } else {
                    g_mode = msg->wParam;
                    Attach();
                    ApplyMode(msg->wParam);
                }
            } catch (...) {
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL CALLBACK HookOwnWindow(HWND hwnd, LPARAM) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow")) {
        return TRUE;
    }
    DWORD pid = 0;
    DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !thread) {
        return TRUE;
    }
    {
        std::lock_guard lock(g_hooksLock);
        for (const auto& h : g_hooks) {
            if (h.thread == thread) {
                return TRUE;
            }
        }
    }
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, nullptr, thread);
    if (!hook) {
        return TRUE;
    }
    // Without this, Windows drops the bar's message before it reaches the hook.
    ChangeWindowMessageFilterEx(hwnd, g_message, MSGFLT_ALLOW, nullptr);
    {
        std::lock_guard lock(g_hooksLock);
        g_hooks.push_back({thread, hwnd, hook});
    }
    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, g_message, kAttach, 0, SMTO_BLOCK, 3000, &result);
    return TRUE;
}

void CALLBACK OnOwnWindowEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (object == OBJID_WINDOW && hwnd) {
        HookOwnWindow(hwnd, 0);
    }
}

DWORD WINAPI SetupThread(void*) {
    // The host only creates the panel's window on first use, which can be any time after it
    // starts. Hook what exists now, then sleep until this process creates or shows a window.
    EnumWindows(HookOwnWindow, 0);
    const DWORD pid = GetCurrentProcessId();
    HWINEVENTHOOK created = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, OnOwnWindowEvent, pid, 0,
                                            WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                               OnOwnWindowEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0 + 1) {
            break;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }
    }
    if (created) UnhookWinEvent(created);
    if (foreground) UnhookWinEvent(foreground);
    return 0;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_SetFunctionHook(reinterpret_cast<void*>(SystemParametersInfoW),
                       reinterpret_cast<void*>(SystemParametersInfoW_Hook),
                       reinterpret_cast<void**>(&g_originalSpi));
    Wh_SetFunctionHook(reinterpret_cast<void*>(SetWindowPos), reinterpret_cast<void*>(SetWindowPos_Hook),
                       reinterpret_cast<void**>(&g_originalSetWindowPos));
    Wh_SetFunctionHook(reinterpret_cast<void*>(SetWindowRgn), reinterpret_cast<void*>(SetWindowRgn_Hook),
                       reinterpret_cast<void**>(&g_originalSetWindowRgn));
    g_message = RegisterWindowMessageW(L"Windhawk_TopBar_PanelMode");
    g_toastMessage = RegisterWindowMessageW(L"Windhawk_TopBar_ToastShown");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_setupThread = CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_setupThread) {
        WaitForSingleObject(g_setupThread, 5000);
        CloseHandle(g_setupThread);
        g_setupThread = nullptr;
    }
    std::vector<HookedThread> hooks;
    {
        std::lock_guard lock(g_hooksLock);
        hooks.swap(g_hooks);
    }
    for (const auto& h : hooks) {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(h.window, g_message, kDetach, 0, SMTO_BLOCK, 3000, &result);
        UnhookWindowsHookEx(h.hook);
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}
