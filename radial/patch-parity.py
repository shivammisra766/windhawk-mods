# Windows Alt+Tab / gesture parity, applied after patch.py and the swipe clamp.
# Adds: Ctrl+Alt+Tab sticky mode, Delete closes the selected window, Home/End,
# mouse (hover, click, middle-click close, wheel, click outside cancels, click
# preview), window title under the app name, three-finger up/down
# (Task View / show desktop / restore).
import os
os.chdir(r'D:\backups\windhawk-2026-11-06\src')
p = 'radial-window-switcher.wh.cpp'
s = open(p, encoding='utf-8').read()


def rep(a, b, count=1):
    global s
    assert s.count(a) == count, (s.count(a), a[:70])
    s = s.replace(a, b)


# --- messages & state -------------------------------------------------------
rep("constexpr UINT WM_RS_GESTURE_END = WM_APP + 11;\n",
    """constexpr UINT WM_RS_GESTURE_END = WM_APP + 11;
constexpr UINT WM_RS_CLOSEWIN = WM_APP + 12;
constexpr UINT WM_RS_FIRST = WM_APP + 13;
constexpr UINT WM_RS_LAST = WM_APP + 14;
constexpr UINT WM_RS_SWIPE_UP = WM_APP + 15;
constexpr UINT WM_RS_SWIPE_DOWN = WM_APP + 16;
""")
rep("static ULONGLONG g_lastTouchTick = 0;\n",
    """static ULONGLONG g_lastTouchTick = 0;
// Ctrl+Alt+Tab: the ring stays open after Alt is released, like Windows.
static std::atomic<bool> g_sticky{false};
static POINT g_showCursor{};  // the cursor when the ring opened
static bool g_mouseMoved = false;
""")

# --- hide resets sticky -----------------------------------------------------
rep("""    g_visible = false;
    g_gestureActive = false;""", """    g_visible = false;
    g_gestureActive = false;
    g_sticky = false;""")

# --- show remembers the cursor ----------------------------------------------
rep("""    g_visible = true;
    g_pending = false;
    g_animStart = GetTickCount64();""", """    g_visible = true;
    g_pending = false;
    g_animStart = GetTickCount64();
    GetCursorPos(&g_showCursor);
    g_mouseMoved = false;""")

# --- timer: sticky mode ignores Alt -----------------------------------------
rep("""    } else if (g_settings.activateOnRelease &&
        GetTickCount64() - g_animStart > 250 && altUpTicks >= 6) {""",
    """    } else if (g_settings.activateOnRelease && !g_sticky &&
        GetTickCount64() - g_animStart > 250 && altUpTicks >= 6) {""")

# --- keyboard hook ----------------------------------------------------------
rep("""            bool otherMods = KeyDown(VK_CONTROL) || KeyDown(VK_LWIN) ||
                             KeyDown(VK_RWIN);
            if (alt && !otherMods) {
                g_pending = true;
                PostMessageW(hwnd, WM_RS_SHOW, KeyDown(VK_SHIFT) ? 1 : 0, 0);
                return 1;
            }""", """            bool ctrl = KeyDown(VK_CONTROL);
            bool win = KeyDown(VK_LWIN) || KeyDown(VK_RWIN);
            if (alt && !win) {
                g_pending = true;
                g_sticky = ctrl;  // Ctrl+Alt+Tab
                PostMessageW(hwnd, WM_RS_SHOW, KeyDown(VK_SHIFT) ? 1 : 0, 0);
                return 1;
            }""")
rep("""        if (up && g_activateOnRelease) {
            PostMessageW(hwnd, WM_RS_ACTIVATE, 0, 0);
        }""", """        if (up && g_activateOnRelease && !g_sticky) {
            PostMessageW(hwnd, WM_RS_ACTIVATE, 0, 0);
        }""")
rep("""        } else if (vk == VK_RETURN || vk == VK_SPACE) {
            PostMessageW(hwnd, WM_RS_ACTIVATE, 0, 0);
        }""", """        } else if (vk == VK_RETURN || vk == VK_SPACE) {
            PostMessageW(hwnd, WM_RS_ACTIVATE, 0, 0);
        } else if (vk == VK_DELETE) {
            PostMessageW(hwnd, WM_RS_CLOSEWIN, (WPARAM)-1, 0);
        } else if (vk == VK_HOME) {
            PostMessageW(hwnd, WM_RS_FIRST, 0, 0);
        } else if (vk == VK_END) {
            PostMessageW(hwnd, WM_RS_LAST, 0, 0);
        }""")

# --- window title under the app name ----------------------------------------
rep("""            // Application name only.
            std::wstring name;
            if (n > 0 && g_sel >= 0 && g_sel < n) {
                name = g_items[g_sel].name;
            } else {
                name = L"No open windows";
            }""", """            // Application name, and the window title below it.
            std::wstring name, title;
            if (n > 0 && g_sel >= 0 && g_sel < n) {
                name = g_items[g_sel].name;
                if (g_items[g_sel].title != name) {
                    title = g_items[g_sel].title;
                }
            } else {
                name = L"No open windows";
            }""")
rep("""            g.DrawString(name.c_str(), (INT)name.size(), &font,
                         gp::RectF(-tw / 2, -cr * 0.6f + yOff, tw, cr * 1.2f),
                         &sf, &textBrush);
        }""", """            if (title.empty()) {
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
        }""")

# --- close / first / last / mouse helpers (before the keyboard hook) --------
rep("""static void OnTimer() {""", """// Closes a window from the ring (Delete, middle click), like Windows'
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

static void OnTimer() {""")

# --- three-finger up/down ---------------------------------------------------
rep("""constexpr double kSwipeVertical = 0.05;""",
    """constexpr double kSwipeVertical = 0.05;
constexpr double kSwipeVerticalFire = 0.10;  // up/down: Task View, desktop""")
rep("""            if (std::fabs(dy) > kSwipeVertical &&
                std::fabs(dy) > std::fabs(dx)) {
                s.state = SwipeState::Rejected;  // up/down: Windows' gesture
            } else if""", """            if (std::fabs(dy) > kSwipeVerticalFire &&
                std::fabs(dy) > std::fabs(dx) * 1.5) {
                // Touchpad Y grows downwards.
                PostMessageW(hwnd, dy < 0 ? WM_RS_SWIPE_UP : WM_RS_SWIPE_DOWN,
                             0, 0);
                s.state = SwipeState::Rejected;  // one action per swipe
            } else if (std::fabs(dy) > kSwipeVertical &&
                       std::fabs(dy) > std::fabs(dx)) {
                // Still going vertical: wait for the up/down distance.
            } else if""")
rep("""static void OnGestureStart() {""", """static void SendKeys(std::initializer_list<WORD> keys) {
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
    return EndsWithI(GetProcessPath(pid), L"\\explorer.exe");
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

static void OnGestureStart() {""")

# --- window procedures ------------------------------------------------------
rep("""        case WM_INPUT:
            OnRawInput(reinterpret_cast<HRAWINPUT>(lp));
            break;  // DefWindowProc does the cleanup""", """        case WM_INPUT:
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
            return 0;""")
rep("""        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_DESTROY:""", """        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return g_visible ? HTCLIENT : HTTRANSPARENT;
        case WM_DESTROY:""")
rep("""        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND: {""", """        case WM_MOUSEACTIVATE:
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
        case WM_ERASEBKGND: {""")
rep("""    // WS_EX_TRANSPARENT: the menu never catches mouse clicks.
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |""",
    """    // Takes the mouse while open (hover, click, wheel), never the focus.
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST |""")
rep("""            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                WS_EX_TRANSPARENT,
            kPreviewClassName,""", """            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kPreviewClassName,""")

# --- readme -----------------------------------------------------------------
rep("""- Press **Esc** to close the ring without switching.""",
    """- Press **Esc** to close the ring without switching.
- **Ctrl+Alt+Tab** keeps the ring open after Alt is released; pick with
  Tab, the arrows or the mouse, then Enter.
- **Delete** (or a middle click) closes the selected window; **Home** and **End**
  jump to the first and last window.
- Mouse: hover a sector to select it, click it (or the preview) to switch,
  scroll to move, click outside the ring to cancel.
- Touchpad: swipe three fingers left/right to open the ring and move, lift to
  switch. Up opens Task View (or brings windows back after a swipe down),
  down shows the desktop (or closes Task View).""")

open(p, 'w', encoding='utf-8', newline='').write(s)
print('parity patched')
