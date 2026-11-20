# macOS-style Lookup on a three-finger tap, applied after patch.py, the swipe clamp and
# patch-parity.py. The code lives in radial\lookup.inc; this wires it in.
# Data: D:\lookup\wordnet.tsv (build-wordnet.py). Model: ollama at D:\ai\bin\ollama.
import os
os.chdir(r'D:\backups\windhawk-2026-11-06\src')
p = 'radial-window-switcher.wh.cpp'
s = open(p, encoding='utf-8').read()
inc = open(r'radial\lookup.inc', encoding='utf-8').read()
assert 'LookupTapFrame' not in s, 'already patched'


def rep(a, b, count=1):
    global s
    assert s.count(a) == count, (s.count(a), a[:70])
    s = s.replace(a, b)


# --- libraries, includes, setting -------------------------------------------
rep("// @compilerOptions -lgdiplus -lgdi32 -ldwmapi -lversion -lshcore -lshell32 -ladvapi32 -lhid\n",
    "// @compilerOptions -lgdiplus -lgdi32 -ldwmapi -lversion -lshcore -lshell32 -ladvapi32 -lhid -lole32 -loleaut32 -luuid -lwinhttp -lshlwapi -lruntimeobject\n")
rep("#include <hidpi.h>\n", """#include <hidpi.h>
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
""")
rep("*/\n// ==/WindhawkModSettings==", """- lookupTap: true
  $name: Three-finger tap looks up a word
  $description: Like macOS Lookup - tap with three fingers on a word (or a selection) for the dictionary, Wikipedia and the local model. Set the Windows three-finger tap to "Nothing"
*/
// ==/WindhawkModSettings==""")
rep("    bool touchpadGesture = true;\n};", "    bool touchpadGesture = true;\n    bool lookupTap = true;\n};")
rep("    s.touchpadGesture = Wh_GetIntSetting(L\"touchpadGesture\") != 0;\n",
    "    s.touchpadGesture = Wh_GetIntSetting(L\"touchpadGesture\") != 0;\n"
    "    s.lookupTap = Wh_GetIntSetting(L\"lookupTap\") != 0;\n")

# --- messages, shared state, forward declarations -----------------------------
rep("constexpr UINT WM_RS_SWIPE_DOWN = WM_APP + 16;\n", """constexpr UINT WM_RS_SWIPE_DOWN = WM_APP + 16;
constexpr UINT WM_RS_LOOKUP_TAP = WM_APP + 30;     // wp = 1: lp = test point
constexpr UINT WM_RS_LOOKUP_RESULT = WM_APP + 31;  // lp = LkMsg*
constexpr UINT WM_RS_LOOKUP_CLOSE = WM_APP + 32;
constexpr UINT WM_RS_LOOKUP_MHOOK = WM_APP + 33;   // hook thread, wp = on
""")
rep("static std::atomic<bool> g_sticky{false};\n", """static std::atomic<bool> g_sticky{false};
static std::atomic<bool> g_lookupVisible{false};
static std::atomic<UINT> g_lookupGen{0};
static void LookupTapFrame(int fingers, double x, double y);
static void LookupMouseHookCtl(bool on);
""")

# --- the tap: every touchpad frame goes through the tap detector too ----------
rep("    if (ri->header.dwType != RIM_TYPEHID || !g_settings.touchpadGesture) {",
    "    if (ri->header.dwType != RIM_TYPEHID ||\n        !(g_settings.touchpadGesture || g_settings.lookupTap)) {")
rep("static void OnTouchFrame(int fingers, double x, double y) {\n    HWND hwnd = g_hwnd;\n",
    "static void OnTouchFrame(int fingers, double x, double y) {\n"
    "    LookupTapFrame(fingers, x, y);\n"
    "    if (!g_settings.touchpadGesture) {\n        return;\n    }\n"
    "    HWND hwnd = g_hwnd;\n")

# --- Esc closes the popover (keyboard hook) -----------------------------------
rep("    bool active = g_visible || g_pending;\n", """    bool active = g_visible || g_pending;

    if (g_lookupVisible && !active && down && vk == VK_ESCAPE) {
        PostMessageW(hwnd, WM_RS_LOOKUP_CLOSE, 1, 0);
        return 1;
    }
""")

# --- the click-outside mouse hook lives on the hook thread --------------------
rep("        if (msg.message == WM_RS_REHOOK) {", """        if (msg.message == WM_RS_LOOKUP_MHOOK) {
            LookupMouseHookCtl(msg.wParam != 0);
            continue;
        }
        if (msg.message == WM_RS_REHOOK) {""")
rep("""    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    return 0;""", """    LookupMouseHookCtl(false);
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    return 0;""")

# --- the code itself, and the window messages ---------------------------------
rep("static void PublishState() {", inc + "static void PublishState() {")
rep("""        case WM_RS_SWIPE_UP:
        case WM_RS_SWIPE_DOWN:
            OnSwipeVertical(msg == WM_RS_SWIPE_UP);
            return 0;
""", """        case WM_RS_SWIPE_UP:
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
""")
rep("""        case WM_RS_QUIT:
            HideSwitcher();""", """        case WM_RS_QUIT:
            HideSwitcher();
            LookupClose();""")
rep("""    FreeDib();
    g_items.clear();""", """    LookupShutdownUi();
    FreeDib();
    g_items.clear();""")

open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched lookup')
