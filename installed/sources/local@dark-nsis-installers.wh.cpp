// ==WindhawkMod==
// @id              dark-nsis-installers
// @name            Dark NSIS installers (local)
// @description     NSIS installers (electron-builder, Steam, ...) hard-code white backgrounds for the header and welcome/finish pages through their own WM_CTLCOLOR handling, so they stay white in dark mode. In NSIS processes only, white backgrounds become black (see-through under Translucent Windows) and dark text becomes white.
// @version         1.0
// @author          local
// @include         *
// @compilerOptions -luser32 -lgdi32 -lcomctl32 -ladvapi32
// ==/WindhawkMod==

#include <windhawk_utils.h>
#include <commctrl.h>

namespace {

HANDLE g_stopEvent = nullptr;
HANDLE g_thread = nullptr;

// Every NSIS stub carries the instfiles (105/106) and verify (111) dialogs.
bool IsNsisProcess() {
    HMODULE exe = GetModuleHandleW(nullptr);
    return FindResourceW(exe, MAKEINTRESOURCEW(105), RT_DIALOG) && FindResourceW(exe, MAKEINTRESOURCEW(106), RT_DIALOG) &&
           FindResourceW(exe, MAKEINTRESOURCEW(111), RT_DIALOG);
}

bool AppsUseDarkMode() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

inline int Luma(COLORREF c) { return (GetRValue(c) * 3 + GetGValue(c) * 6 + GetBValue(c)) / 10; }

LRESULT CALLBACK DialogSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, DWORD_PTR) {
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORBTN) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        HDC hdc = reinterpret_cast<HDC>(wp);
        LOGBRUSH lb{};
        if (r && GetObjectType(reinterpret_cast<HGDIOBJ>(r)) == OBJ_BRUSH &&
            GetObjectW(reinterpret_cast<HGDIOBJ>(r), sizeof(lb), &lb) == sizeof(lb)) {
            if (lb.lbStyle == BS_SOLID && Luma(lb.lbColor) >= 0xE8) {
                // A white panel (header, welcome/finish page): make it black, which Translucent Windows shows through.
                SetBkColor(hdc, RGB(0, 0, 0));
                if (Luma(GetTextColor(hdc)) < 128) SetTextColor(hdc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
            }
            if (lb.lbStyle == BS_NULL && Luma(GetTextColor(hdc)) < 128) {
                // Transparent label that was meant to sit on one of those white panels.
                SetTextColor(hdc, RGB(255, 255, 255));
            }
        }
        return r;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool IsDialog(HWND hwnd) {
    wchar_t cls[16];
    return GetClassNameW(hwnd, cls, 16) && wcscmp(cls, L"#32770") == 0;
}

void Attach(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsDialog(hwnd)) return;
    // Setting the same subclass twice only updates its data, so repeat events are harmless.
    if (WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, DialogSubclassProc, 0)) {
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd) Attach(hwnd);
}

BOOL CALLBACK AttachChild(HWND hwnd, LPARAM) {
    Attach(hwnd);
    return TRUE;
}

BOOL CALLBACK AttachTop(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        Attach(hwnd);
        EnumChildWindows(hwnd, AttachChild, 0);
    }
    return TRUE;
}

DWORD WINAPI WatchThread(void*) {
    // Pages are child dialogs created and shown as the user moves through the installer.
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnEvent, GetCurrentProcessId(), 0,
                                         WINEVENT_OUTOFCONTEXT);
    EnumWindows(AttachTop, 0);
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    if (hook) UnhookWinEvent(hook);
    return 0;
}

BOOL CALLBACK DetachChild(HWND hwnd, LPARAM) {
    if (IsDialog(hwnd)) WindhawkUtils::RemoveWindowSubclassFromAnyThread(hwnd, DialogSubclassProc);
    return TRUE;
}

BOOL CALLBACK DetachTop(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        DetachChild(hwnd, 0);
        EnumChildWindows(hwnd, DetachChild, 0);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    return TRUE;
}

}  // namespace

BOOL Wh_ModInit() {
    if (!IsNsisProcess() || !AppsUseDarkMode()) return FALSE;  // unload everywhere else
    Wh_Log(L"NSIS process, dark mode: watching dialogs");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
    }
    EnumWindows(DetachTop, 0);
    if (g_stopEvent) CloseHandle(g_stopEvent);
}
