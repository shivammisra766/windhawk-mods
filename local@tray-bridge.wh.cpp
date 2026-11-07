// ==WindhawkMod==
// @id              tray-bridge
// @name            Tray bridge
// @description     Passes tray icon updates to the top bar, and places the volume and brightness popup over its centre bubble
// @version         1.7
// @author          local
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -luser32
// ==/WindhawkMod==
// ==WindhawkModSettings==
/*
- osdOverride: 1
  $name: Place the volume popup over the bar
  $description: 1 moves the popup onto the bar's centre bubble. 2 hides it while the bar is up (the island shows its own slider). 0 only logs the rectangles it asks for.
- osdUnits: 0
  $name: Popup rectangle units
  $description: 0 means device independent pixels (the logged rect divided by the bar's scale), 1 means physical pixels.
*/
// ==/WindhawkModSettings==

// Apps register tray icons by sending the taskbar a WM_COPYDATA message. This only looks at
// those messages on their way past and copies them to the top bar: the taskbar itself carries
// on as usual and nothing is replaced. The sender waits for this, so the bar can copy an icon
// before the app is able to destroy it.

#include <windows.h>
#include <windhawk_utils.h>
#include <cwchar>

namespace {

constexpr ULONG_PTR kTrayCopyData = 1;      // What apps send to the taskbar.
constexpr ULONG_PTR kBarCopyData = 0x7472;  // What this sends to the bar.
constexpr DWORD kTraySignature = 0x34753423;

// How Windows lays out the data: handles are always 32 bits, for every kind of app.
#pragma pack(push, 1)
struct NotifyIconData32 {
    DWORD cbSize;
    DWORD hWnd;
    DWORD uID;
    DWORD uFlags;
    DWORD uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    DWORD uVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct TrayData {
    DWORD signature;
    DWORD message;
    NotifyIconData32 nid;
};
#pragma pack(pop)

// The same fixed layout is declared in the bar.
struct BarTrayMessage {
    DWORD message;
    DWORD hwnd;
    DWORD id;
    DWORD flags;
    DWORD callback;
    DWORD icon;
    DWORD state;
    DWORD stateMask;
    DWORD version;
    GUID guid;
    WCHAR tip[128];
};

HANDLE g_stopEvent = nullptr;
HANDLE g_setupThread = nullptr;
HHOOK g_hook = nullptr;
HWND g_tray = nullptr;

constexpr int kQueueSize = 256;
CRITICAL_SECTION g_queueLock;
HANDLE g_queueEvent = nullptr;
HANDLE g_senderThread = nullptr;
int g_queueHead = 0, g_queueCount = 0;
BarTrayMessage g_queue[kQueueSize];

void Forward(const COPYDATASTRUCT* cds) {
    // Older apps send a shorter structure, so only trust what has arrived.
    constexpr DWORD kMinimum = offsetof(TrayData, nid) + offsetof(NotifyIconData32, szTip);
    if (!cds->lpData || cds->cbData < kMinimum) {
        return;
    }
    const auto* data = static_cast<const TrayData*>(cds->lpData);
    if (data->signature != kTraySignature) {
        return;
    }
    HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
    if (!bar) {
        return;
    }
    const DWORD size = cds->cbData - offsetof(TrayData, nid);
    BarTrayMessage out{};
    out.message = data->message;
    out.hwnd = data->nid.hWnd;
    out.id = data->nid.uID;
    out.flags = data->nid.uFlags;
    out.callback = data->nid.uCallbackMessage;
    out.icon = data->nid.hIcon;
    if (size >= offsetof(NotifyIconData32, dwState)) {
        wcsncpy_s(out.tip, data->nid.szTip, _TRUNCATE);
    }
    if (size >= offsetof(NotifyIconData32, szInfo)) {
        out.state = data->nid.dwState;
        out.stateMask = data->nid.dwStateMask;
    }
    if (size >= offsetof(NotifyIconData32, szInfoTitle)) {
        out.version = data->nid.uVersion;
    }
    if (size >= offsetof(NotifyIconData32, hBalloonIcon)) {
        out.guid = data->nid.guidItem;
    }
    // Never send from the taskbar's thread: while it waits, sent messages are dispatched into it, and during
    // startup (every app re-adding its icon while the taskbar's XAML loads) that reentrancy crashed Explorer
    // (0xc000027b in CoreMessaging). Queue it; the sender thread forwards. A full queue drops the update.
    EnterCriticalSection(&g_queueLock);
    if (g_queueCount < kQueueSize) {
        g_queue[(g_queueHead + g_queueCount) % kQueueSize] = out;
        g_queueCount++;
        SetEvent(g_queueEvent);
    }
    LeaveCriticalSection(&g_queueLock);
}

DWORD WINAPI SenderThread(void*) {
    HANDLE waits[] = {g_stopEvent, g_queueEvent};
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        for (;;) {
            BarTrayMessage msg;
            EnterCriticalSection(&g_queueLock);
            if (!g_queueCount) {
                LeaveCriticalSection(&g_queueLock);
                break;
            }
            msg = g_queue[g_queueHead];
            g_queueHead = (g_queueHead + 1) % kQueueSize;
            g_queueCount--;
            LeaveCriticalSection(&g_queueLock);
            HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
            if (!bar) continue;
            COPYDATASTRUCT forward{kBarCopyData, sizeof(msg), &msg};
            DWORD_PTR result = 0;
            SendMessageTimeoutW(bar, WM_COPYDATA, reinterpret_cast<WPARAM>(g_tray), reinterpret_cast<LPARAM>(&forward),
                                SMTO_ABORTIFHUNG, 200, &result);
        }
    }
    return 0;
}

LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto msg = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (msg->message == WM_COPYDATA && msg->hwnd == g_tray && msg->lParam) {
            auto cds = reinterpret_cast<const COPYDATASTRUCT*>(msg->lParam);
            if (cds->dwData == kTrayCopyData) {
                Forward(cds);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD WINAPI SetupThread(void*) {
    // The taskbar may still be starting: wait for it, then this thread ends.
    for (int i = 0; i < 240; i++) {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD pid = 0;
        const DWORD thread = tray ? GetWindowThreadProcessId(tray, &pid) : 0;
        if (thread && pid == GetCurrentProcessId()) {
            g_tray = tray;
            g_hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, nullptr, thread);
            // A hook dies with the thread that set it, so this one stays, asleep, until unload.
            WaitForSingleObject(g_stopEvent, INFINITE);
            if (g_hook) {
                UnhookWindowsHookEx(g_hook);
                g_hook = nullptr;
            }
            return 0;
        }
        if (WaitForSingleObject(g_stopEvent, 500) == WAIT_OBJECT_0) {
            return 0;
        }
    }
    return 0;
}


// ---- Volume and brightness popup ----
// Windows.Internal.HardwareConfirmator.dll asks HardwareConfirmatorHost::GetPositionRect where to put the
// popup. When the bar is up, the answer is the bar's centre bubble. The bar publishes the rectangle (screen
// pixels) as TopBarCentreLeft/Top/Right/Bottom on its window and is told with a registered message that
// the popup is being shown. If the symbol is missing the hook is simply absent and the registry position
// (PositionIndex=3, top centre) applies. Every call is logged so the units can be checked.
struct FRect { float x, y, w, h; };
using GetPositionRect_t = FRect* (*)(void* self, FRect* result, const FRect* input);
GetPositionRect_t g_originalGetPositionRect = nullptr;
UINT g_osdMessage = 0;

FRect* GetPositionRect_Hook(void* self, FRect* result, const FRect* input) {
    FRect* ret = g_originalGetPositionRect(self, result, input);
    FRect* out = ret ? ret : result;
    const int mode = Wh_GetIntSetting(L"osdOverride");
    if (out) Wh_Log(L"GetPositionRect in=%.0f,%.0f %.0fx%.0f mode=%d", out->x, out->y, out->w, out->h, mode);
    // 2: the island draws its own slider; the native popup goes off-screen while the bar is up.
    if (mode == 2 && out) {
        HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
        if (bar && IsWindowVisible(bar) && !IsHungAppWindow(bar)) {
            out->x = -32000.0f;
            out->y = -32000.0f;
            if (HWND alerts = FindWindowW(L"WindhawkTopBarAlerts", nullptr)) PostMessageW(alerts, g_osdMessage, 1, 2);
            return ret;
        }
    }
    const bool override_ = mode == 1;
    const bool pixels = Wh_GetIntSetting(L"osdUnits") == 1;
    HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
    HWND alerts = FindWindowW(L"WindhawkTopBarAlerts", nullptr);
    float scale = 1.0f;
    bool placed = false;
    if (out) {
        const FRect before = *out;
        const FRect in = input ? *input : FRect{};
        if (override_ && bar && IsWindowVisible(bar) && !IsHungAppWindow(bar)) {
            const int l = (int)(INT_PTR)GetPropW(bar, L"TopBarCentreLeft");
            const int t = (int)(INT_PTR)GetPropW(bar, L"TopBarCentreTop");
            const int r = (int)(INT_PTR)GetPropW(bar, L"TopBarCentreRight");
            const int b = (int)(INT_PTR)GetPropW(bar, L"TopBarCentreBottom");
            if (r > l && b > t) {
                // Only for the bar's own monitor; the input is the display rect the popup was going to use.
                if (!pixels) scale = GetDpiForWindow(bar) / 96.0f;
                out->x = l / scale;
                out->y = t / scale;
                out->w = (r - l) / scale;
                out->h = (b - t) / scale;
                placed = true;
            }
        }
    }
    if (alerts) PostMessageW(alerts, g_osdMessage, 1, placed ? 1 : 0);
    return ret;
}

DWORD WINAPI OsdThread(void*) {
    // The DLL may not be loaded yet: wait for it, then hook once. Nothing runs after that.
    for (int i = 0; i < 600; i++) {
        HMODULE mod = GetModuleHandleW(L"Windows.Internal.HardwareConfirmator.dll");
        if (mod) {
            WindhawkUtils::SYMBOL_HOOK hooks[] = {
                {{L"private: struct winrt::Windows::Foundation::Rect __cdecl winrt::Windows::Internal::HardwareConfirmator::implementation::HardwareConfirmatorHost::GetPositionRect(struct winrt::Windows::Foundation::Rect const &)"},
                 reinterpret_cast<void**>(&g_originalGetPositionRect),
                 reinterpret_cast<void*>(GetPositionRect_Hook), true},
            };
            const bool ok = WindhawkUtils::HookSymbols(mod, hooks, 1);
            Wh_Log(L"HardwareConfirmator loaded, GetPositionRect hook %s", ok && g_originalGetPositionRect ? L"set" : L"NOT set");
            if (g_originalGetPositionRect) Wh_ApplyHookOperations();
            return 0;
        }
        if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) return 0;
    }
    Wh_Log(L"HardwareConfirmator never loaded; popup keeps its registry position");
    return 0;
}
HANDLE g_osdThread = nullptr;

}  // namespace

BOOL Wh_ModInit() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    InitializeCriticalSection(&g_queueLock);
    g_queueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_senderThread = CreateThread(nullptr, 0, SenderThread, nullptr, 0, nullptr);
    g_setupThread = CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
    g_osdMessage = RegisterWindowMessageW(L"Windhawk_TopBar_Osd");
    g_osdThread = CreateThread(nullptr, 0, OsdThread, nullptr, 0, nullptr);
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
    if (g_osdThread) {
        WaitForSingleObject(g_osdThread, 5000);
        CloseHandle(g_osdThread);
        g_osdThread = nullptr;
    }
    // The hook is gone, so nothing queues any more; the sender may be mid-send (up to 200 ms).
    if (g_senderThread) {
        WaitForSingleObject(g_senderThread, 5000);
        CloseHandle(g_senderThread);
        g_senderThread = nullptr;
    }
    if (g_queueEvent) {
        CloseHandle(g_queueEvent);
        g_queueEvent = nullptr;
    }
    DeleteCriticalSection(&g_queueLock);
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}
