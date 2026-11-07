$s = 'C:\Users\shiva\AppData\Local\Temp\claude\dd6418d2605d4fa5\ae7d919f-98d3-467e-8ca0-aae07bf2d6fb\scratchpad'
$src = 'C:\ProgramData\Windhawk\ModsSource\taskbar-dock-animation.wh.cpp'
$id = 'local@taskbar-dock-seelen-wave'
$ver = '1.9.2-seelenwave'
$utf8 = New-Object Text.UTF8Encoding($false)
$t = [IO.File]::ReadAllText($src, $utf8)
$nl = if ($t.Contains("`r`n")) { "`r`n" } else { "`n" }

function Replace-Once([string]$text, [string]$old, [string]$new) {
  $i = $text.IndexOf($old)
  if ($i -lt 0) { throw "not found: $old" }
  return $text.Remove($i, $old.Length).Insert($i, $new)
}

# 1. Helpers: Seelen sizing and the growing shelf
$helpers = @'
// Seelen dock wave: item 50, zoom 80, steps 1 / 0.7 / 0.3 of the range.
static constexpr double kItemSize = 50.0;
static constexpr double kItemRange = 30.0;
static winrt::weak_ref<FrameworkElement> g_shelfRef;

static FrameworkElement FindDescendantByClass(FrameworkElement const& root, PCWSTR className) {
    if (!root) return nullptr;
    int c = VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < c; i++) {
        auto ch = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();
        if (!ch) continue;
        if (winrt::get_class_name(ch) == className) return ch;
        auto deep = FindDescendantByClass(ch, className);
        if (deep) return deep;
    }
    return nullptr;
}

static FrameworkElement GetShelf(FrameworkElement const& frame) {
    auto cached = g_shelfRef.get();
    if (cached) return cached;
    auto bg = FindDescendantByClass(frame, L"Taskbar.TaskbarBackground");
    if (!bg || VisualTreeHelper::GetChildrenCount(bg) == 0) return nullptr;
    auto grid = VisualTreeHelper::GetChild(bg, 0).try_as<FrameworkElement>();
    if (grid) g_shelfRef = winrt::make_weak(grid);
    return grid;
}

static void SetShelfGrowth(FrameworkElement const& shelf, double width, double height) {
    if (!shelf) return;
    Thickness m{};
    m.Left = -width / 2.0;
    m.Top = -height;
    m.Right = -width / 2.0;
    m.Bottom = 0.0;
    shelf.Margin(m);
}


'@ -replace "`r?`n", $nl
$t = Replace-Once $t 'void ApplyAnimation(double mouseX, DockAnimationContext& ctx, double intensity, double dtSec) {' ($helpers + 'void ApplyAnimation(double mouseX, DockAnimationContext& ctx, double intensity, double dtSec) {')

# 2. Hover tracking follows where the icon is actually drawn
$t = Replace-Once $t 'distance = std::abs(mouseX - ctx.icons[i].originalCenterX);' 'distance = std::abs(mouseX - (ctx.icons[i].originalCenterX + ctx.icons[i].state.waveTranslateX));'

# 3. Stepped wave replaces the distance curve
$step = @'
    totalExpansion = 0.0;
    for (size_t i = 0; i < ctx.icons.size(); i++) {
        double m = 0.0;
        if (closestIconIndex != (size_t)-1) {
            size_t d = i > closestIconIndex ? i - closestIconIndex : closestIconIndex - i;
            m = d == 0 ? 1.0 : d == 1 ? 0.7 : d == 2 ? 0.3 : 0.0;
        }
        double size = kItemSize + kItemRange * m;
        scales[i] = size / kItemSize;
        extraWidths[i] = size - kItemSize;
        totalExpansion += extraWidths[i];
    }


'@ -replace "`r?`n", $nl
$t = Replace-Once $t '    double bounceScaleFactor = 1.0;' ($step + '    double bounceScaleFactor = 1.0;')

# 4. Shelf follows the animated sizes
$anchor = 'void ResetAllIconScales(std::vector<TaskbarIconInfo>& icons) {'
$a = $t.IndexOf($anchor); if ($a -lt 0) { throw 'reset anchor missing' }
$close = $t.LastIndexOf('}', $a); if ($close -lt 0) { throw 'close brace missing' }
$grow = @'
    double shelfWidth = 0.0, shelfHeight = 0.0;
    for (auto const& icon : ctx.icons) {
        double extra = (icon.state.waveScale - 1.0) * kItemSize;
        if (extra < 0.0) extra = 0.0;
        shelfWidth += extra;
        if (extra > shelfHeight) shelfHeight = extra;
    }
    try {
        SetShelfGrowth(GetShelf(taskbarFrame), shelfWidth, 0.0);
    } catch (...) {
    }

'@ -replace "`r?`n", $nl
$t = $t.Insert($close, $grow)

# 5. Reset returns the shelf to rest
$resetOld = 'void ResetAllIconScales(std::vector<TaskbarIconInfo>& icons) {' + $nl + '    if (icons.empty()) return;'
$resetNew = 'void ResetAllIconScales(std::vector<TaskbarIconInfo>& icons) {' + $nl + '    try { SetShelfGrowth(g_shelfRef.get(), 0.0, 0.0); } catch (...) {}' + $nl + '    if (icons.empty()) return;'
$t = Replace-Once $t $resetOld $resetNew

# --- Smoothness and launch bounce ---
$t = Replace-Once $t 'struct IconAnimState {' ('struct IconAnimState {' + $nl + '    double waveVel = 0.0;' + $nl + '    double appliedScale = 1.0;' + $nl + '    double appliedX = 0.0;' + $nl + '    double appliedPopY = 0.0;')

# Unused per-frame work from the old distance curve
$t = Replace-Once $t 'scales[i] = CalculateScale(distance, (double)g_settings.effectRadius, g_settings.maxScale);' 'scales[i] = 1.0;'
$t = Replace-Once $t 'extraWidths[i] = (scales[i] - 1.0) * element.ActualWidth() * spacingFactor;' 'extraWidths[i] = 0.0;'
$t = Replace-Once $t 'if (sinceCheck > 120) {' 'if (sinceCheck > 300) {'

# Sleep: stop the frame loop once everything has settled
$t = Replace-Once $t '        ApplyAnimation(mouseX, ctx, currentIntensity, dtSec);' ('        ApplyAnimation(mouseX, ctx, currentIntensity, dtSec);' + $nl + '        if (g_dockSettled && g_isMouseInside && currentIntensity >= 1.0) {' + $nl + '            if (g_isRenderingHooked.exchange(false)) {' + $nl + '                Media::CompositionTarget::Rendering(g_renderingToken);' + $nl + '            }' + $nl + '        }')
$t = Replace-Once $t ('            g_isMouseInside = false;' + $nl + '            g_isBouncing = false;') ('            g_isMouseInside = false;' + $nl + '            g_isBouncing = false;' + $nl + '            WakeRenderLoop();')

# Shelf: skip layout when nothing changed
$t = Replace-Once $t '    if (!shelf) return;' ('    if (!shelf) return;' + $nl + '    static double s_w = -1.0, s_h = -1.0;' + $nl + '    if (std::abs(width - s_w) < 0.1 && std::abs(height - s_h) < 0.1) return;' + $nl + '    s_w = width; s_h = height;')

# Continuous wave: same sizes on an icon, blended between icons
$oldStepStart = $t.IndexOf('    totalExpansion = 0.0;')
$oldStepEnd = $t.IndexOf('    double bounceScaleFactor = 1.0;')
if ($oldStepStart -lt 0 -or $oldStepEnd -lt 0) { throw 'wave block missing' }
$wave = @'
    double hoverPos = 0.0;
    {
        const size_t n = ctx.icons.size();
        auto visual = [&](size_t k) { return ctx.icons[k].originalCenterX + ctx.icons[k].state.waveTranslateX; };
        if (n > 0) {
            if (mouseX <= visual(0)) {
                hoverPos = 0.0;
            } else if (mouseX >= visual(n - 1)) {
                hoverPos = (double)(n - 1);
            } else {
                for (size_t k = 0; k + 1 < n; k++) {
                    double a = visual(k), b = visual(k + 1);
                    if (mouseX >= a && mouseX <= b) {
                        hoverPos = (double)k + (b > a ? (mouseX - a) / (b - a) : 0.0);
                        break;
                    }
                }
            }
        }
    }
    // Fade the wave out as the pointer moves off either end of the dock.
    double offDock = 0.0;
    if (!ctx.icons.empty()) {
        double first = ctx.icons.front().originalCenterX + ctx.icons.front().state.waveTranslateX;
        double last = ctx.icons.back().originalCenterX + ctx.icons.back().state.waveTranslateX;
        if (mouseX < first) offDock = first - mouseX;
        else if (mouseX > last) offDock = mouseX - last;
    }
    const double dockFade = std::clamp(1.0 - std::max(0.0, offDock - 40.0) / 40.0, 0.0, 1.0);
    totalExpansion = 0.0;
    for (size_t i = 0; i < ctx.icons.size(); i++) {
        double d = std::abs((double)i - hoverPos);
        double m = d < 1.0 ? 1.0 - 0.3 * d
                 : d < 2.0 ? 0.7 - 0.4 * (d - 1.0)
                 : d < 3.0 ? 0.3 - 0.3 * (d - 2.0)
                 : 0.0;
        m *= dockFade;
        double size = kItemSize + kItemRange * m;
        scales[i] = size / kItemSize;
        extraWidths[i] = size - kItemSize;
        totalExpansion += extraWidths[i];
    }


'@ -replace "`r?`n", $nl
$t = $t.Remove($oldStepStart, $oldStepEnd - $oldStepStart).Insert($oldStepStart, $wave)

# Spring-driven sizes, positions derived from the animated sizes
$i1 = $t.IndexOf('    double cumulativeShift = 0.0;')
$i2 = $t.IndexOf('    double shelfWidth = 0.0, shelfHeight = 0.0;')
if ($i1 -lt 0 -or $i2 -lt 0 -or $i2 -lt $i1) { throw 'transform block missing' }
$spring = @'
    const double omega = 26.0;
    const double frameStep = std::clamp(dtSec, 0.0, 1.0 / 30.0);
    double drawnTotal = 0.0;
    bool settled = true;
    for (size_t i = 0; i < ctx.icons.size(); i++) {
        auto& st = ctx.icons[i].state;
        double target = 1.0 + (scales[i] - 1.0) * intensity;
        double x = st.waveScale, v = st.waveVel;
        for (int sub = 0; sub < 2; sub++) {
            double h = frameStep / 2.0;
            v += (omega * omega * (target - x) - 2.0 * omega * v) * h;
            x += v * h;
        }
        if (std::abs(target - x) < 0.0005 && std::abs(v) < 0.01) {
            x = target;
            v = 0.0;
        } else {
            settled = false;
        }
        st.waveScale = x;
        st.waveVel = v;
        drawnTotal += std::max(0.0, x - 1.0) * kItemSize;
    }

    double popY = 0.0;
    FrameworkElement popTarget{nullptr};
    if (g_popActive) {
        const double hopSec = g_popHops > 1 ? 0.36 : 0.26;
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_popStart).count();
        if (t >= hopSec * g_popHops) {
            g_popActive = false;
        } else {
            popTarget = g_popElement.get();
            int hop = (int)(t / hopSec);
            double phase = (t - hop * hopSec) / hopSec;
            popY = -g_popHeight * std::pow(0.58, hop) * std::sin(phase * 3.14159265);
        }
    }

    double drawnSoFar = 0.0;
    for (size_t i = 0; i < ctx.icons.size(); i++) {
        auto& st = ctx.icons[i].state;
        double extra = std::max(0.0, st.waveScale - 1.0) * kItemSize;
        st.waveTranslateX = drawnSoFar + extra / 2.0 - drawnTotal / 2.0;
        drawnSoFar += extra;

        auto element = ctx.icons[i].element.get();
        if (!element) continue;
        bool isPop = popTarget && winrt::get_abi(popTarget) == winrt::get_abi(element);
        double wantPopY = isPop ? popY : 0.0;
        if (std::abs(st.waveScale - st.appliedScale) < 0.0005 &&
            std::abs(st.waveTranslateX - st.appliedX) < 0.05 &&
            std::abs(wantPopY - st.appliedPopY) < 0.05) {
            continue;
        }

        auto tg = element.RenderTransform().try_as<TransformGroup>();
        if (!tg || tg.Children().Size() < 4) continue;
        auto waveScale = tg.Children().GetAt(0).try_as<ScaleTransform>();
        auto waveTranslate = tg.Children().GetAt(1).try_as<TranslateTransform>();
        auto bounceTranslate = tg.Children().GetAt(3).try_as<TranslateTransform>();
        if (!waveScale || !waveTranslate || !bounceTranslate) continue;

        waveScale.ScaleX(st.waveScale);
        waveScale.ScaleY(st.waveScale);
        waveTranslate.X(st.waveTranslateX);
        bounceTranslate.Y(wantPopY);
        st.appliedScale = st.waveScale;
        st.appliedX = st.waveTranslateX;
        st.appliedPopY = wantPopY;
    }
    g_dockSettled = settled && !g_popActive;


'@ -replace "`r?`n", $nl
$t = $t.Remove($i1, $i2 - $i1).Insert($i1, $spring)

# Pop state and helpers, placed before the shelf helpers
$popDecl = @'
static bool g_dockSettled = false;

// The frame hook forces redraws, so it only runs while something is moving.
static void WakeRenderLoop() {
    g_dockSettled = false;
    if (!g_isRenderingHooked.exchange(true)) {
        g_lastRenderTime = std::chrono::steady_clock::now();
        g_renderingToken = Media::CompositionTarget::Rendering(OnCompositionTargetRendering);
    }
}

static winrt::weak_ref<FrameworkElement> g_popElement;
static std::chrono::steady_clock::time_point g_popStart;
static bool g_popActive = false;
static int g_popHops = 3;
static double g_popHeight = 26.0;

static FrameworkElement FindNamedDescendant(FrameworkElement const& root, PCWSTR name) {
    if (!root) return nullptr;
    int c = VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < c; i++) {
        auto ch = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();
        if (!ch) continue;
        if (ch.Name() == name) return ch;
        auto deep = FindNamedDescendant(ch, name);
        if (deep) return deep;
    }
    return nullptr;
}

static bool IsAppRunning(FrameworkElement const& button) {
    auto dot = FindNamedDescendant(button, L"RunningIndicator");
    return dot && dot.Visibility() == Visibility::Visible && dot.Opacity() > 0.05;
}

// Bounce on left click, using the routed event so handled clicks still count.
static std::vector<std::pair<winrt::weak_ref<FrameworkElement>, void*>> g_popHooked;
static winrt::Windows::UI::Xaml::Input::PointerEventHandler g_popHandler{nullptr};

static void OnDockPointerReleased(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
    try {
        auto el = sender.try_as<FrameworkElement>();
        if (!el) return;
        auto kind = args.GetCurrentPoint(el).Properties().PointerUpdateKind();
        if (kind != winrt::Windows::UI::Input::PointerUpdateKind::LeftButtonReleased) return;
        bool running = IsAppRunning(el);
        g_popElement = winrt::make_weak(el);
        g_popHops = running ? 1 : 3;
        g_popHeight = running ? 12.0 : 26.0;
        g_popStart = std::chrono::steady_clock::now();
        g_popActive = true;
        WakeRenderLoop();
    } catch (...) {
    }
}

static void HookPopClick(FrameworkElement const& el) {
    void* key = winrt::get_abi(el);
    for (auto const& h : g_popHooked) {
        if (h.second == key && h.first.get()) return;
    }
    if (!g_popHandler) g_popHandler = winrt::Windows::UI::Xaml::Input::PointerEventHandler(&OnDockPointerReleased);
    el.AddHandler(UIElement::PointerReleasedEvent(), winrt::box_value(g_popHandler), true);
    g_popHooked.emplace_back(winrt::make_weak(el), key);
}

static void UnhookPopClicks() {
    for (auto const& h : g_popHooked) {
        auto el = h.first.get();
        if (el && g_popHandler) {
            try { el.RemoveHandler(UIElement::PointerReleasedEvent(), winrt::box_value(g_popHandler)); } catch (...) {}
        }
    }
    g_popHooked.clear();
    g_popHandler = nullptr;
}


'@ -replace "`r?`n", $nl
$t = Replace-Once $t '// Seelen dock wave: item 50' ($popDecl + '// Seelen dock wave: item 50')

# Measure the true center: transforms must not feed back into geometry
$t = Replace-Once $t ('auto pt = t.TransformPoint({0, 0});' + $nl + '            double w = e.ActualWidth();') ('double w = e.ActualWidth();' + $nl + '            auto pt = t.TransformPoint({(float)(w / 2.0), 0});')
$t = Replace-Once $t 'double newCenterX = pt.X + (w / 2.0);' 'double newCenterX = pt.X - info.state.waveTranslateX;'
$t = Replace-Once $t 'info.originalCenterX = point.X + (info.elementWidth / 2.0);' 'info.originalCenterX = transform.TransformPoint({(float)(info.elementWidth / 2.0), 0}).X;'
$t = Replace-Once $t ('info.state = it->second;') ('info.state = it->second;' + $nl + '                if (!g_settings.taskbarLabelsMode) info.originalCenterX -= info.state.waveTranslateX;')

# Remove the extra animation step in the 120 ms check; use precise frame time
$t = Replace-Once $t 'ApplyAnimation(mx, ctx, currentIntensity, dtSec);' '(void)mx;'
$t = Replace-Once $t ('double deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(' + $nl + '                               now - g_lastRenderTime)' + $nl + '                               .count();') 'double deltaTime = std::chrono::duration<double, std::milli>(now - g_lastRenderTime).count();'

# Hook each dock button for clicks, and remove the hooks when the mod unloads
$t = Replace-Once $t ('            newIcons.push_back(info);') ('            HookPopClick(element);' + $nl + '            newIcons.push_back(info);')
$unhookAt = $t.LastIndexOf('if (g_isRenderingHooked.exchange(false)) {')
if ($unhookAt -lt 0) { throw 'unhook anchor missing' }
$t = $t.Insert($unhookAt, 'UnhookPopClicks();' + $nl + '        ')

# Keep the loop alive until the bounce finishes
$t = Replace-Once $t 'if (!g_isMouseInside && currentIntensity <= 0.0) {' 'if (!g_isMouseInside && currentIntensity <= 0.0 && !g_popActive) {'

$t = $t -replace '(?m)^// @id\s+taskbar-dock-animation', "// @id              $id"
$t = $t -replace '(?m)^// @version\s+1\.9\.2', "// @version           $ver"
$t = $t -replace '(?m)^// @name\s+Taskbar Dock Animation', '// @name              Taskbar Dock Animation (Seelen wave)'
[IO.File]::WriteAllText("$s\$id.wh.cpp", $t, $utf8)

$comp = 'C:\Program Files\Windhawk\Compiler'
$lib = 'C:\Program Files\Windhawk\Engine\1.7.3\64\windhawk.lib'
$dll = "$s\$($id)_$($ver)_100007.dll"
if (Test-Path $dll) { Remove-Item $dll }
$cargs = @('-std=c++23','-O2','-shared','-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-D_WIN32_IE=0x0A00','-DNTDDI_VERSION=0x0A000008','-D__USE_MINGW_ANSI_STDIO=0','-DWH_MOD',
  ('"-DWH_MOD_ID=L\"' + $id + '\""'), ('"-DWH_MOD_VERSION=L\"' + $ver + '\""'), ('"' + $lib + '"'),
  '-x','c++','-','-include','windhawk_api.h','-target','x86_64-w64-mingw32','-Wl,--export-all-symbols','-o',('"' + $dll + '"'),
  '-lole32','-loleaut32','-lruntimeobject','-lshcore','-lwindowsapp','-luser32') -join ' '
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = "$comp\bin\clang++.exe"; $psi.Arguments = $cargs; $psi.WorkingDirectory = $comp
$psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
$p = [Diagnostics.Process]::Start($psi)
$errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
$bytes = $utf8.GetBytes($t); $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length); $p.StandardInput.Close()
$p.WaitForExit()
"clang exit: $($p.ExitCode)"
$e = $errTask.Result; if ($e) { $e.Substring(0, [Math]::Min(2500, $e.Length)) }
if (Test-Path $dll) { "built: $([math]::Round((Get-Item $dll).Length/1KB)) KB" } else { "no dll produced" }
