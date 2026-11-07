// ==WindhawkMod==
// @id              tray-bridge
// @name            Tray bridge
// @description     Passes tray icon updates to the top bar, and places the volume and brightness popup over its centre bubble
// @version         1.9
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
// on as usual and nothing is replaced. The bridge copies each icon while the app still waits, and
// keeps a table of the current icons, so a bar that starts later is sent the full set.

#include <windows.h>
#include <windhawk_utils.h>
#include <cwchar>
#include <cstddef>
#include <vector>

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

// The bar sets this when it starts, asking for every icon the taskbar already has.
constexpr wchar_t kReplayEventName[] = L"Local\\WindhawkTopBarTrayReplay";
HANDLE g_replayEvent = nullptr;
// Marks the bridge's own TaskbarCreated broadcast so the bar does not treat it as a new taskbar.
constexpr WPARAM kBridgeBroadcast = 0x7472;

// An update on its way to the bar. The icon is the bridge's own copy, made while the app was still
// waiting, so it stays valid however late it is sent; the table below takes it over.
struct QueuedUpdate {
    BarTrayMessage msg;
    HICON copy;
};

constexpr int kQueueSize = 256;
CRITICAL_SECTION g_queueLock;
HANDLE g_queueEvent = nullptr;
HANDLE g_senderThread = nullptr;
int g_queueHead = 0, g_queueCount = 0;
QueuedUpdate g_queue[kQueueSize];

// Every icon the taskbar currently shows, kept so a bar that starts later gets them all.
// Only the sender thread touches it (and Wh_ModUninit after that thread has ended).
struct KnownIcon {
    DWORD hwnd;
    DWORD id;
    DWORD callback;
    DWORD version;
    bool hasGuid;
    bool hidden;
    GUID guid;
    HICON icon;  // Owned.
    WCHAR tip[128];
};
std::vector<KnownIcon> g_known;
HWND g_lastBar = nullptr;

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
    // Recorded even with no bar running, so a bar that starts later gets the full set.
    const DWORD size = cds->cbData - offsetof(TrayData, nid);
    QueuedUpdate out{};
    out.msg.message = data->message;
    out.msg.hwnd = data->nid.hWnd;
    out.msg.id = data->nid.uID;
    out.msg.flags = data->nid.uFlags;
    out.msg.callback = data->nid.uCallbackMessage;
    out.msg.icon = data->nid.hIcon;
    if (size >= offsetof(NotifyIconData32, dwState)) {
        wcsncpy_s(out.msg.tip, data->nid.szTip, _TRUNCATE);
    }
    if (size >= offsetof(NotifyIconData32, szInfo)) {
        out.msg.state = data->nid.dwState;
        out.msg.stateMask = data->nid.dwStateMask;
    }
    if (size >= offsetof(NotifyIconData32, szInfoTitle)) {
        out.msg.version = data->nid.uVersion;
    }
    if (size >= offsetof(NotifyIconData32, hBalloonIcon)) {
        out.msg.guid = data->nid.guidItem;
    }
    // The app's handle is only sure to be valid while it waits for this message, so copy it now.
    // CopyIcon sends no window messages.
    if ((out.msg.flags & NIF_ICON) && out.msg.icon && (out.msg.message == NIM_ADD || out.msg.message == NIM_MODIFY)) {
        out.copy = CopyIcon(reinterpret_cast<HICON>((UINT_PTR)out.msg.icon));
        if (out.copy) {
            out.msg.icon = (DWORD)(UINT_PTR)out.copy;
        }
    }
    // Never send from the taskbar's thread: while it waits, sent messages are dispatched into it, and during
    // startup (every app re-adding its icon while the taskbar's XAML loads) that reentrancy crashed Explorer
    // (0xc000027b in CoreMessaging). Queue it; the sender thread forwards. A full queue drops the update.
    bool queued = false;
    EnterCriticalSection(&g_queueLock);
    if (g_queueCount < kQueueSize) {
        g_queue[(g_queueHead + g_queueCount) % kQueueSize] = out;
        g_queueCount++;
        queued = true;
        SetEvent(g_queueEvent);
    }
    LeaveCriticalSection(&g_queueLock);
    if (!queued && out.copy) {
        DestroyIcon(out.copy);
    }
}

// Same identity rule as the bar: the GUID if there is one, otherwise window and id.
KnownIcon* FindKnown(const BarTrayMessage& m) {
    const bool hasGuid = (m.flags & NIF_GUID) != 0;
    for (auto& k : g_known) {
        if (hasGuid ? (k.hasGuid && IsEqualGUID(k.guid, m.guid)) : (k.hwnd == m.hwnd && k.id == m.id)) {
            return &k;
        }
    }
    return nullptr;
}

void DropKnown(size_t index) {
    if (g_known[index].icon) DestroyIcon(g_known[index].icon);
    g_known.erase(g_known.begin() + index);
}

// Records the update in the table, which takes over the copied icon.
void ApplyToTable(QueuedUpdate& u) {
    const BarTrayMessage& m = u.msg;
    KnownIcon* k = FindKnown(m);
    if (m.message == NIM_DELETE) {
        if (k) DropKnown(k - g_known.data());
        return;
    }
    if (m.message != NIM_ADD && m.message != NIM_MODIFY && m.message != NIM_SETVERSION) {
        return;
    }
    if (!k) {
        if (m.message == NIM_SETVERSION) return;
        g_known.push_back(KnownIcon{});
        k = &g_known.back();
    }
    if (m.hwnd) k->hwnd = m.hwnd;
    k->id = m.id;
    if (m.flags & NIF_GUID) {
        k->guid = m.guid;
        k->hasGuid = true;
    }
    if (m.message == NIM_SETVERSION) {
        k->version = m.version;
        return;
    }
    if (m.flags & NIF_MESSAGE) k->callback = m.callback;
    if (m.flags & NIF_TIP) wcsncpy_s(k->tip, m.tip, _TRUNCATE);
    if ((m.flags & NIF_STATE) && (m.stateMask & NIS_HIDDEN)) k->hidden = (m.state & NIS_HIDDEN) != 0;
    if (u.copy) {
        if (k->icon) DestroyIcon(k->icon);  // The bar turned it into its own bitmap when it was sent.
        k->icon = u.copy;
        u.copy = nullptr;
    }
}

bool SendToBar(HWND bar, const BarTrayMessage& msg, UINT timeout) {
    BarTrayMessage copy = msg;
    COPYDATASTRUCT forward{kBarCopyData, sizeof(copy), &copy};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(bar, WM_COPYDATA, reinterpret_cast<WPARAM>(g_tray), reinterpret_cast<LPARAM>(&forward),
                               SMTO_ABORTIFHUNG, timeout, &result) != 0;
}

// Sends the whole table as fresh adds; the bar matches them to icons it already has.
void Replay(HWND bar) {
    for (size_t i = g_known.size(); i-- > 0;) {
        if (!IsWindow(reinterpret_cast<HWND>((UINT_PTR)g_known[i].hwnd))) DropKnown(i);
    }
    Wh_Log(L"Replaying %d tray icons to the bar", (int)g_known.size());
    for (const KnownIcon& k : g_known) {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) return;
        BarTrayMessage msg{};
        msg.message = NIM_ADD;
        msg.hwnd = k.hwnd;
        msg.id = k.id;
        msg.flags = NIF_MESSAGE | NIF_TIP | NIF_STATE | (k.icon ? NIF_ICON : 0) | (k.hasGuid ? NIF_GUID : 0);
        msg.callback = k.callback;
        msg.icon = (DWORD)(UINT_PTR)k.icon;
        msg.state = k.hidden ? NIS_HIDDEN : 0;
        msg.stateMask = NIS_HIDDEN;
        msg.guid = k.guid;
        wcsncpy_s(msg.tip, k.tip, _TRUNCATE);
        // The bar may only just have entered its message loop, so it gets longer than a live update.
        if (!SendToBar(bar, msg, 1000)) return;
        if (k.version) {
            msg.message = NIM_SETVERSION;
            msg.version = k.version;
            SendToBar(bar, msg, 1000);
        }
    }
}

DWORD WINAPI SenderThread(void*) {
    HANDLE waits[] = {g_stopEvent, g_queueEvent, g_replayEvent};
    for (;;) {
        // Only the Explorer that owns the taskbar answers a replay: the event is auto-reset, so a folder-window
        // Explorer (which also loads this mod, with an empty table) would otherwise swallow it.
        const DWORD count = (g_replayEvent && g_tray) ? 3 : 2;
        const DWORD wait = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 2) {
            g_lastBar = FindWindowW(L"WindhawkTopBar", nullptr);
            if (g_lastBar) Replay(g_lastBar);
            continue;
        }
        if (wait != WAIT_OBJECT_0 + 1) {
            break;
        }
        for (;;) {
            QueuedUpdate u;
            EnterCriticalSection(&g_queueLock);
            if (!g_queueCount) {
                LeaveCriticalSection(&g_queueLock);
                break;
            }
            u = g_queue[g_queueHead];
            g_queueHead = (g_queueHead + 1) % kQueueSize;
            g_queueCount--;
            LeaveCriticalSection(&g_queueLock);
            ApplyToTable(u);
            if (u.copy) {  // An update the table ignores; the bar still gets it this once.
                HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
                if (bar) SendToBar(bar, u.msg, 200);
                DestroyIcon(u.copy);
                continue;
            }
            HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
            if (!bar) {
                g_lastBar = nullptr;
                continue;
            }
            if (bar != g_lastBar) {
                // A bar not talked to yet (it started without asking): give it everything, this update included.
                g_lastBar = bar;
                Replay(bar);
                continue;
            }
            SendToBar(bar, u.msg, 200);
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

// Seconds since this Explorer started.
ULONGLONG ExplorerAgeSeconds() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG c = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
    const ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return n > c ? (n - c) / 10000000ULL : 0;
}

DWORD WINAPI SetupThread(void*) {
    // The taskbar may still be starting: wait for it (checked often, so few early icons slip past).
    for (int i = 0; i < 1200; i++) {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD pid = 0;
        const DWORD thread = tray ? GetWindowThreadProcessId(tray, &pid) : 0;
        if (thread && pid == GetCurrentProcessId()) {
            g_tray = tray;
            g_hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, nullptr, thread);
            // Loaded into an Explorer that has been up a while (the mod was enabled or updated): its icons were
            // added before the hook, so the table is empty. Ask apps once to add them again; this is the only
            // case with no better source. Posted from this thread, so nothing waits on it. At logon Explorer
            // broadcasts this itself, after the hook is in place.
            if (g_hook && ExplorerAgeSeconds() > 30) {
                const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
                if (taskbarCreated) PostMessageW(HWND_BROADCAST, taskbarCreated, kBridgeBroadcast, 0);
                Wh_Log(L"Loaded late: asked apps to add their tray icons again");
            }
            // A hook dies with the thread that set it, so this one stays, asleep, until unload.
            WaitForSingleObject(g_stopEvent, INFINITE);
            if (g_hook) {
                UnhookWindowsHookEx(g_hook);
                g_hook = nullptr;
            }
            return 0;
        }
        if (WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) {
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
    g_replayEvent = CreateEventW(nullptr, FALSE, FALSE, kReplayEventName);
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
    if (g_replayEvent) {
        CloseHandle(g_replayEvent);
        g_replayEvent = nullptr;
    }
    // The bridge's own icon copies: whatever is still queued, and the table.
    for (int i = 0; i < g_queueCount; i++) {
        HICON copy = g_queue[(g_queueHead + i) % kQueueSize].copy;
        if (copy) DestroyIcon(copy);
    }
    g_queueCount = 0;
    for (auto& k : g_known) {
        if (k.icon) DestroyIcon(k.icon);
    }
    g_known.clear();
    DeleteCriticalSection(&g_queueLock);
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}
