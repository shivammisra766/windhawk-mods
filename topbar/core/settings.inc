
// ---- Settings: one writer, the UI thread ----
// WhTool_ModSettingsChanged runs on another thread and only posts WM_APP_SETTINGS. The UI thread then
// calls LoadSettings. An unset value reads as empty or zero, and that means "use the default".

struct Settings {
    std::wstring fontFamily;  // empty: Segoe UI Variable Text, then Segoe UI
    int fontScale = 100;      // percent of the usual text size
    std::wstring language;    // empty: follow Windows
    int clockHours = 0;       // 0 follow Windows, 12, 24
    bool clockSeconds = false;
    std::wstring dateFormat;  // empty: "ddd d MMM"; otherwise a GetDateFormat pattern
    bool dateAbove = false;   // date on a line above the time
    int monitor = 0;          // 0 primary, -1 follow the pointer (island only), n: the nth monitor
    bool mediaAutoExpand = true;
    std::wstring mediaBlocklist;  // comma separated, lowercased and trimmed by the media module
};

Settings g_settings;
std::wstring g_uiLang;  // Windows display language, cached until the settings are read again.

Settings LoadSettings() {
    Settings s;
    if (PCWSTR v = Wh_GetStringSetting(L"fontFamily")) { s.fontFamily = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"language")) { s.language = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"dateFormat")) { s.dateFormat = v; Wh_FreeStringSetting(v); }
    if (PCWSTR v = Wh_GetStringSetting(L"clockFormat")) {
        s.clockHours = !wcscmp(v, L"h12") ? 12 : !wcscmp(v, L"h24") ? 24 : 0;
        Wh_FreeStringSetting(v);
    }
    if (PCWSTR v = Wh_GetStringSetting(L"monitor")) {
        s.monitor = !wcscmp(v, L"follow") ? -1 : _wtoi(v);  // "primary" and anything else read as 0
        if (s.monitor < -1) s.monitor = 0;
        Wh_FreeStringSetting(v);
    }
    s.clockSeconds = Wh_GetIntSetting(L"clockSeconds") != 0;
    s.dateAbove = Wh_GetIntSetting(L"dateAbove") != 0;
    s.mediaAutoExpand = Wh_GetIntSetting(L"mediaAutoExpand") != 0;
    if (PCWSTR v = Wh_GetStringSetting(L"mediaExpandBlocklist")) { s.mediaBlocklist = v; Wh_FreeStringSetting(v); }
    const int scale = Wh_GetIntSetting(L"fontScale");
    s.fontScale = scale >= 50 && scale <= 200 ? scale : 100;
    g_uiLang.clear();
    return s;
}
