// ==WindhawkMod==
// @id              start-menu-pop
// @name            Start menu pop
// @description     Pops Start out of the dock, and eases the search page into place
// @version         2.5
// @author          local
// @include         StartMenuExperienceHost.exe
// @include         SearchHost.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lshcore -lwindowsapp -luser32
// ==/WindhawkMod==

// Event driven: after setup it costs nothing until Start or search opens.
// The animation itself runs on the compositor, not on this thread.

#include <windows.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.Peers.h>
#include <winrt/Windows.UI.Xaml.Automation.Provider.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <chrono>
#include <cwchar>

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr float kFromScale = 0.25f;
constexpr float kDockDistance = 90.0f;  // Scale origin below the frame: the dock.
constexpr auto kOpenTime = std::chrono::milliseconds(260);
constexpr auto kFadeTime = std::chrono::milliseconds(110);
constexpr auto kCloseTime = std::chrono::milliseconds(120);

constexpr float kSearchFromScale = 0.97f;
constexpr auto kSearchTime = std::chrono::milliseconds(200);
constexpr auto kSearchFadeTime = std::chrono::milliseconds(140);

HANDLE g_setupThread = nullptr;
HANDLE g_stopEvent = nullptr;
HWND g_coreWindow = nullptr;
UINT g_runMessage = 0;
bool g_isSearch = false;

winrt::weak_ref<FrameworkElement> g_target;
winrt::event_token g_visibilityToken{};
winrt::event_token g_activatedToken{};
winrt::event_token g_pointerToken{};
bool g_hooked = false;
bool g_isOpen = false;

FrameworkElement FindNamed(FrameworkElement const& root, PCWSTR name) {
    if (!root) {
        return nullptr;
    }
    if (root.Name() == name) {
        return root;
    }
    int count = Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; i++) {
        auto child = Media::VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();
        if (auto found = FindNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

FrameworkElement GetTarget() {
    if (auto cached = g_target.get()) {
        return cached;
    }
    auto window = Window::Current();
    if (!window) {
        return nullptr;
    }
    auto found = FindNamed(window.Content().try_as<FrameworkElement>(),
                           g_isSearch ? L"OuterBorderGrid" : L"FrameRoot");
    if (found) {
        g_target = winrt::make_weak(found);
    }
    return found;
}

void PlayStart(FrameworkElement const& frame, bool opening) {
    auto visual = Hosting::ElementCompositionPreview::GetElementVisual(frame);
    auto compositor = visual.Compositor();
    visual.CenterPoint({(float)frame.ActualWidth() / 2.0f,
                        (float)frame.ActualHeight() + kDockDistance, 0.0f});

    auto scale = compositor.CreateVector3KeyFrameAnimation();
    auto fade = compositor.CreateScalarKeyFrameAnimation();
    const float3 small{kFromScale, kFromScale, 1.0f};
    const float3 full{1.0f, 1.0f, 1.0f};

    if (opening) {
        auto overshoot = compositor.CreateCubicBezierEasingFunction(
            float2{0.2f, 1.25f}, float2{0.4f, 1.0f});
        scale.InsertKeyFrame(0.0f, small);
        scale.InsertKeyFrame(1.0f, full, overshoot);
        scale.Duration(kOpenTime);
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f);
        fade.Duration(kFadeTime);
    } else {
        auto accelerate = compositor.CreateCubicBezierEasingFunction(
            float2{0.5f, 0.0f}, float2{0.9f, 0.4f});
        scale.InsertKeyFrame(0.0f, full);
        scale.InsertKeyFrame(1.0f, small, accelerate);
        scale.Duration(kCloseTime);
        fade.InsertKeyFrame(0.0f, 1.0f);
        fade.InsertKeyFrame(1.0f, 0.0f);
        fade.Duration(kCloseTime);
    }
    visual.StartAnimation(L"Scale", scale);
    visual.StartAnimation(L"Opacity", fade);
}

void PlaySearch(FrameworkElement const& page) {
    auto visual = Hosting::ElementCompositionPreview::GetElementVisual(page);
    auto compositor = visual.Compositor();
    visual.CenterPoint({(float)page.ActualWidth() / 2.0f, 0.0f, 0.0f});

    auto settle = compositor.CreateCubicBezierEasingFunction(
        float2{0.1f, 0.9f}, float2{0.2f, 1.0f});
    auto scale = compositor.CreateVector3KeyFrameAnimation();
    scale.InsertKeyFrame(0.0f, float3{kSearchFromScale, kSearchFromScale, 1.0f});
    scale.InsertKeyFrame(1.0f, float3{1.0f, 1.0f, 1.0f}, settle);
    scale.Duration(kSearchTime);
    auto fade = compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    fade.InsertKeyFrame(1.0f, 1.0f);
    fade.Duration(kSearchFadeTime);
    visual.StartAnimation(L"Scale", scale);
    visual.StartAnimation(L"Opacity", fade);
}

// The top bar's profile button. Start's profile flyout belongs to its user button and only
// exists with Start open, so Start opens with its frame hidden and moved so that button sits
// under the bar's avatar, and the button is pressed. Only the flyout is seen. When it closes,
// the bar closes Start.
constexpr UINT WM_PROFILE_DONE = WM_APP + 5;  // To the bar. wParam 1: Start is still open.
constexpr ULONGLONG kProfileArmMs = 2000;
constexpr float kProfileTopGap = 8.0f;
constexpr float kProfileEdgeGap = 8.0f;

UINT g_profileMessage = 0;
HHOOK g_profileHook = nullptr;
bool g_profileArmed = false;
ULONGLONG g_profileArmTick = 0;
bool g_profileMode = false;
bool g_profileClosing = false;
bool g_profileSeen = false;
ULONGLONG g_profileStart = 0;
DispatcherTimer g_profileTimer{nullptr};
int g_profilePlaceTicks = 0;
int g_profileStableTicks = 0;
float g_profileLastX = 0.0f;
float g_profileLastY = 0.0f;
bool g_profileSettled = false;  // Where the user button came to rest the last time, to skip the wait.
float g_profileSettledX = 0.0f;
float g_profileSettledY = 0.0f;


// The pop animation owns FrameRoot's composition visual, which stops XAML changes to that
// element from showing, so hiding and moving are done on the menu inside it.
FrameworkElement GetProfileTarget() {
    auto window = Window::Current();
    return window ? FindNamed(window.Content().try_as<FrameworkElement>(), L"MainMenu") : nullptr;
}

void StopProfileTimer() {
    if (g_profileTimer) {
        g_profileTimer.Stop();
        g_profileTimer = nullptr;
    }
}

void AskBarToClose() {
    StopProfileTimer();
    if (g_profileClosing) {
        return;
    }
    g_profileClosing = true;
    if (HWND bar = FindWindowW(L"WindhawkTopBar", nullptr)) {
        PostMessageW(bar, WM_PROFILE_DONE, 1, 0);
    }
}

// A flyout left open when Start hides would come back the next time Start opens.
void CloseOpenPopups() {
    if (auto window = Window::Current()) {
        for (auto const& popup : Media::VisualTreeHelper::GetOpenPopups(window)) {
            if (auto child = popup.Child().try_as<FrameworkElement>()) {
                child.Opacity(1.0);  // Windows may reuse it for Start's own user button.
            }
            popup.VerticalOffset(0);
            popup.HorizontalOffset(0);
            popup.IsOpen(false);
        }
    }
}

// Windows decides where the flyout goes from the user button and the space around it, and that
// varies between opens. So measure where it really ended up and move it from there to right
// under the bar, which is the top of this window. It stays invisible until it's there, so the
// move is never seen. Returns once it has been laid out.
bool PlaceProfileFlyout(Window const& window) {
    auto popups = Media::VisualTreeHelper::GetOpenPopups(window);
    if (popups.Size() == 0) {
        return false;
    }
    auto popup = popups.GetAt(0);
    auto child = popup.Child().try_as<FrameworkElement>();
    if (!child) {
        return false;
    }
    if (child.ActualHeight() <= 0) {
        child.Opacity(0.0);
        return false;
    }
    // The popup itself: moving the content inside it gets clipped by the popup's own window.
    const auto at = child.TransformToVisual(nullptr).TransformPoint({0.0f, 0.0f});
    const float dy = kProfileTopGap - at.Y;
    const float dx = kProfileEdgeGap - at.X;
    const bool inPlace = dy > -1.0f && dy < 1.0f && dx > -1.0f && dx < 1.0f;
    child.Opacity(inPlace ? 1.0 : 0.0);
    if (!inPlace) {
        popup.VerticalOffset(popup.VerticalOffset() + dy);
        popup.HorizontalOffset(popup.HorizontalOffset() + dx);
    }
    return true;
}

// Only after Start has gone, so the frame never flashes back.
void RestoreAfterProfile() {
    StopProfileTimer();
    CloseOpenPopups();
    if (auto target = GetProfileTarget()) {
        target.Opacity(1.0);
        target.RenderTransform(nullptr);
        target.IsHitTestVisible(true);
    }
    g_profileMode = false;
    g_profileClosing = false;
    if (HWND bar = FindWindowW(L"WindhawkTopBar", nullptr)) {
        PostMessageW(bar, WM_PROFILE_DONE, 0, 0);
    }
}

void PressUserButton() {
    auto window = Window::Current();
    auto frame = GetProfileTarget();
    if (!window || !frame) {
        AskBarToClose();
        return;
    }
    auto button = FindNamed(window.Content().try_as<FrameworkElement>(), L"UserTileButton");
    if (!button) {
        AskBarToClose();
        return;
    }
    // In this window's units. The window starts at the bottom of the bar.
    const float s = GetDpiForWindow(g_coreWindow) / 96.0f;
    RECT r{};
    GetWindowRect(g_coreWindow, &r);
    HWND bar = FindWindowW(L"WindhawkTopBar", nullptr);
    const int anchor = bar ? (int)(INT_PTR)GetPropW(bar, L"TopBarAnchorX") : r.left;
    const auto at = button.TransformToVisual(nullptr).TransformPoint({0.0f, 0.0f});
    Media::TranslateTransform move;
    move.X((anchor - r.left) / s - at.X - button.ActualWidth() / 2.0);
    move.Y(kProfileTopGap - at.Y);
    frame.RenderTransform(move);
    frame.UpdateLayout();

    auto peer = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(button);
    peer.GetPattern(Automation::Peers::PatternInterface::Invoke)
        .as<Automation::Provider::IInvokeProvider>()
        .Invoke();

    // Runs only while the flyout is up, to see it close.
    g_profileSeen = false;
    g_profilePlaceTicks = 30;
    g_profileStart = GetTickCount64();
    g_profileTimer = DispatcherTimer();
    g_profileTimer.Interval(std::chrono::milliseconds(16));  // To place it as soon as it appears.
    g_profileTimer.Tick([](auto const&, auto const&) {
        try {
            auto current = Window::Current();
            const bool open = current && Media::VisualTreeHelper::GetOpenPopups(current).Size() > 0;
            if (open) {
                g_profileSeen = true;
                // Keep correcting for a moment, in case Windows moves it again as it settles.
                if (g_profilePlaceTicks > 0) {
                    PlaceProfileFlyout(current);
                    if (--g_profilePlaceTicks == 0) {
                        g_profileTimer.Interval(std::chrono::milliseconds(100));
                    }
                }
            } else if (g_profileSeen || GetTickCount64() - g_profileStart > 1500) {
                AskBarToClose();
            }
        } catch (...) {
            AskBarToClose();
        }
    });
    g_profileTimer.Start();
}

void BeginProfile() {
    g_profileMode = true;
    g_profileClosing = false;
    auto frame = GetProfileTarget();
    auto window = Window::Current();
    if (!frame || !window) {
        AskBarToClose();
        return;
    }
    frame.Opacity(0.0);
    frame.IsHitTestVisible(false);

    // On Start's first open its layout isn't ready yet, and a flyout opened from a button that
    // hasn't settled lands in the wrong place. So wait until the button stops moving.
    g_profileStart = GetTickCount64();
    g_profileStableTicks = 0;
    g_profileLastX = g_profileLastY = -100000.0f;
    StopProfileTimer();
    g_profileTimer = DispatcherTimer();
    g_profileTimer.Interval(std::chrono::milliseconds(16));
    g_profileTimer.Tick([](auto const&, auto const&) {
        try {
            auto current = Window::Current();
            auto button = current ? FindNamed(current.Content().try_as<FrameworkElement>(), L"UserTileButton") : nullptr;
            if (button && button.ActualHeight() > 0) {
                const auto at = button.TransformToVisual(nullptr).TransformPoint({0.0f, 0.0f});
                const float dx = at.X - g_profileLastX, dy = at.Y - g_profileLastY;
                g_profileStableTicks = (dx > -0.5f && dx < 0.5f && dy > -0.5f && dy < 0.5f) ? g_profileStableTicks + 1 : 0;
                g_profileLastX = at.X;
                g_profileLastY = at.Y;
                // A warm Start lays the button out where it was last time, already settled: press
                // at once instead of watching it for three more frames. Anywhere else (the first
                // open, another scale or layout), the wait below still applies.
                const float sx = at.X - g_profileSettledX, sy = at.Y - g_profileSettledY;
                if (g_profileSettled && sx > -0.5f && sx < 0.5f && sy > -0.5f && sy < 0.5f) {
                    g_profileStableTicks = 3;
                }
            }
            if (g_profileStableTicks >= 3) {
                g_profileSettled = true;
                g_profileSettledX = g_profileLastX;
                g_profileSettledY = g_profileLastY;
                StopProfileTimer();
                PressUserButton();
            } else if (GetTickCount64() - g_profileStart > 2500) {
                AskBarToClose();
            }
        } catch (...) {
            AskBarToClose();
        }
    });
    g_profileTimer.Start();
}

void OnStateChange(bool nowOpen) {
    try {
        if (g_profileMode) {
            return;  // Activation changes during the flyout aren't Start opening or closing.
        }
        if (nowOpen == g_isOpen) {
            return;
        }
        g_isOpen = nowOpen;
        if (nowOpen && g_profileArmed) {
            g_profileArmed = false;
            if (GetTickCount64() - g_profileArmTick < kProfileArmMs) {
                BeginProfile();
                return;
            }
        }
        auto target = GetTarget();
        if (!target) {
            return;
        }
        if (g_isSearch) {
            if (nowOpen) {
                PlaySearch(target);
            }
        } else {
            PlayStart(target, nowOpen);
        }
    } catch (...) {
    }
}

// Search only loses focus when the user clicks elsewhere, staying open behind them. Escape is
// what closes it, so send that, once focus has settled somewhere other than Search or Start:
// the two work as a pair, and clicking inside Start also takes focus from Search.
DispatcherTimer g_searchCloseTimer{nullptr};

bool ForegroundIsStartOrSearch() {
    HWND fg = GetForegroundWindow();
    if (!fg) {
        return true;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) {
        return true;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool match = QueryFullProcessImageNameW(proc, 0, path, &size) &&
                       (wcsstr(path, L"\\StartMenuExperienceHost.exe") || wcsstr(path, L"\\SearchHost.exe"));
    CloseHandle(proc);
    return match;
}

void CloseSearchIfFocusLeft() {
    if (g_searchCloseTimer) {
        g_searchCloseTimer.Stop();
    }
    g_searchCloseTimer = DispatcherTimer();
    g_searchCloseTimer.Interval(std::chrono::milliseconds(80));
    g_searchCloseTimer.Tick([](auto const&, auto const&) {
        try {
            g_searchCloseTimer.Stop();
            g_searchCloseTimer = nullptr;
            if (!ForegroundIsStartOrSearch() && g_coreWindow) {
                PostMessageW(g_coreWindow, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
                PostMessageW(g_coreWindow, WM_KEYUP, VK_ESCAPE, 0xC0010001);
            }
        } catch (...) {
        }
    });
    g_searchCloseTimer.Start();
}

void HookOnUiThread() {
    if (g_hooked) {
        return;
    }
    auto window = Window::Current();
    if (!window) {
        return;
    }
    g_visibilityToken = window.VisibilityChanged(
        [](winrt::Windows::Foundation::IInspectable const&,
           winrt::Windows::UI::Core::VisibilityChangedEventArgs const& args) {
            if (!args.Visible() && g_profileMode) {
                try {
                    RestoreAfterProfile();
                } catch (...) {
                }
                g_isOpen = false;
                return;
            }
            OnStateChange(args.Visible());
        });
    if (g_isSearch) {
        // Search's window is much taller than its panel, and its invisible part takes clicks, so
        // focus stays when the user clicks there. A press outside the panel closes it instead.
        // Only presses nothing in the panel handled get here, so the panel itself is unaffected.
        g_pointerToken = window.CoreWindow().PointerPressed(
            [](winrt::Windows::UI::Core::CoreWindow const&, winrt::Windows::UI::Core::PointerEventArgs const& args) {
                try {
                    auto panel = GetTarget();
                    if (!panel || !g_coreWindow) {
                        return;
                    }
                    const auto at = panel.TransformToVisual(nullptr).TransformPoint({0.0f, 0.0f});
                    const auto p = args.CurrentPoint().Position();
                    if (p.X < at.X || p.Y < at.Y || p.X > at.X + panel.ActualWidth() ||
                        p.Y > at.Y + panel.ActualHeight()) {
                        PostMessageW(g_coreWindow, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
                        PostMessageW(g_coreWindow, WM_KEYUP, VK_ESCAPE, 0xC0010001);
                    }
                } catch (...) {
                }
            });
    }
    g_activatedToken = window.Activated(
        [](winrt::Windows::Foundation::IInspectable const&,
           winrt::Windows::UI::Core::WindowActivatedEventArgs const& args) {
            const bool active = args.WindowActivationState() !=
                                winrt::Windows::UI::Core::CoreWindowActivationState::Deactivated;
            if (!active && g_isSearch) {
                CloseSearchIfFocusLeft();
            }
            OnStateChange(active);
        });
    g_hooked = true;
}

void UnhookOnUiThread() {
    if (!g_hooked) {
        return;
    }
    auto window = Window::Current();
    if (window) {
        window.VisibilityChanged(g_visibilityToken);
        window.Activated(g_activatedToken);
        if (g_isSearch) {
            window.CoreWindow().PointerPressed(g_pointerToken);
        }
    }
    if (auto target = g_target.get()) {
        auto visual = Hosting::ElementCompositionPreview::GetElementVisual(target);
        visual.StopAnimation(L"Scale");
        visual.StopAnimation(L"Opacity");
        visual.Scale({1.0f, 1.0f, 1.0f});
        visual.Opacity(1.0f);
    }
    g_target = nullptr;
    g_hooked = false;
}

LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto msg = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (msg->message == g_profileMessage && msg->hwnd == g_coreWindow) {
            g_profileArmed = true;
            g_profileArmTick = GetTickCount64();
        } else if (msg->message == g_runMessage && msg->hwnd == g_coreWindow) {
            try {
                if (msg->wParam == 1) {
                    HookOnUiThread();
                } else {
                    UnhookOnUiThread();
                }
            } catch (...) {
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void RunOnUiThread(WPARAM action) {
    if (!g_coreWindow) {
        return;
    }
    DWORD thread = GetWindowThreadProcessId(g_coreWindow, nullptr);
    if (!thread) {
        return;
    }
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, nullptr, thread);
    if (!hook) {
        return;
    }
    DWORD_PTR result = 0;
    SendMessageTimeoutW(g_coreWindow, g_runMessage, action, 0, SMTO_BLOCK, 3000, &result);
    UnhookWindowsHookEx(hook);
}

HWND FindOwnCoreWindow() {
    const DWORD self = GetCurrentProcessId();
    HWND candidate = nullptr;
    while ((candidate = FindWindowExW(nullptr, candidate, L"Windows.UI.Core.CoreWindow", nullptr))) {
        DWORD pid = 0;
        GetWindowThreadProcessId(candidate, &pid);
        if (pid == self) {
            return candidate;
        }
    }
    return nullptr;
}

DWORD WINAPI SetupThread(void*) {
    // The app may still be launching: wait for its window, then this thread ends.
    for (int i = 0; i < 240; i++) {
        if (WaitForSingleObject(g_stopEvent, 500) == WAIT_OBJECT_0) {
            return 0;
        }
        g_coreWindow = FindOwnCoreWindow();
        if (g_coreWindow) {
            RunOnUiThread(1);
            if (!g_isSearch) {
                // Stays for the top bar's profile button, and only looks at this one message.
                ChangeWindowMessageFilterEx(g_coreWindow, g_profileMessage, MSGFLT_ALLOW, nullptr);
                g_profileHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, nullptr,
                                                  GetWindowThreadProcessId(g_coreWindow, nullptr));
                // A hook dies with the thread that set it, so this one stays, asleep, until unload.
                WaitForSingleObject(g_stopEvent, INFINITE);
                if (g_profileHook) {
                    UnhookWindowsHookEx(g_profileHook);
                    g_profileHook = nullptr;
                }
            }
            return 0;
        }
    }
    return 0;
}

}  // namespace

BOOL Wh_ModInit() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_isSearch = wcsstr(path, L"SearchHost.exe") != nullptr;
    g_runMessage = RegisterWindowMessageW(L"Windhawk_StartMenuPop_Run");
    g_profileMessage = RegisterWindowMessageW(L"Windhawk_TopBar_Profile");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_setupThread = CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_setupThread) {
        WaitForSingleObject(g_setupThread, 5000);
        CloseHandle(g_setupThread);
        g_setupThread = nullptr;
    }
    RunOnUiThread(0);
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}
