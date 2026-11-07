// ==WindhawkMod==
// @id              top-bar
// @name            Top bar
// @description     Top bar with three bubbles in the Seelen toolbar style, drawn with Direct2D
// @version         4.4
// @author          local
// @include         windhawk.exe
// @compilerOptions -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32 -lversion -lwininet -liphlpapi -luuid -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lruntimeobject -loleaut32 -lsetupapi -lpdh -lwbemuuid
// ==/WindhawkMod==
// ==WindhawkModSettings==
/*
- fontFamily: ""
  $name: Font
  $description: Leave empty for Segoe UI Variable Text, then Segoe UI.
- fontScale: 100
  $name: Text size (percent)
  $description: 50 to 200. Other values mean 100.
- language: ""
  $name: Language
  $description: Menu and label language. Takes effect at once.
  $options:
  - "": Follow Windows
  - en: English
  - hi: Hindi
  - zh: Chinese (Simplified)
  - ja: Japanese
  - ko: Korean
  - de: German
  - es: Spanish
  - fr: French
  - pt: Portuguese
  - ru: Russian
  - ar: Arabic
  - it: Italian
  - tr: Turkish
- clockFormat: system
  $name: Clock hours
  $options:
  - system: Follow Windows
  - h12: 12-hour
  - h24: 24-hour
- clockSeconds: false
  $name: Show seconds
- dateFormat: ""
  $name: Date format
  $description: "A Windows date pattern such as ddd d MMM or yyyy-MM-dd. Leave empty for ddd d MMM."
- dateAbove: false
  $name: Date above the time
  $description: Put the date on a line above the time instead of beside it.
- monitor: primary
  $name: Monitor
  $description: "The bar stays on the chosen monitor. Follow the mouse moves only the island, and the bar stays on the primary one."
  $options:
  - primary: Primary monitor
  - follow: Follow the mouse (island only)
  - "1": Monitor 1
  - "2": Monitor 2
  - "3": Monitor 3
  - "4": Monitor 4
- mediaAutoExpand: true
  $name: Media - open on a new track
  $description: Opens the media player for a few seconds when a new song or video starts.
- mediaExpandBlocklist: ""
  $name: Media - no auto-open for
  $description: Comma separated words. A match in the app name, app id or title keeps the player closed.
- weatherCity: ""
  $name: Weather city
  $description: Leave empty to let wttr.in guess from your address.
- fahrenheit: true
  $name: Fahrenheit
  $description: Off for Celsius and km/h.
*/
// ==/WindhawkModSettings==
#line 1 "core/includes.inc"

// Runs as a tool mod in its own process. Nothing polls: the bar redraws on Windows
// events, one timer per minute for the clock, and a short timer only while the pill moves.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <sddl.h>
#include <wininet.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wbemidl.h>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <dwmapi.h>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <atomic>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <setupapi.h>
#include <dbt.h>
#undef GetCurrentTime  // winrt headers need this macro gone
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

// From iphlpapi. Windhawk includes windows.h first, which rules out winsock2.h,
// so these two are declared directly.
extern "C" DWORD WINAPI NotifyIpInterfaceChange(USHORT family, void* callback, void* context,
                                                BOOLEAN initialNotification, HANDLE* handle);
extern "C" DWORD WINAPI CancelMibChangeNotify2(HANDLE handle);


namespace {


#line 1 "core/com.inc"

// ---- Small COM helpers ----

template <class T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com o) { std::swap(p_, o.p_); return *this; }
    ~Com() { Reset(); }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T** Put() { Reset(); return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

// Pixels kept on the CPU (premultiplied BGRA) with a Direct2D bitmap made on first use for
// the current render target. Tray icons, the avatar and the app icon are all kept this way.
struct Pixmap {
    int w = 0, h = 0;
    std::vector<DWORD> px;
    Com<ID2D1Bitmap> bmp;
    unsigned gen = 0;  // Render target generation the bitmap belongs to.
};

#line 1 "core/anim.inc"

// ---- Springs and the frame clock ----

// Damped spring in fixed 2 ms steps. Positions are unitless; eps and epsV decide when it has settled.
struct Spring {
    float x = 0, v = 0, target = 0;
    float k = 320, c = 34;
    float eps = 0.004f, epsV = 0.04f;
    void Aim(float t, float stiffness, float damping) { target = t; k = stiffness; c = damping; }
    void Snap(float t) { x = target = t; v = 0; }
    bool Settled() const { return fabsf(x - target) < eps && fabsf(v) < epsV; }
    void Step(double dt) {
        while (dt > 0) {
            const double h = dt < 0.002 ? dt : 0.002;
            const double a = k * (target - x) - c * v;
            v += (float)(a * h);
            x += (float)(v * h);
            dt -= h;
        }
        if (Settled()) Snap(target);
    }
};

// Constants from the spec, to tune by eye.
constexpr float kSpringExpandK = 380.0f, kSpringExpandC = 35.0f;
constexpr float kSpringCollapseK = 260.0f, kSpringCollapseC = 32.0f;

double QpcSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

// A thread that wakes only while something animates: it waits for the display's next frame
// and posts one message per frame, never more than one unanswered. The UI thread never blocks.
class FrameClock {
public:
    void Start(HWND hwnd, UINT message) {
        hwnd_ = hwnd;
        message_ = message;
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, Run, this, 0, nullptr);
    }
    void Stop() {
        stop_ = true;
        if (event_) SetEvent(event_);
        if (thread_) {
            WaitForSingleObject(thread_, 2000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (event_) { CloseHandle(event_); event_ = nullptr; }
    }
    void Wake() {  // Start posting frames.
        active_ = true;
        if (event_) SetEvent(event_);
    }
    void Sleep() { active_ = false; }  // Called from the UI thread when nothing is animating.
    void Acked() { pending_ = false; }
    bool Active() const { return active_; }
private:
    static DWORD WINAPI Run(void* self) {
        auto* c = static_cast<FrameClock*>(self);
        while (!c->stop_) {
            WaitForSingleObject(c->event_, INFINITE);
            while (c->active_ && !c->stop_) {
                const double t0 = QpcSeconds();
                const HRESULT hr = DwmFlush();
                // DwmFlush can fail or return at once, for example on the lock screen: never spin.
                if (FAILED(hr) || QpcSeconds() - t0 < 0.002) ::Sleep(16);
                if (!c->pending_.exchange(true)) PostMessageW(c->hwnd_, c->message_, 0, 0);
            }
        }
        return 0;
    }
    HWND hwnd_ = nullptr;
    UINT message_ = 0;
    HANDLE event_ = nullptr;
    HANDLE thread_ = nullptr;
    std::atomic<bool> active_{false};
    std::atomic<bool> pending_{false};
    std::atomic<bool> stop_{false};
};

#line 1 "core/types.inc"

constexpr wchar_t kClassName[] = L"WindhawkTopBar";
constexpr wchar_t kIslandClass[] = L"WindhawkTopBarIsland";  // Must differ: other mods find the bar by kClassName.
constexpr UINT WM_APPBAR_CB = WM_APP + 1;
constexpr UINT WM_REFRESH_NET = WM_APP + 3;
constexpr UINT WM_REFRESH_FOREGROUND = WM_APP + 4;
constexpr UINT WM_PROFILE_DONE = WM_APP + 5;  // From start-menu-pop. wParam 1: Start is still open.
constexpr UINT WM_REFRESH_TITLE = WM_APP + 6;
constexpr UINT WM_APP_FRAME = WM_APP + 10;     // From the frame clock thread: one per display frame while animating.
constexpr UINT WM_APP_SETTINGS = WM_APP + 11;  // From the settings callback, which runs on another thread.
constexpr UINT WM_APP_PAINT = WM_APP + 12;     // Coalesced repaint request.
constexpr UINT WM_APP_POST = WM_APP + 13;      // Services::Post: lParam is a heap std::function<void()>.
constexpr UINT_PTR kClockTimer = 1;
constexpr UINT_PTR kTitleTimer = 2;  // Coalesces title changes.
constexpr UINT_PTR kPanelTimer = 3;
constexpr UINT_PTR kFullscreenTimer = 4;
constexpr UINT_PTR kReopenTimer = 5;
constexpr UINT_PTR kQuickCloseTimer = 6;
constexpr UINT_PTR kQuickPageTimer = 7;
constexpr UINT_PTR kQuickReopenTimer = 8;
constexpr UINT_PTR kDwellTimer = 9;    // Hover dwell before the island expands.
constexpr UINT_PTR kZoneTimer = 10;    // Polls the pointer against the hover zone, only while expanded.
constexpr UINT_PTR kAlertTimer = 11;   // Next alert deadline.
constexpr UINT_PTR kFollowTimer = 13;  // Follow-mouse: which monitor the island is on. Exists only in that mode.
constexpr int kHotkeyId = 1;  // Ctrl+Alt+D
constexpr UINT_PTR kDropTimer = 14;     // Dwell before a drag over the bar opens the shelf.
constexpr UINT_PTR kSearchCloseTimer = 15;  // Closes Windows Search left open behind another app.
constexpr UINT_PTR kModuleTimerBase = 100;  // Module timers: base + slot.

// From the tray-bridge mod in Explorer, which declares the same layout.
constexpr ULONG_PTR kTrayCopyData = 0x7472;
// Set by the bar at start; the bridge then sends its whole table of current icons.
constexpr wchar_t kTrayReplayEventName[] = L"Local\\WindhawkTopBarTrayReplay";
// wParam of the bridge's own TaskbarCreated broadcast (only when it loads into a running Explorer).
constexpr WPARAM kTrayBridgeBroadcast = 0x7472;
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
// Modes understood by the panel-split mod inside ShellExperienceHost. Native: notifications alone at
// their usual place above the dock (panel-split's default while the bar is up). Calendar: alone, under the island.
constexpr WPARAM kPanelNative = 0;
constexpr WPARAM kPanelCalendar = 2;
constexpr int kCalendarClaimMs = 4000;  // A panel host focused this soon after the bar opened the calendar is the calendar.

// Seelen fancy-toolbar, in CSS px scaled by DPI: item size 16, padding 8, margin 0.
constexpr float kItem = 16.0f;
constexpr float kItemPad = 4.0f;
constexpr float kBarH = 32.0f;
constexpr float kGap = 4.0f;
constexpr float kItemRadius = 4.8f;
constexpr float kFontSize = 12.8f;
constexpr float kPillH = 24.0f;  // Height of all three bubbles.
constexpr float kPillPadX = 12.0f;
// Island (notch). DIP.
constexpr float kNotchH = 30.0f;       // Collapsed height; the bar strip is 32.
constexpr float kNotchShoulder = 6.0f; // Concave fillet where the notch meets the screen edge.
constexpr float kNotchRadius = 10.0f;  // Bottom corners, collapsed.
constexpr float kNotchRadiusOpen = 18.0f;
constexpr float kIslandMaxW = 712.0f;  // Stage: the largest body any module may declare.
constexpr float kIslandMaxH = 184.0f;
constexpr UINT kDwellMs = 120;
constexpr UINT kLeaveGraceMs = 250;
constexpr UINT kZonePollMs = 50;
constexpr float kBubbleMargin = 8.0f;  // Between a side bubble and the screen edge.
constexpr float kBubblePad = 8.0f;     // Inside a side bubble, before the first item.
constexpr float kBubbleClear = 12.0f;  // Least gap between the left bubble and the centre one.
constexpr float kPanelGap = 8.0f;  // Between the bar and a native panel.
constexpr float kPanelEdge = 8.0f;  // Keeps a panel this far from the screen edge.
constexpr float kPanelDrop = 10.0f;  // A panel drops this far into place as it opens.
constexpr int kPanelDropMs = 140;
constexpr int kPanelAnchorMs = 1500;  // A click anchors the panel it opens for this long.

D2D1_COLOR_F Rgb(int r, int g, int b, int a = 255) {
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

// Side bubbles are a little see-through, the centre one is solid. Text is Seelen's gray.
const D2D1_COLOR_F kSideBg = Rgb(0x10, 0x10, 0x10, 0xDC);
const D2D1_COLOR_F kCentreBg = Rgb(0x10, 0x10, 0x10);
const D2D1_COLOR_F kFg = Rgb(228, 228, 228);

const GUID kGuidBatteryPercent = {0xA7AD8041, 0xB45A, 0x4CAE, {0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1}};
const GUID kGuidAcDc = {0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};

enum class Action { None, QuickSettings, Wifi, Bluetooth, TrayIcon, Profile };

// Stable ids for hover, so a list that changes under the pointer doesn't move the highlight.
enum HitId { H_none = 0, H_profile = 1, H_settings, H_battery, H_wifi, H_bluetooth, H_trayBase = 1000 };

struct Hit {
    RECT rect;
    Action action;
    int id = 0;
    int data = 0;
};

struct TrayIcon {
    HWND hwnd = nullptr;
    UINT id = 0;
    GUID guid{};
    bool hasGuid = false;
    UINT callback = 0;
    UINT version = 0;
    bool hidden = false;
    int uid = 0;  // Stable for the life of the icon.
    Pixmap* pix = nullptr;
};

HWND g_hwnd = nullptr;
HWND g_isHwnd = nullptr;  // The island window (island.inc)
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
UINT g_taskbarCreated = 0;
bool g_registered = false;
bool g_tracking = false;
bool g_fullscreen = false;
bool g_abnFullscreen = false;
UINT g_panelMessage = 0;
WPARAM g_panelMode = 0;
int g_panelTicks = 0;
int g_panelResend = 0;
Action g_quickPage = Action::None;
int g_anchorX = 0;
ULONGLONG g_anchorTick = 0;
ULONGLONG g_panelOpenTick = 0;
ULONGLONG g_calendarTick = 0;  // Last time the bar opened (or reopened) the calendar.
RECT g_nativePanelRect{};      // The panel host's window where Windows keeps it, saved before the bar first moves it.
bool g_panelMoved = false;     // The bar moved that window for the calendar and has not put it back yet.
HWND g_notificationPanel = nullptr;
int g_regionTop[3] = {};  // Clip top last seen for each mode, to place the panel before it opens.
int g_hoverId = H_none;
std::vector<Hit> g_hits;

bool g_pinned = false;      // Island held open by the menu; saved in the mod's own storage.
bool g_hidden = false;      // Hidden by the hotkey or the menu: windows gone, strip released.
bool g_fsRaw = false;       // A fullscreen app is up, whether or not the hotkey shows the bar over it.
bool g_fsOverride = false;  // The hotkey brought the bar back over that app.
RECT g_isMon{};             // rcMonitor the island hangs from: g_monitor, or the pointer's monitor when following.
RECT g_monitor{};  // rcMonitor of the bar's monitor, set by PlaceBar. The notch hangs from its top edge.

HWINEVENTHOOK g_foregroundHook = nullptr;
HWINEVENTHOOK g_titleHook = nullptr;
DWORD g_titlePid = 0;
bool g_titlePending = false;
FrameClock g_clock;
double g_lastFrame = 0;
HANDLE g_netNotify = nullptr;
HPOWERNOTIFY g_powerA = nullptr;
HPOWERNOTIFY g_powerB = nullptr;
Pixmap* g_avatar = nullptr;
std::wstring g_avatarPath;
int g_avatarPx = 0;  // Size the avatar was last scaled to; 0 when it was not tried yet.
Pixmap* g_appIcon = nullptr;
std::wstring g_appIconPath;

std::wstring g_appName;
std::wstring g_title;
BYTE g_accent[3] = {0, 120, 212};
int g_battery = -1;
bool g_charging = false;
bool g_connected = true;
bool g_profileOpen = false;
bool g_profileAtDown = false;
UINT g_profileMessage = 0;
bool g_panelCold = false;
bool g_quickHadFocus = false;
bool g_searchHadFocus = false;
PCWSTR g_quickPendingUri = nullptr;
int g_quickReopenTries = 0;
bool g_quickReopenSettled = false;
int g_reopenTries = 0;
int g_reopenStage = 0;
int g_reopenOpens = 0;
std::vector<TrayIcon> g_tray;
int g_trayUid = 0;
ULONGLONG g_trayBroadcastTick = 0;

float Scale() {
    return g_hwnd ? GetDpiForWindow(g_hwnd) / 96.0f : 1.0f;
}

#line 1 "core/icons.inc"
enum IconId { I_wifi, I_offline, I_bluetooth, I_settings, I_bell, I_calendar, I_batfull, I_bathigh, I_batmed, I_batlow, I_batwarn, I_bolt };
struct IconPart { bool fill; float stroke; const char* d; };
struct IconDef { float w; float h; int count; IconPart parts[8]; };
const IconDef kIcons[] = {
    {640.0f, 512.0f, 1, {{true, 0.0f, "M634.91 154.88C457.74-8.99 182.19-8.93 5.09 154.88c-6.66 6.16-6.79 16.59-.35 22.98l34.24 33.97c6.14 6.1 16.02 6.23 22.4.38 145.92-133.68 371.3-133.71 517.25 0 6.38 5.85 16.26 5.71 22.4-.38l34.24-33.97c6.43-6.39 6.3-16.82-.36-22.98zM320 352c-35.35 0-64 28.65-64 64s28.65 64 64 64 64-28.65 64-64-28.65-64-64-64zm202.67-83.59c-115.26-101.93-290.21-101.82-405.34 0-6.9 6.1-7.12 16.69-.57 23.15l34.44 33.99c6 5.92 15.66 6.32 22.05.8 83.95-72.57 209.74-72.41 293.49 0 6.39 5.52 16.05 5.13 22.05-.8l34.44-33.99c6.56-6.46 6.33-17.06-.56-23.15z"}}},
    {24.0f, 24.0f, 7, {{false, 2.0f, "M21 12a9 9 0 1 0 -8.985 9"}, {false, 2.0f, "M3.6 9h16.8"}, {false, 2.0f, "M3.6 15h9.9"}, {false, 2.0f, "M11.5 3a17 17 0 0 0 0 18"}, {false, 2.0f, "M12.5 3a16.991 16.991 0 0 1 2.53 10.275"}, {false, 2.0f, "M19 19m-3 0a3 3 0 1 0 6 0a3 3 0 1 0 -6 0"}, {false, 2.0f, "M17 21l4 -4"}}},
    {24.0f, 24.0f, 1, {{false, 2.0f, "M7 8l10 8l-5 4l0 -16l5 4l-10 8"}}},
    {24.0f, 24.0f, 4, {{false, 2.0f, "M20 7h-9"}, {false, 2.0f, "M14 17H5"}, {false, 2.0f, "M14 17 a3 3 0 1 0 6 0 a3 3 0 1 0 -6 0 z"}, {false, 2.0f, "M4 7 a3 3 0 1 0 6 0 a3 3 0 1 0 -6 0 z"}}},
    {24.0f, 24.0f, 1, {{true, 0.0f, "M12 22c1.1 0 2-.9 2-2h-4c0 1.1.9 2 2 2zm6-6v-5c0-3.07-1.63-5.64-4.5-6.32V4c0-.83-.67-1.5-1.5-1.5s-1.5.67-1.5 1.5v.68C7.64 5.36 6 7.92 6 11v5l-2 2v1h16v-1l-2-2zm-2 1H8v-6c0-2.48 1.51-4.5 4-4.5s4 2.02 4 4.5v6z"}}},
    {448.0f, 512.0f, 1, {{true, 0.0f, "M12 192h424c6.6 0 12 5.4 12 12v260c0 26.5-21.5 48-48 48H48c-26.5 0-48-21.5-48-48V204c0-6.6 5.4-12 12-12zm436-44v-36c0-26.5-21.5-48-48-48h-48V12c0-6.6-5.4-12-12-12h-40c-6.6 0-12 5.4-12 12v52H160V12c0-6.6-5.4-12-12-12h-40c-6.6 0-12 5.4-12 12v52H48C21.5 64 0 85.5 0 112v36c0 6.6 5.4 12 12 12h424c6.6 0 12-5.4 12-12z"}}},
    {256.0f, 256.0f, 1, {{true, 0.0f, "M200,56H32A24,24,0,0,0,8,80v96a24,24,0,0,0,24,24H200a24,24,0,0,0,24-24V80A24,24,0,0,0,200,56Zm8,120a8,8,0,0,1-8,8H32a8,8,0,0,1-8-8V80a8,8,0,0,1,8-8H200a8,8,0,0,1,8,8ZM192,96v64a8,8,0,0,1-8,8H48a8,8,0,0,1-8-8V96a8,8,0,0,1,8-8H184A8,8,0,0,1,192,96Zm64,0v64a8,8,0,0,1-16,0V96a8,8,0,0,1,16,0Z"}}},
    {256.0f, 256.0f, 1, {{true, 0.0f, "M152,96v64a8,8,0,0,1-8,8H48a8,8,0,0,1-8-8V96a8,8,0,0,1,8-8h96A8,8,0,0,1,152,96Zm72-16v96a24,24,0,0,1-24,24H32A24,24,0,0,1,8,176V80A24,24,0,0,1,32,56H200A24,24,0,0,1,224,80Zm-16,0a8,8,0,0,0-8-8H32a8,8,0,0,0-8,8v96a8,8,0,0,0,8,8H200a8,8,0,0,0,8-8Zm40,8a8,8,0,0,0-8,8v64a8,8,0,0,0,16,0V96A8,8,0,0,0,248,88Z"}}},
    {256.0f, 256.0f, 1, {{true, 0.0f, "M112,96v64a8,8,0,0,1-8,8H48a8,8,0,0,1-8-8V96a8,8,0,0,1,8-8h56A8,8,0,0,1,112,96ZM224,80v96a24,24,0,0,1-24,24H32A24,24,0,0,1,8,176V80A24,24,0,0,1,32,56H200A24,24,0,0,1,224,80Zm-16,0a8,8,0,0,0-8-8H32a8,8,0,0,0-8,8v96a8,8,0,0,0,8,8H200a8,8,0,0,0,8-8Zm40,8a8,8,0,0,0-8,8v64a8,8,0,0,0,16,0V96A8,8,0,0,0,248,88Z"}}},
    {256.0f, 256.0f, 1, {{true, 0.0f, "M72,96v64a8,8,0,0,1-8,8H48a8,8,0,0,1-8-8V96a8,8,0,0,1,8-8H64A8,8,0,0,1,72,96ZM224,80v96a24,24,0,0,1-24,24H32A24,24,0,0,1,8,176V80A24,24,0,0,1,32,56H200A24,24,0,0,1,224,80Zm-16,0a8,8,0,0,0-8-8H32a8,8,0,0,0-8,8v96a8,8,0,0,0,8,8H200a8,8,0,0,0,8-8Zm40,8a8,8,0,0,0-8,8v64a8,8,0,0,0,16,0V96A8,8,0,0,0,248,88Z"}}},
    {256.0f, 256.0f, 1, {{true, 0.0f, "M256,96v64a8,8,0,0,1-16,0V96a8,8,0,0,1,16,0ZM224,80v96a24,24,0,0,1-24,24H32A24,24,0,0,1,8,176V80A24,24,0,0,1,32,56H200A24,24,0,0,1,224,80Zm-16,0a8,8,0,0,0-8-8H32a8,8,0,0,0-8,8v96a8,8,0,0,0,8,8H200a8,8,0,0,0,8-8Zm-92,52a8,8,0,0,0,8-8V96a8,8,0,0,0-16,0v28A8,8,0,0,0,116,132Zm0,12a12,12,0,1,0,12,12A12,12,0,0,0,116,144Z"}}},
    {16.0f, 16.0f, 1, {{true, 0.0f, "M11.251.068a.5.5 0 0 1 .227.58L9.677 6.5H13a.5.5 0 0 1 .364.843l-8 8.5a.5.5 0 0 1-.842-.49L6.323 9.5H3a.5.5 0 0 1-.364-.843l8-8.5a.5.5 0 0 1 .615-.09z"}}},
};

#line 1 "core/geometry.inc"

// ---- SVG path data to Direct2D geometry ----
// Geometries belong to the factory, so they are made once and shared by every render target.

// Lets the arc converter and the parser write to a geometry sink one figure at a time.
struct GeoSink {
    ID2D1GeometrySink* s;
    bool open = false;
    float cx = 0, cy = 0;
    void Ensure() {
        if (!open) {
            s->BeginFigure(D2D1::Point2F(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
            open = true;
        }
    }
    void Close(bool closed) {
        if (open) {
            s->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            open = false;
        }
    }
    void Move(float x, float y) { Close(false); cx = x; cy = y; }
    void Line(float, float, float x1, float y1) {
        Ensure();
        s->AddLine(D2D1::Point2F(x1, y1));
        cx = x1; cy = y1;
    }
    void Bezier(float, float, float c1x, float c1y, float c2x, float c2y, float x1, float y1) {
        Ensure();
        s->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c1x, c1y), D2D1::Point2F(c2x, c2y), D2D1::Point2F(x1, y1)));
        cx = x1; cy = y1;
    }
};

struct SvgReader {
    const char* p;
    void Skip() {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    float Num() {
        Skip();
        char* end = nullptr;
        float v = strtof(p, &end);
        p = end;
        return v;
    }
    float Flag() {
        Skip();
        return *p++ == '1' ? 1.0f : 0.0f;
    }
};

// SVG elliptical arc as cubic Beziers (endpoint to centre conversion from the SVG spec).
void ArcToBeziers(GeoSink& out, float x0, float y0, float rx, float ry, float rot, bool large, bool sweep, float x1,
                  float y1) {
    if (rx == 0 || ry == 0) {
        out.Line(x0, y0, x1, y1);
        return;
    }
    const double pi = 3.14159265358979323846;
    double phi = rot * pi / 180.0, c = cos(phi), s = sin(phi);
    double dx = (x0 - x1) / 2.0, dy = (y0 - y1) / 2.0;
    double xp = c * dx + s * dy, yp = -s * dx + c * dy;
    double a = fabs(rx), b = fabs(ry);
    double l = xp * xp / (a * a) + yp * yp / (b * b);
    if (l > 1) {
        a *= sqrt(l);
        b *= sqrt(l);
    }
    double num = a * a * b * b - a * a * yp * yp - b * b * xp * xp;
    double den = a * a * yp * yp + b * b * xp * xp;
    double k = den == 0 ? 0 : sqrt(fmax(0.0, num / den)) * (large == sweep ? -1 : 1);
    double cxp = k * a * yp / b, cyp = -k * b * xp / a;
    double cx = c * cxp - s * cyp + (x0 + x1) / 2.0, cy = s * cxp + c * cyp + (y0 + y1) / 2.0;
    double t1 = atan2((yp - cyp) / b, (xp - cxp) / a);
    double t2 = atan2((-yp - cyp) / b, (-xp - cxp) / a);
    double delta = t2 - t1;
    if (sweep && delta < 0) delta += 2 * pi;
    if (!sweep && delta > 0) delta -= 2 * pi;
    int segments = (int)ceil(fabs(delta) / (pi / 2.0) - 1e-6);
    if (segments < 1) segments = 1;
    double step = delta / segments, h = 4.0 / 3.0 * tan(step / 4.0);
    double px = x0, py = y0;
    for (int i = 0; i < segments; i++) {
        double e1 = t1 + i * step, e2 = e1 + step;
        double c1x = cos(e1) - h * sin(e1), c1y = sin(e1) + h * cos(e1);
        double c2x = cos(e2) + h * sin(e2), c2y = sin(e2) - h * cos(e2);
        auto mapX = [&](double u, double v) { return (float)(cx + c * a * u - s * b * v); };
        auto mapY = [&](double u, double v) { return (float)(cy + s * a * u + c * b * v); };
        float ex = mapX(cos(e2), sin(e2)), ey = mapY(cos(e2), sin(e2));
        out.Bezier((float)px, (float)py, mapX(c1x, c1y), mapY(c1x, c1y), mapX(c2x, c2y), mapY(c2x, c2y), ex, ey);
        px = ex;
        py = ey;
    }
}

Com<ID2D1PathGeometry> ParseSvgPath(ID2D1Factory* factory, const char* d) {
    Com<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(geometry.Put()))) return {};
    Com<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.Put()))) return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    GeoSink out{sink.Get()};
    SvgReader r{d};
    float x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0;
    char cmd = 0, prev = 0;
    for (;;) {
        r.Skip();
        if (!*r.p) break;
        if ((*r.p >= 'a' && *r.p <= 'z') || (*r.p >= 'A' && *r.p <= 'Z')) {
            cmd = *r.p++;
        }
        const bool rel = cmd >= 'a';
        const float bx = rel ? x : 0, by = rel ? y : 0;
        bool done = false;
        switch (cmd | 0x20) {
            case 'm': {
                x = bx + r.Num();
                y = by + r.Num();
                sx = x;
                sy = y;
                out.Move(x, y);
                cmd = rel ? 'l' : 'L';
                prev = 'm';
                continue;
            }
            case 'l': {
                float nx = bx + r.Num(), ny = by + r.Num();
                out.Line(x, y, nx, ny);
                x = nx;
                y = ny;
                break;
            }
            case 'h': {
                float nx = bx + r.Num();
                out.Line(x, y, nx, y);
                x = nx;
                break;
            }
            case 'v': {
                float ny = by + r.Num();
                out.Line(x, y, x, ny);
                y = ny;
                break;
            }
            case 'c': {
                float x1 = bx + r.Num(), y1 = by + r.Num();
                float x2 = bx + r.Num(), y2 = by + r.Num();
                float nx = bx + r.Num(), ny = by + r.Num();
                out.Bezier(x, y, x1, y1, x2, y2, nx, ny);
                cx = x2;
                cy = y2;
                x = nx;
                y = ny;
                prev = 'c';
                continue;
            }
            case 's': {
                float x1 = prev == 'c' ? 2 * x - cx : x, y1 = prev == 'c' ? 2 * y - cy : y;
                float x2 = bx + r.Num(), y2 = by + r.Num();
                float nx = bx + r.Num(), ny = by + r.Num();
                out.Bezier(x, y, x1, y1, x2, y2, nx, ny);
                cx = x2;
                cy = y2;
                x = nx;
                y = ny;
                prev = 'c';
                continue;
            }
            case 'q': {
                float qx = bx + r.Num(), qy = by + r.Num();
                float nx = bx + r.Num(), ny = by + r.Num();
                out.Bezier(x, y, x + 2.0f / 3.0f * (qx - x), y + 2.0f / 3.0f * (qy - y),
                           nx + 2.0f / 3.0f * (qx - nx), ny + 2.0f / 3.0f * (qy - ny), nx, ny);
                x = nx;
                y = ny;
                break;
            }
            case 'a': {
                float rx = r.Num(), ry = r.Num(), rot = r.Num();
                bool large = r.Flag() != 0, sweep = r.Flag() != 0;
                float nx = bx + r.Num(), ny = by + r.Num();
                ArcToBeziers(out, x, y, rx, ry, rot, large, sweep, nx, ny);
                x = nx;
                y = ny;
                break;
            }
            case 'z': {
                out.Close(true);
                x = sx;
                y = sy;
                out.cx = sx;
                out.cy = sy;
                prev = 'z';
                continue;
            }
            default:
                done = true;  // Unknown command: keep what was parsed.
        }
        if (done) break;
        prev = cmd | 0x20;
    }
    out.Close(false);
    sink->Close();
    return geometry;
}

// The icon table's parts as geometries, parsed on first use.
class IconGeometries {
public:
    void Init(ID2D1Factory* factory) {
        factory_ = factory;
        D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
            D2D1_DASH_STYLE_SOLID, 0.0f);
        factory->CreateStrokeStyle(props, nullptr, 0, stroke_.Put());
    }
    void Release() { geos_.clear(); stroke_.Reset(); }
    static float Width(IconId id, float h) { return kIcons[id].w / kIcons[id].h * h; }

    // Centre (cx, cy), height h. The brush supplies the colour and opacity.
    void Draw(ID2D1RenderTarget* rt, ID2D1Brush* brush, IconId id, float cx, float cy, float h) {
        const IconDef& def = kIcons[id];
        const float k = h / def.h;
        D2D1_MATRIX_3X2_F saved;
        rt->GetTransform(&saved);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) *
                         D2D1::Matrix3x2F::Translation(cx - def.w * k / 2.0f, cy - h / 2.0f) * saved);
        for (int i = 0; i < def.count; i++) {
            ID2D1PathGeometry* g = Part(id, i);
            if (!g) continue;
            if (def.parts[i].fill) rt->FillGeometry(g, brush);
            if (def.parts[i].stroke > 0) rt->DrawGeometry(g, brush, def.parts[i].stroke, stroke_.Get());
        }
        rt->SetTransform(saved);
    }

private:
    ID2D1PathGeometry* Part(int icon, int part) {
        int index = 0;
        for (int i = 0; i < icon; i++) index += kIcons[i].count;
        index += part;
        if ((int)geos_.size() <= index) geos_.resize(index + 1);
        if (!geos_[index]) geos_[index] = ParseSvgPath(factory_, kIcons[icon].parts[part].d);
        return geos_[index].Get();
    }
    ID2D1Factory* factory_ = nullptr;
    Com<ID2D1StrokeStyle> stroke_;
    std::vector<Com<ID2D1PathGeometry>> geos_;
};

#line 1 "core/surface.inc"

// ---- LayeredSurface: a retained premultiplied DIB drawn with Direct2D and pushed to a layered window ----

// One software DC render target, kept for the life of the surface. Drawing is limited to a dirty
// rectangle, and only that rectangle is handed to UpdateLayeredWindowIndirect.
class LayeredSurface {
public:
    ~LayeredSurface() { Destroy(); }

    // Sizes the DIB. Contents are lost (all transparent) when the size changes.
    bool Resize(ID2D1Factory* factory, int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (mem_ && w == w_ && h == h_ && rt_) return true;
        Destroy();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        mem_ = CreateCompatibleDC(screen);
        dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!mem_ || !dib_) {
            Destroy();
            return false;
        }
        old_ = SelectObject(mem_, dib_);
        w_ = w;
        h_ = h;
        // The software target keeps the working set small: the default one loads the GPU driver.
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        if (FAILED(factory->CreateDCRenderTarget(&props, rt_.Put()))) {
            Destroy();
            return false;
        }
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType is invalid with transparency
        ++generation_;
        return true;
    }

    // Starts drawing: everything outside `dirty` is left alone, everything inside is cleared.
    ID2D1DCRenderTarget* Begin(const RECT& dirty) {
        if (!rt_) return nullptr;
        RECT all{0, 0, w_, h_};
        if (FAILED(rt_->BindDC(mem_, &all))) return nullptr;
        rt_->BeginDraw();
        rt_->PushAxisAlignedClip(D2D1::RectF((float)dirty.left, (float)dirty.top, (float)dirty.right, (float)dirty.bottom),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        rt_->Clear(D2D1::ColorF(0, baseAlpha_));
        return rt_.Get();
    }

    // Returns false if the target was lost and has to be rebuilt by the next Resize.
    bool End() {
        rt_->PopAxisAlignedClip();
        const HRESULT hr = rt_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            rt_.Reset();
            return false;
        }
        return true;
    }

    // `pos` is the window's top left on screen.
    void Present(HWND hwnd, POINT pos, const RECT& dirty, bool whole) {
        if (!mem_) return;
        HDC screen = GetDC(nullptr);
        POINT src{0, 0};
        SIZE size{w_, h_};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        RECT rc = dirty;
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.hdcDst = screen;
        info.pptDst = &pos;
        info.psize = &size;
        info.hdcSrc = mem_;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = whole ? nullptr : &rc;
        UpdateLayeredWindowIndirect(hwnd, &info);
        ReleaseDC(nullptr, screen);
    }

    // Pushes only the crop (srcX, 0, size) of the DIB, and moves and resizes the window to match. Used by the
    // island, whose window is just the current shape while the DIB is the largest stage.
    void PresentCrop(HWND hwnd, POINT pos, POINT src, SIZE size, BYTE alpha = 255) {
        if (!mem_) return;
        HDC screen = GetDC(nullptr);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.hdcDst = screen;
        info.pptDst = &pos;
        info.psize = &size;
        info.hdcSrc = mem_;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        UpdateLayeredWindowIndirect(hwnd, &info);
        ReleaseDC(nullptr, screen);
    }

    // Alpha of the cleared background. 1/255 keeps transparent areas hit-testable, so a drag over them still reaches OLE.
    void SetBaseAlpha(float a) { baseAlpha_ = a; }
    ID2D1DCRenderTarget* Target() const { return rt_.Get(); }
    unsigned Generation() const { return generation_; }
    int Width() const { return w_; }
    int Height() const { return h_; }

    void Destroy() {
        rt_.Reset();
        if (mem_) {
            if (old_) SelectObject(mem_, old_);
            DeleteDC(mem_);
        }
        if (dib_) DeleteObject(dib_);
        mem_ = nullptr;
        dib_ = nullptr;
        old_ = nullptr;
        bits_ = nullptr;
        w_ = h_ = 0;
    }

private:
    HDC mem_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    unsigned generation_ = 0;
    float baseAlpha_ = 0.0f;
    Com<ID2D1DCRenderTarget> rt_;
};

#line 1 "core/text.inc"

// ---- Text: DirectWrite layouts, cached ----

struct TextOpts {
    const wchar_t* family = nullptr;  // null: the bar's font
    float size = 0;                   // px, 0: the bar's size
    float maxW = 1e6f;
    float maxH = 1e6f;
    bool ellipsis = false;
    bool center = false;              // centred both ways inside maxW x maxH
    int weight = 400;                 // DWRITE_FONT_WEIGHT: 600 semibold, 700 bold
    bool right = false;               // right-aligned inside maxW
    bool wrap = false;                // wrap words inside maxW
};

class TextService {
public:
    bool Init() {
        return SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(factory_.Put())));
    }
    // The first installed family of: the user's choice, Segoe UI Variable Text, Segoe UI.
    void SetFont(const std::wstring& preferred, float sizePx) {
        family_ = PickFamily(preferred);
        size_ = sizePx;
        cache_.clear();
    }
    float DefaultSize() const { return size_; }

    Com<IDWriteTextLayout> Layout(const std::wstring& text, const TextOpts& o = {}) {
        if (!factory_ || text.empty()) return {};
        const wchar_t* family = o.family ? o.family : family_.c_str();
        const float size = o.size > 0 ? o.size : size_;
        wchar_t head[160];
        swprintf_s(head, L"%ls|%.2f|%.1f|%.1f|%d%d%d%d|%d|", family, size, o.maxW, o.maxH, o.ellipsis, o.center, o.right, o.wrap, o.weight);
        const std::wstring key = std::wstring(head) + text;
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        if (cache_.size() > 200) cache_.clear();
        Com<IDWriteTextFormat> fmt;
        if (FAILED(factory_->CreateTextFormat(family, nullptr, (DWRITE_FONT_WEIGHT)o.weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"", fmt.Put()))) {
            return {};
        }
        Com<IDWriteTextLayout> layout;
        if (FAILED(factory_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), o.maxW, o.maxH,
                                              layout.Put()))) {
            return {};
        }
        layout->SetWordWrapping(o.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (o.right) layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (o.center) {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else if (o.maxH < 1e5f) {
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (o.ellipsis) {
            Com<IDWriteInlineObject> sign;
            if (SUCCEEDED(factory_->CreateEllipsisTrimmingSign(layout.Get(), sign.Put()))) {
                DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trimming, sign.Get());
            }
        }
        cache_[key] = layout;
        return layout;
    }

    // Width including trailing spaces, as the GDI+ build measured it.
    float Measure(const std::wstring& text, float size = 0) {
        TextOpts o;
        o.size = size;
        Com<IDWriteTextLayout> layout = Layout(text, o);
        if (!layout) return 0;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    void Draw(ID2D1RenderTarget* rt, const std::wstring& text, float x, float y, ID2D1Brush* brush,
              const TextOpts& o = {}) {
        Com<IDWriteTextLayout> layout = Layout(text, o);
        if (layout) rt->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

private:
    std::wstring PickFamily(const std::wstring& preferred) {
        Com<IDWriteFontCollection> fonts;
        if (!factory_ || FAILED(factory_->GetSystemFontCollection(fonts.Put(), FALSE))) return L"Segoe UI";
        auto has = [&](const wchar_t* name) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            return SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
        };
        if (!preferred.empty() && has(preferred.c_str())) return preferred;
        if (has(L"Segoe UI Variable Text")) return L"Segoe UI Variable Text";
        return L"Segoe UI";
    }
    Com<IDWriteFactory> factory_;
    std::wstring family_ = L"Segoe UI";
    float size_ = 13;
    std::unordered_map<std::wstring, Com<IDWriteTextLayout>> cache_;
};

#line 1 "core/settings.inc"

// ---- Settings: one writer, the UI thread ----
// WhTool_ModSettingsChanged runs on another thread and only posts WM_APP_SETTINGS. The UI thread then
// calls LoadSettings. An unset value reads as empty or zero, and that means "use the default".

struct Settings {
    std::wstring fontFamily;  // empty: Segoe UI Variable Text, then Segoe UI
    int fontScale = 100;      // percent of the usual text size
    std::wstring language;    // empty: follow Windows
    int clockHours = 0;       // 0 follow Windows, 12, 24
    bool clockSeconds = false;
    std::wstring dateFormat;  // empty: "ddd d MMM"; otherwise a GetDateFormat pattern
    bool dateAbove = false;   // date on a line above the time
    int monitor = 0;          // 0 primary, -1 follow the pointer (island only), n: the nth monitor
    bool mediaAutoExpand = true;
    std::wstring mediaBlocklist;  // comma separated, lowercased and trimmed by the media module
    std::wstring weatherCity; // empty: wttr.in guesses from the address
    bool fahrenheit = true;
};

Settings g_settings;
std::wstring g_uiLang;  // Windows display language, cached until the settings are read again.

Settings LoadSettings() {
    Settings s;
    if (PCWSTR v = Wh_GetStringSetting(L"fontFamily")) { s.fontFamily = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"language")) { s.language = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"dateFormat")) { s.dateFormat = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"clockFormat")) {
        s.clockHours = !wcscmp(v, L"h12") ? 12 : !wcscmp(v, L"h24") ? 24 : 0;
        Wh_FreeStringSetting(v);
    }
    if (PCWSTR v = Wh_GetStringSetting(L"monitor")) {
        s.monitor = !wcscmp(v, L"follow") ? -1 : _wtoi(v);  // "primary" and anything else read as 0
        if (s.monitor < -1) s.monitor = 0;
        Wh_FreeStringSetting(v);
    }
    s.clockSeconds = Wh_GetIntSetting(L"clockSeconds") != 0;
    s.dateAbove = Wh_GetIntSetting(L"dateAbove") != 0;
    s.mediaAutoExpand = Wh_GetIntSetting(L"mediaAutoExpand") != 0;
    if (PCWSTR v = Wh_GetStringSetting(L"mediaExpandBlocklist")) { s.mediaBlocklist = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"weatherCity")) { s.weatherCity = v; Wh_FreeStringSetting(v); }
    s.fahrenheit = Wh_GetIntSetting(L"fahrenheit") != 0;
    const int scale = Wh_GetIntSetting(L"fontScale");
    s.fontScale = scale >= 50 && scale <= 200 ? scale : 100;
    g_uiLang.clear();
    return s;
}

#line 1 "core/loc.inc"

// ---- Loc: strings per language, generated from strings.tsv by build-bar.ps1 ----

constexpr int kLocLangCount = 13;
const wchar_t* const kLocLangs[kLocLangCount] = { L"en", L"hi", L"zh", L"ja", L"ko", L"de", L"es", L"fr", L"pt", L"ru", L"ar", L"it", L"tr" };
struct LocEntry { const wchar_t* key; const wchar_t* text[kLocLangCount]; };
const LocEntry kLoc[] = {
    { L"window.title", { L"Top bar", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"" } },
    { L"Media", { L"Media", L"मीडिया", L"媒体", L"メディア", L"미디어", L"Medien", L"Multimedia", L"Média", L"Mídia", L"Медиа", L"", L"Media", L"Medya" } },
    { L"Calendar", { L"Calendar", L"कैलेंडर", L"日历", L"カレンダー", L"캘린더", L"Kalender", L"Calendario", L"Calendrier", L"Calendário", L"Календарь", L"", L"Calendario", L"Takvim" } },
    { L"Weather", { L"Weather", L"मौसम", L"天气", L"天気", L"날씨", L"Wetter", L"Tiempo", L"Météo", L"Tempo", L"Погода", L"", L"Meteo", L"Hava" } },
    { L"Hardware Monitor", { L"Hardware Monitor", L"हार्डवेयर मॉनिटर", L"硬件监视器", L"ハードウェア モニター", L"하드웨어 모니터", L"Hardware-Monitor", L"Monitor de hardware", L"Moniteur matériel", L"Monitor de hardware", L"Монитор системы", L"", L"Monitor hardware", L"Donanım İzleme" } },
    { L"File Tray", { L"File Tray", L"फ़ाइल ट्रे", L"文件托盘", L"ファイル トレイ", L"파일 트레이", L"Dateiablage", L"Bandeja de archivos", L"Bac à fichiers", L"Bandeja de arquivos", L"Файлы", L"", L"Vassoio file", L"Dosya Tepsisi" } },
    { L"Bluetooth", { L"Bluetooth", L"ब्लूटूथ", L"蓝牙", L"Bluetooth", L"블루투스", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"", L"Bluetooth", L"Bluetooth" } },
    { L"Copied", { L"Copied", L"कॉपी किया गया", L"已复制", L"コピーしました", L"복사됨", L"Kopiert", L"Copiado", L"Copié", L"Copiado", L"Скопировано", L"", L"Copiato", L"Kopyalandı" } },
    { L"Volume", { L"Volume", L"वॉल्यूम", L"音量", L"音量", L"볼륨", L"Lautstärke", L"Volumen", L"Volume", L"Volume", L"Громкость", L"", L"Volume", L"Ses" } },
    { L"Brightness", { L"Brightness", L"चमक", L"亮度", L"明るさ", L"밝기", L"Helligkeit", L"Brillo", L"Luminosité", L"Brilho", L"Яркость", L"", L"Luminosità", L"Parlaklık" } },
    { L"Muted", { L"Muted", L"म्यूट", L"已静音", L"ミュート", L"음소거", L"Stumm", L"Silenciado", L"Muet", L"Sem som", L"Без звука", L"", L"Muto", L"Sessiz" } },
    { L"Low Battery", { L"Low Battery", L"बैटरी कम", L"电量低", L"バッテリー残量低下", L"배터리 부족", L"Akku schwach", L"Batería baja", L"Batterie faible", L"Bateria fraca", L"Батарея разряжена", L"", L"Batteria scarica", L"Pil Az" } },
    { L"Charging", { L"Charging", L"चार्ज हो रहा है", L"正在充电", L"充電中", L"충전 중", L"Wird geladen", L"Cargando", L"En charge", L"Carregando", L"Зарядка", L"", L"In carica", L"Şarj oluyor" } },
    { L"Connected", { L"Connected", L"कनेक्ट किया गया", L"已连接", L"接続済み", L"연결됨", L"Verbunden", L"Conectado", L"Connecté", L"Conectado", L"Подключено", L"", L"Connesso", L"Bağlandı" } },
    { L"Disconnected", { L"Disconnected", L"डिस्कनेक्ट किया गया", L"已断开", L"切断されました", L"연결 끊김", L"Getrennt", L"Desconectado", L"Déconnecté", L"Desconectado", L"Отключено", L"", L"Disconnesso", L"Bağlantı kesildi" } },
    { L"Caps Lock", { L"Caps Lock", L"कैप्स लॉक", L"大写锁定", L"Caps Lock", L"Caps Lock", L"Feststelltaste", L"Bloq Mayús", L"Verr. Maj", L"Caps Lock", L"Caps Lock", L"", L"Blocco maiuscole", L"Caps Lock" } },
    { L"Num Lock", { L"Num Lock", L"नम लॉक", L"数字锁定", L"Num Lock", L"Num Lock", L"Num-Taste", L"Bloq Num", L"Verr. Num", L"Num Lock", L"Num Lock", L"", L"Blocco num", L"Num Lock" } },
    { L"On", { L"On", L"चालू", L"开", L"オン", L"켜짐", L"Ein", L"Activado", L"Activé", L"Ligado", L"Вкл", L"", L"Attivo", L"Açık" } },
    { L"Off", { L"Off", L"बंद", L"关", L"オフ", L"꺼짐", L"Aus", L"Desactivado", L"Désactivé", L"Desligado", L"Выкл", L"", L"Disattivo", L"Kapalı" } },
    { L"Do Not Disturb", { L"Do Not Disturb", L"परेशान न करें", L"专注助手", L"応答不可", L"방해 금지", L"Nicht stören", L"No molestar", L"Ne pas déranger", L"Não perturbe", L"Не беспокоить", L"", L"Non disturbare", L"Rahatsız Etme" } },
    { L"Focus", { L"Focus", L"फ़ोकस", L"专注", L"集中", L"집중", L"Fokus", L"Concentración", L"Concentration", L"Foco", L"Фокус", L"", L"Concentrazione", L"Odak" } },
    { L"Break", { L"Break", L"विराम", L"休息", L"休憩", L"휴식", L"Pause", L"Descanso", L"Pause", L"Pausa", L"Перерыв", L"", L"Pausa", L"Mola" } },
    { L"Unknown", { L"Unknown", L"अज्ञात", L"未知", L"不明", L"알 수 없음", L"Unbekannt", L"Desconocido", L"Inconnu", L"Desconhecido", L"Неизвестно", L"", L"Sconosciuto", L"Bilinmiyor" } },
    { L"Loading...", { L"Loading...", L"लोड हो रहा है...", L"加载中...", L"読み込み中...", L"불러오는 중...", L"Wird geladen...", L"Cargando...", L"Chargement...", L"Carregando...", L"Загрузка...", L"", L"Caricamento...", L"Yükleniyor..." } },
    { L"Locating...", { L"Locating...", L"स्थान...", L"定位中...", L"位置情報...", L"위치 확인 중...", L"Standort...", L"Ubicando...", L"Localisation...", L"Localizando...", L"Определение...", L"", L"Localizzazione...", L"Konum..." } },
    { L"Wind", { L"Wind", L"हवा", L"风速", L"風", L"바람", L"Wind", L"Viento", L"Vent", L"Vento", L"Ветер", L"", L"Vento", L"Rüzgar" } },
    { L"Feels Like", { L"Feels Like", L"महसूस", L"体感", L"体感", L"체감", L"Gefühlt", L"Sensación", L"Ressenti", L"Sensação", L"Ощущается", L"", L"Percepita", L"Hissedilen" } },
    { L"Humidity", { L"Humidity", L"नमी", L"湿度", L"湿度", L"습도", L"Luftfeuchte", L"Humedad", L"Humidité", L"Umidade", L"Влажность", L"", L"Umidità", L"Nem" } },
    { L"Drag files here", { L"Drag files here", L"फ़ाइलें यहाँ खींचें", L"将文件拖到此处", L"ここにファイルをドラッグ", L"여기에 파일을 끌어다 놓으세요", L"Dateien hierher ziehen", L"Arrastra archivos aquí", L"Déposez des fichiers ici", L"Arraste arquivos aqui", L"Перетащите файлы сюда", L"", L"Trascina i file qui", L"Dosyaları buraya sürükleyin" } },
    { L"No devices", { L"No devices", L"कोई डिवाइस नहीं", L"无设备", L"デバイスなし", L"장치 없음", L"Keine Geräte", L"Sin dispositivos", L"Aucun appareil", L"Nenhum dispositivo", L"Нет устройств", L"", L"Nessun dispositivo", L"Cihaz yok" } },
    { L"item", { L"item", L"आइटम", L"项", L"項目", L"항목", L"Element", L"elemento", L"élément", L"item", L"элемент", L"", L"elemento", L"öğe" } },
    { L"items", { L"items", L"आइटम", L"项", L"項目", L"항목", L"Elemente", L"elementos", L"éléments", L"itens", L"элементов", L"", L"elementi", L"öğe" } },
    { L"Media source", { L"Media source", L"मीडिया स्रोत", L"媒体来源", L"メディア ソース", L"미디어 소스", L"Medienquelle", L"Fuente multimedia", L"Source multimédia", L"Fonte de mídia", L"Источник медиа", L"", L"Sorgente multimediale", L"Medya kaynağı" } },
    { L"Auto", { L"Auto", L"स्वचालित", L"自动", L"自動", L"자동", L"Automatisch", L"Automático", L"Auto", L"Automático", L"Авто", L"", L"Automatico", L"Otomatik" } },
    { L"Playing from %s", { L"Playing from %s", L"%s से चल रहा है", L"正在 %s 中播放", L"%s で再生中", L"%s에서 재생 중", L"Wiedergabe über %s", L"Reproduciendo desde %s", L"Lecture depuis %s", L"Reproduzindo em %s", L"Воспроизводится в %s", L"", L"In riproduzione da %s", L"%s üzerinden oynatılıyor" } },
    { L"menu.pin", { L"Pin island open", L"खुला रखें", L"固定展开", L"開いたままにする", L"열어 두기", L"Offen halten", L"Mantener abierta", L"Garder ouvert", L"Manter aberta", L"Держать открытым", L"إبقاء مفتوحًا", L"Tieni aperta", L"Açık tut" } },
    { L"menu.unpin", { L"Unpin island", L"अनपिन करें", L"取消固定", L"固定を解除", L"고정 해제", L"Lösen", L"Soltar", L"Détacher", L"Soltar", L"Открепить", L"إلغاء التثبيت", L"Sblocca", L"Sabitlemeyi kaldır" } },
    { L"menu.hide", { L"Hide bar (Ctrl+Alt+D)", L"बार छिपाएँ (Ctrl+Alt+D)", L"隐藏栏 (Ctrl+Alt+D)", L"バーを隠す (Ctrl+Alt+D)", L"바 숨기기 (Ctrl+Alt+D)", L"Leiste ausblenden (Strg+Alt+D)", L"Ocultar barra (Ctrl+Alt+D)", L"Masquer la barre (Ctrl+Alt+D)", L"Ocultar barra (Ctrl+Alt+D)", L"Скрыть панель (Ctrl+Alt+D)", L"إخفاء الشريط (Ctrl+Alt+D)", L"Nascondi barra (Ctrl+Alt+D)", L"Çubuğu gizle (Ctrl+Alt+D)" } },
    { L"media.playingfrom", { L"Playing from %ls", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"" } },
    { L"media.pausedin", { L"Paused in %ls", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"" } },
    { L"media.unknown", { L"Unknown", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"" } },
};

// Language of the current settings: the chosen one, else the Windows display language, else English.
const wchar_t* Loc(const wchar_t* key) {
    static std::wstring cachedFor;
    static int column = 0;
    std::wstring want = g_settings.language;
    if (want.empty()) {
        if (g_uiLang.empty()) {  // The display language, not the regional format.
            wchar_t names[256] = {};
            ULONG count = 0, size = 256;
            if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names, &size) || !names[0]) {
                GetUserDefaultLocaleName(names, LOCALE_NAME_MAX_LENGTH);
            }
            g_uiLang = names;  // The first of the double-null list.
        }
        want = g_uiLang;
    }
    if (want != cachedFor) {
        cachedFor = want;
        column = 0;
        for (int i = 1; i < kLocLangCount; i++) {
            const size_t n = wcslen(kLocLangs[i]);
            if (!_wcsnicmp(want.c_str(), kLocLangs[i], n)) { column = i; break; }
        }
    }
    for (const LocEntry& e : kLoc) {
        if (!wcscmp(e.key, key)) {
            if (e.text[column] && e.text[column][0]) return e.text[column];
            return e.text[0];  // English fallback
        }
    }
    return key;
}

#line 1 "bar/state.inc"
// ---- State readers ----

void ReadAccent() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return;
    }
    BYTE palette[32];
    DWORD size = sizeof(palette);
    if (RegQueryValueExW(key, L"AccentPalette", nullptr, nullptr, palette, &size) == ERROR_SUCCESS && size >= 16) {
        // Base tone: Seelen's hover overlay.
        g_accent[0] = palette[12];
        g_accent[1] = palette[13];
        g_accent[2] = palette[14];
    }
    RegCloseKey(key);
}

D2D1_COLOR_F Overlay(int alpha) {
    return Rgb(g_accent[0], g_accent[1], g_accent[2], alpha);
}

void ReadBattery() {
    SYSTEM_POWER_STATUS st{};
    if (GetSystemPowerStatus(&st) && st.BatteryFlag != 128) {
        g_battery = st.BatteryLifePercent <= 100 ? st.BatteryLifePercent : -1;
        g_charging = st.ACLineStatus == 1;
    } else {
        g_battery = -1;
    }
}

void ReadNetwork() {
    DWORD flags = 0;
    g_connected = InternetGetConnectedState(&flags, 0) != FALSE;
}

// Copies an icon's pixels into a premultiplied BGRA pixmap. Runs while the icon handle is valid.
Pixmap* PixmapFromIcon(HICON icon) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        return nullptr;
    }
    Pixmap* result = nullptr;
    BITMAP bm{};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<DWORD> pixels((size_t)bm.bmWidth * bm.bmHeight);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, info.hbmColor, 0, bm.bmHeight, pixels.data(), &bi, DIB_RGB_COLORS)) {
            bool hasAlpha = false;
            for (DWORD px : pixels) {
                if (px >> 24) {
                    hasAlpha = true;
                    break;
                }
            }
            for (DWORD& px : pixels) {
                if (!hasAlpha) {
                    if (px) px |= 0xFF000000;  // Old icons carry no alpha: any colour is opaque.
                } else {
                    const DWORD a = px >> 24;
                    if (a != 255) {
                        const DWORD r = ((px >> 16) & 0xFF) * a / 255, g = ((px >> 8) & 0xFF) * a / 255,
                                    b = (px & 0xFF) * a / 255;
                        px = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                }
            }
            result = new Pixmap();
            result->w = bm.bmWidth;
            result->h = bm.bmHeight;
            result->px = std::move(pixels);
        }
        ReleaseDC(nullptr, dc);
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return result;
}

// Decodes an image file with WIC and scales it to px x px, premultiplied BGRA.
Pixmap* LoadPixmapFile(const wchar_t* path, int px) {
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                reinterpret_cast<void**>(wic.Put())))) {
        return nullptr;
    }
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICBitmapScaler> scaler;
    Com<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.Put())) ||
        FAILED(decoder->GetFrame(0, frame.Put())) || FAILED(wic->CreateBitmapScaler(scaler.Put())) ||
        FAILED(scaler->Initialize(frame.Get(), px, px, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    auto* out = new Pixmap();
    out->w = out->h = px;
    out->px.resize((size_t)px * px);
    if (FAILED(converter->CopyPixels(nullptr, px * 4, (UINT)(out->px.size() * 4), reinterpret_cast<BYTE*>(out->px.data())))) {
        delete out;
        return nullptr;
    }
    return out;
}

void LoadAppIcon(const std::wstring& path) {
    if (path == g_appIconPath) {
        return;
    }
    g_appIconPath = path;
    delete g_appIcon;
    g_appIcon = nullptr;
    SHFILEINFOW info{};
    if (!path.empty() && SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon) {
        g_appIcon = PixmapFromIcon(info.hIcon);
        DestroyIcon(info.hIcon);
    }
}

std::wstring AppInfo(HWND hwnd, std::wstring* outPath) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return L"";
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        *outPath = path;
        static std::unordered_map<std::wstring, std::wstring> names;  // Version info is read from disk: once per file.
        auto cached = names.find(path);
        if (cached != names.end()) {
            CloseHandle(proc);
            return cached->second;
        }
        DWORD handle = 0;
        DWORD len = GetFileVersionInfoSizeW(path, &handle);
        if (len) {
            std::vector<BYTE> data(len);
            struct Lang { WORD lang; WORD page; }* langs = nullptr;
            UINT langLen = 0;
            if (GetFileVersionInfoW(path, 0, len, data.data()) &&
                VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<void**>(&langs), &langLen) && langLen >= sizeof(Lang)) {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].page);
                wchar_t* value = nullptr;
                UINT valueLen = 0;
                if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &valueLen) && valueLen > 1) {
                    result = value;
                }
            }
        }
        if (result.empty()) {
            std::wstring p = path;
            size_t slash = p.find_last_of(L'\\');
            result = slash == std::wstring::npos ? p : p.substr(slash + 1);
            size_t dot = result.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                result.resize(dot);
            }
        }
        names[path] = result;
    }
    CloseHandle(proc);
    return result;
}

bool IsShellWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
           !wcscmp(cls, L"Windows.UI.Core.CoreWindow") || !wcscmp(cls, L"ControlCenterWindow") ||
           !wcscmp(cls, L"XamlExplorerHostIslandWindow") || !wcscmp(cls, kClassName) || !wcscmp(cls, kIslandClass);
}

// True when what the bar shows (name, title, icon) is different now.
bool ReadForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg || IsShellWindow(fg)) {
        return false;
    }
    const std::wstring oldApp = g_appName, oldTitle = g_title, oldPath = g_appIconPath;
    wchar_t title[256] = {};
    GetWindowTextW(fg, title, 256);
    std::wstring path;
    g_appName = AppInfo(fg, &path);
    g_title = g_appName == title ? L"" : title;
    LoadAppIcon(path);
    return g_appName != oldApp || g_title != oldTitle || g_appIconPath != oldPath;
}

// Finds the account picture. The image itself is decoded at the size the bar needs, later, by EnsureAvatar.
void LoadAvatar() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return;
    }
    BYTE buffer[256];
    DWORD size = sizeof(buffer);
    wchar_t* sid = nullptr;
    if (GetTokenInformation(token, TokenUser, buffer, size, &size)) {
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid);
    }
    CloseHandle(token);
    if (!sid) {
        return;
    }
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\";
    key += sid;
    LocalFree(sid);
    wchar_t path[MAX_PATH] = {};
    DWORD bytes = sizeof(path);
    // This process is 32-bit, and the value only exists in the 64-bit registry view.
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Image96", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path,
                     &bytes) == ERROR_SUCCESS) {
        g_avatarPath = path;
    }
}

// Scales the avatar to the pixel size it is drawn at, again whenever that changes (DPI).
void EnsureAvatar(int px) {
    if (g_avatarPath.empty() || px <= 0 || g_avatarPx == px) {
        return;
    }
    g_avatarPx = px;
    delete g_avatar;
    g_avatar = LoadPixmapFile(g_avatarPath.c_str(), px);
}

// Seelen's own thresholds for these icons.
IconId BatteryIcon() {
    if (g_battery > 90) return I_batfull;
    if (g_battery > 66) return I_bathigh;
    if (g_battery > 33) return I_batmed;
    if (g_battery > 5) return I_batlow;
    return I_batwarn;
}

#line 1 "bar/render.inc"

// ---- Drawing: layout builds a command list and the hit list, Direct2D draws it ----
// The bar is one full-width layered window with three bubbles on it. Everything between and around
// the bubbles stays fully transparent, so Windows lets clicks through to what is below.

Com<ID2D1Factory> g_factory;
LayeredSurface g_surface;
TextService g_text;
IconGeometries g_icons;
Com<ID2D1SolidColorBrush> g_brush;
unsigned g_brushGen = 0;
std::wstring g_fontKey;

// Dirty tracking. Callers say what changed; Paint also adds whatever bubble moved or resized.
RECT g_dirty{};
bool g_dirtyAny = false;
bool g_dirtyWhole = false;
bool g_paintPending = false;
constexpr int kBubbles = 3;
RECT g_bubbleRect[kBubbles]{};  // As last painted: left, (centre: the island's own window), right.
bool g_isDirty = false;  // The island needs drawing (see island.inc).
float IslandMaxExpandedW();  // DIP, island.inc

enum class CmdKind { Hover, Text, Icon, Avatar, Person, Pixmap, Tray };

struct Cmd {
    CmdKind kind = CmdKind::Text;
    D2D1_RECT_F r{};
    std::wstring text;
    IconId icon = I_wifi;
    float cx = 0, cy = 0, h = 0;  // Icon centre and height
    float alpha = 1.0f;
    float radius = 0;
    bool ellipsis = false;
    Pixmap* pix = nullptr;
};

std::vector<Cmd> g_cmds;
D2D1_RECT_F g_bubbles[kBubbles]{};  // 0 left, 1 unused, 2 right
bool g_hasBubble[kBubbles]{};
float g_bubbleRadius = 0;

bool EmptyRect(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

void UnionInto(RECT& a, const RECT& b) {
    if (EmptyRect(b)) return;
    if (EmptyRect(a)) { a = b; return; }
    a.left = std::min(a.left, b.left);
    a.top = std::min(a.top, b.top);
    a.right = std::max(a.right, b.right);
    a.bottom = std::max(a.bottom, b.bottom);
}

void RequestPaint() {
    if (g_hwnd && !g_paintPending) {
        g_paintPending = true;
        PostMessageW(g_hwnd, WM_APP_PAINT, 0, 0);
    }
}

void Invalidate(const RECT& r) {
    UnionInto(g_dirty, r);
    g_dirtyAny = true;
    RequestPaint();
}

void InvalidateAll() {
    g_dirtyWhole = true;
    g_dirtyAny = true;
    RequestPaint();
}

// Only what the new layout says has moved (the pill during its animation).
void InvalidateLayout() {
    g_dirtyAny = true;
    RequestPaint();
}

bool EnsureGfx() {
    if (g_factory) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_factory.Put()))) return false;
    if (!g_text.Init()) {
        g_factory.Reset();
        return false;
    }
    g_icons.Init(g_factory.Get());
    return true;
}

// Sets the font when the settings or the DPI changed it. Both windows draw with it.
void PrepareText() {
    const float s = Scale();
    wchar_t key[160];
    swprintf_s(key, L"%ls|%.3f", g_settings.fontFamily.c_str(), kFontSize * s * g_settings.fontScale / 100.0f);
    if (g_fontKey != key) {
        g_fontKey = key;
        g_text.SetFont(g_settings.fontFamily, kFontSize * s * g_settings.fontScale / 100.0f);
        g_dirtyWhole = g_dirtyAny = true;
        g_isDirty = true;
    }
}

void Layout(int W, int H) {
    const float s = Scale();
    g_cmds.clear();
    g_hits.clear();
    for (bool& b : g_hasBubble) b = false;

    const float bubbleH = roundf(kPillH * s);
    const float by = floorf((H - bubbleH) / 2.0f);
    const float itemH = bubbleH - 4.0f * s;
    const float itemY = by + 2.0f * s;
    const float cy = by + bubbleH / 2.0f;
    const float icon = roundf(kItem * s);
    const float pad = kItemPad * s;
    const float gap = kGap * s;
    const float margin = kBubbleMargin * s;
    const float bpad = kBubblePad * s;
    g_bubbleRadius = bubbleH / 2.0f;

    auto addHit = [&](float x, float y, float w, float h, float radius, Action action, int id, int data = 0) {
        g_hits.push_back({{(LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h)}, action, id, data});
        if (id == g_hoverId) {
            Cmd c;
            c.kind = CmdKind::Hover;
            c.r = D2D1::RectF(x, y, x + w, y + h);
            c.radius = radius;
            g_cmds.push_back(c);
        }
    };
    auto text = [&](const std::wstring& str, float x, float y, float w, float h, bool ellipsis = false) {
        Cmd c;
        c.kind = CmdKind::Text;
        c.r = D2D1::RectF(x, y, x + w, y + h);
        c.text = str;
        c.ellipsis = ellipsis;
        g_cmds.push_back(c);
    };
    auto iconCmd = [&](IconId id, float x, float y, float h, float alpha = 1.0f) {
        Cmd c;
        c.kind = CmdKind::Icon;
        c.icon = id;
        c.cx = x;
        c.cy = y;
        c.h = h;
        c.alpha = alpha;
        g_cmds.push_back(c);
    };
    auto image = [&](CmdKind kind, Pixmap* pix, float x, float y, float size, float radius = 0) {
        Cmd c;
        c.kind = kind;
        c.pix = pix;
        c.r = D2D1::RectF(x, y, x + size, y + size);
        c.radius = radius;
        g_cmds.push_back(c);
    };

    // Right bubble: tray icons, Bluetooth, Wi-Fi, battery, settings, laid out from the right edge.
    float right = W - margin - bpad;
    auto iconItem = [&](IconId id, Action action, int hitId) {
        const float w = IconGeometries::Width(id, icon) + 2 * pad;
        right -= w;
        addHit(right, itemY, w, itemH, kItemRadius * s, action, hitId);
        iconCmd(id, right + w / 2.0f, cy, icon);
        right -= gap;
    };
    iconItem(I_settings, Action::QuickSettings, H_settings);
    if (g_battery >= 0) {
        wchar_t pct[8];
        swprintf_s(pct, L"%d%%", g_battery);
        const std::wstring label = pct;
        const float space = g_text.Measure(L" ");
        const float labelW = g_text.Measure(label);
        const IconId battery = BatteryIcon();
        const float batteryW = IconGeometries::Width(battery, icon);
        const float boltW = g_charging ? IconGeometries::Width(I_bolt, icon) + space : 0.0f;
        const float w = boltW + batteryW + space + labelW + 2 * pad;
        right -= w;
        addHit(right, itemY, w, itemH, kItemRadius * s, Action::QuickSettings, H_battery);
        float x = right + pad;
        if (g_charging) {
            iconCmd(I_bolt, x + IconGeometries::Width(I_bolt, icon) / 2.0f, cy, icon);
            x += boltW;
        }
        iconCmd(battery, x + batteryW / 2.0f, cy, icon);
        x += batteryW + space;
        text(label, x, itemY, labelW + 2, itemH);
        right -= gap;
    }
    iconItem(g_connected ? I_wifi : I_offline, Action::Wifi, H_wifi);
    iconItem(I_bluetooth, Action::Bluetooth, H_bluetooth);
    for (int i = (int)g_tray.size() - 1; i >= 0; i--) {
        if (g_tray[i].hidden || !g_tray[i].pix) {
            continue;
        }
        const float w = icon + 2 * pad;
        right -= w;
        addHit(right, itemY, w, itemH, kItemRadius * s, Action::TrayIcon, H_trayBase + g_tray[i].uid, i);
        image(CmdKind::Tray, g_tray[i].pix, right + pad, cy - icon / 2.0f, icon);
        right -= gap;
    }
    g_bubbles[2] = D2D1::RectF(right + gap - bpad, by, W - margin, by + bubbleH);
    g_hasBubble[2] = true;

    // Left bubble: user, separator, app, separator, title. It gives way to the pill as it grows.
    float left = margin + bpad;
    const float slot = icon + 2 * pad;
    addHit(left, itemY, slot, itemH, kItemRadius * s, Action::Profile, H_profile);
    const float avatarY = cy - icon / 2.0f;
    if (g_avatar) {
        image(CmdKind::Avatar, g_avatar, left + pad, avatarY, icon, kItemRadius * s);
    } else {
        Cmd c;
        c.kind = CmdKind::Person;
        c.r = D2D1::RectF(left, itemY, left + slot, itemY + itemH);
        g_cmds.push_back(c);
    }
    float drawnRight = left + slot;
    left += slot + gap;

    // The left bubble stops short of the island at its widest, so nothing reflows when it opens.
    const float expHalf = IslandMaxExpandedW() * s / 2.0f + kNotchShoulder * s;
    const float limit = (W / 2.0f - expHalf) - kBubbleClear * s - bpad;
    auto textItem = [&](const std::wstring& str) {
        const float w = g_text.Measure(str);
        if (left + pad + w < limit) {
            text(str, left + pad, itemY, w + 2, itemH);
            drawnRight = left + pad + w;
        }
        left += w + 2 * pad + gap;
    };
    textItem(L"|");
    if (!g_appName.empty()) {
        if (g_appIcon && left + pad + icon < limit) {
            image(CmdKind::Pixmap, g_appIcon, left + pad, avatarY, icon);
            drawnRight = std::max(drawnRight, left + pad + icon);
        }
        const float nameW = g_text.Measure(g_appName);
        const float nameX = left + pad + icon + g_text.Measure(L"  ");
        if (nameX + nameW < limit) {
            text(g_appName, nameX, itemY, nameW + 2, itemH);
            drawnRight = nameX + nameW;
        }
        left = nameX + nameW + pad + gap;
    }
    if (!g_title.empty()) {
        textItem(L"-");
        const float titleX = left + pad;
        if (titleX < limit) {
            text(g_title, titleX, itemY, limit - titleX, itemH, true);
            drawnRight = titleX + std::min(g_text.Measure(g_title), limit - titleX);
        }
    }
    g_bubbles[0] = D2D1::RectF(margin, by, drawnRight + bpad, by + bubbleH);
    g_hasBubble[0] = true;
}

ID2D1Bitmap* BitmapOf(ID2D1RenderTarget* rt, Pixmap& p) {
    if (p.bmp && p.gen == g_surface.Generation()) return p.bmp.Get();
    p.bmp.Reset();
    D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(p.w, p.h), p.px.data(), p.w * 4, &props, p.bmp.Put()))) return nullptr;
    p.gen = g_surface.Generation();
    return p.bmp.Get();
}

void DrawFrame(ID2D1RenderTarget* rt) {
    ID2D1SolidColorBrush* brush = g_brush.Get();
    auto color = [&](D2D1_COLOR_F c, float alpha = 1.0f) {
        c.a *= alpha;
        brush->SetColor(c);
    };

    for (int i = 0; i < kBubbles; i++) {
        if (!g_hasBubble[i]) continue;
        color(kSideBg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(g_bubbles[i], g_bubbleRadius, g_bubbleRadius), brush);
    }

    for (const Cmd& c : g_cmds) {
        switch (c.kind) {
            case CmdKind::Hover:
                color(Overlay(51));
                rt->FillRoundedRectangle(D2D1::RoundedRect(c.r, c.radius, c.radius), brush);
                break;
            case CmdKind::Text: {
                color(kFg);
                TextOpts o;
                o.maxW = c.r.right - c.r.left;
                o.maxH = c.r.bottom - c.r.top;
                o.ellipsis = c.ellipsis;
                g_text.Draw(rt, c.text, c.r.left, c.r.top, brush, o);
                break;
            }
            case CmdKind::Icon:
                color(kFg, c.alpha);
                g_icons.Draw(rt, brush, c.icon, c.cx, c.cy, c.h);
                break;
            case CmdKind::Person: {
                color(kFg);
                TextOpts o;
                o.family = L"Segoe Fluent Icons";
                o.size = roundf(kItem * Scale());
                o.maxW = c.r.right - c.r.left;
                o.maxH = c.r.bottom - c.r.top;
                o.center = true;
                g_text.Draw(rt, std::wstring(1, (wchar_t)0xE77B), c.r.left, c.r.top, brush, o);
                break;
            }
            case CmdKind::Avatar: {
                // A bitmap brush fills the rounded square, which is cheaper than clipping to a geometry.
                ID2D1Bitmap* bmp = BitmapOf(rt, *c.pix);
                if (!bmp) break;
                Com<ID2D1BitmapBrush> bb;
                if (SUCCEEDED(rt->CreateBitmapBrush(bmp,
                        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                        bb.Put()))) {
                    bb->SetTransform(D2D1::Matrix3x2F::Translation(c.r.left, c.r.top));
                    rt->FillRoundedRectangle(D2D1::RoundedRect(c.r, c.radius, c.radius), bb.Get());
                }
                break;
            }
            case CmdKind::Pixmap:
            case CmdKind::Tray:
                if (ID2D1Bitmap* bmp = BitmapOf(rt, *c.pix)) {
                    rt->DrawBitmap(bmp, c.r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                break;
        }
    }
}

void Paint() {
    g_paintPending = false;
    if (!g_hwnd || g_fullscreen || g_hidden || !g_dirtyAny) {
        return;
    }
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    const int W = wr.right - wr.left;
    const int H = wr.bottom - wr.top;
    if (W <= 0 || H <= 0 || !EnsureGfx()) {
        return;
    }
    const unsigned before = g_surface.Generation();
    if (!g_surface.Resize(g_factory.Get(), W, H)) {
        return;
    }
    PrepareText();
    bool whole = g_dirtyWhole || g_surface.Generation() != before;

    const float s = Scale();
    EnsureAvatar((int)roundf(kItem * s));
    Layout(W, H);

    ID2D1DCRenderTarget* rt = g_surface.Target();
    if (!g_brush || g_brushGen != g_surface.Generation()) {
        g_brush.Reset();
        rt->CreateSolidColorBrush(kFg, g_brush.Put());
        g_brushGen = g_surface.Generation();
    }
    if (!g_brush) {
        return;
    }

    // Whatever moved or resized: old and new place both need drawing.
    RECT dirty = whole ? RECT{0, 0, W, H} : g_dirty;
    for (int i = 0; i < kBubbles; i++) {
        RECT now{};
        if (g_hasBubble[i]) {
            now = {(LONG)floorf(g_bubbles[i].left) - 1, (LONG)floorf(g_bubbles[i].top) - 1,
                   (LONG)ceilf(g_bubbles[i].right) + 1, (LONG)ceilf(g_bubbles[i].bottom) + 1};
        }
        if (!EqualRect(&now, &g_bubbleRect[i])) {
            UnionInto(dirty, now);
            UnionInto(dirty, g_bubbleRect[i]);
            g_bubbleRect[i] = now;
        }
    }
    const RECT bounds{0, 0, W, H};
    IntersectRect(&dirty, &dirty, &bounds);
    g_dirty = RECT{};
    g_dirtyAny = false;
    g_dirtyWhole = false;
    if (EmptyRect(dirty)) {
        return;
    }

    if (!g_surface.Begin(dirty)) {
        InvalidateAll();
        return;
    }
    DrawFrame(rt);
    if (!g_surface.End()) {
        g_brush.Reset();
        InvalidateAll();  // The target was lost: the next paint builds a new one.
        return;
    }
    g_surface.Present(g_hwnd, POINT{wr.left, wr.top}, dirty, EqualRect(&dirty, &bounds) != FALSE);
}

// What to release before the thread ends.
void ReleaseGfx() {
    g_brush.Reset();
    g_icons.Release();
    g_surface.Destroy();
    g_factory.Reset();
}

#line 1 "bar/module.inc"
// ---- Module interface: FROZEN at the end of P4. See specs/module-api.md. ----
// Feature modules (media, alerts, dashboards) implement IModule and are listed in g_modules.
// Everything here runs on the UI thread. A module's own workers hand results over with Services::Post.

struct Size2 { float w, h; };  // DIP

enum class View { Pill, Alert, Dashboard };

struct DrawCtx {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* brush;  // Shared. Set its colour before each use; multiply alpha by `alpha`.
    D2D1_RECT_F rect;             // Pixels. Exactly Measure(view) scaled by `scale`, in the island's stage.
    float alpha;                  // Content fade, 0..1.
    float scale;                  // DPI scale: DIP * scale = pixels.
    double now, dt;               // QPC seconds.
    View view;
};

struct PointerEvent {
    enum Kind { Move, Down, Up, Leave } kind;
    D2D1_POINT_2F pt;  // Pixels relative to rect.left, rect.top.
    bool left;
};

enum class PanelKind { Notifications, Calendar };

class Services;

class IModule {
public:
    virtual ~IModule() = default;
    virtual const wchar_t* Id() const = 0;
    virtual void Start(Services&) {}
    virtual void Stop() {}
    virtual void OnSettings(const Settings&) {}

    // Dashboards appear as pages in the expanded island, in DashboardOrder().
    virtual bool HasDashboard() const { return false; }
    virtual int DashboardOrder() const { return 100; }
    // Collapsed: the module with the highest PillPriority() above 0 shows; otherwise the clock.
    virtual int PillPriority() const { return 0; }

    virtual Size2 Measure(View) = 0;  // Called when state changes, not per frame.
    virtual void Draw(const DrawCtx&) = 0;
    virtual bool OnPointer(const PointerEvent&, View) { return false; }  // true: handled
    virtual bool OnWheel(int steps) { return false; }  // Dashboard only; steps > 0 is wheel up. true: consumed, the page does not change.
    virtual void OnClickAlert() {}                                        // The alert was tapped.

    virtual bool Animating(View) const { return false; }  // True keeps the frame clock running.
    virtual void OnVisibility(View, bool shown) {}        // Start or stop costly sampling.
};

class Services {
public:
    // Alerts queue by priority (higher first), then arrival. ms == 0: stays until ClearAlert.
    // An alert that arrives while the island is expanded waits; it does not close the island.
    void ShowAlert(IModule*, int priority, int ms);
    void ClearAlert(IModule*);

    void Invalidate();       // Redraw the island once.
    void Refresh();          // Measure() or PillPriority() changed: recompute the island's state.
    void WakeAnimation();    // Re-check Animating() and run the frame clock.
    void Post(std::function<void()>);  // From any thread to the UI thread.
    void SetTimer(IModule*, UINT ms, std::function<void()>);  // One timer per module; repeats until KillTimer.
    void KillTimer(IModule*);
    void Relayout();         // Re-measure the views after a module's size or presence changed.
    void Expand(IModule*, int ms);  // Open the island on that module's dashboard for at least ms (then hover rules).
    void OpenPanel(PanelKind, int anchorX);  // The native notification or calendar panel.
    const Settings& GetSettings() const;
    const wchar_t* Loc(const wchar_t* key) const;
    TextService& Text();
    // Reserved, added by the module that first needs them: Images() for decoded art, PersistInt.
};

// ---- Stub modules: what P4 ships so the shell can be seen. Later agents replace or add to these. ----

float ToDip(float px) { return px / Scale(); }

// The clock string from the settings: 12/24 hours, seconds, a custom date pattern, date above or beside.
std::wstring ClockText() {
    wchar_t timeText[64] = {};
    wchar_t dateText[128] = {};
    const Settings& st = g_settings;
    const wchar_t* pattern = nullptr;
    DWORD flags = st.clockSeconds ? 0 : TIME_NOSECONDS;
    if (st.clockHours == 24) { pattern = st.clockSeconds ? L"HH':'mm':'ss" : L"HH':'mm"; flags = 0; }
    else if (st.clockHours == 12) { pattern = st.clockSeconds ? L"h':'mm':'ss tt" : L"h':'mm tt"; flags = 0; }
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags, nullptr, pattern, timeText, 64)) wcscpy_s(timeText, L"--:--");
    bool ok = false;
    if (!st.dateFormat.empty()) ok = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, st.dateFormat.c_str(), dateText, 128, nullptr) > 0;
    if (!ok) GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"ddd d MMM", dateText, 128, nullptr);
    return std::wstring(dateText) + (st.dateAbove ? L"\n" : L", ") + timeText;
}

// Width of text with every digit set as the widest one, so a ticking clock never changes the notch's size.
float StableWidth(const std::wstring& text, float size) {
    wchar_t widest = L'0';
    float best = 0;
    for (wchar_t d = L'0'; d <= L'9'; d++) {
        const float w = g_text.Measure(std::wstring(1, d), size);
        if (w > best) { best = w; widest = d; }
    }
    std::wstring t = text;
    for (wchar_t& ch : t) if (ch >= L'0' && ch <= L'9') ch = widest;
    return g_text.Measure(t, size);
}

void DrawCentredText(const DrawCtx& c, const std::wstring& text, D2D1_RECT_F r, float size = 0) {
    D2D1_COLOR_F col = kFg;
    col.a *= c.alpha;
    c.brush->SetColor(col);
    TextOpts o;
    o.maxW = r.right - r.left;
    o.maxH = r.bottom - r.top;
    o.center = true;
    o.size = size;
    g_text.Draw(c.rt, text, r.left, r.top, c.brush, o);
}

void AlertsFullscreenChanged();  // topbar/alerts/alerts.inc
float AlertsDotsReserve();  // Extra pill width (DIP, both sides) while a privacy dot is shown.

void IslandOpenCalendar();  // island.inc: the native calendar, under the island.

// The collapsed content: date and time. A click opens the calendar.
class ClockModule : public IModule {
public:
    const wchar_t* Id() const override { return L"clock"; }
    bool OnPointer(const PointerEvent& e, View v) override {
        if (v != View::Pill) return false;
        if (e.kind == PointerEvent::Leave) down_ = false;
        if (!e.left) return false;
        if (e.kind == PointerEvent::Down) down_ = true;
        if (e.kind == PointerEvent::Up && down_) {
            down_ = false;
            IslandOpenCalendar();
        }
        return true;
    }
    Size2 Measure(View) override {
        const float size = g_settings.dateAbove ? g_text.DefaultSize() * 0.85f : 0.0f;  // Two lines need to be smaller.
        return {ToDip(StableWidth(ClockText(), size)) + 2 * kPillPadX + AlertsDotsReserve(), kNotchH};
    }
    void Draw(const DrawCtx& c) override {
        DrawCentredText(c, ClockText(), c.rect, g_settings.dateAbove ? g_text.DefaultSize() * 0.85f : 0.0f);
    }
private:
    bool down_ = false;
};

ClockModule g_clockModule;

#line 1 "media/engine.inc"
// ---- Media engine: SMTC on one MTA thread, event driven. See specs/media.md. ----
// Nothing here touches Direct2D. It publishes immutable snapshots; the UI thread (media.inc) draws them.

extern Services g_services;  // Defined in island.inc.
extern LayeredSurface g_isSurface;

namespace wm = winrt::Windows::Media::Control;
namespace ws = winrt::Windows::Storage::Streams;

struct MArt {
    int w = 0, h = 0;
    std::vector<DWORD> px;  // premultiplied BGRA
    float acc[3] = {0.298f, 0.788f, 0.941f};
    bool hasAccent = false;
    size_t hash = 0;
};

struct MSource {
    std::wstring aumid, name;
    bool playing = false;
    std::shared_ptr<const MArt> icon;
};

struct MSnap {
    bool has = false, playing = false;
    std::wstring title, artist, album, aumid, sourceName;
    std::shared_ptr<const MArt> art;
    std::vector<MSource> sources;
    int active = -1;
    long long posMs = 0, durMs = 0;
    ULONGLONG baseTick = 0;  // GetTickCount64 when posMs was true
    double rate = 1.0;
    bool canNext = true, canPrev = true, canSeek = false, canToggle = true;
    unsigned trackSeq = 0;  // Bumped for each new track that may open the panel.
};

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool HasWord(const std::wstring& lower, const wchar_t* w) { return lower.find(w) != std::wstring::npos; }

// Display name. Fix for the old substring bug: "edge" and "media" matched unrelated ids, so use whole tokens.
std::wstring FriendlySourceName(const std::wstring& aumid) {
    const std::wstring l = LowerW(aumid);
    struct { const wchar_t* key; const wchar_t* name; } table[] = {
        {L"spotify", L"Spotify"}, {L"msedge", L"Edge"}, {L"chrome", L"Chrome"}, {L"firefox", L"Firefox"},
        {L"brave", L"Brave"}, {L"opera", L"Opera"}, {L"vlc", L"VLC"}, {L"zunemusic", L"Media Player"},
        {L"zunevideo", L"Movies & TV"}, {L"applemusic", L"Apple Music"}, {L"itunes", L"iTunes"},
        {L"musicbee", L"MusicBee"}, {L"foobar", L"foobar2000"}, {L"tidal", L"TIDAL"}, {L"vivaldi", L"Vivaldi"}};
    for (auto& t : table) if (HasWord(l, t.key)) return t.name;
    std::wstring s = aumid;
    size_t p = s.find_last_of(L"!\\/");
    if (p != std::wstring::npos) s = s.substr(p + 1);
    if (s.size() > 4 && !_wcsicmp(s.c_str() + s.size() - 4, L".exe")) s.resize(s.size() - 4);
    p = s.find_last_of(L'.');
    if (p != std::wstring::npos && p + 1 < s.size()) s = s.substr(p + 1);
    if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
    return s.empty() ? aumid : s;
}

// The base name of the process that owns `hwnd`, lowercase, without .exe. Also fills the full path.
std::wstring ProcessOfWindow(HWND hwnd, std::wstring* path) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = (DWORD)std::size(buf);
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        if (path) *path = buf;
        std::wstring base = buf;
        const size_t p = base.find_last_of(L"\\/");
        if (p != std::wstring::npos) base = base.substr(p + 1);
        if (base.size() > 4 && !_wcsicmp(base.c_str() + base.size() - 4, L".exe")) base.resize(base.size() - 4);
        out = LowerW(base);
    }
    CloseHandle(h);
    return out;
}

bool ProcessMatchesAumid(const std::wstring& procBase, const std::wstring& aumidLower) {
    if (procBase.size() < 3) return false;
    if (HasWord(aumidLower, procBase.c_str())) return true;
    if (procBase == L"msedge" && HasWord(aumidLower, L"msedge")) return true;
    return false;
}

struct WinFind {
    const std::wstring* aumidLower;
    const std::wstring* title;
    HWND byTitle = nullptr, byProcess = nullptr;
    std::wstring exePath;
};

BOOL CALLBACK FindWinProc(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<WinFind*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (!wcscmp(cls, kClassName) || !wcscmp(cls, kIslandClass) || !wcscmp(cls, L"Shell_TrayWnd")) return TRUE;
    wchar_t cap[256] = {};
    GetWindowTextW(hwnd, cap, 256);
    if (!cap[0]) return TRUE;
    std::wstring path;
    const std::wstring proc = ProcessOfWindow(hwnd, &path);
    if (proc.empty() || !ProcessMatchesAumid(proc, *f->aumidLower)) return TRUE;
    if (f->exePath.empty()) f->exePath = path;
    if (!f->byProcess) f->byProcess = hwnd;
    // Bug 7 of the old mod: an empty title matched the first window. Only match a non-empty one.
    if (!f->title->empty() && !f->byTitle && LowerW(cap).find(LowerW(*f->title)) != std::wstring::npos) f->byTitle = hwnd;
    return TRUE;
}

// VLC with no title in SMTC: use its window caption, which ends in "VLC media player".
struct VlcCtx { std::wstring out; };
BOOL CALLBACK VlcEnumProc(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<VlcCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cap[256] = {};
    GetWindowTextW(hwnd, cap, 256);
    const std::wstring s = cap;
    const std::wstring tail = L" - VLC media player";
    if (s.size() > tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0 &&
        ProcessOfWindow(hwnd, nullptr) == L"vlc") {
        c->out = s.substr(0, s.size() - tail.size());
        return FALSE;
    }
    return TRUE;
}

std::wstring VlcCaption() {
    VlcCtx ctx;
    EnumWindows(VlcEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

// ---- Art decode and accent ----

void RgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2;
    const float d = mx - mn;
    h = 0; s = 0;
    if (d > 1e-5f) {
        s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
    }
}

float HueToRgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

void HslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s <= 1e-5f) { r = g = b = l; return; }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = HueToRgb(p, q, h + 1.0f / 3);
    g = HueToRgb(p, q, h);
    b = HueToRgb(p, q, h - 1.0f / 3);
}

float RelLum(float r, float g, float b) {
    auto f = [](float c) { return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * f(r) + 0.7152f * f(g) + 0.0722f * f(b);
}

// Spec section 9.2: vivid dominant colour, 16^3 buckets, at least 3:1 against the bar's surface.
void ComputeAccent(MArt& a) {
    struct B { float w = 0; int n = 0; double r = 0, g = 0, b = 0, sat = 0; };
    std::vector<B> bk(4096);
    for (DWORD px : a.px) {
        const int a8 = px >> 24;
        if (a8 < 32) continue;
        const int r = std::min(255, (int)(((px >> 16) & 255) * 255 / a8)), g = std::min(255, (int)(((px >> 8) & 255) * 255 / a8)),
                  b = std::min(255, (int)((px & 255) * 255 / a8));
        const int lum = (299 * r + 587 * g + 114 * b) / 1000;
        if (lum < 36 || lum > 220) continue;
        const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        const int sat = mx ? (mx - mn) * 255 / mx : 0;
        if (sat < 28) continue;
        B& x = bk[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
        x.w += 1.0f + sat / 32.0f;
        x.n++; x.r += r; x.g += g; x.b += b; x.sat += sat;
    }
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < 4096; i++) {
        if (!bk[i].n) continue;
        for (int k = 0; k < 3; k++) {
            if (top[k] < 0 || bk[i].w > bk[top[k]].w) {
                for (int m = 2; m > k; m--) top[m] = top[m - 1];
                top[k] = i;
                break;
            }
        }
    }
    int best = -1;
    for (int k = 0; k < 3; k++) {
        if (top[k] < 0) continue;
        if (best < 0 || bk[top[k]].sat / bk[top[k]].n > bk[best].sat / bk[best].n) best = top[k];
    }
    if (best < 0) return;  // Black and white art keeps the default.
    double sr = 0, sg = 0, sb = 0;
    long cnt = 0;
    const int br = best >> 8, bg = (best >> 4) & 15, bb = best & 15;
    for (int dr = -1; dr <= 1; dr++) for (int dg = -1; dg <= 1; dg++) for (int db = -1; db <= 1; db++) {
        const int r = br + dr, g = bg + dg, b = bb + db;
        if (r < 0 || g < 0 || b < 0 || r > 15 || g > 15 || b > 15) continue;
        const B& x = bk[(r << 8) | (g << 4) | b];
        sr += x.r; sg += x.g; sb += x.b; cnt += x.n;
    }
    if (!cnt) return;
    float r = (float)(sr / cnt / 255), g = (float)(sg / cnt / 255), b = (float)(sb / cnt / 255);
    float h, s, l;
    RgbToHsl(r, g, b, h, s, l);
    s = std::max(s, 0.35f);
    const float bgLum = RelLum(0x10 / 255.0f, 0x10 / 255.0f, 0x10 / 255.0f);
    for (int i = 0; i < 20; i++) {
        HslToRgb(h, s, l, r, g, b);
        if ((RelLum(r, g, b) + 0.05f) / (bgLum + 0.05f) >= 3.0f) break;
        l = std::min(1.0f, l + 0.04f);
    }
    a.acc[0] = r; a.acc[1] = g; a.acc[2] = b;
    a.hasAccent = true;
}

// Bytes -> 128x128 premultiplied BGRA (centre crop). Bug 10 of the old mod: big art was rejected; now it is scaled.
std::shared_ptr<MArt> DecodeArt(const std::vector<uint8_t>& bytes) {
    Com<IWICImagingFactory> wic;
    if (bytes.empty() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                 __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put())))) return nullptr;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> conv;
    Com<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put())) ||
        FAILED(dec->GetFrame(0, frame.Put())) || FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) return nullptr;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 16384 || h > 16384) return nullptr;
    constexpr UINT N = 128;
    const float k = (float)N / (float)std::min(w, h);
    const UINT sw = std::max<UINT>(N, (UINT)lroundf(w * k)), sh = std::max<UINT>(N, (UINT)lroundf(h * k));
    if (FAILED(wic->CreateBitmapScaler(scaler.Put())) ||
        FAILED(scaler->Initialize(conv.Get(), sw, sh, WICBitmapInterpolationModeFant))) return nullptr;
    auto art = std::make_shared<MArt>();
    art->w = art->h = N;
    art->px.resize((size_t)N * N);
    WICRect rc{(INT)((sw - N) / 2), (INT)((sh - N) / 2), (INT)N, (INT)N};
    if (FAILED(scaler->CopyPixels(&rc, N * 4, (UINT)(art->px.size() * 4), reinterpret_cast<BYTE*>(art->px.data())))) return nullptr;
    ComputeAccent(*art);
    return art;
}

// ---- The engine ----

enum class MCmdKind { Toggle, Next, Prev, Seek, Pick, Open };
struct MCmd { MCmdKind kind; long long arg = 0; std::wstring s; };

template <class Op>
bool AwaitOp(const Op& op, int ms = 2000) {
    try {
        if (op.wait_for(std::chrono::milliseconds(ms)) == winrt::Windows::Foundation::AsyncStatus::Completed) return true;
        op.Cancel();
    } catch (...) {}
    return false;
}

class MediaEngine {
public:
    void Start(std::function<void()> notify) {
        notify_ = std::move(notify);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        snap_ = std::make_shared<const MSnap>();
        alive_ = std::make_shared<std::atomic<bool>>(true);
        thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
    }
    void Stop() {
        if (alive_) *alive_ = false;
        if (stop_) SetEvent(stop_);
        if (thread_) {
            WaitForSingleObject(thread_, 2000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
        if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
    }
    std::shared_ptr<const MSnap> Snapshot() {
        std::lock_guard<std::mutex> g(mu_);
        return snap_;
    }
    void Command(MCmd c) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (c.kind == MCmdKind::Seek) {  // Only the newest seek matters.
                cmds_.erase(std::remove_if(cmds_.begin(), cmds_.end(), [](const MCmd& m) { return m.kind == MCmdKind::Seek; }), cmds_.end());
            }
            cmds_.push_back(std::move(c));
        }
        if (wake_) SetEvent(wake_);
    }
    // The panel is open: read the timeline every 1.5 s, like the old mod, so the jitter guard works unchanged.
    void SetPanelOpen(bool open) {
        if (panelOpen_.exchange(open) != open && wake_) SetEvent(wake_);
    }
    // The island shows media (pill or player). Only then does the 1-2 s polling fallback run.
    void SetShown(bool shown) {
        if (shown_.exchange(shown) != shown && wake_) SetEvent(wake_);
    }
    std::shared_ptr<std::atomic<bool>> Alive() const { return alive_; }

private:
    struct SessRec {
        wm::GlobalSystemMediaTransportControlsSession s{nullptr};
        std::wstring aumid;
        bool playing = false;
        wm::GlobalSystemMediaTransportControlsSession::PlaybackInfoChanged_revoker r1;
        wm::GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker r2;
    };
    struct SrcState {
        bool wasPlaying = false;
        unsigned startedSeq = 0;
        std::shared_ptr<const MArt> icon;
        ULONGLONG iconTry = 0;
    };

    static DWORD WINAPI ThreadMain(void* self) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        static_cast<MediaEngine*>(self)->Run();
        winrt::uninit_apartment();
        return 0;
    }

    void Run() {
        HANDLE h[2] = {stop_, wake_};
        DWORD timeout = 0;
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, h, FALSE, timeout);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1 && WaitForSingleObject(stop_, 100) == WAIT_OBJECT_0) break;  // Coalesce bursts.
            // A timeout wake is a fallback poll: players whose events are missing or late get re-read here.
            pollTick_ = w == WAIT_TIMEOUT;
            try {
                RunCommands();
                Refresh();
            } catch (...) {
                Wh_Log(L"media: refresh failed");
                forceRebuild_ = true;
            }
            pollTick_ = false;
            timeout = NextTimeout();
        }
        recs_.clear();
        mgrSess_.revoke();
        mgrCur_.revoke();
        mgr_ = nullptr;
    }

    // No sessions: only events and a slow health check. Playing: events plus a fallback, faster with the panel open.
    // Polling fallback: while the island shows media, re-read every 1.5-2 s (playing, or paused under 30 s ago),
    // then 15 s for a long pause. Hidden or idle keeps the slow health checks, so nothing playing costs nothing.
    DWORD NextTimeout() {
        const ULONGLONG now = GetTickCount64();
        DWORD t = 120000;
        if (!recs_.empty()) t = 30000;
        if (cur_.playing) t = panelOpen_ ? 1500 : 5000;
        if (shown_ && cur_.has) {
            if (cur_.playing || now - lastPlayingTick_ < 30000) t = std::min<DWORD>(t, panelOpen_ ? 1500 : 2000);
            else t = std::min<DWORD>(t, 15000);
        }
        if ((forceRebuild_ || !mgr_) && cur_.has) t = std::min<DWORD>(t, 2000);  // Recovering a dropped session.
        if (artRetryAt_) t = std::min<DWORD>(t, artRetryAt_ > now ? (DWORD)(artRetryAt_ - now) + 5 : 5);
        return t;
    }

    void DropManager() {
        recs_.clear();
        mgrSess_.revoke();
        mgrCur_.revoke();
        mgr_ = nullptr;
        sessionsDirty_ = true;
    }

    // Clears the shown media, keeping the auto-expand sequence.
    void ClearSnapshot(std::vector<MSource> sources) {
        const unsigned seq = cur_.trackSeq;
        cur_ = MSnap();
        cur_.trackSeq = seq;
        cur_.sources = std::move(sources);
        firstRead_ = false;
        lastKey_.clear();
        artKey_.clear();
        artRetryAt_ = 0;
        Publish(false);
    }

    // The manager can drop and recreate sessions without raising SessionsChanged. On a poll, compare the list.
    void VerifySessions() {
        auto list = mgr_.GetSessions();
        if (list.Size() != recs_.size()) { sessionsDirty_ = true; return; }
        for (uint32_t i = 0; i < list.Size(); i++) {
            if (recs_[i].aumid != list.GetAt(i).SourceAppUserModelId().c_str()) { sessionsDirty_ = true; return; }
        }
    }

    bool EnsureManager() {
        if (mgr_) return true;
        try {
            auto op = wm::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            if (!AwaitOp(op, 3000)) return false;
            mgr_ = op.GetResults();
            mgrSess_ = mgr_.SessionsChanged(winrt::auto_revoke, [this](auto&&, auto&&) { sessionsDirty_ = true; SetEvent(wake_); });
            mgrCur_ = mgr_.CurrentSessionChanged(winrt::auto_revoke, [this](auto&&, auto&&) { sessionsDirty_ = true; SetEvent(wake_); });
            sessionsDirty_ = true;
            return true;
        } catch (...) {
            mgr_ = nullptr;  // Bug 4: a dead broker left the manager stale forever. Retry on the next health tick.
            return false;
        }
    }

    wm::GlobalSystemMediaTransportControlsSession* Chosen() {
        for (SessRec& r : recs_) if (r.aumid == chosenAumid_ && r.playing) return &r.s;
        for (SessRec& r : recs_) if (r.aumid == chosenAumid_) return &r.s;  // Bug 12: prefer the playing duplicate.
        return nullptr;
    }

    void RunCommands() {
        std::deque<MCmd> cmds;
        {
            std::lock_guard<std::mutex> g(mu_);
            cmds.swap(cmds_);
        }
        for (MCmd& c : cmds) {
            try {
                if (c.kind == MCmdKind::Pick) {
                    dockPick_ = c.s;
                    userSwitchUntil_ = GetTickCount64() + 4000;
                    forceRead_ = true;
                    continue;
                }
                if (c.kind == MCmdKind::Open) { OpenPlayer(); continue; }
                auto* s = Chosen();
                if (!s) continue;  // No media key fallback: it can double-skip.
                if (c.kind == MCmdKind::Toggle) AwaitOp(s->TryTogglePlayPauseAsync());
                else if (c.kind == MCmdKind::Next) AwaitOp(s->TrySkipNextAsync());
                else if (c.kind == MCmdKind::Prev) AwaitOp(s->TrySkipPreviousAsync());
                else if (c.kind == MCmdKind::Seek && cur_.durMs > 0) {
                    const long long target = std::max(0LL, std::min(c.arg, cur_.durMs));
                    AwaitOp(s->TryChangePlaybackPositionAsync((curStartMs_ + target) * 10000));
                    cur_.posMs = target;
                    cur_.baseTick = GetTickCount64();
                    ignoreTimelineUntil_ = cur_.baseTick + 1500;
                    backPending_ = false;
                    Publish(true);
                }
            } catch (...) {}
        }
        if (!cmds.empty()) forceRead_ = true;
    }

    void OpenPlayer() {
        const std::wstring al = LowerW(chosenAumid_);
        WinFind f{&al, &cur_.title};
        EnumWindows(FindWinProc, reinterpret_cast<LPARAM>(&f));
        HWND hw = f.byTitle ? f.byTitle : f.byProcess;
        if (hw) {
            if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE);
            SetForegroundWindow(hw);
        } else if (chosenAumid_.find(L'!') != std::wstring::npos) {
            ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + chosenAumid_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void RebuildSessions() {
        recs_.clear();
        sessionsDirty_ = false;
        auto list = mgr_.GetSessions();
        for (uint32_t i = 0; i < list.Size(); i++) {
            auto s = list.GetAt(i);
            recs_.emplace_back();
            SessRec& r = recs_.back();
            r.s = s;
            r.aumid = s.SourceAppUserModelId().c_str();
            r.r1 = s.PlaybackInfoChanged(winrt::auto_revoke, [this](auto&&, auto&&) { SetEvent(wake_); });
            r.r2 = s.MediaPropertiesChanged(winrt::auto_revoke, [this](auto&&, auto&&) { propsDirty_ = true; SetEvent(wake_); });
        }
    }

    // Spec section 6.2, kept: dock pick, then auto (playing beats paused, newest start wins).
    void ChooseSource(const std::wstring& systemCurrent) {
        std::wstring want;
        unsigned best = 0;
        bool newStart = false;
        for (auto& o : order_) if (srcs_[o].startedSeq > startedBefore_) newStart = true;
        if (newStart) dockPick_.clear();
        bool pickAlive = false;
        for (auto& o : order_) if (o == dockPick_) pickAlive = true;
        if (!pickAlive) dockPick_.clear();
        if (!dockPick_.empty()) { chosenAumid_ = dockPick_; return; }
        for (auto& o : order_) {
            bool playing = false;
            for (SessRec& r : recs_) if (r.aumid == o && r.playing) playing = true;
            const unsigned score = srcs_[o].startedSeq + (playing ? 1000000u : 0u);
            if (srcs_[o].startedSeq == 0 && !playing) continue;
            if (!want.size() || score > best) { want = o; best = score; }
        }
        if (want.empty()) want = !systemCurrent.empty() ? systemCurrent : (order_.empty() ? L"" : order_[0]);
        chosenAumid_ = want;
    }

    std::shared_ptr<const MArt> SourceIcon(const std::wstring& aumid, SrcState& st) {
        if (st.icon) return st.icon;
        const ULONGLONG now = GetTickCount64();
        if (st.iconTry && now - st.iconTry < 15000) return nullptr;  // Bug 6: throttle the window scan.
        st.iconTry = now;
        const std::wstring al = LowerW(aumid);
        const std::wstring none;
        WinFind f{&al, &none};
        EnumWindows(FindWinProc, reinterpret_cast<LPARAM>(&f));
        if (f.exePath.empty()) return nullptr;
        SHFILEINFOW info{};
        if (!SHGetFileInfoW(f.exePath.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon) return nullptr;
        std::unique_ptr<Pixmap> pm(PixmapFromIcon(info.hIcon));
        DestroyIcon(info.hIcon);
        if (!pm) return nullptr;
        auto art = std::make_shared<MArt>();
        art->w = pm->w; art->h = pm->h; art->px = std::move(pm->px);
        st.icon = art;
        return art;
    }

    bool ReadArt(const wm::GlobalSystemMediaTransportControlsSessionMediaProperties& props, std::shared_ptr<const MArt>& out, size_t& hash) {
        auto ref = props.Thumbnail();
        if (!ref) return false;
        auto op = ref.OpenReadAsync();
        if (!AwaitOp(op)) return false;
        auto stream = op.GetResults();
        const uint64_t size = stream.Size();
        if (!size || size > (24u << 20)) return false;
        ws::DataReader reader(stream);
        auto load = reader.LoadAsync((uint32_t)size);
        if (!AwaitOp(load)) return false;
        std::vector<uint8_t> bytes((size_t)size);
        reader.ReadBytes(bytes);
        const size_t hsh = std::hash<std::string_view>()(std::string_view((const char*)bytes.data(), bytes.size()));
        if (out && hash == hsh) return true;  // Same bytes: keep the decoded art.
        auto art = DecodeArt(bytes);
        if (!art) return false;
        art->hash = hsh;
        hash = hsh;
        out = art;
        return true;
    }

    void Refresh() {
        const ULONGLONG now = GetTickCount64();
        if (!EnsureManager()) {
            // The broker stays away: after a few tries, stop showing media that can no longer be confirmed.
            if (cur_.has && ++mgrFails_ >= 3) ClearSnapshot({});
            return;
        }
        mgrFails_ = 0;
        std::wstring systemCurrent;
        try {
            if (pollTick_ && !sessionsDirty_ && !forceRebuild_ && !recs_.empty()) VerifySessions();
            if (sessionsDirty_ || recs_.empty() || forceRebuild_) { RebuildSessions(); forceRebuild_ = false; }
            if (auto c = mgr_.GetCurrentSession()) systemCurrent = c.SourceAppUserModelId().c_str();
        } catch (...) {
            DropManager();
            return;  // Keep the last snapshot; the next tick asks for a new manager.
        }
        // Playback state of every session, so a start in another player is noticed.
        for (SessRec& r : recs_) {
            try {
                r.playing = r.s.GetPlaybackInfo().PlaybackStatus() ==
                            wm::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            } catch (...) {
                r.playing = false;
                forceRebuild_ = true;  // A dead session object: rebuild the list on the next wake.
            }
        }
        // Merge by AUMID, stable order, history forgotten when the source goes away.
        std::vector<std::wstring> present;
        for (SessRec& r : recs_) if (std::find(present.begin(), present.end(), r.aumid) == present.end()) present.push_back(r.aumid);
        order_.erase(std::remove_if(order_.begin(), order_.end(), [&](const std::wstring& o) {
            if (std::find(present.begin(), present.end(), o) != present.end()) return false;
            srcs_.erase(o);
            return true;
        }), order_.end());
        for (auto& p : present) if (std::find(order_.begin(), order_.end(), p) == order_.end()) { order_.push_back(p); srcs_[p]; }
        startedBefore_ = startCounter_;
        for (int pass = 0; pass < 2; pass++) {  // The system's current session counts as the newest of several.
            for (auto& o : order_) {
                if ((o == systemCurrent) != (pass == 1)) continue;
                bool playing = false;
                for (SessRec& r : recs_) if (r.aumid == o && r.playing) playing = true;
                SrcState& st = srcs_[o];
                if (playing && !st.wasPlaying) st.startedSeq = ++startCounter_;
                st.wasPlaying = playing;
            }
        }
        const std::wstring prevAumid = chosenAumid_;
        ChooseSource(systemCurrent);

        MSnap next = cur_;
        next.sources.clear();
        for (auto& o : order_) {
            MSource ms;
            ms.aumid = o;
            ms.name = FriendlySourceName(o);
            for (SessRec& r : recs_) if (r.aumid == o && r.playing) ms.playing = true;
            ms.icon = SourceIcon(o, srcs_[o]);
            next.sources.push_back(ms);
        }
        next.active = -1;
        for (size_t i = 0; i < order_.size(); i++) if (order_[i] == chosenAumid_) next.active = (int)i;

        auto* sess = Chosen();
        if (!sess) {  // Session loss: only now drop the snapshot (bug 3).
            if (recs_.empty() || !chosenAumid_.empty()) ClearSnapshot(next.sources);
            return;
        }
        bool readOk = true;
        try {
            next.has = true;
            next.aumid = chosenAumid_;
            next.sourceName = FriendlySourceName(chosenAumid_);
            auto info = sess->GetPlaybackInfo();
            next.playing = info.PlaybackStatus() == wm::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            auto ctl = info.Controls();
            next.canNext = ctl.IsNextEnabled();
            next.canPrev = ctl.IsPreviousEnabled();
            next.canSeek = ctl.IsPlaybackPositionEnabled();
            next.canToggle = ctl.IsPlayPauseToggleEnabled() || ctl.IsPlayEnabled() || ctl.IsPauseEnabled();
            double rate = 1.0;
            if (auto r = info.PlaybackRate()) rate = r.Value();
            next.rate = rate > 0.05 && rate < 8 ? rate : 1.0;

            const bool srcChanged = chosenAumid_ != prevAumid;
            // pollTick_: players that never raise MediaPropertiesChanged still get their new track read.
            if (propsDirty_ || forceRead_ || pollTick_ || srcChanged || lastKey_.empty() || cur_.title.empty() || artRetryAt_ ||
                next.playing != cur_.playing) {
                propsDirty_ = false;
                auto pop = sess->TryGetMediaPropertiesAsync();
                if (!AwaitOp(pop)) throw std::runtime_error("props");
                auto props = pop.GetResults();
                next.title = props.Title().c_str();
                next.artist = props.Artist().c_str();
                next.album = props.AlbumTitle().c_str();
                if (next.title.empty() && LowerW(chosenAumid_).find(L"vlc") != std::wstring::npos) next.title = VlcCaption();
                const std::wstring key = chosenAumid_ + L"\n" + next.title + L"\n" + next.artist + L"\n" + next.album;
                const bool newTrack = key != lastKey_;
                if (newTrack) {
                    next.art = nullptr;  // Never keep old art across a real track change.
                    artHash_ = 0;
                    artRetryAt_ = now + 1500;
                    artTries_ = 0;
                }
                // Art: replaced only when the bytes differ (bug 5). Retry at +1.5 s and +3 s for slow players.
                if (newTrack || (artRetryAt_ && now >= artRetryAt_)) {
                    std::shared_ptr<const MArt> a = next.art;
                    size_t hsh = artHash_;
                    bool got = false;
                    try { got = ReadArt(props, a, hsh); } catch (...) {}
                    if (got) { next.art = a; artHash_ = hsh; }
                    if (artTries_ < 2) { artRetryAt_ = now + 1500; artTries_++; } else artRetryAt_ = 0;
                }
                // Auto-expand trigger. Skipped on the first read, after a user switch, and for resume-from-pause (bug 2).
                const std::wstring tkey = next.title + L"\n" + next.artist;
                if (newTrack && !firstRead_ && now >= userSwitchUntil_ && !next.title.empty() && next.playing &&
                    tkey != lastTrackKey_) {
                    next.trackSeq++;
                }
                lastTrackKey_ = tkey;
                lastKey_ = key;
                forceRead_ = false;
            } else if (artRetryAt_ && now >= artRetryAt_) {
                artRetryAt_ = 0;
            }
            // Timeline.
            if (now >= ignoreTimelineUntil_) {
                auto tl = sess->GetTimelineProperties();
                using namespace std::chrono;
                curStartMs_ = duration_cast<milliseconds>(tl.StartTime()).count();
                const long long endMs = duration_cast<milliseconds>(tl.EndTime()).count();
                next.durMs = std::max(0LL, endMs - curStartMs_);  // Bug 13: duration is end - start.
                const long long rawPos = duration_cast<milliseconds>(tl.Position()).count();
                const long long updated = tl.LastUpdatedTime().time_since_epoch().count();
                long long pos = rawPos - curStartMs_;
                const long long age = duration_cast<milliseconds>(winrt::clock::now() - tl.LastUpdatedTime()).count();
                if (next.playing && age >= 0 && age < 10000) pos += (long long)(age * next.rate);
                pos = std::max(0LL, std::min(pos, next.durMs > 0 ? next.durMs : pos));
                // Spotify refreshes LastUpdatedTime without moving Position: hold a backward jump until confirmed.
                const long long expected = cur_.posMs + (cur_.playing ? (long long)((now - cur_.baseTick) * cur_.rate) : 0);
                const bool sameTrack = next.title == cur_.title && next.aumid == cur_.aumid;
                // Some players publish the timeline once and never again: neither Position nor LastUpdatedTime moves.
                // Re-basing on that stale reading would snap the thumb back each poll, so keep extrapolating locally.
                const bool frozen = sameTrack && next.playing && cur_.playing && cur_.baseTick &&
                                    rawPos == lastRawPos_ && updated == lastUpdated_;
                lastRawPos_ = rawPos;
                lastUpdated_ = updated;
                if (frozen) {
                    next.posMs = cur_.posMs;
                    next.baseTick = cur_.baseTick;
                } else if (sameTrack && next.playing && cur_.playing && pos < expected - 1500 && !backPending_) {
                    backPending_ = true;  // keep the old base
                    next.posMs = cur_.posMs;
                    next.baseTick = cur_.baseTick;
                } else {
                    backPending_ = false;
                    next.posMs = pos;
                    next.baseTick = now;
                }
            }
        } catch (...) {
            readOk = false;  // Bug 3: keep the last snapshot instead of publishing an empty one.
        }
        if (!readOk) {
            // The session may have been dropped and recreated under the same id: rebuild, and after
            // several failures in a row ask for a fresh manager.
            forceRebuild_ = true;
            if (++readFails_ >= 3) { readFails_ = 0; DropManager(); }
            cur_.sources = next.sources;
            cur_.active = next.active;
            Publish(false);
            return;
        }
        readFails_ = 0;
        if (next.playing) lastPlayingTick_ = now;
        firstRead_ = false;
        cur_ = std::move(next);
        Publish(false);
    }

    // Posts to the UI only when something visible changed, or when the panel is open (it shows the position).
    void Publish(bool force) {
        auto snap = std::make_shared<MSnap>(cur_);
        bool visible = force;
        {
            std::lock_guard<std::mutex> g(mu_);
            const MSnap& o = *snap_;
            visible = visible || o.has != snap->has || o.playing != snap->playing || o.title != snap->title ||
                      o.artist != snap->artist || o.album != snap->album || o.art != snap->art || o.aumid != snap->aumid ||
                      o.sources.size() != snap->sources.size() || o.active != snap->active || o.trackSeq != snap->trackSeq ||
                      o.durMs != snap->durMs || o.canNext != snap->canNext || o.canPrev != snap->canPrev ||
                      o.canSeek != snap->canSeek || o.rate != snap->rate;
            if (!visible) {
                for (size_t i = 0; i < o.sources.size(); i++)
                    if (o.sources[i].playing != snap->sources[i].playing || o.sources[i].icon != snap->sources[i].icon) visible = true;
            }
            snap_ = snap;
        }
        if (visible || panelOpen_) {
            auto alive = alive_;
            auto fn = notify_;
            g_services.Post([alive, fn] { if (*alive) fn(); });
        }
    }

    std::function<void()> notify_;
    HANDLE stop_ = nullptr, wake_ = nullptr, thread_ = nullptr;
    std::mutex mu_;
    std::shared_ptr<const MSnap> snap_;
    std::deque<MCmd> cmds_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::atomic<bool> panelOpen_{false}, shown_{false};
    std::atomic<bool> sessionsDirty_{true}, propsDirty_{false};

    // Media thread only.
    wm::GlobalSystemMediaTransportControlsSessionManager mgr_{nullptr};
    wm::GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker mgrSess_;
    wm::GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker mgrCur_;
    std::vector<SessRec> recs_;
    std::unordered_map<std::wstring, SrcState> srcs_;
    std::vector<std::wstring> order_;
    std::wstring chosenAumid_, dockPick_, lastKey_, artKey_, lastTrackKey_;
    unsigned startCounter_ = 0, startedBefore_ = 0;
    MSnap cur_;
    bool firstRead_ = true, forceRead_ = false, forceRebuild_ = false, backPending_ = false, pollTick_ = false;
    size_t artHash_ = 0;
    int artTries_ = 0, readFails_ = 0, mgrFails_ = 0;
    ULONGLONG artRetryAt_ = 0, userSwitchUntil_ = 0, ignoreTimelineUntil_ = 0, lastPlayingTick_ = 0;
    long long curStartMs_ = 0, lastRawPos_ = -1, lastUpdated_ = 0;
};

#line 1 "media/media.inc"
// ---- Media module: the collapsed pill (art, clock, bars) and the player page. UI thread only. ----

// IAudioMeterInformation is only forward-declared by this compiler's headers, so it is declared here.
struct IMeterInfo : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* n) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 n, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};
const GUID kIidMeterInfo = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

// The pill keeps the clock exactly centred: both slots are the same width.
constexpr float kMediaSlot = 26.0f;
constexpr float kPlayerW = 380.0f, kPlayerH = 182.0f;
constexpr int kBarsHist = 7;

// Digits differ in width; measuring with the widest one stops the pill from shifting each minute.
std::wstring WidestDigitForm(const std::wstring& s) {
    wchar_t best = L'0';
    float bw = 0;
    for (wchar_t d = L'0'; d <= L'9'; d++) {
        const float w = g_text.Measure(std::wstring(1, d));
        if (w > bw) { bw = w; best = d; }
    }
    std::wstring o = s;
    for (wchar_t& c : o) if (c >= L'0' && c <= L'9') c = best;
    return o;
}

// Comma separated, case-insensitive substring match against name, app id and title.
bool MediaBlocked(const std::wstring& list, const MSnap& s) {
    if (list.empty()) return false;
    const std::wstring hay = LowerW(s.sourceName + L"\n" + s.aumid + L"\n" + s.title);
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(L',', i);
        if (j == std::wstring::npos) j = list.size();
        std::wstring e = LowerW(list.substr(i, j - i));
        const size_t a = e.find_first_not_of(L" \t"), b = e.find_last_not_of(L" \t");
        if (a != std::wstring::npos) {
            e = e.substr(a, b - a + 1);
            if (hay.find(e) != std::wstring::npos) return true;
        }
        i = j + 1;
    }
    return false;
}

std::wstring FormatMs(long long ms) {
    if (ms < 0) ms = 0;
    const long long t = ms / 1000;
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf_s(b, L"%lld:%02lld", t / 60, t % 60);
    return b;
}

class MediaModule : public IModule {
public:
    const wchar_t* Id() const override { return L"media"; }

    void Start(Services& s) override {
        services_ = &s;
        OnSettings(s.GetSettings());
        engine_.Start([this] { OnSnapshot(); });
    }
    void Stop() override {
        services_->KillTimer(this);
        engine_.Stop();
        meter_.Reset();
    }
    void OnSettings(const Settings& s) override {
        autoExpand_ = s.mediaAutoExpand;
        blocklist_ = s.mediaBlocklist;
    }

    int PillPriority() const override { return snap_ && snap_->has ? 10 : 0; }
    bool HasDashboard() const override { return snap_ && snap_->has; }
    int DashboardOrder() const override { return 5; }

    Size2 Measure(View v) override {
        if (v == View::Dashboard) return {kPlayerW, kPlayerH};
        const float clockW = ToDip(g_text.Measure(WidestDigitForm(ClockText())));
        return {clockW + 2 * kPillPadX + 2 * kMediaSlot, kNotchH};
    }

    bool Animating(View v) const override {
        return v == View::Dashboard && snap_ && (snap_->playing || morph_ != (snap_->playing ? 1.0f : 0.0f));
    }

    void OnVisibility(View v, bool shown) override {
        if (v == View::Pill) visPill_ = shown;
        else {
            visDash_ = shown;
            engine_.SetPanelOpen(shown);
            if (shown) { meter_.Reset(); lastSample_ = 0; dragging_ = false; pressed_ = Z_none; }
        }
        engine_.SetShown(visPill_ || visDash_);
        UpdateTimer();
    }

    void Draw(const DrawCtx& c) override {
        if (!snap_ || !snap_->has) return;
        if (c.view == View::Dashboard) DrawPlayer(c);
        else DrawPill(c);
    }

    bool OnPointer(const PointerEvent& e, View v) override;

private:
    enum Zone { Z_none, Z_prev, Z_play, Z_next, Z_scrub, Z_open, Z_dock0 = 10 };

    // ---- Snapshot handling ----
    void OnSnapshot() {
        auto old = snap_;
        snap_ = engine_.Snapshot();
        const bool presence = !old || old->has != snap_->has;
        const bool dockChange = !old || old->sources.size() != snap_->sources.size();
        if (snap_->has && snap_->trackSeq != lastSeq_) {
            const bool first = lastSeq_ == ~0u;
            lastSeq_ = snap_->trackSeq;
            if (!first && autoExpand_ && !g_fullscreen && !MediaBlocked(blocklist_, *snap_)) services_->Expand(this, 5000);
        }
        lastSeq_ = snap_->trackSeq;
        UpdateTimer();
        if (presence || dockChange || !old || old->title != snap_->title || old->durMs != snap_->durMs) services_->Relayout();
        services_->Invalidate();
        services_->WakeAnimation();
    }

    // Collapsed: bars move at about 12 fps, only while the pill shows and music plays. Otherwise no timer.
    void UpdateTimer() {
        const bool want = visPill_ && snap_ && snap_->has && snap_->playing;
        if (want == timerOn_) return;
        timerOn_ = want;
        if (want) {
            meter_.Reset();
            services_->SetTimer(this, 80, [this] { Sample(); services_->Invalidate(); });
        } else {
            services_->KillTimer(this);
            services_->Invalidate();
        }
    }

    // ---- Audio peak ----
    bool EnsureMeter() {
        if (meter_) return true;
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(en.Put()))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.Put()))) return false;
        void* p = nullptr;
        if (FAILED(dev->Activate(kIidMeterInfo, CLSCTX_ALL, nullptr, &p)) || !p) return false;
        *meter_.Put() = static_cast<IMeterInfo*>(p);
        return true;
    }

    void Sample() {
        float peak = 0;
        if (EnsureMeter()) {
            if (FAILED(meter_->GetPeakValue(&peak))) { meter_.Reset(); peak = 0; }  // Default device changed: re-resolve.
        }
        float v = std::min(1.0f, sqrtf(peak) * 1.15f);
        if (!snap_ || !snap_->playing) v = 0;
        const float prev = hist_[(histPos_ + kBarsHist - 1) % kBarsHist];
        v = v > prev ? prev * 0.3f + v * 0.7f : prev * 0.15f + v * 0.85f;  // Attack 0.7, release 0.85, as the old one.
        hist_[histPos_] = v;
        histPos_ = (histPos_ + 1) % kBarsHist;
    }

    // ---- Images ----
    struct Img {
        std::shared_ptr<const MArt> src;
        Com<ID2D1Bitmap> bmp;
        unsigned gen = 0;
    };
    ID2D1Bitmap* BitmapFor(ID2D1RenderTarget* rt, Img& img, const std::shared_ptr<const MArt>& a) {
        if (!a) return nullptr;
        if (img.src != a || !img.bmp || img.gen != g_isSurface.Generation()) {
            img.bmp.Reset();
            D2D1_BITMAP_PROPERTIES p = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(rt->CreateBitmap(D2D1::SizeU(a->w, a->h), a->px.data(), a->w * 4, &p, img.bmp.Put()))) return nullptr;
            img.src = a;
            img.gen = g_isSurface.Generation();
        }
        return img.bmp.Get();
    }

    static void FillBitmapRounded(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp, D2D1_RECT_F r, float radius, float alpha) {
        Com<ID2D1BitmapBrush> bb;
        if (!bmp || FAILED(rt->CreateBitmapBrush(bmp, bb.Put()))) return;
        const D2D1_SIZE_F sz = bmp->GetSize();
        bb->SetOpacity(alpha);
        bb->SetTransform(D2D1::Matrix3x2F::Scale((r.right - r.left) / sz.width, (r.bottom - r.top) / sz.height) *
                         D2D1::Matrix3x2F::Translation(r.left, r.top));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), bb.Get());
    }

    D2D1_COLOR_F Accent(float a = 1.0f) const {
        float r = 0.298f, g = 0.788f, b = 0.941f;
        if (snap_ && snap_->art && snap_->art->hasAccent) { r = snap_->art->acc[0]; g = snap_->art->acc[1]; b = snap_->art->acc[2]; }
        return D2D1::ColorF(r, g, b, a);
    }

    static void SetCol(const DrawCtx& c, D2D1_COLOR_F col, float a = 1.0f) {
        col.a *= a * c.alpha;
        c.brush->SetColor(col);
    }

    void DrawArt(const DrawCtx& c, D2D1_RECT_F r, float radius) {
        const float s = c.scale;
        if (snap_->art) {
            if (ID2D1Bitmap* b = BitmapFor(c.rt, art_, snap_->art)) { FillBitmapRounded(c.rt, b, r, radius * s, c.alpha); return; }
        }
        SetCol(c, Accent(), 0.24f);
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius * s, radius * s), c.brush);
        const std::wstring letter = snap_->sourceName.empty() ? L"" : snap_->sourceName.substr(0, 1);
        SetCol(c, kFg, 0.9f);
        TextOpts o;
        o.center = true;
        o.maxW = r.right - r.left;
        o.maxH = r.bottom - r.top;
        g_text.Draw(c.rt, letter, r.left, r.top, c.brush, o);
    }

    // Bars from the history ring, oldest on the left. Paused: a row of dots.
    void DrawBars(const DrawCtx& c, float x, float y, float w, float h, int n) {
        const float s = c.scale;
        const float gap = 1.6f * s;
        const float bw = (w - (n - 1) * gap) / n;
        for (int i = 0; i < n; i++) {
            if (!snap_->playing) {
                SetCol(c, kFg, 0.5f);
                c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + i * (bw + gap) + bw / 2, y + h / 2), bw / 2, bw / 2), c.brush);
                continue;
            }
            const float amp = hist_[(histPos_ + kBarsHist - n + i + kBarsHist) % kBarsHist];
            const float bh = std::max(3.0f * s, amp * 0.86f * h);
            SetCol(c, Accent(), 0.45f + 0.5f * amp);
            const float bx = x + i * (bw + gap);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, y + (h - bh) / 2, bx + bw, y + (h + bh) / 2), bw / 2, bw / 2), c.brush);
        }
    }

    void DrawPill(const DrawCtx& c) {
        const float s = c.scale, cy = (c.rect.top + c.rect.bottom) / 2;
        const float slot = kMediaSlot * s;
        const float art = 18.0f * s;
        DrawArt(c, D2D1::RectF(c.rect.left + (slot - art) / 2 + 3 * s, cy - art / 2, c.rect.left + (slot + art) / 2 + 3 * s, cy + art / 2), 4.0f);
        DrawCentredText(c, ClockText(), D2D1::RectF(c.rect.left + slot, c.rect.top, c.rect.right - slot, c.rect.bottom));
        const float bw = 18.0f * s, bh = 14.0f * s;
        DrawBars(c, c.rect.right - slot + (slot - bw) / 2 - 3 * s, cy - bh / 2, bw, bh, 5);
    }

    // ---- Player page ----
    struct Geo { float x, y, w, h; };
    static D2D1_RECT_F R(const DrawCtx& c, float x, float y, float w, float h) {
        const float s = c.scale;
        return D2D1::RectF(c.rect.left + x * s, c.rect.top + y * s, c.rect.left + (x + w) * s, c.rect.top + (y + h) * s);
    }

    long long CurPos() const {
        const ULONGLONG now = GetTickCount64();
        if (dragging_ && snap_->durMs > 0) return (long long)(dragFrac_ * snap_->durMs);
        if (now < ovTick_ + 1500) return std::min<long long>(ovPos_ + (snap_->playing ? (long long)((now - ovTick_) * snap_->rate) : 0), snap_->durMs);
        long long p = snap_->posMs + (snap_->playing ? (long long)((now - snap_->baseTick) * snap_->rate) : 0);
        return std::max(0LL, snap_->durMs > 0 ? std::min(p, snap_->durMs) : p);
    }

    void Poly(const DrawCtx& c, const D2D1_POINT_2F* p, int n) {
        Com<ID2D1PathGeometry> g;
        Com<ID2D1GeometrySink> sk;
        if (FAILED(g_factory->CreatePathGeometry(g.Put())) || FAILED(g->Open(sk.Put()))) return;
        sk->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < n; i++) sk->AddLine(p[i]);
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        sk->Close();
        c.rt->FillGeometry(g.Get(), c.brush);
    }

    // The play/pause morph: two quads, each blending a half of the play triangle into a pause bar.
    void DrawPlayPause(const DrawCtx& c, float cx, float cy, float size, float t) {
        const float L = cx - size / 2, T = cy - size / 2;
        auto lerp = [](float a, float b, float k) { return a + (b - a) * k; };
        const float pl[2][4][2] = {{{0.15f, 0}, {0.55f, 0.25f}, {0.55f, 0.75f}, {0.15f, 1}},
                                   {{0.55f, 0.25f}, {1.0f, 0.5f}, {1.0f, 0.5f}, {0.55f, 0.75f}}};
        const float pa[2][4][2] = {{{0.12f, 0}, {0.40f, 0}, {0.40f, 1}, {0.12f, 1}},
                                   {{0.60f, 0}, {0.88f, 0}, {0.88f, 1}, {0.60f, 1}}};
        for (int q = 0; q < 2; q++) {
            D2D1_POINT_2F pts[4];
            for (int i = 0; i < 4; i++) pts[i] = D2D1::Point2F(L + lerp(pl[q][i][0], pa[q][i][0], t) * size, T + lerp(pl[q][i][1], pa[q][i][1], t) * size);
            Poly(c, pts, 4);
        }
    }

    void DrawSkip(const DrawCtx& c, float cx, float cy, float size, bool next) {
        const float d = next ? 1.0f : -1.0f;
        const float h = size / 2;
        D2D1_POINT_2F a[3] = {{cx - d * h * 0.9f, cy - h}, {cx - d * h * 0.9f, cy + h}, {cx + d * h * 0.5f, cy}};
        Poly(c, a, 3);
        c.rt->FillRectangle(D2D1::RectF(std::min(cx + d * h * 0.62f, cx + d * h * 0.98f), cy - h, std::max(cx + d * h * 0.62f, cx + d * h * 0.98f), cy + h), c.brush);
    }

    void DrawPlayer(const DrawCtx& c) {
        const MSnap& m = *snap_;
        const float s = c.scale;
        morph_ += (((m.playing ? 1.0f : 0.0f) - morph_) * std::min(1.0f, (float)c.dt * 14.0f));
        if (fabsf(morph_ - (m.playing ? 1.0f : 0.0f)) < 0.01f) morph_ = m.playing ? 1.0f : 0.0f;
        if (m.playing && c.now - lastSample_ >= 0.033) { Sample(); lastSample_ = c.now; }

        // Album wash: a soft radial of the accent from the top middle.
        {
            const float w = c.rect.right - c.rect.left, h = c.rect.bottom - c.rect.top;
            D2D1_GRADIENT_STOP st[2] = {{0.0f, Accent(0.115f * c.alpha)}, {1.0f, Accent(0.0f)}};
            Com<ID2D1GradientStopCollection> col;
            Com<ID2D1RadialGradientBrush> rb;
            if (SUCCEEDED(c.rt->CreateGradientStopCollection(st, 2, col.Put())) &&
                SUCCEEDED(c.rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(c.rect.left + w / 2, c.rect.top), D2D1::Point2F(0, 0), w * 0.75f, h * 1.15f), col.Get(), rb.Put()))) {
                c.rt->FillRectangle(c.rect, rb.Get());
            }
        }
        // Art with a source badge, text column, bars.
        const auto artR = R(c, 24, 18, 64, 64);
        DrawArt(c, artR, 16.0f);
        if (m.active >= 0 && (size_t)m.active < m.sources.size() && m.sources[m.active].icon) {
            const auto bad = D2D1::RectF(artR.right - 24 * s, artR.bottom - 22 * s, artR.right - 3 * s, artR.bottom - 1 * s);
            SetCol(c, Rgb(0x10, 0x10, 0x10), 1.0f);
            c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((bad.left + bad.right) / 2, (bad.top + bad.bottom) / 2), 11 * s, 11 * s), c.brush);
            if (ID2D1Bitmap* b = BitmapFor(c.rt, srcImg_[m.sources[m.active].aumid], m.sources[m.active].icon))
                c.rt->DrawBitmap(b, D2D1::RectF(bad.left + 2.5f * s, bad.top + 2.5f * s, bad.right - 2.5f * s, bad.bottom - 2.5f * s), c.alpha);
        }
        const float textSize = g_text.DefaultSize();
        auto text = [&](const std::wstring& t, float y, float h, float size, float a) {
            SetCol(c, kFg, a);
            TextOpts o;
            o.maxW = 202 * s;
            o.maxH = h * s;
            o.ellipsis = true;
            o.size = size;
            const auto r = R(c, 106, y, 202, h);
            g_text.Draw(c.rt, t, r.left, r.top, c.brush, o);
        };
        text(m.title.empty() ? Loc(L"media.unknown") : m.title, 18, 22, textSize * 1.15f, 1.0f);
        text(m.artist, 40, 18, textSize, 0.8f);
        float srcY = 62;
        if (!m.album.empty()) { text(m.album, 58, 16, textSize * 0.92f, 0.7f); srcY = 76; }
        {
            wchar_t line[160];
            swprintf_s(line, Loc(m.playing ? L"media.playingfrom" : L"media.pausedin"), m.sourceName.c_str());
            text(line, srcY, 14, textSize * 0.85f, 0.55f);
        }
        DrawBars(c, R(c, 324, 24, 32, 20).left, R(c, 324, 24, 32, 20).top, 32 * s, 20 * s, 7);

        // Scrubber and time labels; hidden for live streams (duration 0).
        if (m.durMs > 0) {
            const long long pos = CurPos();
            const float frac = (float)pos / (float)m.durMs;
            const auto tr = R(c, 72, 111.5f, 236, 5);
            const float cx = tr.left + frac * (tr.right - tr.left), half = (dragging_ ? 3.5f : 2.5f) * s;
            SetCol(c, kFg, m.canSeek ? 0.18f : 0.1f);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, (tr.top + tr.bottom) / 2 - half, tr.right, (tr.top + tr.bottom) / 2 + half), half, half), c.brush);
            SetCol(c, Accent(), m.canSeek ? 1.0f : 0.5f);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, (tr.top + tr.bottom) / 2 - half, cx, (tr.top + tr.bottom) / 2 + half), half, half), c.brush);
            if (m.canSeek) {
                const float rad = (dragging_ ? 6.5f : 4.5f) * s, my = (tr.top + tr.bottom) / 2;
                if (dragging_) { SetCol(c, Accent(), 0.22f); c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, my), rad * 2.2f, rad * 2.2f), c.brush); }
                SetCol(c, Accent());
                c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, my), rad + 1.6f * s, rad + 1.6f * s), c.brush);
                SetCol(c, Rgb(255, 255, 255));
                c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, my), rad, rad), c.brush);
            }
            auto label = [&](const std::wstring& t, float x, float w, bool right) {
                SetCol(c, kFg, 0.6f);
                TextOpts o;
                o.maxW = w * s;
                o.maxH = 16 * s;
                o.size = textSize * 0.85f;
                const auto r = R(c, x, 106, w, 16);
                const float tw = g_text.Measure(t) * (o.size / textSize);
                g_text.Draw(c.rt, t, right ? r.right - tw : r.left, r.top, c.brush, o);
            };
            label(FormatMs(pos), 24, 40, true);
            label(L"-" + FormatMs(m.durMs - pos), 316, 40, false);
        }

        // Transport.
        const float cxm = c.rect.left + (kPlayerW / 2) * s, cyb = c.rect.top + 140 * s;
        struct Btn { Zone z; float dx, r; bool enabled; };
        const Btn btns[3] = {{Z_prev, -64, 16, m.canPrev}, {Z_play, 0, 22, m.canToggle}, {Z_next, 64, 16, m.canNext}};
        for (const Btn& b : btns) {
            const bool hot = hover_ == b.z, down = pressed_ == b.z;
            const float rad = b.r * s * (down ? 0.87f : 1.0f);
            const bool big = b.z == Z_play;
            if (down) SetCol(c, Accent(), 0.5f);
            else SetCol(c, Rgb(255, 255, 255), (big ? (hot ? 0.16f : 0.08f) : (hot ? 0.09f : 0.04f)) * (b.enabled ? 1.0f : 0.5f));
            c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cxm + b.dx * s, cyb), rad, rad), c.brush);
            SetCol(c, kFg, (big ? (hot ? 1.0f : 0.88f) : (hot ? 0.92f : 0.62f)) * (b.enabled ? 1.0f : 0.35f));
            if (big) DrawPlayPause(c, cxm, cyb, 18 * s * (down ? 0.87f : 1.0f), morph_);
            else DrawSkip(c, cxm + b.dx * s, cyb, 12 * s * (down ? 0.87f : 1.0f), b.z == Z_next);
        }

        // Source dock, only with two or more sources.
        const std::vector<int> dock = DockSlots();
        if (dock.size() >= 2 || (dock.size() == 1 && m.sources.size() >= 2)) {
            const auto pill = R(c, 20, 125, 2 + (float)dock.size() * 28, 30);
            SetCol(c, Rgb(255, 255, 255), 0.06f);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 15 * s, 15 * s), c.brush);
            for (size_t i = 0; i < dock.size(); i++) {
                const int si = dock[i];
                const auto sr = R(c, 22 + (float)i * 28, 127, 26, 26);
                const float scx = (sr.left + sr.right) / 2, scy = (sr.top + sr.bottom) / 2;
                const bool active = si == m.active, hot = hover_ == Z_dock0 + (int)i;
                if (si < 0) {  // "+N"
                    SetCol(c, kFg, hot ? 1.0f : 0.6f);
                    wchar_t t[16];
                    swprintf_s(t, L"+%d", (int)m.sources.size() - 2);
                    TextOpts o; o.center = true; o.maxW = sr.right - sr.left; o.maxH = sr.bottom - sr.top; o.size = textSize * 0.9f;
                    g_text.Draw(c.rt, t, sr.left, sr.top, c.brush, o);
                    continue;
                }
                if (active) {
                    SetCol(c, Accent());
                    c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(scx, scy), 12.25f * s, 12.25f * s), c.brush, 1.5f * s);
                }
                const MSource& src = m.sources[si];
                const float ia = active || hot ? 1.0f : 0.5f;
                if (ID2D1Bitmap* b = src.icon ? BitmapFor(c.rt, srcImg_[src.aumid], src.icon) : nullptr) {
                    c.rt->DrawBitmap(b, D2D1::RectF(scx - 8 * s, scy - 8 * s, scx + 8 * s, scy + 8 * s), ia * c.alpha);
                } else {
                    SetCol(c, kFg, ia);
                    TextOpts o; o.center = true; o.maxW = sr.right - sr.left; o.maxH = sr.bottom - sr.top; o.size = textSize;
                    g_text.Draw(c.rt, src.name.substr(0, 1), sr.left, sr.top, c.brush, o);
                }
                if (src.playing) {
                    SetCol(c, Rgb(0x10, 0x10, 0x10));
                    c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sr.right - 4 * s, sr.bottom - 4 * s), 4.5f * s, 4.5f * s), c.brush);
                    SetCol(c, Rgb(0x3a, 0xd0, 0x6c));
                    c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sr.right - 4 * s, sr.bottom - 4 * s), 3.5f * s, 3.5f * s), c.brush);
                }
            }
        }
    }

    // Up to 3 sources: one slot each. More: first, active, "+N" (index -1).
    std::vector<int> DockSlots() const {
        std::vector<int> out;
        const int n = (int)snap_->sources.size();
        if (n < 2) return out;
        if (n <= 3) { for (int i = 0; i < n; i++) out.push_back(i); return out; }
        out.push_back(0);
        out.push_back(snap_->active > 0 ? snap_->active : 1);
        out.push_back(-1);
        return out;
    }

    Zone HitTest(D2D1_POINT_2F p) const {  // p in DIP
        auto in = [&](float x, float y, float w, float h) { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; };
        const std::vector<int> dock = DockSlots();
        for (size_t i = 0; i < dock.size(); i++) if (in(22 + (float)i * 28, 127, 26, 26)) return (Zone)(Z_dock0 + (int)i);
        auto circ = [&](float cx, float r) { const float dx = p.x - cx, dy = p.y - 140; return dx * dx + dy * dy <= (r + 6) * (r + 6); };
        if (circ(190, 22)) return Z_play;
        if (circ(126, 16)) return Z_prev;
        if (circ(254, 16)) return Z_next;
        if (snap_->durMs > 0 && in(66, 104, 248, 22)) return Z_scrub;
        if (in(24, 18, 64, 64) || in(106, 18, 202, 62)) return Z_open;
        return Z_none;
    }

    Services* services_ = nullptr;
    MediaEngine engine_;
    std::shared_ptr<const MSnap> snap_;
    unsigned lastSeq_ = ~0u;
    bool autoExpand_ = true, visPill_ = false, visDash_ = false, timerOn_ = false;
    std::wstring blocklist_;
    Img art_;
    std::unordered_map<std::wstring, Img> srcImg_;
    Com<IMeterInfo> meter_;
    float hist_[kBarsHist] = {};
    int histPos_ = 0;
    double lastSample_ = 0;
    float morph_ = 0;
    Zone hover_ = Z_none, pressed_ = Z_none;
    bool dragging_ = false;
    float dragFrac_ = 0;
    long long ovPos_ = 0;
    ULONGLONG ovTick_ = 0;
    bool wasLeft_ = false;
};

bool MediaModule::OnPointer(const PointerEvent& e, View v) {
    if (v != View::Dashboard || !snap_ || !snap_->has) return false;
    const float s = Scale();
    const D2D1_POINT_2F p = D2D1::Point2F(e.pt.x / s, e.pt.y / s);
    auto fracAt = [&](float x) { return std::max(0.0f, std::min(1.0f, (x - 72.0f) / 236.0f)); };
    auto finishScrub = [&] {
        if (!dragging_) return;
        dragging_ = false;
        const long long target = (long long)(dragFrac_ * snap_->durMs);
        ovPos_ = target;
        ovTick_ = GetTickCount64();
        engine_.Command({MCmdKind::Seek, target});
    };
    switch (e.kind) {
        case PointerEvent::Move: {
            if (dragging_) { dragFrac_ = fracAt(p.x); services_->Invalidate(); return true; }
            const Zone z = HitTest(p);
            if (z != hover_) { hover_ = z; services_->Invalidate(); }
            return true;
        }
        case PointerEvent::Leave:
            finishScrub();
            hover_ = pressed_ = Z_none;
            services_->Invalidate();
            return true;
        case PointerEvent::Down: {
            const Zone z = HitTest(p);
            pressed_ = z;
            if (z == Z_scrub && snap_->canSeek) { dragging_ = true; dragFrac_ = fracAt(p.x); }
            services_->Invalidate();
            return true;
        }
        case PointerEvent::Up: {
            const Zone z = HitTest(p);
            const Zone was = pressed_;
            pressed_ = Z_none;
            if (dragging_) { dragFrac_ = fracAt(p.x); finishScrub(); services_->Invalidate(); return true; }
            if (z == was) {
                if (z == Z_prev) engine_.Command({MCmdKind::Prev});
                else if (z == Z_next) engine_.Command({MCmdKind::Next});
                else if (z == Z_play) engine_.Command({MCmdKind::Toggle});
                else if (z == Z_open) engine_.Command({MCmdKind::Open});
                else if (z >= Z_dock0) {
                    const std::vector<int> dock = DockSlots();
                    const size_t i = z - Z_dock0;
                    if (i < dock.size()) {
                        int target = dock[i];
                        if (target < 0) target = (snap_->active + 1) % (int)snap_->sources.size();  // "+N" cycles.
                        if (target != snap_->active) engine_.Command({MCmdKind::Pick, 0, snap_->sources[target].aumid});
                    }
                }
            }
            services_->Invalidate();
            return true;
        }
    }
    return false;
}

MediaModule g_mediaModule;

#line 1 "bar/dash.inc"
// ---- Dashboards (P6c): shared drawing helpers, page base, idle, calendar, hardware, weather, timer ----
// Pages are modules with HasDashboard(). The island owns switching, the dot column and the slide;
// a page does work only between OnVisibility(Dashboard, true) and (…, false).

// Fixed look (specs/dashboards.md section 2).
const D2D1_COLOR_F kCard = Rgb(255, 255, 255, 22);
const D2D1_COLOR_F kCardHover = Rgb(255, 255, 255, 38);
const D2D1_COLOR_F kHairline = Rgb(255, 255, 255, 26);
const D2D1_COLOR_F kMuted = Rgb(228, 228, 228, 153);
const D2D1_COLOR_F kAccent = Rgb(228, 228, 228);
const D2D1_COLOR_F kAmber = Rgb(255, 176, 33);
const D2D1_COLOR_F kRed = Rgb(255, 89, 82);
const D2D1_COLOR_F kInk = Rgb(16, 16, 16);

constexpr float kPageW = 380.0f, kPageH = 184.0f;
enum TextFlag { TF_Center = 1, TF_Right = 2, TF_Ellipsis = 4, TF_Wrap = 8 };

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string out(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], (int)out.size(), nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], (int)out.size());
    return out;
}

// Where the bar keeps content that must survive a reformat: D: when it is a fixed disk, else the user profile.
// Never the Windhawk mod storage, which every process can write.
const std::wstring& DataRoot() {
    static std::wstring root;
    if (root.empty()) {
        auto usable = [](const std::wstring& p) {
            SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
            const DWORD a = GetFileAttributesW(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        };
        if (GetDriveTypeW(L"D:\\") == DRIVE_FIXED && usable(L"D:\\windhawk-data\\top-bar")) {
            root = L"D:\\windhawk-data\\top-bar";
        } else {
            wchar_t buf[MAX_PATH] = {};
            GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
            root = std::wstring(buf) + L"\\top-bar";
            usable(root);
        }
    }
    return root;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &n, nullptr) && n == data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool ReadWholeFile(const std::wstring& path, std::string& out, DWORD cap = 16u << 20) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart <= cap;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD n = 0;
        ok = ReadFile(f, out.empty() ? nullptr : &out[0], (DWORD)out.size(), &n, nullptr) && n == out.size();
    }
    CloseHandle(f);
    return ok;
}

ULONGLONG WallSeconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL;
}

// Drawing in DIP relative to the page's top left; everything scales and fades with the DrawCtx.
struct Pg {
    const DrawCtx& c;
    float s;
    explicit Pg(const DrawCtx& ctx) : c(ctx), s(ctx.scale) {}
    D2D1_RECT_F R(float l, float t, float r, float b) const {
        return D2D1::RectF(c.rect.left + l * s, c.rect.top + t * s, c.rect.left + r * s, c.rect.top + b * s);
    }
    float X(float x) const { return c.rect.left + x * s; }
    float Y(float y) const { return c.rect.top + y * s; }
    void Col(D2D1_COLOR_F k, float a = 1.0f) const {
        k.a *= c.alpha * a;
        c.brush->SetColor(k);
    }
    void Fill(float l, float t, float r, float b, float rad, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(R(l, t, r, b), rad * s, rad * s), c.brush);
    }
    void Outline(float l, float t, float r, float b, float rad, float w, D2D1_COLOR_F k, float a = 1.0f,
                 ID2D1StrokeStyle* style = nullptr) const {
        Col(k, a);
        c.rt->DrawRoundedRectangle(D2D1::RoundedRect(R(l, t, r, b), rad * s, rad * s), c.brush, w * s, style);
    }
    void Dot(float cx, float cy, float r, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(X(cx), Y(cy)), r * s, r * s), c.brush);
    }
    void Ring(float cx, float cy, float r, float w, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(X(cx), Y(cy)), r * s, r * s), c.brush, w * s);
    }
    void Line(float x1, float y1, float x2, float y2, float w, D2D1_COLOR_F k, float a = 1.0f) const {
        Col(k, a);
        c.rt->DrawLine(D2D1::Point2F(X(x1), Y(y1)), D2D1::Point2F(X(x2), Y(y2)), c.brush, w * s);
    }
    void Text(const std::wstring& t, float l, float tp, float r, float b, float size, D2D1_COLOR_F k, int flags = 0,
              int weight = 400) const {
        if (t.empty()) return;
        Col(k);
        TextOpts o;
        o.size = size * s;
        o.maxW = (r - l) * s;
        o.maxH = (b - tp) * s;
        o.center = flags & TF_Center;
        o.right = flags & TF_Right;
        o.ellipsis = flags & TF_Ellipsis;
        o.wrap = flags & TF_Wrap;
        o.weight = weight;
        g_text.Draw(c.rt, t, X(l), Y(tp), c.brush, o);
    }
};

// Width in DIP of a text at a size and weight.
float TextWidth(const std::wstring& t, float size, int weight = 400) {
    TextOpts o;
    o.size = size * Scale();
    o.weight = weight;
    Com<IDWriteTextLayout> layout = g_text.Layout(t, o);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace / Scale();
}

// Arc from 12 o'clock, clockwise, `frac` of a turn. Round caps.
void DrawArcFrac(const Pg& p, float cx, float cy, float r, float frac, float w, D2D1_COLOR_F k) {
    if (frac <= 0.002f) return;
    static Com<ID2D1StrokeStyle> round;
    if (!round) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        g_factory->CreateStrokeStyle(sp, nullptr, 0, round.Put());
    }
    p.Col(k);
    if (frac >= 0.999f) {
        p.c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(p.X(cx), p.Y(cy)), r * p.s, r * p.s), p.c.brush, w * p.s);
        return;
    }
    const float a = frac * 6.2831853f;
    Com<ID2D1PathGeometry> geo;
    Com<ID2D1GeometrySink> sink;
    if (FAILED(g_factory->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
    const float R = r * p.s;
    sink->BeginFigure(D2D1::Point2F(p.X(cx), p.Y(cy) - R), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(p.X(cx) + R * sinf(a), p.Y(cy) - R * cosf(a)), D2D1::SizeF(R, R), 0,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    p.c.rt->DrawGeometry(geo.Get(), p.c.brush, w * p.s, round.Get());
}

// ---- Page base: pointer hit rects in DIP, rebuilt by each Draw ----
struct HitRect {
    float l, t, r, b;
    int id;
};

class PageBase : public IModule {
public:
    bool HasDashboard() const override { return true; }
    Size2 Measure(View) override { return {kPageW, kPageH}; }
    void Start(Services& s) override { sv_ = &s; }
    void Stop() override { if (sv_) sv_->KillTimer(this); }
    void OnVisibility(View v, bool shown) override {
        if (v != View::Dashboard || shown == shown_) return;
        shown_ = shown;
        hover_ = down_ = 0;
        if (shown) Shown(); else Hidden();
    }
    bool OnPointer(const PointerEvent& e, View) override {
        const float x = e.pt.x / Scale(), y = e.pt.y / Scale();
        int id = 0;
        if (e.kind != PointerEvent::Leave) {
            for (const HitRect& h : hits_) if (x >= h.l && x < h.r && y >= h.t && y < h.b) id = h.id;
        }
        if (e.kind == PointerEvent::Down) down_ = id;
        bool changed = id != hover_;
        hover_ = id;
        if (e.kind == PointerEvent::Up) {
            const int was = down_;
            down_ = 0;
            if (was && was == id) { Click(id); changed = true; }
        }
        if (e.kind == PointerEvent::Leave) down_ = 0;
        if (changed && sv_) sv_->Invalidate();
        return id != 0;
    }
protected:
    virtual void Shown() {}
    virtual void Hidden() {}
    virtual void Click(int) {}
    void Hit(float l, float t, float r, float b, int id) { hits_.push_back({l, t, r, b, id}); }
    Services* sv_ = nullptr;
    bool shown_ = false;
    int hover_ = 0, down_ = 0;
    std::vector<HitRect> hits_;
};

// ---- Idle ----
class IdlePage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-idle"; }
    int DashboardOrder() const override { return 10; }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        hits_.clear();
        Hit(0, 0, kPageW, kPageH, 1);  // The whole page: a click opens the calendar.
        wchar_t tm[40] = {}, dt[64] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, tm, 40);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"dddd, MMMM d", dt, 64, nullptr);
        p.Text(tm, 0, 44, 360, 112, 56, kFg, TF_Center, 700);
        p.Text(dt, 0, 114, 360, 138, 16, kMuted, TF_Center, 600);
        if (g_battery >= 0) {
            wchar_t line[64];
            if (!g_charging && life_ != 0xFFFFFFFFu && life_ > 0) {
                swprintf_s(line, L"%d%%   %uh %02um left", g_battery, life_ / 3600, (life_ / 60) % 60);
            } else {
                swprintf_s(line, L"%d%%", g_battery);
            }
            const float tw = TextWidth(line, 11);
            const float x0 = 180.0f - (tw + 22.0f) / 2.0f;
            const bool low = !g_charging && g_battery <= 20;
            IconId icon = g_charging ? I_bolt : g_battery >= 90 ? I_batfull : g_battery >= 60 ? I_bathigh : g_battery >= 35 ? I_batmed : g_battery >= 15 ? I_batlow : I_batwarn;
            p.Col(low ? kRed : kFg);
            g_icons.Draw(c.rt, c.brush, icon, p.X(x0 + 7), p.Y(160), 14.0f * c.scale);
            p.Text(line, x0 + 22, 150, x0 + 22 + tw + 4, 170, 11, low ? kRed : kMuted);
        }
    }
protected:
    void Click(int) override { IslandOpenCalendar(); }
    void Shown() override {
        SYSTEM_POWER_STATUS st{};
        life_ = GetSystemPowerStatus(&st) ? st.BatteryLifeTime : 0xFFFFFFFFu;
    }
private:
    DWORD life_ = 0xFFFFFFFFu;
};

// ---- Calendar (read-only; a click on the clock or the idle page opens the native calendar) ----
int DaysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

class CalendarPage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-calendar"; }
    int DashboardOrder() const override { return 20; }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t month[48] = {}, wday[48] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"MMMM", month, 48, nullptr);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"dddd", wday, 48, nullptr);
        DWORD first = 0;  // 0 = Monday
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, (LPWSTR)&first, 2);
        first %= 7;
        SYSTEMTIME f = now;
        f.wDay = 1;
        FILETIME ft;
        SystemTimeToFileTime(&f, &ft);
        FileTimeToSystemTime(&ft, &f);
        const int firstMon = (f.wDayOfWeek + 6) % 7;
        const int startIdx = (firstMon - (int)first + 7) % 7;
        const int dim = DaysInMonth(now.wYear, now.wMonth);
        const int rows = (startIdx + dim + 6) / 7;

        p.Fill(22, 16, 115, 164, 12, kCard);
        p.Text(month, 28, 22, 112, 38, 12, kAccent, TF_Ellipsis, 700);
        wchar_t year[8];
        swprintf_s(year, L"%d", now.wYear);
        p.Text(year, 28, 37, 112, 52, 11, kMuted);
        wchar_t day[8];
        swprintf_s(day, L"%d", now.wDay);
        p.Text(day, 22, 50, 115, 138, 63, kFg, TF_Center, 700);
        p.Text(wday, 26, 140, 112, 158, 12, kMuted, TF_Center | TF_Ellipsis, 700);

        const float gx = 134, gy = 16, col = 28, headH = 18;
        for (int i = 0; i < 7; i++) {
            const int mon = ((int)first + i) % 7;
            wchar_t nm[8] = {};
            std::wstring ch = L"?";
            if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, 0x31 + mon, nm, 8) && nm[0]) ch.assign(nm, 1);
            p.Text(ch, gx + i * col, gy, gx + (i + 1) * col, gy + headH, 11, kMuted, TF_Center, 600);
        }
        p.Line(gx, gy + headH + 3, gx + 7 * col, gy + headH + 3, 1, kHairline);
        const float top = gy + headH + 7, rowH = (kPageH - 14 - top) / rows;
        const float rad = std::min(col, rowH) / 2.0f - 1.5f;
        for (int d = 1; d <= dim; d++) {
            const int idx = startIdx + d - 1, r = idx / 7, cc = idx % 7;
            const float cx = gx + cc * col + col / 2, cy = top + r * rowH + rowH / 2;
            const bool today = d == now.wDay;
            const int mon = ((int)first + cc) % 7;
            wchar_t n[4];
            swprintf_s(n, L"%d", d);
            if (today) p.Dot(cx, cy, rad, kAccent);
            D2D1_COLOR_F dc = today ? kInk : kFg;
            if (!today && mon >= 5) dc.a *= 0.62f;  // weekends recede
            p.Text(n, cx - col / 2, cy - rowH / 2, cx + col / 2, cy + rowH / 2, 12, dc, TF_Center, today ? 700 : 400);
        }
    }
};

// ---- Hardware ----
// CPU, RAM and disk are cheap and sampled on the bar thread at 1 Hz. GPU and network come from PDH: the
// first PDH use blocks for about half a second, so a worker owns it and only runs while the page shows.
struct HwShared {
    std::atomic<int> gpu{-1};          // percent, -1 unavailable
    std::atomic<long long> up{-1};     // bytes per second
    std::atomic<long long> down{-1};
    std::atomic<bool> run{false};
    std::atomic<bool> alive{false};
    Services* sv = nullptr;
};
HwShared g_hw;
HANDLE g_hwEvent = nullptr;
PDH_HQUERY g_pdhQuery = nullptr;
PDH_HCOUNTER g_pdhGpu = nullptr, g_pdhUp = nullptr, g_pdhDown = nullptr;

// Sum of every item that matches `filter`, grouped by `keyOf`; mode 0 = max of the groups, 1 = sum.
double PdhArray(PDH_HCOUNTER counter, bool gpu) {
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA) return -1;
    std::vector<BYTE> buf(bytes + 64);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    std::unordered_map<std::wstring, double> groups;
    double sum = 0;
    for (DWORD i = 0; i < count; i++) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        const std::wstring name = items[i].szName;
        const double v = items[i].FmtValue.doubleValue;
        if (gpu) {  // per engine, summed over processes; the busiest engine is the load
            const size_t at = name.find(L"_luid_");
            groups[at == std::wstring::npos ? name : name.substr(at)] += v;
        } else {
            if (name.find(L"Loopback") != std::wstring::npos || name.find(L"vEthernet") != std::wstring::npos ||
                name.find(L"isatap") != std::wstring::npos || name.find(L"Teredo") != std::wstring::npos) continue;
            sum += v;
        }
    }
    if (!gpu) return sum;
    double best = 0;
    for (auto& g : groups) best = std::max(best, g.second);
    return best;
}

DWORD WINAPI HwWorker(void*) {
    for (;;) {
        if (!g_pdhQuery && PdhOpenQueryW(nullptr, 0, &g_pdhQuery) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(g_pdhQuery, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g_pdhGpu);
            PdhAddEnglishCounterW(g_pdhQuery, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &g_pdhUp);
            PdhAddEnglishCounterW(g_pdhQuery, L"\\Network Interface(*)\\Bytes Received/sec", 0, &g_pdhDown);
        }
        if (g_pdhQuery) {
            PdhCollectQueryData(g_pdhQuery);  // Baseline: rates need two samples.
            while (g_hw.run) {
                WaitForSingleObject(g_hwEvent, 1000);
                if (!g_hw.run) break;
                if (PdhCollectQueryData(g_pdhQuery) != ERROR_SUCCESS) continue;
                if (g_pdhGpu) {
                    const double v = PdhArray(g_pdhGpu, true);
                    g_hw.gpu = v < 0 ? -1 : (int)(std::min(v, 100.0) + 0.5);
                }
                if (g_pdhUp) g_hw.up = (long long)std::max(PdhArray(g_pdhUp, false), -1.0);
                if (g_pdhDown) g_hw.down = (long long)std::max(PdhArray(g_pdhDown, false), -1.0);
                if (g_hw.sv) g_hw.sv->Post([] { if (g_hw.sv) g_hw.sv->Invalidate(); });
            }
        } else {
            g_hw.run = false;
        }
        g_hw.alive = false;
        if (!(g_hw.run && !g_hw.alive.exchange(true))) break;
    }
    return 0;
}

class HardwarePage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-hardware"; }
    int DashboardOrder() const override { return 40; }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        struct Card { const wchar_t* label; std::wstring value; float frac; int glyph; };
        wchar_t b[48];
        Card cards[6];
        swprintf_s(b, L"%d%%", cpu_);
        cards[0] = {L"CPU", cpu_ < 0 ? L"--" : b, cpu_ < 0 ? -1.0f : cpu_ / 100.0f, 0};
        wchar_t ram[48];
        swprintf_s(ram, L"%.1f / %.1f GB", ramUsed_, ramTotal_);
        cards[1] = {L"RAM", ramTotal_ > 0.1 ? ram : L"--", ramTotal_ > 0.1 ? (float)(ramUsed_ / ramTotal_) : -1.0f, 1};
        wchar_t gp[16];
        swprintf_s(gp, L"%d%%", g_hw.gpu.load());
        cards[2] = {L"GPU", g_hw.gpu < 0 ? L"--" : gp, g_hw.gpu < 0 ? -1.0f : g_hw.gpu / 100.0f, 2};
        wchar_t nu[32], nd[32];
        swprintf_s(nu, L"%.1f Mbps", g_hw.up.load() * 8.0 / 1e6);
        swprintf_s(nd, L"%.1f Mbps", g_hw.down.load() * 8.0 / 1e6);
        cards[3] = {L"NET UP", g_hw.up < 0 ? L"--" : nu, -1.0f, 3};
        cards[4] = {L"NET DOWN", g_hw.down < 0 ? L"--" : nd, -1.0f, 4};
        wchar_t dk[48];
        swprintf_s(dk, L"%.0f / %.0f GB", diskUsed_, diskTotal_);
        cards[5] = {L"DISK C:", diskTotal_ > 0.1 ? dk : L"--", diskTotal_ > 0.1 ? (float)(diskUsed_ / diskTotal_) : -1.0f, 5};
        // Reading order: CPU, NET UP / RAM, NET DOWN / GPU, DISK.
        static const int order[6] = {0, 3, 1, 4, 2, 5};
        const float cw = (kPageW - 48 - 9 - 12) / 2.0f;
        for (int i = 0; i < 6; i++) {
            const Card& k = cards[order[i]];
            const float l = 24 + (i % 2) * (cw + 9), t = 22 + (i / 2) * 44, r = l + cw, bt = t + 38;
            p.Fill(l, t, r, bt, 9, kCard);
            D2D1_COLOR_F tint = k.frac >= 0.90f ? kRed : k.frac >= 0.75f ? kAmber : kAccent;
            Glyph(p, k.glyph, l + 16, t + 19, tint);
            p.Text(k.label, l + 30, t + 2, r - 9, t + 15, 11, kMuted);
            p.Text(k.value, l + 30, t + 15, r - 9, k.frac >= 0 ? t + 31 : bt - 4, 13.5f, kFg, TF_Ellipsis, 600);
            if (k.frac >= 0) {
                p.Fill(l + 30, bt - 6, r - 9, bt - 3.5f, 1.25f, kHairline);
                const float w = (r - 9 - l - 30) * std::min(k.frac, 1.0f);
                if (w > 0.5f) p.Fill(l + 30, bt - 6, l + 30 + w, bt - 3.5f, 1.25f, tint);
            }
        }
    }
protected:
    void Shown() override {
        cpu_ = -1;
        kPrev_ = 0;
        g_hw.gpu = -1;
        g_hw.up = g_hw.down = -1;
        g_hw.sv = sv_;
        tick_ = 0;
        Sample();
        sv_->SetTimer(this, 1000, [this] { Sample(); });
        if (!g_hwEvent) g_hwEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_hw.run = true;
        if (!g_hw.alive.exchange(true)) {
            if (HANDLE h = CreateThread(nullptr, 0, HwWorker, nullptr, 0, nullptr)) CloseHandle(h);
            else g_hw.alive = false;
        }
    }
    void Hidden() override {
        sv_->KillTimer(this);
        g_hw.run = false;
        if (g_hwEvent) SetEvent(g_hwEvent);
    }
private:
    void Sample() {
        FILETIME idle, kern, user;
        if (GetSystemTimes(&idle, &kern, &user)) {
            auto u = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
            const ULONGLONG i = u(idle), k = u(kern) + u(user);  // kernel time includes idle
            if (kPrev_) {
                const ULONGLONG dk = k - kPrev_, di = i - iPrev_;
                if (dk > 0) cpu_ = (int)(100.0 * (double)(dk - std::min(di, dk)) / (double)dk + 0.5);
            }
            kPrev_ = k;
            iPrev_ = i;
        }
        MEMORYSTATUSEX m{sizeof(m)};
        if (GlobalMemoryStatusEx(&m)) {
            ramTotal_ = m.ullTotalPhys / 1073741824.0;
            ramUsed_ = (m.ullTotalPhys - m.ullAvailPhys) / 1073741824.0;
        }
        if (tick_++ % 30 == 0) {
            ULARGE_INTEGER avail{}, total{}, freeB{};
            if (GetDiskFreeSpaceExW(L"C:\\", &avail, &total, &freeB) && total.QuadPart) {
                diskTotal_ = total.QuadPart / 1073741824.0;
                diskUsed_ = (total.QuadPart - freeB.QuadPart) / 1073741824.0;
            }
        }
        sv_->Invalidate();
    }
    static void Glyph(const Pg& p, int id, float x, float y, D2D1_COLOR_F k) {
        switch (id) {
            case 0: p.Outline(x - 6, y - 6, x + 6, y + 6, 2, 1.3f, k); p.Fill(x - 2.5f, y - 2.5f, x + 2.5f, y + 2.5f, 1, k); break;
            case 1: for (int i = -1; i <= 1; i++) p.Fill(x + i * 4.5f - 1.5f, y - 6, x + i * 4.5f + 1.5f, y + 6, 1, k); break;
            case 2: p.Outline(x - 7, y - 5, x + 7, y + 5, 2, 1.3f, k); p.Ring(x, y, 2.5f, 1.3f, k); break;
            case 3: p.Line(x, y + 6, x, y - 5, 1.6f, k); p.Line(x - 4, y - 1, x, y - 5, 1.6f, k); p.Line(x + 4, y - 1, x, y - 5, 1.6f, k); break;
            case 4: p.Line(x, y - 6, x, y + 5, 1.6f, k); p.Line(x - 4, y + 1, x, y + 5, 1.6f, k); p.Line(x + 4, y + 1, x, y + 5, 1.6f, k); break;
            default: p.Outline(x - 7, y - 4, x + 7, y + 4, 2.5f, 1.3f, k); p.Dot(x + 3.5f, y, 1.2f, k); break;
        }
    }
    int cpu_ = -1, tick_ = 0;
    ULONGLONG kPrev_ = 0, iPrev_ = 0;
    double ramUsed_ = 0, ramTotal_ = 0, diskUsed_ = 0, diskTotal_ = 0;
};

// ---- Weather ----
struct WeatherData {
    bool valid = false;
    int code = 0, temp = 0, feels = 0, humidity = 0, wind = 0;
    bool fahrenheit = true;
    std::wstring desc, area, dir, city;
    ULONGLONG when = 0;  // WallSeconds of the reading
};

// Value after "key": up to the closing quote, or to the next separator for numbers.
bool JsonValue(const std::string& j, const char* key, size_t from, std::string& out, size_t* at = nullptr) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return false;
    p += k.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == ':')) p++;
    if (p >= j.size()) return false;
    out.clear();
    if (j[p] == '"') {
        for (p++; p < j.size() && j[p] != '"'; p++) {
            if (j[p] == '\\' && p + 1 < j.size()) p++;
            out += j[p];
        }
    } else {
        while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != ']' && j[p] != '\r' && j[p] != '\n') out += j[p++];
    }
    if (at) *at = p;
    return true;
}

std::string PercentEncode(const std::wstring& city) {
    const std::string u = Utf8(city);
    bool encoded = false;  // already %XX: pass through
    for (size_t i = 0; i + 2 < u.size(); i++) {
        if (u[i] == '%' && isxdigit((unsigned char)u[i + 1]) && isxdigit((unsigned char)u[i + 2])) encoded = true;
    }
    if (encoded) return u;
    std::string out;
    char buf[4];
    for (unsigned char ch : u) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += (char)ch;
        else { snprintf(buf, sizeof(buf), "%%%02X", ch); out += buf; }
    }
    return out;
}

// Runs on a worker. Fills `w` only for a real reading (HTTP 200 and a code or a description).
bool FetchWeather(const std::wstring& city, bool fahrenheit, WeatherData& w) {
    const std::wstring url = L"https://wttr.in/" + Wide(PercentEncode(city)) + L"?format=j2";
    HINTERNET h = InternetOpenW(L"top-bar", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!h) return false;
    DWORD to = 10000;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_SEND_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    HINTERNET u = InternetOpenUrlW(h, url.c_str(), L"Accept: application/json\r\n", (DWORD)-1,
                                   INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    bool ok = false;
    if (u) {
        DWORD status = 0, len = sizeof(status);
        if (HttpQueryInfoW(u, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr) && status == 200) {
            std::string body;
            char buf[4096];
            DWORD n = 0;
            while (body.size() < (256u << 10) && InternetReadFile(u, buf, sizeof(buf), &n) && n) body.append(buf, n);
            const size_t cc = body.find("\"current_condition\"");
            std::string v;
            if (cc != std::string::npos) {
                WeatherData r;
                r.fahrenheit = fahrenheit;
                if (JsonValue(body, fahrenheit ? "temp_F" : "temp_C", cc, v)) r.temp = atoi(v.c_str());
                if (JsonValue(body, fahrenheit ? "FeelsLikeF" : "FeelsLikeC", cc, v)) r.feels = atoi(v.c_str());
                if (JsonValue(body, "humidity", cc, v)) r.humidity = atoi(v.c_str());
                if (JsonValue(body, fahrenheit ? "windspeedMiles" : "windspeedKmph", cc, v)) r.wind = atoi(v.c_str());
                if (JsonValue(body, "winddir16Point", cc, v)) r.dir = Wide(v);
                if (JsonValue(body, "weatherCode", cc, v)) r.code = atoi(v.c_str());
                const size_t wd = body.find("\"weatherDesc\"", cc);
                if (wd != std::string::npos && JsonValue(body, "value", wd, v)) r.desc = Wide(v);
                while (!r.desc.empty() && r.desc.back() == L' ') r.desc.pop_back();
                const size_t na = body.find("\"areaName\"");
                if (na != std::string::npos && JsonValue(body, "value", na, v)) r.area = Wide(v);
                if (r.code != 0 || !r.desc.empty()) {
                    r.valid = true;
                    r.when = WallSeconds();
                    w = r;
                    ok = true;
                }
            }
        }
        InternetCloseHandle(u);
    }
    InternetCloseHandle(h);
    return ok;
}

int WeatherKind(int code) {  // 0 sun, 1 partly, 2 cloud, 3 rain, 4 snow, 5 storm, 6 fog
    switch (code) {
        case 113: return 0;
        case 116: return 1;
        case 119: case 122: return 2;
        case 143: case 248: case 260: return 6;
        case 200: case 386: case 389: case 392: case 395: return code >= 392 ? 4 : 5;
        case 179: case 182: case 185: case 227: case 230: case 323: case 326: case 329: case 332: case 335: case 338:
        case 350: case 368: case 371: case 374: case 377: return 4;
        default: return code >= 176 ? 3 : 2;
    }
}

const wchar_t* WindArrow(const std::wstring& dir) {  // flow direction, not bearing
    static const wchar_t* names[] = {L"N", L"NNE", L"NE", L"ENE", L"E", L"ESE", L"SE", L"SSE", L"S", L"SSW", L"SW", L"WSW", L"W", L"WNW", L"NW", L"NNW"};
    static const wchar_t* arrows[] = {L"\u2193", L"\u2193", L"\u2199", L"\u2190", L"\u2190", L"\u2190", L"\u2196", L"\u2191", L"\u2191", L"\u2191", L"\u2197", L"\u2192", L"\u2192", L"\u2192", L"\u2198", L"\u2193"};
    for (int i = 0; i < 16; i++) if (dir == names[i]) return arrows[i];
    return L"";
}

class WeatherPage;
struct WeatherJob { std::wstring city; bool f; Services* sv; WeatherPage* self; };
DWORD WINAPI WeatherThread(void* arg);

class WeatherPage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-weather"; }
    int DashboardOrder() const override { return 30; }
    void Start(Services& s) override {
        PageBase::Start(s);
        city_ = s.GetSettings().weatherCity;
        fahrenheit_ = s.GetSettings().fahrenheit;
    }
    void OnSettings(const Settings& s) override {
        if (s.weatherCity == city_ && s.fahrenheit == fahrenheit_) return;
        city_ = s.weatherCity;
        fahrenheit_ = s.fahrenheit;
        fails_ = 0;
        nextTry_ = 0;
        data_.valid = false;  // another unit or place: the old reading no longer applies
        if (shown_ && sv_) Consider();
    }
    void Draw(const DrawCtx& c) override {
        Pg p(c);
        const bool have = data_.valid;
        const ULONGLONG age = have ? WallSeconds() - data_.when : 0;
        const float dim = have && age > 3600 ? 0.55f : 1.0f;
        std::wstring place = !city_.empty() ? city_ : (have ? data_.area : L"");
        if (!have) place = fetching_ ? L"Locating..." : (fails_ ? L"Offline" : L"");
        p.Text(place, 26, 24, 188, 44, 11, kMuted, TF_Ellipsis);
        wchar_t t[16];
        if (have) swprintf_s(t, L"%d\u00B0", data_.temp); else wcscpy_s(t, L"--");
        if (have) WeatherIcon(p, WeatherKind(data_.code), 48, 78, 44, dim);
        p.Text(t, 80, 46, 190, 110, 42, kFg, 0, 700);
        if (have) {
            std::wstring d = data_.desc;
            if (age > 3600) { wchar_t a[32]; swprintf_s(a, L" (%llu h old)", age / 3600); d += a; }
            const size_t n = d.size();
            const float sz = n > 58 ? 9.8f : n > 44 ? 10.5f : n > 32 ? 11.5f : n > 22 ? 12.5f : 13.5f;
            p.Text(d, 26, 118, 188, 162, sz, kFg, TF_Wrap, 600);
        }
        p.Line(200, 34, 200, 150, 1, kHairline);
        wchar_t wind[40] = L"--", feels[16] = L"--", hum[16] = L"--";
        if (have) {
            swprintf_s(wind, L"%d %ls %ls", data_.wind, data_.fahrenheit ? L"mph" : L"km/h", WindArrow(data_.dir));
            swprintf_s(feels, L"%d\u00B0", data_.feels);
            swprintf_s(hum, L"%d%%", data_.humidity);
        }
        const wchar_t* labels[3] = {L"Wind", L"Feels like", L"Humidity"};
        const wchar_t* values[3] = {wind, feels, hum};
        for (int i = 0; i < 3; i++) {
            const float top = 25 + i * 47;
            p.Fill(214, top, 358, top + 40, 10, kCard, dim);
            if (i == 0) { p.Line(222, top + 15, 235, top + 15, 1.4f, kAccent, dim); p.Line(222, top + 20, 232, top + 20, 1.4f, kAccent, dim); p.Line(222, top + 25, 236, top + 25, 1.4f, kAccent, dim); }
            else if (i == 1) { p.Line(229, top + 12, 229, top + 24, 1.6f, kAccent, dim); p.Dot(229, top + 27, 3, kAccent, dim); }
            else { p.Dot(229, top + 24, 4, kAccent, dim); p.Line(229, top + 12, 225.5f, top + 21, 1.4f, kAccent, dim); p.Line(229, top + 12, 232.5f, top + 21, 1.4f, kAccent, dim); }
            p.Text(labels[i], 244, top + 3, 354, top + 19, 11, kMuted, TF_Ellipsis);
            p.Text(values[i], 244, top + 18, 354, top + 37, 13.5f, kFg, TF_Ellipsis, 600);
        }
    }
protected:
    void Shown() override {
        if (!loaded_) { loaded_ = true; LoadCache(); }
        Consider();
    }
    void Hidden() override { sv_->KillTimer(this); }
private:
    static void WeatherIcon(const Pg& p, int kind, float cx, float cy, float size, float a) {
        const float u = size;
        auto cloud = [&](float ox, float oy, D2D1_COLOR_F k) {
            p.Dot(cx + ox - 0.20f * u, cy + oy + 0.06f * u, 0.17f * u, k, a);
            p.Dot(cx + ox + 0.00f * u, cy + oy - 0.08f * u, 0.22f * u, k, a);
            p.Dot(cx + ox + 0.21f * u, cy + oy + 0.06f * u, 0.16f * u, k, a);
            p.Fill(cx + ox - 0.20f * u, cy + oy + 0.0f * u, cx + ox + 0.21f * u, cy + oy + 0.22f * u, 0.1f * u, k, a);
        };
        const D2D1_COLOR_F white = Rgb(228, 228, 228);
        if (kind == 0 || kind == 1) {
            const float sx = kind == 0 ? 0 : -0.14f * u, sy = kind == 0 ? 0 : -0.14f * u;
            p.Dot(cx + sx, cy + sy, 0.2f * u, kAmber, a);
            for (int i = 0; i < 8; i++) {
                const float an = i * 0.7853982f, c1 = cosf(an), s1 = sinf(an);
                p.Line(cx + sx + c1 * 0.29f * u, cy + sy + s1 * 0.29f * u, cx + sx + c1 * 0.4f * u, cy + sy + s1 * 0.4f * u, 1.6f, kAmber, a);
            }
        }
        if (kind >= 1) cloud(kind == 1 ? 0.08f * u : 0, kind == 1 ? 0.08f * u : -0.06f * u, white);
        if (kind == 3) for (int i = -1; i <= 1; i++) p.Line(cx + i * 0.2f * u, cy + 0.24f * u, cx + i * 0.2f * u - 0.06f * u, cy + 0.4f * u, 1.6f, Rgb(90, 170, 255), a);
        if (kind == 4) for (int i = -1; i <= 1; i++) p.Dot(cx + i * 0.2f * u, cy + 0.33f * u, 0.04f * u, white, a);
        if (kind == 5) {
            p.Line(cx + 0.05f * u, cy + 0.2f * u, cx - 0.05f * u, cy + 0.33f * u, 1.8f, kAmber, a);
            p.Line(cx - 0.05f * u, cy + 0.33f * u, cx + 0.06f * u, cy + 0.33f * u, 1.8f, kAmber, a);
            p.Line(cx + 0.06f * u, cy + 0.33f * u, cx - 0.03f * u, cy + 0.46f * u, 1.8f, kAmber, a);
        }
        if (kind == 6) for (int i = -1; i <= 1; i++) p.Line(cx - 0.3f * u, cy + i * 0.14f * u, cx + 0.3f * u, cy + i * 0.14f * u, 2.0f, white, a);
    }
    std::wstring CachePath() { return DataRoot() + L"\\weather.txt"; }
    void LoadCache() {
        std::string txt;
        if (!ReadWholeFile(CachePath(), txt, 65536)) return;
        WeatherData d;
        size_t pos = 0;
        std::wstring city;
        while (pos < txt.size()) {
            size_t e = txt.find('\n', pos);
            if (e == std::string::npos) e = txt.size();
            std::string line = txt.substr(pos, e - pos);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            pos = e + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "when") d.when = strtoull(v.c_str(), nullptr, 10);
            else if (k == "code") d.code = atoi(v.c_str());
            else if (k == "temp") d.temp = atoi(v.c_str());
            else if (k == "feels") d.feels = atoi(v.c_str());
            else if (k == "humidity") d.humidity = atoi(v.c_str());
            else if (k == "wind") d.wind = atoi(v.c_str());
            else if (k == "f") d.fahrenheit = v == "1";
            else if (k == "desc") d.desc = Wide(v);
            else if (k == "area") d.area = Wide(v);
            else if (k == "dir") d.dir = Wide(v);
            else if (k == "city") city = Wide(v);
        }
        d.valid = d.when != 0 && d.fahrenheit == fahrenheit_ && city == city_ && (d.code != 0 || !d.desc.empty());
        if (d.valid) { data_ = d; lastOk_ = d.when; }
    }
    void SaveCache() {
        char b[64];
        std::string s = "when=" + std::to_string(data_.when) + "\ncode=" + std::to_string(data_.code) + "\ntemp=" + std::to_string(data_.temp) +
                        "\nfeels=" + std::to_string(data_.feels) + "\nhumidity=" + std::to_string(data_.humidity) + "\nwind=" + std::to_string(data_.wind) +
                        "\nf=" + (data_.fahrenheit ? "1" : "0") + "\ndesc=" + Utf8(data_.desc) + "\narea=" + Utf8(data_.area) + "\ndir=" + Utf8(data_.dir) +
                        "\ncity=" + Utf8(city_) + "\n";
        (void)b;
        WriteFileAtomic(CachePath(), s);
    }
    // Fetch now if the reading is stale and backoff allows; otherwise a one-shot for the next due time.
    void Consider() {
        sv_->KillTimer(this);
        if (!shown_ || fetching_) return;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG wall = WallSeconds();
        const bool stale = !data_.valid || wall - lastOk_ >= 900;
        ULONGLONG due = stale ? 0 : (900 - (wall - lastOk_)) * 1000;
        if (stale && nextTry_ > now) due = nextTry_ - now;
        if (due == 0) { Start(); return; }
        sv_->SetTimer(this, (UINT)std::min<ULONGLONG>(due + 50, 3600000), [this] { sv_->KillTimer(this); Consider(); });
    }
    void Start() {
        fetching_ = true;
        const std::wstring city = city_;
        const bool f = fahrenheit_;
        Services* sv = sv_;
        WeatherPage* self = this;
        auto* job = new WeatherJob{city, f, sv, self};
        HANDLE h = CreateThread(nullptr, 0, WeatherThread, job, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete job; fetching_ = false; }
        sv_->Invalidate();
    }
    friend DWORD WINAPI WeatherThread(void*);
    void Done(bool ok, const WeatherData& w, const std::wstring& city, bool f) {
        fetching_ = false;
        if (city != city_ || f != fahrenheit_) { Consider(); return; }  // settings changed meanwhile
        if (ok) {
            data_ = w;
            lastOk_ = w.when;
            fails_ = 0;
            nextTry_ = 0;
            SaveCache();
        } else {
            static const ULONGLONG back[] = {60000, 120000, 300000, 900000};
            nextTry_ = GetTickCount64() + back[std::min(fails_, 3)];
            fails_++;
        }
        sv_->Invalidate();
        if (shown_) Consider();
    }
    std::wstring city_;
    bool fahrenheit_ = true, loaded_ = false, fetching_ = false;
    int fails_ = 0;
    ULONGLONG nextTry_ = 0, lastOk_ = 0;
    WeatherData data_;
};

DWORD WINAPI WeatherThread(void* arg) {
    std::unique_ptr<WeatherJob> j(static_cast<WeatherJob*>(arg));
    WeatherData w;
    const bool ok = FetchWeather(j->city, j->f, w);
    WeatherPage* page = j->self;
    const std::wstring city = j->city;
    const bool f = j->f;
    j->sv->Post([page, ok, w, city, f] { page->Done(ok, w, city, f); });
    return 0;
}

// ---- Focus timer: page and collapsed pill ----
class TimerModule : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-timer"; }
    int DashboardOrder() const override { return 50; }
    int PillPriority() const override { return active_ || finished_ ? 50 : 0; }
    Size2 Measure(View v) override { return v == View::Pill ? Size2{180.0f, kNotchH} : Size2{kPageW, kPageH}; }
    void OnVisibility(View v, bool shown) override {
        if (v == View::Alert) return;
        vis_[v == View::Pill ? 0 : 1] = shown;
        shown_ = vis_[1];
        hover_ = down_ = 0;
        Arm();
    }
    void Draw(const DrawCtx& c) override {
        const float rem = Remaining() / 1000.0f;
        const float frac = total_ > 0 && (active_ || finished_) ? (finished_ ? 1.0f : std::min(rem / total_, 1.0f)) : 0.0f;
        D2D1_COLOR_F col = finished_ ? kRed : isBreak_ ? Rgb(48, 212, 97) : Rgb(255, 148, 0);
        wchar_t tm[16];
        const int secs = (int)ceilf(rem);
        swprintf_s(tm, L"%d:%02d", secs / 60, secs % 60);
        if (c.view == View::Pill) {
            Pg p(c);
            const float h = (c.rect.bottom - c.rect.top) / c.scale;
            p.Ring(24, h / 2, 8, 2, kHairline);
            DrawArcFrac(p, 24, h / 2, 8, frac, 2, col);
            p.Text(finished_ ? L"Done" : tm, 40, 0, 170, h, 13, kFg, 0, 600);
            return;
        }
        Pg p(c);
        hits_.clear();
        p.Ring(86, 92, 44, 5, kCardHover);
        DrawArcFrac(p, 86, 92, 44, frac, 5, col);
        p.Text(active_ || finished_ ? tm : L"Ready", 46, 72, 126, 112, 18, kFg, TF_Center, 600);
        if (!running_ && active_) p.Text(L"Paused", 46, 108, 126, 124, 10, kMuted, TF_Center);
        const wchar_t* label = finished_ ? (isBreak_ ? L"Break Complete" : L"Focus Complete") : active_ ? (isBreak_ ? L"Break Timer" : L"Focus Timer") : L"Focus Timer";
        p.Text(label, 160, 26, 350, 48, 13.5f, finished_ ? kRed : kFg, TF_Ellipsis, 600);
        auto button = [&](int id, const wchar_t* text, float t) {
            p.Fill(160, t, 340, t + 28, 8, hover_ == id ? kCardHover : kCard);
            p.Text(text, 160, t, 340, t + 28, 12, kFg, TF_Center, 600);
            Hit(160, t, 340, t + 28, id);
        };
        if (active_) {
            button(4, running_ ? L"Pause" : L"Resume", 66);
            button(5, L"Stop", 102);
        } else {
            button(1, L"25 min focus", 56);
            button(2, L"50 min focus", 90);
            button(3, L"5 min break", 124);
        }
    }
protected:
    void Click(int id) override {
        const ULONGLONG now = GetTickCount64();
        if (id >= 1 && id <= 3) {
            static const int mins[] = {0, 25, 50, 5};
            total_ = mins[id] * 60;
            isBreak_ = id == 3;
            endsAt_ = now + (ULONGLONG)total_ * 1000;
            active_ = running_ = true;
            finished_ = false;
        } else if (id == 4 && active_) {
            if (running_) { remaining_ = endsAt_ > now ? endsAt_ - now : 0; running_ = false; }
            else { endsAt_ = now + remaining_; running_ = true; }
        } else if (id == 5) {
            active_ = running_ = finished_ = false;
        }
        Arm();
        sv_->Refresh();
    }
private:
    ULONGLONG Remaining() const {
        if (!active_) return 0;
        if (!running_) return remaining_;
        const ULONGLONG now = GetTickCount64();
        return endsAt_ > now ? endsAt_ - now : 0;
    }
    void Tick() {
        const ULONGLONG now = GetTickCount64();
        bool changed = false;
        if (active_ && running_ && Remaining() == 0) {  // computed from endsAt, never by counting ticks
            active_ = running_ = false;
            finished_ = true;
            finishedUntil_ = now + 6000;
            MessageBeep(MB_ICONASTERISK);
            changed = true;
        } else if (finished_ && now >= finishedUntil_) {
            finished_ = false;
            changed = true;
        }
        Arm();
        if (changed) sv_->Refresh(); else sv_->Invalidate();
    }
    // Visible: one paint per displayed second. Hidden: a single one-shot at the deadline. Idle: nothing.
    void Arm() {
        if (!sv_) return;
        const bool visible = vis_[0] || vis_[1];
        const ULONGLONG now = GetTickCount64();
        UINT ms = 0;
        if (finished_) ms = (UINT)(finishedUntil_ > now ? finishedUntil_ - now : 0) + 20;
        else if (active_ && running_) {
            const ULONGLONG rem = Remaining();
            ms = visible ? (UINT)(rem % 1000) + 20 : (UINT)rem + 20;
            if (ms < 20) ms = 20;
        }
        if (ms) sv_->SetTimer(this, ms, [this] { Tick(); });
        else sv_->KillTimer(this);
    }
    bool active_ = false, running_ = false, isBreak_ = false, finished_ = false;
    bool vis_[2] = {false, false};
    int total_ = 0;
    ULONGLONG endsAt_ = 0, remaining_ = 0, finishedUntil_ = 0;
};

IdlePage g_idlePage;
CalendarPage g_calendarPage;
WeatherPage g_weatherPage;
HardwarePage g_hardwarePage;
TimerModule g_timerModule;

#line 1 "bar/shelf.inc"
// ---- Shelf and Dropshelf (P6c): a page of dropped things, kept on D:, with OLE drop on every bubble ----
// Rows are addressed by item id, never by path or position: a drag blocks in DoDragDrop while the list can
// change, and the old tray removed every text row because they all shared an empty path.
// Everything here runs on the bar's thread.

unsigned IslandSurfaceGen();  // island.inc
void IslandOpenShelf();       // island.inc
void IslandRepaint();         // island.inc

enum class SK { File, Text, Link, Image, Stored };

struct ShelfItem {
    unsigned id = 0;
    SK kind = SK::File;
    std::wstring title;
    std::wstring path;  // File: the original. Text/Image/Stored: blob file name inside the blob folder ("" if inline).
    std::wstring text;  // Text: body when inline. Link: the URL.
    unsigned long long size = 0;
    unsigned long long addedAt = 0;
    bool isDir = false, missing = false;
    Pixmap* icon = nullptr;
    bool iconTried = false;
    std::shared_ptr<Pixmap> thumb;  // Small shell or WIC thumbnail, made off the bar thread.
    bool thumbAsked = false;
};

constexpr size_t kShelfMaxItems = 100;
constexpr unsigned long long kShelfBlobBudget = 2ULL << 30;
constexpr unsigned long long kShelfItemCap = 512ULL << 20;

bool g_dropActive = false;       // A drag is over the bar or the island.
ULONGLONG g_dropLeft = 0;        // When it last left.
bool g_shelfInternalDrag = false;  // Our own drag out: dropping it back on the bar does nothing.
bool g_shelfChoiceOpen = false;    // The keep-or-send card after a drop is up: the island stays open for it.

bool ShelfHoldsOpen() {
    return g_shelfChoiceOpen || g_dropActive || g_shelfInternalDrag || (g_dropLeft && GetTickCount64() - g_dropLeft < 300);
}

std::wstring ShelfDir() { return DataRoot() + L"\\shelf"; }
std::wstring BlobDir() { return ShelfDir() + L"\\blobs"; }

std::wstring EscapeField(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\t') o += L"\\t";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else o += c;
    }
    return o;
}

std::wstring UnescapeField(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t n = s[++i];
            o += n == L't' ? L'\t' : n == L'n' ? L'\n' : n == L'r' ? L'\r' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}

bool SafeBlobName(const std::wstring& n) {
    return !n.empty() && n.find_first_of(L"\\/:") == std::wstring::npos && n.find(L"..") == std::wstring::npos;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring FileNameOf(const std::wstring& p) {
    const size_t at = p.find_last_of(L"\\/");
    return at == std::wstring::npos ? p : p.substr(at + 1);
}

std::wstring ExtOf(const std::wstring& name) {
    const size_t at = name.find_last_of(L'.');
    return at == std::wstring::npos || name.size() - at > 8 ? L"" : Lower(name.substr(at));
}

bool IsImageExt(const std::wstring& e) {
    return e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".gif" || e == L".bmp" || e == L".webp" || e == L".tif" || e == L".tiff";
}

bool LooksLikeLink(const std::wstring& t) {
    if (t.find_first_of(L" \t\r\n") != std::wstring::npos) return false;
    const std::wstring l = Lower(t);
    return l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0 || l.rfind(L"mailto:", 0) == 0;
}

std::wstring OneLine(const std::wstring& t, size_t cap) {
    std::wstring o;
    bool space = false;
    for (wchar_t c : t) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') { space = !o.empty(); continue; }
        if (space) { o += L' '; space = false; }
        o += c;
        if (o.size() >= cap) { o += L"…"; break; }
    }
    return o;
}

std::wstring FormatSize(unsigned long long b) {
    wchar_t buf[32];
    if (b >= (1ULL << 30)) swprintf_s(buf, L"%.1f GB", b / 1073741824.0);
    else if (b >= (1ULL << 20)) swprintf_s(buf, L"%.1f MB", b / 1048576.0);
    else if (b >= 1024) swprintf_s(buf, L"%.0f KB", b / 1024.0);
    else swprintf_s(buf, L"%llu B", b);
    return buf;
}

// ---- Send to phone: the PCTracker app's Explorer verb, read at run time ----
// The verb's command only drops a request in PCTracker's outbox and exits; the agent does the sending.
// No verb registered: no phone option anywhere.
void AlertsPhoneSent(int sent, int failed);  // alerts.inc
void SetExpanded(bool expanded);             // island.inc

struct PhoneVerb {
    std::wstring file, dir;
    ULONGLONG readAt = 0;
};
PhoneVerb g_phoneVerb;

std::wstring ReadVerbCommand(const wchar_t* key) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, buf, &size) != ERROR_SUCCESS) return L"";
    std::wstring cmd = buf;
    if (type == REG_EXPAND_SZ) {
        wchar_t out[4096];
        const DWORD n = ExpandEnvironmentStringsW(buf, out, 4096);
        if (n && n <= 4096) cmd = out;
    }
    while (!cmd.empty() && iswspace(cmd.back())) cmd.pop_back();
    return cmd;
}

// Cached for 5 s: rows ask while they draw.
const std::wstring& PhoneCommand(bool dir) {
    const ULONGLONG now = GetTickCount64();
    if (!g_phoneVerb.readAt || now - g_phoneVerb.readAt > 5000) {
        g_phoneVerb.readAt = now;
        g_phoneVerb.file = ReadVerbCommand(L"Software\\Classes\\*\\shell\\PCTrackerSendToPhone\\command");
        g_phoneVerb.dir = ReadVerbCommand(L"Software\\Classes\\Directory\\shell\\PCTrackerSendToPhone\\command");
    }
    return dir ? g_phoneVerb.dir : g_phoneVerb.file;
}

// The verb's command with "%1" replaced by the quoted path. No window, not waited for.
bool LaunchPhoneSend(const std::wstring& path, bool dir) {
    const std::wstring& cmd = PhoneCommand(dir);
    if (cmd.empty() || path.empty()) return false;
    const std::wstring q = L"\"" + path + L"\"";
    std::wstring line = cmd;
    size_t at = line.find(L"\"%1\"");
    if (at != std::wstring::npos) line.replace(at, 4, q);
    else if ((at = line.find(L"%1")) != std::wstring::npos) line.replace(at, 2, q);
    else line += L" " + q;
    std::wstring cwd;  // The helper's own folder, when the command names it in quotes.
    if (line[0] == L'"') {
        const size_t e = line.find(L'"', 1);
        if (e != std::wstring::npos) {
            cwd = line.substr(1, e - 1);
            const size_t s = cwd.find_last_of(L"\\/");
            cwd = s == std::wstring::npos ? L"" : cwd.substr(0, s);
        }
    }
    const DWORD ca = cwd.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(cwd.c_str());
    if (ca == INVALID_FILE_ATTRIBUTES || !(ca & FILE_ATTRIBUTE_DIRECTORY)) cwd.clear();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        Wh_Log(L"top-bar: send to phone failed to start, error %lu", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool CopyTextToClipboard(const std::wstring& t) {
    if (!OpenClipboard(g_hwnd)) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

// DIB bytes (as on the clipboard) to a PNG file, through WIC.
bool DibToPng(const std::string& dib, const std::wstring& out) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    const auto* bi = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    DWORD off = 14 + bi->biSize;
    if (bi->biBitCount <= 8) off += (bi->biClrUsed ? bi->biClrUsed : 1u << bi->biBitCount) * 4;
    else if (bi->biCompression == BI_BITFIELDS && bi->biSize == 40) off += 12;
    std::string bmp(14, '\0');
    bmp[0] = 'B';
    bmp[1] = 'M';
    const DWORD total = (DWORD)(dib.size() + 14);
    memcpy(&bmp[2], &total, 4);
    memcpy(&bmp[10], &off, 4);
    bmp += dib;
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put())))) return false;
    Com<IWICStream> in, file;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> fr;
    Com<IPropertyBag2> props;
    if (FAILED(wic->CreateStream(in.Put())) || FAILED(in->InitializeFromMemory(reinterpret_cast<BYTE*>(&bmp[0]), (DWORD)bmp.size())) ||
        FAILED(wic->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put())) || FAILED(dec->GetFrame(0, frame.Put())) ||
        FAILED(wic->CreateStream(file.Put())) || FAILED(file->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.Put())) || FAILED(enc->Initialize(file.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(fr.Put(), props.Put())) || FAILED(fr->Initialize(props.Get())) || FAILED(fr->WriteSource(frame.Get(), nullptr)) ||
        FAILED(fr->Commit()) || FAILED(enc->Commit())) {
        file.Reset();
        DeleteFileW(out.c_str());
        return false;
    }
    return true;
}

// ---- Reading a data object. Only called from Drop or paste; QueryGetData is all DragEnter uses. ----
CLIPFORMAT g_cfDesc, g_cfContents, g_cfUrlW, g_cfUrlA, g_cfPng;

void InitFormats() {
    if (g_cfDesc) return;
    g_cfDesc = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    g_cfContents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    g_cfUrlW = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocatorW");
    g_cfUrlA = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocator");
    g_cfPng = (CLIPFORMAT)RegisterClipboardFormatW(L"PNG");
}

constexpr DWORD kTymeds = TYMED_HGLOBAL | TYMED_ISTREAM;

bool HasFormat(IDataObject* d, CLIPFORMAT cf, DWORD tymed = kTymeds, LONG idx = -1) {
    FORMATETC f{cf, nullptr, DVASPECT_CONTENT, idx, tymed};
    return d->QueryGetData(&f) == S_OK;
}

// Reads a format into memory (cap bytes), or straight to a file when `file` is set.
bool ReadFormat(IDataObject* d, CLIPFORMAT cf, LONG idx, std::string* mem, const std::wstring* file, unsigned long long cap, unsigned long long* sizeOut = nullptr) {
    FORMATETC f{cf, nullptr, DVASPECT_CONTENT, idx, kTymeds};
    STGMEDIUM m{};
    if (FAILED(d->GetData(&f, &m))) return false;
    bool ok = false;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (file) {
        h = CreateFileW(file->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { ReleaseStgMedium(&m); return false; }
    }
    unsigned long long total = 0;
    auto sink = [&](const void* p, size_t n) -> bool {
        total += n;
        if (total > cap) return false;
        if (file) { DWORD w = 0; return WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n; }
        mem->append(static_cast<const char*>(p), n);
        return true;
    };
    if (m.tymed == TYMED_HGLOBAL && m.hGlobal) {
        if (void* p = GlobalLock(m.hGlobal)) {
            ok = sink(p, GlobalSize(m.hGlobal));
            GlobalUnlock(m.hGlobal);
        }
    } else if (m.tymed == TYMED_ISTREAM && m.pstm) {
        char buf[65536];
        ok = true;
        for (;;) {
            ULONG got = 0;
            const HRESULT hr = m.pstm->Read(buf, sizeof(buf), &got);
            if (got && !sink(buf, got)) { ok = false; break; }
            if (hr != S_OK || got == 0) break;
        }
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        if (!ok) DeleteFileW(file->c_str());
    }
    ReleaseStgMedium(&m);
    if (ok && sizeOut) *sizeOut = total;
    return ok;
}

bool AcceptsData(IDataObject* d) {
    InitFormats();
    return HasFormat(d, CF_HDROP, TYMED_HGLOBAL) || (HasFormat(d, g_cfDesc, TYMED_HGLOBAL) && HasFormat(d, g_cfContents, kTymeds | TYMED_ISTORAGE, 0)) ||
           HasFormat(d, g_cfUrlW) || HasFormat(d, g_cfUrlA) || HasFormat(d, CF_UNICODETEXT) || HasFormat(d, CF_TEXT) || HasFormat(d, g_cfPng) ||
           HasFormat(d, CF_DIBV5) || HasFormat(d, CF_DIB);
}

DWORD PickDropEffect(DWORD allowed) {  // Never MOVE: the source keeps its data.
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

// ---- Thumbnails: one worker thread, started on demand, gone after 20 s idle ----
// IShellItemImageFactory gives thumbnails only (no icons: the row already has the shell icon). Images it
// cannot thumbnail are decoded with WIC. Results are small premultiplied pixmaps, posted to the bar thread.
void ShelfThumbReady(unsigned id, std::shared_ptr<Pixmap> pm);  // after the page

struct ThumbJob {
    unsigned id;
    std::wstring path;
    bool image;
    int px;
};

SRWLOCK g_thumbLock = SRWLOCK_INIT;
std::deque<ThumbJob> g_thumbQueue;
HANDLE g_thumbThread = nullptr;
HANDLE g_thumbEvent = nullptr;
bool g_thumbRunning = false, g_thumbStop = false;

// Scales any WIC source to fit px x px and copies it out as premultiplied BGRA.
std::shared_ptr<Pixmap> ThumbFromSource(IWICImagingFactory* wic, IWICBitmapSource* src, int px) {
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) return nullptr;
    const double f = std::min(1.0, std::min((double)px / w, (double)px / h));
    const UINT tw = std::max(1u, (UINT)(w * f + 0.5)), th = std::max(1u, (UINT)(h * f + 0.5));
    Com<IWICFormatConverter> conv;
    Com<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapScaler(scaler.Put())) || FAILED(scaler->Initialize(conv.Get(), tw, th, WICBitmapInterpolationModeFant)))
        return nullptr;
    auto pm = std::make_shared<Pixmap>();
    pm->w = (int)tw;
    pm->h = (int)th;
    pm->px.resize((size_t)tw * th);
    if (FAILED(scaler->CopyPixels(nullptr, tw * 4, tw * th * 4, reinterpret_cast<BYTE*>(pm->px.data())))) return nullptr;
    return pm;
}

std::shared_ptr<Pixmap> ThumbFromShell(IWICImagingFactory* wic, const ThumbJob& j) {
    Com<IShellItemImageFactory> fac;
    if (FAILED(SHCreateItemFromParsingName(j.path.c_str(), nullptr, IID_PPV_ARGS(fac.Put())))) return nullptr;
    HBITMAP hbm = nullptr;
    if (FAILED(fac->GetImage(SIZE{j.px, j.px}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbm)) || !hbm) return nullptr;
    std::shared_ptr<Pixmap> out;
    BITMAP bm{};
    if (GetObjectW(hbm, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0 && (long long)bm.bmWidth * bm.bmHeight <= 4096LL * 4096) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<DWORD> pix((size_t)bm.bmWidth * bm.bmHeight);
        HDC dc = GetDC(nullptr);
        const bool got = GetDIBits(dc, hbm, 0, bm.bmHeight, pix.data(), &bi, DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, dc);
        if (got) {
            bool alpha = false;
            for (DWORD v : pix) if (v >> 24) { alpha = true; break; }
            for (DWORD& v : pix) {
                if (!alpha) { v |= 0xFF000000; continue; }  // Photos come back with alpha 0: opaque.
                const DWORD a = v >> 24;                    // Otherwise premultiplied; keep channels within alpha.
                const DWORD r = std::min((v >> 16) & 0xFF, a), g = std::min((v >> 8) & 0xFF, a), b = std::min(v & 0xFF, a);
                v = (a << 24) | (r << 16) | (g << 8) | b;
            }
            Com<IWICBitmap> wb;
            if (SUCCEEDED(wic->CreateBitmapFromMemory(bm.bmWidth, bm.bmHeight, GUID_WICPixelFormat32bppPBGRA, bm.bmWidth * 4,
                                                      (UINT)(pix.size() * 4), reinterpret_cast<BYTE*>(pix.data()), wb.Put())))
                out = ThumbFromSource(wic, wb.Get(), j.px);
        }
    }
    DeleteObject(hbm);
    return out;
}

std::shared_ptr<Pixmap> ThumbFromWic(IWICImagingFactory* wic, const ThumbJob& j) {
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateDecoderFromFilename(j.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.Put())) ||
        FAILED(dec->GetFrame(0, frame.Put())))
        return nullptr;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || (unsigned long long)w * h > 64000000ULL) return nullptr;  // Skip huge images.
    return ThumbFromSource(wic, frame.Get(), j.px);
}

DWORD WINAPI ThumbThread(void*) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Com<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put()));
    bool idle = false;
    for (;;) {
        AcquireSRWLockExclusive(&g_thumbLock);
        if (g_thumbStop || (g_thumbQueue.empty() && idle) || !wic) {
            g_thumbRunning = false;
            g_thumbQueue.clear();
            ReleaseSRWLockExclusive(&g_thumbLock);
            break;
        }
        if (g_thumbQueue.empty()) {
            ReleaseSRWLockExclusive(&g_thumbLock);
            idle = WaitForSingleObject(g_thumbEvent, 20000) == WAIT_TIMEOUT;
            continue;
        }
        ThumbJob j = std::move(g_thumbQueue.front());
        g_thumbQueue.pop_front();
        ReleaseSRWLockExclusive(&g_thumbLock);
        idle = false;
        std::shared_ptr<Pixmap> pm = ThumbFromShell(wic.Get(), j);
        if (!pm && j.image) pm = ThumbFromWic(wic.Get(), j);
        // Posted even on failure, so the row stops waiting. The heap lambda owns the pixmap until it runs.
        auto* fn = new std::function<void()>([id = j.id, pm] { ShelfThumbReady(id, pm); });
        HWND hwnd = g_hwnd;
        if (!hwnd || !PostMessageW(hwnd, WM_APP_POST, 0, reinterpret_cast<LPARAM>(fn))) delete fn;
    }
    wic.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

bool ThumbRequest(ThumbJob j) {
    if (!g_thumbEvent) g_thumbEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_thumbEvent) return false;
    bool ok = true;
    AcquireSRWLockExclusive(&g_thumbLock);
    if (g_thumbStop) ok = false;
    else {
        g_thumbQueue.push_back(std::move(j));
        if (!g_thumbRunning) {
            if (g_thumbThread) CloseHandle(g_thumbThread);  // A worker that went idle has exited.
            g_thumbThread = CreateThread(nullptr, 0, ThumbThread, nullptr, 0, nullptr);
            g_thumbRunning = g_thumbThread != nullptr;
            if (!g_thumbRunning) { g_thumbQueue.clear(); ok = false; }
        }
    }
    ReleaseSRWLockExclusive(&g_thumbLock);
    if (ok) SetEvent(g_thumbEvent);
    return ok;
}

// Bar thread, before the window's resources go: the worker runs code from this DLL.
void ShelfShutdown() {
    AcquireSRWLockExclusive(&g_thumbLock);
    g_thumbStop = true;
    g_thumbQueue.clear();
    HANDLE t = g_thumbThread;
    g_thumbThread = nullptr;
    ReleaseSRWLockExclusive(&g_thumbLock);
    if (g_thumbEvent) SetEvent(g_thumbEvent);
    if (t) {
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
    if (g_thumbEvent) { CloseHandle(g_thumbEvent); g_thumbEvent = nullptr; }
}

// Uploads a pixmap for the current render target when needed.
ID2D1Bitmap* PixmapBitmap(const Pg& p, Pixmap& px) {
    const unsigned gen = IslandSurfaceGen();
    if (!px.bmp || px.gen != gen) {
        px.bmp.Reset();
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(p.c.rt->CreateBitmap(D2D1::SizeU(px.w, px.h), px.px.data(), px.w * 4, &bp, px.bmp.Put()))) px.gen = gen;
    }
    return px.bmp.Get();
}

// ---- Drag-out data helpers ----
HGLOBAL MakeHGlobal(const void* data, size_t bytes) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes ? bytes : 1);
    if (!mem) return nullptr;
    if (void* p = GlobalLock(mem)) {
        if (bytes) memcpy(p, data, bytes);
        GlobalUnlock(mem);
        return mem;
    }
    GlobalFree(mem);
    return nullptr;
}

bool SetHGlobal(IDataObject* d, CLIPFORMAT cf, const void* data, size_t bytes, LONG index = -1) {
    HGLOBAL mem = MakeHGlobal(data, bytes);
    if (!mem) return false;
    FORMATETC f{cf, nullptr, DVASPECT_CONTENT, index, TYMED_HGLOBAL};
    STGMEDIUM m{};
    m.tymed = TYMED_HGLOBAL;
    m.hGlobal = mem;
    if (FAILED(d->SetData(&f, &m, TRUE))) { GlobalFree(mem); return false; }  // Owned by the object on success.
    return true;
}

bool SetText(IDataObject* d, const std::wstring& t) {
    bool ok = SetHGlobal(d, CF_UNICODETEXT, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
    const int n = WideCharToMultiByte(CP_ACP, 0, t.c_str(), (int)t.size() + 1, nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        std::string a((size_t)n, '\0');
        WideCharToMultiByte(CP_ACP, 0, t.c_str(), (int)t.size() + 1, &a[0], n, nullptr, nullptr);
        SetHGlobal(d, CF_TEXT, a.data(), a.size());
    }
    return ok;
}

// One virtual file (descriptor + contents at index 0): Explorer and upload targets get a real file.
bool SetVirtualFile(IDataObject* d, const std::wstring& name, const std::string& bytes) {
    InitFormats();
    FILEGROUPDESCRIPTORW g{};
    g.cItems = 1;
    FILEDESCRIPTORW& fd = g.fgd[0];
    fd.dwFlags = FD_FILESIZE | FD_PROGRESSUI | FD_ATTRIBUTES;
    fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    fd.nFileSizeLow = (DWORD)bytes.size();
    fd.nFileSizeHigh = (DWORD)((unsigned long long)bytes.size() >> 32);
    wcsncpy_s(fd.cFileName, name.c_str(), _TRUNCATE);
    if (!SetHGlobal(d, g_cfDesc, &g, sizeof(g))) return false;
    return SetHGlobal(d, g_cfContents, bytes.data(), bytes.size(), 0);
}

bool SetHDrop(IDataObject* d, const std::wstring& path) {
    std::vector<BYTE> buf(sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t), 0);
    auto* df = reinterpret_cast<DROPFILES*>(buf.data());
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(buf.data() + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    return SetHGlobal(d, CF_HDROP, buf.data(), buf.size());
}

// A file name made from a row title: no separators or reserved characters, short.
std::wstring SafeFileName(const std::wstring& title, const wchar_t* fallback, const wchar_t* ext) {
    std::wstring o;
    for (wchar_t c : OneLine(title, 60)) o += (c < 32 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    while (!o.empty() && (o.back() == L' ' || o.back() == L'.' || o.back() == L'…')) o.pop_back();
    while (!o.empty() && o.front() == L' ') o.erase(0, 1);
    return (o.empty() ? std::wstring(fallback) : o) + ext;
}

// The picture as CF_DIB (BITMAPINFOHEADER + bottom-up 32 bpp rows), for apps that paste pictures. Capped at 4K.
bool ImageDib(const std::wstring& path, std::string& out) {
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put())))) return false;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.Put())) ||
        FAILED(dec->GetFrame(0, frame.Put())) || FAILED(frame->GetSize(&w, &h)) || !w || !h || (unsigned long long)w * h > 8400000ULL ||
        FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    const size_t stride = (size_t)w * 4;
    out.assign(sizeof(BITMAPINFOHEADER) + stride * h, '\0');
    auto* bi = reinterpret_cast<BITMAPINFOHEADER*>(&out[0]);
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = (LONG)w;
    bi->biHeight = (LONG)h;
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(stride * h);
    BYTE* px = reinterpret_cast<BYTE*>(&out[sizeof(BITMAPINFOHEADER)]);
    if (FAILED(conv->CopyPixels(nullptr, (UINT)stride, (UINT)(stride * h), px))) { out.clear(); return false; }
    std::vector<BYTE> row(stride);
    for (UINT y = 0; y < h / 2; y++) {  // Top-down from WIC, bottom-up for CF_DIB
        BYTE* a = px + y * stride;
        BYTE* b = px + (h - 1 - y) * stride;
        memcpy(row.data(), a, stride);
        memcpy(a, b, stride);
        memcpy(b, row.data(), stride);
    }
    return true;
}

// ---- The page ----
class ShelfPage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-shelf"; }
    int DashboardOrder() const override { return 60; }
    int Count() { EnsureLoaded(); return (int)items_.size(); }

    // A thumbnail arrived from the worker. The row may be gone by now; it is found by id.
    void SetThumb(unsigned id, std::shared_ptr<Pixmap> pm) {
        ShelfItem* it = Find(id);
        if (!it || !pm) return;
        it->thumb = std::move(pm);
        if (sv_) sv_->Invalidate();
    }

    // Adds what a data object offers. Reads finish before this returns; items are built in locals and committed at the end.
    int Ingest(IDataObject* d) {
        EnsureLoaded();
        InitFormats();
        std::vector<ShelfItem> fresh;
        std::wstring refuse;
        if (HasFormat(d, CF_HDROP, TYMED_HGLOBAL)) {
            FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM m{};
            if (SUCCEEDED(d->GetData(&f, &m))) {
                HDROP drop = (HDROP)m.hGlobal;
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                wchar_t tmp[MAX_PATH + 1] = {};
                GetTempPathW(MAX_PATH, tmp);
                const std::wstring tempDir = Lower(tmp);
                for (UINT i = 0; i < n && i < 64; i++) {
                    wchar_t path[MAX_PATH * 2] = {};
                    if (!DragQueryFileW(drop, i, path, MAX_PATH * 2)) continue;
                    ShelfItem it;
                    it.kind = SK::File;
                    it.path = path;
                    it.title = FileNameOf(it.path);
                    const DWORD a = GetFileAttributesW(path);
                    it.isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
                    WIN32_FILE_ATTRIBUTE_DATA info{};
                    if (!it.isDir && GetFileAttributesExW(path, GetFileExInfoStandard, &info)) it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                    if (!it.isDir && Lower(it.path).rfind(tempDir, 0) == 0) {  // Apps delete temp files soon: keep a copy.
                        if (it.size > kShelfItemCap) { refuse = L"Too large to keep"; continue; }
                        it.kind = IsImageExt(ExtOf(it.title)) ? SK::Image : SK::Stored;
                        std::wstring blob = NewBlobName(ExtOf(it.title));
                        if (CopyFileW(path, (BlobDir() + L"\\" + blob).c_str(), FALSE)) { it.path = blob; } else { continue; }
                    }
                    fresh.push_back(std::move(it));
                }
                ReleaseStgMedium(&m);
            }
        } else if (HasFormat(d, g_cfDesc, TYMED_HGLOBAL) && HasFormat(d, g_cfContents, kTymeds, 0)) {
            std::string desc;
            if (ReadFormat(d, g_cfDesc, -1, &desc, nullptr, 1 << 20) && desc.size() >= sizeof(UINT)) {
                const auto* g = reinterpret_cast<const FILEGROUPDESCRIPTORW*>(desc.data());
                const UINT n = std::min<UINT>(g->cItems, (UINT)((desc.size() - sizeof(UINT)) / sizeof(FILEDESCRIPTORW)));
                const bool isUrl = n >= 1 && ExtOf(g->fgd[0].cFileName) == L".url";
                for (UINT i = 0; !isUrl && i < n && i < 32; i++) {
                    ShelfItem it;
                    it.title = g->fgd[i].cFileName;
                    it.kind = IsImageExt(ExtOf(it.title)) ? SK::Image : SK::Stored;
                    const std::wstring blob = NewBlobName(ExtOf(it.title));
                    const std::wstring full = BlobDir() + L"\\" + blob;
                    unsigned long long sz = 0;
                    if (ReadFormat(d, g_cfContents, (LONG)i, nullptr, &full, kShelfItemCap, &sz)) {
                        it.path = blob;
                        it.size = sz;
                        fresh.push_back(std::move(it));
                    }
                }
            }
        }
        if (fresh.empty() && refuse.empty()) {
            std::wstring url;
            std::string raw;
            if (ReadFormat(d, g_cfUrlW, -1, &raw, nullptr, 65536)) {
                url.assign(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
                url = url.c_str();
            } else if (raw.clear(), ReadFormat(d, g_cfUrlA, -1, &raw, nullptr, 65536)) {
                url = Wide(std::string(raw.c_str()));
            }
            if (!url.empty()) {
                ShelfItem it;
                it.kind = SK::Link;
                it.text = url;
                it.title = OneLine(url, 60);
                fresh.push_back(std::move(it));
            }
        }
        if (fresh.empty() && refuse.empty()) {
            std::wstring text;
            std::string raw;
            if (ReadFormat(d, CF_UNICODETEXT, -1, &raw, nullptr, 4u << 20)) {
                text.assign(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
                text = text.c_str();
            } else if (raw.clear(), ReadFormat(d, CF_TEXT, -1, &raw, nullptr, 4u << 20)) {
                text = Wide(std::string(raw.c_str()));
            }
            while (!text.empty() && iswspace(text.back())) text.pop_back();
            if (!text.empty()) {
                ShelfItem it;
                it.kind = LooksLikeLink(text) ? SK::Link : SK::Text;
                it.title = OneLine(text, 60);
                it.size = text.size();
                if (it.kind == SK::Text && Utf8(text).size() > 4096) {  // Long text lives in a blob, so the index stays small.
                    const std::wstring blob = NewBlobName(L".txt");
                    if (WriteFileAtomic(BlobDir() + L"\\" + blob, Utf8(text))) it.path = blob;
                    else { it.text = text.substr(0, 4000); }
                } else {
                    it.text = text;
                }
                fresh.push_back(std::move(it));
            }
        }
        if (fresh.empty() && refuse.empty()) {
            const std::wstring blob = NewBlobName(L".png");
            const std::wstring full = BlobDir() + L"\\" + blob;
            std::string raw;
            bool ok = false;
            if (HasFormat(d, g_cfPng) && ReadFormat(d, g_cfPng, -1, nullptr, &full, kShelfItemCap)) ok = true;
            else if ((HasFormat(d, CF_DIBV5) && ReadFormat(d, CF_DIBV5, -1, &raw, nullptr, 256u << 20) && DibToPng(raw, full)) ||
                     (raw.clear(), HasFormat(d, CF_DIB) && ReadFormat(d, CF_DIB, -1, &raw, nullptr, 256u << 20) && DibToPng(raw, full))) ok = true;
            if (ok) {
                ShelfItem it;
                it.kind = SK::Image;
                it.path = blob;
                it.title = L"Image";
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &info)) it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                fresh.push_back(std::move(it));
            }
        }
        int added = 0;
        lastIngest_.clear();
        for (ShelfItem& it : fresh) {
            bool isNew = false;
            if (Commit(std::move(it), &isNew)) { added++; lastIngest_.push_back({items_.back().id, isNew}); }
            else refuse = L"Shelf is full";
        }
        if (added) { Save(); scroll_ = 0; }
        if (!added) Flash(refuse.empty() ? L"Nothing to keep" : refuse);
        if (sv_) sv_->Invalidate();
        return added;
    }

    void Draw(const DrawCtx& c) override {
        EnsureLoaded();
        Pg p(c);
        hits_.clear();
        p.Text(L"Shelf", 26, 14, 120, 34, 12, kFg, 0, 700);
        const int n = (int)items_.size();
        wchar_t cnt[32];
        swprintf_s(cnt, n == 1 ? L"%d item" : L"%d items", n);
        p.Text(status_.empty() ? std::wstring(cnt) : status_, 110, 14, 254, 34, 11, status_.empty() ? kMuted : kAccent, TF_Right | TF_Ellipsis);
        auto chip = [&](int id, float l, const wchar_t* text, D2D1_COLOR_F tint) {
            p.Fill(l, 14, l + 44, 34, 7, hover_ == id ? kCardHover : kCard);
            p.Text(text, l, 14, l + 44, 34, 11, tint, TF_Center, 600);
            Hit(l, 14, l + 44, 34, id);
        };
        chip(1, 262, L"Paste", kFg);
        chip(2, 310, confirmUntil_ > GetTickCount64() ? L"Sure?" : L"Clear", confirmUntil_ > GetTickCount64() ? kRed : kFg);

        if (n == 0) {
            static Com<ID2D1StrokeStyle> dash;
            if (!dash) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
                g_factory->CreateStrokeStyle(sp, nullptr, 0, dash.Put());
            }
            p.Outline(26, 46, 354, 150, 10, 1, kFg, 0.3f, dash.Get());
            p.Text(L"Drop anything here", 26, 46, 354, 150, 12, kMuted, TF_Center);
        }
        const int cap = kRows;
        scroll_ = std::max(0, std::min(scroll_, n - cap));
        for (int r = 0; r < cap; r++) {
            const int d = scroll_ + r;
            if (d >= n) break;
            ShelfItem& it = items_[n - 1 - d];
            const float top = 46 + r * (kRowH + kGap), bot = top + kRowH;
            const int rowId = 1000 + (int)it.id, rmId = 100000 + (int)it.id;
            const bool hot = hover_ == rowId || hover_ == rmId || hover_ == kPhoneIdBase + (int)it.id;
            if (hot) p.Fill(26, top, 354, bot, 9, kCard);
            Hit(26, top, 354, bot, rowId);
            const float a = it.missing ? 0.45f : 1.0f;
            DrawKindIcon(p, it, 34, top + 6, a);
            p.Col(kFg, a);
            const bool phone = hot && CanPhone(it);
            p.Text(it.title, 58, top, 258, bot, 11, it.missing ? kMuted : kFg, TF_Ellipsis);
            p.Text(SizeLabel(it), 258, top, phone ? 302 : 322, bot, 11, kMuted, TF_Right | TF_Ellipsis);
            if (hot) {
                const float cx = 337, cy = top + kRowH / 2;
                p.Dot(cx, cy, 7, hover_ == rmId ? kCardHover : kCard);
                p.Line(cx - 2.5f, cy - 2.5f, cx + 2.5f, cy + 2.5f, 1.3f, kFg);
                p.Line(cx - 2.5f, cy + 2.5f, cx + 2.5f, cy - 2.5f, 1.3f, kFg);
                Hit(cx - 10, cy - 10, cx + 10, cy + 10, rmId);
            }
            if (phone) {  // Send to phone; the row stays.
                const int phId = kPhoneIdBase + (int)it.id;
                const float cx = 315, cy = top + kRowH / 2;
                p.Dot(cx, cy, 7, hover_ == phId ? kCardHover : kCard);
                PhoneGlyph(p, cx, cy, kFg);
                Hit(cx - 9, cy - 10, cx + 9, cy + 10, phId);
            }
        }
        if (n > cap) {  // A thin scroll mark
            const float trackT = 46, trackB = 46 + cap * (kRowH + kGap) - kGap;
            const float th = (trackB - trackT) * cap / n, ty = trackT + (trackB - trackT - th) * scroll_ / std::max(1, n - cap);
            p.Fill(358, ty, 360, ty + th, 1, kFg, 0.35f);
        }
        DrawChoice(c);
        if (g_dropActive) p.Outline(3, 3, 377, 181, 14, 1.5f, kAccent, 0.8f);
    }

    bool Animating(View v) const override { return v == View::Dashboard && !choice_.Settled(); }

    // After a drop: the card asks keep or send, for the dropped rows the phone can take. Keep is the default.
    void OfferPhone() {
        offer_.clear();
        for (const Offer& o : lastIngest_) {
            if (ShelfItem* it = Find(o.id)) if (CanPhone(*it)) offer_.push_back(o);
        }
        lastIngest_.clear();
        if (offer_.empty()) return;
        g_shelfChoiceOpen = true;
        choiceUntil_ = GetTickCount64() + kChoiceMs;
        ArmTimer();
        ShowChoice(true);
    }

    bool OnWheel(int steps) override {
        const int n = Count();
        if (n <= kRows) return false;
        const int next = std::max(0, std::min(scroll_ - steps, n - kRows));
        if (next == scroll_) return false;  // At an end: the next notch changes the page.
        scroll_ = next;
        sv_->Invalidate();
        return true;
    }

    bool OnPointer(const PointerEvent& e, View v) override {
        const float x = e.pt.x / Scale(), y = e.pt.y / Scale();
        if (e.kind == PointerEvent::Down) {
            press_ = 0;
            for (const HitRect& h : hits_) if (x >= h.l && x < h.r && y >= h.t && y < h.b && h.id >= 1000 && h.id < 100000) press_ = h.id - 1000;
            for (const HitRect& h : hits_) if (x >= h.l - 3 && x < h.r + 3 && y >= h.t - 3 && y < h.b + 3 && h.id >= 100000) press_ = 0;  // The row buttons never start a drag
            for (const HitRect& h : hits_) if (x >= h.l && x < h.r && y >= h.t && y < h.b && h.id == kChoiceCard) press_ = 0;  // Nor does the card over the rows
            pressPt_ = e.pt;
        } else if (e.kind == PointerEvent::Move && press_ && (GetKeyState(VK_LBUTTON) & 0x8000)) {
            if (fabsf(e.pt.x - pressPt_.x) > GetSystemMetrics(SM_CXDRAG) || fabsf(e.pt.y - pressPt_.y) > GetSystemMetrics(SM_CYDRAG)) {
                const unsigned id = (unsigned)press_;
                press_ = 0;
                down_ = 0;
                DragOut(id);
                return true;
            }
        } else if (e.kind == PointerEvent::Up || e.kind == PointerEvent::Leave) {
            press_ = 0;
        }
        return PageBase::OnPointer(e, v);
    }

protected:
    void Shown() override { EnsureLoaded(); }
    void Hidden() override {
        scroll_ = 0;
        offer_.clear();  // Closing the island over the card keeps what was dropped.
        g_shelfChoiceOpen = false;
        choiceUntil_ = 0;
        choice_.Snap(0);
        ArmTimer();
        for (ShelfItem& it : items_) {  // Keep the small CPU pixels as the cache; drop the GPU copies.
            if (it.icon) it.icon->bmp.Reset();
            if (it.thumb) it.thumb->bmp.Reset();
        }
    }
    void Click(int id) override {
        if (id == 1) {
            Com<IDataObject> obj;
            if (SUCCEEDED(OleGetClipboard(obj.Put())) && obj) Ingest(obj.Get()); else Flash(L"Clipboard is empty");
        } else if (id == 2) {
            if (items_.empty()) return;
            bool destroys = false;
            for (const ShelfItem& it : items_) if (it.kind != SK::File && it.kind != SK::Link) destroys = true;
            const ULONGLONG now = GetTickCount64();
            if (destroys && confirmUntil_ <= now) {  // This destroys stored content: ask once.
                confirmUntil_ = now + 3000;
                ArmStatusTimer(3000);
            } else {
                for (ShelfItem& it : items_) Free(it);
                items_.clear();
                confirmUntil_ = 0;
                Save();
            }
        } else if (id == kChoiceKeep) {
            ResolveChoice(false);
        } else if (id == kChoiceSend) {
            ResolveChoice(true);
        } else if (id == kChoiceCard) {
            return;
        } else if (id >= kPhoneIdBase) {
            bool removed = false;
            Flash(SendItem((unsigned)(id - kPhoneIdBase), false, &removed) ? L"Sent to phone" : L"Could not send to phone");
        } else if (id >= 100000) {
            Remove((unsigned)(id - 100000));
        } else if (id >= 1000) {
            OpenItem((unsigned)(id - 1000));
        }
        sv_->Invalidate();
    }

private:
    static constexpr int kRows = 3;
    static constexpr float kRowH = 30, kGap = 6;
    static constexpr int kChoiceKeep = 3, kChoiceSend = 4, kChoiceCard = 5;  // Hit ids of the drop card
    static constexpr int kPhoneIdBase = 200000;                              // + item id: a row's phone button
    static constexpr ULONGLONG kChoiceMs = 4000;                            // Then it stays in the bar.

    struct Offer {
        unsigned id;
        bool fresh;  // Added by this drop, not an existing row that the drop promoted.
    };

    // Whether a row can go to the phone: a file or folder on disk, or the file the shelf stored.
    bool CanPhone(const ShelfItem& it) {
        if (it.missing) return false;
        if (it.kind == SK::File) return !PhoneCommand(it.isDir).empty();
        if (it.kind == SK::Link || it.path.empty() || !SafeBlobName(it.path)) return false;  // Links and short text are not files.
        return !PhoneCommand(false).empty();
    }

    static void PhoneGlyph(const Pg& p, float cx, float cy, D2D1_COLOR_F k) {
        p.Outline(cx - 3.2f, cy - 5, cx + 3.2f, cy + 5, 1.6f, 1.1f, k);
        p.Line(cx - 1, cy + 3, cx + 1, cy + 3, 1.1f, k);
    }

    std::wstring SentDir() { return ShelfDir() + L"\\sent"; }

    // Stored files go to the phone from their own folder under sent\, named after the row: the agent may send
    // after the row is gone. A hard link costs no space; sent\ is swept after a week.
    std::wstring SentLink(const ShelfItem& it, bool move) {
        const std::wstring blob = BlobPath(it);
        std::wstring name = SafeFileName(it.title, it.kind == SK::Text ? L"Text" : L"File", L"");
        const std::wstring ext = ExtOf(it.path);
        if (!ext.empty() && ExtOf(name) != ext) name += ext;
        const std::wstring dir = SentDir() + L"\\" + std::to_wstring(WallSeconds()) + L"_" + std::to_wstring(it.id);
        if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
        const std::wstring target = dir + L"\\" + name;
        if (CreateHardLinkW(target.c_str(), blob.c_str(), nullptr)) return target;
        if (move && MoveFileExW(blob.c_str(), target.c_str(), 0)) return target;
        RemoveDirectoryW(dir.c_str());
        return L"";
    }

    void SweepSent() {
        const ULONGLONG now = WallSeconds();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((SentDir() + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const ULONGLONG at = wcstoull(fd.cFileName, nullptr, 10);
            if (!at || now < at || now - at < 7 * 86400) continue;
            const std::wstring dir = SentDir() + L"\\" + fd.cFileName;
            WIN32_FIND_DATAW f2;
            HANDLE h2 = FindFirstFileW((dir + L"\\*").c_str(), &f2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (!(f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + f2.cFileName).c_str());
                } while (FindNextFileW(h2, &f2));
                FindClose(h2);
            }
            RemoveDirectoryW(dir.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Hands one row to the phone. remove: drop the row once handed over (only when the file survives that).
    bool SendItem(unsigned id, bool remove, bool* removed) {
        *removed = false;
        ShelfItem* it = Find(id);
        if (!it || !CanPhone(*it)) return false;
        bool ok = false, canRemove = true;
        if (it->kind == SK::File) {
            if (GetFileAttributesW(it->path.c_str()) == INVALID_FILE_ATTRIBUTES) { it->missing = true; return false; }
            ok = LaunchPhoneSend(it->path, it->isDir);
        } else {
            std::wstring path = SentLink(*it, remove);
            if (path.empty()) { path = BlobPath(*it); canRemove = false; }  // Only the stored copy: keep the row.
            ok = LaunchPhoneSend(path, false);
        }
        if (ok && remove && canRemove) { Remove(id); *removed = true; }
        return ok;
    }

    void ShowChoice(bool on) {
        choice_.Aim(on ? 1.0f : 0.0f, on ? kSpringExpandK : kSpringCollapseK, on ? kSpringExpandC : kSpringCollapseC);
        choiceT_ = QpcSeconds();
        if (sv_) { sv_->WakeAnimation(); sv_->Invalidate(); }
    }

    void ResolveChoice(bool send) {
        if (offer_.empty()) return;
        const std::vector<Offer> o = std::move(offer_);
        offer_.clear();
        g_shelfChoiceOpen = false;
        choiceUntil_ = 0;
        ArmTimer();
        ShowChoice(false);
        if (!send) return;
        int sent = 0, failed = 0;
        for (const Offer& f : o) {
            bool removed = false;  // Sent from the drop: not kept, unless the drop only promoted a row already there.
            if (SendItem(f.id, f.fresh, &removed)) sent++; else failed++;
        }
        Flash(sent ? L"Sent to phone" : L"Could not send to phone");
        AlertsPhoneSent(sent, failed);
        // The confirmation is an alert, and alerts wait while the island is open: let it close.
        if (sv_) sv_->Post([] { if (!g_pinned) SetExpanded(false); });
    }

    // The drop card: rises over the bottom rows, dark and flat like the island.
    void DrawChoice(const DrawCtx& c) {
        if (!choice_.Settled()) {
            double dt = c.now - choiceT_;
            if (dt > 0.05) dt = 0.05;
            if (dt > 0) choice_.Step(dt);
        }
        choiceT_ = c.now;
        const float a = std::clamp(choice_.x, 0.0f, 1.0f);
        if (a <= 0.01f) return;
        DrawCtx cc = c;
        cc.alpha *= a;
        Pg q(cc);
        const float dy = (1.0f - choice_.x) * 22.0f;  // Overshoot shows as a small lift
        const float t = 116 + dy, b = 174 + dy;
        q.Fill(22, t, 358, b, 13, kCentreBg);
        q.Fill(22, t, 358, b, 13, kCard);
        q.Outline(22.5f, t + 0.5f, 357.5f, b - 0.5f, 13, 1, kHairline);
        const bool open = !offer_.empty();
        if (open) Hit(22, t, 358, b, kChoiceCard);
        std::wstring what;
        if (offer_.size() == 1) { if (ShelfItem* it = Find(offer_[0].id)) what = it->title; }
        else if (offer_.size() > 1) what = std::to_wstring(offer_.size()) + L" items";
        q.Text(L"Dropped", 36, t + 9, 160, t + 27, 10.5f, kMuted, TF_Ellipsis);
        q.Text(what, 36, t + 27, 160, t + 47, 11.5f, kFg, TF_Ellipsis, 600);
        auto button = [&](int id, float l, float r, const wchar_t* text, bool primary) {
            const float bt = t + 14, bb = b - 14;
            const bool hot = open && hover_ == id;
            if (primary) q.Fill(l, bt, r, bb, 9, kAccent, hot ? 1.0f : 0.9f);
            else q.Fill(l, bt, r, bb, 9, hot ? kCardHover : kCard);
            if (id == kChoiceSend) {
                PhoneGlyph(q, l + 14, (bt + bb) / 2, primary ? kInk : kFg);
                q.Text(text, l + 22, bt, r - 6, bb, 11, primary ? kInk : kFg, TF_Center | TF_Ellipsis, 600);
            } else {
                q.Text(text, l + 4, bt, r - 4, bb, 11, primary ? kInk : kFg, TF_Center | TF_Ellipsis, 600);
            }
            if (open) Hit(l, bt, r, bb, id);
        };
        button(kChoiceKeep, 166, 250, L"Keep in bar", false);
        button(kChoiceSend, 256, 346, L"Send to phone", true);
    }

    ShelfItem* Find(unsigned id) {
        for (ShelfItem& it : items_) if (it.id == id) return &it;
        return nullptr;
    }
    std::wstring BlobPath(const ShelfItem& it) { return it.kind == SK::File ? it.path : BlobDir() + L"\\" + it.path; }
    std::wstring NewBlobName(const std::wstring& ext) {
        EnsureLoaded();
        return std::to_wstring(nextId_++) + L"b" + (ext.empty() ? L".bin" : ext);
    }
    void Free(ShelfItem& it) {
        if (it.kind != SK::File && !it.path.empty() && SafeBlobName(it.path)) DeleteFileW((BlobDir() + L"\\" + it.path).c_str());
        delete it.icon;
        it.icon = nullptr;
        it.thumb.reset();
    }
    void Remove(unsigned id) {
        for (size_t i = 0; i < items_.size(); i++) {
            if (items_[i].id == id) { Free(items_[i]); items_.erase(items_.begin() + i); break; }
        }
        Save();
    }
    // Dedup promotes the existing row. Text and links compare exactly, paths without case.
    bool Commit(ShelfItem it, bool* isNew) {
        *isNew = false;
        for (size_t i = 0; i < items_.size(); i++) {
            ShelfItem& o = items_[i];
            const bool same = o.kind == it.kind && ((it.kind == SK::File && Lower(o.path) == Lower(it.path)) ||
                                                     (it.kind == SK::Link && o.text == it.text) ||
                                                     (it.kind == SK::Text && o.title == it.title && o.size == it.size && o.text == it.text && o.path.empty() && it.path.empty()));
            if (same) {
                ShelfItem keep = std::move(o);
                items_.erase(items_.begin() + i);
                keep.addedAt = WallSeconds();
                items_.push_back(std::move(keep));
                if (it.kind != SK::File && !it.path.empty()) DeleteFileW((BlobDir() + L"\\" + it.path).c_str());
                return true;
            }
        }
        unsigned long long blobs = 0;
        for (const ShelfItem& o : items_) if (o.kind == SK::Stored || o.kind == SK::Image) blobs += o.size;
        if (items_.size() >= kShelfMaxItems || blobs + it.size > kShelfBlobBudget) {
            if (it.kind != SK::File && !it.path.empty()) DeleteFileW((BlobDir() + L"\\" + it.path).c_str());
            return false;
        }
        it.id = nextId_++;
        it.addedAt = WallSeconds();
        items_.push_back(std::move(it));
        *isNew = true;
        return true;
    }

    void Flash(const std::wstring& text) {
        status_ = text;
        ArmStatusTimer(1800);
    }
    void ArmStatusTimer(UINT ms) {
        statusUntil_ = GetTickCount64() + ms;
        ArmTimer();
    }
    // The module has one timer: it serves the status line and the drop card, whichever is due first.
    void ArmTimer() {
        if (!sv_) return;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG next = statusUntil_;
        if (choiceUntil_ && (!next || choiceUntil_ < next)) next = choiceUntil_;
        if (!next) { sv_->KillTimer(this); return; }
        sv_->SetTimer(this, (UINT)std::max<ULONGLONG>(next > now ? next - now : 0, 10), [this] { OnTimer(); });
    }
    void OnTimer() {
        const ULONGLONG now = GetTickCount64();
        if (statusUntil_ && statusUntil_ <= now) {
            statusUntil_ = 0;
            status_.clear();
            confirmUntil_ = 0;
            sv_->Invalidate();
        }
        if (choiceUntil_ && choiceUntil_ <= now) ResolveChoice(false);  // Nothing chosen: it stays in the bar.
        ArmTimer();
    }

    std::wstring SizeLabel(const ShelfItem& it) {
        switch (it.kind) {
            case SK::File: return it.isDir ? L"—" : FormatSize(it.size);
            case SK::Text: { wchar_t b[32]; swprintf_s(b, L"%llu ch", it.size); return b; }
            case SK::Link: {
                const size_t s = it.text.find(L"://");
                if (s == std::wstring::npos) return it.text.substr(it.text.find(L':') + 1, 20);
                const size_t e = it.text.find_first_of(L"/?#", s + 3);
                return it.text.substr(s + 3, e == std::wstring::npos ? std::wstring::npos : e - s - 3);
            }
            default: return FormatSize(it.size);
        }
    }

    void DrawKindIcon(const Pg& p, ShelfItem& it, float x, float y, float alpha) {
        const bool chip = it.kind == SK::Text || it.kind == SK::Link;
        if (chip) {
            p.Fill(x, y, x + 18, y + 18, 5, kCardHover, alpha);
            p.Text(it.kind == SK::Text ? L"T" : L"↗", x, y, x + 18, y + 18, 11, kFg, TF_Center, 700);
            return;
        }
        if (!it.iconTried) {
            it.iconTried = true;
            SHFILEINFOW sfi{};
            const std::wstring path = it.kind == SK::File ? it.path : it.title;
            DWORD flags = SHGFI_ICON | SHGFI_SMALLICON;
            if (it.missing || it.kind != SK::File || it.path.rfind(L"\\\\", 0) == 0) flags |= SHGFI_USEFILEATTRIBUTES;
            if (SHGetFileInfoW(path.c_str(), it.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags) && sfi.hIcon) {
                it.icon = PixmapFromIcon(sfi.hIcon);
                DestroyIcon(sfi.hIcon);
            }
        }
        // Thumbnails for files, stored files and images; asked once, for rows that are drawn.
        if (!it.thumbAsked && !it.missing && !it.isDir && (it.kind == SK::File || SafeBlobName(it.path))) {
            it.thumbAsked = true;
            const int px = std::max(16, std::min(64, (int)ceilf(18 * p.s)));
            ThumbRequest(ThumbJob{it.id, BlobPath(it), it.kind == SK::Image || IsImageExt(ExtOf(it.title)), px});
        }
        if (it.thumb && it.thumb->w > 0 && it.thumb->h > 0) {
            Pixmap& px = *it.thumb;
            if (ID2D1Bitmap* b = PixmapBitmap(p, px)) {
                const float f = 18.0f / std::max(px.w, px.h), w = px.w * f, h = px.h * f;  // Fit, keep aspect
                const float l = x + (18 - w) / 2, t = y + (18 - h) / 2;
                p.c.rt->DrawBitmap(b, p.R(l, t, l + w, t + h), p.c.alpha * alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                return;
            }
        }
        if (it.icon) {
            if (ID2D1Bitmap* b = PixmapBitmap(p, *it.icon)) p.c.rt->DrawBitmap(b, p.R(x, y, x + 18, y + 18), p.c.alpha * alpha);
        } else {
            p.Fill(x + 3, y + 1, x + 15, y + 17, 2, kMuted, alpha);
        }
    }

    // ---- Open ----
    void OpenItem(unsigned id) {
        ShelfItem* it = Find(id);
        if (!it) return;
        if (it->kind == SK::Text) {
            std::wstring body = it->text;
            if (!it->path.empty()) {
                std::string raw;
                if (SafeBlobName(it->path) && ReadWholeFile(BlobPath(*it), raw, 8u << 20)) body = Wide(raw);
            }
            Flash(CopyTextToClipboard(body) ? L"Copied" : L"Copy failed");
            return;
        }
        std::wstring target;
        if (it->kind == SK::Link) {
            if (!LooksLikeLink(it->text)) { Flash(CopyTextToClipboard(it->text) ? L"Copied" : L"Copy failed"); return; }  // Never launch arbitrary strings.
            target = it->text;
        } else {
            if (it->kind != SK::File && !SafeBlobName(it->path)) return;
            target = BlobPath(*it);
            if (it->kind == SK::File && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) { it->missing = true; return; }
        }
        AllowSetForegroundWindow(ASFW_ANY);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    // ---- Drag out ----
    class Source : public IDropSource {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
            if (r == IID_IUnknown || r == IID_IDropSource) { *o = this; return S_OK; }
            *o = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
            if (esc) return DRAGDROP_S_CANCEL;
            return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
        }
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
    };

    // An empty shell container that accepts SetData of any format (measured; see the spec).
    static bool EmptyDataObject(Com<IDataObject>& obj) {
        return SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_IDataObject, reinterpret_cast<void**>(obj.Put()))) && obj;
    }

    // Files: the shell's own data object (CF_HDROP, the ID list, names), else CF_HDROP by hand.
    static bool FileDataObject(const std::wstring& path, Com<IDataObject>& obj) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            if (PIDLIST_ABSOLUTE parent = ILClone(pidl)) {
                ILRemoveLastID(parent);
                PCUITEMID_CHILD child = ILFindLastID(pidl);
                if (FAILED(SHCreateDataObject(parent, 1, &child, nullptr, IID_IDataObject, reinterpret_cast<void**>(obj.Put())))) obj.Reset();
                ILFree(parent);
            }
            ILFree(pidl);
        }
        if (obj) return true;
        return EmptyDataObject(obj) && SetHDrop(obj.Get(), path);
    }

    void DragOut(unsigned id) {
        ShelfItem* it = Find(id);
        if (!it) return;
        const ShelfItem copy = *it;  // The list can change while the drag blocks.
        InitFormats();
        Com<IDataObject> obj;
        if (copy.kind == SK::File || copy.kind == SK::Stored || copy.kind == SK::Image) {
            if (copy.kind != SK::File && !SafeBlobName(copy.path)) return;
            const std::wstring path = BlobPath(copy);
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                if (ShelfItem* now = Find(copy.id)) now->missing = true;
                Flash(L"File not found");
                sv_->Invalidate();
                return;
            }
            if (!FileDataObject(path, obj)) return;
            if (copy.kind == SK::Image || (copy.kind == SK::File && IsImageExt(ExtOf(copy.title)))) {
                // The picture itself too, for apps that take pasted images rather than files.
                std::string png, dib;
                bool set = true;
                if (ExtOf(path) == L".png" && ReadWholeFile(path, png, 32u << 20)) set = SetHGlobal(obj.Get(), g_cfPng, png.data(), png.size());
                png = std::string();
                if (set && ImageDib(path, dib)) set = SetHGlobal(obj.Get(), CF_DIB, dib.data(), dib.size());
                if (!set) {  // The shell object refused extra formats: a plain container with CF_HDROP.
                    Com<IDataObject> plain;
                    if (EmptyDataObject(plain) && SetHDrop(plain.Get(), path)) {
                        obj = plain;
                        if (!dib.empty() || ImageDib(path, dib)) SetHGlobal(obj.Get(), CF_DIB, dib.data(), dib.size());
                    }
                }
            }
        } else {
            if (!EmptyDataObject(obj)) return;
            std::wstring body = copy.text;
            if (copy.kind == SK::Text && !copy.path.empty() && SafeBlobName(copy.path)) {
                std::string raw;
                if (ReadWholeFile(BlobPath(copy), raw, 8u << 20)) body = Wide(raw);
            }
            SetText(obj.Get(), body);
            // Never CF_HDROP for text rows: editors would open a file instead of inserting text.
            if (copy.kind == SK::Link) {
                SetHGlobal(obj.Get(), g_cfUrlW, copy.text.c_str(), (copy.text.size() + 1) * sizeof(wchar_t));
                const std::string a = Utf8(copy.text);
                SetHGlobal(obj.Get(), g_cfUrlA, a.c_str(), a.size() + 1);
                if (LooksLikeLink(copy.text)) SetVirtualFile(obj.Get(), SafeFileName(copy.title, L"Link", L".url"), "[InternetShortcut]\r\nURL=" + a + "\r\n");
            } else {
                SetVirtualFile(obj.Get(), SafeFileName(copy.title, L"Text", L".txt"), "\xEF\xBB\xBF" + Utf8(body));
            }
        }
        if (!obj) return;
        static const CLIPFORMAT cfPreferred = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        const DWORD copyEffect = DROPEFFECT_COPY;
        SetHGlobal(obj.Get(), cfPreferred, &copyEffect, sizeof(copyEffect));
        static Source source;
        DWORD effect = 0;
        g_shelfInternalDrag = true;
        using ShDo = HRESULT(WINAPI*)(HWND, IDataObject*, IDropSource*, DWORD, DWORD*);
        static ShDo shDo = reinterpret_cast<ShDo>(GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHDoDragDrop"));
        if (shDo) shDo(g_isHwnd, obj.Get(), &source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        else DoDragDrop(obj.Get(), &source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        g_shelfInternalDrag = false;
        g_dropLeft = GetTickCount64();
        hover_ = down_ = 0;  // No WM_MOUSELEAVE arrives after a drag.
        if (copy.kind == SK::File && GetFileAttributesW(copy.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (ShelfItem* now = Find(copy.id)) now->missing = true;  // Addressed by id, never by path.
        }
        sv_->Invalidate();
    }

    // ---- Persistence ----
    void EnsureLoaded() {
        if (loaded_) return;
        loaded_ = true;
        SHCreateDirectoryExW(nullptr, BlobDir().c_str(), nullptr);
        SweepSent();
        std::string raw;
        const std::wstring index = ShelfDir() + L"\\index.txt";
        if (!ReadWholeFile(index, raw)) {
            if (GetFileAttributesW(index.c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileExW(index.c_str(), (index + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
            return;
        }
        const std::wstring text = Wide(raw);
        size_t pos = 0;
        bool header = false, bad = false;
        std::vector<ShelfItem> loaded;
        while (pos < text.size()) {
            size_t e = text.find(L'\n', pos);
            if (e == std::wstring::npos) e = text.size();
            std::wstring line = text.substr(pos, e - pos);
            pos = e + 1;
            while (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty()) continue;
            if (!header) {
                if (line != L"TBSHELF 2") { bad = true; break; }
                header = true;
                continue;
            }
            std::vector<std::wstring> f;
            size_t s = 0;
            for (;;) {
                const size_t t = line.find(L'\t', s);
                f.push_back(UnescapeField(line.substr(s, t == std::wstring::npos ? std::wstring::npos : t - s)));
                if (t == std::wstring::npos) break;
                s = t + 1;
            }
            if (f.size() < 4) continue;  // Unknown or short lines are skipped
            ShelfItem it;
            it.id = (unsigned)wcstoul(f[1].c_str(), nullptr, 10);
            it.addedAt = wcstoull(f[2].c_str(), nullptr, 10);
            const wchar_t k = f[0].empty() ? L'?' : f[0][0];
            if (k == L'F') { it.kind = SK::File; it.path = f[3]; it.title = FileNameOf(it.path); }
            else if (k == L'T' && f.size() >= 6) { it.kind = SK::Text; it.title = f[3]; it.path = f[4]; it.text = f[5]; if (!it.path.empty() && !SafeBlobName(it.path)) continue; it.size = it.text.size(); }
            else if (k == L'U') { it.kind = SK::Link; it.title = f[3]; it.text = f.size() > 4 ? f[4] : L""; }
            else if ((k == L'I' || k == L'S') && f.size() >= 5) { it.kind = k == L'I' ? SK::Image : SK::Stored; it.title = f[3]; it.path = f[4]; if (!SafeBlobName(it.path)) continue; }
            else continue;
            if (!it.id) continue;
            // Missing entries stay, shown dim. Network paths are not touched here: they can stall for seconds.
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (it.kind == SK::File) {
                if (it.path.rfind(L"\\\\", 0) != 0) {
                    if (GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &info)) {
                        it.isDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        if (!it.isDir) it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                    } else {
                        it.missing = true;
                    }
                }
            } else if (!it.path.empty()) {
                if (GetFileAttributesExW((BlobDir() + L"\\" + it.path).c_str(), GetFileExInfoStandard, &info)) {
                    it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                    if (it.kind == SK::Text) it.size = std::max<unsigned long long>(it.size / 2, 4096);
                } else {
                    it.missing = true;
                }
            }
            nextId_ = std::max(nextId_, it.id + 1);
            loaded.push_back(std::move(it));
        }
        if (bad || (!header && !raw.empty())) {  // Keep the unreadable file: it may hold the only list.
            MoveFileExW(index.c_str(), (index + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
            return;
        }
        items_ = std::move(loaded);
        // Orphan blobs are removed only after a good parse.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((BlobDir() + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                bool used = false;
                for (const ShelfItem& it : items_) if (it.kind != SK::File && it.path == fd.cFileName) used = true;
                if (!used) DeleteFileW((BlobDir() + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    void Save() {
        const std::wstring index = ShelfDir() + L"\\index.txt";
        if (items_.empty()) {
            DeleteFileW(index.c_str());
            return;
        }
        std::wstring out = L"TBSHELF 2\n";
        for (const ShelfItem& it : items_) {
            const std::wstring head = std::to_wstring(it.id) + L"\t" + std::to_wstring(it.addedAt) + L"\t";
            switch (it.kind) {
                case SK::File: out += L"F\t" + head + EscapeField(it.path); break;
                case SK::Text: out += L"T\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.path) + L"\t" + EscapeField(it.text); break;
                case SK::Link: out += L"U\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.text); break;
                case SK::Image: out += L"I\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.path); break;
                case SK::Stored: out += L"S\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.path); break;
            }
            out += L"\n";
        }
        SHCreateDirectoryExW(nullptr, ShelfDir().c_str(), nullptr);
        if (!WriteFileAtomic(index, Utf8(out))) Flash(L"Could not save the shelf");
    }

    std::vector<ShelfItem> items_;
    bool loaded_ = false;
    unsigned nextId_ = 1;
    int scroll_ = 0;
    int press_ = 0;
    D2D1_POINT_2F pressPt_{};
    std::wstring status_;
    ULONGLONG confirmUntil_ = 0, statusUntil_ = 0;
    std::vector<Offer> lastIngest_;  // Rows the last Ingest added or promoted.
    std::vector<Offer> offer_;       // Rows the drop card is asking about.
    ULONGLONG choiceUntil_ = 0;
    Spring choice_;                  // The card: 0 gone, 1 up.
    double choiceT_ = 0;
};

ShelfPage g_shelf;
IModule* ShelfModule() { return &g_shelf; }
void ShelfThumbReady(unsigned id, std::shared_ptr<Pixmap> pm) { g_shelf.SetThumb(id, std::move(pm)); }

// ---- Drop target: one object for the bar window and the island ----
class ShelfDropTarget : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDropTarget) { *o = this; return S_OK; }
        *o = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
        accept_ = !g_shelfInternalDrag && AcceptsData(d);
        *eff = accept_ ? PickDropEffect(*eff) : DROPEFFECT_NONE;
        if (!helper_) CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_IDropTargetHelper, reinterpret_cast<void**>(helper_.Put()));
        POINT p{pt.x, pt.y};
        HWND at = WindowFromPoint(p);
        if (at != g_hwnd && at != g_isHwnd) at = g_hwnd;
        if (helper_) helper_->DragEnter(at, d, &p, *eff);
        if (accept_) {
            g_dropActive = true;
            g_dropLeft = 0;
            SetTimer(g_hwnd, kDropTimer, 120, nullptr);  // A passing drag does not flash the island open
            IslandRepaint();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* eff) override {
        // Allowed effects are not repeated here; COPY is what the source was told on enter.
        *eff = accept_ ? (*eff & DROPEFFECT_COPY ? DROPEFFECT_COPY : PickDropEffect(*eff)) : DROPEFFECT_NONE;
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragOver(&p, *eff);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        Left();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
        POINT p{pt.x, pt.y};
        *eff = accept_ ? PickDropEffect(*eff ? *eff : (DROPEFFECT_COPY | DROPEFFECT_LINK)) : DROPEFFECT_NONE;
        if (helper_) helper_->Drop(d, &p, *eff);
        if (accept_) {
            IslandOpenShelf();  // Stay on the shelf after the drop
            if (g_shelf.Ingest(d) == 0) *eff = DROPEFFECT_NONE;
            else g_shelf.OfferPhone();  // Keep in bar, or send to the phone
        }
        Left();
        return S_OK;
    }
private:
    void Left() {
        g_dropActive = false;
        g_dropLeft = GetTickCount64();
        KillTimer(g_hwnd, kDropTimer);
        IslandRepaint();
    }
    Com<IDropTargetHelper> helper_;
    bool accept_ = false;
};

ShelfDropTarget g_dropTarget;
bool g_integrityLogged = false;

void ShelfDropRegister(HWND hwnd) {
    if (!hwnd) return;
    if (!g_integrityLogged) {
        g_integrityLogged = true;
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            DWORD need = 0;
            GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &need);
            std::vector<BYTE> buf(need + 8);
            if (need && GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), (DWORD)buf.size(), &need)) {
                auto* l = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
                const DWORD rid = *GetSidSubAuthority(l->Label.Sid, *GetSidSubAuthorityCount(l->Label.Sid) - 1);
                if (rid > SECURITY_MANDATORY_MEDIUM_RID) Wh_Log(L"top-bar: running above medium integrity, drops from normal apps will be refused");
            }
            CloseHandle(tok);
        }
    }
    RegisterDragDrop(hwnd, &g_dropTarget);
}

void ShelfDropRevoke(HWND hwnd) {
    if (hwnd) RevokeDragDrop(hwnd);
}

void ShelfDropDwell() {
    KillTimer(g_hwnd, kDropTimer);
    if (g_dropActive) IslandOpenShelf();
}

#line 1 "bar/modules.inc"
// ---- The module list: collapsed content, alerts, and the dashboard pages (sorted by DashboardOrder) ----
IModule* AlertsModuleInstance();  // topbar/alerts/alerts.inc
IModule* const g_modules[] = {&g_clockModule, &g_mediaModule, AlertsModuleInstance(), &g_idlePage, &g_calendarPage, &g_weatherPage, &g_hardwarePage, &g_timerModule, &g_shelf};

#line 1 "bar/placement.inc"
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

void UpdateFullscreen() {
    g_fsRaw = g_abnFullscreen || ForegroundCoversMonitor();
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

#line 1 "bar/panels.inc"
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


#line 1 "bar/tray.inc"
// A tray icon's identity is its GUID if it has one, otherwise its window and id.
TrayIcon* FindTrayIcon(const BarTrayMessage& m, HWND hwnd) {
    const bool hasGuid = (m.flags & NIF_GUID) != 0;
    for (auto& icon : g_tray) {
        if (hasGuid ? (icon.hasGuid && IsEqualGUID(icon.guid, m.guid)) : (icon.hwnd == hwnd && icon.id == m.id)) {
            return &icon;
        }
    }
    return nullptr;
}

void RemoveDeadTrayIcons() {
    for (size_t i = g_tray.size(); i-- > 0;) {
        if (!IsWindow(g_tray[i].hwnd)) {
            delete g_tray[i].pix;
            g_tray.erase(g_tray.begin() + i);
        }
    }
}

// The icon handle is the bridge's own copy, alive in Explorer until the icon next changes.
void OnTrayMessage(const BarTrayMessage& m) {
    // Windows' own speaker, network and battery icons, the same ones Seelen leaves out.
    if ((m.flags & NIF_GUID) && m.guid.Data1 >= 0x7820AE73 && m.guid.Data1 <= 0x7820AE75) {
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>((UINT_PTR)m.hwnd);
    TrayIcon* icon = FindTrayIcon(m, hwnd);
    if (m.message == NIM_DELETE) {
        if (icon) {
            delete icon->pix;
            g_tray.erase(g_tray.begin() + (icon - g_tray.data()));
        }
    } else if (m.message == NIM_ADD || m.message == NIM_MODIFY || m.message == NIM_SETVERSION) {
        if (!icon) {
            if (m.message == NIM_SETVERSION) {
                return;
            }
            g_tray.emplace_back();
            icon = &g_tray.back();
            icon->uid = ++g_trayUid;
        }
        if (hwnd) {
            icon->hwnd = hwnd;
        }
        icon->id = m.id;
        if (m.flags & NIF_GUID) {
            icon->guid = m.guid;
            icon->hasGuid = true;
        }
        if (m.message == NIM_SETVERSION) {
            icon->version = m.version;
            return;
        }
        if (m.flags & NIF_MESSAGE) {
            icon->callback = m.callback;
        }
        if ((m.flags & NIF_STATE) && (m.stateMask & NIS_HIDDEN)) {
            icon->hidden = (m.state & NIS_HIDDEN) != 0;
        }
        if ((m.flags & NIF_ICON) && m.icon) {
            if (Pixmap* bitmap = PixmapFromIcon(reinterpret_cast<HICON>((UINT_PTR)m.icon))) {
                delete icon->pix;
                icon->pix = bitmap;
            }
        }
    } else {
        return;
    }
    RemoveDeadTrayIcons();
    InvalidateAll();
}

// Passes a click to the app in the form it asked for, as the taskbar would.
void ClickTrayIcon(int index, bool rightButton) {
    if (index < 0 || index >= (int)g_tray.size()) {
        return;
    }
    const TrayIcon icon = g_tray[index];
    if (!IsWindow(icon.hwnd) || !icon.callback) {
        RemoveDeadTrayIcons();
        InvalidateAll();
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(icon.hwnd, &pid);
    AllowSetForegroundWindow(pid);  // So its window or menu is able to come to the front.
    POINT pt;
    GetCursorPos(&pt);
    auto send = [&](UINT event) {
        if (icon.version >= 4) {
            SendNotifyMessageW(icon.hwnd, icon.callback, MAKEWPARAM(pt.x, pt.y), MAKELPARAM(event, icon.id));
        } else {
            SendNotifyMessageW(icon.hwnd, icon.callback, icon.id, event);
        }
    };
    send(rightButton ? WM_RBUTTONDOWN : WM_LBUTTONDOWN);
    send(rightButton ? WM_RBUTTONUP : WM_LBUTTONUP);
    if (icon.version >= 3) {
        send(rightButton ? WM_CONTEXTMENU : NIN_SELECT);
    }
}


#line 1 "bar/profile.inc"
bool IsStartWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(proc, 0, path, &size) &&
                    (wcsstr(path, L"\\StartMenuExperienceHost.exe") || wcsstr(path, L"\\SearchHost.exe"));
    CloseHandle(proc);
    return ok;
}

// Shows Start's own profile flyout under the avatar. See start-menu-pop.
void ToggleProfile() {
    if (g_profileOpen) {
        g_profileOpen = false;
        SendWinKey(0);
        return;
    }
    HWND hwnd = nullptr;
    bool armed = false;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"Windows.UI.Core.CoreWindow", nullptr))) {
        if (IsStartWindow(hwnd)) {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(hwnd, g_profileMessage, 0, 0, SMTO_ABORTIFHUNG, 150, &result)) {
                armed = true;
            }
        }
    }
    if (armed) {
        g_profileOpen = true;
        SendWinKey(0);
    }
}



#line 1 "bar/island.inc"
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

void AimIfChanged(Spring& sp, float target, float k, float c) {
    if (fabsf(sp.target - target) > 0.01f) sp.Aim(target, k, c);
}

bool IslandSettled() {
    return g_isW.Settled() && g_isH.Settled() && g_mix.Settled() && g_cover.Settled();
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

    if (!g_isInit) {
        g_isInit = true;
        g_isW.eps = g_isH.eps = 0.25f;  // Pixels.
        g_isW.epsV = g_isH.epsV = 5.0f;
        g_isW.Snap(tw);
        g_isH.Snap(th);
        g_mix.Snap(1.0f);
        g_viewCur = want;
        g_curSize = size;
        g_viewPrev = ViewSel{};
    } else {
        AimIfChanged(g_isW, tw, tw > g_isW.x ? kSpringExpandK : kSpringCollapseK, tw > g_isW.x ? kSpringExpandC : kSpringCollapseC);
        AimIfChanged(g_isH, th, th > g_isH.x ? kSpringExpandK : kSpringCollapseK, th > g_isH.x ? kSpringExpandC : kSpringCollapseC);
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
    g_autoUntil = 0;
    if (g_hwnd) {
        KillTimer(g_hwnd, kDwellTimer);
        KillTimer(g_hwnd, kZoneTimer);
    }
    g_isInit = false;
    if (g_isHwnd) ShowWindow(g_isHwnd, SW_HIDE);
}

// Every 50 ms while expanded: is the pointer still in the island (shoulders included)?
void IslandZoneTick() {
    if (g_isState != IState::Expanded) return;
    POINT p;
    GetCursorPos(&p);
    const float s = Scale();
    RECT zone = g_isTarget;
    InflateRect(&zone, (int)ceilf(kNotchShoulder * s), 0);
    bool in = PtInRect(&zone, p) != FALSE;
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
    // The calendar hangs under the island: opening over it would cover the panel.
    HWND fg = GetForegroundWindow();
    if (g_panelMode == kPanelCalendar && fg && IsShellExperienceWindow(fg)) return;
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

// The calendar hangs under the island; the notifications open natively above the dock (anchor unused).
void Services::OpenPanel(PanelKind kind, int anchorX) {
    if (kind == PanelKind::Calendar && !g_pinned) SetExpanded(false);  // The calendar takes the island's place.
    g_anchorX = anchorX;
    g_anchorTick = GetTickCount64();
    SetPropW(g_hwnd, L"TopBarAnchorX", reinterpret_cast<HANDLE>((INT_PTR)anchorX));
    SetPropW(g_hwnd, L"TopBarAnchorTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
    TogglePanel(kind == PanelKind::Notifications ? kPanelNative : kPanelCalendar);
}

void IslandOpenCalendar() {
    if (g_dwelling) {  // The click wins over a hover that was about to expand.
        g_dwelling = false;
        KillTimer(g_hwnd, kDwellTimer);
    }
    g_services.OpenPanel(PanelKind::Calendar, (g_isBody.left + g_isBody.right) / 2);
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

#line 1 "alerts/alerts.inc"
// ---- Alerts (P6b): Caps Lock, battery, Bluetooth, DND, notifications, clipboard, devices, privacy dots ----
// One module, one queue (specs/alerts.md section 2). Everything that mutates the queue runs on the UI thread;
// workers (keyboard hook, Bluetooth, notification listener, registry watcher) only post. The module owns a
// hidden top-level window: WM_DEVICECHANGE needs a top-level window, and the registered messages sent by
// panel-split (toast shown) and tray-bridge (volume popup shown) are addressed to it by class name.
// Not built here, by decision: Progress, Num Lock, privacy-dot tap. DND off goes through QuietHoursSettings (levels.inc).

namespace wfn = winrt::Windows::Foundation;
namespace wun = winrt::Windows::UI::Notifications;
namespace wunm = winrt::Windows::UI::Notifications::Management;
namespace wbt = winrt::Windows::Devices::Bluetooth;
namespace wde = winrt::Windows::Devices::Enumeration;

constexpr wchar_t kAlertsClass[] = L"WindhawkTopBarAlerts";
constexpr UINT WM_AL_CAPS = WM_APP + 40;     // lParam: Caps Lock on.
constexpr UINT WM_AL_PRIVACY = WM_APP + 41;  // A ConsentStore key changed.
constexpr UINT_PTR kAlTick = 1, kAlHeartbeat = 2, kAlClipboard = 3, kAlOsd = 4, kAlPrivacy = 5;

std::atomic<HWND> g_alHwnd{nullptr};
HANDLE g_alStop = nullptr;
HANDLE g_alWake = nullptr;  // Auto-reset: a toast appeared, read the notification list.
UINT g_alMsgToast = 0, g_alMsgOsd = 0;
std::atomic<bool> g_alHookSeen{false};
wunm::UserNotificationListener g_alListener{nullptr};

const wchar_t* AlT(const wchar_t* key, const wchar_t* en) {
    const wchar_t* v = Loc(key);
    return v == key ? en : v;
}

std::wstring AlFmt(const wchar_t* fmt, ...) {
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

enum class AK { Caps, Battery, Bluetooth, Device, Dnd, Notification, Clipboard, Volume, Brightness, Phone };
constexpr UINT WM_AL_LEVEL = WM_APP + 42;  // wParam: volume 0-100 (-1 unknown) | 0x10000 muted; lParam: brightness (-1 unknown).
HANDLE g_alLevelRead = nullptr, g_alLevelSet = nullptr;  // Auto-reset: read both levels / apply the requested one.
std::atomic<int> g_alSetVolume{-1}, g_alSetBright{-1};
HANDLE g_alDndOff = nullptr;               // Auto-reset: turn Do Not Disturb off (on the level thread, it is MTA).
constexpr UINT WM_AL_DNDFAIL = WM_APP + 43;  // That failed: open Settings instead.
DWORD WINAPI AlLevelThread(void*);  // alerts/levels.inc
// Level card size, DIP: normal, and while hovered (bigger, so it is easy to drag).
constexpr float kAlLevelW = 280.0f, kAlLevelH = 44.0f, kAlLevelHoverW = 360.0f, kAlLevelHoverH = 64.0f;

struct AItem {
    AK kind = AK::Caps;
    std::wstring title, sub, hint;
    std::wstring key;     // Bluetooth: device id. Notification: AUMID.
    std::shared_ptr<Pixmap> icon;
    int ms = 3000;
    int remaining = 0;
    int level = -1;       // Battery percent, for the glyph.
    bool charging = false;
    bool danger = false;  // Red status dot.
    bool good = false;    // Green status dot.
    bool on = false;      // Caps: now on.
    uint32_t nid = 0;     // Notification id in the bell list.
    bool more = false;    // The "+N more" line.
};

int AlPriority(AK k) {
    switch (k) {
        case AK::Volume:
        case AK::Brightness: return 95;  // Answers a key press: above everything.
        case AK::Caps: return 90;
        case AK::Notification: return 60;
        case AK::Clipboard: return 10;
        default: return 40;
    }
}

// ---- Decoding a notification logo into a Pixmap (WIC, premultiplied BGRA) ----
std::shared_ptr<Pixmap> AlDecodeImage(const std::vector<uint8_t>& bytes) {
    std::shared_ptr<Pixmap> out;
    if (bytes.empty()) return out;
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.Put())))) return out;
    Com<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size()))) return out;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put()))) return out;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.Put()))) return out;
    Com<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return out;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512) return out;
    auto pm = std::make_shared<Pixmap>();
    pm->w = (int)w;
    pm->h = (int)h;
    pm->px.resize((size_t)w * h);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)(pm->px.size() * 4), reinterpret_cast<BYTE*>(pm->px.data())))) return out;
    return pm;
}

ID2D1Bitmap* AlBitmap(ID2D1RenderTarget* rt, Pixmap& p) {
    if (p.bmp && p.gen == g_isSurface.Generation()) return p.bmp.Get();
    p.bmp.Reset();
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(p.w, p.h), p.px.data(), p.w * 4, &props, p.bmp.Put()))) return nullptr;
    p.gen = g_isSurface.Generation();
    return p.bmp.Get();
}

std::shared_ptr<Pixmap> AlIconOfExe(const wchar_t* path) {
    SHFILEINFOW fi{};
    if (!SHGetFileInfoW(path, 0, &fi, sizeof(fi), SHGFI_ICON | SHGFI_LARGEICON) || !fi.hIcon) return nullptr;
    std::shared_ptr<Pixmap> pm(PixmapFromIcon(fi.hIcon));
    DestroyIcon(fi.hIcon);
    return pm;
}

std::wstring AlExeOfWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc, 0, path, &size)) path[0] = 0;
    CloseHandle(proc);
    return path;
}

// ---- Do Not Disturb: WNF (undocumented, read-only) ----
typedef LONG AlNtStatus;
typedef AlNtStatus(NTAPI* AlWnfCallback)(ULONG64, ULONG, void*, void*, const void*, ULONG);
typedef AlNtStatus(NTAPI* AlWnfSubscribe)(void**, ULONG64, ULONG, AlWnfCallback, void*, const void*, ULONG, ULONG);
typedef AlNtStatus(NTAPI* AlWnfUnsubscribe)(void*);
typedef AlNtStatus(NTAPI* AlWnfQuery)(const ULONG64*, const void*, const void*, ULONG*, void*, ULONG*);
constexpr ULONG64 kAlWnfQuietHours = 0xD83063EA3BF1C75ULL;
void* g_alWnfSub = nullptr;
std::atomic<bool> g_alDndFirst{true};
class AlertsModule;
AlertsModule* g_alMod = nullptr;
AlNtStatus NTAPI AlWnfProc(ULONG64 name, ULONG, void*, void*, const void* buf, ULONG size);

// ---- Privacy: ConsentStore ----
std::wstring AlCleanApp(const wchar_t* sub) {
    std::wstring n = sub;
    size_t p = n.rfind(L'#');
    if (p != std::wstring::npos) n = n.substr(p + 1);
    p = n.rfind(L'\\');
    if (p != std::wstring::npos) n = n.substr(p + 1);
    if (n.size() > 4 && !_wcsicmp(n.c_str() + n.size() - 4, L".exe")) n.resize(n.size() - 4);
    p = n.find(L'_');
    if (p != std::wstring::npos && p > 0 && n.find(L'.') != std::wstring::npos) n.resize(p);
    return n;
}

bool AlKeyInUse(HKEY key, std::wstring* app, const wchar_t* name, int depth) {
    DWORD type = 0, size = 8;
    ULONGLONG start = 0, stop = 0;
    DWORD s1 = 8, s2 = 8, t1 = 0, t2 = 0;
    const bool a = RegQueryValueExW(key, L"LastUsedTimeStart", nullptr, &t1, reinterpret_cast<BYTE*>(&start), &s1) == ERROR_SUCCESS && t1 == REG_QWORD && s1 == 8;
    const bool b = RegQueryValueExW(key, L"LastUsedTimeStop", nullptr, &t2, reinterpret_cast<BYTE*>(&stop), &s2) == ERROR_SUCCESS && t2 == REG_QWORD && s2 == 8;
    (void)type; (void)size;
    if (a && b && stop == 0 && start != 0) {
        if (app && name) *app = AlCleanApp(name);
        return true;
    }
    if (depth <= 0) return false;
    wchar_t sub[512];
    for (DWORD i = 0;; i++) {
        DWORD len = 512;
        if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(key, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
            const bool r = AlKeyInUse(k, app, sub, depth - 1);
            RegCloseKey(k);
            if (r) return true;
        }
    }
    return false;
}

bool AlCapInUse(const wchar_t* cap, std::wstring* app) {
    wchar_t path[256];
    swprintf_s(path, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\%s", cap);
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY k;
        if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) continue;
        const bool r = AlKeyInUse(k, app, nullptr, 2);
        RegCloseKey(k);
        if (r) return true;
    }
    return false;
}

struct AlBtInfo { std::wstring name; int battery = -1; bool le = false; };
struct AlBtJob { std::wstring id; uint64_t addr; bool le; int tries; ULONGLONG next; };

class AlertsModule : public IModule {
public:
    const wchar_t* Id() const override { return L"alerts"; }
    void Start(Services& s) override;
    void Stop() override;
    Size2 Measure(View v) override;
    void Draw(const DrawCtx& c) override;
    bool OnPointer(const PointerEvent& e, View v) override;
    void OnClickAlert() override;
    // The slider's fill runs on its own spring; the frame clock runs only while it moves.
    bool Animating(View v) const override { return v == View::Alert && IsLevel() && !fill_.Settled(); }
    void OnSettings(const Settings&) override { pctLayout_.Reset(); }  // The font may have changed.
    void OnVisibility(View v, bool shown) override {
        if (v != View::Alert) return;
        visible_ = shown;
        if (!shown) hovered_ = false;
    }

    // ---- UI thread entry points ----
    void Enqueue(AItem it);
    void UpdateBtBattery(const std::wstring& id, int pct);
    void OnDnd(int value);
    void OnListenerOk(bool ok) { listenerOk_ = ok; UpdateToastFlag(); }
    void UpdateToastFlag();
    void PublishGeometry();
    void FullscreenChanged();
    int Dots() const { return (mic_ ? 1 : 0) + (cam_ ? 1 : 0); }
    // A slider, or the short "Sent to phone" that follows closing the shelf under the pointer.
    bool HoldsHover() const { return IsLevel() || (hasCur_ && cur_.kind == AK::Phone); }
    void DrawOverlay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float L, float R, float B, float s);
    LRESULT Wnd(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    void Present();
    void Next();
    void Tick();
    void Finish() { hasCur_ = false; Next(); }
    void OnCaps(bool on);
    void OnPower();
    void OnClipboard();
    void ReadClipboardNow();
    void OnDevice(WPARAM wp, LPARAM lp);
    void ReadPrivacy();
    void OsdShown(bool fromHook);
    void OsdHidden();
    void OnLevels(WPARAM wp, LPARAM lp);
    bool IsLevel() const { return hasCur_ && (cur_.kind == AK::Volume || cur_.kind == AK::Brightness); }
    void SetLevelAt(float x);
    void AimFill(int level);
    void Launch(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

    Services* sv_ = nullptr;
    HWND hwnd_ = nullptr;
    AItem cur_;
    bool hasCur_ = false;
    std::deque<AItem> pending_;
    int moreCount_ = 0;
    bool visible_ = false, hovered_ = false;
    bool listenerOk_ = false;
    int batPct_ = -1, batAc_ = -1, dndValue_ = 0;
    int clipTries_ = 0;
    bool osdSeen_ = false;
    // Own volume and brightness slider: last readings, to tell which one a key press changed.
    int lastVol_ = -1, lastBright_ = -1;
    bool lastMute_ = false, levelsPrimed_ = false, dragging_ = false;
    AK lastLevelKind_ = AK::Volume;
    float trackL_ = 0, trackR_ = 0;  // Slider track, pixels from the card's left edge.
    int levelFrom_ = -1;             // The reading before the one being shown, for the fill to start from.
    Spring fill_;                    // Fill fraction as drawn: springs to key presses, follows a drag at once.
    double fillT_ = 0;               // QPC time of the last fill step.
    Com<IDWriteTextLayout> pctLayout_;  // The percent, laid out once per value at the hover size.
    int pctLevel_ = -1;
    float pctSize_ = 0, pctScale_ = 0;
    bool mic_ = false, cam_ = false;
    std::wstring micApp_, camApp_;
    HPOWERNOTIFY powerA_ = nullptr, powerB_ = nullptr;
    HWINEVENTHOOK osdHook_ = nullptr;
    std::vector<HANDLE> threads_;
    RECT lastGeo_{};
    ULONGLONG lastCaps_ = 0;
    // Bluetooth worker state.
    std::mutex btLock_;
    std::map<std::wstring, AlBtInfo> btInfo_;
    std::vector<AlBtJob> btJobs_;
    std::map<std::wstring, ULONGLONG> btRecent_;
    friend DWORD WINAPI AlBtThread(void*);
    friend DWORD WINAPI AlNotifThread(void*);
    friend DWORD WINAPI AlCapsThread(void*);
    friend DWORD WINAPI AlPrivacyThread(void*);
    friend void CALLBACK AlOsdEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    friend AlNtStatus NTAPI AlWnfProc(ULONG64, ULONG, void*, void*, const void*, ULONG);
};

// ---- Queue ----

void AlertsModule::Present() {
    hovered_ = false;
    if (cur_.remaining <= 0) cur_.remaining = cur_.ms;
    sv_->ShowAlert(this, AlPriority(cur_.kind), 0);
    if (hwnd_) SetTimer(hwnd_, kAlTick, 100, nullptr);
}

void AlertsModule::Next() {
    if (!pending_.empty()) {
        cur_ = std::move(pending_.front());
        pending_.pop_front();
        hasCur_ = true;
        Present();
        return;
    }
    if (moreCount_ > 0) {  // The notifications that did not fit: one line, tap opens the bell.
        AItem it;
        it.kind = AK::Notification;
        it.more = true;
        it.title = AlFmt(AlT(L"alert.more", L"+%d more notifications"), moreCount_);
        it.sub = AlT(L"alert.more.sub", L"Tap to open the notification list");
        it.ms = 5000;
        moreCount_ = 0;
        cur_ = it;
        hasCur_ = true;
        Present();
        return;
    }
    hasCur_ = false;
    if (hwnd_) KillTimer(hwnd_, kAlTick);
    sv_->ClearAlert(this);
}

void AlertsModule::Enqueue(AItem it) {
    if (g_fullscreen || !sv_) return;  // The bar is hidden; toasts are back, nothing to show later.
    it.remaining = it.ms;
    switch (it.kind) {
        case AK::Volume:
        case AK::Brightness:
            if (hasCur_ && cur_.kind == it.kind) {  // Same slider: new value, full time again, no re-present.
                cur_.level = it.level;
                cur_.on = it.on;
                cur_.remaining = it.ms;
                AimFill(it.level);
                IslandRecompute();
                return;
            }
            if (hasCur_ && cur_.kind != AK::Clipboard && !IsLevel()) pending_.push_front(cur_);
            cur_ = it;
            hasCur_ = true;
            // A new card starts from the previous reading and springs to the new one.
            fill_.Snap(std::clamp((levelFrom_ >= 0 ? levelFrom_ : it.level) / 100.0f, 0.0f, 1.0f));
            AimFill(it.level);
            Present();
            return;
        case AK::Caps:
            if (hasCur_ && cur_.kind == AK::Caps) { cur_ = it; Present(); return; }
            // The interrupted alert resumes with the time it had left. A clipboard alert is just dropped.
            if (hasCur_ && cur_.kind != AK::Clipboard) pending_.push_front(cur_);
            cur_ = it;
            hasCur_ = true;
            Present();
            return;
        case AK::Clipboard:
            if (hasCur_ || !pending_.empty()) return;
            break;
        case AK::Notification: {
            if (hasCur_ && cur_.kind == AK::Clipboard) { cur_ = it; Present(); return; }
            int n = 0;
            for (const AItem& p : pending_) if (p.kind == AK::Notification) n++;
            if (n >= 5) { moreCount_++; return; }  // Never dropped silently: counted, then shown as one line.
            pending_.push_back(it);
            if (!hasCur_) Next();
            return;
        }
        default: {
            if (hasCur_ && cur_.kind == it.kind && !cur_.more) { cur_ = it; Present(); return; }
            for (AItem& p : pending_) if (p.kind == it.kind) { p = it; return; }
            pending_.push_back(it);
            if (!hasCur_) Next();
            return;
        }
    }
    cur_ = it;
    hasCur_ = true;
    Present();
}

void AlertsModule::Tick() {
    if (!hasCur_) { KillTimer(hwnd_, kAlTick); return; }
    // Waiting, reading, or under the volume popup. The phone confirmation runs out under the pointer too.
    if (!visible_ || (hovered_ && cur_.kind != AK::Phone) || g_covered) return;
    cur_.remaining -= 100;
    if (cur_.remaining <= 0) Finish();
}

void AlertsModule::UpdateBtBattery(const std::wstring& id, int pct) {
    auto apply = [&](AItem& a) {
        if (a.kind != AK::Bluetooth || a.key != id || pct < 0) return false;
        a.level = pct;
        a.sub = AlFmt(AlT(L"alert.bt.connected.bat", L"Connected, battery %d%%"), pct);
        return true;
    };
    if (hasCur_ && apply(cur_)) { sv_->Invalidate(); return; }
    for (AItem& p : pending_) if (apply(p)) return;
}

// ---- Sources ----

void AlertsModule::OnCaps(bool on) {
    const ULONGLONG now = GetTickCount64();
    if (now - lastCaps_ < 80) return;
    lastCaps_ = now;
    AItem it;
    it.kind = AK::Caps;
    it.on = on;
    it.title = on ? AlT(L"alert.caps.on", L"Caps Lock on") : AlT(L"alert.caps.off", L"Caps Lock off");
    it.sub = on ? AlT(L"alert.caps.tap", L"Tap to turn off") : L"";
    it.hint = on ? L"" : L"";
    it.danger = on;
    it.ms = 2500;
    Enqueue(it);
}

void AlertsModule::OnPower() {
    SYSTEM_POWER_STATUS st{};
    if (!GetSystemPowerStatus(&st) || st.BatteryFlag == 128 || st.BatteryLifePercent > 100) return;
    const int pct = st.BatteryLifePercent;
    const int ac = st.ACLineStatus == 1 ? 1 : 0;
    if (batPct_ < 0) { batPct_ = pct; batAc_ = ac; return; }  // The first read is a baseline, not an event.
    AItem it;
    it.kind = AK::Battery;
    it.level = pct;
    it.charging = ac != 0;
    it.ms = 4000;
    std::wstring left;
    if (!ac && st.BatteryLifeTime != (DWORD)-1) {
        const DWORD m = st.BatteryLifeTime / 60;
        left = AlFmt(L", %uh %02um", m / 60, m % 60);
    }
    bool fire = false;
    if (ac != batAc_) {
        fire = true;
        it.title = ac ? AlT(L"alert.power.on", L"Power connected") : AlT(L"alert.power.off", L"On battery power");
        it.sub = AlFmt(L"%d%%", pct) + left;
        it.good = ac != 0;
    } else if (!ac && ((batPct_ > 20 && pct <= 20) || (batPct_ > 10 && pct <= 10))) {  // Crossed, not equal: sleep can skip a value.
        fire = true;
        it.title = pct <= 10 ? AlT(L"alert.bat.crit", L"Battery critical") : AlT(L"alert.bat.low", L"Battery low");
        it.sub = AlFmt(L"%d%%", pct) + left;
        it.danger = true;
        it.ms = 6000;
    }
    batPct_ = pct;
    batAc_ = ac;
    if (fire) Enqueue(it);
}

void AlertsModule::OnDnd(int value) {
    if (g_alDndFirst.exchange(false)) { dndValue_ = value; return; }  // The first callback only reports the current state.
    if (value == dndValue_) return;
    dndValue_ = value;
    if (g_fullscreen) return;  // Automatic rules (games, fullscreen) flip it; not worth an alert.
    AItem it;
    it.kind = AK::Dnd;
    it.on = value != 0;
    it.title = value ? AlT(L"alert.dnd.on", L"Do Not Disturb on") : AlT(L"alert.dnd.off", L"Do Not Disturb off");
    it.sub = value ? AlT(L"alert.dnd.tapoff", L"Tap to turn off") : L"";
    it.ms = 3000;
    Enqueue(it);
}

void AlertsModule::OnDevice(WPARAM wp, LPARAM lp) {
    if ((wp != DBT_DEVICEARRIVAL && wp != DBT_DEVICEREMOVECOMPLETE) || !lp) return;
    const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
    const bool arrive = wp == DBT_DEVICEARRIVAL;
    AItem it;
    it.kind = AK::Device;
    it.ms = 3000;
    it.good = arrive;
    it.hint = AlT(L"alert.device.tap", L"Tap to open USB settings");
    if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
        const auto* vol = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp);
        if (vol->dbcv_flags & (DBTF_NET | DBTF_MEDIA)) return;  // Network drives and discs are not USB.
        wchar_t letter = 0;
        for (int i = 0; i < 26; i++) if (vol->dbcv_unitmask & (1u << i)) { letter = L'A' + i; break; }
        if (!letter) return;
        std::wstring label;
        if (arrive) {
            wchar_t root[4] = {letter, L':', L'\\', 0};
            wchar_t name[64] = {};
            if (GetVolumeInformationW(root, name, 64, nullptr, nullptr, nullptr, nullptr, 0)) label = name;
        }
        it.title = arrive ? AlT(L"alert.drive.in", L"Drive connected") : AlT(L"alert.drive.out", L"Drive removed");
        it.sub = label.empty() ? AlFmt(L"%c:", letter) : AlFmt(L"%s (%c:)", label.c_str(), letter);
    } else if (hdr->dbch_devicetype == DBT_DEVTYP_PORT) {
        const auto* port = reinterpret_cast<const DEV_BROADCAST_PORT_W*>(lp);
        it.title = arrive ? AlT(L"alert.port.in", L"Serial port connected") : AlT(L"alert.port.out", L"Serial port removed");
        it.sub = port->dbcp_name;
    } else {
        return;
    }
    Enqueue(it);
}

void AlertsModule::ReadClipboardNow() {
    if (!OpenClipboard(hwnd_)) {
        if (++clipTries_ < 6) SetTimer(hwnd_, kAlClipboard, 40, nullptr);  // The owner may still hold it.
        return;
    }
    clipTries_ = 0;
    AItem it;
    it.kind = AK::Clipboard;
    it.ms = 2500;
    it.hint = AlT(L"alert.clip.tap", L"Tap to open clipboard history");
    // Password managers mark their data so history and monitors skip it: show no content then.
    bool priv = false;
    static const UINT fmtExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT fmtHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(fmtExclude)) priv = true;
    if (IsClipboardFormatAvailable(fmtHistory)) {
        if (HANDLE h = GetClipboardData(fmtHistory)) {
            if (const DWORD* p = static_cast<const DWORD*>(GlobalLock(h))) {
                if (GlobalSize(h) >= sizeof(DWORD) && *p == 0) priv = true;
                GlobalUnlock(h);
            }
        }
    }
    std::wstring text;
    int files = 0;
    bool image = false;
    if (!priv) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    const size_t max = GlobalSize(h) / sizeof(wchar_t);
                    for (size_t i = 0; i < max && p[i] && text.size() < 96; i++) text += (p[i] == L'\r' || p[i] == L'\n' || p[i] == L'\t') ? L' ' : p[i];
                    GlobalUnlock(h);
                }
            }
        } else if (IsClipboardFormatAvailable(CF_HDROP)) {
            if (HANDLE h = GetClipboardData(CF_HDROP)) files = (int)DragQueryFileW(static_cast<HDROP>(h), 0xFFFFFFFF, nullptr, 0);
        } else if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP)) {
            image = true;
        }
    }
    HWND owner = GetClipboardOwner();
    CloseClipboard();
    it.title = AlT(L"alert.clip", L"Copied");
    if (!priv) {
        if (!text.empty()) it.sub = text;
        else if (files) it.sub = AlFmt(AlT(L"alert.clip.files", L"%d files"), files);
        else if (image) it.sub = AlT(L"alert.clip.image", L"Image");
    }
    if (!owner) owner = GetForegroundWindow();
    if (owner) {
        const std::wstring exe = AlExeOfWindow(owner);
        const wchar_t* base = wcsrchr(exe.c_str(), L'\\');
        base = base ? base + 1 : exe.c_str();
        if (!exe.empty() && _wcsicmp(base, L"explorer.exe") && !wcsstr(base, L"indhawk")) it.icon = AlIconOfExe(exe.c_str());
    }
    Enqueue(it);
}

void AlertsModule::OnClipboard() {
    clipTries_ = 0;
    SetTimer(hwnd_, kAlClipboard, 100, nullptr);  // Apps fire several updates per copy: coalesce.
}

void AlertsModule::ReadPrivacy() {
    std::wstring m, c;
    const bool mic = AlCapInUse(L"microphone", &m);
    const bool cam = AlCapInUse(L"webcam", &c);
    if (mic == mic_ && cam == cam_) return;
    mic_ = mic;
    cam_ = cam;
    micApp_ = m;
    camApp_ = c;
    IslandRecompute();  // The pill makes room for the dots.
}

void AlertsModule::OsdShown(bool fromHook) {
    if (!fromHook && osdSeen_) return;
    if (fromHook) osdSeen_ = true;
    SetCovered(true);
    SetTimer(hwnd_, kAlOsd, 3500, nullptr);  // Fallback: the popup's hide is not always an event.
}

void AlertsModule::OsdHidden() {
    KillTimer(hwnd_, kAlOsd);
    SetCovered(false);
}

void AlertsModule::OnLevels(WPARAM wp, LPARAM lp) {
    const int vol = (int)(short)(wp & 0xFFFF);
    const bool mute = (wp & 0x10000) != 0;
    const int bright = (int)lp;
    if (!levelsPrimed_) {  // The reading taken at start: a baseline, nothing to show.
        levelsPrimed_ = true;
        lastVol_ = vol; lastMute_ = mute; lastBright_ = bright;
        return;
    }
    AK kind = lastLevelKind_;
    if (vol >= 0 && (vol != lastVol_ || mute != lastMute_)) kind = AK::Volume;
    else if (bright >= 0 && bright != lastBright_) kind = AK::Brightness;
    levelFrom_ = kind == AK::Volume ? lastVol_ : lastBright_;
    lastVol_ = vol; lastMute_ = mute; lastBright_ = bright;
    if (dragging_) return;  // The card already shows what the drag set.
    lastLevelKind_ = kind;
    AItem it;
    it.kind = kind;
    it.level = kind == AK::Volume ? vol : bright;
    it.on = kind == AK::Volume && mute;
    it.ms = 2000;
    if (it.level < 0) return;
    Enqueue(it);
}

void AlertsModule::AimFill(int level) {
    const float to = std::clamp(level / 100.0f, 0.0f, 1.0f);
    if (fill_.Settled() && fabsf(fill_.x - to) < fill_.eps) return;
    if (fill_.Settled()) fillT_ = QpcSeconds();  // Moving again after a rest: no stale step.
    fill_.Aim(to, 300.0f, 34.0f);                // About critically damped, settles in some 250 ms.
    if (sv_) sv_->WakeAnimation();
}

void AlertsModule::SetLevelAt(float x) {
    if (!IsLevel() || trackR_ <= trackL_) return;
    const float frac = std::clamp((x - trackL_) / (trackR_ - trackL_), 0.0f, 1.0f);
    fill_.Snap(frac);  // A drag is followed exactly, between whole percents too.
    const int v = (int)std::lround(frac * 100.0f);
    if (v == cur_.level) { sv_->Invalidate(); return; }
    cur_.level = v;
    cur_.remaining = cur_.ms;
    if (cur_.kind == AK::Volume) { g_alSetVolume = v; lastVol_ = v; cur_.on = false; }
    else { g_alSetBright = v; lastBright_ = v; }
    if (g_alLevelSet) SetEvent(g_alLevelSet);
    IslandRecompute();
}

// ---- Island-facing parts ----

Size2 AlertsModule::Measure(View v) {
    if (v != View::Alert || !hasCur_) return {40.0f, kNotchH};
    if (IsLevel()) return hovered_ ? Size2{kAlLevelHoverW + 2.0f * AlertsDotsReserve(), kAlLevelHoverH}  // Bigger to drag.
                                   : Size2{kAlLevelW + 2.0f * AlertsDotsReserve(), kAlLevelH};
    float w = 300.0f;
    switch (cur_.kind) {
        case AK::Caps: w = 210.0f; break;
        case AK::Notification: w = 360.0f; break;
        case AK::Clipboard: w = 340.0f; break;
        case AK::Device: w = 300.0f; break;
        case AK::Phone: w = 250.0f; break;
        default: break;
    }
    return {w + 2.0f * AlertsDotsReserve(), hovered_ && cur_.kind != AK::Phone ? 66.0f : 44.0f};
}

bool AlertsModule::OnPointer(const PointerEvent& e, View v) {
    if (v != View::Alert) return false;
    if (IsLevel()) {  // Press or drag on the card sets the level; hovering holds it open and grows it.
        if (e.kind == PointerEvent::Down && e.left) { dragging_ = true; SetLevelAt(e.pt.x); }
        else if (e.kind == PointerEvent::Move && dragging_) SetLevelAt(e.pt.x);
        else if (e.kind == PointerEvent::Up) dragging_ = false;
        else if (e.kind == PointerEvent::Leave) {
            dragging_ = false;
            if (hovered_) {
                hovered_ = false;
                cur_.remaining = cur_.ms;  // The dismiss timer starts over from leaving.
                IslandRecompute();
            }
        }
        if (e.kind == PointerEvent::Move && !hovered_) { hovered_ = true; IslandRecompute(); }
        return true;
    }
    if (e.kind == PointerEvent::Move && !hovered_) { hovered_ = true; IslandRecompute(); }
    else if (e.kind == PointerEvent::Leave && hovered_) { hovered_ = false; IslandRecompute(); }
    return false;
}

void AlertsModule::Draw(const DrawCtx& c) {
    // Once finished, the card is still drawn while the island fades it out, so it leaves with the body.
    if (c.view != View::Alert || (!hasCur_ && c.alpha >= 0.999f)) return;
    const float s = c.scale;
    const D2D1_RECT_F r = c.rect;
    const float badge = 28.0f * s, bx = r.left + 10.0f * s, by = r.top + 8.0f * s;
    auto col = [&](D2D1_COLOR_F k, float a = 1.0f) { k.a *= a * c.alpha; c.brush->SetColor(k); };

    if (cur_.kind == AK::Volume || cur_.kind == AK::Brightness) {  // Glyph, track with fill, percent.
        const bool vol = cur_.kind == AK::Volume;
        // Lay out in the body as it stands now, so the hover growth rides the island's size springs:
        // g goes 0 to 1 as the body grows from the normal card to the hover card.
        const float ew = std::min(r.right - r.left, std::max(g_isW.x, 1.0f));
        const float eh = std::min(r.bottom - r.top, std::max(g_isH.x, 1.0f));
        const float g = Clamp01((eh / s - kAlLevelH) / (kAlLevelHoverH - kAlLevelH));
        const float left = (r.left + r.right) / 2.0f - ew / 2.0f, right = left + ew;
        const float gs = s * (1.0f + 0.4f * g);  // Glyph scale.
        const float lw = 1.4f * gs;
        const float cx = left + (24.0f + 6.0f * g) * s, cy = r.top + eh / 2.0f;
        col(kFg);
        if (vol) {
            D2D1_POINT_2F p[] = {{cx - 8 * gs, cy - 3 * gs}, {cx - 4 * gs, cy - 3 * gs}, {cx + 1 * gs, cy - 7 * gs}, {cx + 1 * gs, cy + 7 * gs}, {cx - 4 * gs, cy + 3 * gs}, {cx - 8 * gs, cy + 3 * gs}};
            for (int i = 0; i < 6; i++) c.rt->DrawLine(p[i], p[(i + 1) % 6], c.brush, lw);
            if (cur_.on) {  // Muted: a cross instead of waves.
                c.rt->DrawLine(D2D1::Point2F(cx + 4 * gs, cy - 3 * gs), D2D1::Point2F(cx + 10 * gs, cy + 3 * gs), c.brush, lw);
                c.rt->DrawLine(D2D1::Point2F(cx + 10 * gs, cy - 3 * gs), D2D1::Point2F(cx + 4 * gs, cy + 3 * gs), c.brush, lw);
            } else if (cur_.level > 0) {
                c.rt->DrawLine(D2D1::Point2F(cx + 5 * gs, cy - 3 * gs), D2D1::Point2F(cx + 5 * gs, cy + 3 * gs), c.brush, lw);
                if (cur_.level > 50) c.rt->DrawLine(D2D1::Point2F(cx + 9 * gs, cy - 6 * gs), D2D1::Point2F(cx + 9 * gs, cy + 6 * gs), c.brush, lw);
            }
        } else {
            c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.5f * gs, 3.5f * gs), c.brush, lw);
            for (int i = 0; i < 8; i++) {
                const float a = i * 3.14159265f / 4.0f, ca = std::cos(a), sa = std::sin(a);
                c.rt->DrawLine(D2D1::Point2F(cx + ca * 6.0f * gs, cy + sa * 6.0f * gs), D2D1::Point2F(cx + ca * 8.5f * gs, cy + sa * 8.5f * gs), c.brush, lw);
            }
        }
        const float tl = left + (46.0f + 14.0f * g) * s, th = (6.0f + 6.0f * g) * s;
        const float tr = std::max(tl, right - (52.0f + 8.0f * g + 12.0f * Dots()) * s);  // Never inverted while the body is narrow.
        trackL_ = tl - r.left;  // Pointer x is measured from the view's left edge, r.left.
        trackR_ = tr - r.left;
        if (!fill_.Settled()) {  // Stepped by real time, here, so it moves at the display's rate.
            double dt = c.now - fillT_;
            if (dt > 0.05) dt = 0.05;
            if (dt > 0) fill_.Step(dt);
        }
        fillT_ = c.now;
        const float frac = std::clamp(fill_.x, 0.0f, 1.0f);
        col(Rgb(255, 255, 255, 0x2e));
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, cy - th / 2, tr, cy + th / 2), th / 2, th / 2), c.brush);
        if (frac > 0) {
            col(kFg, cur_.on ? 0.4f : 1.0f);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, cy - th / 2, tl + std::max(th, (tr - tl) * frac), cy + th / 2), th / 2, th / 2), c.brush);
        }
        // The percent is laid out once per value at the hover size and scaled down: no new layout per frame.
        const float big = g_text.DefaultSize() * 1.3f;
        if (!pctLayout_ || pctLevel_ != cur_.level || pctSize_ != big || pctScale_ != s) {
            TextOpts o;
            o.maxW = 48.0f * s;
            o.maxH = 26.0f * s;
            o.size = big;
            pctLayout_ = g_text.Layout(std::to_wstring(cur_.level), o);
            pctLevel_ = cur_.level;
            pctSize_ = big;
            pctScale_ = s;
        }
        if (pctLayout_) {
            const float k = (1.0f + 0.3f * g) / 1.3f, x0 = tr + 10.0f * s;
            D2D1_MATRIX_3X2_F old;
            c.rt->GetTransform(&old);
            c.rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(x0, cy)) * old);
            col(kFg, 0.8f);
            c.rt->DrawTextLayout(D2D1::Point2F(x0, cy - 13.0f * s), pctLayout_.Get(), c.brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            c.rt->SetTransform(old);
        }
        return;
    }

    // Badge: app icon, or a glyph for the kind.
    const D2D1_RECT_F br = D2D1::RectF(bx, by, bx + badge, by + badge);
    if (cur_.icon && cur_.icon->w > 0) {
        if (ID2D1Bitmap* bmp = AlBitmap(c.rt, *cur_.icon)) c.rt->DrawBitmap(bmp, br, c.alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        col(Rgb(255, 255, 255, 0x1e));
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(br, 8.0f * s, 8.0f * s), c.brush);
        col(kFg);
        const float cx = bx + badge / 2, cy = by + badge / 2;
        TextOpts o;
        o.center = true;
        o.maxW = badge;
        o.maxH = badge;
        switch (cur_.kind) {
            case AK::Battery: {
                const int p = cur_.level;
                g_icons.Draw(c.rt, c.brush, p < 10 ? I_batwarn : p < 30 ? I_batlow : p < 70 ? I_batmed : p < 95 ? I_bathigh : I_batfull, cx, cy, 16.0f * s);
                if (cur_.charging) g_icons.Draw(c.rt, c.brush, I_bolt, cx + 8.0f * s, cy - 6.0f * s, 9.0f * s);
                break;
            }
            case AK::Bluetooth: g_icons.Draw(c.rt, c.brush, I_bluetooth, cx, cy, 16.0f * s); break;
            case AK::Notification: g_icons.Draw(c.rt, c.brush, I_bell, cx, cy, 16.0f * s); break;
            case AK::Caps: o.size = 15.0f * s; g_text.Draw(c.rt, L"A", bx, by, c.brush, o); break;
            case AK::Dnd: o.size = 15.0f * s; g_text.Draw(c.rt, L"☾", bx, by, c.brush, o); break;
            case AK::Device: o.size = 9.0f * s; g_text.Draw(c.rt, wcsstr(cur_.title.c_str(), L"erial") ? L"COM" : L"USB", bx, by, c.brush, o); break;
            case AK::Phone:
                c.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 5.0f * s, cy - 8.0f * s, cx + 5.0f * s, cy + 8.0f * s), 2.5f * s, 2.5f * s), c.brush, 1.4f * s);
                c.rt->DrawLine(D2D1::Point2F(cx - 1.5f * s, cy + 5.0f * s), D2D1::Point2F(cx + 1.5f * s, cy + 5.0f * s), c.brush, 1.2f * s);
                break;
            case AK::Volume:
            case AK::Brightness:
                break;
            case AK::Clipboard:
                c.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 6.0f * s, cy - 7.0f * s, cx + 6.0f * s, cy + 8.0f * s), 2.0f * s, 2.0f * s), c.brush, 1.4f * s);
                c.rt->DrawLine(D2D1::Point2F(cx - 3.0f * s, cy), D2D1::Point2F(cx + 3.0f * s, cy), c.brush, 1.2f * s);
                c.rt->DrawLine(D2D1::Point2F(cx - 3.0f * s, cy + 3.5f * s), D2D1::Point2F(cx + 3.0f * s, cy + 3.5f * s), c.brush, 1.2f * s);
                break;
        }
    }
    if (cur_.danger || cur_.good) {  // Status dot: the only colour in the cards.
        col(cur_.danger ? Rgb(255, 69, 58) : Rgb(48, 209, 88));
        c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx + badge - 1.0f * s, by + badge - 1.0f * s), 4.0f * s, 4.0f * s), c.brush);
    }

    const float tx = bx + badge + 10.0f * s;
    const float reserve = (12.0f + 12.0f * Dots()) * s;
    TextOpts o;
    o.ellipsis = true;
    o.maxW = std::max(10.0f, r.right - reserve - tx);
    o.maxH = 18.0f * s;
    col(kFg);
    g_text.Draw(c.rt, cur_.title, tx, r.top + 6.0f * s, c.brush, o);
    if (!cur_.sub.empty()) {
        col(kFg, 0.62f);
        o.size = g_text.DefaultSize() * 0.9f;
        g_text.Draw(c.rt, cur_.sub, tx, r.top + 24.0f * s, c.brush, o);
    }
    if (hovered_ && !cur_.hint.empty()) {
        col(kFg, 0.45f);
        o.size = g_text.DefaultSize() * 0.85f;
        g_text.Draw(c.rt, cur_.hint, tx, r.top + 44.0f * s, c.brush, o);
    }
}

void AlertsModule::OnClickAlert() {
    if (!hasCur_ || IsLevel()) return;  // A slider is set by pressing on it, not dismissed.
    const AItem it = cur_;
    switch (it.kind) {
        case AK::Caps:
            if (it.on) {  // Turn it off. UIPI blocks this when an elevated window has focus: then nothing changes.
                INPUT in[2]{};
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = VK_CAPITAL;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
            }
            break;
        case AK::Battery: Launch(L"ms-settings:batterysaver"); break;
        case AK::Bluetooth: Launch(L"ms-settings:bluetooth"); break;
        case AK::Device: Launch(L"ms-settings:usb"); break;
        case AK::Dnd:  // Turn it off (levels.inc, AlDndOff); Settings opens only if that fails.
            if (!it.on) break;
            if (g_alDndOff) SetEvent(g_alDndOff);
            else Launch(L"ms-settings:quietmomentshome");
            break;
        case AK::Clipboard: SendWinKey('V'); break;
        case AK::Phone:
        case AK::Volume:
        case AK::Brightness:
            break;
        case AK::Notification:
            if (it.more) {
                RECT body = g_isBody;
                sv_->OpenPanel(PanelKind::Notifications, (body.left + body.right) / 2);
            } else {
                if (!it.key.empty()) Launch((L"shell:AppsFolder\\" + it.key).c_str());
                if (it.nid && g_alListener) {  // Tapped means handled: it leaves the bell list too.
                    const uint32_t id = it.nid;
                    try { g_alListener.RemoveNotification(id); } catch (...) {}
                }
            }
            break;
    }
    Finish();
}

void AlertsModule::DrawOverlay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float L, float R, float B, float s) {
    if (!Dots() || B > 55.0f * s) return;
    float x = R - 12.0f * s;
    const float y = B / 2.0f;
    auto dot = [&](D2D1_COLOR_F c) {
        brush->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 4.0f * s, 4.0f * s), brush);
        x -= 12.0f * s;
    };
    if (mic_) dot(Rgb(255, 159, 10));
    if (cam_) dot(Rgb(48, 209, 88));
}

void AlertsModule::UpdateToastFlag() {
    if (!g_hwnd) return;
    if (listenerOk_ && !g_fullscreen) {
        SetPropW(g_hwnd, L"TopBarToastsOk", reinterpret_cast<HANDLE>(1));
        SetPropW(g_hwnd, L"TopBarToastsTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
    } else {
        RemovePropW(g_hwnd, L"TopBarToastsOk");
    }
}

void AlertsModule::FullscreenChanged() {
    if (g_fullscreen) {  // Toasts are back; whatever is queued would be stale when the bar returns.
        pending_.clear();
        moreCount_ = 0;
        hasCur_ = false;
        if (hwnd_) KillTimer(hwnd_, kAlTick);
        if (sv_) sv_->ClearAlert(this);
    }
    UpdateToastFlag();
}

// The collapsed notch's rectangle in screen pixels, for the popup hook. Widened to what a slider needs.
void AlertsModule::PublishGeometry() {
    if (!g_hwnd || g_monitor.right <= g_monitor.left) return;
    const float s = Scale();
    float w = 196.0f;
    if (IModule* pill = PillModule()) w = std::max(w, pill->Measure(View::Pill).w);
    const int cx = (g_monitor.left + g_monitor.right) / 2;
    const RECT r{(LONG)(cx - w * s / 2.0f), g_monitor.top, (LONG)(cx + w * s / 2.0f), g_monitor.top + (LONG)(kNotchH * s + 0.5f)};
    if (EqualRect(&r, &lastGeo_)) return;
    lastGeo_ = r;
    SetPropW(g_hwnd, L"TopBarCentreLeft", reinterpret_cast<HANDLE>((INT_PTR)r.left));
    SetPropW(g_hwnd, L"TopBarCentreTop", reinterpret_cast<HANDLE>((INT_PTR)r.top));
    SetPropW(g_hwnd, L"TopBarCentreRight", reinterpret_cast<HANDLE>((INT_PTR)r.right));
    SetPropW(g_hwnd, L"TopBarCentreBottom", reinterpret_cast<HANDLE>((INT_PTR)r.bottom));
    Wh_Log(L"alerts: popup rect published %ld,%ld-%ld,%ld (px)", r.left, r.top, r.right, r.bottom);
}

// ---- Workers ----

LRESULT CALLBACK AlCapsHook(int code, WPARAM wp, LPARAM lp) {
    // System-wide input path: post and return, nothing else.
    if (code == HC_ACTION && (wp == WM_KEYUP || wp == WM_SYSKEYUP)) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->vkCode == VK_CAPITAL) {
            if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_CAPS, 0, (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI AlCapsThread(void*) {
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, AlCapsHook, GetModuleHandleW(nullptr), 0);
    // A hook dies with its thread: pump until unload.
    for (;;) {
        if (MsgWaitForMultipleObjects(1, &g_alStop, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0 + 1) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

DWORD WINAPI AlPrivacyThread(void*) {
    static const wchar_t* kCaps[] = {L"microphone", L"webcam"};
    HKEY keys[4] = {};
    HANDLE ev[5] = {g_alStop};
    int n = 0;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const wchar_t* cap : kCaps) {
            wchar_t path[256];
            swprintf_s(path, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\%s", cap);
            HKEY k;
            if (RegOpenKeyExW(root, path, 0, KEY_NOTIFY | KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) continue;
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, e, TRUE) != ERROR_SUCCESS) { CloseHandle(e); RegCloseKey(k); continue; }
            keys[n] = k;
            ev[n + 1] = e;
            n++;
        }
    }
    if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_PRIVACY, 0, 0);
    while (n > 0) {
        const DWORD w = WaitForMultipleObjects(n + 1, ev, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
        const int i = (int)(w - WAIT_OBJECT_0) - 1;
        if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_PRIVACY, 0, 0);
        if (i >= 0 && i < n) RegNotifyChangeKeyValue(keys[i], TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, ev[i + 1], TRUE);
    }
    for (int i = 0; i < n; i++) { RegCloseKey(keys[i]); CloseHandle(ev[i + 1]); }
    return 0;
}

AlNtStatus NTAPI AlWnfProc(ULONG64 name, ULONG, void*, void*, const void* buf, ULONG size) {
    if (name != kAlWnfQuietHours) return 0;
    const int v = (buf && size >= sizeof(int)) ? *static_cast<const int*>(buf) : 0;
    g_services.Post([v] { if (g_alMod) g_alMod->OnDnd(v); });
    return 0;
}

// Bluetooth battery: the devnode property Settings uses, then the GATT battery service.
int AlBtBatteryClassic(uint64_t addr) {
    if (!addr) return -1;
    wchar_t hex[16];
    swprintf_s(hex, L"%012llX", (unsigned long long)addr);
    std::wstring want = hex;
    std::transform(want.begin(), want.end(), want.begin(), ::towlower);
    HDEVINFO set = SetupDiGetClassDevsExW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT, nullptr, nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) return -1;
    static const DEVPROPKEY key = {{0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
    int result = -1;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        wchar_t inst[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &d, inst, 512, nullptr)) continue;
        std::wstring low = inst;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        if (low.find(want) == std::wstring::npos) continue;
        DEVPROPTYPE type = 0;
        BYTE b = 0;
        DWORD need = 0;
        if (SetupDiGetDevicePropertyW(set, &d, &key, &type, &b, sizeof(b), &need, 0) && type == DEVPROP_TYPE_BYTE && b != 0xFF) {
            result = std::min<int>(b, 100);
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

int AlBtBatteryGatt(const winrt::hstring& id) {
    using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
    try {
        auto dev = wbt::BluetoothLEDevice::FromIdAsync(id).get();
        if (!dev) return -1;
        auto svc = dev.GetGattServicesForUuidAsync(GattServiceUuids::Battery(), wbt::BluetoothCacheMode::Uncached).get();
        if (svc.Status() != GattCommunicationStatus::Success || svc.Services().Size() == 0) return -1;
        auto ch = svc.Services().GetAt(0).GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel(), wbt::BluetoothCacheMode::Uncached).get();
        if (ch.Status() != GattCommunicationStatus::Success || ch.Characteristics().Size() == 0) return -1;
        auto rd = ch.Characteristics().GetAt(0).ReadValueAsync(wbt::BluetoothCacheMode::Uncached).get();
        if (rd.Status() != GattCommunicationStatus::Success || rd.Value().Length() < 1) return -1;
        return std::min<int>(winrt::Windows::Storage::Streams::DataReader::FromBuffer(rd.Value()).ReadByte(), 100);
    } catch (...) {
        return -1;
    }
}

uint64_t AlBtAddress(const std::wstring& id) {
    const size_t p = id.rfind(L'-');
    if (p == std::wstring::npos || id.size() - p - 1 != 17) return 0;
    uint64_t a = 0;
    for (size_t i = p + 1; i < id.size(); i++) {
        if (id[i] == L':') continue;
        const wchar_t ch = id[i];
        const int v = ch >= L'0' && ch <= L'9' ? ch - L'0' : (ch | 0x20) >= L'a' && (ch | 0x20) <= L'f' ? (ch | 0x20) - L'a' + 10 : -1;
        if (v < 0) return 0;
        a = a * 16 + v;
    }
    return a;
}

DWORD WINAPI AlBtThread(void*) {
    AlertsModule& m = *g_alMod;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto make = [&](const winrt::hstring& selector, bool le) {
            auto watcher = wde::DeviceInformation::CreateWatcher(selector);
            auto ready = std::make_shared<std::atomic<bool>>(false);  // Devices listed before the enumeration ends are not events.
            watcher.EnumerationCompleted([ready](auto&&, auto&&) { *ready = true; });
            watcher.Added([&m, ready, le](wde::DeviceWatcher const&, wde::DeviceInformation const& info) {
                if (!*ready) {
                    std::lock_guard lock(m.btLock_);
                    m.btInfo_[info.Id().c_str()] = {info.Name().c_str(), -1, le};
                    return;
                }
                const std::wstring id = info.Id().c_str(), name = info.Name().c_str();
                if (name.empty()) return;
                const ULONGLONG now = GetTickCount64();
                {
                    std::lock_guard lock(m.btLock_);
                    ULONGLONG& last = m.btRecent_[name];
                    const bool dup = now - last < 1000;  // A dual-mode device shows on both watchers.
                    last = now;
                    m.btInfo_[id] = {name, -1, le};
                    if (dup) return;
                    m.btJobs_.push_back({id, AlBtAddress(id), le, 0, now + 300});
                }
                g_services.Post([id, name] {
                    AItem it;
                    it.kind = AK::Bluetooth;
                    it.key = id;
                    it.title = name;
                    it.sub = AlT(L"alert.bt.connected", L"Connected");
                    it.hint = AlT(L"alert.bt.tap", L"Tap to open Bluetooth settings");
                    it.good = true;
                    it.ms = 4000;
                    if (g_alMod) g_alMod->Enqueue(it);
                });
            });
            watcher.Removed([&m, ready](wde::DeviceWatcher const&, wde::DeviceInformationUpdate const& upd) {
                if (!*ready) return;
                const std::wstring id = upd.Id().c_str();
                AlBtInfo info;
                const ULONGLONG now = GetTickCount64();
                {
                    std::lock_guard lock(m.btLock_);
                    auto f = m.btInfo_.find(id);  // Removed carries only the id.
                    if (f == m.btInfo_.end() || f->second.name.empty()) return;
                    info = f->second;
                    ULONGLONG& last = m.btRecent_[info.name + L"\x01off"];
                    if (now - last < 1000) return;
                    last = now;
                    m.btJobs_.erase(std::remove_if(m.btJobs_.begin(), m.btJobs_.end(), [&](const AlBtJob& j) { return j.id == id; }), m.btJobs_.end());
                }
                g_services.Post([id, info] {
                    AItem it;
                    it.kind = AK::Bluetooth;
                    it.key = id + L"\x01off";
                    it.title = info.name;
                    it.sub = info.battery >= 0 ? AlFmt(AlT(L"alert.bt.disconnected.bat", L"Disconnected, last battery %d%%"), info.battery)
                                               : AlT(L"alert.bt.disconnected", L"Disconnected");
                    it.hint = AlT(L"alert.bt.tap", L"Tap to open Bluetooth settings");
                    it.level = info.battery;
                    it.ms = 4000;
                    if (g_alMod) g_alMod->Enqueue(it);
                });
            });
            watcher.Start();
            return watcher;
        };
        auto w1 = make(wbt::BluetoothDevice::GetDeviceSelectorFromConnectionStatus(wbt::BluetoothConnectionStatus::Connected), false);
        auto w2 = make(wbt::BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(wbt::BluetoothConnectionStatus::Connected), true);
        // One loop serves the battery retries (6 tries, 1.5 s apart: the level appears late) and waits on the stop event.
        while (WaitForSingleObject(g_alStop, 250) == WAIT_TIMEOUT) {
            AlBtJob job{};
            bool have = false;
            const ULONGLONG now = GetTickCount64();
            {
                std::lock_guard lock(m.btLock_);
                for (size_t i = 0; i < m.btJobs_.size(); i++) {
                    if (m.btJobs_[i].next <= now) { job = m.btJobs_[i]; m.btJobs_.erase(m.btJobs_.begin() + i); have = true; break; }
                }
            }
            if (!have) continue;
            int pct = AlBtBatteryClassic(job.addr);
            if (pct < 0 && job.le) pct = AlBtBatteryGatt(job.id.c_str());
            if (pct >= 0) {
                { std::lock_guard lock(m.btLock_); m.btInfo_[job.id].battery = pct; }
                const std::wstring id = job.id;
                g_services.Post([id, pct] { if (g_alMod) g_alMod->UpdateBtBattery(id, pct); });
            } else if (++job.tries < 6) {
                job.next = GetTickCount64() + 1500;
                std::lock_guard lock(m.btLock_);
                m.btJobs_.push_back(job);
            }
        }
        w1.Stop();
        w2.Stop();
    } catch (...) {
        Wh_Log(L"alerts: Bluetooth watcher failed");
    }
    return 0;
}

// Notifications: the listener is only read when a toast appears (panel-split posts a message), with a slow
// safety poll. Without that hook the safety poll runs every 5 s and shows what it finds, as the old mod did.
DWORD WINAPI AlNotifThread(void*) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        // Early init can bind to a dead proxy for good, so toasts stay native for the first 30 s.
        if (WaitForSingleObject(g_alStop, 30000) == WAIT_OBJECT_0) return 0;
        auto listener = wunm::UserNotificationListener::Current();
        bool granted = false;
        for (int tries = 0; !granted; tries++) {
            const auto st = listener.RequestAccessAsync().get();
            granted = st == wunm::UserNotificationListenerAccessStatus::Allowed;
            if (granted) break;
            if (tries >= 2) g_services.Post([] { if (g_alMod) g_alMod->OnListenerOk(false); });  // Toasts stay native.
            if (WaitForSingleObject(g_alStop, tries < 2 ? 3000 : 60000) == WAIT_OBJECT_0) return 0;
        }
        g_alListener = listener;
        g_services.Post([] { if (g_alMod) g_alMod->OnListenerOk(true); });
        std::set<uint32_t> seen;
        bool first = true;
        auto poll = [&](bool pop) {
            int found = 0;
            auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
            std::set<uint32_t> current;
            for (uint32_t i = 0; i < list.Size(); i++) current.insert(list.GetAt(i).Id());
            if (first) { seen = current; first = false; return 0; }
            for (uint32_t i = 0; i < list.Size(); i++) {
                try {
                    auto un = list.GetAt(i);
                    const uint32_t id = un.Id();
                    if (seen.count(id)) continue;
                    seen.insert(id);
                    found++;
                    if (!pop) continue;
                    AItem it;
                    it.kind = AK::Notification;
                    it.nid = id;
                    it.ms = 7000;  // The toast is gone, so this is the reading time.
                    auto info = un.AppInfo();
                    auto disp = info.DisplayInfo();
                    std::wstring app = disp.DisplayName().c_str();
                    std::wstring lower = app;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    if (lower.find(L"powershell") != std::wstring::npos || lower.find(L"pwsh") != std::wstring::npos ||
                        lower.find(L"cmd.exe") != std::wstring::npos || lower.find(L"conhost.exe") != std::wstring::npos) continue;
                    it.key = info.AppUserModelId().c_str();
                    try {
                        auto stream = disp.GetLogo({32.0f, 32.0f}).OpenReadAsync().get();
                        const uint32_t size = (uint32_t)stream.Size();
                        if (size > 0 && size < (4u << 20)) {
                            winrt::Windows::Storage::Streams::DataReader rd(stream);
                            rd.LoadAsync(size).get();
                            std::vector<uint8_t> bytes(size);
                            rd.ReadBytes(bytes);
                            it.icon = AlDecodeImage(bytes);
                        }
                    } catch (...) {}
                    auto binding = un.Notification().Visual().GetBinding(wun::KnownNotificationBindings::ToastGeneric());
                    if (binding) {
                        auto texts = binding.GetTextElements();
                        if (texts.Size() > 0) it.title = texts.GetAt(0).Text().c_str();
                        if (texts.Size() > 1) it.sub = texts.GetAt(1).Text().c_str();
                    }
                    if (it.title.empty()) it.title = app.empty() ? L"New notification" : app;
                    if (it.title.size() > 120) { it.title.resize(120); it.title += L"..."; }
                    if (it.sub.size() > 160) { it.sub.resize(160); it.sub += L"..."; }
                    it.hint = app.empty() ? AlT(L"alert.notif.tap", L"Tap to open") : AlFmt(AlT(L"alert.notif.tap.app", L"Tap to open %s"), app.c_str());
                    g_services.Post([it] { if (g_alMod) g_alMod->Enqueue(it); });
                } catch (...) {}
            }
            for (auto it = seen.begin(); it != seen.end();) it = current.count(*it) ? std::next(it) : seen.erase(it);
            return found;
        };
        try { poll(false); } catch (...) {}
        HANDLE h[2] = {g_alStop, g_alWake};
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, h, FALSE, g_alHookSeen ? 30000 : 5000);
            if (w == WAIT_OBJECT_0) break;
            try {
                if (w == WAIT_OBJECT_0 + 1) {
                    // The window can appear before the listener lists it: look again at 100, 300 and 800 ms.
                    for (DWORD d : {100u, 200u, 500u}) {
                        if (WaitForSingleObject(g_alStop, d) == WAIT_OBJECT_0) return 0;
                        if (poll(true) > 0) break;
                    }
                } else {
                    poll(!g_alHookSeen);
                }
            } catch (const winrt::hresult_error&) {
                // E_NOTIMPL and REGDB_E_CLASSNOTREG while the subsystem is not ready: quiet retry.
            } catch (...) {}
        }
    } catch (...) {
        Wh_Log(L"alerts: notification listener failed");
    }
    return 0;
}

void CALLBACK AlOsdEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW || !hwnd || !g_alMod) return;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"XamlExplorerHostIslandWindow")) return;
    RECT r{};
    GetWindowRect(hwnd, &r);
    if (r.right - r.left > 700 || r.bottom - r.top > 300 || r.right <= r.left) return;  // Not the fullscreen-sized host.
    AlertsModule& m = *g_alMod;
    if (event == EVENT_OBJECT_SHOW) m.OsdShown(false);
    else m.OsdHidden();
}

LRESULT CALLBACK AlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_alMod) return g_alMod->Wnd(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT AlertsModule::Wnd(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_alMsgToast && g_alMsgToast) { g_alHookSeen = true; SetEvent(g_alWake); return 0; }
    if (msg == g_alMsgOsd && g_alMsgOsd) {  // lParam 1: the hook moved the popup onto the bubble, so the island yields.
        if (lp == 2) {  // 2: the hook hid the popup; the island shows its own slider.
            osdSeen_ = true;
            if (g_alLevelRead) SetEvent(g_alLevelRead);
            return 0;
        }
        if (lp) OsdShown(true);
        else osdSeen_ = true;
        return 0;
    }
    switch (msg) {
        case WM_AL_CAPS: OnCaps(lp != 0); return 0;
        case WM_AL_PRIVACY: SetTimer(hwnd, kAlPrivacy, 250, nullptr); return 0;
        case WM_AL_LEVEL: OnLevels(wp, lp); return 0;
        case WM_AL_DNDFAIL: Launch(L"ms-settings:quietmomentshome"); return 0;
        case WM_POWERBROADCAST: OnPower(); return TRUE;
        case WM_CLIPBOARDUPDATE: OnClipboard(); return 0;
        case WM_DEVICECHANGE: OnDevice(wp, lp); return TRUE;
        case WM_TIMER:
            if (wp == kAlTick) Tick();
            else if (wp == kAlHeartbeat) UpdateToastFlag();
            else if (wp == kAlClipboard) { KillTimer(hwnd, kAlClipboard); ReadClipboardNow(); }
            else if (wp == kAlOsd) OsdHidden();
            else if (wp == kAlPrivacy) { KillTimer(hwnd, kAlPrivacy); ReadPrivacy(); }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Lifetime ----

void AlertsModule::Start(Services& s) {
    sv_ = &s;
    g_alMod = this;
    g_alStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_alWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alLevelRead = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alLevelSet = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alDndOff = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alMsgToast = RegisterWindowMessageW(L"Windhawk_TopBar_ToastShown");
    g_alMsgOsd = RegisterWindowMessageW(L"Windhawk_TopBar_Osd");
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = AlWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kAlertsClass;
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAlertsClass, L"Top bar alerts", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return;
    g_alHwnd = hwnd_;
    ChangeWindowMessageFilterEx(hwnd_, g_alMsgToast, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, g_alMsgOsd, MSGFLT_ALLOW, nullptr);
    powerA_ = RegisterPowerSettingNotification(hwnd_, &kGuidBatteryPercent, DEVICE_NOTIFY_WINDOW_HANDLE);
    powerB_ = RegisterPowerSettingNotification(hwnd_, &kGuidAcDc, DEVICE_NOTIFY_WINDOW_HANDLE);
    AddClipboardFormatListener(hwnd_);
    SetTimer(hwnd_, kAlHeartbeat, 10000, nullptr);
    // DND state through WNF (read-only).
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto query = reinterpret_cast<AlWnfQuery>(GetProcAddress(nt, "NtQueryWnfStateData"))) {
            ULONG stamp = 0, size = sizeof(int);
            int v = 0;
            ULONG64 name = kAlWnfQuietHours;
            if (query(&name, nullptr, nullptr, &stamp, &v, &size) == 0 && size >= sizeof(int)) dndValue_ = v;
        }
        if (auto sub = reinterpret_cast<AlWnfSubscribe>(GetProcAddress(nt, "RtlSubscribeWnfStateChangeNotification")))
            sub(&g_alWnfSub, kAlWnfQuietHours, 0, AlWnfProc, nullptr, nullptr, 0, 0);
    }
    // The volume and brightness popup: window events from Explorer (log, and a fallback signal).
    DWORD pid = 0;
    if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) GetWindowThreadProcessId(tray, &pid);
    if (pid) osdHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, AlOsdEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
    threads_.push_back(CreateThread(nullptr, 0, AlCapsThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlPrivacyThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlBtThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlNotifThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlLevelThread, nullptr, 0, nullptr));
    OnPower();
}

void AlertsModule::Stop() {
    if (g_alStop) SetEvent(g_alStop);
    if (osdHook_) UnhookWinEvent(osdHook_);
    osdHook_ = nullptr;
    if (g_alWnfSub) {
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
            if (auto unsub = reinterpret_cast<AlWnfUnsubscribe>(GetProcAddress(nt, "RtlUnsubscribeWnfStateChangeNotification"))) unsub(g_alWnfSub);
        g_alWnfSub = nullptr;
    }
    for (HANDLE t : threads_) {
        if (!t) continue;
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
    threads_.clear();
    if (hwnd_) {
        RemoveClipboardFormatListener(hwnd_);
        if (powerA_) UnregisterPowerSettingNotification(powerA_);
        if (powerB_) UnregisterPowerSettingNotification(powerB_);
        g_alHwnd = nullptr;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    g_alListener = nullptr;
    g_alMod = nullptr;
    if (g_hwnd) {
        RemovePropW(g_hwnd, L"TopBarToastsOk");
        RemovePropW(g_hwnd, L"TopBarToastsTick");
    }
    UnregisterClassW(kAlertsClass, GetModuleHandleW(nullptr));
    if (g_alStop) CloseHandle(g_alStop);
    if (g_alWake) CloseHandle(g_alWake);
    if (g_alLevelRead) CloseHandle(g_alLevelRead);
    if (g_alLevelSet) CloseHandle(g_alLevelSet);
    if (g_alDndOff) CloseHandle(g_alDndOff);
    g_alStop = g_alWake = g_alLevelRead = g_alLevelSet = g_alDndOff = nullptr;
}

IModule* AlertsModuleInstance() {
    static AlertsModule m;
    return &m;
}

// Hooks called from the island and the pill.
void AlertsDrawOverlay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float L, float R, float B, float s) {
    if (g_alMod) g_alMod->DrawOverlay(rt, brush, L, R, B, s);
}
void AlertsPublishGeometry() {
    if (g_alMod) g_alMod->PublishGeometry();
}
// The shelf handed something to the phone (or could not): a short confirmation.
void AlertsPhoneSent(int sent, int failed) {
    if (!g_alMod) return;
    AItem it;
    it.kind = AK::Phone;
    it.ms = 2200;
    if (sent) {
        it.title = AlT(L"alert.phone.sent", L"Sent to phone");
        if (failed) it.sub = AlFmt(AlT(L"alert.phone.some", L"%d sent, %d failed"), sent, failed);
        else if (sent > 1) it.sub = AlFmt(AlT(L"alert.phone.n", L"%d items"), sent);
        it.good = !failed;
        it.danger = failed > 0;
    } else {
        it.title = AlT(L"alert.phone.fail", L"Could not send to phone");
        it.danger = true;
    }
    g_alMod->Enqueue(it);
}
bool AlertsHoldsHover() {
    return g_alMod && g_alMod->HoldsHover();
}
void AlertsFullscreenChanged() {
    if (g_alMod) g_alMod->FullscreenChanged();
}
float AlertsDotsReserve() {
    return g_alMod ? 12.0f * g_alMod->Dots() : 0.0f;
}

#line 1 "alerts/levels.inc"
// ---- Volume and brightness for the island's own slider ----
// Read on request (tray-bridge hid the native popup: a key was pressed), set on request (the slider).
// Its own MTA thread, so Core Audio and WMI calls never run on the UI thread.

struct AlLevels {
    Com<IAudioEndpointVolume> ep;
    Com<IWbemServices> wmi;

    void Connect() {
        ep.Reset();
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(en.Put()))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.Put())))
            dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep.Put()));
    }

    int Volume(bool* mute) {
        Connect();  // The default device may have changed since the last press; cheap enough per press.
        float v = 0;
        BOOL m = FALSE;
        if (!ep || FAILED(ep->GetMasterVolumeLevelScalar(&v))) return -1;
        ep->GetMute(&m);
        *mute = m != FALSE;
        return (int)std::lround(v * 100.0f);
    }

    bool Wmi() {
        if (wmi) return true;
        Com<IWbemLocator> loc;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(loc.Put())))) return false;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        const HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, wmi.Put());
        SysFreeString(ns);
        if (FAILED(hr)) return false;
        CoSetProxyBlanket(wmi.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        return true;
    }

    // First instance of a class. Brightness exists on the internal panel only (a desktop gets -1).
    bool First(const wchar_t* query, Com<IWbemClassObject>& out) {
        if (!Wmi()) return false;
        BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
        Com<IEnumWbemClassObject> en;
        const HRESULT hr = wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.Put());
        SysFreeString(lang);
        SysFreeString(q);
        ULONG n = 0;
        return SUCCEEDED(hr) && en && SUCCEEDED(en->Next(2000, 1, out.Put(), &n)) && n == 1;
    }

    int Brightness() {
        Com<IWbemClassObject> o;
        if (!First(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE", o)) return -1;
        VARIANT v;
        VariantInit(&v);
        int r = -1;
        if (SUCCEEDED(o->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) r = v.vt == VT_UI1 ? v.bVal : v.vt == VT_I4 ? v.lVal : -1;
        VariantClear(&v);
        return r;
    }

    void SetBrightness(int pct) {
        Com<IWbemClassObject> inst;
        if (!First(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE", inst)) return;
        VARIANT path;
        VariantInit(&path);
        if (FAILED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) { VariantClear(&path); return; }
        Com<IWbemClassObject> cls, def, args;
        BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods"), method = SysAllocString(L"WmiSetBrightness");
        if (SUCCEEDED(wmi->GetObject(clsName, 0, nullptr, cls.Put(), nullptr)) &&
            SUCCEEDED(cls->GetMethod(method, 0, def.Put(), nullptr)) && def && SUCCEEDED(def->SpawnInstance(0, args.Put()))) {
            VARIANT t, b;
            VariantInit(&t);
            t.vt = VT_I4;
            t.lVal = 0;
            VariantInit(&b);
            b.vt = VT_UI1;
            b.bVal = (BYTE)std::clamp(pct, 0, 100);
            args->Put(L"Timeout", 0, &t, 0);
            args->Put(L"Brightness", 0, &b, 0);
            wmi->ExecMethod(path.bstrVal, method, 0, nullptr, args.Get(), nullptr, nullptr);
        }
        SysFreeString(clsName);
        SysFreeString(method);
        VariantClear(&path);
    }
};

// ---- Do Not Disturb off: the QuietHoursSettings COM class (undocumented) ----
// The object Settings and Quick Settings drive: CLSID_QuietHoursSettings, hosted out of process by
// WpnUserService, interface IQuietHoursSettings (layout from Rafael Rivera's quiethours.idl, also used
// by WinDndToggle and others). Only the first two methods are declared. The class is registered in the
// 64-bit view only, so a 32-bit host asks for the 64-bit server; the interface's proxy is registered in
// both views. Setting the user profile to Unrestricted is what turning DND off in Quick Settings does.
struct IAlQuietHoursSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_UserSelectedProfile(LPWSTR* profile) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserSelectedProfile(LPCWSTR profile) = 0;
};
constexpr GUID kAlClsidQuietHours = {0xf53321fa, 0x34f8, 0x4b7f, {0xb9, 0xa3, 0x36, 0x18, 0x77, 0xcb, 0x94, 0xcf}};
constexpr GUID kAlIidQuietHours = {0x6bff4732, 0x81ec, 0x4ffb, {0xae, 0x67, 0xb6, 0xc1, 0xbc, 0x29, 0x63, 0x1f}};
constexpr wchar_t kAlProfileOff[] = L"Microsoft.QuietHoursProfile.Unrestricted";

bool AlDndOff() {
    IAlQuietHoursSettings* qh = nullptr;
    HRESULT hr = CoCreateInstance(kAlClsidQuietHours, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_ACTIVATE_64_BIT_SERVER, kAlIidQuietHours,
                                  reinterpret_cast<void**>(&qh));
    if (FAILED(hr)) hr = CoCreateInstance(kAlClsidQuietHours, nullptr, CLSCTX_LOCAL_SERVER, kAlIidQuietHours, reinterpret_cast<void**>(&qh));
    if (FAILED(hr) || !qh) {
        Wh_Log(L"alerts: QuietHoursSettings unavailable, 0x%08lX", (unsigned long)hr);
        return false;
    }
    hr = qh->put_UserSelectedProfile(kAlProfileOff);
    bool ok = SUCCEEDED(hr);
    if (ok) {  // Read it back: a changed vtable would succeed and do something else.
        LPWSTR now = nullptr;
        ok = SUCCEEDED(qh->get_UserSelectedProfile(&now)) && now && !wcscmp(now, kAlProfileOff);
        if (now) CoTaskMemFree(now);
    }
    Wh_Log(L"alerts: DND off via QuietHoursSettings: hr 0x%08lX, %s", (unsigned long)hr, ok ? L"verified" : L"not verified");
    qh->Release();
    return ok;
}

DWORD WINAPI AlLevelThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        AlLevels lv;
        HANDLE waits[] = {g_alStop, g_alLevelRead, g_alLevelSet, g_alDndOff};
        auto post = [&] {
            bool mute = false;
            const int vol = lv.Volume(&mute);
            const int bright = lv.Brightness();
            if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_LEVEL, (WPARAM)(vol & 0xFFFF) | (mute ? 0x10000 : 0), (LPARAM)bright);
        };
        post();  // Baseline, so the first key press can be told apart.
        for (;;) {
            const DWORD w = WaitForMultipleObjects(4, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) {
                post();
                // The popup can show before the new brightness is readable: look once more.
                if (WaitForSingleObject(g_alStop, 300) == WAIT_OBJECT_0) break;
                post();
            } else if (w == WAIT_OBJECT_0 + 2) {
                const int v = g_alSetVolume.exchange(-1), b = g_alSetBright.exchange(-1);
                if (v >= 0) {
                    lv.Connect();
                    if (lv.ep) {
                        lv.ep->SetMute(FALSE, nullptr);
                        lv.ep->SetMasterVolumeLevelScalar(v / 100.0f, nullptr);
                    }
                }
                if (b >= 0) lv.SetBrightness(b);
            } else if (w == WAIT_OBJECT_0 + 3) {
                if (!AlDndOff())  // Settings is the fallback; opened from the UI thread.
                    if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_DNDFAIL, 0, 0);
            } else {
                break;
            }
        }
    }
    CoUninitialize();
    return 0;
}

#line 1 "bar/window.inc"
void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW || !hwnd) {
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND) {
        OnForegroundForPanels(hwnd);  // Closes Quick Settings or Search left behind by an app switch.
        if (IsShellExperienceWindow(hwnd) || IsQuickSettingsWindow(hwnd)) {
            if (GetTickCount64() - g_anchorTick >= kPanelAnchorMs) {
                g_quickPage = Action::None;  // Opened from the keyboard, not from an icon.
            }
            const bool shellPanel = IsShellExperienceWindow(hwnd);
            if (shellPanel) {
                g_notificationPanel = hwnd;
                // Opened from the dock, Win+N or an alert: it stays at its native place above the dock.
                if (!CalendarOpenWanted()) {
                    g_panelMode = kPanelNative;
                    SaveNativePanelRect(hwnd);  // No-op if already saved; checks a place left by an older bar.
                    RestoreNativePanel(hwnd);
                }
            }
            if (!shellPanel || g_panelMode == kPanelCalendar) {
                if (GetTickCount64() - g_panelOpenTick > kPanelDropMs) {
                    g_panelOpenTick = GetTickCount64();  // Unless it was already started before opening.
                }
                PlacePanel(hwnd);
                FollowPanel();
            }
        }
        else if (g_panelMoved && hwnd != g_hwnd && hwnd != g_isHwnd && g_panelMode == kPanelCalendar &&
                 GetTickCount64() - g_calendarTick > 1500) {
            // The calendar closed: put the window back now, so a native open from the dock is in place at once.
            RestoreNativePanel(g_notificationPanel);
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

// One timer per minute (per second with seconds on), aimed at the next boundary, so the clock never polls.
void ArmClock() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int ms = g_settings.clockSeconds ? 1000 - st.wMilliseconds + 20 : (60 - st.wSecond) * 1000 - st.wMilliseconds + 50;
    SetTimer(g_hwnd, kClockTimer, ms, nullptr);
}

// The clock text is only on screen as the collapsed pill; any other view redraws it when it comes back.
void ClockTick() {
    if (g_viewCur.m == &g_clockModule && g_viewCur.v == View::Pill) IslandRecompute();  // The text may be wider now.
}

// Follow-mouse moves only the island: it hangs from the monitor the pointer is on. Not while it is open under the pointer.
void FollowTick() {
    if (g_settings.monitor != -1 || g_expanded || g_hidden) return;
    const RECT before = g_isMon;
    UpdateIslandMonitor();
    if (!EqualRect(&before, &g_isMon)) {
        g_isInit = false;  // No spring across screens: it appears in its new place.
        IslandRecompute();
    }
}

// The monitor choice changed (settings) or a display was added or removed.
void ApplyMonitor() {
    PlaceBar();
    if (g_settings.monitor == -1) SetTimer(g_hwnd, kFollowTimer, 250, nullptr);
    else KillTimer(g_hwnd, kFollowTimer);
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

// The hit list moves with the buttons: resync hover against the new layout.
void ResyncHover() {
    if (!g_tracking) return;
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);
    SetHover(HitIdAt(p));
}

// One display frame: the springs step by the real time since the last one (QPC, at most 50 ms).
void StepFrame() {
    g_clock.Acked();
    const double now = QpcSeconds();
    double dt = g_lastFrame ? now - g_lastFrame : 1.0 / 60.0;
    if (dt > 0.05) dt = 0.05;
    g_lastFrame = now;
    g_frameDt = dt;
    g_isW.Step(dt);
    g_isH.Step(dt);
    g_mix.Step(dt);
    g_cover.Step(dt);
    if (IslandSettled() && !ModulesAnimating()) {
        g_clock.Sleep();
        g_lastFrame = 0;
    }
    g_isDirty = true;
    Paint();
    IslandPaint();
}

void RunAction(Action action) {
    switch (action) {
        case Action::QuickSettings:
        case Action::Wifi:
        case Action::Bluetooth:
            OpenQuickPage(action);
            break;
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
        if (wp == kTrayBridgeBroadcast || GetTickCount64() - g_trayBroadcastTick < 3000) {
            return 0;  // The tray bridge asking apps for their icons, not a new taskbar.
        }
        g_registered = false;
        if (!g_hidden) RegisterBar();
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
            SetHover(HitIdAt(p));
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            SetHover(H_none);
            return 0;
        case WM_APP_PAINT:
            Paint();
            IslandPaint();
            return 0;
        case WM_APP_POST: {
            auto* fn = reinterpret_cast<std::function<void()>*>(lp);
            if (fn) {
                (*fn)();
                delete fn;
            }
            return 0;
        }
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
            } else {
                ShowBarMenu();
            }
            return 0;
        }
        case WM_HOTKEY:
            if (wp == kHotkeyId) ToggleHotkey();
            return 0;
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
                ClockTick();
            } else if (wp == kFollowTimer) {
                FollowTick();
            } else if (wp == kTitleTimer) {
                KillTimer(hwnd, kTitleTimer);
                g_titlePending = false;
                if (ReadForeground()) {  // A spinner in a terminal title changes nothing the bar shows.
                    InvalidateAll();
                    ResyncHover();
                }
            } else if (wp == kDropTimer) {
                ShelfDropDwell();
            } else if (wp == kDwellTimer) {
                IslandDwellTick();
            } else if (wp == kZoneTimer) {
                IslandZoneTick();
            } else if (wp == kAlertTimer) {
                KillTimer(hwnd, wp);
                IslandRecompute();
            } else if (wp >= kModuleTimerBase) {
                RunModuleTimer(wp);
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
            } else if (wp == kSearchCloseTimer) {
                StepSearchClose();
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
            StepFrame();
            return 0;
        case WM_APP_SETTINGS:
            g_settings = LoadSettings();
            for (IModule* m : g_modules) m->OnSettings(g_settings);
            ArmClock();
            ApplyMonitor();
            InvalidateAll();
            IslandRecompute();
            return 0;
        case WM_REFRESH_TITLE:
            if (!g_titlePending) {
                g_titlePending = true;  // A busy title (a spinner in a terminal) costs one read per 100 ms.
                SetTimer(hwnd, kTitleTimer, 100, nullptr);
            }
            return 0;
        case WM_REFRESH_FOREGROUND:
            RehookTitle();
            if (ReadForeground()) InvalidateAll();
            IslandRecompute();  // A panel opening or closing moves the buttons.
            return 0;
        case WM_SETTINGCHANGE:
            {
                const BYTE before[3] = {g_accent[0], g_accent[1], g_accent[2]};
                ReadAccent();
                if (ReadForeground() || memcmp(before, g_accent, 3)) InvalidateAll();  // Many settings broadcast; few matter.
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            ApplyMonitor();  // Picks the monitor again: the chosen one may be gone, or its scale new.
            IslandRescale();
            InvalidateAll();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(hwnd, kHotkeyId);
            ShelfDropRevoke(hwnd);
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
    OleInitialize(nullptr);  // Drag and drop needs OLE, not only COM.

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
    g_pinned = Wh_GetIntValue(L"pinned", 0) != 0;
    if (!RegisterHotKey(g_hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D')) {
        Wh_Log(L"Ctrl+Alt+D is taken (the old island mod?): error %lu", GetLastError());
    }
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

    g_surface.SetBaseAlpha(1.0f / 255.0f);  // Gaps between bubbles stay drop targets.
    RegisterBar();
    IslandInit();
    ApplyMonitor();
    ShelfDropRegister(g_hwnd);
    ShelfDropRegister(g_isHwnd);
    ArmClock();
    InvalidateAll();

    // Ask the tray bridge in Explorer for every icon the taskbar already has. Not a TaskbarCreated
    // broadcast: some apps ignore it, so icons added before the bar started went missing. Set just before
    // the message loop so the bridge's replay is answered at once. If the bridge is not loaded yet, it
    // replays on its own the first time it sees this window.
    if (HANDLE replay = CreateEventW(nullptr, FALSE, FALSE, kTrayReplayEventName)) {
        SetEvent(replay);
        CloseHandle(replay);
    }

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
    ShelfShutdown();  // Joins the thumbnail worker before the DLL can go.
    IslandShutdown();
    ReleaseGfx();
    OleUninitialize();
    return 0;
}


#line 1 "bar/tail.inc"
}  // namespace

BOOL WhTool_ModInit() {
    g_thread = CreateThread(nullptr, 0, BarThread, nullptr, 0, &g_threadId);
    return g_thread != nullptr;
}

void WhTool_ModSettingsChanged() {
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_APP_SETTINGS, 0, 0);  // Read on the UI thread, which is the only writer.
    }
}

void WhTool_ModUninit() {
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}


#line 1 "scaffold.inc"
// Windhawk tool mod implementation for mods which don't need to inject to other
// processes or hook other functions. Context:
// https://github.com/ramensoftware/windhawk/wiki/Mods-as-tools:-Running-mods-in-a-dedicated-process
//
// The mod will load and run in a dedicated windhawk.exe process.
//
// Paste the code below as part of the mod code, and use these callbacks:
// * WhTool_ModInit
// * WhTool_ModSettingsChanged
// * WhTool_ModUninit
//
// Currently, other callbacks are not supported.

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    DWORD sessionId;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
        sessionId == 0) {
        return FALSE;
    }

    bool isExcluded = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0 ||
            wcscmp(argv[i], L"-service-start") == 0 ||
            wcscmp(argv[i], L"-service-stop") == 0) {
            isExcluded = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isExcluded) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
