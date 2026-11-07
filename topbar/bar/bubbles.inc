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

