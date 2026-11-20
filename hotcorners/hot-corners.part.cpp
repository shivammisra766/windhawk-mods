// ==WindhawkMod==
// @id              hot-corners
// @name            Hot corners
// @description     macOS-style hot corners: rest the pointer in a screen corner to open Task View, Start, notifications or the desktop
// @version         1.0
// @author          local
// @include         windhawk.exe
// @compilerOptions -luser32 -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Hot corners

Rest the pointer in a corner of the screen for a moment to run that corner's action.

- Fires once per visit; move out and back in to fire again.
- Nothing fires while a mouse button is held (dragging a window into a corner), or while a
  fullscreen app is in front.
- Runs in its own small process. A low-level mouse hook on its own thread checks each mouse
  move against the corners; nothing runs while the mouse is still.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- topLeft: taskView
  $name: Top-left corner
  $options:
  - none: Nothing
  - taskView: Task View
  - start: Start
  - notifications: Notifications
  - quickSettings: Quick Settings
  - desktop: Show desktop
- topRight: notifications
  $name: Top-right corner
  $options:
  - none: Nothing
  - taskView: Task View
  - start: Start
  - notifications: Notifications
  - quickSettings: Quick Settings
  - desktop: Show desktop
- bottomLeft: start
  $name: Bottom-left corner
  $options:
  - none: Nothing
  - taskView: Task View
  - start: Start
  - notifications: Notifications
  - quickSettings: Quick Settings
  - desktop: Show desktop
- bottomRight: desktop
  $name: Bottom-right corner
  $options:
  - none: Nothing
  - taskView: Task View
  - start: Start
  - notifications: Notifications
  - quickSettings: Quick Settings
  - desktop: Show desktop
- dwellMs: 250
  $name: Rest time (ms)
  $description: How long the pointer must stay in the corner. Flicking through a corner does nothing
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <vector>

enum class Action { None, TaskView, Start, Notifications, QuickSettings, Desktop };

static std::atomic<Action> g_actions[4];  // TL, TR, BL, BR
static std::atomic<int> g_dwellMs{250};

constexpr ULONG_PTR kInjected = 0x484F5443;  // "HOTC": marks our own SendInput
constexpr int kZone = 2;                      // px from the corner that count as "in it"
constexpr int kRearm = 12;                    // px the pointer must leave by before it can fire again

static Action ParseAction(PCWSTR s) {
    if (!wcscmp(s, L"taskView")) return Action::TaskView;
    if (!wcscmp(s, L"start")) return Action::Start;
    if (!wcscmp(s, L"notifications")) return Action::Notifications;
    if (!wcscmp(s, L"quickSettings")) return Action::QuickSettings;
    if (!wcscmp(s, L"desktop")) return Action::Desktop;
    return Action::None;
}

static void LoadSettings() {
    const wchar_t* names[4] = {L"topLeft", L"topRight", L"bottomLeft", L"bottomRight"};
    for (int i = 0; i < 4; i++) {
        PCWSTR v = Wh_GetStringSetting(names[i]);
        g_actions[i] = ParseAction(v);
        Wh_FreeStringSetting(v);
    }
    int d = Wh_GetIntSetting(L"dwellMs");
    g_dwellMs = d < 0 ? 0 : d > 2000 ? 2000 : d;
}

// ---- Hook thread state (only touched on that thread) ----

static DWORD g_threadId = 0;
static HANDLE g_thread = nullptr;
static HHOOK g_hook = nullptr;
static std::vector<RECT> g_monitors;
static ULONGLONG g_monitorsAt = 0;
static int g_corner = -1;       // the corner the pointer is resting in, or -1
static bool g_fired = false;    // already fired for this visit
static POINT g_cornerPt{};      // the corner's own point
static UINT_PTR g_timer = 0;

static BOOL CALLBACK AddMonitor(HMONITOR, HDC, LPRECT r, LPARAM) {
    g_monitors.push_back(*r);
    return TRUE;
}

static void RefreshMonitors() {
    ULONGLONG now = GetTickCount64();
    if (!g_monitors.empty() && now - g_monitorsAt < 5000) return;
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, 0);
    g_monitorsAt = now;
}

// The corner (0 TL, 1 TR, 2 BL, 3 BR) within `zone` px of the point, and that corner's point.
static int CornerAt(POINT p, int zone, POINT* at) {
    for (const RECT& m : g_monitors) {
        const POINT c[4] = {{m.left, m.top}, {m.right - 1, m.top}, {m.left, m.bottom - 1}, {m.right - 1, m.bottom - 1}};
        for (int i = 0; i < 4; i++) {
            if (abs(p.x - c[i].x) <= zone && abs(p.y - c[i].y) <= zone) {
                if (at) *at = c[i];
                return i;
            }
        }
    }
    return -1;
}

static bool ButtonsHeld() {
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
}

// A fullscreen app (game, video, presentation) in front: corners stay quiet.
static bool FullscreenInFront() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = L"";
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

static void SendCombo(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = k;
        i.ki.dwExtraInfo = kInjected;
        in.push_back(i);
    }
    for (auto it = keys.end(); it != keys.begin();) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = *--it;
        i.ki.dwFlags = KEYEVENTF_KEYUP;
        i.ki.dwExtraInfo = kInjected;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

static void Run(Action a) {
    switch (a) {
        case Action::TaskView: SendCombo({VK_LWIN, VK_TAB}); break;
        case Action::Start: SendCombo({VK_LWIN}); break;
        case Action::Notifications: SendCombo({VK_LWIN, 'N'}); break;
        case Action::QuickSettings: SendCombo({VK_LWIN, 'A'}); break;
        case Action::Desktop: SendCombo({VK_LWIN, 'D'}); break;
        case Action::None: break;
    }
}

static void StopTimer() {
    if (g_timer) {
        KillTimer(nullptr, g_timer);
        g_timer = 0;
    }
}

static void CALLBACK DwellTimer(HWND, UINT, UINT_PTR, DWORD) {
    StopTimer();
    if (g_corner < 0 || g_fired) return;
    POINT p;
    if (!GetCursorPos(&p) || abs(p.x - g_cornerPt.x) > kZone || abs(p.y - g_cornerPt.y) > kZone) return;
    if (ButtonsHeld() || FullscreenInFront()) {
        g_fired = true;  // Not this visit; leave and come back.
        return;
    }
    g_fired = true;
    Action a = g_actions[g_corner];
    Wh_Log(L"Corner %d fired, action %d", g_corner, (int)a);
    Run(a);
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE) {
        const POINT p = reinterpret_cast<MSLLHOOKSTRUCT*>(lp)->pt;
        if (g_corner >= 0) {
            // Still near the corner: keep waiting (or stay fired) until the pointer really leaves.
            if (abs(p.x - g_cornerPt.x) > kRearm || abs(p.y - g_cornerPt.y) > kRearm) {
                g_corner = -1;
                g_fired = false;
                StopTimer();
            }
        } else {
            RefreshMonitors();
            POINT at;
            int c = CornerAt(p, kZone, &at);
            if (c >= 0 && g_actions[c] != Action::None) {
                g_corner = c;
                g_cornerPt = at;
                g_fired = false;
                StopTimer();
                g_timer = SetTimer(nullptr, 0, (UINT)std::max(1, g_dwellMs.load()), DwellTimer);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID ready) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&HookThread), &self);
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, self, 0);
    if (!g_hook) Wh_Log(L"SetWindowsHookEx failed: %u", GetLastError());
    SetEvent((HANDLE)ready);
    // The hook lives as long as this loop does (a hook dies with its thread).
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_DISPLAYCHANGE || msg.message == WM_APP) g_monitorsAt = 0;
        DispatchMessageW(&msg);
    }
    StopTimer();
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    return 0;
}

BOOL WhTool_ModInit() {
    LoadSettings();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, HookThread, ready, 0, &g_threadId);
    if (!g_thread) {
        CloseHandle(ready);
        return FALSE;
    }
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    return TRUE;
}

void WhTool_ModSettingsChanged() {
    LoadSettings();
    if (g_threadId) PostThreadMessageW(g_threadId, WM_APP, 0, 0);
}

void WhTool_ModUninit() {
    if (g_thread) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

