// ==WindhawkMod==
// @id              top-bar
// @name            Top bar
// @description     Top bar with three bubbles in the Seelen toolbar style, drawn with Direct2D
// @version         3.1
// @author          local
// @include         windhawk.exe
// @compilerOptions -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32 -lversion -lwininet -liphlpapi -luuid -ld2d1 -ldwrite -lwindowscodecs -ldwmapi
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
*/
// ==/WindhawkModSettings==
#line 1 "core/includes.inc"

// Runs as a tool mod in its own process. Nothing polls: the bar redraws on Windows
// events, one timer per minute for the clock, and a short timer only while the pill moves.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <sddl.h>
#include <wininet.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <atomic>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

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
constexpr UINT_PTR kWingTimer = 12;    // End of the hold that keeps bell and calendar out while a panel opens.
constexpr UINT_PTR kFollowTimer = 13;  // Follow-mouse: which monitor the island is on. Exists only in that mode.
constexpr int kHotkeyId = 1;  // Ctrl+Alt+D
constexpr UINT_PTR kModuleTimerBase = 100;  // Module timers: base + slot.

// From the tray-bridge mod in Explorer, which declares the same layout.
constexpr ULONG_PTR kTrayCopyData = 0x7472;
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
// Modes understood by the panel-split mod inside ShellExperienceHost.
constexpr WPARAM kPanelNotifications = 1;
constexpr WPARAM kPanelCalendar = 2;

// Seelen fancy-toolbar, in CSS px scaled by DPI: item size 16, padding 8, margin 0.
constexpr float kItem = 16.0f;
constexpr float kItemPad = 4.0f;
constexpr float kBarH = 32.0f;
constexpr float kGap = 4.0f;
constexpr float kItemRadius = 4.8f;
constexpr float kFontSize = 12.8f;
constexpr float kPillH = 24.0f;  // Height of all three bubbles.
constexpr float kPillPadX = 12.0f;
constexpr float kWingSize = 28.0f;     // Bell and calendar buttons.
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
constexpr int kWingHoldMs = 1500;
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

enum class Action { None, QuickSettings, Wifi, Bluetooth, TrayIcon, Notifications, Calendar, Profile };

// Stable ids for hover, so a list that changes under the pointer doesn't move the highlight.
enum HitId { H_none = 0, H_profile = 1, H_settings, H_battery, H_wifi, H_bluetooth, H_bell, H_calendar, H_trayBase = 1000 };

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
HWND g_quickReturnTo = nullptr;
int g_quickCloseStage = 0;
bool g_quickHadFocus = false;
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
        rt_->Clear(D2D1::ColorF(0, 0.0f));
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
    void PresentCrop(HWND hwnd, POINT pos, POINT src, SIZE size) {
        if (!mem_) return;
        HDC screen = GetDC(nullptr);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
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
        swprintf_s(head, L"%ls|%.2f|%.1f|%.1f|%d%d|", family, size, o.maxW, o.maxH, o.ellipsis, o.center);
        const std::wstring key = std::wstring(head) + text;
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        if (cache_.size() > 200) cache_.clear();
        Com<IDWriteTextFormat> fmt;
        if (FAILED(factory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"", fmt.Put()))) {
            return {};
        }
        Com<IDWriteTextLayout> layout;
        if (FAILED(factory_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), o.maxW, o.maxH,
                                              layout.Put()))) {
            return {};
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
RECT g_bubbleRect[5]{};  // As last painted: left, (centre: the island's own window), right, bell, calendar.
bool g_isDirty = false;  // The island needs drawing (see island.inc).
float g_wingT = 0.0f;    // Bell and calendar travel: 0 parked under the notch, 1 at their place.
float g_wingX[2]{};      // Their place (bar pixels, left edge), valid when g_wingOk.
bool g_wingOk[2]{};      // False when the title or tray leave no room for the button.
float g_wingSize = 0;
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
D2D1_RECT_F g_bubbles[5]{};  // 0 left, 1 unused, 2 right, 3 bell, 4 calendar
bool g_hasBubble[5]{};
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

    // The left bubble stops where the bell will stand when the island is fully open, so nothing reflows.
    const float btn = roundf(kWingSize * s);
    const float expHalf = IslandMaxExpandedW() * s / 2.0f + kNotchShoulder * s;
    const float limit = (W / 2.0f - expHalf) - margin - gap - btn - gap - bpad;
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

    // Bell and calendar run out from under the notch to stand beside the side bubbles.
    g_wingSize = btn;
    const float parked = W / 2.0f - btn / 2.0f;
    const float wy = floorf((H - btn) / 2.0f);
    g_wingX[0] = roundf(g_bubbles[0].right + gap);
    g_wingX[1] = roundf(g_bubbles[2].left - gap - btn);
    g_wingOk[0] = g_wingX[0] + btn + gap <= W / 2.0f - expHalf;
    g_wingOk[1] = g_wingX[1] >= W / 2.0f + expHalf + gap;
    static bool logged[2];
    for (int i = 0; i < 2; i++) {
        if (g_wingOk[i] == logged[i]) {
            logged[i] = !g_wingOk[i];
            if (!g_wingOk[i]) Wh_Log(L"No room for the %ls button beside the island", i ? L"calendar" : L"bell");
        }
    }
    if (g_wingT > 0.01f) {
        const float t = g_wingT;
        for (int i = 0; i < 2; i++) {
            if (!g_wingOk[i]) continue;
            const float x = parked + (g_wingX[i] - parked) * t;
            const int slot = 3 + i;
            g_bubbles[slot] = D2D1::RectF(x, wy, x + btn, wy + btn);
            g_hasBubble[slot] = true;
            if (t > 0.6f) {
                addHit(x, wy, btn, btn, btn / 2.0f, i ? Action::Calendar : Action::Notifications, i ? H_calendar : H_bell);
            }
            iconCmd(i ? I_calendar : I_bell, x + btn / 2.0f, wy + btn / 2.0f, (i ? 14.0f : 16.0f) * s);
        }
    }
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

    for (int i = 0; i < 5; i++) {
        if (!g_hasBubble[i]) continue;
        const float radius = i < 3 ? g_bubbleRadius : g_wingSize / 2.0f;
        color(kSideBg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(g_bubbles[i], radius, radius), brush);
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
    for (int i = 0; i < 5; i++) {
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
    void WakeAnimation();    // Re-check Animating() and run the frame clock.
    void Post(std::function<void()>);  // From any thread to the UI thread.
    void SetTimer(IModule*, UINT ms, std::function<void()>);  // One timer per module; repeats until KillTimer.
    void KillTimer(IModule*);
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

// The collapsed content: date and time.
class ClockModule : public IModule {
public:
    const wchar_t* Id() const override { return L"clock"; }
    Size2 Measure(View) override {
        const float size = g_settings.dateAbove ? g_text.DefaultSize() * 0.85f : 0.0f;  // Two lines need to be smaller.
        return {ToDip(StableWidth(ClockText(), size)) + 2 * kPillPadX, kNotchH};
    }
    void Draw(const DrawCtx& c) override {
        DrawCentredText(c, ClockText(), c.rect, g_settings.dateAbove ? g_text.DefaultSize() * 0.85f : 0.0f);
    }
};

// Stub dashboard page. Replaced by the real dashboards (P6c).
class PlaceholderPage : public IModule {
public:
    PlaceholderPage(int index, int count) : index_(index), count_(count) {}
    const wchar_t* Id() const override { return index_ == 1 ? L"page-1" : L"page-2"; }
    bool HasDashboard() const override { return true; }
    int DashboardOrder() const override { return index_ * 10; }
    Size2 Measure(View) override { return {475.0f, 140.0f}; }
    void Draw(const DrawCtx& c) override {
        wchar_t line[64];
        swprintf_s(line, L"Placeholder page %d of %d", index_, count_);
        const float h = c.rect.bottom - c.rect.top;
        DrawCentredText(c, line, D2D1::RectF(c.rect.left, c.rect.top + h * 0.22f, c.rect.right, c.rect.top + h * 0.5f), g_text.DefaultSize() * 1.4f);
        DrawCentredText(c, L"Scroll to switch pages", D2D1::RectF(c.rect.left, c.rect.top + h * 0.5f, c.rect.right, c.rect.top + h * 0.7f));
    }
private:
    int index_, count_;
};

ClockModule g_clockModule;
PlaceholderPage g_page1(1, 2), g_page2(2, 2);
IModule* const g_modules[] = {&g_clockModule, &g_page1, &g_page2};

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
void FollowPanel() {
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
    if (g_panelCold && g_panelTicks <= 1) {
        g_panelTicks = 26;  // A host starting from nothing can take far longer than usual.
    }
    if (fg && IsShellExperienceWindow(fg) && g_panelCold) {
        // First open since the host started: its clip was sized before the mode could arrive,
        // so reopen once, now that it can be set beforehand.
        g_panelCold = false;
        g_panelResend = 0;
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

// Runs while the app that sent the update is still waiting, so its icon handle is valid.
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

IState g_isState = IState::Collapsed;
Spring g_isW, g_isH;    // Body size, pixels.
Spring g_wing;          // 0 bell and calendar parked, 1 out.
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
int g_wheelAcc = 0;
ULONGLONG g_leaveSince = 0;
ULONGLONG g_wingHold = 0;
ULONGLONG g_alertSeq = 0;
std::vector<AlertItem> g_alerts;
RECT g_isBody{};        // Screen rect of the body as drawn.
RECT g_isTarget{};      // Screen rect of the body when the springs have settled (no shoulders).
D2D1_RECT_F g_isView{}; // Stage rect of the view last drawn, for pointer events.
int g_isCropX = 0;

struct ModuleTimer { IModule* m; std::function<void()> fn; };
std::vector<ModuleTimer> g_moduleTimers;

float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

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

bool PanelWingsWanted() {
    HWND fg = GetForegroundWindow();
    return fg && IsShellExperienceWindow(fg) && (g_panelMode == kPanelNotifications || g_panelMode == kPanelCalendar);
}

void AimIfChanged(Spring& sp, float target, float k, float c) {
    if (fabsf(sp.target - target) > 0.01f) sp.Aim(target, k, c);
}

bool IslandSettled() {
    return g_isW.Settled() && g_isH.Settled() && g_wing.Settled() && g_mix.Settled();
}

// Derives everything from the inputs: alerts, hover, tab, panel. Called when an input changes, never per frame.
void IslandRecompute() {
    if (!g_isHwnd || g_fullscreen || g_hidden || !EnsureGfx()) return;
    PrepareText();
    const ULONGLONG now = GetTickCount64();
    g_alerts.erase(std::remove_if(g_alerts.begin(), g_alerts.end(), [&](const AlertItem& a) { return a.expires && a.expires <= now; }),
                   g_alerts.end());
    const std::vector<IModule*> dashes = Dashboards();
    if (g_tab >= (int)dashes.size()) g_tab = std::max(0, (int)dashes.size() - 1);

    IState state = IState::Collapsed;
    if (g_expanded && !dashes.empty()) state = IState::Expanded;
    else if (!g_alerts.empty()) state = IState::Alert;
    ViewSel want{PillModule(), View::Pill};
    if (state == IState::Expanded) want = {dashes[g_tab], View::Dashboard};
    else if (state == IState::Alert) want = {TopAlert(), View::Alert};

    const float s = Scale();
    const Size2 size = want.m->Measure(want.v);
    const float tw = std::min(std::max(size.w, 40.0f), kIslandMaxW) * s;
    const float th = std::min(std::max(size.h, 20.0f), kIslandMaxH) * s;

    const bool wings = IslandOnBar() && (state == IState::Expanded || PanelWingsWanted() || now < g_wingHold);  // The buttons belong to the bar's monitor.
    if (!g_isInit) {
        g_isInit = true;
        g_isW.eps = g_isH.eps = 0.25f;  // Pixels.
        g_isW.epsV = g_isH.epsV = 5.0f;
        g_isW.Snap(tw);
        g_isH.Snap(th);
        g_wing.Snap(wings ? 1.0f : 0.0f);
        g_wingT = g_wing.x;
        g_mix.Snap(1.0f);
        g_viewCur = want;
        g_curSize = size;
        g_viewPrev = ViewSel{};
    } else {
        AimIfChanged(g_isW, tw, tw > g_isW.x ? kSpringExpandK : kSpringCollapseK, tw > g_isW.x ? kSpringExpandC : kSpringCollapseC);
        AimIfChanged(g_isH, th, th > g_isH.x ? kSpringExpandK : kSpringCollapseK, th > g_isH.x ? kSpringExpandC : kSpringCollapseC);
        AimIfChanged(g_wing, wings ? 1.0f : 0.0f, 320.0f, 34.0f);
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

    // Where the body will stand once settled, for the pointer tests.
    const float cx = (g_isMon.left + g_isMon.right) / 2.0f;
    g_isTarget = {(LONG)roundf(cx - tw / 2.0f), g_isMon.top, (LONG)roundf(cx + tw / 2.0f), g_isMon.top + (LONG)roundf(th)};

    if (state == IState::Expanded) SetTimer(g_hwnd, kZoneTimer, kZonePollMs, nullptr);
    else KillTimer(g_hwnd, kZoneTimer);

    ULONGLONG next = 0;
    for (const AlertItem& a : g_alerts) if (a.expires && (!next || a.expires < next)) next = a.expires;
    if (next) SetTimer(g_hwnd, kAlertTimer, (UINT)std::max<ULONGLONG>(next - now, 10) + 5, nullptr);
    else KillTimer(g_hwnd, kAlertTimer);
    if (now < g_wingHold) SetTimer(g_hwnd, kWingTimer, (UINT)(g_wingHold - now) + 30, nullptr);
    else KillTimer(g_hwnd, kWingTimer);

    g_isDirty = true;
    if (!IslandSettled()) {
        g_lastFrame = 0;
        g_clock.Wake();
    }
    RequestPaint();
}

void IslandRescale() {  // DPI changed: sizes are pixels, so start over from the new scale.
    g_isInit = false;
    IslandRecompute();
}

void IslandMoved() {  // The bar moved (monitor, work area): the notch follows rcMonitor.
    g_isDirty = true;
    RequestPaint();
}

void SetExpanded(bool expanded) {
    if (g_expanded == expanded) return;
    g_expanded = expanded;
    g_leaveSince = 0;
    IslandRecompute();
}

// Fullscreen: forget the pointer and any open state, and hide. IslandRecompute brings it back.
void IslandReset() {
    g_expanded = false;
    g_dwelling = false;
    g_isTracking = false;
    g_leaveSince = 0;
    g_wingHold = 0;
    if (g_hwnd) {
        KillTimer(g_hwnd, kDwellTimer);
        KillTimer(g_hwnd, kZoneTimer);
        KillTimer(g_hwnd, kWingTimer);
    }
    g_isInit = false;
    g_wing.Snap(0.0f);
    g_wingT = 0.0f;
    if (g_isHwnd) ShowWindow(g_isHwnd, SW_HIDE);
}

// Every 50 ms while expanded: is the pointer in the island, or in the strip band the buttons run along?
void IslandZoneTick() {
    if (g_isState != IState::Expanded) return;
    POINT p;
    GetCursorPos(&p);
    const float s = Scale();
    RECT zone = g_isTarget;
    InflateRect(&zone, (int)ceilf(kNotchShoulder * s), 0);
    bool in = PtInRect(&zone, p) != FALSE;
    // The strip band from the left edge of the bell to the right edge of the calendar keeps hover alive across the gaps.
    int lo = zone.left, hi = zone.right;
    if (IslandOnBar() && g_wingOk[0]) lo = std::min<int>(lo, g_monitor.left + (int)g_wingX[0]);
    if (IslandOnBar() && g_wingOk[1]) hi = std::max<int>(hi, g_monitor.left + (int)(g_wingX[1] + g_wingSize));
    const RECT band{lo, g_isMon.top, hi, g_isMon.top + (LONG)(kBarH * s + 0.5f)};
    if (PtInRect(&band, p)) in = true;
    // Over the settled body, a window that covers it (a panel, a popup) counts as leaving.
    if (in && IslandSettled() && PtInRect(&g_isTarget, p)) {
        HWND at = WindowFromPoint(p);
        if (at != g_isHwnd && at != g_hwnd) in = false;
    }
    if (in || g_pinned) {
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
    auto drawView = [&](const ViewSel& v, const Size2& sz, float alpha) {
        if (!v.m || alpha <= 0.01f) return;
        const float vw = sz.w * s, vh = sz.h * s;
        DrawCtx c{rt, brush, D2D1::RectF(cx - vw / 2.0f, 0, cx + vw / 2.0f, vh), alpha, s, now, g_frameDt, v.v};
        v.m->Draw(c);
        if (v == g_viewCur) g_isView = c.rect;
    };
    const float mix = Clamp01(g_mix.x);
    if (mix < 1.0f) drawView(g_viewPrev, g_prevSize, 1.0f - mix);
    drawView(g_viewCur, g_curSize, mix);
    if (g_viewCur.v == View::Dashboard) {
        const int n = (int)Dashboards().size();
        if (n > 1) {
            const float step = 10.0f * s, y = B - 10.0f * s;
            for (int i = 0; i < n; i++) {
                D2D1_COLOR_F c = kFg;
                c.a *= mix * (i == g_tab ? 1.0f : 0.35f);
                brush->SetColor(c);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + (i - (n - 1) / 2.0f) * step, y), 2.5f * s, 2.5f * s), brush);
            }
        }
    }
    rt->PopAxisAlignedClip();

    if (!g_isSurface.End()) {
        g_isBrush.Reset();
        g_isDirty = true;
        RequestPaint();
        return;
    }
    const int x0 = (g_isMon.left + g_isMon.right) / 2 - stageW / 2;
    g_isCropX = cl;
    g_isBody = {x0 + (LONG)L, g_isMon.top, x0 + (LONG)R, g_isMon.top + (LONG)B};
    g_isSurface.PresentCrop(g_isHwnd, POINT{x0 + cl, g_isMon.top}, POINT{cl, 0}, SIZE{cr - cl, cb});
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
            if (!g_expanded && !g_covered && !g_dwelling && !g_fullscreen) {
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
                    const int tab = std::min(std::max(g_tab - steps, 0), std::max(count - 1, 0));
                    if (tab != g_tab) {
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
                if (IModule* m = TopAlert()) m->OnClickAlert();
            } else {
                IslandPointer(PointerEvent::Up, lp, true);
            }
            return 0;
        case WM_RBUTTONUP:
            ShowBarMenu();
            return 0;
        case WM_DESTROY:
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

void Services::OpenPanel(PanelKind kind, int anchorX) {
    g_anchorX = anchorX;
    g_anchorTick = GetTickCount64();
    SetPropW(g_hwnd, L"TopBarAnchorX", reinterpret_cast<HANDLE>((INT_PTR)anchorX));
    SetPropW(g_hwnd, L"TopBarAnchorTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
    TogglePanel(kind == PanelKind::Notifications ? kPanelNotifications : kPanelCalendar);
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

#line 1 "bar/window.inc"
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
    g_wing.Step(dt);
    g_mix.Step(dt);
    const float wingT = fmaxf(g_wing.x, 0.0f);
    const bool wingMoved = wingT != g_wingT;
    g_wingT = wingT;
    if (IslandSettled() && !ModulesAnimating()) {
        g_clock.Sleep();
        g_lastFrame = 0;
    }
    g_isDirty = true;
    if (wingMoved) InvalidateLayout();  // Paint adds the old and new place of both buttons.
    Paint();
    IslandPaint();
    if (wingMoved) ResyncHover();
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
                    if (g_hits[index].action == Action::Notifications || g_hits[index].action == Action::Calendar) {
                        // Keep the buttons out while the panel opens, even if the pointer has left the island.
                        g_wingHold = GetTickCount64() + kWingHoldMs;
                        IslandRecompute();
                    }
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
            } else if (wp == kDwellTimer) {
                IslandDwellTick();
            } else if (wp == kZoneTimer) {
                IslandZoneTick();
            } else if (wp == kAlertTimer || wp == kWingTimer) {
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

    RegisterBar();
    IslandInit();
    ApplyMonitor();
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
    IslandShutdown();
    ReleaseGfx();
    CoUninitialize();
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
