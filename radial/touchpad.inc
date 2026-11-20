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
            if (std::fabs(dy) > kSwipeVertical &&
                std::fabs(dy) > std::fabs(dx)) {
                s.state = SwipeState::Rejected;  // up/down: Windows' gesture
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
    if (ri->header.dwType != RIM_TYPEHID || !g_settings.touchpadGesture) {
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

