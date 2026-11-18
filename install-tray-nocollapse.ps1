# Replaces the tray's Visibility=Collapsed (suspected cause of the login crash loop, see crashdiag)
# with a hide that keeps the tray in the layout. Run elevated.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-tray-nocollapse.reg, then restart Explorer.
$out = "$PSScriptRoot\install-tray-nocollapse-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-tray-nocollapse.reg" /y | Out-Null
  $mod = 'SOFTWARE\Windhawk\Engine\Mods\windows-11-taskbar-styler'
  $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mod\Settings", $true)
  if ($key.GetValue('controlStyles[42].target') -ne 'SystemTray.SystemTrayFrame') { throw 'slot 42 is not SystemTrayFrame' }
  if ($key.GetValue('controlStyles[2].styles[4]') -ne 'Visibility=Collapsed') { throw 'slot 2 style 4 is not the collapse' }
  $frame = @('Width=0', 'MaxWidth=0', 'Opacity=0', 'IsHitTestVisible=False')
  for ($j = 0; $j -lt $frame.Count; $j++) { $key.SetValue("controlStyles[42].styles[$j]", $frame[$j]) }
  $key.SetValue('controlStyles[2].styles[4]', 'Opacity=0')
  $key.SetValue('controlStyles[2].styles[5]', 'IsHitTestVisible=False')
  $key.Close()
  Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
