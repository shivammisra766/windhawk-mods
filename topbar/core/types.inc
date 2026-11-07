
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
constexpr UINT_PTR kDropTimer = 14;     // Dwell before a drag over the bar opens the shelf.
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
