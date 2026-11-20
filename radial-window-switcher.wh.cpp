// ==WindhawkMod==
// @id              radial-window-switcher
// @name            Radial Window Switcher
// @name:ru-RU      Круговой переключатель окон
// @description     A macOS-style radial Alt+Tab: app icons in a ring, a live window preview and smooth animations
// @description:ru-RU Круговой Alt+Tab в стиле macOS: иконки приложений по кругу, живое превью окна и плавные анимации
// @version         1.0.0
// @author          kotikdetka
// @github          https://github.com/kotikdetka
// @include         windhawk.exe
// @include         explorer.exe
// @compilerOptions -lgdiplus -lgdi32 -ldwmapi -lversion -lshcore -lshell32 -ladvapi32 -lhid -lole32 -loleaut32 -luuid -lwinhttp -lshlwapi -lruntimeobject
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Radial Window Switcher

![Radial Window Switcher](https://raw.githubusercontent.com/kotikdetka/radial-window-switcher/6fe413c441dce347eaf6f80ea5c2bb8c68d17b27/photo_2026-10-26_18-49-46.jpg)

Replaces the standard Alt+Tab with a radial switcher: the icons of open
windows are arranged in a ring in the center of the screen, the name of the
selected app is shown in the middle, and a live preview of the selected window
appears next to its sector.

## Usage

- Hold **Alt** and press **Tab** to open the ring.
- While holding Alt, press **Tab** for the next window, **Shift+Tab** for the
  previous one. Arrow keys also work.
- Release **Alt** to switch to the selected window.
- Press **Esc** to close the ring without switching.
- **Ctrl+Alt+Tab** keeps the ring open after Alt is released; pick with
  Tab, the arrows or the mouse, then Enter.
- **Delete** (or a middle click) closes the selected window; **Home** and **End**
  jump to the first and last window.
- Mouse: hover a sector to select it, click it (or the preview) to switch,
  scroll to move, click outside the ring to cancel.
- Touchpad: swipe three fingers left/right to open the ring and move, lift to
  switch. Up opens Task View (or brings windows back after a swipe down),
  down shows the desktop (or closes Task View).
- A quick Alt+Tab switches to the previous window, just like the standard one.

## Features

- Smooth animations: the ring spins in, sectors pop out one by one, the
  selection glows and a pointer glides to the selected app.
- Live window previews (DWM thumbnails), like the standard Alt+Tab.
- The selected sector uses the Windows accent color.
- Per-monitor DPI aware; the ring opens on the monitor with the cursor.
- Can use Alt+\` or Alt+Q instead of Alt+Tab, keeping the standard switcher.

## Notes

- The switcher runs in its own Windhawk process. Only a small part runs in
  Explorer: it catches Explorer's Alt+Tab hotkey and hands it to the
  switcher, so the standard switcher doesn't open as well.
- Minimized windows have no preview: Windows doesn't render them.
- Alt+\` and Alt+Q can't be caught while an elevated (administrator) window is
  in the foreground. Alt+Tab works there too, through Explorer.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- trigger: altTab
  $name: Shortcut
  $name:ru-RU: Сочетание клавиш
  $description: Hold Alt and press the second key to cycle through windows
  $description:ru-RU: Держите Alt и нажимайте вторую клавишу, чтобы листать окна
  $options:
  - altTab: Alt+Tab (replace the standard switcher)
  - altBacktick: Alt+` (key above Tab)
  - altQ: Alt+Q
  $options:ru-RU:
  - altTab: Alt+Tab (заменить стандартный)
  - altBacktick: Alt+` (клавиша Ё)
  - altQ: Alt+Q
- activateOnRelease: true
  $name: Switch when Alt is released
  $name:ru-RU: Переключать при отпускании Alt
  $description: If disabled, the ring stays open until Enter is pressed
  $description:ru-RU: Если выключено, меню остаётся открытым до нажатия Enter
- showPreview: true
  $name: Show live window preview
  $name:ru-RU: Показывать живое превью окна
- previewSize: 380
  $name: Preview width (px)
  $name:ru-RU: Ширина превью (px)
- radius: 230
  $name: Ring radius (px)
  $name:ru-RU: Радиус круга (px)
- iconSize: 64
  $name: Icon size (px)
  $name:ru-RU: Размер иконок (px)
- maxItems: 12
  $name: Maximum number of windows
  $name:ru-RU: Максимум окон в круге
- dimOpacity: 70
  $name: Background dimming (0-200)
  $name:ru-RU: Затемнение фона (0-200)
- useAccentColor: true
  $name: Highlight the selection with the Windows accent color
  $name:ru-RU: Подсвечивать выбранный сектор акцентным цветом Windows
- animate: true
  $name: Animations
  $name:ru-RU: Анимации
- touchpadGesture: true
  $name: Three-finger touchpad swipe
  $description: Swipe left or right with three fingers to open the ring and move through it, lift to switch. Set the Windows three-finger left/right swipes to "Nothing" so the standard switcher doesn't open as well
- lookupTap: true
  $name: Three-finger tap looks up a word
  $description: Like macOS Lookup - tap with three fingers on a word (or a selection) for the dictionary, Wikipedia and the local model. Set the Windows three-finger tap to "Nothing"
*/
// ==/WindhawkModSettings==

#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>
#include <hidusage.h>
#include <hidpi.h>
#include <uiautomation.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <mutex>
#include <string_view>

#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gp = Gdiplus;

// ============================================================================
// Settings
// ============================================================================

enum class Trigger { AltBacktick, AltTab, AltQ };

struct Settings {
    Trigger trigger = Trigger::AltTab;
    bool activateOnRelease = true;
    bool showPreview = true;
    int previewSize = 380;
    int radius = 230;
    int iconSize = 64;
    int maxItems = 12;
    int dimOpacity = 70;
    bool useAccentColor = true;
    bool animate = true;
    bool touchpadGesture = true;
    bool lookupTap = true;
};

static Settings g_settings;
// The trigger key: matched by virtual-key code (Tab, Q), or by scan code for
// the key above Tab, whose virtual-key code depends on the layout.
static std::atomic<DWORD> g_triggerVk{VK_TAB};
static std::atomic<DWORD> g_triggerScan{0};
static std::atomic<bool> g_activateOnRelease{true};

static void LoadSettings() {
    Settings s;
    const auto trig = WindhawkUtils::StringSetting::make(L"trigger");
    if (wcscmp(trig, L"altBacktick") == 0) {
        s.trigger = Trigger::AltBacktick;
    } else if (wcscmp(trig, L"altQ") == 0) {
        s.trigger = Trigger::AltQ;
    }
    s.activateOnRelease = Wh_GetIntSetting(L"activateOnRelease") != 0;
    s.showPreview = Wh_GetIntSetting(L"showPreview") != 0;
    s.previewSize = std::clamp(Wh_GetIntSetting(L"previewSize"), 160, 900);
    s.radius = std::clamp(Wh_GetIntSetting(L"radius"), 140, 520);
    s.iconSize = std::clamp(Wh_GetIntSetting(L"iconSize"), 24, 160);
    s.maxItems = std::clamp(Wh_GetIntSetting(L"maxItems"), 2, 24);
    s.dimOpacity = std::clamp(Wh_GetIntSetting(L"dimOpacity"), 0, 200);
    s.useAccentColor = Wh_GetIntSetting(L"useAccentColor") != 0;
    s.animate = Wh_GetIntSetting(L"animate") != 0;
    s.touchpadGesture = Wh_GetIntSetting(L"touchpadGesture") != 0;
    s.lookupTap = Wh_GetIntSetting(L"lookupTap") != 0;
    g_settings = s;
    g_activateOnRelease = s.activateOnRelease;

    switch (s.trigger) {
        case Trigger::AltQ:
            g_triggerVk = 'Q';
            g_triggerScan = 0;
            break;
        case Trigger::AltBacktick:
            g_triggerVk = 0;
            g_triggerScan = 0x29;  // the key above Tab
            break;
        default:
            g_triggerVk = VK_TAB;
            g_triggerScan = 0;
            break;
    }
}

// ============================================================================
// Constants & globals
// ============================================================================

constexpr ULONG_PTR kInjectedMagic = 0x52414454;  // marks our own SendInput
constexpr UINT WM_RS_SHOW = WM_APP + 1;
constexpr UINT WM_RS_NEXT = WM_APP + 2;
constexpr UINT WM_RS_PREV = WM_APP + 3;
constexpr UINT WM_RS_ACTIVATE = WM_APP + 4;
constexpr UINT WM_RS_CANCEL = WM_APP + 5;
constexpr UINT WM_RS_RELOAD = WM_APP + 6;
constexpr UINT WM_RS_QUIT = WM_APP + 7;
constexpr UINT WM_RS_REHOOK = WM_APP + 8;
constexpr UINT WM_RS_PREWARM = WM_APP + 9;
constexpr UINT WM_RS_GESTURE_START = WM_APP + 10;
constexpr UINT WM_RS_GESTURE_END = WM_APP + 11;
constexpr UINT WM_RS_CLOSEWIN = WM_APP + 12;
constexpr UINT WM_RS_FIRST = WM_APP + 13;
constexpr UINT WM_RS_LAST = WM_APP + 14;
constexpr UINT WM_RS_SWIPE_UP = WM_APP + 15;
constexpr UINT WM_RS_SWIPE_DOWN = WM_APP + 16;
constexpr UINT WM_RS_LOOKUP_TAP = WM_APP + 30;     // wp = 1: lp = test point
constexpr UINT WM_RS_LOOKUP_RESULT = WM_APP + 31;  // lp = LkMsg*
constexpr UINT WM_RS_LOOKUP_CLOSE = WM_APP + 32;
constexpr UINT WM_RS_LOOKUP_MHOOK = WM_APP + 33;   // hook thread, wp = on
constexpr UINT_PTR kTimerId = 1;
constexpr float kPi = 3.14159265358979f;
constexpr wchar_t kClassName[] = L"RadialWindowSwitcher_Windhawk";
constexpr wchar_t kPreviewClassName[] = L"RadialWindowSwitcherPreview_Windhawk";
// The switcher window's title tells Explorer whether it can take Alt+Tab
// over right now. FindWindow compares titles without sending messages.
constexpr wchar_t kTitleIdle[] = L"Radial Window Switcher";
constexpr wchar_t kTitleReady[] = L"Radial Window Switcher (ready)";
// Posted by Explorer to the switcher window when it gets the Alt+Tab hotkey.
// wParam: 1 for Shift+Alt+Tab.
constexpr wchar_t kHotkeyMessageName[] = L"RadialWindowSwitcher_Hotkey";
// Explorer's thread that receives the Alt+Tab hotkey.
constexpr wchar_t kAltTabThreadName[] = L"Immersive Shell";
static UINT g_hotkeyMessage = 0;

static HINSTANCE g_hInst = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_readyEvent = nullptr;
static std::atomic<HWND> g_hwnd{nullptr};
static HHOOK g_hook = nullptr;
static HWND g_previewHwnd = nullptr;
static HANDLE g_hookThread = nullptr;
static DWORD g_hookThreadId = 0;
static HANDLE g_hookReadyEvent = nullptr;

// Shared between the UI thread and the keyboard-hook thread.
static std::atomic<bool> g_visible{false};
static std::atomic<bool> g_pending{false};  // show requested, not processed yet
static std::atomic<bool> g_altHeld{false};  // physical Alt state, set by the hook
static bool g_dirty = false;
// The ring was opened by a touchpad swipe: Alt isn't held, the fingers are.
static bool g_gestureActive = false;
static ULONGLONG g_lastTouchTick = 0;
// Ctrl+Alt+Tab: the ring stays open after Alt is released, like Windows.
static std::atomic<bool> g_sticky{false};
static std::atomic<bool> g_lookupVisible{false};
static std::atomic<UINT> g_lookupGen{0};
static void LookupTapFrame(int fingers, double x, double y);
static void LookupMouseHookCtl(bool on);
static POINT g_showCursor{};  // the cursor when the ring opened
static bool g_mouseMoved = false;
static ULONGLONG g_animStart = 0;

// ============================================================================
// Icons / app info
// ============================================================================

struct IconImage {
    std::vector<BYTE> pixels;
    int w = 0, h = 0;
    std::unique_ptr<gp::Bitmap> bmp;
};

struct AppInfo {
    std::wstring name;
    std::shared_ptr<IconImage> icon;
};

struct Item {
    HWND hwnd = nullptr;
    std::wstring name;
    std::wstring title;
    std::shared_ptr<IconImage> icon;
};

static std::unordered_map<std::wstring, AppInfo> g_appCache;
static std::shared_ptr<IconImage> g_defaultIcon;
static std::vector<Item> g_items;
static int g_sel = 0;

static std::shared_ptr<IconImage> IconFromHicon(HICON hIcon) {
    ICONINFO ii{};
    if (!hIcon || !GetIconInfo(hIcon, &ii)) {
        return nullptr;
    }
    std::shared_ptr<IconImage> img;
    if (ii.hbmColor) {
        BITMAP bm{};
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        int w = bm.bmWidth, h = bm.bmHeight;
        if (w > 0 && h > 0) {
            img = std::make_shared<IconImage>();
            img->w = w;
            img->h = h;
            img->pixels.resize((size_t)w * h * 4);

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            HDC dc = GetDC(nullptr);
            GetDIBits(dc, ii.hbmColor, 0, h, img->pixels.data(), &bi,
                      DIB_RGB_COLORS);

            bool hasAlpha = false;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (img->pixels[i * 4 + 3]) {
                    hasAlpha = true;
                    break;
                }
            }
            if (!hasAlpha && ii.hbmMask) {
                std::vector<BYTE> mask((size_t)w * h * 4);
                GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi,
                          DIB_RGB_COLORS);
                for (size_t i = 0; i < (size_t)w * h; i++) {
                    img->pixels[i * 4 + 3] = mask[i * 4] ? 0 : 255;
                }
            }
            ReleaseDC(nullptr, dc);

            img->bmp = std::make_unique<gp::Bitmap>(
                w, h, w * 4, PixelFormat32bppARGB, img->pixels.data());
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return img;
}

static std::shared_ptr<IconImage> LoadExeIcon(const std::wstring& path,
                                              int size) {
    using PrivateExtractIconsW_t =
        UINT(WINAPI*)(LPCWSTR, int, int, int, HICON*, UINT*, UINT, UINT);
    static auto pExtract = (PrivateExtractIconsW_t)(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "PrivateExtractIconsW");
    if (!pExtract) {
        return nullptr;
    }
    HICON hIcon = nullptr;
    UINT id = 0;
    UINT n = pExtract(path.c_str(), 0, size, size, &hIcon, &id, 1, 0);
    if (n == 0 || n == 0xFFFFFFFF || !hIcon) {
        return nullptr;
    }
    auto img = IconFromHicon(hIcon);
    DestroyIcon(hIcon);
    return img;
}

static HICON GetWindowIconHandle(HWND hwnd) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &r) &&
        r) {
        return (HICON)r;
    }
    r = GetClassLongPtrW(hwnd, GCLP_HICON);
    if (r) {
        return (HICON)r;
    }
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &r) &&
        r) {
        return (HICON)r;
    }
    return nullptr;
}

static std::wstring GetProcessPath(DWORD pid) {
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
            result.assign(buf, size);
        }
        CloseHandle(h);
    }
    return result;
}

static std::wstring GetFileDescription(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) {
        return L"";
    }
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return L"";
    }
    struct LangCodePage {
        WORD lang;
        WORD codePage;
    }* tr = nullptr;
    UINT cb = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                       (LPVOID*)&tr, &cb) &&
        cb >= sizeof(LangCodePage)) {
        wchar_t sub[64];
        _snwprintf(sub, ARRAYSIZE(sub),
                   L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang,
                   tr[0].codePage);
        LPWSTR val = nullptr;
        UINT len = 0;
        if (VerQueryValueW(data.data(), sub, (LPVOID*)&val, &len) && val &&
            len > 1) {
            return val;
        }
    }
    return L"";
}

static std::wstring FileStem(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        name.resize(dot);
    }
    return name;
}

static bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

static AppInfo GetAppInfo(const std::wstring& path, int iconPx) {
    std::wstring key = path + L"|" + std::to_wstring(iconPx);
    auto it = g_appCache.find(key);
    if (it != g_appCache.end()) {
        return it->second;
    }
    if (g_appCache.size() > 256) {
        g_appCache.clear();
    }
    AppInfo info;
    // In a 32-bit tool process, reads of System32 files would be redirected
    // to SysWOW64, where 64-bit-only system apps don't exist.
    PVOID wow64State = nullptr;
    BOOL redirectionOff = Wow64DisableWow64FsRedirection(&wow64State);
    info.name = GetFileDescription(path);
    info.icon = LoadExeIcon(path, iconPx);
    if (redirectionOff) {
        Wow64RevertWow64FsRedirection(wow64State);
    }
    if (info.name.empty()) {
        info.name = FileStem(path);
    }
    g_appCache.emplace(key, info);
    return info;
}

// ============================================================================
// Window enumeration
// ============================================================================

static bool IsSwitchableWindow(HWND h) {
    if (h == g_hwnd || h == g_previewHwnd || !IsWindowVisible(h)) {
        return false;
    }
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) {
        return false;
    }
    if ((ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW)) {
        return false;
    }
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) {
        return false;
    }
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked) {
        return false;
    }
    wchar_t title[2];
    if (GetWindowTextW(h, title, ARRAYSIZE(title)) == 0) {
        return false;
    }
    wchar_t cls[64];
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    static const wchar_t* kSkip[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow"};
    for (auto s : kSkip) {
        if (wcscmp(cls, s) == 0) {
            return false;
        }
    }
    return true;
}

static BOOL CALLBACK EnumWindowsProc(HWND h, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<HWND>*>(lp);
    if (IsSwitchableWindow(h)) {
        list->push_back(h);
    }
    return list->size() < (size_t)g_settings.maxItems;
}

static std::wstring GetTitle(HWND h) {
    wchar_t buf[512];
    int n = GetWindowTextW(h, buf, ARRAYSIZE(buf));
    return std::wstring(buf, std::max(n, 0));
}

static void BuildItems(int iconPx) {
    g_items.clear();
    std::vector<HWND> list;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&list));

    if (!g_defaultIcon) {
        g_defaultIcon = IconFromHicon(LoadIconW(nullptr, IDI_APPLICATION));
    }

    for (HWND h : list) {
        Item it;
        it.hwnd = h;
        it.title = GetTitle(h);

        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        std::wstring path = GetProcessPath(pid);
        bool uwp = false;
        if (EndsWithI(path, L"\\ApplicationFrameHost.exe")) {
            uwp = true;
            path.clear();
            HWND core = FindWindowExW(h, nullptr,
                                      L"Windows.UI.Core.CoreWindow", nullptr);
            if (core) {
                DWORD corePid = 0;
                GetWindowThreadProcessId(core, &corePid);
                path = GetProcessPath(corePid);
            }
        }

        if (!path.empty()) {
            AppInfo app = GetAppInfo(path, iconPx);
            it.name = app.name;
            it.icon = app.icon;
        }
        if (uwp || it.name.empty()) {
            it.name = it.title;
        }
        if (!it.icon) {
            it.icon = IconFromHicon(GetWindowIconHandle(h));
        }
        if (!it.icon) {
            it.icon = g_defaultIcon;
        }
        g_items.push_back(std::move(it));
    }
}

// ============================================================================
// Layout & drawing
// ============================================================================

struct Layout {
    RECT mon{};
    RECT work{};
    int width = 0, height = 0;
    float cx = 0, cy = 0;  // circle center, window-local
    float scale = 1;
    float outer = 0, inner = 0, icon = 0;
};
static Layout g_lay;

static HDC g_memDC = nullptr;
static HBITMAP g_dib = nullptr;
static HGDIOBJ g_oldBmp = nullptr;
static void* g_bits = nullptr;
static int g_dibW = 0, g_dibH = 0;

static void FreeDib() {
    if (g_memDC) {
        if (g_oldBmp) SelectObject(g_memDC, g_oldBmp);
        DeleteDC(g_memDC);
    }
    if (g_dib) DeleteObject(g_dib);
    g_memDC = nullptr;
    g_dib = nullptr;
    g_oldBmp = nullptr;
    g_bits = nullptr;
    g_dibW = g_dibH = 0;
}

static bool EnsureDib(int w, int h) {
    if (g_dib && g_dibW == w && g_dibH == h) {
        return true;
    }
    FreeDib();
    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    if (!g_dib) {
        FreeDib();
        return false;
    }
    g_oldBmp = SelectObject(g_memDC, g_dib);
    g_dibW = w;
    g_dibH = h;
    return true;
}

static float GetMonitorScale(HMONITOR mon) {
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y))) {
        x = 96;
    }
    return x / 96.f;
}

// The menu always opens in the center of the monitor with the cursor.
static void ComputeLayout() {
    Layout& L = g_lay;
    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);

    L.mon = mi.rcMonitor;
    L.work = mi.rcWork;
    L.width = L.mon.right - L.mon.left;
    L.height = L.mon.bottom - L.mon.top;
    L.scale = GetMonitorScale(mon);

    RECT w = mi.rcWork;
    float maxOuter = std::min(w.right - w.left, w.bottom - w.top) / 2.f -
                     24 * L.scale;
    L.outer = std::min(g_settings.radius * L.scale, maxOuter);
    L.inner = L.outer * 0.46f;
    L.icon = g_settings.iconSize * L.scale;

    L.cx = (w.left + w.right) / 2.f - L.mon.left;
    L.cy = (w.top + w.bottom) / 2.f - L.mon.top;
}

static gp::PointF Polar(float r, float deg) {
    float a = deg * kPi / 180.f;
    return gp::PointF(r * cosf(a), r * sinf(a));
}

static gp::PointF Lerp(const gp::PointF& a, const gp::PointF& b, float t) {
    return gp::PointF(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t);
}

static float Dist(const gp::PointF& a, const gp::PointF& b) {
    return hypotf(a.X - b.X, a.Y - b.Y);
}

static float RadToDeg(float r) { return r * 180.f / kPi; }

// Annular sector with a constant-width gap to its neighbours and rounded corners.
static void BuildSectorPath(gp::GraphicsPath& p, float ri, float ro, float a0,
                            float a1, float gap, float corner) {
    float go = RadToDeg(gap / 2 / ro);
    float gi = RadToDeg(gap / 2 / ri);
    float so = a0 + go, eo = a1 - go, si = a0 + gi, ei = a1 - gi;
    if (eo <= so || ei <= si) {
        so = a0, eo = a1, si = a0, ei = a1;
    }

    gp::PointF Pos = Polar(ro, so), Poe = Polar(ro, eo);
    gp::PointF Pis = Polar(ri, si), Pie = Polar(ri, ei);
    float edge = std::max(Dist(Pos, Pis), 1.f);
    float c = std::min(corner, edge * 0.45f);
    float co = std::min(RadToDeg(c / ro), (eo - so) * 0.45f);
    float ci = std::min(RadToDeg(c / ri), (ei - si) * 0.45f);
    float te = c / edge;
    const float k = 0.55f;

    auto roundCorner = [&](const gp::PointF& from, const gp::PointF& cornerPt,
                           const gp::PointF& to) {
        p.AddBezier(from, Lerp(from, cornerPt, k), Lerp(to, cornerPt, k), to);
    };

    gp::PointF e1 = Lerp(Pos, Pis, te);
    roundCorner(e1, Pos, Polar(ro, so + co));
    p.AddArc(-ro, -ro, 2 * ro, 2 * ro, so + co, (eo - co) - (so + co));
    gp::PointF e2 = Lerp(Poe, Pie, te);
    roundCorner(Polar(ro, eo - co), Poe, e2);
    gp::PointF e3 = Lerp(Pie, Poe, te);
    p.AddLine(e2, e3);
    roundCorner(e3, Pie, Polar(ri, ei - ci));
    p.AddArc(-ri, -ri, 2 * ri, 2 * ri, ei - ci, -((ei - ci) - (si + ci)));
    gp::PointF e4 = Lerp(Pis, Pos, te);
    roundCorner(Polar(ri, si + ci), Pis, e4);
    p.CloseFigure();
}

static void AddRoundRect(gp::GraphicsPath& p, const gp::RectF& r, float rad) {
    rad = std::min(rad, std::min(r.Width, r.Height) / 2);
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

static float EaseOutCubic(float t) {
    t = Clamp01(t);
    return 1.f - powf(1.f - t, 3.f);
}

static float EaseOutBack(float t) {
    t = Clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1.f;
    return 1.f + c3 * powf(t - 1.f, 3.f) + c1 * powf(t - 1.f, 2.f);
}

static gp::Color MixColor(const gp::Color& a, const gp::Color& b, float t,
                          float alphaMul = 1.f) {
    auto mix = [t](BYTE x, BYTE y) { return (float)x + ((float)y - (float)x) * t; };
    auto toByte = [](float v) {
        return (BYTE)std::lround(std::clamp(v, 0.f, 255.f));
    };
    return gp::Color(toByte(mix(a.GetA(), b.GetA()) * alphaMul),
                     toByte(mix(a.GetR(), b.GetR())),
                     toByte(mix(a.GetG(), b.GetG())),
                     toByte(mix(a.GetB(), b.GetB())));
}

static float NormalizeAngle(float a) {
    a = fmodf(a + 180.f, 360.f);
    if (a < 0) a += 360.f;
    return a - 180.f;
}

// Milliseconds since `since`, or "long ago" when animations are off.
static float AnimMs(ULONGLONG since) {
    if (!g_settings.animate) {
        return 100000.f;
    }
    return (float)(GetTickCount64() - since);
}

// Read once per frame in Render() rather than per sector.
static DWORD g_accentColor = 0;
static bool g_hasAccentColor = false;

static void RefreshAccentColor() {
    BOOL opaque = FALSE;
    g_hasAccentColor = g_settings.useAccentColor &&
                       SUCCEEDED(DwmGetColorizationColor(&g_accentColor, &opaque));
}

static gp::Color GetAccentColor(BYTE alpha) {
    if (g_hasAccentColor) {
        DWORD c = g_accentColor;
        return gp::Color(alpha, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
    return gp::Color((BYTE)std::min(255, alpha * 3 / 4), 255, 255, 255);
}

// ---------------------------------------------------------------------------
// Animation state
// ---------------------------------------------------------------------------

static std::vector<float> g_hover;  // 0..1 highlight per sector
static float g_pointerAngle = -90.f;
static int g_textIndex = -1;
static ULONGLONG g_textChangeAt = 0;
static bool g_needFrame = false;

// ---------------------------------------------------------------------------
// Live window preview (DWM thumbnail, like the system Alt+Tab)
// ---------------------------------------------------------------------------

static HTHUMBNAIL g_thumb = nullptr;
static HWND g_thumbSource = nullptr;
static bool g_previewShown = false;
static ULONGLONG g_previewAt = 0;
static RECT g_previewFinal{};
static float g_previewDirX = 0, g_previewDirY = 0;
static gp::RectF g_previewDrawRect;  // window-local, for the glow frame

static void ClosePreview() {
    if (g_thumb) {
        DwmUnregisterThumbnail(g_thumb);
        g_thumb = nullptr;
    }
    g_thumbSource = nullptr;
    g_previewShown = false;
    if (g_previewHwnd) {
        ShowWindow(g_previewHwnd, SW_HIDE);
    }
}

// Places the preview at the right animation frame. Returns true while moving.
static bool UpdatePreviewAnim() {
    if (!g_previewShown || !g_thumb || !g_previewHwnd) {
        return false;
    }
    float s = g_lay.scale;
    float t = Clamp01(AnimMs(g_previewAt) / 240.f);
    float e = EaseOutCubic(t);
    float sc = 0.78f + 0.22f * EaseOutBack(t);

    const RECT& f = g_previewFinal;
    float fw = (float)(f.right - f.left), fh = (float)(f.bottom - f.top);
    float slide = (1.f - e) * 40.f * s;
    float cx = (f.left + f.right) / 2.f - g_previewDirX * slide;
    float cy = (f.top + f.bottom) / 2.f - g_previewDirY * slide;
    int w = std::max(1, (int)std::lround(fw * sc));
    int h = std::max(1, (int)std::lround(fh * sc));
    int x = (int)std::lround(cx - w / 2.f);
    int y = (int)std::lround(cy - h / 2.f);

    SetWindowPos(g_previewHwnd, nullptr, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);

    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    props.rcDestination = RECT{0, 0, w, h};
    props.fVisible = TRUE;
    props.opacity = (BYTE)std::lround(255 * (0.35f + 0.65f * e));
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(g_thumb, &props);

    g_previewDrawRect = gp::RectF((float)(x - g_lay.mon.left),
                                  (float)(y - g_lay.mon.top), (float)w,
                                  (float)h);
    return t < 1.f;
}

static void OpenPreview(int index) {
    if (index < 0 || index >= (int)g_items.size()) {
        ClosePreview();
        return;
    }
    HWND src = g_items[index].hwnd;
    if (src == g_thumbSource && g_thumb) {
        return;
    }
    ClosePreview();
    if (!g_settings.showPreview || !g_previewHwnd || !IsWindow(src) ||
        IsIconic(src)) {
        return;  // minimized windows have nothing to show
    }
    if (FAILED(DwmRegisterThumbnail(g_previewHwnd, src, &g_thumb))) {
        g_thumb = nullptr;
        return;
    }
    SIZE srcSize{};
    if (FAILED(DwmQueryThumbnailSourceSize(g_thumb, &srcSize)) ||
        srcSize.cx <= 0 || srcSize.cy <= 0) {
        ClosePreview();
        return;
    }
    g_thumbSource = src;

    float s = g_lay.scale;
    float maxW = g_settings.previewSize * s;
    float maxH = maxW * 0.62f;
    float k = std::min({maxW / srcSize.cx, maxH / srcSize.cy, 1.f});
    float w = srcSize.cx * k, h = srcSize.cy * k;

    // Put the preview outside the ring, in the direction of the sector.
    int n = (int)g_items.size();
    float a = (-90.f + index * 360.f / n) * kPi / 180.f;
    float dx = cosf(a), dy = sinf(a);
    float ring = g_lay.outer + 26 * s;
    float dist = ring + fabsf(dx) * w / 2 + fabsf(dy) * h / 2;
    float cx = g_lay.mon.left + g_lay.cx + dx * dist;
    float cy = g_lay.mon.top + g_lay.cy + dy * dist;

    const RECT& wk = g_lay.work;
    float m = 10 * s;
    float x = std::clamp(cx - w / 2, wk.left + m, std::max(wk.left + m, wk.right - m - w));
    float y = std::clamp(cy - h / 2, wk.top + m, std::max(wk.top + m, wk.bottom - m - h));

    g_previewFinal = RECT{(LONG)std::lround(x), (LONG)std::lround(y),
                          (LONG)std::lround(x + w), (LONG)std::lround(y + h)};
    g_previewDirX = dx;
    g_previewDirY = dy;
    g_previewAt = GetTickCount64();
    g_previewShown = true;

    UpdatePreviewAnim();
    ShowWindow(g_previewHwnd, SW_SHOWNOACTIVATE);
}

// ---------------------------------------------------------------------------
// Rendering. Returns true while anything is still animating.
// ---------------------------------------------------------------------------

static void DrawIcon(gp::Graphics& g, const IconImage& icon, const gp::RectF& r,
                     float alpha) {
    if (alpha >= 0.999f) {
        g.DrawImage(icon.bmp.get(), r);
        return;
    }
    gp::ColorMatrix cm = {{{1, 0, 0, 0, 0},
                           {0, 1, 0, 0, 0},
                           {0, 0, 1, 0, 0},
                           {0, 0, 0, alpha, 0},
                           {0, 0, 0, 0, 1}}};
    gp::ImageAttributes ia;
    ia.SetColorMatrix(&cm);
    g.DrawImage(icon.bmp.get(), r, 0, 0, (float)icon.w, (float)icon.h,
                gp::UnitPixel, &ia);
}

static bool Render() {
    const Layout& L = g_lay;
    if (!g_bits || !g_hwnd) {
        return false;
    }
    RefreshAccentColor();
    const float s = L.scale;
    const float ms = AnimMs(g_animStart);
    const int n = (int)g_items.size();
    const float step = n > 0 ? 360.f / n : 360.f;
    bool animating = ms < 700.f;

    // Smoothly follow the selection.
    if ((int)g_hover.size() != n) {
        g_hover.assign(n, 0.f);
    }
    for (int i = 0; i < n; i++) {
        float target = (i == g_sel) ? 1.f : 0.f;
        float& h = g_hover[i];
        if (!g_settings.animate) {
            h = target;
        } else {
            h += (target - h) * 0.3f;
            if (fabsf(target - h) < 0.005f) {
                h = target;
            } else {
                animating = true;
            }
        }
    }
    if (n > 0) {
        float target = -90.f + g_sel * step;
        float diff = NormalizeAngle(target - g_pointerAngle);
        if (!g_settings.animate || fabsf(diff) < 0.2f) {
            g_pointerAngle = target;
        } else {
            g_pointerAngle += diff * 0.3f;
            animating = true;
        }
    }
    if (g_textIndex != g_sel) {
        g_textIndex = g_sel;
        g_textChangeAt = GetTickCount64();
    }
    float textT = Clamp01(AnimMs(g_textChangeAt) / 200.f);
    if (textT < 1.f) {
        animating = true;
    }

    float fade = EaseOutCubic(ms / 160.f);

    {
        gp::Bitmap canvas(L.width, L.height, L.width * 4,
                          PixelFormat32bppPARGB, (BYTE*)g_bits);
        gp::Graphics g(&canvas);
        g.Clear(gp::Color((BYTE)g_settings.dimOpacity, 0, 0, 0));
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
        g.SetCompositingQuality(gp::CompositingQualityHighQuality);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);

        // Soft shadow + frame behind the live preview window.
        if (g_previewShown) {
            float rad = 10 * s;
            for (int k = 4; k >= 1; k--) {
                gp::RectF r = g_previewDrawRect;
                r.Inflate(k * 4 * s, k * 4 * s);
                r.Offset(0, 4 * s);
                gp::GraphicsPath sp;
                AddRoundRect(sp, r, rad + k * 4 * s);
                gp::SolidBrush sb(gp::Color(22, 0, 0, 0));
                g.FillPath(&sb, &sp);
            }
            gp::RectF fr = g_previewDrawRect;
            fr.Inflate(3 * s, 3 * s);
            gp::GraphicsPath fp;
            AddRoundRect(fp, fr, rad + 3 * s);
            gp::Pen framePen(GetAccentColor(200), 2.f * s);
            g.DrawPath(&framePen, &fp);
        }

        g.TranslateTransform(L.cx, L.cy);
        float ringRot = -40.f * (1.f - EaseOutCubic(ms / 450.f));

        float gap = 8 * s, corner = 12 * s;
        gp::Color normalFill(44, 255, 255, 255), normalEdge(70, 255, 255, 255);
        gp::Color selFill = GetAccentColor(185), selEdge(170, 255, 255, 255);

        if (n > 0) {
            float midR = (L.inner + L.outer) / 2;
            float maxIcon = std::min(L.icon, (L.outer - L.inner) * 0.6f);
            if (n > 1) {
                maxIcon = std::min(maxIcon,
                                   2 * midR * sinf(step * kPi / 360.f) * 0.7f);
            }
            float stagger = std::min(28.f, 220.f / n);

            // Unselected sectors first, the highlighted one on top.
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < n; i++) {
                    bool front = g_hover[i] > 0.5f;
                    if (front != (pass == 1)) continue;

                    float local = Clamp01((ms - 40.f - i * stagger) / 260.f);
                    if (local <= 0.f) continue;
                    float pop = EaseOutBack(local);
                    float appear = EaseOutCubic(local);
                    float h = g_hover[i];

                    gp::GraphicsState st = g.Save();
                    float k = 0.55f + 0.45f * pop;
                    g.ScaleTransform(k, k);

                    float a = -90.f + i * step + ringRot;
                    float ro = L.outer + 12 * s * h;
                    float ri = L.inner + 2 * s * h;

                    gp::GraphicsPath path;
                    if (n == 1) {
                        path.AddEllipse(-ro, -ro, 2 * ro, 2 * ro);
                        path.AddEllipse(-ri, -ri, 2 * ri, 2 * ri);
                    } else {
                        BuildSectorPath(path, ri, ro, a - step / 2,
                                        a + step / 2, gap, corner);
                    }

                    if (h > 0.01f) {
                        gp::Pen glow(GetAccentColor((BYTE)std::lround(70 * h * appear)),
                                     10.f * s);
                        glow.SetLineJoin(gp::LineJoinRound);
                        g.DrawPath(&glow, &path);
                    }
                    gp::SolidBrush brush(MixColor(normalFill, selFill, h, appear));
                    g.FillPath(&brush, &path);
                    gp::Pen pen(MixColor(normalEdge, selEdge, h, appear), 1.2f * s);
                    g.DrawPath(&pen, &path);

                    const Item& item = g_items[i];
                    if (item.icon && item.icon->bmp) {
                        gp::PointF c = Polar((ri + ro) / 2, a);
                        float sz = maxIcon * (1.f + 0.2f * h);
                        DrawIcon(g, *item.icon,
                                 gp::RectF(c.X - sz / 2, c.Y - sz / 2, sz, sz),
                                 appear);
                    }
                    g.Restore(st);
                }
            }
        }

        // Center disc.
        float cpop = EaseOutBack((ms - 60.f) / 300.f);
        if (cpop > 0.f) {
            float cr = (L.inner - 14 * s) * (0.6f + 0.4f * cpop);
            gp::SolidBrush discBrush(gp::Color(160, 20, 20, 24));
            g.FillEllipse(&discBrush, -cr, -cr, 2 * cr, 2 * cr);
            gp::Pen discPen(gp::Color(70, 255, 255, 255), 1.2f * s);
            g.DrawEllipse(&discPen, -cr, -cr, 2 * cr, 2 * cr);

            gp::Pen gloss(gp::Color(140, 255, 255, 255), 3 * s);
            gloss.SetStartCap(gp::LineCapRound);
            gloss.SetEndCap(gp::LineCapRound);
            float gr = cr - 10 * s;
            g.DrawArc(&gloss, -gr, -gr, 2 * gr, 2 * gr, 205.f, 35.f);

            // Pointer arc that glides toward the selected sector.
            if (n > 0) {
                gp::Pen pointer(GetAccentColor(255), 4 * s);
                pointer.SetStartCap(gp::LineCapRound);
                pointer.SetEndCap(gp::LineCapRound);
                float pr = cr - 1 * s;
                float sweep = std::min(46.f, step * 0.7f);
                g.DrawArc(&pointer, -pr, -pr, 2 * pr, 2 * pr,
                          g_pointerAngle + ringRot - sweep / 2, sweep);
            }

            // Application name, and the window title below it.
            std::wstring name, title;
            if (n > 0 && g_sel >= 0 && g_sel < n) {
                name = g_items[g_sel].name;
                if (g_items[g_sel].title != name) {
                    title = g_items[g_sel].title;
                }
            } else {
                name = L"No open windows";
            }
            float te = EaseOutCubic(textT);
            float tw = cr * 1.6f;
            float yOff = (1.f - te) * 10 * s;
            gp::Font font(L"Segoe UI", 21 * s * (0.6f + 0.4f * cpop),
                          gp::FontStyleBold, gp::UnitPixel);
            gp::SolidBrush textBrush(
                gp::Color((BYTE)std::lround(255 * te), 255, 255, 255));
            gp::StringFormat sf;
            sf.SetAlignment(gp::StringAlignmentCenter);
            sf.SetLineAlignment(gp::StringAlignmentCenter);
            sf.SetTrimming(gp::StringTrimmingEllipsisCharacter);
            if (title.empty()) {
                g.DrawString(name.c_str(), (INT)name.size(), &font,
                             gp::RectF(-tw / 2, -cr * 0.6f + yOff, tw,
                                       cr * 1.2f),
                             &sf, &textBrush);
            } else {
                sf.SetLineAlignment(gp::StringAlignmentFar);
                g.DrawString(name.c_str(), (INT)name.size(), &font,
                             gp::RectF(-tw / 2, -cr * 0.6f + yOff, tw,
                                       cr * 0.62f),
                             &sf, &textBrush);
                gp::Font titleFont(L"Segoe UI", 13 * s * (0.6f + 0.4f * cpop),
                                   gp::FontStyleRegular, gp::UnitPixel);
                gp::SolidBrush titleBrush(gp::Color(
                    (BYTE)std::lround(190 * te), 255, 255, 255));
                gp::StringFormat tf;
                tf.SetAlignment(gp::StringAlignmentCenter);
                tf.SetLineAlignment(gp::StringAlignmentNear);
                tf.SetTrimming(gp::StringTrimmingEllipsisWord);
                float ttw = cr * 1.5f;
                g.DrawString(title.c_str(), (INT)title.size(), &titleFont,
                             gp::RectF(-ttw / 2, 6 * s + yOff, ttw,
                                       cr * 0.5f),
                             &tf, &titleBrush);
            }
        }
        g.Flush(gp::FlushIntentionSync);
    }

    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)std::lround(255 * fade), AC_SRC_ALPHA};
    POINT dst{L.mon.left, L.mon.top};
    SIZE size{L.width, L.height};
    POINT src{0, 0};
    UpdateLayeredWindow(g_hwnd, nullptr, &dst, &size, g_memDC, &src, 0, &bf,
                        ULW_ALPHA);
    return animating;
}

static void Frame() {
    bool a = UpdatePreviewAnim();
    bool b = Render();
    g_needFrame = a || b;
    g_dirty = false;
}

// ============================================================================
// Switching logic
// ============================================================================

static void SendDummyKey() {
    // An unassigned key between Alt down/up stops apps from opening their
    // menu bar on Alt release, and grants us foreground rights.
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = 0xE8;
    in[0].ki.dwExtraInfo = kInjectedMagic;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void ForceForeground(HWND h) {
    if (IsIconic(h)) {
        ShowWindowAsync(h, SW_RESTORE);
    }
    SendDummyKey();  // makes this process the source of the last input
    SetForegroundWindow(h);
}

static void HideSwitcher() {
    g_visible = false;
    g_gestureActive = false;
    g_sticky = false;
    g_pending = false;
    ClosePreview();
    if (g_hwnd) {
        KillTimer(g_hwnd, kTimerId);
        ShowWindow(g_hwnd, SW_HIDE);
    }
    FreeDib();
}

static void ActivateSelected() {
    HWND target = nullptr;
    if (g_sel >= 0 && g_sel < (int)g_items.size()) {
        target = g_items[g_sel].hwnd;
    }
    HideSwitcher();
    if (target && IsWindow(target)) {
        ForceForeground(target);
    }
}

static void SetSelection(int idx) {
    int n = (int)g_items.size();
    if (n == 0) {
        return;
    }
    idx = ((idx % n) + n) % n;
    if (idx == g_sel) {
        return;
    }
    g_sel = idx;
    OpenPreview(idx);
    g_dirty = true;
}

static void ShowSwitcher(bool reverse) {
    SendDummyKey();  // stops the Alt-release from opening app menu bars
    // g_pending stays true until g_visible is set, so the hook keeps
    // swallowing Tab while icons are being collected.
    ComputeLayout();
    BuildItems((int)std::lround(g_lay.icon * 1.25f));

    int n = (int)g_items.size();
    g_sel = 0;
    if (n > 1) {
        HWND fg = GetForegroundWindow();
        HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
        bool firstIsActive =
            g_items[0].hwnd == fg || g_items[0].hwnd == fgRoot;
        if (reverse) {
            g_sel = n - 1;
        } else {
            g_sel = firstIsActive ? 1 : 0;
        }
    }

    if (!EnsureDib(g_lay.width, g_lay.height)) {
        Wh_Log(L"Failed to allocate DIB");
        g_pending = false;
        return;
    }

    g_visible = true;
    g_pending = false;
    g_animStart = GetTickCount64();
    GetCursorPos(&g_showCursor);
    g_mouseMoved = false;
    g_hover.assign(n, 0.f);
    g_pointerAngle = n > 0 ? -90.f + g_sel * 360.f / n : -90.f;
    g_textIndex = -1;
    ClosePreview();

    Frame();
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_lay.mon.left, g_lay.mon.top,
                 g_lay.width, g_lay.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    OpenPreview(g_sel);
    SetTimer(g_hwnd, kTimerId, 10, nullptr);
}

static void MoveSelection(int delta) {
    if (!g_visible || g_items.empty()) {
        return;
    }
    if (g_gestureActive) {
        // A swipe stops at the ends instead of wrapping back to the start.
        const int last = (int)g_items.size() - 1;
        SetSelection(std::clamp(g_sel + delta, 0, last));
        Frame();
        return;
    }
    SetSelection(g_sel + delta);
    Frame();
}

// Closes a window from the ring (Delete, middle click), like Windows'
// Alt+Tab, and keeps the ring open on the rest.
static void CloseItem(int idx) {
    if (!g_visible || idx < 0 || idx >= (int)g_items.size()) {
        return;
    }
    HWND h = g_items[idx].hwnd;
    PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
    g_items.erase(g_items.begin() + idx);
    if (g_items.empty()) {
        HideSwitcher();
        return;
    }
    g_sel = std::min(idx, (int)g_items.size() - 1);
    g_hover.assign(g_items.size(), 0.f);
    g_textIndex = -1;
    ClosePreview();
    OpenPreview(g_sel);
    Frame();
}

// The sector under a point in window coordinates, or -1.
static int HitSector(int x, int y) {
    const int n = (int)g_items.size();
    if (n == 0) {
        return -1;
    }
    float dx = x - g_lay.cx, dy = y - g_lay.cy;
    float r = hypotf(dx, dy);
    if (r < g_lay.inner * 0.75f || r > g_lay.outer + 24 * g_lay.scale) {
        return -1;
    }
    float step = 360.f / n;
    float a = RadToDeg(atan2f(dy, dx)) + 90.f;  // 0 = top, clockwise
    int idx = (int)std::lround(a / step);
    return ((idx % n) + n) % n;
}

static bool InCenter(int x, int y) {
    return hypotf(x - g_lay.cx, y - g_lay.cy) < g_lay.inner * 0.75f;
}

static void OnMouseMove(int x, int y) {
    if (!g_mouseMoved) {
        POINT cur{};
        GetCursorPos(&cur);
        // The ring opens under a resting cursor: only real movement selects.
        if (abs(cur.x - g_showCursor.x) + abs(cur.y - g_showCursor.y) < 4) {
            return;
        }
        g_mouseMoved = true;
    }
    int idx = HitSector(x, y);
    if (idx >= 0 && idx != g_sel) {
        SetSelection(idx);
        Frame();
    }
}

static void OnTimer() {
    if (!g_visible) {
        return;
    }

    // Safety net: if Alt was released and we somehow missed the key-up.
    // Ask Windows for the real key state: the keyboard hook can't see keys
    // while an elevated window (e.g. Windhawk itself) is in the foreground.
    // Requires Alt to be up on several checks in a row (~60 ms), so a single
    // stale reading can't close the menu.
    static int altUpTicks = 0;
    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                   (GetAsyncKeyState(VK_LMENU) & 0x8000) ||
                   (GetAsyncKeyState(VK_RMENU) & 0x8000);
    altUpTicks = altDown ? 0 : altUpTicks + 1;
    if (g_gestureActive) {
        // Safety net for a lift the touchpad never reported.
        altUpTicks = 0;
        if (GetTickCount64() - g_lastTouchTick > 1500) {
            ActivateSelected();
            return;
        }
    } else if (g_settings.activateOnRelease && !g_sticky &&
        GetTickCount64() - g_animStart > 250 && altUpTicks >= 6) {
        altUpTicks = 0;
        ActivateSelected();
        return;
    }

    if (g_needFrame || g_dirty) {
        Frame();
    }
}

// ============================================================================
// Keyboard hook (tool process)
// ============================================================================

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    HWND hwnd = g_hwnd;
    if (code != HC_ACTION || !hwnd) {
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
    if (k->dwExtraInfo == kInjectedMagic) {
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
    DWORD vk = k->vkCode;
    const DWORD triggerVk = g_triggerVk;
    bool isTrigger = triggerVk ? vk == triggerVk
                               : (k->scanCode == g_triggerScan &&
                                  !(k->flags & LLKHF_EXTENDED));
    bool isAlt = vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
    bool isModifier = isAlt || vk == VK_SHIFT || vk == VK_LSHIFT ||
                      vk == VK_RSHIFT || vk == VK_CONTROL ||
                      vk == VK_LCONTROL || vk == VK_RCONTROL ||
                      vk == VK_LWIN || vk == VK_RWIN;
    if (isAlt) {
        if (down) {
            g_altHeld = true;
        } else if (up) {
            g_altHeld = false;
        }
    }

    bool active = g_visible || g_pending;

    if (g_lookupVisible && !active && down && vk == VK_ESCAPE) {
        PostMessageW(hwnd, WM_RS_LOOKUP_CLOSE, 1, 0);
        return 1;
    }

    if (!active) {
        // Keep this path instant: no logging, no SendInput, no window
        // lookups.
        if (down && isTrigger) {
            bool alt = g_altHeld || (k->flags & LLKHF_ALTDOWN) ||
                       KeyDown(VK_MENU);
            bool ctrl = KeyDown(VK_CONTROL);
            bool win = KeyDown(VK_LWIN) || KeyDown(VK_RWIN);
            if (alt && !win) {
                g_pending = true;
                g_sticky = ctrl;  // Ctrl+Alt+Tab
                PostMessageW(hwnd, WM_RS_SHOW, KeyDown(VK_SHIFT) ? 1 : 0, 0);
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    // Menu is open (or about to be).
    if (isAlt) {
        if (up && g_activateOnRelease && !g_sticky) {
            PostMessageW(hwnd, WM_RS_ACTIVATE, 0, 0);
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (isModifier) {
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (down) {
        if (isTrigger) {
            PostMessageW(hwnd, KeyDown(VK_SHIFT) ? WM_RS_PREV : WM_RS_NEXT, 0,
                         0);
        } else if (vk == VK_ESCAPE) {
            PostMessageW(hwnd, WM_RS_CANCEL, 0, 0);
        } else if (vk == VK_RIGHT || vk == VK_DOWN) {
            PostMessageW(hwnd, WM_RS_NEXT, 0, 0);
        } else if (vk == VK_LEFT || vk == VK_UP) {
            PostMessageW(hwnd, WM_RS_PREV, 0, 0);
        } else if (vk == VK_RETURN || vk == VK_SPACE) {
            PostMessageW(hwnd, WM_RS_ACTIVATE, 0, 0);
        } else if (vk == VK_DELETE) {
            PostMessageW(hwnd, WM_RS_CLOSEWIN, (WPARAM)-1, 0);
        } else if (vk == VK_HOME) {
            PostMessageW(hwnd, WM_RS_FIRST, 0, 0);
        } else if (vk == VK_END) {
            PostMessageW(hwnd, WM_RS_LAST, 0, 0);
        }
    }
    return 1;  // swallow everything else while the menu is open
}

// Installs a fresh hook first, then removes the old one, so there is no gap.
// Called once at startup, and again only when Windows seems to have dropped
// the hook (see OnForwardedHotkey): a new hook goes ahead of the ones other
// programs installed after it, so it isn't re-installed needlessly.
static void InstallKeyboardHook() {
    HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_hInst, 0);
    if (!fresh) {
        Wh_Log(L"SetWindowsHookEx(WH_KEYBOARD_LL) failed: %u", GetLastError());
        return;
    }
    HHOOK old = g_hook;
    g_hook = fresh;
    if (old) {
        UnhookWindowsHookEx(old);
    }
}

// The hook runs on its own thread that does nothing else, so it always answers
// quickly and Windows doesn't drop it for exceeding LowLevelHooksTimeout.
static DWORD WINAPI HookThreadProc(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create queue
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    InstallKeyboardHook();
    SetEvent(g_hookReadyEvent);

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_RS_LOOKUP_MHOOK) {
            LookupMouseHookCtl(msg.wParam != 0);
            continue;
        }
        if (msg.message == WM_RS_REHOOK) {
            Wh_Log(L"Installing the keyboard hook again");
            InstallKeyboardHook();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LookupMouseHookCtl(false);
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    return 0;
}

static DWORD GetIntegrityLevel(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return 0;
    }
    DWORD level = 0;
    BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer),
                            &size)) {
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
        PSID sid = label->Label.Sid;
        level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    }
    CloseHandle(token);
    return level;
}

// True if the window belongs to a process this one can't see the keys of
// (e.g. an elevated app).
static bool IsAboveThisProcess(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return true;
    }
    DWORD level = GetIntegrityLevel(process);
    CloseHandle(process);
    static const DWORD ownLevel = GetIntegrityLevel(GetCurrentProcess());
    return !level || level > ownLevel;
}

// Explorer received the Alt+Tab hotkey and handed it over: the keyboard hook
// didn't see the keys, usually because an elevated window is in front.
static void OnForwardedHotkey(bool shift) {
    if (g_visible || g_pending) {
        MoveSelection(shift ? -1 : 1);
        return;
    }
    // Otherwise the hook should have seen the keys: Windows may have dropped
    // it, which would leave every Alt+Tab to this path from now on.
    HWND foreground = GetForegroundWindow();
    if (foreground && !IsAboveThisProcess(foreground)) {
        PostThreadMessageW(g_hookThreadId, WM_RS_REHOOK, 0, 0);
    }
    g_pending = true;
    ShowSwitcher(shift);
}

// ============================================================================
// Three-finger touchpad swipe (tool process)
// ============================================================================
//
// Windows opens its own switcher for this gesture inside Explorer, without
// any keystrokes, so neither the keyboard hook nor the hotkey sees it. The
// precision touchpad's raw HID reports are read instead (RIDEV_INPUTSINK, so
// they arrive in the background), and a horizontal three-finger swipe drives
// the ring: open, step per distance moved, switch when the fingers lift.

struct TouchFinger {
    USHORT link = 0;
    LONG xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    double xScale = 1, yScale = 1;  // logical unit -> physical unit
};

struct TouchDevice {
    std::vector<BYTE> preparsed;
    std::vector<TouchFinger> fingers;
    bool hasCount = false;
    USHORT countLink = 0;
    double width = 1;  // physical width, the unit of all gesture distances
    bool valid = false;
};

static std::unordered_map<HANDLE, TouchDevice> g_touchDevices;

enum class SwipeState { Idle, Tracking, Active, Rejected };

static struct {
    SwipeState state = SwipeState::Idle;
    double startX = 0, startY = 0, anchorX = 0;
    // Hybrid-mode reports split one frame across several reports.
    int remaining = 0;
    int frameFingers = 0;
    double frameX = 0, frameY = 0;
} g_swipe;

constexpr double kSwipeOpen = 0.05;    // of the pad width, to open the ring
constexpr double kSwipeStep = 0.15;    // of the pad width, per window
constexpr double kSwipeVertical = 0.05;
constexpr double kSwipeVerticalFire = 0.10;  // up/down: Task View, desktop

static TouchDevice* GetTouchDevice(HANDLE h) {
    auto it = g_touchDevices.find(h);
    if (it != g_touchDevices.end()) {
        return it->second.valid ? &it->second : nullptr;
    }
    TouchDevice& d = g_touchDevices[h];
    UINT size = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &size) != 0 ||
        !size) {
        return nullptr;
    }
    d.preparsed.resize(size);
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d.preparsed.data(),
                               &size) == (UINT)-1) {
        return nullptr;
    }
    auto pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(d.preparsed.data());
    HIDP_CAPS caps{};
    if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) {
        return nullptr;
    }
    USHORT n = caps.NumberInputValueCaps;
    if (!n) {
        return nullptr;
    }
    std::vector<HIDP_VALUE_CAPS> vc(n);
    if (HidP_GetValueCaps(HidP_Input, vc.data(), &n, pp) !=
        HIDP_STATUS_SUCCESS) {
        return nullptr;
    }
    std::unordered_map<USHORT, TouchFinger> byLink;
    std::unordered_map<USHORT, int> axes;  // bit 1 = X, bit 2 = Y
    for (USHORT i = 0; i < n; i++) {
        const auto& c = vc[i];
        USHORT usage = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
        if (c.UsagePage == 0x0D && usage == 0x54) {
            d.hasCount = true;
            d.countLink = c.LinkCollection;
        } else if (c.UsagePage == 0x01 && (usage == 0x30 || usage == 0x31)) {
            LONG lmin = c.LogicalMin, lmax = c.LogicalMax;
            if (lmax <= lmin) {
                continue;
            }
            TouchFinger& f = byLink[c.LinkCollection];
            f.link = c.LinkCollection;
            double scale = c.PhysicalMax > c.PhysicalMin
                               ? double(c.PhysicalMax - c.PhysicalMin) /
                                     double(lmax - lmin)
                               : 1.0;
            if (usage == 0x30) {
                f.xMin = lmin, f.xMax = lmax, f.xScale = scale;
                axes[c.LinkCollection] |= 1;
            } else {
                f.yMin = lmin, f.yMax = lmax, f.yScale = scale;
                axes[c.LinkCollection] |= 2;
            }
        }
    }
    for (auto& [link, f] : byLink) {
        if (axes[link] == 3) {
            d.fingers.push_back(f);
        }
    }
    std::sort(d.fingers.begin(), d.fingers.end(),
              [](const TouchFinger& a, const TouchFinger& b) {
                  return a.link < b.link;
              });
    if (d.fingers.empty()) {
        return nullptr;
    }
    const auto& f0 = d.fingers[0];
    d.width = (f0.xMax - f0.xMin) * f0.xScale;
    if (d.width <= 0) {
        return nullptr;
    }
    d.valid = true;
    Wh_Log(L"Touchpad: %u finger slots, contact count %s",
           (unsigned)d.fingers.size(), d.hasCount ? L"yes" : L"no");
    return &d;
}

// One complete frame: how many fingers touch and where their centre is.
static void OnTouchFrame(int fingers, double x, double y) {
    LookupTapFrame(fingers, x, y);
    if (!g_settings.touchpadGesture) {
        return;
    }
    HWND hwnd = g_hwnd;
    auto& s = g_swipe;
    if (fingers != 3) {
        if (s.state == SwipeState::Active) {
            PostMessageW(hwnd, WM_RS_GESTURE_END, 0, 0);
        }
        // A fourth finger or a lifted one ends this swipe; a new one starts
        // only once the pad is free again.
        if (fingers == 0) {
            s.state = SwipeState::Idle;
        } else if (s.state != SwipeState::Idle) {
            s.state = SwipeState::Rejected;
        }
        return;
    }
    switch (s.state) {
        case SwipeState::Idle:
            s.state = SwipeState::Tracking;
            s.startX = x, s.startY = y;
            break;
        case SwipeState::Tracking: {
            double dx = x - s.startX, dy = y - s.startY;
            if (std::fabs(dy) > kSwipeVerticalFire &&
                std::fabs(dy) > std::fabs(dx) * 1.5) {
                // Touchpad Y grows downwards.
                PostMessageW(hwnd, dy < 0 ? WM_RS_SWIPE_UP : WM_RS_SWIPE_DOWN,
                             0, 0);
                s.state = SwipeState::Rejected;  // one action per swipe
            } else if (std::fabs(dy) > kSwipeVertical &&
                       std::fabs(dy) > std::fabs(dx)) {
                // Still going vertical: wait for the up/down distance.
            } else if (std::fabs(dx) > kSwipeOpen &&
                       std::fabs(dx) > std::fabs(dy) * 1.5) {
                s.state = SwipeState::Active;
                s.anchorX = x;
                PostMessageW(hwnd, WM_RS_GESTURE_START, 0, 0);
            }
            break;
        }
        case SwipeState::Active:
            while (x - s.anchorX >= kSwipeStep) {
                s.anchorX += kSwipeStep;
                PostMessageW(hwnd, WM_RS_NEXT, 0, 0);
            }
            while (s.anchorX - x >= kSwipeStep) {
                s.anchorX -= kSwipeStep;
                PostMessageW(hwnd, WM_RS_PREV, 0, 0);
            }
            break;
        case SwipeState::Rejected:
            break;
    }
}

static void OnTouchReport(TouchDevice& d, BYTE* report, ULONG len) {
    auto pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(d.preparsed.data());
    auto& s = g_swipe;
    int slots = (int)d.fingers.size();
    if (d.hasCount) {
        ULONG count = 0;
        if (HidP_GetUsageValue(HidP_Input, 0x0D, d.countLink, 0x54, &count,
                               pp, (PCHAR)report, len) ==
                HIDP_STATUS_SUCCESS &&
            count > 0) {
            // First report of a frame.
            s.remaining = (int)count;
            s.frameFingers = 0;
            s.frameX = s.frameY = 0;
        }
        if (s.remaining <= 0) {
            return;
        }
        slots = std::min(slots, s.remaining);
    } else {
        s.remaining = slots;
        s.frameFingers = 0;
        s.frameX = s.frameY = 0;
    }

    for (int i = 0; i < slots; i++) {
        const TouchFinger& f = d.fingers[i];
        USAGE usages[16];
        ULONG nUsages = 16;
        bool tip = false;
        if (HidP_GetUsages(HidP_Input, 0x0D, f.link, usages, &nUsages, pp,
                           (PCHAR)report, len) == HIDP_STATUS_SUCCESS) {
            for (ULONG u = 0; u < nUsages; u++) {
                tip |= usages[u] == 0x42;
            }
        }
        ULONG ux = 0, uy = 0;
        if (tip &&
            HidP_GetUsageValue(HidP_Input, 0x01, f.link, 0x30, &ux, pp,
                               (PCHAR)report, len) == HIDP_STATUS_SUCCESS &&
            HidP_GetUsageValue(HidP_Input, 0x01, f.link, 0x31, &uy, pp,
                               (PCHAR)report, len) == HIDP_STATUS_SUCCESS) {
            s.frameFingers++;
            s.frameX += (LONG(ux) - f.xMin) * f.xScale / d.width;
            s.frameY += (LONG(uy) - f.yMin) * f.yScale / d.width;
        }
    }
    s.remaining -= slots;
    if (s.remaining <= 0) {
        int n = s.frameFingers;
        OnTouchFrame(n, n ? s.frameX / n : 0, n ? s.frameY / n : 0);
    }
}

static void OnRawInput(HRAWINPUT hri) {
    static std::vector<BYTE> buf;
    UINT size = 0;
    if (GetRawInputData(hri, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 ||
        !size) {
        return;
    }
    if (buf.size() < size) {
        buf.resize(size);
    }
    if (GetRawInputData(hri, RID_INPUT, buf.data(), &size,
                        sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        return;
    }
    auto* ri = reinterpret_cast<RAWINPUT*>(buf.data());
    if (ri->header.dwType != RIM_TYPEHID ||
        !(g_settings.touchpadGesture || g_settings.lookupTap)) {
        return;
    }
    TouchDevice* d = GetTouchDevice(ri->header.hDevice);
    if (!d) {
        return;
    }
    g_lastTouchTick = GetTickCount64();
    const DWORD each = ri->data.hid.dwSizeHid;
    for (DWORD i = 0; i < ri->data.hid.dwCount; i++) {
        OnTouchReport(*d, ri->data.hid.bRawData + i * each, each);
    }
}

static void RegisterTouchpad(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x0D;  // digitizer
    rid.usUsage = 0x05;      // touch pad
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        Wh_Log(L"RegisterRawInputDevices failed: %u", GetLastError());
    }
}

static void SendKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = k;
        i.ki.dwExtraInfo = kInjectedMagic;
        in.push_back(i);
    }
    for (auto it = keys.end(); it != keys.begin();) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = *--it;
        i.ki.dwFlags = KEYEVENTF_KEYUP;
        i.ki.dwExtraInfo = kInjectedMagic;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

static bool g_desktopShown = false;  // by a swipe down
static bool g_taskViewOpen = false;  // by a swipe up

static bool ForegroundIsExplorerShell(bool* isDesktop) {
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = L"";
    if (fg) {
        GetClassNameW(fg, cls, ARRAYSIZE(cls));
    }
    *isDesktop = !fg || wcscmp(cls, L"Progman") == 0 ||
                 wcscmp(cls, L"WorkerW") == 0 ||
                 wcscmp(cls, L"Shell_TrayWnd") == 0;
    DWORD pid = 0;
    if (fg) {
        GetWindowThreadProcessId(fg, &pid);
    }
    return EndsWithI(GetProcessPath(pid), L"\explorer.exe");
}

// Windows' three-finger up/down: up opens Task View (or brings the windows
// back after a swipe down), down shows the desktop (or closes Task View).
static void OnSwipeVertical(bool up) {
    if (g_visible) {
        return;
    }
    bool isDesktop = false;
    bool shell = ForegroundIsExplorerShell(&isDesktop);
    if (up) {
        if (g_desktopShown && isDesktop) {
            SendKeys({VK_LWIN, 'D'});
            g_desktopShown = false;
        } else if (!(g_taskViewOpen && shell && !isDesktop)) {
            SendKeys({VK_LWIN, VK_TAB});
            g_taskViewOpen = true;
            g_desktopShown = false;
        }
    } else {
        if (g_taskViewOpen && shell && !isDesktop) {
            SendKeys({VK_ESCAPE});
        } else if (!(g_desktopShown && isDesktop)) {
            SendKeys({VK_LWIN, 'D'});
            g_desktopShown = true;
        }
        g_taskViewOpen = false;
    }
}

static void OnGestureStart() {
    if (g_visible || g_pending) {
        return;  // already open from the keyboard
    }
    g_pending = true;
    ShowSwitcher(false);
    if (g_visible) {
        g_gestureActive = true;
        g_lastTouchTick = GetTickCount64();
    }
}

// ============================================================================
// Lookup (tool process): three-finger tap -> macOS-style word popover
// ============================================================================
//
// A three-finger tap looks up the text under the pointer: the selection if
// the pointer is on it, otherwise the word. The word comes from UI Automation
// (the app's own text), or from OCR of the screen around the pointer when the
// app doesn't expose its text. The popover shows WordNet (offline), a
// Wikipedia summary, and what the word means in its sentence from the local
// model (ollama, started on demand, model unloaded after every answer).
//
// Work runs on worker threads that hold a reference to this DLL, so an unload
// in the middle of a lookup can't pull the code out from under them. Results
// come back to the UI thread as WM_RS_LOOKUP_RESULT; stale ones (an older
// generation) are dropped.

constexpr wchar_t kLookupWordNet[] = L"D:\\lookup\\wordnet.tsv";
constexpr wchar_t kLookupOllamaExe[] = L"D:\\ai\\bin\\ollama\\ollama.exe";
constexpr char kLookupModel[] = "qwen3:1.7b";
constexpr wchar_t kLookupClassName[] = L"RadialLookupPopover";

constexpr ULONGLONG kTapMaxMs = 280;   // all three fingers down and up within
constexpr double kTapMaxMove = 0.03;   // of the pad width, while three touch

static std::atomic<bool> g_lookupQuit{false};

// A short log at D:\lookup\lookup.log (also sent to the Windhawk log).
static void LkLog(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, ARRAYSIZE(buf) - 1, fmt, ap);
    va_end(ap);
    buf[ARRAYSIZE(buf) - 1] = 0;
    Wh_Log(L"%s", buf);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1100];
    int n = _snwprintf(line, ARRAYSIZE(line) - 1, L"%02d:%02d:%02d.%03d %s\r\n", t.wHour,
                       t.wMinute, t.wSecond, t.wMilliseconds, buf);
    if (n < 0) return;
    std::string u;
    int m = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
    u.resize(m);
    WideCharToMultiByte(CP_UTF8, 0, line, n, u.data(), m, nullptr, nullptr);
    static std::mutex lock;
    std::lock_guard<std::mutex> g(lock);
    HANDLE f = CreateFileW(L"D:\\lookup\\lookup.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 512 * 1024) {
        CloseHandle(f);  // keep it small: start over
        f = CreateFileW(L"D:\\lookup\\lookup.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
    }
    DWORD w = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
}

static std::wstring LkUtf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string LkWideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

static bool LkIsDevanagari(wchar_t c) { return c >= 0x0900 && c <= 0x097F; }

static bool LkHasDevanagari(const std::wstring& s) {
    for (wchar_t c : s) {
        if (LkIsDevanagari(c)) return true;
    }
    return false;
}

static bool LkWordChar(wchar_t c) {
    return iswalnum(c) || LkIsDevanagari(c) || c == L'\'' || c == L'-' ||
           c == 0x2019 || (c >= 0x00C0 && c <= 0x024F);
}

static std::wstring LkCollapseSpace(const std::wstring& s) {
    std::wstring o;
    bool sp = false;
    for (wchar_t c : s) {
        if (iswspace(c) || c == 0xA0) {
            sp = true;
            continue;
        }
        if (sp && !o.empty()) o += L' ';
        sp = false;
        o += c;
    }
    return o;
}

// Trims punctuation and quotes around a word or phrase, and a possessive 's.
static std::wstring LkCleanWord(std::wstring w) {
    w = LkCollapseSpace(w);
    size_t a = 0, b = w.size();
    while (a < b && !(iswalnum(w[a]) || LkIsDevanagari(w[a]) ||
                      (w[a] >= 0x00C0 && w[a] <= 0x024F))) a++;
    while (b > a && !(iswalnum(w[b - 1]) || LkIsDevanagari(w[b - 1]) ||
                      (w[b - 1] >= 0x00C0 && w[b - 1] <= 0x024F) ||
                      (w[b - 1] >= 0x093E && w[b - 1] <= 0x094D))) b--;
    w = w.substr(a, b - a);
    if (w.size() > 2) {
        size_t n = w.size();
        if ((w[n - 2] == L'\'' || w[n - 2] == 0x2019) &&
            (w[n - 1] == L's' || w[n - 1] == L'S')) {
            w.resize(n - 2);
        }
    }
    return w;
}

// The sentence around [off, off+len) in a paragraph.
static std::wstring LkSentenceAround(const std::wstring& p, size_t off,
                                     size_t len) {
    if (off > p.size()) off = p.size();
    auto isEnd = [](wchar_t c) {
        return c == L'.' || c == L'!' || c == L'?' || c == 0x0964 ||
               c == L'\n' || c == L'\r';
    };
    size_t a = off;
    while (a > 0 && !isEnd(p[a - 1]) && off - a < 300) a--;
    size_t b = std::min(p.size(), off + len);
    while (b < p.size() && !isEnd(p[b]) && b - off < 300) b++;
    if (b < p.size()) b++;
    return LkCollapseSpace(p.substr(a, b - a));
}

// ---------------------------------------------------------------------------
// Finding the word: UI Automation
// ---------------------------------------------------------------------------

struct LkHit {
    std::wstring word, context;
    RECT rect{};
    bool fromSelection = false;
};

static const CLSID kLkClsidCUIAutomation = {
    0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const IID kLkIidIUIAutomation = {
    0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
static const IID kLkIidIUIAutomation2 = {
    0x34723aff, 0x0c9d, 0x49d0, {0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a}};
static const IID kLkIidTextPattern = {
    0x32eba289, 0x3583, 0x42c9, {0x9c, 0x59, 0x3b, 0x6d, 0x9a, 0x1e, 0x9b, 0x6a}};
constexpr PATTERNID kLkTextPatternId = 10014;

static std::wstring LkBstr(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
    if (b) SysFreeString(b);
    return s;
}

// True if a rectangle in the array (x, y, w, h groups) contains the point;
// `out` receives the union of all of them.
static bool LkRectsContain(SAFEARRAY* sa, POINT pt, int tol, RECT* out) {
    if (!sa) return false;
    LONG lb = 0, ub = -1;
    SafeArrayGetLBound(sa, 1, &lb);
    SafeArrayGetUBound(sa, 1, &ub);
    LONG n = ub - lb + 1;
    double* d = nullptr;
    bool hit = false;
    if (n >= 4 && SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
        RECT u{};
        bool any = false;
        for (LONG i = 0; i + 3 < n; i += 4) {
            RECT r{(LONG)d[i], (LONG)d[i + 1], (LONG)(d[i] + d[i + 2]),
                   (LONG)(d[i + 1] + d[i + 3])};
            if (r.right <= r.left || r.bottom <= r.top) continue;
            if (!any) {
                u = r;
                any = true;
            } else {
                UnionRect(&u, &u, &r);
            }
            if (pt.x >= r.left - tol && pt.x <= r.right + tol &&
                pt.y >= r.top - tol && pt.y <= r.bottom + tol) {
                hit = true;
            }
        }
        SafeArrayUnaccessData(sa);
        if (hit && out) *out = u;
    }
    SafeArrayDestroy(sa);
    return hit;
}

static std::wstring LkParagraphContext(IUIAutomationTextRange* r) {
    winrt::com_ptr<IUIAutomationTextRange> para, pre;
    if (FAILED(r->Clone(para.put())) || !para) return {};
    para->ExpandToEnclosingUnit(TextUnit_Paragraph);
    BSTR b = nullptr;
    para->GetText(4000, &b);
    std::wstring ptext = LkBstr(b);
    if (ptext.empty()) return {};
    size_t off = 0;
    if (SUCCEEDED(para->Clone(pre.put())) && pre &&
        SUCCEEDED(pre->MoveEndpointByRange(TextPatternRangeEndpoint_End, r,
                                           TextPatternRangeEndpoint_Start))) {
        BSTR pb = nullptr;
        pre->GetText(4000, &pb);
        off = LkBstr(pb).size();
    }
    BSTR wb = nullptr;
    r->GetText(400, &wb);
    size_t len = LkBstr(wb).size();
    return LkSentenceAround(ptext, off, len);
}

static bool LkUiaWordAt(POINT pt, LkHit& hit) {
    winrt::com_ptr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(kLkClsidCUIAutomation, nullptr,
                                CLSCTX_INPROC_SERVER, kLkIidIUIAutomation,
                                uia.put_void())) ||
        !uia) {
        return false;
    }
    winrt::com_ptr<IUIAutomation2> uia2;
    if (SUCCEEDED(uia->QueryInterface(kLkIidIUIAutomation2, uia2.put_void())) &&
        uia2) {
        uia2->put_ConnectionTimeout(1500);
        uia2->put_TransactionTimeout(2000);
    }
    winrt::com_ptr<IUIAutomationElement> el;
    if (FAILED(uia->ElementFromPoint(pt, el.put())) || !el) return false;
    int pid = 0;
    el->get_CurrentProcessId(&pid);
    if ((DWORD)pid == GetCurrentProcessId()) return false;

    winrt::com_ptr<IUIAutomationTreeWalker> walker;
    uia->get_ControlViewWalker(walker.put());
    winrt::com_ptr<IUIAutomationTextPattern> tp;
    for (int i = 0; i < 10 && el; i++) {
        if (SUCCEEDED(el->GetCurrentPatternAs(kLkTextPatternId, kLkIidTextPattern,
                                              tp.put_void())) &&
            tp) {
            break;
        }
        tp = nullptr;
        if (!walker) break;
        winrt::com_ptr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(el.get(), parent.put()))) break;
        el = parent;
    }
    if (!tp) return false;

    // The selection, if the pointer is on it.
    winrt::com_ptr<IUIAutomationTextRangeArray> sel;
    if (SUCCEEDED(tp->GetSelection(sel.put())) && sel) {
        int n = 0;
        sel->get_Length(&n);
        for (int i = 0; i < n && i < 4; i++) {
            winrt::com_ptr<IUIAutomationTextRange> r;
            if (FAILED(sel->GetElement(i, r.put())) || !r) continue;
            BSTR b = nullptr;
            r->GetText(200, &b);
            std::wstring t = LkCleanWord(LkBstr(b));
            if (t.empty() || t.size() > 120) continue;
            SAFEARRAY* sa = nullptr;
            RECT rc{};
            if (SUCCEEDED(r->GetBoundingRectangles(&sa)) &&
                LkRectsContain(sa, pt, 3, &rc)) {
                hit.word = t;
                hit.rect = rc;
                hit.fromSelection = true;
                hit.context = LkParagraphContext(r.get());
                return true;
            }
        }
    }

    winrt::com_ptr<IUIAutomationTextRange> r;
    if (FAILED(tp->RangeFromPoint(pt, r.put())) || !r) return false;
    if (FAILED(r->ExpandToEnclosingUnit(TextUnit_Word))) return false;
    BSTR b = nullptr;
    r->GetText(120, &b);
    std::wstring t = LkCleanWord(LkBstr(b));
    if (t.empty()) return false;
    SAFEARRAY* sa = nullptr;
    RECT rc{};
    // RangeFromPoint returns the nearest text even far from the pointer.
    if (FAILED(r->GetBoundingRectangles(&sa)) ||
        !LkRectsContain(sa, pt, 3, &rc)) {
        return false;
    }
    hit.word = t;
    hit.rect = rc;
    hit.context = LkParagraphContext(r.get());
    return true;
}

// ---------------------------------------------------------------------------
// Finding the word: OCR of the screen around the pointer
// ---------------------------------------------------------------------------

namespace lkw {
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::Imaging;
using winrt::Windows::Globalization::Language;
using winrt::Windows::Storage::Streams::DataWriter;
}  // namespace lkw

static bool LkOcrPick(const lkw::OcrResult& res, float ix, float iy, float k,
                      LONG ox, LONG oy, LkHit& out) {
    float best = 1e9f;
    bool found = false;
    for (const auto& line : res.Lines()) {
        for (const auto& w : line.Words()) {
            auto r = w.BoundingRect();
            float dx = std::max({r.X - ix, 0.f, ix - (r.X + r.Width)});
            float dy = std::max({r.Y - iy, 0.f, iy - (r.Y + r.Height)});
            float d = std::sqrt(dx * dx + dy * dy);
            if (d < best && d <= 6 * k) {
                best = d;
                found = true;
                out.word = std::wstring(w.Text());
                out.context = std::wstring(line.Text());
                out.rect = {ox + LONG(r.X / k), oy + LONG(r.Y / k),
                            ox + LONG((r.X + r.Width) / k),
                            oy + LONG((r.Y + r.Height) / k)};
            }
        }
    }
    return found;
}

static bool LkOcrWordAt(POINT pt, LkHit& hit) {
    RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            0, 0};
    vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT cap{pt.x - 360, pt.y - 70, pt.x + 360, pt.y + 70};
    IntersectRect(&cap, &cap, &vs);
    int w = cap.right - cap.left, h = cap.bottom - cap.top;
    if (w < 16 || h < 16) return false;
    // Small UI text reads much better enlarged.
    float scale = GetMonitorScale(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    int k = scale < 1.5f ? 2 : 1;
    int W = w * k, H = h * k;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    SetStretchBltMode(mem, HALFTONE);
    StretchBlt(mem, 0, 0, W, H, screen, cap.left, cap.top, w, h,
               SRCCOPY | CAPTUREBLT);
    GdiFlush();
    auto* px = static_cast<uint8_t*>(bits);
    for (int i = 0; i < W * H; i++) px[i * 4 + 3] = 255;

    bool ok = false;
    try {
        lkw::SoftwareBitmap bmp(lkw::BitmapPixelFormat::Bgra8, W, H,
                                lkw::BitmapAlphaMode::Premultiplied);
        lkw::DataWriter dw;
        dw.WriteBytes(winrt::array_view<const uint8_t>(px, px + W * H * 4));
        bmp.CopyFromBuffer(dw.DetachBuffer());
        float ix = float(pt.x - cap.left) * k, iy = float(pt.y - cap.top) * k;

        LkHit en, hi;
        bool haveEn = false, haveHi = false;
        if (auto eng = lkw::OcrEngine::TryCreateFromLanguage(lkw::Language(L"en-US"))) {
            haveEn = LkOcrPick(eng.RecognizeAsync(bmp).get(), ix, iy, (float)k,
                               cap.left, cap.top, en);
        }
        if (auto eng = lkw::OcrEngine::TryCreateFromLanguage(lkw::Language(L"hi-IN"))) {
            haveHi = LkOcrPick(eng.RecognizeAsync(bmp).get(), ix, iy, (float)k,
                               cap.left, cap.top, hi) &&
                     LkHasDevanagari(hi.word);
        }
        if (haveHi) {
            hit = hi;
            ok = true;
        } else if (haveEn) {
            hit = en;
            ok = true;
        }
    } catch (...) {
        ok = false;
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (ok) hit.word = LkCleanWord(hit.word);
    return ok && !hit.word.empty();
}

// ---------------------------------------------------------------------------
// WordNet (D:\lookup\wordnet.tsv, sorted by key bytes; see build-wordnet.py)
// ---------------------------------------------------------------------------

struct LkSense {
    std::wstring lemma, pos, def, example, syns;
};

static std::mutex g_wnLock;
static HANDLE g_wnFile = INVALID_HANDLE_VALUE, g_wnMap = nullptr;
static const char* g_wnData = nullptr;
static size_t g_wnSize = 0;

static bool LkWnOpen() {
    if (g_wnData) return true;
    g_wnFile = CreateFileW(kLookupWordNet, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_wnFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(g_wnFile, &sz);
    g_wnMap = CreateFileMappingW(g_wnFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (g_wnMap) g_wnData = (const char*)MapViewOfFile(g_wnMap, FILE_MAP_READ, 0, 0, 0);
    if (!g_wnData) {
        if (g_wnMap) CloseHandle(g_wnMap);
        CloseHandle(g_wnFile);
        g_wnMap = nullptr;
        g_wnFile = INVALID_HANDLE_VALUE;
        return false;
    }
    g_wnSize = (size_t)sz.QuadPart;
    return true;
}

static void LkWnClose() {
    std::lock_guard<std::mutex> lock(g_wnLock);
    if (g_wnData) UnmapViewOfFile(g_wnData);
    if (g_wnMap) CloseHandle(g_wnMap);
    if (g_wnFile != INVALID_HANDLE_VALUE) CloseHandle(g_wnFile);
    g_wnData = nullptr;
    g_wnMap = nullptr;
    g_wnFile = INVALID_HANDLE_VALUE;
}

static size_t LkWnLineEnd(size_t s) {
    const void* e = memchr(g_wnData + s, '\n', g_wnSize - s);
    return e ? (size_t)((const char*)e - g_wnData) : g_wnSize;
}

static std::string_view LkWnKey(size_t s) {
    size_t e = LkWnLineEnd(s);
    const void* t = memchr(g_wnData + s, '\t', e - s);
    size_t k = t ? (size_t)((const char*)t - g_wnData) : e;
    return std::string_view(g_wnData + s, k - s);
}

// All lines whose key is exactly `key`.
static std::vector<std::string_view> LkWnLines(std::string_view key) {
    std::vector<std::string_view> out;
    size_t lo = 0, hi = g_wnSize;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t s = mid;
        while (s > lo && g_wnData[s - 1] != '\n') s--;
        if (LkWnKey(s) < key) {
            lo = LkWnLineEnd(s) + 1;
        } else {
            hi = s;
        }
    }
    for (size_t s = lo; s < g_wnSize;) {
        if (LkWnKey(s) != key) break;
        size_t e = LkWnLineEnd(s);
        out.emplace_back(g_wnData + s, e - s);
        s = e + 1;
    }
    return out;
}

static std::vector<std::string_view> LkSplitTabs(std::string_view l) {
    std::vector<std::string_view> f;
    size_t a = 0;
    for (size_t i = 0; i <= l.size(); i++) {
        if (i == l.size() || l[i] == '\t') {
            f.push_back(l.substr(a, i - a));
            a = i + 1;
        }
    }
    return f;
}

static std::vector<LkSense> LkDictionary(const std::wstring& word) {
    std::vector<LkSense> out;
    std::wstring lw = word;
    for (auto& c : lw) c = towlower(c);
    std::lock_guard<std::mutex> lock(g_wnLock);
    if (!LkWnOpen()) return out;

    std::vector<std::string> cands{LkWideToUtf8(lw)};
    for (auto l : LkWnLines(cands[0])) {
        auto f = LkSplitTabs(l);
        if (f.size() >= 3 && f[1] == "=") cands.emplace_back(f[2]);
    }
    // WordNet's morphy rules, plus doubled consonants (running, stopped).
    static const char* rules[][2] = {
        {"s", ""},     {"ses", "s"}, {"xes", "x"}, {"zes", "z"}, {"ches", "ch"},
        {"shes", "sh"}, {"men", "man"}, {"ies", "y"}, {"es", "e"}, {"es", ""},
        {"ed", "e"},   {"ed", ""},   {"ing", "e"}, {"ing", ""},  {"er", ""},
        {"est", ""},   {"er", "e"},  {"est", "e"}, {"ly", ""},   {"ily", "y"}};
    std::string w8 = cands[0];
    for (auto& r : rules) {
        size_t n = strlen(r[0]);
        if (w8.size() > n + 1 && w8.compare(w8.size() - n, n, r[0]) == 0) {
            std::string base = w8.substr(0, w8.size() - n) + r[1];
            cands.push_back(base);
            size_t b = base.size();
            if (!*r[1] && b >= 3 && base[b - 1] == base[b - 2]) {
                cands.push_back(base.substr(0, b - 1));
            }
        }
    }

    std::vector<std::string> seen;
    std::vector<std::string> posTaken;
    for (const auto& c : cands) {
        if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
        seen.push_back(c);
        for (auto l : LkWnLines(c)) {
            auto f = LkSplitTabs(l);
            if (f.size() < 5 || f[1] == "=") continue;
            std::string pos(f[1]);
            // A part of speech comes from the closest form only.
            bool taken = std::find(posTaken.begin(), posTaken.end(), pos + "|" + c) ==
                             posTaken.end() &&
                         std::any_of(posTaken.begin(), posTaken.end(),
                                     [&](const std::string& p) {
                                         return p.compare(0, pos.size() + 1, pos + "|") == 0;
                                     });
            if (taken) continue;
            posTaken.push_back(pos + "|" + c);
            LkSense s;
            s.lemma = LkUtf8ToWide(f[0]);
            s.pos = pos == "n" ? L"noun" : pos == "v" ? L"verb" : pos == "a" ? L"adjective" : L"adverb";
            s.def = LkUtf8ToWide(f[2]);
            s.example = LkUtf8ToWide(f[3]);
            s.syns = LkUtf8ToWide(f[4]);
            out.push_back(std::move(s));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// HTTP (WinHTTP) and a minimal JSON string reader
// ---------------------------------------------------------------------------

static bool LkHttp(const std::wstring& host, INTERNET_PORT port, bool https,
                   const wchar_t* verb, const std::wstring& path,
                   const std::string& body, DWORD timeoutMs, std::string& out,
                   DWORD& status) {
    out.clear();
    status = 0;
    HINTERNET ses = WinHttpOpen(L"WindowsLookup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        ses = WinHttpOpen(L"WindowsLookup/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!ses) return false;
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, verb, path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             https ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        const wchar_t* hdr = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                          : L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(req, hdr, body.empty() ? 0 : (DWORD)-1L,
                               (LPVOID)(body.empty() ? nullptr : body.data()),
                               (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                WINHTTP_NO_HEADER_INDEX);
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                size_t at = out.size();
                out.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, out.data() + at, avail, &got)) {
                    ok = false;
                    break;
                }
                out.resize(at + got);
                if (out.size() > 8 * 1024 * 1024) break;
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static bool LkHttpUrl(const std::wstring& url, DWORD timeoutMs, std::string& out) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    DWORD st = 0;
    return LkHttp(host, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS, L"GET", path,
                  "", timeoutMs, out, st) &&
           st == 200;
}

static void LkAppendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

// The string value of the first "key" at or after `from`. Not a full parser:
// enough for the flat fields read from Wikipedia and ollama.
static bool LkJsonStr(const std::string& j, const char* key, std::wstring& out,
                      size_t from = 0, size_t* endPos = nullptr) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    while (p != std::string::npos) {
        size_t q = p + k.size();
        while (q < j.size() && isspace((unsigned char)j[q])) q++;
        if (q < j.size() && j[q] == ':') {
            q++;
            while (q < j.size() && isspace((unsigned char)j[q])) q++;
            if (q >= j.size() || j[q] != '"') return false;
            q++;
            std::string s;
            while (q < j.size() && j[q] != '"') {
                char c = j[q++];
                if (c != '\\') {
                    s += c;
                    continue;
                }
                if (q >= j.size()) break;
                char e = j[q++];
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': break;
                    case 'b': case 'f': break;
                    case 'u': {
                        if (q + 4 > j.size()) break;
                        uint32_t cp = strtoul(j.substr(q, 4).c_str(), nullptr, 16);
                        q += 4;
                        if (cp >= 0xD800 && cp < 0xDC00 && q + 6 <= j.size() &&
                            j[q] == '\\' && j[q + 1] == 'u') {
                            uint32_t lo = strtoul(j.substr(q + 2, 4).c_str(), nullptr, 16);
                            q += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        LkAppendUtf8(s, cp);
                        break;
                    }
                    default: s += e; break;
                }
            }
            out = LkUtf8ToWide(s);
            if (endPos) *endPos = q;
            return true;
        }
        p = j.find(k, p + 1);
    }
    return false;
}

static std::string LkJsonEscape(const std::wstring& w) {
    std::string s = LkWideToUtf8(w), o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c == '\n') {
            o += "\\n";
        } else if (c < 0x20) {
            o += ' ';
        } else {
            o += (char)c;
        }
    }
    return o;
}

static std::wstring LkUrlEncode(const std::wstring& w) {
    std::string s = LkWideToUtf8(w);
    std::wstring o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (wchar_t)c;
        } else {
            wchar_t b[4];
            swprintf(b, 4, L"%%%02X", c);
            o += b;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Wikipedia
// ---------------------------------------------------------------------------

struct LkWiki {
    std::wstring title, extract, url, thumbUrl;
};

static bool LkWikiSummary(const std::wstring& host, std::wstring title, LkWiki& w) {
    for (auto& c : title) {
        if (c == L' ') c = L'_';
    }
    std::string body;
    DWORD st = 0;
    if (!LkHttp(host, 443, true, L"GET",
                L"/api/rest_v1/page/summary/" + LkUrlEncode(title), "", 5000, body,
                st) ||
        st != 200) {
        return false;
    }
    std::wstring type;
    LkJsonStr(body, "type", type);
    if (type == L"disambiguation") return false;
    LkJsonStr(body, "title", w.title);
    LkJsonStr(body, "extract", w.extract);
    size_t t = body.find("\"thumbnail\"");
    if (t != std::string::npos) LkJsonStr(body, "source", w.thumbUrl, t);
    size_t d = body.find("\"desktop\"");
    if (d != std::string::npos) LkJsonStr(body, "page", w.url, d);
    return !w.extract.empty();
}

static bool LkWikipedia(const std::wstring& word, LkWiki& w) {
    std::wstring host = LkHasDevanagari(word) ? L"hi.wikipedia.org" : L"en.wikipedia.org";
    if (LkWikiSummary(host, word, w)) return true;
    // Not an exact title: take the best search match.
    std::string body;
    DWORD st = 0;
    if (!LkHttp(host, 443, true, L"GET",
                L"/w/api.php?action=opensearch&limit=1&namespace=0&format=json&search=" +
                    LkUrlEncode(word),
                "", 5000, body, st) ||
        st != 200) {
        return false;
    }
    // ["query",["Title"],...]
    size_t a = body.find("[\"", body.find(',') );
    if (a == std::string::npos) return false;
    size_t b = body.find('"', a + 2);
    if (b == std::string::npos || b == a + 2) return false;
    std::wstring title;
    std::string fake = "{\"t\":\"" + body.substr(a + 2, b - a - 2) + "\"}";
    LkJsonStr(fake, "t", title);
    return !title.empty() && LkWikiSummary(host, title, w);
}

// ---------------------------------------------------------------------------
// Local model (ollama)
// ---------------------------------------------------------------------------

static bool LkOllamaUp() {
    std::string b;
    DWORD st = 0;
    return LkHttp(L"127.0.0.1", 11434, false, L"GET", L"/api/version", "", 500, b,
                  st) &&
           st == 200;
}

// Starts `ollama serve` with the user's OLLAMA_* settings (models on D:).
static bool LkOllamaStart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            wchar_t name[128];
            DWORD nameLen = ARRAYSIZE(name), type = 0;
            BYTE data[2048];
            DWORD dataLen = sizeof(data) - 2;
            LONG r = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data, &dataLen);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || _wcsnicmp(name, L"OLLAMA_", 7) != 0) continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            data[dataLen] = data[dataLen + 1] = 0;
            wchar_t expanded[2048];
            ExpandEnvironmentStringsW((wchar_t*)data, expanded, ARRAYSIZE(expanded));
            SetEnvironmentVariableW(name, expanded);
        }
        RegCloseKey(key);
    }
    std::wstring cmd = std::wstring(L"\"") + kLookupOllamaExe + L"\" serve";
    std::wstring dir = kLookupOllamaExe;
    dir.resize(dir.rfind(L'\\'));
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(kLookupOllamaExe, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                        dir.c_str(), &si, &pi)) {
        LkLog(L"starting ollama failed: %u", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 80 && !g_lookupQuit; i++) {
        Sleep(250);
        if (LkOllamaUp()) return true;
    }
    return false;
}

static bool LkOllamaAsk(const std::wstring& word, const std::wstring& context,
                        std::wstring& answer) {
    std::wstring prompt;
    if (!context.empty() && context != word) {
        prompt = L"Sentence: \"" + context + L"\"\nIn one or two short sentences, explain what \"" +
                 word + L"\" means in this sentence. Answer in English, plain text, no preamble.";
    } else {
        prompt = L"In one or two short sentences, explain what \"" + word +
                 L"\" means. Answer in English, plain text, no preamble.";
    }
    std::string body = std::string("{\"model\":\"") + kLookupModel + "\",\"prompt\":\"" +
                       LkJsonEscape(prompt) +
                       "\",\"stream\":false,\"think\":false,\"keep_alive\":0,"
                       "\"options\":{\"num_thread\":8,\"num_predict\":110,\"temperature\":0.3}}";
    std::string out;
    DWORD st = 0;
    if (!LkHttp(L"127.0.0.1", 11434, false, L"POST", L"/api/generate", body, 60000, out,
                st) ||
        st != 200 || !LkJsonStr(out, "response", answer)) {
        return false;
    }
    size_t t = answer.find(L"</think>");
    if (t != std::wstring::npos) answer = answer.substr(t + 8);
    answer = LkCollapseSpace(answer);
    return !answer.empty();
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

enum class LkKind { NotFound, Word, Dict, Wiki, AiStatus, Ai };

struct LkMsg {
    UINT gen = 0;
    LkKind kind = LkKind::NotFound;
    std::wstring a, b, c;
    RECT rect{};
    std::vector<LkSense> senses;
    std::string bytes;
    bool ok = false;
};

static void LkPost(LkMsg* m) {
    HWND h = g_hwnd;
    if (!h || g_lookupQuit || !PostMessageW(h, WM_RS_LOOKUP_RESULT, 0, (LPARAM)m)) {
        delete m;
    }
}

static bool LkCurrent(UINT gen) { return !g_lookupQuit && gen == g_lookupGen; }

struct LkJob {
    UINT gen = 0;
    POINT pt{};
    std::wstring word, context;
    HMODULE self = nullptr;
};

static bool LkStartThread(LPTHREAD_START_ROUTINE fn, LkJob* job) {
    // The thread keeps the DLL loaded until it ends (FreeLibraryAndExitThread).
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&LkStartThread), &job->self);
    HANDLE t = CreateThread(nullptr, 0, fn, job, 0, nullptr);
    if (!t) {
        if (job->self) FreeLibrary(job->self);
        delete job;
        return false;
    }
    CloseHandle(t);
    return true;
}

static void LkRunAi(const LkJob& job) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    if (!LkOllamaUp()) {
        auto* m = new LkMsg{job.gen, LkKind::AiStatus};
        m->a = L"Starting the local model\x2026";
        LkPost(m);
        if (!LkOllamaStart()) {
            auto* f = new LkMsg{job.gen, LkKind::Ai};
            f->a = L"Couldn't start the local model.";
            LkPost(f);
            return;
        }
    }
    if (!LkCurrent(job.gen)) return;
    std::wstring ans;
    auto* m = new LkMsg{job.gen, LkKind::Ai};
    m->ok = LkOllamaAsk(job.word, job.context, ans);
    m->a = m->ok ? ans : L"The local model didn't answer.";
    LkLog(L"model: %s", m->ok ? L"answered" : L"no answer");
    LkPost(m);
}

static DWORD WINAPI LkAiThread(LPVOID p) {
    auto* job = static_cast<LkJob*>(p);
    HMODULE self = job->self;
    LkRunAi(*job);
    delete job;
    FreeLibraryAndExitThread(self, 0);
}

static void LkRunLookup(const LkJob& job) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LkHit hit;
    bool ok = false;
    try {
        ok = LkUiaWordAt(job.pt, hit);
    } catch (...) {
        ok = false;
    }
    if (ok) {
        LkLog(L"UIA word '%s'", hit.word.c_str());
    } else if (LkCurrent(job.gen)) {
        hit = {};
        ok = LkOcrWordAt(job.pt, hit);
        if (ok) LkLog(L"OCR word '%s'", hit.word.c_str());
    }
    if (!LkCurrent(job.gen)) {
        LkLog(L"lookup %u superseded", job.gen);
        return;
    }
    if (!ok || hit.word.empty()) {
        LkLog(L"no word at %d,%d", job.pt.x, job.pt.y);
        LkPost(new LkMsg{job.gen, LkKind::NotFound});
        return;
    }
    if (hit.context.size() > 400) hit.context.resize(400);

    auto* wm = new LkMsg{job.gen, LkKind::Word};
    wm->a = hit.word;
    wm->b = hit.context;
    wm->rect = hit.rect;
    LkPost(wm);

    auto* ai = new LkJob{job.gen, job.pt, hit.word, hit.context};
    LkStartThread(LkAiThread, ai);

    auto* dm = new LkMsg{job.gen, LkKind::Dict};
    if (!LkHasDevanagari(hit.word)) dm->senses = LkDictionary(hit.word);
    LkLog(L"dictionary: %u senses", (unsigned)dm->senses.size());
    LkPost(dm);

    if (!LkCurrent(job.gen)) return;
    LkWiki w;
    auto* km = new LkMsg{job.gen, LkKind::Wiki};
    km->ok = LkWikipedia(hit.word, w);
    if (km->ok) {
        km->a = w.title;
        km->b = w.extract;
        km->c = w.url;
        if (!w.thumbUrl.empty() && LkCurrent(job.gen)) {
            LkHttpUrl(w.thumbUrl, 5000, km->bytes);
        }
    }
    LkLog(L"wikipedia: %s '%s', thumb %u bytes", km->ok ? L"found" : L"none", km->a.c_str(),
          (unsigned)km->bytes.size());
    LkPost(km);
}

static DWORD WINAPI LkLookupThread(LPVOID p) {
    auto* job = static_cast<LkJob*>(p);
    HMODULE self = job->self;
    LkRunLookup(*job);
    delete job;
    FreeLibraryAndExitThread(self, 0);
}

// ---------------------------------------------------------------------------
// Popover (UI thread)
// ---------------------------------------------------------------------------

enum class LkState { Pending, None, Done };

static struct {
    UINT gen = 0;
    std::wstring word, context;
    RECT anchor{};
    bool above = false;
    float scale = 1;
    LkState dict = LkState::Pending;
    std::vector<LkSense> senses;
    LkState wiki = LkState::Pending;
    std::wstring wikiTitle, wikiText, wikiUrl;
    std::unique_ptr<gp::Bitmap> thumb;
    LkState ai = LkState::Pending;
    std::wstring aiText = L"Thinking\x2026";
    RECT wikiHit{};  // window-local
} g_pop;

static HWND g_popHwnd = nullptr;
static HDC g_popMem = nullptr;
static HBITMAP g_popDib = nullptr;
static HGDIOBJ g_popOld = nullptr;
static int g_popW = 0, g_popH = 0;
static POINT g_lookupPt{};
static HWINEVENTHOOK g_popFgHook = nullptr;
static ULONGLONG g_popShownAt = 0;
static std::atomic<LONG> g_popL{0}, g_popT{0}, g_popR{0}, g_popB{0};
static HHOOK g_lookupMouseHook = nullptr;  // hook thread

struct LkDpiScope {
    DPI_AWARENESS_CONTEXT old;
    LkDpiScope() { old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~LkDpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

static void LookupMouseHookCtl(bool on);

static LRESULT CALLBACK LookupMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_lookupVisible &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        RECT r{g_popL, g_popT, g_popR, g_popB};
        if (!PtInRect(&r, m->pt)) {
            if (HWND h = g_hwnd) PostMessageW(h, WM_RS_LOOKUP_CLOSE, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Runs on the hook thread (its message loop is what keeps a LL hook alive).
static void LookupMouseHookCtl(bool on) {
    if (on && !g_lookupMouseHook) {
        g_lookupMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LookupMouseProc, g_hInst, 0);
    } else if (!on && g_lookupMouseHook) {
        UnhookWindowsHookEx(g_lookupMouseHook);
        g_lookupMouseHook = nullptr;
    }
}

static void LookupClose(const wchar_t* why = L"") {
    if (g_lookupVisible) LkLog(L"popover closed: %s", why);
    if (g_lookupVisible) {
        g_lookupVisible = false;
        if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_RS_LOOKUP_MHOOK, 0, 0);
    }
    g_lookupGen++;  // workers stop, late results are dropped
    if (g_popFgHook) {
        UnhookWinEvent(g_popFgHook);
        g_popFgHook = nullptr;
    }
    if (g_popHwnd) ShowWindow(g_popHwnd, SW_HIDE);
    g_pop.thumb.reset();
    g_pop.senses.clear();
}

static void CALLBACK LkForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD,
                                      DWORD) {
    if (g_lookupVisible && GetTickCount64() - g_popShownAt > 200) {
        LookupClose(L"app switch");
    }
}

// The look of the other mods (top bar, island, panels): Start's theme. Black
// at 77% with no blur, #1F1F1F tiles for cards, white text with secondary text
// at lower alpha, a faint white hairline, Segoe UI Variable Text, 14 px panels.
static const gp::Color kLkSurface(0xC4, 0, 0, 0);
static const gp::Color kLkTile(0xFF, 0x1F, 0x1F, 0x1F);
static const gp::Color kLkHairline(26, 255, 255, 255);
static const gp::Color kLkFg(255, 255, 255, 255);
static const gp::Color kLkMuted(153, 228, 228, 228);
static const gp::Color kLkFaint(110, 228, 228, 228);

static const wchar_t* LkFamily(bool semibold) {
    static std::wstring regular, semi;
    if (regular.empty()) {
        auto has = [](const wchar_t* name) {
            gp::FontFamily f(name);
            return f.GetLastStatus() == gp::Ok && f.IsAvailable();
        };
        regular = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        semi = has(L"Segoe UI Variable Text Semibold") ? L"Segoe UI Variable Text Semibold"
                                                       : L"Segoe UI Semibold";
    }
    return semibold ? semi.c_str() : regular.c_str();
}

static gp::Font* LkFont(const std::wstring& text, float px, int style, bool semibold = false) {
    const wchar_t* fam = LkHasDevanagari(text) ? L"Nirmala UI" : LkFamily(semibold);
    return new gp::Font(fam, px, style, gp::UnitPixel);
}

// How much the popover keeps when space runs short: 0 everything, 1 fewer
// senses and shorter Wikipedia text, 2 the minimum. The local model's
// section is always kept.
struct LkLevel {
    int perPos, total, wikiLines, aiLines;
    bool examples, syns;
};
static const LkLevel kLkLevels[] = {
    {2, 5, 6, 6, true, true},
    {1, 3, 4, 5, true, false},
    {1, 2, 3, 4, false, false},
};

static float g_lkCardTop[3], g_lkCardBottom[3];  // from the measuring pass

// Lays the popover out, and draws it when `draw`. Returns the content height.
static float LkLayout(gp::Graphics& g, bool draw, float W, float H, int level) {
    const LkLevel& L = kLkLevels[level];
    const float s = g_pop.scale;
    const float outer = 10 * s;       // surface edge to card
    const float pad = 12 * s;         // card edge to text
    const float x0 = outer + pad;
    const float cw = W - 2 * x0;
    float y = 14 * s;

    if (draw) {
        gp::GraphicsPath path;
        AddRoundRect(path, gp::RectF(0.5f, 0.5f, W - 1, H - 1), 14 * s);
        gp::SolidBrush bg(kLkSurface);
        g.FillPath(&bg, &path);
        gp::Pen pen(kLkHairline, 1);
        g.DrawPath(&pen, &path);
    }

    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | gp::StringFormatFlagsLineLimit);
    fmt.SetTrimming(gp::StringTrimmingEllipsisWord);

    auto text = [&](const std::wstring& t, gp::Font* f, const gp::Color& c, float x,
                    float w, int maxLines) -> float {
        if (t.empty()) {
            delete f;
            return 0;
        }
        float lh = f->GetHeight(&g);
        gp::RectF lay(x, y, w, maxLines > 0 ? lh * maxLines + 0.5f : 100000.f);
        gp::RectF box;
        g.MeasureString(t.c_str(), (INT)t.size(), f, lay, &fmt, &box);
        float h = std::min(box.Height, lay.Height);
        if (draw) {
            gp::SolidBrush br(c);
            gp::RectF dr(x, y, w, h + 1);
            g.DrawString(t.c_str(), (INT)t.size(), f, dr, &fmt, &br);
        }
        delete f;
        return h;
    };
    auto cardBegin = [&](int i) {
        y += 8 * s;
        if (draw && g_lkCardBottom[i] > g_lkCardTop[i]) {
            gp::GraphicsPath path;
            AddRoundRect(path,
                         gp::RectF(outer, g_lkCardTop[i], W - 2 * outer,
                                   g_lkCardBottom[i] - g_lkCardTop[i]),
                         10 * s);
            gp::SolidBrush tile(kLkTile);
            g.FillPath(&tile, &path);
        }
        if (!draw) g_lkCardTop[i] = y;
        y += pad - 1 * s;
    };
    auto cardEnd = [&](int i) {
        y += pad;
        if (!draw) g_lkCardBottom[i] = y;
    };
    auto label = [&](const wchar_t* t) {
        y += text(t, LkFont(t, 11 * s, gp::FontStyleRegular, true), kLkMuted, x0, cw, 1) +
             6 * s;
    };

    // Header: the word, on the surface itself.
    y += text(g_pop.word, LkFont(g_pop.word, 20 * s, gp::FontStyleRegular, true), kLkFg,
              outer + 4 * s, W - 2 * outer - 8 * s, 2) +
         2 * s;

    // Dictionary.
    if (!LkHasDevanagari(g_pop.word)) {
        cardBegin(0);
        label(L"Dictionary");
        if (g_pop.dict == LkState::Pending) {
            y += text(L"Looking up\x2026", LkFont(L"", 13 * s, gp::FontStyleRegular), kLkMuted,
                      x0, cw, 1);
        } else if (g_pop.senses.empty()) {
            y += text(L"No entry in the dictionary.", LkFont(L"", 13 * s, gp::FontStyleRegular),
                      kLkMuted, x0, cw, 1);
        } else {
            std::wstring lastHead;
            int shown = 0, perPos = 0;
            std::wstring lw = g_pop.word;
            for (auto& c : lw) c = towlower(c);
            for (const auto& sn : g_pop.senses) {
                std::wstring head = sn.lemma != lw ? sn.lemma + L"  \x00B7  " + sn.pos : sn.pos;
                if (head != lastHead) {
                    if (shown >= L.total) break;
                    if (!lastHead.empty()) y += 6 * s;
                    y += text(head, LkFont(head, 12.5f * s, gp::FontStyleItalic), kLkMuted, x0, cw,
                              1) +
                         3 * s;
                    lastHead = head;
                    perPos = 0;
                }
                if (perPos >= L.perPos || shown >= L.total) continue;
                perPos++;
                shown++;
                std::wstring num = std::to_wstring(perPos);
                float ind = 16 * s;
                text(num, LkFont(num, 13.5f * s, gp::FontStyleRegular), kLkMuted, x0, ind, 1);
                y += text(sn.def, LkFont(sn.def, 13.5f * s, gp::FontStyleRegular), kLkFg, x0 + ind,
                          cw - ind, 3) +
                     2 * s;
                if (L.examples && !sn.example.empty()) {
                    std::wstring ex = L"\x201C" + sn.example + L"\x201D";
                    y += text(ex, LkFont(ex, 12.5f * s, gp::FontStyleItalic), kLkMuted, x0 + ind,
                              cw - ind, 2) +
                         2 * s;
                }
                if (L.syns && perPos == 1 && !sn.syns.empty()) {
                    std::wstring sy = L"Synonyms: " + sn.syns;
                    y += text(sy, LkFont(sy, 12 * s, gp::FontStyleRegular), kLkFaint, x0 + ind,
                              cw - ind, 1) +
                         2 * s;
                }
                y += 3 * s;
            }
            y -= 3 * s;
        }
        cardEnd(0);
    }

    // Wikipedia (the whole card opens the article).
    g_pop.wikiHit = {};
    if (g_pop.wiki != LkState::None) {
        cardBegin(1);
        float top = y;
        label(L"Wikipedia");
        if (g_pop.wiki == LkState::Pending) {
            y += text(L"Searching\x2026", LkFont(L"", 13 * s, gp::FontStyleRegular), kLkMuted, x0,
                      cw, 1);
        } else {
            float th = g_pop.thumb ? 60 * s : 0;
            float tw = g_pop.thumb ? cw - th - 12 * s : cw;
            float ty = y;
            y += text(g_pop.wikiTitle,
                      LkFont(g_pop.wikiTitle, 14.5f * s, gp::FontStyleRegular, true), kLkFg, x0,
                      tw, 2) +
                 3 * s;
            y += text(g_pop.wikiText, LkFont(g_pop.wikiText, 13 * s, gp::FontStyleRegular),
                      kLkMuted, x0, tw, L.wikiLines);
            if (g_pop.thumb) {
                gp::RectF r(W - x0 - th, ty, th, th);
                if (draw) {
                    gp::GraphicsPath path;
                    AddRoundRect(path, r, 8 * s);
                    gp::TextureBrush tb(g_pop.thumb.get());
                    gp::Matrix m;
                    m.Translate(r.X, r.Y);
                    m.Scale(th / g_pop.thumb->GetWidth(), th / g_pop.thumb->GetHeight());
                    tb.SetTransform(&m);
                    g.FillPath(&tb, &path);
                }
                y = std::max(y, ty + th);
            }
            g_pop.wikiHit = {(LONG)outer, (LONG)(top - pad), (LONG)(W - outer), (LONG)(y + pad)};
        }
        cardEnd(1);
    }

    // In this sentence (local model).
    cardBegin(2);
    label(L"In this sentence \x00B7 local model");
    y += text(g_pop.aiText, LkFont(g_pop.aiText, 13.5f * s, gp::FontStyleRegular),
              g_pop.ai == LkState::Done ? kLkFg : kLkMuted, x0, cw, L.aiLines);
    cardEnd(2);

    return y + outer;
}

static void LkFreeSurface() {
    if (g_popMem) {
        SelectObject(g_popMem, g_popOld);
        DeleteDC(g_popMem);
        g_popMem = nullptr;
    }
    if (g_popDib) {
        DeleteObject(g_popDib);
        g_popDib = nullptr;
    }
}

static LRESULT CALLBACK LkPopProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            SetCursor(LoadCursorW(nullptr, PtInRect(&g_pop.wikiHit, p) && !g_pop.wikiUrl.empty()
                                               ? IDC_HAND
                                               : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONUP: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            if (PtInRect(&g_pop.wikiHit, p) && !g_pop.wikiUrl.empty()) {
                std::wstring url = g_pop.wikiUrl;
                LookupClose(L"opened Wikipedia");
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool LkEnsureWindow() {
    if (g_popHwnd) return true;
    LkDpiScope dpi;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = LkPopProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kLookupClassName;
    RegisterClassExW(&wc);
    // Layered and drawn by us with per-pixel alpha, like the bar's panels.
    g_popHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kLookupClassName, L"Lookup", WS_POPUP, 0, 0, 1, 1,
                                nullptr, nullptr, g_hInst, nullptr);
    return g_popHwnd != nullptr;
}

// Redraws the popover and fits the window to it.
static void LkRender(bool firstShow) {
    if (!LkEnsureWindow()) return;
    LkDpiScope dpi;
    const float s = g_pop.scale;
    const int W = (int)std::lround(372 * s);

    // The side of the word with more room, chosen once per lookup so the
    // popover doesn't jump while results arrive.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&g_pop.anchor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    const int gap = (int)std::lround(8 * s);
    const int below = wa.bottom - gap - (g_pop.anchor.bottom + gap);
    const int aboveRoom = (g_pop.anchor.top - gap) - (wa.top + gap);
    if (firstShow) g_pop.above = below < (int)std::lround(460 * s) && aboveRoom > below;
    const int maxH = std::min((int)std::lround(680 * s), std::max(g_pop.above ? aboveRoom : below,
                                                                   (int)std::lround(160 * s)));

    float h = 0;
    int level = 0;
    {
        gp::Bitmap probe(1, 1, PixelFormat32bppPARGB);
        gp::Graphics g(&probe);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        for (level = 0; level < 3; level++) {
            h = LkLayout(g, false, (float)W, 0, level);
            if (h <= maxH) break;
        }
        if (level == 3) level = 2;
    }
    const int H = std::min((int)std::ceil(h), maxH);

    LkFreeSurface();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    g_popDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_popDib) {
        ReleaseDC(nullptr, screen);
        return;
    }
    g_popMem = CreateCompatibleDC(screen);
    g_popOld = SelectObject(g_popMem, g_popDib);
    g_popW = W;
    g_popH = H;
    {
        gp::Bitmap surf(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        gp::Graphics g(&surf);
        g.Clear(gp::Color(0, 0, 0, 0));
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
        LkLayout(g, true, (float)W, (float)H, level);
    }
    GdiFlush();

    int x = g_pop.anchor.left - (int)std::lround(22 * s);
    int y = g_pop.above ? g_pop.anchor.top - gap - H : g_pop.anchor.bottom + gap;
    x = std::clamp(x, (int)wa.left + gap, std::max((int)wa.left + gap, (int)wa.right - gap - W));
    y = std::clamp(y, (int)wa.top + gap, std::max((int)wa.top + gap, (int)wa.bottom - gap - H));
    g_popL = x, g_popT = y, g_popR = x + W, g_popB = y + H;

    POINT pos{x, y}, src{0, 0};
    SIZE size{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_popHwnd, screen, &pos, &size, g_popMem, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    SetWindowPos(g_popHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

static std::unique_ptr<gp::Bitmap> LkDecodeThumb(const std::string& bytes, int px) {
    if (bytes.empty()) return nullptr;
    IStream* st = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    if (!st) return nullptr;
    std::unique_ptr<gp::Bitmap> out;
    {
        gp::Bitmap src(st);
        if (src.GetLastStatus() == gp::Ok && src.GetWidth() && src.GetHeight()) {
            out = std::make_unique<gp::Bitmap>(px, px, PixelFormat32bppPARGB);
            gp::Graphics g(out.get());
            g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
            float sw = (float)src.GetWidth(), sh = (float)src.GetHeight();
            float side = std::min(sw, sh);  // centre square crop
            g.DrawImage(&src, gp::RectF(0, 0, (float)px, (float)px), (sw - side) / 2,
                        (sh - side) / 4, side, side, gp::UnitPixel);
        }
    }
    st->Release();
    return out;
}

static void OnLookupResult(LkMsg* m) {
    std::unique_ptr<LkMsg> own(m);
    if (m->gen != g_lookupGen) return;
    switch (m->kind) {
        case LkKind::NotFound:
            return;
        case LkKind::Word: {
            g_pop.gen = m->gen;
            g_pop.word = m->a;
            g_pop.context = m->b;
            g_pop.anchor = m->rect;
            if (IsRectEmpty(&g_pop.anchor)) {
                g_pop.anchor = {g_lookupPt.x, g_lookupPt.y, g_lookupPt.x + 1, g_lookupPt.y + 12};
            }
            g_pop.scale = GetMonitorScale(MonitorFromRect(&g_pop.anchor, MONITOR_DEFAULTTONEAREST));
            g_pop.dict = LkState::Pending;
            g_pop.senses.clear();
            g_pop.wiki = LkState::Pending;
            g_pop.wikiTitle.clear();
            g_pop.wikiText.clear();
            g_pop.wikiUrl.clear();
            g_pop.thumb.reset();
            g_pop.ai = LkState::Pending;
            g_pop.aiText = L"Thinking\x2026";
            LkRender(true);
            LkLog(L"popover shown for '%s' (context %u chars)", g_pop.word.c_str(),
                  (unsigned)g_pop.context.size());
            g_popShownAt = GetTickCount64();
            if (!g_lookupVisible) {
                g_lookupVisible = true;
                if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_RS_LOOKUP_MHOOK, 1, 0);
            }
            if (!g_popFgHook) {
                g_popFgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                              nullptr, LkForegroundProc, 0, 0,
                                              WINEVENT_OUTOFCONTEXT);
            }
            return;
        }
        case LkKind::Dict:
            g_pop.senses = std::move(m->senses);
            g_pop.dict = LkState::Done;
            break;
        case LkKind::Wiki:
            if (m->ok) {
                g_pop.wiki = LkState::Done;
                g_pop.wikiTitle = m->a;
                g_pop.wikiText = m->b;
                g_pop.wikiUrl = m->c;
                g_pop.thumb = LkDecodeThumb(m->bytes, (int)std::lround(128 * g_pop.scale));
            } else {
                g_pop.wiki = LkState::None;
            }
            break;
        case LkKind::AiStatus:
            if (g_pop.ai == LkState::Pending) g_pop.aiText = m->a;
            break;
        case LkKind::Ai:
            g_pop.ai = m->ok ? LkState::Done : LkState::None;
            g_pop.aiText = m->a;
            break;
    }
    if (g_lookupVisible) LkRender(false);
}

// A tap, or a test request with a point (wp = 1, lp = x, y).
static void OnLookupTap(WPARAM wp, LPARAM lp) {
    if (g_visible || g_pending) return;  // the ring is open
    POINT pt{};
    if (wp == 1) {
        pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
    } else {
        GetPhysicalCursorPos(&pt);
    }
    if (g_lookupVisible) {
        bool sameSpot = std::abs(pt.x - g_lookupPt.x) < 24 && std::abs(pt.y - g_lookupPt.y) < 24;
        LookupClose(L"tap");
        if (sameSpot) return;  // the same tap again closes it
    }
    g_lookupPt = pt;
    LkLog(L"lookup %u at %d,%d (%s)", g_lookupGen + 1, pt.x, pt.y, wp == 1 ? L"test" : L"tap");
    auto* job = new LkJob{++g_lookupGen, pt};
    LkStartThread(LkLookupThread, job);
}

// Tap detection, fed with every touchpad frame (see OnTouchFrame).
static struct {
    bool down = false, candidate = false, seen3 = false;
    int maxFingers = 0;
    ULONGLONG start = 0;
    double x3 = 0, y3 = 0;
} g_tap;

static void LookupTapFrame(int fingers, double x, double y) {
    auto& t = g_tap;
    if (!g_settings.lookupTap) return;
    if (fingers > 0) {
        if (!t.down) {
            t = {};
            t.down = true;
            t.candidate = true;
            t.start = GetTickCount64();
        }
        t.maxFingers = std::max(t.maxFingers, fingers);
        if (fingers > 3 || GetTickCount64() - t.start > kTapMaxMs) t.candidate = false;
        if (fingers == 3) {
            if (!t.seen3) {
                t.seen3 = true;
                t.x3 = x, t.y3 = y;
            } else if (std::fabs(x - t.x3) > kTapMaxMove || std::fabs(y - t.y3) > kTapMaxMove) {
                t.candidate = false;
            }
        }
        return;
    }
    if (t.down && t.candidate && t.maxFingers == 3 && t.seen3 &&
        GetTickCount64() - t.start <= kTapMaxMs) {
        if (HWND h = g_hwnd) PostMessageW(h, WM_RS_LOOKUP_TAP, 0, 0);
    }
    t = {};
}

static void LookupShutdownUi() {
    g_lookupQuit = true;
    LookupClose();
    LkFreeSurface();
    if (g_popHwnd) {
        DestroyWindow(g_popHwnd);
        g_popHwnd = nullptr;
    }
    UnregisterClassW(kLookupClassName, g_hInst);
    g_pop.thumb.reset();
    LkWnClose();
}

static void PublishState() {
    if (g_hwnd) {
        SetWindowTextW(g_hwnd, g_settings.trigger == Trigger::AltTab
                                   ? kTitleReady
                                   : kTitleIdle);
    }
}

// ============================================================================
// Window & thread (tool process)
// ============================================================================

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_hotkeyMessage && g_hotkeyMessage) {
        if (g_settings.trigger == Trigger::AltTab) {
            OnForwardedHotkey(wp != 0);
        }
        return 0;
    }
    switch (msg) {
        case WM_RS_SHOW:
            if (g_visible) {
                MoveSelection(wp ? -1 : 1);
            } else {
                ShowSwitcher(wp != 0);
            }
            return 0;
        case WM_RS_NEXT:
            MoveSelection(1);
            return 0;
        case WM_RS_PREV:
            MoveSelection(-1);
            return 0;
        case WM_RS_ACTIVATE:
            if (g_visible) {
                ActivateSelected();
            }
            return 0;
        case WM_RS_GESTURE_START:
            OnGestureStart();
            return 0;
        case WM_RS_GESTURE_END:
            if (g_visible && g_gestureActive) {
                ActivateSelected();
            }
            return 0;
        case WM_INPUT:
            OnRawInput(reinterpret_cast<HRAWINPUT>(lp));
            break;  // DefWindowProc does the cleanup
        case WM_RS_CLOSEWIN:
            CloseItem(wp == (WPARAM)-1 ? g_sel : (int)wp);
            return 0;
        case WM_RS_FIRST:
            if (g_visible) {
                SetSelection(0);
                Frame();
            }
            return 0;
        case WM_RS_LAST:
            if (g_visible) {
                SetSelection((int)g_items.size() - 1);
                Frame();
            }
            return 0;
        case WM_RS_SWIPE_UP:
        case WM_RS_SWIPE_DOWN:
            OnSwipeVertical(msg == WM_RS_SWIPE_UP);
            return 0;
        case WM_RS_LOOKUP_TAP:
            OnLookupTap(wp, lp);
            return 0;
        case WM_RS_LOOKUP_RESULT:
            OnLookupResult(reinterpret_cast<LkMsg*>(lp));
            return 0;
        case WM_RS_LOOKUP_CLOSE:
            LookupClose(wp == 1 ? L"Esc" : L"click outside");
            return 0;
        case WM_MOUSEMOVE:
            if (g_visible) {
                OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp));
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_visible) {
                int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
                int idx = HitSector(x, y);
                if (idx >= 0) {
                    g_sel = idx;
                    ActivateSelected();
                } else if (!InCenter(x, y)) {
                    HideSwitcher();  // a click outside the ring
                }
            }
            return 0;
        case WM_MBUTTONUP:
            if (g_visible) {
                int idx = HitSector((short)LOWORD(lp), (short)HIWORD(lp));
                if (idx >= 0) {
                    CloseItem(idx);
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (g_visible) {
                MoveSelection(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            }
            return 0;
        case WM_RS_CANCEL:
            if (g_visible) {
                HideSwitcher();
            }
            return 0;
        case WM_RS_RELOAD:
            if (g_visible) HideSwitcher();
            LoadSettings();
            g_appCache.clear();
            PublishState();
            return 0;
        case WM_RS_PREWARM:
            if (!g_visible && !g_pending) {
                ComputeLayout();
                BuildItems((int)std::lround(g_lay.icon * 1.25f));
                g_items.clear();
            }
            return 0;
        case WM_RS_QUIT:
            HideSwitcher();
            LookupClose();
            DestroyWindow(h);
            return 0;
        case WM_TIMER:
            if (wp == kTimerId) {
                OnTimer();
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return g_visible ? HTCLIENT : HTTRANSPARENT;
        case WM_DESTROY:
            g_hwnd = nullptr;
            g_previewHwnd = nullptr;  // owned window, destroyed with us
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Host window for the live DWM thumbnail. Ignores the mouse entirely.
static LRESULT CALLBACK PreviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_LBUTTONUP:
            // Clicking the preview switches to that window.
            if (HWND owner = g_hwnd) {
                PostMessageW(owner, WM_RS_ACTIVATE, 0, 0);
            }
            return 0;
        case WM_MBUTTONUP:
            if (HWND owner = g_hwnd) {
                PostMessageW(owner, WM_RS_CLOSEWIN, (WPARAM)-1, 0);
            }
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI ThreadProc(LPVOID) {
    gp::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    gp::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    WNDCLASSEXW pwc{sizeof(pwc)};
    pwc.lpfnWndProc = PreviewProc;
    pwc.hInstance = g_hInst;
    pwc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    pwc.lpszClassName = kPreviewClassName;
    RegisterClassExW(&pwc);

    // Takes the mouse while open (hover, click, wheel), never the focus.
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST |
                                 WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             kClassName, kTitleIdle, WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, g_hInst, nullptr);

    if (g_hwnd) {
        // Explorer runs at a higher integrity level than this process may, so
        // let its forwarded message through UIPI.
        ChangeWindowMessageFilterEx(g_hwnd, g_hotkeyMessage, MSGFLT_ALLOW,
                                    nullptr);

        // Owned by the menu window, so it always stays above it.
        g_previewHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kPreviewClassName, L"", WS_POPUP, 0, 0, 1, 1, g_hwnd, nullptr,
            g_hInst, nullptr);
        if (g_previewHwnd) {
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
            DwmSetWindowAttribute(g_previewHwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                  &corner, sizeof(corner));
        }

        RegisterTouchpad(g_hwnd);

        // Load icons and app names in advance so the first Alt+Tab is fast.
        PostMessageW(g_hwnd, WM_RS_PREWARM, 0, 0);

        g_hookReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0,
                                    &g_hookThreadId);
        if (g_hookThread) {
            // Always set by HookThreadProc right after installing the hook.
            WaitForSingleObject(g_hookReadyEvent, INFINITE);
        }
        PublishState();
    } else {
        Wh_Log(L"CreateWindowEx failed: %u", GetLastError());
    }
    SetEvent(g_readyEvent);

    if (g_hwnd) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, INFINITE);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
    if (g_hookReadyEvent) {
        CloseHandle(g_hookReadyEvent);
        g_hookReadyEvent = nullptr;
    }
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    LookupShutdownUi();
    FreeDib();
    g_items.clear();
    g_appCache.clear();
    g_defaultIcon.reset();
    UnregisterClassW(kClassName, g_hInst);
    UnregisterClassW(kPreviewClassName, g_hInst);
    gp::GdiplusShutdown(gdipToken);
    return 0;
}

BOOL WhTool_ModInit() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThreadProc), &g_hInst);

    LoadSettings();

    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
        return FALSE;
    }
    return TRUE;
}

void WhTool_ModSettingsChanged() {
    if (HWND hwnd = g_hwnd) {
        PostMessageW(hwnd, WM_RS_RELOAD, 0, 0);
    }
}

void WhTool_ModUninit() {
    if (g_thread) {
        WaitForSingleObject(g_readyEvent, INFINITE);  // always set by ThreadProc
        if (HWND hwnd = g_hwnd) {
            PostMessageW(hwnd, WM_RS_QUIT, 0, 0);
        }
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_readyEvent) {
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
    }
}

// ============================================================================
// Explorer's Alt+Tab hotkey (explorer.exe)
// ============================================================================
//
// The standard switcher is started by a hotkey that Explorer registers for
// itself. The keyboard hook in the tool process normally swallows Tab before
// that, but low-level hooks don't see keys while an elevated window is in
// front. Hotkeys are delivered in every case, so Explorer's "Immersive Shell"
// thread (the one that receives the hotkey) is watched, and the hotkey is
// handed to the switcher instead of opening the standard one.
//
// This is the only part of the mod that runs inside Explorer.

static bool g_isExplorer = false;
static HHOOK g_messageHook = nullptr;
static HANDLE g_messageHookThread = nullptr;  // to know when it ends
static std::atomic<int> g_messageHookCalls{0};
static HANDLE g_explorerStopEvent = nullptr;
static HANDLE g_explorerThread = nullptr;

// Hands the hotkey to the switcher. Returns false if the switcher isn't
// running or doesn't replace Alt+Tab; the standard switcher then opens.
static bool ForwardHotkey(bool shift) {
    HWND switcher = FindWindowW(kClassName, kTitleReady);
    if (!switcher) {
        return false;
    }
    // Explorer has just received the hotkey, so it may pass the foreground
    // on. The switcher needs it to activate the chosen window.
    DWORD processId = 0;
    GetWindowThreadProcessId(switcher, &processId);
    AllowSetForegroundWindow(processId);
    return PostMessageW(switcher, g_hotkeyMessage, shift ? 1 : 0, 0) != FALSE;
}

static LRESULT CALLBACK GetMessageProc(int code, WPARAM wParam, LPARAM lParam) {
    g_messageHookCalls++;

    auto* msg = reinterpret_cast<MSG*>(lParam);
    if (code == HC_ACTION && wParam == PM_REMOVE && msg &&
        msg->message == WM_HOTKEY && HIWORD(msg->lParam) == VK_TAB) {
        const UINT mods = LOWORD(msg->lParam);
        if ((mods & MOD_ALT) && !(mods & (MOD_CONTROL | MOD_WIN)) &&
            ForwardHotkey((mods & MOD_SHIFT) != 0)) {
            // Explorer gets an empty message instead of the hotkey.
            msg->message = WM_NULL;
        }
    }

    const LRESULT result = CallNextHookEx(nullptr, code, wParam, lParam);
    g_messageHookCalls--;
    return result;
}

// The ID of Explorer's thread that receives the hotkey, or 0.
static DWORD FindAltTabThread() {
    DWORD found = 0;
    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more && !found;
         more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != processId) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                   entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name &&
            wcscmp(name, kAltTabThreadName) == 0) {
            found = entry.th32ThreadID;
        }
        if (name) {
            LocalFree(name);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return found;
}

static bool HookAltTabThread() {
    const DWORD threadId = FindAltTabThread();
    if (!threadId) {
        return false;
    }
    HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, threadId);
    if (!thread) {
        return false;
    }
    // A thread of this process: no module (SetWindowsHookEx docs).
    HHOOK hook =
        SetWindowsHookExW(WH_GETMESSAGE, GetMessageProc, nullptr, threadId);
    if (!hook) {
        Wh_Log(L"SetWindowsHookEx(WH_GETMESSAGE) failed: %u", GetLastError());
        CloseHandle(thread);
        return false;
    }
    Wh_Log(L"Hooked Explorer's thread %u", threadId);
    g_messageHook = hook;
    g_messageHookThread = thread;
    return true;
}

static void UnhookAltTabThread() {
    if (g_messageHook) {
        UnhookWindowsHookEx(g_messageHook);
        g_messageHook = nullptr;
    }
    if (g_messageHookThread) {
        CloseHandle(g_messageHookThread);
        g_messageHookThread = nullptr;
    }
    // Let calls already running on Explorer's thread leave the module before
    // it gets unloaded. No new calls start after unhooking, so this ends.
    while (g_messageHookCalls > 0) {
        Sleep(1);
    }
}

static DWORD WINAPI ExplorerThreadProc(LPVOID) {
    // Only the shell process (the one with the taskbar) receives the hotkey.
    // Other Explorer processes (separate folder windows) have nothing to do.
    for (;;) {
        if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            DWORD processId = 0;
            GetWindowThreadProcessId(taskbar, &processId);
            if (processId != GetCurrentProcessId()) {
                return 0;
            }
            break;
        }
        // Explorer is still starting up.
        if (WaitForSingleObject(g_explorerStopEvent, 1000) != WAIT_TIMEOUT) {
            return 0;
        }
    }

    // Hooks the thread, and again if it's ever created anew. Until it's
    // found, it's looked for again, less and less often.
    DWORD interval = 500;
    for (;;) {
        if (!g_messageHook) {
            HookAltTabThread();
        }
        const HANDLE events[] = {g_explorerStopEvent, g_messageHookThread};
        const DWORD result =
            WaitForMultipleObjects(g_messageHook ? 2 : 1, events, FALSE,
                                   g_messageHook ? INFINITE : interval);
        if (result == WAIT_TIMEOUT) {
            interval = std::min<DWORD>(interval * 2, 30000);
        } else if (result == WAIT_OBJECT_0 + 1) {
            // The thread ended (its ID can be reused).
            UnhookAltTabThread();
            interval = 500;
        } else {
            break;
        }
    }

    UnhookAltTabThread();
    return 0;
}

static void StartExplorerPart() {
    // Nothing to block if the switcher doesn't replace Alt+Tab.
    if (g_settings.trigger != Trigger::AltTab) {
        return;
    }
    g_explorerStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_explorerThread =
        CreateThread(nullptr, 0, ExplorerThreadProc, nullptr, 0, nullptr);
}

static void StopExplorerPart() {
    if (g_explorerThread) {
        SetEvent(g_explorerStopEvent);
        WaitForSingleObject(g_explorerThread, INFINITE);
        CloseHandle(g_explorerThread);
        g_explorerThread = nullptr;
    }
    if (g_explorerStopEvent) {
        CloseHandle(g_explorerStopEvent);
        g_explorerStopEvent = nullptr;
    }
}

static bool IsExplorerProcess() {
    WCHAR path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length == ARRAYSIZE(path)) {
        return false;
    }
    PCWSTR name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"explorer.exe") == 0;
}

////////////////////////////////////////////////////////////////////////////////
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
//
// This mod also targets explorer.exe: there, each callback runs the Explorer
// part instead (the lines marked "explorer.exe part").

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    g_hotkeyMessage = RegisterWindowMessageW(kHotkeyMessageName);

    // explorer.exe part.
    if (IsExplorerProcess()) {
        g_isExplorer = true;
        LoadSettings();
        StartExplorerPart();
        return TRUE;
    }

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
    // explorer.exe part.
    if (g_isExplorer) {
        StopExplorerPart();
        LoadSettings();
        StartExplorerPart();
        return;
    }

    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    // explorer.exe part.
    if (g_isExplorer) {
        StopExplorerPart();
        return;
    }

    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
