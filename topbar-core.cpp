// ==WindhawkMod==
// @id              top-bar
// @name            Top bar
// @description     Top bar in the Seelen toolbar style with a built-in pill
// @version         2.1
// @author          local
// @include         windhawk.exe
// @compilerOptions -lgdiplus -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32 -lversion -lwininet -liphlpapi
// ==/WindhawkMod==

// Runs as a tool mod in its own process. Nothing polls: the bar redraws on Windows
// events, one timer per minute for the clock, and a short timer only while the pill moves.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <sddl.h>
#include <wininet.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

// From iphlpapi. Windhawk includes windows.h first, which rules out winsock2.h,
// so these two are declared directly.
extern "C" DWORD WINAPI NotifyIpInterfaceChange(USHORT family, void* callback, void* context,
                                                BOOLEAN initialNotification, HANDLE* handle);
extern "C" DWORD WINAPI CancelMibChangeNotify2(HANDLE handle);

namespace {

using namespace Gdiplus;

//@@ICONS@@

constexpr wchar_t kClassName[] = L"WindhawkTopBar";
constexpr UINT WM_APPBAR_CB = WM_APP + 1;
constexpr UINT WM_REFRESH_NET = WM_APP + 3;
constexpr UINT WM_REFRESH_FOREGROUND = WM_APP + 4;
constexpr UINT WM_PROFILE_DONE = WM_APP + 5;  // From start-menu-pop. wParam 1: Start is still open.
constexpr UINT_PTR kClockTimer = 1;
constexpr UINT_PTR kAnimTimer = 2;
constexpr UINT_PTR kPanelTimer = 3;
constexpr UINT_PTR kFullscreenTimer = 4;
constexpr UINT_PTR kReopenTimer = 5;
constexpr UINT_PTR kQuickCloseTimer = 6;
constexpr UINT_PTR kQuickPageTimer = 7;
constexpr UINT_PTR kQuickReopenTimer = 8;

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
constexpr float kBarPadX = 16.0f;
constexpr float kGap = 4.0f;
constexpr float kItemRadius = 4.8f;
constexpr float kFontSize = 12.8f;
constexpr float kPillH = 24.0f;
constexpr float kPillPadX = 12.0f;
constexpr float kPillButton = 20.0f;
constexpr int kPillAnimMs = 200;
constexpr float kPanelGap = 8.0f;  // Between the bar and a native panel.
constexpr float kPanelEdge = 8.0f;  // Keeps a panel this far from the screen edge.
constexpr float kPanelDrop = 10.0f;  // A panel drops this far into place as it opens.
constexpr int kPanelDropMs = 140;
constexpr int kPanelAnchorMs = 1500;  // A click anchors the panel it opens for this long.

// Seelen's dark tokens: gray-50 at 90%, gray-900.
const Color kBarBg(230, 24, 24, 24);
const Color kFg(255, 228, 228, 228);

const GUID kGuidBatteryPercent = {0xA7AD8041, 0xB45A, 0x4CAE, {0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1}};
const GUID kGuidAcDc = {0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};

enum class Action { None, QuickSettings, Wifi, Bluetooth, TrayIcon, Notifications, Calendar, Profile };

struct Hit {
    RECT rect;
    Action action;
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
    Bitmap* bitmap = nullptr;
};

HWND g_hwnd = nullptr;
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
ULONG_PTR g_gdiplusToken = 0;
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
int g_hoverIndex = -1;
std::vector<Hit> g_hits;

// Pill animation: g_pillT runs from 0 (clock only) to 1 (buttons shown).
float g_pillT = 0.0f;
float g_pillFrom = 0.0f;
bool g_pillTarget = false;
ULONGLONG g_pillStart = 0;
RECT g_pillRect{};

HWINEVENTHOOK g_foregroundHook = nullptr;
HWINEVENTHOOK g_titleHook = nullptr;
HANDLE g_netNotify = nullptr;
HPOWERNOTIFY g_powerA = nullptr;
HPOWERNOTIFY g_powerB = nullptr;
Bitmap* g_avatar = nullptr;
Bitmap* g_appIcon = nullptr;
std::wstring g_appIconPath;
std::vector<GraphicsPath*> g_iconPaths;

std::wstring g_appName;
std::wstring g_title;
Color g_accentBase(255, 0, 120, 212);
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
ULONGLONG g_trayBroadcastTick = 0;

float Scale() {
    return g_hwnd ? GetDpiForWindow(g_hwnd) / 96.0f : 1.0f;
}

// ---- SVG path data to GDI+ ----

struct PathReader {
    const char* p;
    void Skip() {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    bool HasNumber() {
        Skip();
        return *p == '-' || *p == '+' || *p == '.' || (*p >= '0' && *p <= '9');
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

void AddArc(GraphicsPath& out, float x0, float y0, float rx, float ry, float rot, bool large, bool sweep, float x1,
            float y1) {
    if (rx == 0 || ry == 0) {
        out.AddLine(x0, y0, x1, y1);
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
        out.AddBezier((float)px, (float)py, mapX(c1x, c1y), mapY(c1x, c1y), mapX(c2x, c2y), mapY(c2x, c2y), ex, ey);
        px = ex;
        py = ey;
    }
}

GraphicsPath* ParseSvgPath(const char* d) {
    auto* out = new GraphicsPath(FillModeWinding);
    PathReader r{d};
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
        switch (cmd | 0x20) {
            case 'm': {
                x = bx + r.Num();
                y = by + r.Num();
                sx = x;
                sy = y;
                out->StartFigure();
                cmd = rel ? 'l' : 'L';
                prev = 'm';
                continue;
            }
            case 'l': {
                float nx = bx + r.Num(), ny = by + r.Num();
                out->AddLine(x, y, nx, ny);
                x = nx;
                y = ny;
                break;
            }
            case 'h': {
                float nx = bx + r.Num();
                out->AddLine(x, y, nx, y);
                x = nx;
                break;
            }
            case 'v': {
                float ny = by + r.Num();
                out->AddLine(x, y, x, ny);
                y = ny;
                break;
            }
            case 'c': {
                float x1 = bx + r.Num(), y1 = by + r.Num();
                float x2 = bx + r.Num(), y2 = by + r.Num();
                float nx = bx + r.Num(), ny = by + r.Num();
                out->AddBezier(x, y, x1, y1, x2, y2, nx, ny);
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
                out->AddBezier(x, y, x1, y1, x2, y2, nx, ny);
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
                out->AddBezier(x, y, x + 2.0f / 3.0f * (qx - x), y + 2.0f / 3.0f * (qy - y),
                               nx + 2.0f / 3.0f * (qx - nx), ny + 2.0f / 3.0f * (qy - ny), nx, ny);
                x = nx;
                y = ny;
                break;
            }
            case 'a': {
                float rx = r.Num(), ry = r.Num(), rot = r.Num();
                bool large = r.Flag() != 0, sweep = r.Flag() != 0;
                float nx = bx + r.Num(), ny = by + r.Num();
                AddArc(*out, x, y, rx, ry, rot, large, sweep, nx, ny);
                x = nx;
                y = ny;
                break;
            }
            case 'z': {
                out->CloseFigure();
                x = sx;
                y = sy;
                prev = 'z';
                continue;
            }
            default:
                return out;  // Unknown command: keep what was parsed.
        }
        prev = cmd | 0x20;
    }
    return out;
}

GraphicsPath* PartPath(int icon, int part) {
    int index = 0;
    for (int i = 0; i < icon; i++) index += kIcons[i].count;
    index += part;
    if ((int)g_iconPaths.size() <= index) g_iconPaths.resize(index + 1, nullptr);
    if (!g_iconPaths[index]) g_iconPaths[index] = ParseSvgPath(kIcons[icon].parts[part].d);
    return g_iconPaths[index];
}

float IconWidth(IconId id, float h) {
    return kIcons[id].w / kIcons[id].h * h;
}

void DrawIcon(Graphics& g, IconId id, float cx, float cy, float h, const Color& color) {
    const IconDef& def = kIcons[id];
    const float k = h / def.h;
    GraphicsState state = g.Save();
    g.TranslateTransform(cx - def.w * k / 2.0f, cy - h / 2.0f);
    g.ScaleTransform(k, k);
    for (int i = 0; i < def.count; i++) {
        GraphicsPath* path = PartPath(id, i);
        if (def.parts[i].fill) {
            SolidBrush brush(color);
            g.FillPath(&brush, path);
        }
        if (def.parts[i].stroke > 0) {
            Pen pen(color, def.parts[i].stroke);
            pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
            pen.SetLineJoin(LineJoinRound);
            g.DrawPath(&pen, path);
        }
    }
    g.Restore(state);
}

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
        g_accentBase = Color(255, palette[12], palette[13], palette[14]);  // Base tone: Seelen's hover overlay.
    }
    RegCloseKey(key);
}

Color Overlay(BYTE alpha) {
    return Color(alpha, g_accentBase.GetR(), g_accentBase.GetG(), g_accentBase.GetB());
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

Bitmap* BitmapFromIcon(HICON icon) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        return nullptr;
    }
    Bitmap* result = nullptr;
    BITMAP bm{};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bm), &bm)) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<DWORD> pixels(bm.bmWidth * bm.bmHeight);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, info.hbmColor, 0, bm.bmHeight, pixels.data(), &bi, DIB_RGB_COLORS)) {
            bool hasAlpha = false;
            for (DWORD px : pixels) {
                if (px >> 24) {
                    hasAlpha = true;
                    break;
                }
            }
            if (!hasAlpha) {
                for (DWORD& px : pixels) {
                    if (px) px |= 0xFF000000;
                }
            }
            result = new Bitmap(bm.bmWidth, bm.bmHeight, PixelFormat32bppARGB);
            Rect rect(0, 0, bm.bmWidth, bm.bmHeight);
            BitmapData data{};
            if (result->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppARGB, &data) == Ok) {
                for (int y = 0; y < bm.bmHeight; y++) {
                    memcpy(static_cast<BYTE*>(data.Scan0) + y * data.Stride, &pixels[y * bm.bmWidth], bm.bmWidth * 4);
                }
                result->UnlockBits(&data);
            }
        }
        ReleaseDC(nullptr, dc);
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return result;
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
        g_appIcon = BitmapFromIcon(info.hIcon);
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
    }
    CloseHandle(proc);
    return result;
}

bool IsShellWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
           !wcscmp(cls, L"Windows.UI.Core.CoreWindow") || !wcscmp(cls, L"ControlCenterWindow") ||
           !wcscmp(cls, L"XamlExplorerHostIslandWindow") || !wcscmp(cls, kClassName);
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
        auto* image = new Bitmap(path);
        if (image->GetLastStatus() == Ok) {
            g_avatar = image;
        } else {
            delete image;
        }
    }
}

void AddRounded(GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// Seelen's own thresholds for these icons.
IconId BatteryIcon() {
    if (g_battery > 90) return I_batfull;
    if (g_battery > 66) return I_bathigh;
    if (g_battery > 33) return I_batmed;
    if (g_battery > 5) return I_batlow;
    return I_batwarn;
}

// Draws into a premultiplied DIB and pushes it to a layered window.
template <typename Draw>
void Present(HWND hwnd, Draw draw) {
    RECT wr;
    GetWindowRect(hwnd, &wr);
    const int W = wr.right - wr.left;
    const int H = wr.bottom - wr.top;
    if (W <= 0 || H <= 0) {
        return;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap target(W, H, W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&target);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        draw(g, W, H);
    }
    POINT src{0, 0};
    POINT dst{wr.left, wr.top};
    SIZE size{W, H};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

const FontFamily* TextFamily(FontFamily& preferred, FontFamily& fallback) {
    return preferred.IsAvailable() ? &preferred : &fallback;
}

void Paint() {
    if (!g_hwnd || g_fullscreen) {
        return;
    }
    const float s = Scale();
    Present(g_hwnd, [&](Graphics& g, int W, int H) {
        g.Clear(kBarBg);

        SolidBrush fg(kFg);
        SolidBrush hoverFill(Overlay(51));
        FontFamily preferred(L"Segoe UI Variable Text");
        FontFamily fallback(L"Segoe UI");
        const FontFamily* family = TextFamily(preferred, fallback);
        Font text(family, kFontSize * s, FontStyleRegular, UnitPixel);

        StringFormat tf(StringFormat::GenericTypographic());
        tf.SetFormatFlags(tf.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
        tf.SetLineAlignment(StringAlignmentCenter);
        StringFormat tfEllipsis(&tf);
        tfEllipsis.SetTrimming(StringTrimmingEllipsisCharacter);

        auto measure = [&](const std::wstring& str) {
            RectF box;
            g.MeasureString(str.c_str(), -1, &text, PointF(0, 0), &tf, &box);
            return box.Width;
        };

        const float pad = kItemPad * s;
        const float itemY = (H - kPillH * s) / 2.0f;
        const float itemH = kPillH * s;
        const float cy = H / 2.0f;
        const float icon = kItem * s;

        g_hits.clear();
        auto addHit = [&](float x, float y, float w, float h, Action action, float radius) {
            g_hits.push_back({{(LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h)}, action});
            if ((int)g_hits.size() - 1 == g_hoverIndex) {
                GraphicsPath p;
                AddRounded(p, x, y, w, h, radius);
                g.FillPath(&hoverFill, &p);
            }
        };

        // Right group, in the order of the Seelen layout, laid out from the right edge.
        float right = W - kBarPadX * s;
        auto iconItem = [&](IconId id, Action action) {
            const float w = IconWidth(id, icon) + 2 * pad;
            right -= w;
            addHit(right, itemY, w, itemH, action, kItemRadius * s);
            DrawIcon(g, id, right + w / 2.0f, cy, icon, kFg);
            right -= kGap * s;
        };

        iconItem(I_settings, Action::QuickSettings);
        if (g_battery >= 0) {
            wchar_t pct[8];
            swprintf_s(pct, L"%d%%", g_battery);
            const std::wstring label = pct;
            const float space = measure(L" ");
            const float labelW = measure(label);
            const IconId battery = BatteryIcon();
            const float batteryW = IconWidth(battery, icon);
            const float boltW = g_charging ? IconWidth(I_bolt, icon) + space : 0.0f;
            const float w = boltW + batteryW + space + labelW + 2 * pad;
            right -= w;
            addHit(right, itemY, w, itemH, Action::QuickSettings, kItemRadius * s);
            float x = right + pad;
            if (g_charging) {
                DrawIcon(g, I_bolt, x + IconWidth(I_bolt, icon) / 2.0f, cy, icon, kFg);
                x += boltW;
            }
            DrawIcon(g, battery, x + batteryW / 2.0f, cy, icon, kFg);
            x += batteryW + space;
            g.DrawString(label.c_str(), -1, &text, RectF(x, itemY, labelW + 1, itemH), &tf, &fg);
            right -= kGap * s;
        }
        iconItem(g_connected ? I_wifi : I_offline, Action::Wifi);
        iconItem(I_bluetooth, Action::Bluetooth);

        // Tray icons of background apps, as in the Seelen toolbar.
        for (int i = (int)g_tray.size() - 1; i >= 0; i--) {
            if (g_tray[i].hidden || !g_tray[i].bitmap) {
                continue;
            }
            const float w = icon + 2 * pad;
            right -= w;
            addHit(right, itemY, w, itemH, Action::TrayIcon, kItemRadius * s);
            g_hits.back().data = i;
            g.DrawImage(g_tray[i].bitmap, right + pad, cy - icon / 2.0f, icon, icon);
            right -= kGap * s;
        }

        // Centre pill: part of the bar, clock fixed in the middle, grows outward.
        wchar_t timeText[32] = {};
        wchar_t dateText[32] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"hh':'mm tt", timeText, 32);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"ddd d MMM", dateText, 32, nullptr);
        const std::wstring clock = std::wstring(dateText) + L", " + timeText;
        const float clockW = measure(clock);
        const float t = g_pillT;
        const float button = kPillButton * s;
        const float grow = (button + kGap * s) * t;
        const float pillW = clockW + 2 * kPillPadX * s + 2 * grow;
        const float pillX = (W - pillW) / 2.0f;
        const float pillH = kPillH * s;
        const float pillY = (H - pillH) / 2.0f;
        g_pillRect = {(LONG)pillX, 0, (LONG)(pillX + pillW + 1), H};
        {
            SolidBrush pillBrush(Color(255, 0, 0, 0));
            GraphicsPath pill;
            AddRounded(pill, pillX, pillY, pillW, pillH, pillH / 2.0f);
            g.FillPath(&pillBrush, &pill);
        }
        g.DrawString(clock.c_str(), -1, &text, RectF((W - clockW) / 2.0f, pillY, clockW + 1, pillH), &tf, &fg);
        if (t > 0.01f) {
            const BYTE alpha = (BYTE)(255 * t);
            const Color iconColor(alpha, kFg.GetR(), kFg.GetG(), kFg.GetB());
            const float buttonY = pillY + (pillH - button) / 2.0f;
            const float inset = (pillH - button) / 2.0f;
            auto pillButton = [&](float x, IconId id, Action action, float iconH) {
                if (t > 0.5f) {
                    addHit(x, buttonY, button, button, action, button / 2.0f);
                }
                DrawIcon(g, id, x + button / 2.0f, pillY + pillH / 2.0f, iconH, iconColor);
            };
            pillButton(pillX + inset, I_bell, Action::Notifications, 15.0f * s);
            pillButton(pillX + pillW - inset - button, I_calendar, Action::Calendar, 13.0f * s);
        }

        // Left group: user, separator, app, separator, title.
        float left = kBarPadX * s;
        const float slot = icon + 2 * pad;
        addHit(left, itemY, slot, itemH, Action::Profile, kItemRadius * s);
        const float avatarX = left + pad;
        const float avatarY = cy - icon / 2.0f;
        if (g_avatar) {
            GraphicsPath clip;
            AddRounded(clip, avatarX, avatarY, icon, icon, kItemRadius * s);
            GraphicsState state = g.Save();
            g.SetClip(&clip);
            g.DrawImage(g_avatar, avatarX, avatarY, icon, icon);
            g.Restore(state);
        } else {
            FontFamily iconFamily(L"Segoe Fluent Icons");
            Font personFont(&iconFamily, icon, FontStyleRegular, UnitPixel);
            StringFormat centered;
            centered.SetAlignment(StringAlignmentCenter);
            centered.SetLineAlignment(StringAlignmentCenter);
            wchar_t person[2] = {0xE77B, 0};
            g.DrawString(person, 1, &personFont, RectF(left, itemY, slot, itemH), &centered, &fg);
        }
        left += slot + kGap * s;

        const float leftLimit = pillX - 12.0f * s;
        auto textItem = [&](const std::wstring& str) {
            const float w = measure(str);
            if (left + pad + w < leftLimit) {
                g.DrawString(str.c_str(), -1, &text, RectF(left + pad, itemY, w + 1, itemH), &tf, &fg);
            }
            left += w + 2 * pad + kGap * s;
        };

        textItem(L"|");
        if (!g_appName.empty()) {
            if (g_appIcon && left + pad + icon < leftLimit) {
                g.DrawImage(g_appIcon, left + pad, avatarY, icon, icon);
            }
            const float nameW = measure(g_appName);
            const float nameX = left + pad + icon + measure(L"  ");
            if (nameX + nameW < leftLimit) {
                g.DrawString(g_appName.c_str(), -1, &text, RectF(nameX, itemY, nameW + 1, itemH), &tf, &fg);
            }
            left = nameX + nameW + pad + kGap * s;
        }
        if (!g_title.empty()) {
            textItem(L"-");
            const float titleX = left + pad;
            if (titleX < leftLimit) {
                g.DrawString(g_title.c_str(), -1, &text, RectF(titleX, itemY, leftLimit - titleX, itemH), &tfEllipsis, &fg);
            }
        }
    });
}

// ---- Bar placement and panels ----

void PlaceBar() {
    if (!g_hwnd) {
        return;
    }
    HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
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
    if (fullscreen) {
        ShowWindow(g_hwnd, SW_HIDE);
    } else {
        PlaceBar();
        Paint();
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
            SendMessageTimeoutW(hwnd, g_panelMessage, mode, 0, SMTO_ABORTIFHUNG, 200, &result);
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
            delete g_tray[i].bitmap;
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
            delete icon->bitmap;
            g_tray.erase(g_tray.begin() + (icon - g_tray.data()));
        }
    } else if (m.message == NIM_ADD || m.message == NIM_MODIFY || m.message == NIM_SETVERSION) {
        if (!icon) {
            if (m.message == NIM_SETVERSION) {
                return;
            }
            g_tray.emplace_back();
            icon = &g_tray.back();
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
            if (Bitmap* bitmap = BitmapFromIcon(reinterpret_cast<HICON>((UINT_PTR)m.icon))) {
                delete icon->bitmap;
                icon->bitmap = bitmap;
            }
        }
    } else {
        return;
    }
    RemoveDeadTrayIcons();
    Paint();
}

// Passes a click to the app in the form it asked for, as the taskbar would.
void ClickTrayIcon(int index, bool rightButton) {
    if (index < 0 || index >= (int)g_tray.size()) {
        return;
    }
    const TrayIcon icon = g_tray[index];
    if (!IsWindow(icon.hwnd) || !icon.callback) {
        RemoveDeadTrayIcons();
        Paint();
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
            if (SendMessageTimeoutW(hwnd, g_profileMessage, 0, 0, SMTO_ABORTIFHUNG, 300, &result)) {
                armed = true;
            }
        }
    }
    if (armed) {
        g_profileOpen = true;
        SendWinKey(0);
    }
}


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
        PostMessageW(g_hwnd, WM_REFRESH_FOREGROUND, 0, 0);
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

void SetPillTarget(bool expanded) {
    if (expanded == g_pillTarget) {
        return;
    }
    g_pillTarget = expanded;
    g_pillFrom = g_pillT;
    g_pillStart = GetTickCount64();
    SetTimer(g_hwnd, kAnimTimer, 15, nullptr);
}

void StepPill() {
    const float x = fmin(1.0f, (float)(GetTickCount64() - g_pillStart) / kPillAnimMs);
    const float eased = 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
    const float target = g_pillTarget ? 1.0f : 0.0f;
    g_pillT = g_pillFrom + (target - g_pillFrom) * eased;
    if (x >= 1.0f) {
        g_pillT = target;
        KillTimer(g_hwnd, kAnimTimer);
    }
    Paint();
    if (g_tracking) {
        // The hit list moved with the pill: resync hover against the new layout.
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
        const int index = HitIndexAt(p);
        if (index != g_hoverIndex) {
            g_hoverIndex = index;
            Paint();
        }
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
        Paint();
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
            const int index = HitIndexAt(p);
            if (index != g_hoverIndex) {
                g_hoverIndex = index;
                Paint();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hoverIndex = -1;
            SetPillTarget(false);
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
                Paint();
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
                Paint();
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
            Paint();
            return TRUE;
        case WM_TIMER:
            if (wp == kClockTimer) {
                ArmClock();
                Paint();
            } else if (wp == kAnimTimer) {
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
            Paint();
            return 0;
        case WM_REFRESH_NET:
            ReadNetwork();
            Paint();
            return 0;
        case WM_REFRESH_FOREGROUND:
            ReadForeground();
            Paint();
            return 0;
        case WM_SETTINGCHANGE:
            ReadAccent();
            ReadForeground();
            Paint();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PlaceBar();
            Paint();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            RemovePropW(hwnd, L"TopBarAnchorX");
            RemovePropW(hwnd, L"TopBarAnchorTick");
            UnregisterBar();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI BarThread(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gdiInput;
    GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr);

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
    g_titleHook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, OnWinEvent, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    RegisterBar();
    ArmClock();
    Paint();

    // Apps add their tray icons again when told the taskbar is new, and the bridge sees that.
    g_trayBroadcastTick = GetTickCount64();
    PostMessageW(HWND_BROADCAST, g_taskbarCreated, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

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
        delete icon.bitmap;
    }
    g_tray.clear();
    for (GraphicsPath* path : g_iconPaths) {
        delete path;
    }
    g_iconPaths.clear();
    GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}

}  // namespace

BOOL WhTool_ModInit() {
    g_thread = CreateThread(nullptr, 0, BarThread, nullptr, 0, &g_threadId);
    return g_thread != nullptr;
}

void WhTool_ModSettingsChanged() {}

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

