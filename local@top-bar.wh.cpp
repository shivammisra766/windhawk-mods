// ==WindhawkMod==
// @id              top-bar
// @name            Top bar
// @description     Top bar with three bubbles in the Seelen toolbar style, drawn with Direct2D
// @version         3.2
// @author          local
// @include         windhawk.exe
// @compilerOptions -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32 -lversion -lwininet -liphlpapi -luuid -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lruntimeobject -lsetupapi -loleaut32
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
  $description: A code such as de, fr, hi, ja. Leave empty to follow Windows.
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
constexpr UINT_PTR kWingTimer = 12;    // End of the hold that keeps bell and calendar out while a panel opens.
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
    float Measure(const std::wstring& text) {
        Com<IDWriteTextLayout> layout = Layout(text);
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
};

Settings g_settings;

Settings LoadSettings() {
    Settings s;
    if (PCWSTR v = Wh_GetStringSetting(L"fontFamily")) { s.fontFamily = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"language")) { s.language = v; Wh_FreeStringSetting(v); }
    const int scale = Wh_GetIntSetting(L"fontScale");
    s.fontScale = scale >= 50 && scale <= 200 ? scale : 100;
    return s;
}

#line 1 "core/loc.inc"

// ---- Loc: strings per language, generated from strings.tsv by build-bar.ps1 ----

constexpr int kLocLangCount = 12;
const wchar_t* const kLocLangs[kLocLangCount] = { L"en", L"hi", L"zh", L"ja", L"ko", L"de", L"es", L"fr", L"pt", L"ru", L"ar", L"it" };
struct LocEntry { const wchar_t* key; const wchar_t* text[kLocLangCount]; };
const LocEntry kLoc[] = {
    { L"window.title", { L"Top bar", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"" } },
};

// Language of the current settings: the chosen one, else the Windows display language, else English.
const wchar_t* Loc(const wchar_t* key) {
    static std::wstring cachedFor;
    static int column = 0;
    std::wstring want = g_settings.language;
    if (want.empty()) {
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
        GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
        want = name;
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

void ReadForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg || IsShellWindow(fg)) {
        return;
    }
    wchar_t title[256] = {};
    GetWindowTextW(fg, title, 256);
    std::wstring path;
    g_appName = AppInfo(fg, &path);
    g_title = g_appName == title ? L"" : title;
    LoadAppIcon(path);
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
    if (!g_hwnd || g_fullscreen || !g_dirtyAny) {
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

std::wstring ClockText() {
    wchar_t timeText[32] = {};
    wchar_t dateText[32] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"hh':'mm tt", timeText, 32);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"ddd d MMM", dateText, 32, nullptr);
    return std::wstring(dateText) + L", " + timeText;
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

// The collapsed content: date and time.
class ClockModule : public IModule {
public:
    const wchar_t* Id() const override { return L"clock"; }
    Size2 Measure(View) override { return {ToDip(g_text.Measure(ClockText())) + 2 * kPillPadX + AlertsDotsReserve(), kNotchH}; }
    void Draw(const DrawCtx& c) override { DrawCentredText(c, ClockText(), c.rect); }
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
IModule* AlertsModuleInstance();  // topbar/alerts/alerts.inc
IModule* const g_modules[] = {&g_clockModule, AlertsModuleInstance(), &g_page1, &g_page2};

#line 1 "bar/placement.inc"
// ---- Bar placement and panels ----

void IslandMoved();
void IslandReset();
void IslandRecompute();

void PlaceBar() {
    if (!g_hwnd) {
        return;
    }
    HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    g_monitor = mi.rcMonitor;
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
    const bool fullscreen = g_abnFullscreen || ForegroundCoversMonitor();
    if (fullscreen == g_fullscreen) {
        return;
    }
    g_fullscreen = fullscreen;
    AlertsFullscreenChanged();
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
        InvalidateAll();
    }
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

HWND g_isHwnd = nullptr;
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

IState g_isState = IState::Collapsed;
Spring g_cover;         // 0 island shown, 1 faded out under the native volume popup (P5b).
Spring g_isW, g_isH;    // Body size, pixels.
Spring g_wing;          // 0 bell and calendar parked, 1 out.
Spring g_mix;           // Cross-fade between g_viewPrev and g_viewCur.
ViewSel g_viewCur, g_viewPrev;
Size2 g_curSize{}, g_prevSize{};  // Measure() of each, taken when state changes.
double g_frameDt = 0;
bool g_isInit = false;
bool g_expanded = false;   // Hover (after the dwell) or pinned.
bool g_pinned = false;     // Reserved for the menu (P5c).
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
    return g_isW.Settled() && g_isH.Settled() && g_wing.Settled() && g_mix.Settled() && g_cover.Settled();
}

// Derives everything from the inputs: alerts, hover, tab, panel. Called when an input changes, never per frame.
void IslandRecompute() {
    if (!g_isHwnd || g_fullscreen || !EnsureGfx()) return;
    PrepareText();
    const ULONGLONG now = GetTickCount64();
    g_alerts.erase(std::remove_if(g_alerts.begin(), g_alerts.end(), [&](const AlertItem& a) { return a.expires && a.expires <= now; }),
                   g_alerts.end());
    const std::vector<IModule*> dashes = Dashboards();
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

    const bool wings = state == IState::Expanded || PanelWingsWanted() || now < g_wingHold;
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
    AimIfChanged(g_cover, (g_covered && state != IState::Expanded) ? 1.0f : 0.0f, 900.0f, 60.0f);  // About 80 ms.

    // Where the body will stand once settled, for the pointer tests.
    const float cx = (g_monitor.left + g_monitor.right) / 2.0f;
    g_isTarget = {(LONG)roundf(cx - tw / 2.0f), g_monitor.top, (LONG)roundf(cx + tw / 2.0f), g_monitor.top + (LONG)roundf(th)};

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
    if (g_wingOk[0]) lo = std::min<int>(lo, g_monitor.left + (int)g_wingX[0]);
    if (g_wingOk[1]) hi = std::max<int>(hi, g_monitor.left + (int)(g_wingX[1] + g_wingSize));
    const RECT band{lo, g_monitor.top, hi, g_monitor.top + (LONG)(kBarH * s + 0.5f)};
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
    if (g_fullscreen || !g_isInit || !EnsureGfx()) return;
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
    AlertsDrawOverlay(rt, brush, L, R, B, s);

    if (!g_isSurface.End()) {
        g_isBrush.Reset();
        g_isDirty = true;
        RequestPaint();
        return;
    }
    const int x0 = (g_monitor.left + g_monitor.right) / 2 - stageW / 2;
    g_isCropX = cl;
    g_isBody = {x0 + (LONG)L, g_monitor.top, x0 + (LONG)R, g_monitor.top + (LONG)B};
    g_isSurface.PresentCrop(g_isHwnd, POINT{x0 + cl, g_monitor.top}, POINT{cl, 0}, SIZE{cr - cl, cb},
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
// Not built here, by decision: Progress, Num Lock, privacy-dot tap, the 64-bit "DND off" helper.

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

enum class AK { Caps, Battery, Bluetooth, Device, Dnd, Notification, Clipboard };

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
    bool mic_ = false, cam_ = false;
    std::wstring micApp_, camApp_;
    HPOWERNOTIFY powerA_ = nullptr, powerB_ = nullptr;
    HWINEVENTHOOK osdHook_ = nullptr;
    std::vector<HANDLE> threads_;
    RECT lastGeo_{};
    int osdLogs_ = 0;
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
    if (!visible_ || hovered_ || g_covered) return;  // Waiting, reading, or under the volume popup.
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
    it.sub = value ? AlT(L"alert.dnd.tap", L"Tap to open settings") : L"";
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

// ---- Island-facing parts ----

Size2 AlertsModule::Measure(View v) {
    if (v != View::Alert || !hasCur_) return {40.0f, kNotchH};
    float w = 300.0f;
    switch (cur_.kind) {
        case AK::Caps: w = 210.0f; break;
        case AK::Notification: w = 360.0f; break;
        case AK::Clipboard: w = 340.0f; break;
        case AK::Device: w = 300.0f; break;
        default: break;
    }
    return {w + 2.0f * AlertsDotsReserve(), hovered_ ? 66.0f : 44.0f};
}

bool AlertsModule::OnPointer(const PointerEvent& e, View v) {
    if (v != View::Alert) return false;
    if (e.kind == PointerEvent::Move && !hovered_) { hovered_ = true; IslandRecompute(); }
    else if (e.kind == PointerEvent::Leave && hovered_) { hovered_ = false; IslandRecompute(); }
    return false;
}

void AlertsModule::Draw(const DrawCtx& c) {
    if (c.view != View::Alert || !hasCur_) return;
    const float s = c.scale;
    const D2D1_RECT_F r = c.rect;
    const float badge = 28.0f * s, bx = r.left + 10.0f * s, by = r.top + 8.0f * s;
    auto col = [&](D2D1_COLOR_F k, float a = 1.0f) { k.a *= a * c.alpha; c.brush->SetColor(k); };

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
    if (!hasCur_) return;
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
        case AK::Dnd: Launch(L"ms-settings:quietmomentshome"); break;  // No 64-bit helper: see tray-bridge notes.
        case AK::Clipboard: SendWinKey('V'); break;
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
    if (m.osdLogs_++ < 40) {
        Wh_Log(L"alerts: XamlExplorerHostIslandWindow %s rect %ld,%ld-%ld,%ld (%ldx%ld) dpi %u", event == EVENT_OBJECT_SHOW ? L"SHOW" : L"HIDE",
               r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top, GetDpiForWindow(hwnd));
    }
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
        if (lp) OsdShown(true);
        else osdSeen_ = true;
        return 0;
    }
    switch (msg) {
        case WM_AL_CAPS: OnCaps(lp != 0); return 0;
        case WM_AL_PRIVACY: SetTimer(hwnd, kAlPrivacy, 250, nullptr); return 0;
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
    g_alStop = g_alWake = nullptr;
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
void AlertsFullscreenChanged() {
    if (g_alMod) g_alMod->FullscreenChanged();
}
float AlertsDotsReserve() {
    return g_alMod ? 12.0f * g_alMod->Dots() : 0.0f;
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
    g_cover.Step(dt);
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
                g_isDirty = true;
                IslandRecompute();  // The text may be wider now.
            } else if (wp == kTitleTimer) {
                KillTimer(hwnd, kTitleTimer);
                g_titlePending = false;
                ReadForeground();
                InvalidateAll();
                ResyncHover();
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
            ReadForeground();
            InvalidateAll();
            IslandRecompute();  // A panel opening or closing moves the buttons.
            return 0;
        case WM_SETTINGCHANGE:
            ReadAccent();
            ReadForeground();
            InvalidateAll();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PlaceBar();
            IslandRescale();
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
    IslandInit();
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
