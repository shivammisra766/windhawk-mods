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
            delete g_tray[i].pix;
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
            delete icon->pix;
            g_tray.erase(g_tray.begin() + (icon - g_tray.data()));
        }
    } else if (m.message == NIM_ADD || m.message == NIM_MODIFY || m.message == NIM_SETVERSION) {
        if (!icon) {
            if (m.message == NIM_SETVERSION) {
                return;
            }
            g_tray.emplace_back();
            icon = &g_tray.back();
            icon->uid = ++g_trayUid;
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
            if (Pixmap* bitmap = PixmapFromIcon(reinterpret_cast<HICON>((UINT_PTR)m.icon))) {
                delete icon->pix;
                icon->pix = bitmap;
            }
        }
    } else {
        return;
    }
    RemoveDeadTrayIcons();
    InvalidateAll();
}

// Passes a click to the app in the form it asked for, as the taskbar would.
void ClickTrayIcon(int index, bool rightButton) {
    if (index < 0 || index >= (int)g_tray.size()) {
        return;
    }
    const TrayIcon icon = g_tray[index];
    if (!IsWindow(icon.hwnd) || !icon.callback) {
        RemoveDeadTrayIcons();
        InvalidateAll();
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

