bool IsStartWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(proc, 0, path, &size) &&
                    (wcsstr(path, L"\\StartMenuExperienceHost.exe") || wcsstr(path, L"\\SearchHost.exe"));
    CloseHandle(proc);
    return ok;
}

// Ids above the bar's own timers in types.inc and below the module timers.
constexpr UINT_PTR kProfileWatchTimer = 40;  // While open: clears g_profileOpen if start-menu-pop never answers.
constexpr UINT_PTR kProfileArmTimer = 41;    // Retries arming a Start host that did not answer.
constexpr UINT kProfileWatchMs = 3000;
constexpr UINT kProfileArmRetryMs = 150;
constexpr int kProfileArmRetries = 4;        // About 0.6 s, then the click is dropped.
int g_profileArmLeft = 0;

// Tells start-menu-pop that the next Start open is for the profile flyout. Only Start's own host
// handles the message, so an answer from Search alone does not count.
bool ArmProfile() {
    HWND hwnd = nullptr;
    bool armed = false;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"Windows.UI.Core.CoreWindow", nullptr))) {
        if (IsStartMenuWindow(hwnd)) {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(hwnd, g_profileMessage, 0, 0, SMTO_ABORTIFHUNG, 150, &result)) {
                armed = true;
            }
        }
    }
    return armed;
}

void OpenProfile() {
    g_profileOpen = true;
    SendWinKey(0);
    SetTimer(g_hwnd, kProfileWatchTimer, kProfileWatchMs, nullptr);
}

// Shows Start's own profile flyout under the avatar. See start-menu-pop.
void ToggleProfile() {
    KillTimer(g_hwnd, kProfileArmTimer);
    if (g_profileOpen) {
        g_profileOpen = false;
        KillTimer(g_hwnd, kProfileWatchTimer);
        SendWinKey(0);
        return;
    }
    if (ArmProfile()) {
        OpenProfile();
        return;
    }
    // A host that is starting or briefly busy: try again for a moment instead of dropping the click.
    g_profileArmLeft = kProfileArmRetries;
    SetTimer(g_hwnd, kProfileArmTimer, kProfileArmRetryMs, nullptr);
}

void ProfileArmTick() {
    HWND fg = GetForegroundWindow();
    if (fg && IsStartWindow(fg)) {
        KillTimer(g_hwnd, kProfileArmTimer);  // Start opened some other way: Win now would close it.
        return;
    }
    if (ArmProfile()) {
        KillTimer(g_hwnd, kProfileArmTimer);
        OpenProfile();
    } else if (--g_profileArmLeft <= 0) {
        KillTimer(g_hwnd, kProfileArmTimer);
        Wh_Log(L"Profile: Start's host did not answer");
    }
}

// WM_PROFILE_DONE ends the flyout. If it never comes (Start crashed, start-menu-pop off, a Start
// open that was not taken as the profile), g_profileOpen would stay set and the next click would
// send a bare Win. So look every few seconds, and let it go once Start is no longer in front.
void ProfileWatchTick() {
    HWND fg = GetForegroundWindow();
    if (g_profileOpen && fg && IsStartWindow(fg)) {
        return;  // Still up.
    }
    KillTimer(g_hwnd, kProfileWatchTimer);
    g_profileOpen = false;
}
