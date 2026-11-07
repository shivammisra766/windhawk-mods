# Installs 4.0 with the popup's styler rules, then restarts what must reload. Run elevated, then
# restart the bar unelevated. Undo: reg import ..\windhawk-hklm-before-4.0.reg, restart Explorer.
$s = $PSScriptRoot
$out = "$s\install-4.0-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-4.0.reg" /y | Out-Null
  & "$s\install-bar.ps1"
  $mod = 'SOFTWARE\Windhawk\Engine\Mods\windows-11-taskbar-styler'
  $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mod\Settings", $true)
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
    for ($j = $styles.Count; $j -lt 12; $j++) { $key.DeleteValue("controlStyles[$found].styles[$j]", $false) }
  }
  # Old glass rules for the popup. The slots stay, since the styler stops at the first missing one.
  foreach ($i in 17, 18, 41) {
    $key.SetValue("controlStyles[$i].styles[0]", 'Opacity=1')
    for ($j = 1; $j -lt 12; $j++) { $key.DeleteValue("controlStyles[$i].styles[$j]", $false) }
  }
  SetRule 'Grid#ConfirmatorMainGrid' @('Background:=<SolidColorBrush Color="#FF101010"/>', 'CornerRadius=0,0,10,10', 'BorderThickness=0', 'Shadow:=', 'Margin=0', 'Padding=0', 'MinHeight=0')
  SetRule 'Grid#VolumeConfirmator' @('Background:=<SolidColorBrush Color="#00000000"/>', 'Margin=0', 'Padding=0')
  SetRule 'Grid#BrightnessConfirmator' @('Background:=<SolidColorBrush Color="#00000000"/>', 'Margin=0', 'Padding=0')
  $key.Close()
  Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  'ShellExperienceHost', 'ShellHost' | ForEach-Object { Stop-Process -Name $_ -Force -ErrorAction SilentlyContinue }
  Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
