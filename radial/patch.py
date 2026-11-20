import os
os.chdir(r'D:\backups\windhawk-2026-11-06\src')
p = 'radial-window-switcher.wh.cpp'
s = open(p, encoding='utf-8').read()
inc = open(r'radial\touchpad.inc', encoding='utf-8').read()


def rep(a, b):
    global s
    assert s.count(a) == 1, a[:60]
    s = s.replace(a, b)


rep("static void PublishState() {", inc + "static void PublishState() {")
rep("""        case WM_RS_ACTIVATE:
            if (g_visible) {
                ActivateSelected();
            }
            return 0;""", """        case WM_RS_ACTIVATE:
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
            break;  // DefWindowProc does the cleanup""")
rep("""        // Load icons and app names in advance so the first Alt+Tab is fast.""",
    """        RegisterTouchpad(g_hwnd);

        // Load icons and app names in advance so the first Alt+Tab is fast.""")
open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched')
