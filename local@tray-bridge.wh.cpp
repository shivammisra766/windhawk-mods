// ==WindhawkMod==
// @id              tray-bridge
// @name            Tray bridge
// @description     Passes tray icon updates to the top bar, which shows the icons
// @version         1.3
// @author          local
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -luser32
// ==/WindhawkMod==

// Apps register tray icons by sending the taskbar a WM_COPYDATA message. This only looks at
// those messages on their way past and copies them to the top bar: the taskbar itself carries
// on as usual and nothing is replaced. The sender waits for this, so the bar can copy an icon
// before the app is able to destroy it.

#include <windows.h>
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
    COPYDATASTRUCT forward{kBarCopyData, sizeof(out), &out};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(bar, WM_COPYDATA, reinterpret_cast<WPARAM>(g_tray), reinterpret_cast<LPARAM>(&forward),
                        SMTO_ABORTIFHUNG, 200, &result);
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

}  // namespace

BOOL Wh_ModInit() {
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
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}
