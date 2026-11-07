# Quick Settings restyle, matching the dark Start menu, plus the current bar. Run elevated, then
# restart the bar process unelevated with: windhawk.exe -tool-mod "local@top-bar"
# To undo the look: reg import windhawk-hklm-before-qs-restyle.reg, then restart the hosts.
$s = $PSScriptRoot
$out = "$s\install-qs-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-qs-restyle.reg" /y | Out-Null
  $base = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods'
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

  # windhawk.exe is 32-bit, so the bar needs both architectures.
  $id = 'local@top-bar'; $ver = '2.0'; $dll = "$($id)_$($ver)_300020.dll"
  foreach ($a in '32', '64') { Copy-Item "$s\build\$a\$dll" "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll" -Force }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  Set-ItemProperty "$base\$id" -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty "$base\$id" -Name Version -Value $ver -Type String
  Set-ItemProperty "$base\$id" -Name SettingsChangeTime -Value $now -Type DWord

  $mod = 'SOFTWARE\Windhawk\Engine\Mods\windows-11-notification-center-styler'
  $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mod\Settings", $true)

  # Adds or replaces one rule, found by its target, and clears leftover styles from before.
  function SetRule($target, $styles) {
    $found = $null; $next = 0
    foreach ($n in $key.GetValueNames()) {
      if ($n -match '^controlStyles\[(\d+)\]\.target$') {
        if ([int]$Matches[1] -ge $next) { $next = [int]$Matches[1] + 1 }
        if ($key.GetValue($n) -eq $target) { $found = [int]$Matches[1] }
      }
    }
    if ($null -eq $found) { $found = $next }
    $key.SetValue("controlStyles[$found].target", $target)
    for ($j = 0; $j -lt $styles.Count; $j++) { $key.SetValue("controlStyles[$found].styles[$j]", $styles[$j]) }
    for ($j = $styles.Count; $j -lt 20; $j++) { $key.DeleteValue("controlStyles[$found].styles[$j]", $false) }
  }

  # Near-solid panels, so nothing behind shows through the text. Same shape as Start.
  foreach ($i in 0, 1, 4, 5) { $key.SetValue("controlStyles[$i].styles[0]", 'Background:=<WindhawkBlur BlurAmount="0" TintColor="#F4101010"/>') }

  # These gave every tile the same corners, which split tiles need to differ on.
  # The slots stay, because the styler stops reading at the first missing one.
  foreach ($i in 20, 21, 23, 24, 25) { $key.SetValue("controlStyles[$i].styles[0]", 'Opacity=1') }

  # Tiles: Start's dark tile when off, soft white with dark icons when on, no borders.
  $tile = @(
    'Background@Normal:=#1F1F1F', 'Background@PointerOver:=#2A2A2A', 'Background@Pressed:=#333333', 'Background@Disabled:=#181818',
    'Background@Checked:=#E4E4E4', 'Background@CheckedPointerOver:=#F2F2F2', 'Background@CheckedPressed:=#CFCFCF', 'Background@CheckedDisabled:=#8A8A8A',
    'Foreground@Normal:=#E4E4E4', 'Foreground@PointerOver:=#FFFFFF', 'Foreground@Pressed:=#E4E4E4',
    'Foreground@Checked:=#111111', 'Foreground@CheckedPointerOver:=#111111', 'Foreground@CheckedPressed:=#111111', 'Foreground@CheckedDisabled:=#111111',
    'BorderBrush@Normal:=#00000000', 'BorderBrush@PointerOver:=#00000000', 'BorderBrush@Checked:=#00000000', 'BorderBrush@CheckedPointerOver:=#00000000')
  SetRule 'ControlCenter.PaginatedToggleButton#ToggleButton > ContentPresenter#ContentPresenter@CommonStates' $tile
  SetRule 'ControlCenter.PaginatedToggleButton#SplitL2Button > ContentPresenter#ContentPresenter@CommonStates' $tile

  # Corners: a plain tile is fully rounded; a split tile reads as one shape.
  SetRule 'ContentPresenter > Grid > ControlCenter.PaginatedToggleButton#ToggleButton > ContentPresenter#ContentPresenter' @('CornerRadius=12', 'BorderThickness=0')
  SetRule 'ContentPresenter > Grid > Grid > ControlCenter.PaginatedToggleButton#ToggleButton > ContentPresenter#ContentPresenter' @('CornerRadius=12,0,0,12', 'BorderThickness=0')
  SetRule 'ControlCenter.PaginatedToggleButton#SplitL2Button > ContentPresenter#ContentPresenter' @('CornerRadius=0,12,12,0', 'BorderThickness=0')

  # Sliders: thicker and monochrome.
  SetRule 'Grid#HorizontalTemplate > Windows.UI.Xaml.Shapes.Rectangle#HorizontalTrackRect' @('Fill=#3A3A3A', 'Height=6', 'RadiusX=3', 'RadiusY=3')
  SetRule 'Grid#HorizontalTemplate > Windows.UI.Xaml.Shapes.Rectangle#HorizontalDecreaseRect' @('Fill=#E4E4E4', 'Height=6', 'RadiusX=3', 'RadiusY=3')
  SetRule 'Border > Windows.UI.Xaml.Shapes.Ellipse#SliderInnerThumb' @('Fill=#E4E4E4')

  # No pager dots or white focus box.
  SetRule 'Microsoft.UI.Xaml.Controls.PipsPager#QuickActionsPager' @('Visibility=Collapsed')
  SetRule 'ControlCenter.PaginatedToggleButton' @('UseSystemFocusVisuals=False')
  SetRule 'GridViewItem' @('UseSystemFocusVisuals=False')
  $key.Close()
  Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value $now -Type DWord

  # Windows restarts these by itself, and they load the changes.
  'ShellExperienceHost', 'ShellHost' | ForEach-Object { Stop-Process -Name $_ -Force -ErrorAction SilentlyContinue }
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
