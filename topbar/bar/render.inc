
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
RECT g_bubbleRect[3]{};  // As last painted: left, centre, right.

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
D2D1_RECT_F g_bubbles[3]{};
bool g_hasBubble[3]{};
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

void Layout(int W, int H) {
    const float s = Scale();
    g_cmds.clear();
    g_hits.clear();
    g_hasBubble[0] = g_hasBubble[1] = g_hasBubble[2] = false;

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

    // Centre bubble: the pill, clock fixed in the middle, growing outward on hover.
    wchar_t timeText[32] = {};
    wchar_t dateText[32] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"hh':'mm tt", timeText, 32);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"ddd d MMM", dateText, 32, nullptr);
    const std::wstring clock = std::wstring(dateText) + L", " + timeText;
    const float clockW = g_text.Measure(clock);
    const float t = g_pillT;
    const float button = kPillButton * s;
    const float grow = (button + gap) * t;
    const float pillW = clockW + 2 * kPillPadX * s + 2 * grow;
    const float pillX = floorf((W - pillW) / 2.0f);
    g_pillRect = {(LONG)pillX, 0, (LONG)(pillX + pillW + 1), H};
    g_bubbles[1] = D2D1::RectF(pillX, by, pillX + pillW, by + bubbleH);
    g_hasBubble[1] = true;
    text(clock, (W - clockW) / 2.0f, by, clockW + 2, bubbleH);
    if (t > 0.01f) {
        const float buttonY = by + (bubbleH - button) / 2.0f;
        const float inset = (bubbleH - button) / 2.0f;
        auto pillButton = [&](float x, IconId id, Action action, int hitId, float iconH) {
            if (t > 0.5f) {
                addHit(x, buttonY, button, button, button / 2.0f, action, hitId);
            }
            iconCmd(id, x + button / 2.0f, by + bubbleH / 2.0f, iconH, t);
        };
        pillButton(pillX + inset, I_bell, Action::Notifications, H_bell, 15.0f * s);
        pillButton(pillX + pillW - inset - button, I_calendar, Action::Calendar, H_calendar, 13.0f * s);
    }

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

    const float limit = pillX - kBubbleClear * s - bpad;
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

    for (int i = 0; i < 3; i++) {
        if (!g_hasBubble[i]) continue;
        color(i == 1 ? kCentreBg : kSideBg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(g_bubbles[i], g_bubbleRadius, g_bubbleRadius), brush);
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
    bool whole = g_dirtyWhole || g_surface.Generation() != before;

    const float s = Scale();
    wchar_t key[160];
    swprintf_s(key, L"%ls|%.3f", g_settings.fontFamily.c_str(), kFontSize * s * g_settings.fontScale / 100.0f);
    if (g_fontKey != key) {
        g_fontKey = key;
        g_text.SetFont(g_settings.fontFamily, kFontSize * s * g_settings.fontScale / 100.0f);
        whole = true;
    }
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
    for (int i = 0; i < 3; i++) {
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
