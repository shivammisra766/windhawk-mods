
// ---- Loc: strings per language, generated from strings.tsv by build-bar.ps1 ----

//@@STRINGS@@

// Language of the current settings: the chosen one, else the Windows display language, else English.
const wchar_t* Loc(const wchar_t* key) {
    static std::wstring cachedFor;
    static int column = 0;
    std::wstring want = g_settings.language;
    if (want.empty()) {
        if (g_uiLang.empty()) {  // The display language, not the regional format.
            wchar_t names[256] = {};
            ULONG count = 0, size = 256;
            if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names, &size) || !names[0]) {
                GetUserDefaultLocaleName(names, LOCALE_NAME_MAX_LENGTH);
            }
            g_uiLang = names;  // The first of the double-null list.
        }
        want = g_uiLang;
    }
    if (want != cachedFor) {
        cachedFor = want;
        column = 0;
        for (int i = 1; i < kLocLangCount; i++) {
            const size_t n = wcslen(kLocLangs[i]);
            if (!_wcsnicmp(want.c_str(), kLocLangs[i], n)) { column = i; break; }
        }
    }
    for (const LocEntry& e : kLoc) {
        if (!wcscmp(e.key, key)) {
            if (e.text[column] && e.text[column][0]) return e.text[column];
            return e.text[0];  // English fallback
        }
    }
    return key;
}
