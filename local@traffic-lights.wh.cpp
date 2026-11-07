// ==WindhawkMod==
// @id              traffic-lights
// @name            Traffic lights
// @description     macOS-style close, minimize and zoom circles on the left of every title bar
// @version         1.0
// @author          local
// @include         *
// @exclude         dwm.exe
// @exclude         windhawk.exe
// @exclude         LockApp.exe
// @exclude         LogonUI.exe
// @exclude         ShellExperienceHost.exe
// @exclude         ShellHost.exe
// @exclude         StartMenuExperienceHost.exe
// @exclude         SearchHost.exe
// @exclude         TextInputHost.exe
// @compilerOptions -luser32 -lgdi32 -ldwmapi -luxtheme -lcomctl32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Traffic lights

Draws three circles (close, minimize, zoom) at the left of the title bar of every
captioned top-level window. The circles live in a small layered window owned by the
app's window and created on the app's own UI thread, so they move, hide and stack
with it, work in elevated apps, and need no cross-process messaging.

* Standard (DWM-drawn) title bars: the native icon and title are hidden with
  SetWindowThemeAttribute, and the title is drawn centered by the mod. The native
  buttons on the right stay: DWM draws them out of process.
* Custom title bars (Chrome, Edge, Electron, Terminal, WinUI 3, File Explorer tabs,
  UWP frames): the circles sit over the top-left corner of the app's own title area.
  Use "Right-side processes" for apps whose top-left content should stay reachable.

Click: close = SC_CLOSE, minimize = SC_MINIMIZE, zoom = SC_MAXIMIZE / SC_RESTORE.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- size: 12
  $name: Circle diameter (px at 100%)
- gap: 8
  $name: Gap between circles (px at 100%)
- leftMargin: 9
  $name: Left margin (px at 100%)
- drawTitle: true
  $name: Centered title on standard title bars
  $description: Hides the native icon and title of DWM-drawn title bars and draws the title centered, so the circles never cover it.
- standardFrames: draw
  $name: Standard title bars
  $options:
  - draw: Show circles
  - skip: Leave alone (for example when Caption Buttons Customizer handles them)
- customFrames: left
  $name: Apps that draw their own title bar
  $options:
  - left: Circles at the top left
  - right: Circles left of the native buttons
  - skip: Leave alone
- customHeight: 0
  $name: Custom title bar height (px at 100%, 0 = per app)
- rightSideProcesses: ""
  $name: Right-side processes
  $description: Comma-separated exe names whose circles go left of the native buttons instead of the top left, e.g. WindowsTerminal.exe, notepad.exe
- skipProcesses: ""
  $name: Skipped processes
  $description: Comma-separated exe names that never get circles.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <math.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>

// ---------------------------------------------------------------------------
// Settings

enum class StdMode { Draw, Skip };
enum class CustomMode { Left, Right, Skip };

struct Settings {
    int size = 12, gap = 8, leftMargin = 9, customHeight = 0;
    bool drawTitle = true;
    StdMode standard = StdMode::Draw;
    CustomMode custom = CustomMode::Left;
    bool processSkipped = false;
    bool processRightSide = false;
};
Settings g_s;

WCHAR g_exeName[MAX_PATH];

static bool ListHasExe(PCWSTR list) {
    if (!list || !*list) return false;
    std::wstring s = list;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(L',', pos);
        if (end == std::wstring::npos) end = s.size();
        std::wstring item = s.substr(pos, end - pos);
        size_t a = item.find_first_not_of(L" \t"), b = item.find_last_not_of(L" \t");
        if (a != std::wstring::npos) {
            item = item.substr(a, b - a + 1);
            if (_wcsicmp(item.c_str(), g_exeName) == 0) return true;
        }
        pos = end + 1;
    }
    return false;
}

static void LoadSettings() {
    // Missing values read as 0 or empty, so 0 means the default.
    Settings s;
    int v;
    if ((v = Wh_GetIntSetting(L"size")) >= 6 && v <= 32) s.size = v;
    if ((v = Wh_GetIntSetting(L"gap")) >= 1 && v <= 32) s.gap = v;
    if ((v = Wh_GetIntSetting(L"leftMargin")) >= 1 && v <= 64) s.leftMargin = v;
    if ((v = Wh_GetIntSetting(L"customHeight")) >= 16 && v <= 96) s.customHeight = v;
    s.drawTitle = Wh_GetIntSetting(L"drawTitle") != 0;
    PCWSTR str = Wh_GetStringSetting(L"standardFrames");
    s.standard = (str && wcscmp(str, L"skip") == 0) ? StdMode::Skip : StdMode::Draw;
    Wh_FreeStringSetting(str);
    str = Wh_GetStringSetting(L"customFrames");
    s.custom = (str && wcscmp(str, L"skip") == 0)    ? CustomMode::Skip
               : (str && wcscmp(str, L"right") == 0) ? CustomMode::Right
                                                     : CustomMode::Left;
    Wh_FreeStringSetting(str);
    str = Wh_GetStringSetting(L"rightSideProcesses");
    s.processRightSide = ListHasExe(str);
    Wh_FreeStringSetting(str);
    str = Wh_GetStringSetting(L"skipProcesses");
    s.processSkipped = ListHasExe(str);
    Wh_FreeStringSetting(str);
    g_s = s;
}

// ---------------------------------------------------------------------------
// Globals

constexpr UINT_PTR kSubclassId = 0x7A11;
constexpr WCHAR kOverlayClass[] = L"WindhawkTrafficLights_" WH_MOD_ID;

HMODULE g_module;
ATOM g_overlayAtom;
UINT g_msgAttach, g_msgDetach, g_msgUpdate;
volatile bool g_unloading;

std::mutex g_lock;
std::unordered_set<HWND> g_windows;

struct Lights {
    HWND owner = nullptr;
    HWND overlay = nullptr;
    bool active = false;
    bool hover = false;
    int pressed = -1;          // button index while the mouse is down
    bool inUpdate = false;
    bool dying = false;        // WM_DESTROY seen: never create an overlay again
    bool ncAttrSet = false;    // we hid the native icon and title
    // Last rendered state, to skip work when only the position changes.
    std::wstring key;
    int x = 0, y = 0, w = 0, h = 0;
    // Layout of the group in overlay coordinates, kept for hit testing.
    float gx = 0, gy = 0, d = 0, g = 0, scale = 1;
    bool enabled[3] = {true, true, true};
    HFONT font = nullptr;
    UINT fontDpi = 0;
};

static void Update(Lights* L, bool force);

// ---------------------------------------------------------------------------
// Helpers

struct DpiScope {
    DPI_AWARENESS_CONTEXT old;
    DpiScope() { old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~DpiScope() { if (old) SetThreadDpiAwarenessContext(old); }
};

static bool IsCloaked(HWND h) {
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c;
}

static bool IsDarkCaption(HWND h) {
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark))))
        return dark != FALSE;
    DWORD v = 1, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

// Typical title band heights (px at 100%) of apps that draw their own title bar.
static int CustomBandHeight(HWND h) {
    if (g_s.customHeight) return g_s.customHeight;
    WCHAR cls[128];
    if (!GetClassNameW(h, cls, ARRAYSIZE(cls))) return 32;
    if (!wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS")) return 40;  // Windows Terminal tab row
    if (!wcsncmp(cls, L"Chrome_WidgetWin_", 17)) return 36;         // Chromium and Electron
    if (!wcscmp(cls, L"CabinetWClass")) return 40;                  // File Explorer tab row
    if (!wcscmp(cls, L"SunAwtFrame")) return 40;                    // JetBrains custom frames
    if (!wcscmp(cls, L"Notepad")) return 40;                        // Notepad tabs
    return 32;                                                      // WinUI, UWP frames, others
}

// Can this window ever get lights? Checked once at creation (cheap filter).
static bool IsCandidateStyle(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_CHILD) return false;
    if (GetAncestor(h, GA_PARENT) != GetDesktopWindow()) return false;  // message-only, owned children
    if (!(style & WS_POPUP)) return true;  // overlapped windows always have a caption
    return (style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) != 0;
}

// ---------------------------------------------------------------------------
// Rendering: premultiplied BGRA, analytic anti-aliasing, no D2D or GDI+ in every process.

struct Canvas {
    int w, h;
    float* px;  // r, g, b, a premultiplied, floats for easy blending
    void Blend(int x, int y, COLORREF c, float a) {
        if (a <= 0 || x < 0 || y < 0 || x >= w || y >= h) return;
        if (a > 1) a = 1;
        float* p = px + 4 * (y * w + x);
        float ia = 1 - a;
        p[0] = GetRValue(c) / 255.f * a + p[0] * ia;
        p[1] = GetGValue(c) / 255.f * a + p[1] * ia;
        p[2] = GetBValue(c) / 255.f * a + p[2] * ia;
        p[3] = a + p[3] * ia;
    }
};

static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

static COLORREF Darken(COLORREF c, float f) {
    return RGB((int)(GetRValue(c) * f), (int)(GetGValue(c) * f), (int)(GetBValue(c) * f));
}

static void DrawLight(Canvas& cv, float cx, float cy, float r, float s, int kind, bool colored,
                      bool glyph, bool pressed, bool dark) {
    static const COLORREF kFill[3] = {RGB(0xFF, 0x5F, 0x57), RGB(0xFE, 0xBC, 0x2E), RGB(0x28, 0xC8, 0x40)};
    static const COLORREF kEdge[3] = {RGB(0xE2, 0x46, 0x3F), RGB(0xE1, 0xA1, 0x16), RGB(0x14, 0xAE, 0x2B)};
    static const COLORREF kGlyph[3] = {RGB(0x4D, 0x00, 0x00), RGB(0x98, 0x57, 0x12), RGB(0x0A, 0x65, 0x0A)};
    COLORREF fill, edge;
    if (colored) {
        fill = kFill[kind];
        edge = kEdge[kind];
    } else if (dark) {
        fill = RGB(0x4E, 0x4E, 0x4E);
        edge = RGB(0x42, 0x42, 0x42);
    } else {
        fill = RGB(0xD6, 0xD6, 0xD6);
        edge = RGB(0xC2, 0xC2, 0xC2);
    }
    if (pressed) {
        fill = Darken(fill, 0.78f);
        edge = Darken(edge, 0.78f);
    }
    float ring = 0.6f * s;
    float hw = 0.55f * s + 0.15f;  // glyph half stroke
    float k = r * 0.48f;           // glyph half length
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float fx = x + 0.5f - cx, fy = y + 0.5f - cy;
            float dist = sqrtf(fx * fx + fy * fy);
            float outer = Clamp01(r - dist + 0.5f);
            if (outer <= 0) continue;
            cv.Blend(x, y, edge, outer);
            cv.Blend(x, y, fill, Clamp01(r - ring - dist + 0.5f));
            if (!glyph) continue;
            float gd;
            if (kind == 0)
                gd = fminf(SegDist(fx, fy, -k, -k, k, k), SegDist(fx, fy, -k, k, k, -k));
            else if (kind == 1)
                gd = SegDist(fx, fy, -k * 1.15f, 0, k * 1.15f, 0);
            else
                gd = fminf(SegDist(fx, fy, -k * 1.1f, 0, k * 1.1f, 0), SegDist(fx, fy, 0, -k * 1.1f, 0, k * 1.1f));
            cv.Blend(x, y, kGlyph[kind], Clamp01(hw - gd + 0.5f) * 0.9f);
        }
    }
}

static HFONT CaptionFont(Lights* L, UINT dpi) {
    if (L->font && L->fontDpi == dpi) return L->font;
    if (L->font) DeleteObject(L->font);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        ncm.lfCaptionFont = {};
        ncm.lfCaptionFont.lfHeight = -MulDiv(9, dpi, 72);
        lstrcpynW(ncm.lfCaptionFont.lfFaceName, L"Segoe UI", LF_FACESIZE);
    }
    ncm.lfCaptionFont.lfQuality = ANTIALIASED_QUALITY;  // grayscale, so coverage can become alpha
    L->font = CreateFontIndirectW(&ncm.lfCaptionFont);
    L->fontDpi = dpi;
    return L->font;
}

// Draws white text on black into a scratch DIB and turns its gray levels into coverage.
static void DrawTitle(Canvas& cv, Lights* L, UINT dpi, const std::wstring& text, int areaL, int areaR,
                      int winW, int ownerOffsetX, COLORREF color) {
    if (text.empty() || areaR - areaL < 20) return;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return;
    HFONT old = (HFONT)SelectObject(dc, CaptionFont(L, dpi));
    RECT calc = {0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), (int)text.size(), &calc, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    int tw = calc.right - calc.left;
    int areaW = areaR - areaL;
    // Centered on the whole window like macOS, pushed right of the circles if needed.
    int left = ownerOffsetX + (winW - tw) / 2;
    if (left < areaL) left = areaL;
    if (left + tw > areaR) left = areaR - tw;
    if (left < areaL) left = areaL;
    int drawW = tw < areaW ? tw : areaW;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = drawW;
    bi.bmiHeader.biHeight = -cv.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HBITMAP oldBmp = (HBITMAP)SelectObject(dc, bmp);
        memset(bits, 0, (size_t)drawW * cv.h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rc = {0, 0, drawW, cv.h};
        DrawTextW(dc, text.c_str(), (int)text.size(), &rc,
                  DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);
        GdiFlush();
        const BYTE* p = (const BYTE*)bits;
        for (int y = 0; y < cv.h; y++)
            for (int x = 0; x < drawW; x++) {
                const BYTE* q = p + 4 * (y * drawW + x);
                BYTE m = q[0] > q[1] ? q[0] : q[1];
                if (q[2] > m) m = q[2];
                if (m) cv.Blend(left + x, y, color, m / 255.f);
            }
        SelectObject(dc, oldBmp);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, old);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------
// Layout and update (always runs on the owner's thread)

struct Layout {
    bool ok = false;
    bool standard = false;
    int x = 0, y = 0, w = 0, h = 0;   // overlay rect, screen px
    float gx = 0, gy = 0;             // group left and center line, overlay px
    float scale = 1;
    UINT dpi = 96;
    int ownerOffsetX = 0, winW = 0;   // visible window left relative to overlay, width
    int titleL = 0, titleR = 0;       // title area, overlay px
};

static Layout ComputeLayout(Lights* L) {
    Layout lo;
    HWND o = L->owner;
    if (g_s.processSkipped) return lo;
    if (!IsWindowVisible(o) || IsIconic(o) || IsCloaked(o)) return lo;
    LONG style = GetWindowLongW(o, GWL_STYLE), ex = GetWindowLongW(o, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (ex & WS_EX_TOOLWINDOW) || !(style & WS_SYSMENU)) return lo;
    bool hasCaption = (style & WS_CAPTION) == WS_CAPTION;
    if (!hasCaption && !((style & WS_THICKFRAME) && (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)))) return lo;

    lo.dpi = GetDpiForWindow(L->overlay);  // the overlay is per-monitor aware, so this is the monitor's DPI
    if (!lo.dpi) lo.dpi = 96;
    float s = lo.dpi / 96.f;
    lo.scale = s;

    RECT wr, vis;
    if (!GetWindowRect(o, &wr)) return lo;
    if (FAILED(DwmGetWindowAttribute(o, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = wr;
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(o, MONITOR_DEFAULTTONEAREST), &mi);
    bool zoomed = IsZoomed(o) != FALSE;
    bool coversMonitor = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                         wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    if (coversMonitor && (!zoomed || !hasCaption)) return lo;  // fullscreen
    if (zoomed) {
        if (vis.top < mi.rcWork.top) vis.top = mi.rcWork.top;
        if (vis.left < mi.rcWork.left) vis.left = mi.rcWork.left;
        if (vis.right > mi.rcWork.right) vis.right = mi.rcWork.right;
    }
    if (vis.right - vis.left < 140 * s || vis.bottom - vis.top < 40 * s) return lo;

    // Where DWM puts its own buttons (window-relative), for the title area and the right placement.
    RECT bb;
    int buttonsLeft = vis.right - (int)(3 * 46 * s);
    if (SUCCEEDED(DwmGetWindowAttribute(o, DWMWA_CAPTION_BUTTON_BOUNDS, &bb, sizeof(bb))) && bb.right > bb.left)
        buttonsLeft = wr.left + bb.left;
    if (buttonsLeft <= vis.left || buttonsLeft > vis.right) buttonsLeft = vis.right - (int)(3 * 46 * s);

    // A standard frame has its caption in the non-client area, above the client (and menu bar).
    POINT ct = {0, 0};
    ClientToScreen(o, &ct);
    int bandTop = vis.top, bandBottom = ct.y;
    if (GetMenu(o)) {
        MENUBARINFO mbi = {sizeof(mbi)};
        if (GetMenuBarInfo(o, OBJID_MENU, 0, &mbi) && mbi.rcBar.bottom > mbi.rcBar.top) bandBottom = mbi.rcBar.top;
    }
    int bandH = bandBottom - bandTop;
    lo.standard = hasCaption && bandH >= 18 * s && bandH <= 90 * s;

    if (lo.standard ? g_s.standard == StdMode::Skip : g_s.custom == CustomMode::Skip) return lo;

    float d = g_s.size * s, g = g_s.gap * s, groupW = 3 * d + 2 * g;
    float pad = 4 * s;
    if (lo.standard) {
        lo.x = vis.left;
        lo.y = bandTop;
        lo.w = buttonsLeft - (int)(2 * s) - vis.left;
        lo.h = bandH;
        lo.gx = g_s.leftMargin * s;
        lo.gy = bandH / 2.f;
        lo.ownerOffsetX = 0;
        lo.winW = vis.right - vis.left;
        lo.titleL = (int)(lo.gx + groupW + 12 * s);
        lo.titleR = lo.w - (int)(8 * s);
    } else {
        float bh = CustomBandHeight(o) * s;
        bool right = g_s.custom == CustomMode::Right || g_s.processRightSide;
        float left = right ? buttonsLeft - 12 * s - groupW : vis.left + g_s.leftMargin * s;
        lo.x = (int)floorf(left - pad);
        lo.y = (int)floorf(bandTop + bh / 2 - d / 2 - pad);
        lo.w = (int)ceilf(groupW + 2 * pad) + 1;
        lo.h = (int)ceilf(d + 2 * pad) + 1;
        lo.gx = left - lo.x;
        lo.gy = bandTop + bh / 2 - lo.y;
    }
    if (lo.w < groupW || lo.h < d) return lo;
    lo.ok = true;
    return lo;
}

static void ApplyNcAttributes(Lights* L, bool hide) {
    if (hide == L->ncAttrSet) return;
    WTA_OPTIONS opt;
    opt.dwMask = WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON;
    opt.dwFlags = hide ? opt.dwMask : 0;
    if (SUCCEEDED(SetWindowThemeAttribute(L->owner, WTA_NONCLIENT, &opt, sizeof(opt)))) L->ncAttrSet = hide;
}

static void ReadEnabled(Lights* L) {
    LONG style = GetWindowLongW(L->owner, GWL_STYLE);
    L->enabled[1] = (style & WS_MINIMIZEBOX) != 0;
    L->enabled[2] = (style & WS_MAXIMIZEBOX) != 0;
    L->enabled[0] = true;
    if (GetClassLongPtrW(L->owner, GCL_STYLE) & CS_NOCLOSE) L->enabled[0] = false;
    HMENU m = GetSystemMenu(L->owner, FALSE);
    if (m) {
        UINT st = GetMenuState(m, SC_CLOSE, MF_BYCOMMAND);
        if (st != (UINT)-1 && (st & (MF_GRAYED | MF_DISABLED))) L->enabled[0] = false;
    }
}

static void Render(Lights* L, const Layout& lo, const std::wstring& title, bool dark) {
    Canvas cv{lo.w, lo.h, nullptr};
    std::vector<float> buf((size_t)lo.w * lo.h * 4, 0.f);
    cv.px = buf.data();
    float s = lo.scale, d = g_s.size * s, g = g_s.gap * s, r = d / 2;

    // Nearly invisible plate under the group, so the gaps between circles still take the mouse.
    int px0 = (int)floorf(lo.gx - 2 * s), px1 = (int)ceilf(lo.gx + 3 * d + 2 * g + 2 * s);
    int py0 = (int)floorf(lo.gy - r - 3 * s), py1 = (int)ceilf(lo.gy + r + 3 * s);
    for (int y = py0; y < py1; y++)
        for (int x = px0; x < px1; x++) cv.Blend(x, y, RGB(0, 0, 0), 1.f / 255.f);

    for (int i = 0; i < 3; i++) {
        bool en = L->enabled[i] && IsWindowEnabled(L->owner);
        bool colored = en && (L->active || L->hover);
        DrawLight(cv, lo.gx + r + i * (d + g), lo.gy, r, s, i, colored, colored && L->hover,
                  en && L->pressed == i, dark);
    }
    if (lo.standard && g_s.drawTitle) {
        COLORREF tc = dark ? (L->active ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x9A, 0x9A, 0x9A))
                           : (L->active ? RGB(0x00, 0x00, 0x00) : RGB(0x80, 0x80, 0x80));
        DrawTitle(cv, L, lo.dpi, title, lo.titleL, lo.titleR, lo.winW, lo.ownerOffsetX, tc);
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = lo.w;
    bi.bmiHeader.biHeight = -lo.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bmp && bits) {
        BYTE* out = (BYTE*)bits;
        for (size_t i = 0, n = (size_t)lo.w * lo.h; i < n; i++) {
            const float* p = cv.px + 4 * i;
            out[4 * i + 0] = (BYTE)(p[2] * 255.f + 0.5f);
            out[4 * i + 1] = (BYTE)(p[1] * 255.f + 0.5f);
            out[4 * i + 2] = (BYTE)(p[0] * 255.f + 0.5f);
            out[4 * i + 3] = (BYTE)(p[3] * 255.f + 0.5f);
        }
        HBITMAP old = (HBITMAP)SelectObject(dc, bmp);
        POINT dst = {lo.x, lo.y}, src = {0, 0};
        SIZE sz = {lo.w, lo.h};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(L->overlay, nullptr, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l);

static bool EnsureOverlay(Lights* L) {
    if (L->overlay && IsWindow(L->overlay)) return true;
    L->overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kOverlayClass, L"",
                                 WS_POPUP, 0, 0, 1, 1, L->owner, nullptr, g_module, L);
    return L->overlay != nullptr;
}

static void Update(Lights* L, bool force) {
    if (g_unloading || L->inUpdate || L->dying) return;
    if (!IsWindowVisible(L->owner) || IsIconic(L->owner)) {
        // Owned popups hide with a minimized owner on their own, but not with a hidden one.
        if (L->overlay && IsWindowVisible(L->overlay)) ShowWindow(L->overlay, SW_HIDE);
        L->key.clear();
        return;
    }
    L->inUpdate = true;
    {
        DpiScope dpiScope;
        if (EnsureOverlay(L)) {
            Layout lo = ComputeLayout(L);
            if (!lo.ok) {
                if (IsWindowVisible(L->overlay)) ShowWindow(L->overlay, SW_HIDE);
                L->key.clear();
                // No circles (fullscreen, skipped, too small): give the native title back.
                ApplyNcAttributes(L, false);
            } else {
                ApplyNcAttributes(L, lo.standard && g_s.drawTitle);
                ReadEnabled(L);
                bool dark = IsDarkCaption(L->owner);
                std::wstring title;
                if (lo.standard && g_s.drawTitle) {
                    WCHAR buf[512];
                    int n = InternalGetWindowText(L->owner, buf, ARRAYSIZE(buf));
                    title.assign(buf, n > 0 ? n : 0);
                }
                L->gx = lo.gx;
                L->gy = lo.gy;
                L->scale = lo.scale;
                L->d = g_s.size * lo.scale;
                L->g = g_s.gap * lo.scale;
                WCHAR k[160] = {};
                _snwprintf(k, ARRAYSIZE(k) - 1, L"%d %d %d %d %d %d %d %d%d%d %d %d %u|", lo.w, lo.h, (int)(lo.gx * 4), (int)(lo.gy * 4),
                           L->active, L->hover, L->pressed, L->enabled[0], L->enabled[1], L->enabled[2],
                           IsWindowEnabled(L->owner) ? 1 : 0, dark, lo.dpi);
                std::wstring key = std::wstring(k) + title;
                if (force || key != L->key) {
                    Render(L, lo, title, dark);
                    L->key = key;
                } else if (lo.x != L->x || lo.y != L->y) {
                    SetWindowPos(L->overlay, nullptr, lo.x, lo.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                }
                L->x = lo.x; L->y = lo.y; L->w = lo.w; L->h = lo.h;
                if (!IsWindowVisible(L->overlay)) ShowWindow(L->overlay, SW_SHOWNOACTIVATE);
                EnableWindow(L->overlay, IsWindowEnabled(L->owner));
            }
        }
    }
    L->inUpdate = false;
}

// ---------------------------------------------------------------------------
// Overlay window

static int HitButton(Lights* L, int x, int y) {
    float r = L->d / 2;
    for (int i = 0; i < 3; i++) {
        float cx = L->gx + r + i * (L->d + L->g);
        float dx = x + 0.5f - cx, dy = y + 0.5f - L->gy;
        if (dx * dx + dy * dy <= (r + 1.5f * L->scale) * (r + 1.5f * L->scale)) return i;
    }
    return -1;
}

static bool InGroup(Lights* L, int x, int y) {
    float r = L->d / 2;
    return x >= L->gx - 2 * L->scale && x < L->gx + 3 * L->d + 2 * L->g + 2 * L->scale &&
           y >= L->gy - r - 3 * L->scale && y < L->gy + r + 3 * L->scale;
}

static void DoButton(Lights* L, int i) {
    if (i < 0 || !L->enabled[i] || !IsWindowEnabled(L->owner)) return;
    WPARAM cmd = i == 0 ? SC_CLOSE : i == 1 ? SC_MINIMIZE : (IsZoomed(L->owner) ? SC_RESTORE : SC_MAXIMIZE);
    PostMessageW(L->owner, WM_SYSCOMMAND, cmd, 0);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Lights* L = (Lights*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
            return DefWindowProcW(h, m, w, l);
        case WM_NCHITTEST: {
            if (!L) break;
            POINT pt = {(short)LOWORD(l), (short)HIWORD(l)};
            ScreenToClient(h, &pt);
            // Outside the group, the owner (same thread) gets the hit test: drag, double-click, menu.
            return InGroup(L, pt.x, pt.y) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_MOUSEMOVE:
            if (L && !L->hover) {
                L->hover = true;
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                Update(L, false);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (L && L->hover && GetCapture() != h) {
                L->hover = false;
                Update(L, false);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (L) {
                int b = HitButton(L, (short)LOWORD(l), (short)HIWORD(l));
                if (b >= 0 && L->enabled[b]) {
                    L->pressed = b;
                    SetCapture(h);
                    Update(L, false);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (L && L->pressed >= 0) {
                int b = HitButton(L, (short)LOWORD(l), (short)HIWORD(l));
                int was = L->pressed;
                L->pressed = -1;
                ReleaseCapture();
                POINT pt;
                GetCursorPos(&pt);
                RECT rc;
                GetWindowRect(h, &rc);
                if (!PtInRect(&rc, pt)) L->hover = false;
                Update(L, false);
                if (b == was) DoButton(L, b);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (L && L->pressed >= 0) {
                L->pressed = -1;
                Update(L, false);
            }
            return 0;
        case WM_RBUTTONUP:
            return 0;
        case WM_CLOSE:
            return 0;  // apps that close every window of their thread must not close the overlay
        case WM_DPICHANGED:
            if (L) Update(L, true);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// Owner subclass

static void Release(Lights* L) {
    if (L->overlay && IsWindow(L->overlay)) DestroyWindow(L->overlay);
    L->overlay = nullptr;
    if (L->font) DeleteObject(L->font);
    L->font = nullptr;
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_windows.erase(L->owner);
    }
}

static LRESULT CALLBACK OwnerProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    Lights* L = (Lights*)ref;
    if (m == g_msgDetach && g_msgDetach) {
        if (IsWindow(h)) ApplyNcAttributes(L, false);
        Release(L);
        RemoveWindowSubclass(h, OwnerProc, kSubclassId);
        delete L;
        return 0;
    }
    if (m == WM_NCDESTROY) {
        Release(L);
        RemoveWindowSubclass(h, OwnerProc, kSubclassId);
        delete L;
        return DefSubclassProc(h, m, w, l);
    }
    if (m == g_msgUpdate && g_msgUpdate) {
        Update(L, true);
        return 0;
    }
    if (m == WM_DESTROY) {
        L->dying = true;
        if (L->overlay && IsWindow(L->overlay)) DestroyWindow(L->overlay);
        L->overlay = nullptr;
    }
    LRESULT r = DefSubclassProc(h, m, w, l);
    switch (m) {
        case WM_NCACTIVATE:
            L->active = w != FALSE;
            Update(L, false);
            break;
        case WM_ACTIVATE:
            L->active = LOWORD(w) != WA_INACTIVE;
            Update(L, false);
            break;
        case WM_WINDOWPOSCHANGED: {
            const WINDOWPOS* wp = (const WINDOWPOS*)l;
            if (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE) ||
                (wp->flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED)))
                Update(L, false);
            break;
        }
        case WM_SETTEXT:
        case WM_STYLECHANGED:
        case WM_ENABLE:
        case WM_SHOWWINDOW:
        case WM_SIZE:
            Update(L, false);
            break;
        case WM_DPICHANGED:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_DWMCOMPOSITIONCHANGED:
        case 0x0320:  // WM_DWMCOLORIZATIONCHANGED
            Update(L, true);
            break;
    }
    return r;
}

// Must run on the window's thread.
static bool AttachOnThread(HWND h) {
    if (g_unloading) return false;
    DWORD_PTR existing;
    if (GetWindowSubclass(h, OwnerProc, kSubclassId, &existing)) return true;
    Lights* L = new Lights;
    L->owner = h;
    L->active = GetForegroundWindow() == h || GetActiveWindow() == h;
    if (!SetWindowSubclass(h, OwnerProc, kSubclassId, (DWORD_PTR)L)) {
        delete L;
        return false;
    }
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_windows.insert(h);
    }
    if (IsWindowVisible(h)) Update(L, true);
    return true;
}

static LRESULT CALLBACK CallWndProcAttach(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const CWPSTRUCT* c = (const CWPSTRUCT*)l;
        if (c->message == g_msgAttach && c->wParam == 0x7A11) AttachOnThread(c->hwnd);
    }
    return CallNextHookEx(nullptr, code, w, l);
}

static void AttachFromAnyThread(HWND h) {
    DWORD tid = GetWindowThreadProcessId(h, nullptr);
    if (!tid) return;
    if (tid == GetCurrentThreadId()) {
        AttachOnThread(h);
        return;
    }
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcAttach, g_module, tid);
    if (!hook) return;
    DWORD_PTR res;
    SendMessageTimeoutW(h, g_msgAttach, 0x7A11, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &res);
    UnhookWindowsHookEx(hook);
}

// ---------------------------------------------------------------------------
// Discovery: windows created from now on, plus the ones that exist at load.

using CreateWindowExW_t = decltype(&CreateWindowExW);
using CreateWindowExA_t = decltype(&CreateWindowExA);
CreateWindowExW_t CreateWindowExW_Orig;
CreateWindowExA_t CreateWindowExA_Orig;

static void OnCreated(HWND h) {
    if (!h || g_unloading || g_s.processSkipped) return;
    WCHAR cls[64];
    if (GetClassNameW(h, cls, ARRAYSIZE(cls)) && wcscmp(cls, kOverlayClass) == 0) return;
    if (!IsCandidateStyle(h)) return;
    AttachOnThread(h);
}

HWND WINAPI CreateWindowExW_Hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND r = CreateWindowExW_Orig(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (r && !(style & WS_CHILD)) OnCreated(r);
    return r;
}

HWND WINAPI CreateWindowExA_Hook(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND r = CreateWindowExA_Orig(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (r && !(style & WS_CHILD)) OnCreated(r);
    return r;
}

static BOOL CALLBACK EnumExisting(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    WCHAR cls[64];
    if (GetClassNameW(h, cls, ARRAYSIZE(cls)) && wcscmp(cls, kOverlayClass) == 0) return TRUE;
    if (IsCandidateStyle(h)) AttachFromAnyThread(h);
    return TRUE;
}

static std::vector<HWND> TrackedWindows() {
    std::lock_guard<std::mutex> g(g_lock);
    return std::vector<HWND>(g_windows.begin(), g_windows.end());
}

// ---------------------------------------------------------------------------
// Windhawk callbacks

BOOL Wh_ModInit() {
    DWORD session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session) && session == 0) return FALSE;

    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!n || n == ARRAYSIZE(path)) return FALSE;
    PCWSTR base = wcsrchr(path, L'\\');
    lstrcpynW(g_exeName, base ? base + 1 : path, MAX_PATH);

    LoadSettings();

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Wh_ModInit, &g_module);
    g_msgAttach = RegisterWindowMessageW(L"TrafficLights_Attach_" WH_MOD_ID);
    g_msgDetach = RegisterWindowMessageW(L"TrafficLights_Detach_" WH_MOD_ID);
    g_msgUpdate = RegisterWindowMessageW(L"TrafficLights_Update_" WH_MOD_ID);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    g_overlayAtom = RegisterClassExW(&wc);
    if (!g_overlayAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return FALSE;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return FALSE;
    Wh_SetFunctionHook((void*)GetProcAddress(user32, "CreateWindowExW"), (void*)CreateWindowExW_Hook,
                       (void**)&CreateWindowExW_Orig);
    Wh_SetFunctionHook((void*)GetProcAddress(user32, "CreateWindowExA"), (void*)CreateWindowExA_Hook,
                       (void**)&CreateWindowExA_Orig);
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_s.processSkipped) EnumWindows(EnumExisting, 0);
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    for (HWND h : TrackedWindows()) PostMessageW(h, g_msgUpdate, 0, 0);
}

void Wh_ModBeforeUninit() {
    g_unloading = true;
    // Each window's own thread destroys its overlay and removes the subclass.
    for (HWND h : TrackedWindows()) {
        DWORD_PTR res;
        if (GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId())
            SendMessageW(h, g_msgDetach, 0, 0);
        else
            SendMessageTimeoutW(h, g_msgDetach, 0, 0, SMTO_NOTIMEOUTIFNOTHUNG | SMTO_ABORTIFHUNG, 5000, &res);
    }
}

void Wh_ModUninit() {
    if (g_overlayAtom) UnregisterClassW(kOverlayClass, g_module);
}
