# Alt+Space opens Windows Search (Spotlight's Cmd+Space), applied after patch-lookup.py.
# The keyboard hook swallows Alt+Space (apps would open their window menu) and the UI
# thread then sends Win+S, after cancelling the Alt menu and releasing Alt for the system.
import os
os.chdir(r'D:\backups\windhawk-2026-11-06\src')
p = 'radial-window-switcher.wh.cpp'
s = open(p, encoding='utf-8').read()
assert 'altSpaceSearch' not in s, 'already patched'
assert 'LookupTapFrame' in s, 'apply patch-lookup.py first'


def rep(a, b, count=1):
    global s
    assert s.count(a) == count, (s.count(a), a[:70])
    s = s.replace(a, b)


# --- setting -------------------------------------------------------------------
rep("*/\n// ==/WindhawkModSettings==", """- altSpaceSearch: true
  $name: Alt+Space opens Search
  $description: Like Spotlight's Cmd+Space. Replaces the window menu that Alt+Space opens in apps
*/
// ==/WindhawkModSettings==""")
rep("    bool lookupTap = true;\n};", "    bool lookupTap = true;\n    bool altSpaceSearch = true;\n};")
rep("    s.lookupTap = Wh_GetIntSetting(L\"lookupTap\") != 0;\n",
    "    s.lookupTap = Wh_GetIntSetting(L\"lookupTap\") != 0;\n"
    "    s.altSpaceSearch = Wh_GetIntSetting(L\"altSpaceSearch\") != 0;\n"
    "    g_altSpaceSearch = s.altSpaceSearch;\n")
rep("static Settings g_settings;\n", "static Settings g_settings;\nstatic std::atomic<bool> g_altSpaceSearch{true};  // read by the hook thread\n")

rep("constexpr UINT WM_RS_LOOKUP_MHOOK = WM_APP + 33;   // hook thread, wp = on\n",
    "constexpr UINT WM_RS_LOOKUP_MHOOK = WM_APP + 33;   // hook thread, wp = on\n"
    "constexpr UINT WM_RS_SEARCH = WM_APP + 34;         // Alt+Space\n")

# --- the hook: swallow Alt+Space -------------------------------------------------
rep("""            if (alt && !win) {
                g_pending = true;""", """            if (alt && !win) {
                g_pending = true;""")  # sanity: the Alt+Tab path is where we expect it
rep("""        return CallNextHookEx(nullptr, code, wp, lp);
    }

    // Menu is open (or about to be).""", """        if (down && vk == VK_SPACE && g_altSpaceSearch) {
            bool alt = g_altHeld || (k->flags & LLKHF_ALTDOWN) || KeyDown(VK_MENU);
            if (alt && !KeyDown(VK_CONTROL) && !KeyDown(VK_SHIFT) && !KeyDown(VK_LWIN) &&
                !KeyDown(VK_RWIN)) {
                PostMessageW(hwnd, WM_RS_SEARCH, 0, 0);
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    // Menu is open (or about to be).""")

# --- the UI thread: open Search ----------------------------------------------------
rep("static void PublishState() {", """// Alt+Space: cancel the app's Alt menu, let go of Alt for the system (the user is still
// holding it, which would turn Win+S into Alt+Win+S), then send Win+S. A second Alt+Space
// sends Win+S again, which closes Search, like Spotlight.
static void OpenSearchFromAltSpace() {
    SendDummyKey();
    std::vector<INPUT> in;
    for (WORD k : {VK_LMENU, VK_RMENU}) {
        if (GetAsyncKeyState(k) & 0x8000) {
            INPUT i{};
            i.type = INPUT_KEYBOARD;
            i.ki.wVk = k;
            i.ki.dwFlags = KEYEVENTF_KEYUP | (k == VK_RMENU ? KEYEVENTF_EXTENDEDKEY : 0);
            i.ki.dwExtraInfo = kInjectedMagic;
            in.push_back(i);
        }
    }
    if (!in.empty()) {
        SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
    }
    g_altHeld = false;
    SendKeys({VK_LWIN, 'S'});
}

static void PublishState() {""")
rep("""        case WM_RS_LOOKUP_TAP:
            OnLookupTap(wp, lp);
            return 0;""", """        case WM_RS_LOOKUP_TAP:
            OnLookupTap(wp, lp);
            return 0;
        case WM_RS_SEARCH:
            OpenSearchFromAltSpace();
            return 0;""")

open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched spotlight')
